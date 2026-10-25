#include "waywal/video_engine.h"

#include "linux-dmabuf-v1-client-protocol.h"
#include "wayland_core.h"
#include "waywal/log.h"

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>

static void video_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    (void)wl_buffer;
    bool *in_use = (bool *)data;
    if (in_use) {
        *in_use = false;
    }
}

static const struct wl_buffer_listener g_video_buffer_listener = {
    .release = video_buffer_release,
};

static bool is_output_active_for_video(const video_engine_t *ve, const output_node_t *out)
{
    if (!ve || !out)
        return false;
    if (ve->target_output)
        return (out == ve->target_output);
    if (ve->has_target_filter)
        return out->video_targeted;
    return true;
}

static output_node_t *select_pll_reference_output(const video_engine_t *ve)
{
    if (!ve || !ve->daemon_state)
        return NULL;
    output_node_t *best = NULL;
    for (output_node_t *out = ve->daemon_state->outputs; out != NULL; out = out->next) {
        if (!is_output_active_for_video(ve, out) || !out->surface)
            continue;
        if (!best || out->refresh_mhz > best->refresh_mhz) {
            best = out;
        }
    }
    return best;
}

static struct wl_buffer *ensure_shared_video_buffer(video_engine_t *ve, VASurfaceID export_surf,
                                                    uint32_t buf_idx)
{
    if (!ve || buf_idx >= WAYWAL_VA_SURFACE_POOL_SIZE || export_surf == VA_INVALID_SURFACE)
        return NULL;

    if (ve->video_wl_buffers[buf_idx]) {
        return ve->video_wl_buffers[buf_idx];
    }

    struct zwp_linux_dmabuf_v1 *dmabuf_proto = ve->daemon_state->dmabuf_ctx.dmabuf_proto;
    if (!dmabuf_proto)
        return NULL;

    vaapi_prime_frame_t prime_frame;
    if (!vaapi_decoder_export_prime(&ve->decoder, export_surf, &prime_frame)) {
        WAYWAL_LOG_ERR("vaapi_decoder_export_prime failed for surface %u", export_surf);
        return NULL;
    }

    struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(dmabuf_proto);
    if (!params) {
        vaapi_prime_frame_close(&prime_frame);
        return NULL;
    }

    for (uint32_t p = 0; p < prime_frame.num_planes; ++p) {
        zwp_linux_buffer_params_v1_add(params, prime_frame.fds[p], p, prime_frame.offsets[p],
                                       prime_frame.strides[p],
                                       (uint32_t)(prime_frame.modifiers[p] >> 32),
                                       (uint32_t)(prime_frame.modifiers[p] & 0xFFFFFFFF));
    }

    struct wl_buffer *wl_buf = zwp_linux_buffer_params_v1_create_immed(
        params, prime_frame.width, prime_frame.height, prime_frame.drm_format, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    vaapi_prime_frame_close(&prime_frame);

    if (wl_buf) {
        WAYWAL_LOG_INFO("Created shared video wl_buffer %p for surface %u (idx %u, %ux%u, fmt 0x%08x)",
                        (void *)wl_buf, export_surf, buf_idx, prime_frame.width,
                        prime_frame.height, prime_frame.drm_format);
        wl_buffer_add_listener(wl_buf, &g_video_buffer_listener,
                               &ve->video_buffer_in_use[buf_idx]);
        ve->video_wl_buffers[buf_idx] = wl_buf;
        ve->video_buffer_in_use[buf_idx] = false;
    } else {
        WAYWAL_LOG_ERR("create_immed returned NULL for shared video surface %u", export_surf);
    }
    return wl_buf;
}

bool video_engine_init(video_engine_t *ve, daemon_state_t *state)
{
    if (!ve || !state)
        return false;
    memset(ve, 0, sizeof(*ve));
    ve->daemon_state = state;
    ve->state = WAYWAL_VIDEO_STATE_STOPPED;
    ve->reorder_count = 0;
    ve->has_decoded_any = false;
    ve->playback_speed = 1.0f;
    ve->loop_count = 0; /* infinite loop */
    ve->seamless_looping = true;

    ve->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (ve->timer_fd < 0) {
        WAYWAL_LOG_ERR("Failed to create video presentation timerfd: %s", strerror(errno));
        return false;
    }

    /* Register timerfd in daemon epoll event loop */
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = ve->timer_fd};
    if (epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD, ve->timer_fd, &ev) < 0) {
        WAYWAL_LOG_ERR("epoll_ctl ADD failed for video timerfd: %s", strerror(errno));
        close(ve->timer_fd);
        ve->timer_fd = -1;
        return false;
    }

    WAYWAL_LOG_INFO("Hardware video engine initialized (timer_fd: %d)", ve->timer_fd);
    return true;
}

void video_engine_destroy(video_engine_t *ve)
{
    if (!ve)
        return;
    video_engine_stop(ve);

    if (ve->timer_fd >= 0) {
        if (ve->daemon_state && ve->daemon_state->epoll_fd >= 0) {
            epoll_ctl(ve->daemon_state->epoll_fd, EPOLL_CTL_DEL, ve->timer_fd, NULL);
        }
        close(ve->timer_fd);
        ve->timer_fd = -1;
    }
}

static void reclaim_output_shm_for_video(output_node_t *out)
{
    if (!out)
        return;
    out->transition_active = false;
    if (out->shm_old_data) {
        free(out->shm_old_data);
        out->shm_old_data = NULL;
    }
    if (out->shm_new_data) {
        free(out->shm_new_data);
        out->shm_new_data = NULL;
    }
    out->shm_buffer_cap = 0;

    if (out->shm_data && out->shm_size > 0) {
        madvise(out->shm_data, out->shm_size, MADV_REMOVE);
        madvise(out->shm_data, out->shm_size, MADV_DONTNEED);
    }
}

static void configure_output_video_viewport(output_node_t *out, uint32_t vid_w, uint32_t vid_h)
{
    if (!out || !out->viewport || out->width <= 0 || out->height <= 0 || vid_w == 0 || vid_h == 0)
        return;

    if (out->vp_configured && out->vp_dst_w == out->width && out->vp_dst_h == out->height &&
        out->vp_src_w == vid_w && out->vp_src_h == vid_h &&
        out->vp_scaling_mode == out->scaling_mode) {
        return;
    }

    wp_viewport_set_destination(out->viewport, out->width, out->height);

    if (out->scaling_mode == WAYWAL_SCALING_FILL) {
        double r_src = (double)vid_w / (double)vid_h;
        double r_dst = (double)out->width / (double)out->height;
        double crop_w = (double)vid_w;
        double crop_h = (double)vid_h;
        double off_x = 0.0;
        double off_y = 0.0;

        if (r_src > r_dst + 1e-4) {
            crop_h = (double)vid_h;
            crop_w = crop_h * r_dst;
            off_x = ((double)vid_w - crop_w) * 0.5;
        } else if (r_dst > r_src + 1e-4) {
            crop_w = (double)vid_w;
            crop_h = crop_w / r_dst;
            off_y = ((double)vid_h - crop_h) * 0.5;
        }

        if (fabs(crop_w - (double)vid_w) < 0.5 && fabs(crop_h - (double)vid_h) < 0.5) {
            wp_viewport_set_source(out->viewport, wl_fixed_from_int(0), wl_fixed_from_int(0),
                                   wl_fixed_from_int((int)vid_w), wl_fixed_from_int((int)vid_h));
        } else {
            wp_viewport_set_source(out->viewport, wl_fixed_from_double(off_x),
                                   wl_fixed_from_double(off_y), wl_fixed_from_double(crop_w),
                                   wl_fixed_from_double(crop_h));
        }
    } else {
        wp_viewport_set_source(out->viewport, wl_fixed_from_int(0), wl_fixed_from_int(0),
                               wl_fixed_from_int((int)vid_w), wl_fixed_from_int((int)vid_h));
    }

    out->vp_dst_w = out->width;
    out->vp_dst_h = out->height;
    out->vp_src_w = vid_w;
    out->vp_src_h = vid_h;
    out->vp_scaling_mode = out->scaling_mode;
    out->vp_configured = true;
}

static void video_engine_release_wl_buffers(video_engine_t *ve)
{
    if (!ve)
        return;
    bool destroyed_any = false;
    for (uint32_t i = 0; i < WAYWAL_VA_SURFACE_POOL_SIZE; ++i) {
        if (ve->video_wl_buffers[i]) {
            wl_buffer_destroy(ve->video_wl_buffers[i]);
            ve->video_wl_buffers[i] = NULL;
            destroyed_any = true;
        }
        ve->video_buffer_in_use[i] = false;
    }
    if (destroyed_any && ve->daemon_state && ve->daemon_state->display) {
        wl_display_flush(ve->daemon_state->display);
    }
}

static void video_engine_prepare_reload(video_engine_t *ve)
{
    if (!ve)
        return;
    struct itimerspec its = {0};
    if (ve->timer_fd >= 0) {
        timerfd_settime(ve->timer_fd, 0, &its, NULL);
        uint64_t dummy;
        while (read(ve->timer_fd, &dummy, sizeof(dummy)) > 0);
    }
    ve->state = WAYWAL_VIDEO_STATE_STOPPED;
    ve->reorder_count = 0;
    ve->has_decoded_any = false;
    ve->playback_epoch_ns = 0;
    if (ve->demuxer) {
        ve->demuxer->destroy(ve->demuxer);
        ve->demuxer = NULL;
    }
}

static bool can_reuse_vaapi_decoder(vaapi_decoder_t *dec, const demuxer_t *demux,
                                    bool allow_direct_nv12)
{
    if (!dec || !dec->initialized || !demux)
        return false;
    if (dec->width != demux->width || dec->height != demux->height)
        return false;

    bool profile_matches = false;
    switch (demux->codec) {
    case WAYWAL_CODEC_H264:
        profile_matches = (dec->va_profile == VAProfileH264Main ||
                           dec->va_profile == VAProfileH264High ||
                           dec->va_profile == VAProfileH264ConstrainedBaseline);
        break;
    case WAYWAL_CODEC_HEVC:
        profile_matches = (dec->va_profile == VAProfileHEVCMain ||
                           dec->va_profile == VAProfileHEVCMain10);
        break;
    case WAYWAL_CODEC_VP9:
        profile_matches = (dec->va_profile == VAProfileVP9Profile0 ||
                           dec->va_profile == VAProfileVP9Profile2);
        break;
    case WAYWAL_CODEC_AV1:
        profile_matches = (dec->va_profile == VAProfileAV1Profile0);
        break;
    default:
        break;
    }
    if (!profile_matches)
        return false;

    memset(&dec->cached_sps, 0, sizeof(dec->cached_sps));
    memset(&dec->cached_pps, 0, sizeof(dec->cached_pps));
    memset(&dec->cached_hevc_sps, 0, sizeof(dec->cached_hevc_sps));
    memset(&dec->cached_hevc_pps, 0, sizeof(dec->cached_hevc_pps));
    memset(&dec->cached_av1_seq, 0, sizeof(dec->cached_av1_seq));
    for (int i = 0; i < 8; ++i) {
        dec->vp9_ref_frames[i] = VA_INVALID_SURFACE;
        dec->av1_ref_frames[i] = VA_INVALID_SURFACE;
    }
    dec->is_avcc = false;
    dec->nal_length_size = 4;
    memset(&dec->cached_iq, 16, sizeof(dec->cached_iq));
    dec->iq_valid = true;
    if (demux->extradata && demux->extradata_size > 0) {
        vaapi_decoder_set_extradata(dec, demux->extradata, demux->extradata_size);
    }
    bool is_10bit =
        (dec->va_profile == VAProfileHEVCMain10 || dec->va_profile == VAProfileVP9Profile2 ||
         (dec->va_profile == VAProfileAV1Profile0 && dec->cached_av1_seq.valid &&
          dec->cached_av1_seq.high_bitdepth) ||
         (dec->cached_hevc_sps.valid && (dec->cached_hevc_sps.bit_depth_luma_minus8 == 2 ||
                                         dec->cached_hevc_sps.bit_depth_chroma_minus8 == 2)));
    uint32_t expected_fourcc = is_10bit ? VA_FOURCC_P010 : VA_FOURCC_NV12;
    if (dec->fourcc != expected_fourcc)
        return false;
    bool want_direct = is_10bit ? (allow_direct_nv12 && dec->allow_direct_p010) : allow_direct_nv12;
    if (dec->has_vpp != (!want_direct))
        return false;
    uint32_t max_refs = 4;
    if (dec->cached_sps.valid) {
        max_refs = dec->cached_sps.num_ref_frames;
        if (max_refs < 1)
            max_refs = 1;
        if (max_refs > 16)
            max_refs = 16;
    } else if (dec->cached_hevc_sps.valid) {
        max_refs = (uint32_t)dec->cached_hevc_sps.sps_max_dec_pic_buffering_minus1 + 1;
        if (max_refs < 4)
            max_refs = 4;
        if (max_refs > 15)
            max_refs = 15;
    } else if (demux->codec == WAYWAL_CODEC_VP9 || demux->codec == WAYWAL_CODEC_AV1) {
        max_refs = 8;
    }
    uint32_t needed_surfaces = max_refs + 3;
    if (needed_surfaces < 4)
        needed_surfaces = 4;
    if (needed_surfaces > WAYWAL_VA_SURFACE_POOL_SIZE)
        needed_surfaces = WAYWAL_VA_SURFACE_POOL_SIZE;

    return (dec->num_surfaces == needed_surfaces);
}

static bool video_engine_start_playback(video_engine_t *ve, output_node_t *out)
{
    if (!ve || !ve->demuxer)
        return false;

    int drm_fd = ve->daemon_state ? ve->daemon_state->dmabuf_ctx.drm_fd : -1;
    /* Enable direct NV12/P010 KMS overlay scanout when the compositor advertises NV12 and
     * the VA-API driver exports composed multi-planar surfaces (Intel iHD/i965, AMD radeonsi).
     * Keep the 3-surface BGRX VPP ring active on NVIDIA where libva-nvidia-driver exports
     * separate R8 and GR88 layers. */
    bool has_nv12 = ve->daemon_state && ve->daemon_state->dmabuf_ctx.has_nv12;
    bool has_p010 = ve->daemon_state && ve->daemon_state->dmabuf_ctx.has_p010;
    bool allow_direct_nv12 =
        has_nv12 && (!ve->decoder.initialized || !ve->decoder.is_nvidia_vaapi);
    ve->decoder.allow_direct_p010 =
        has_p010 && (!ve->decoder.initialized || !ve->decoder.is_nvidia_vaapi);

    if (can_reuse_vaapi_decoder(&ve->decoder, ve->demuxer, allow_direct_nv12)) {
        if (ve->decoder.va_display) {
            for (uint32_t i = 0; i < ve->decoder.num_surfaces; ++i) {
                if (ve->decoder.surfaces[i] != VA_INVALID_SURFACE) {
                    vaSyncSurface(ve->decoder.va_display, ve->decoder.surfaces[i]);
                }
            }
            if (ve->decoder.has_vpp) {
                for (uint32_t i = 0; i < ve->decoder.num_vpp_surfaces; ++i) {
                    if (ve->decoder.vpp_surfaces[i] != VA_INVALID_SURFACE) {
                        vaSyncSurface(ve->decoder.va_display, ve->decoder.vpp_surfaces[i]);
                    }
                }
            }
        }
        vaapi_decoder_reset(&ve->decoder);
        ve->decoder.current_surface_idx = 0;
        WAYWAL_LOG_INFO("Reusing active VA-API decoder context (%ux%u, %u surfaces)",
                        ve->decoder.width, ve->decoder.height, ve->decoder.num_surfaces);
    } else {
        if (ve->decoder.initialized) {
            if (ve->daemon_state) {
                daemon_clear_all_outputs(ve->daemon_state, ve->daemon_state->current_color);
            }
            video_engine_release_wl_buffers(ve);
            vaapi_decoder_destroy(&ve->decoder);
        }
        ve->decoder.allow_direct_p010 = has_p010;
        if (!vaapi_decoder_init_with_params(&ve->decoder, drm_fd, ve->demuxer->codec,
                                            ve->demuxer->width, ve->demuxer->height,
                                            ve->demuxer->extradata, ve->demuxer->extradata_size,
                                            allow_direct_nv12)) {
            WAYWAL_LOG_ERR("Failed to initialize hardware VA-API decoder for video");
            ve->demuxer->destroy(ve->demuxer);
            ve->demuxer = NULL;
            ve->state = WAYWAL_VIDEO_STATE_STOPPED;
            ve->reorder_count = 0;
            ve->has_decoded_any = false;
            return false;
        }
    }

    ve->target_output = out;
    ve->state = WAYWAL_VIDEO_STATE_PLAYING;
    ve->current_loop = 0;
    ve->presented_loop_id = 0;
    ve->last_pts_us = 0;
    ve->playback_epoch_ns = 0;
    ve->reorder_count = 0;
    ve->current_gop_id = 0;
    ve->next_present_gop = 0;
    ve->next_present_poc = 0;
    ve->has_decoded_any = false;
    ve->last_vpp_idx = 0;
    memset(ve->combined_busy_mask, 0, sizeof(ve->combined_busy_mask));

    if (ve->daemon_state) {
        for (output_node_t *node = ve->daemon_state->outputs; node != NULL; node = node->next) {
            if (is_output_active_for_video(ve, node)) {
                node->vp_configured = false;
                reclaim_output_shm_for_video(node);
            }
        }
        malloc_trim(0);
    }

    WAYWAL_LOG_INFO("Hardware video playback started: %ux%u @ %u fps (duration: %.2fs)",
                    ve->demuxer->width, ve->demuxer->height, ve->demuxer->fps_num,
                    (double)ve->demuxer->duration_us / 1e6);

    /* Arm timer to trigger first frame decode immediately (1 ms) */
    struct itimerspec its = {
        .it_interval = {0, 0}, .it_value = {.tv_sec = 0, .tv_nsec = 1000000L} /* 1ms */
    };
    timerfd_settime(ve->timer_fd, 0, &its, NULL);
    return true;
}

bool video_engine_load_file(video_engine_t *ve, const char *filepath, output_node_t *out)
{
    if (!ve || !filepath)
        return false;

    bool saved_filter = ve->has_target_filter;
    video_engine_prepare_reload(ve);
    ve->has_target_filter = saved_filter;

    ve->demuxer = demuxer_open_file(filepath);
    if (!ve->demuxer) {
        WAYWAL_LOG_ERR("Failed to demux video file '%s'", filepath);
        video_engine_release_wl_buffers(ve);
        if (ve->decoder.initialized) {
            vaapi_decoder_destroy(&ve->decoder);
        }
        return false;
    }

    return video_engine_start_playback(ve, out);
}

bool video_engine_load_mem(video_engine_t *ve, int memfd, size_t size, output_node_t *out)
{
    if (!ve || memfd < 0 || size == 0)
        return false;

    bool saved_filter = ve->has_target_filter;
    video_engine_prepare_reload(ve);
    ve->has_target_filter = saved_filter;

    void *mapped = mmap(NULL, size, PROT_READ, MAP_SHARED, memfd, 0);
    if (mapped == MAP_FAILED) {
        WAYWAL_LOG_ERR("mmap failed for video memfd: %s", strerror(errno));
        close(memfd);
        video_engine_release_wl_buffers(ve);
        if (ve->decoder.initialized) {
            vaapi_decoder_destroy(&ve->decoder);
        }
        return false;
    }

    ve->demuxer = demuxer_open_mem((const uint8_t *)mapped, size);
    if (!ve->demuxer) {
        WAYWAL_LOG_ERR("Failed to demux video from memory buffer");
        munmap(mapped, size);
        close(memfd);
        video_engine_release_wl_buffers(ve);
        if (ve->decoder.initialized) {
            vaapi_decoder_destroy(&ve->decoder);
        }
        return false;
    }

    typedef struct {
        int file_fd;
        bool owns_mmap;
    } demuxer_common_state_t;
    demuxer_common_state_t *state = (demuxer_common_state_t *)ve->demuxer->internal_state;
    if (state) {
        state->owns_mmap = true;
        state->file_fd = memfd;
    }

    return video_engine_start_playback(ve, out);
}

static void update_combined_busy_mask(video_engine_t *ve)
{
    uint32_t pool_size =
        ve->decoder.num_surfaces > 0 ? ve->decoder.num_surfaces : WAYWAL_VA_SURFACE_POOL_SIZE;
    memset(ve->combined_busy_mask, 0, sizeof(ve->combined_busy_mask));
    if (!ve->decoder.has_vpp) {
        for (uint32_t i = 0; i < pool_size; ++i) {
            if (ve->video_buffer_in_use[i]) {
                ve->combined_busy_mask[i] = true;
            }
        }
    }
    for (uint32_t r = 0; r < ve->reorder_count; ++r) {
        uint32_t idx = ve->reorder_buf[r].surf_idx;
        if (idx < pool_size) {
            ve->combined_busy_mask[idx] = true;
        }
    }
    ve->decoder.surface_busy_mask = ve->combined_busy_mask;
}

static bool reorder_has_ready_frame(video_engine_t *ve)
{
    if (ve->reorder_count == 0)
        return false;
    if (ve->reorder_count >= WAYWAL_VIDEO_REORDER_CAP - 1)
        return true;

    if (ve->current_gop_id > ve->next_present_gop) {
        bool has_old_gop = false;
        for (uint32_t i = 0; i < ve->reorder_count; ++i) {
            if (ve->reorder_buf[i].gop_id == ve->next_present_gop) {
                has_old_gop = true;
                break;
            }
        }
        if (has_old_gop) {
            return true;
        }
        ve->next_present_gop = ve->current_gop_id;
        ve->next_present_poc = 0;
    }

    for (uint32_t i = 0; i < ve->reorder_count; ++i) {
        if (ve->reorder_buf[i].gop_id == ve->next_present_gop &&
            ve->reorder_buf[i].poc <= ve->next_present_poc) {
            return true;
        }
    }
    return false;
}

void video_engine_dispatch_frame(video_engine_t *ve)
{
    if (!ve || ve->timer_fd < 0) {
        return;
    }

    uint64_t expirations = 0;
    ssize_t s = read(ve->timer_fd, &expirations, sizeof(expirations));
    if (s <= 0 || expirations == 0)
        return;

    if (ve->state != WAYWAL_VIDEO_STATE_PLAYING || !ve->demuxer) {
        return;
    }

    uint32_t pool_size =
        ve->decoder.num_surfaces > 0 ? ve->decoder.num_surfaces : WAYWAL_VA_SURFACE_POOL_SIZE;

    while (ve->reorder_count < WAYWAL_VIDEO_REORDER_CAP) {
        if (reorder_has_ready_frame(ve)) {
            break;
        }

        demux_packet_t pkt;
        if (!ve->demuxer->read_packet(ve->demuxer, &pkt)) {
            if (ve->loop_count > 0 && ve->current_loop + 1 >= ve->loop_count) {
                if (ve->reorder_count > 0) {
                    break;
                }
                ve->current_loop++;
                WAYWAL_LOG_INFO("Video loop count (%lu) reached, stopping playback", ve->loop_count);
                video_engine_stop(ve);
                return;
            }
            ve->current_loop++;

            ve->demuxer->seek_us(ve->demuxer, 0);
            vaapi_decoder_reset(&ve->decoder);
            if (!ve->demuxer->read_packet(ve->demuxer, &pkt)) {
                if (ve->reorder_count > 0) {
                    break;
                }
                WAYWAL_LOG_WARN("Failed to seek to start of stream on video loop");
                video_engine_stop(ve);
                return;
            }
        }

        update_combined_busy_mask(ve);

        VASurfaceID decoded_surface = VA_INVALID_SURFACE;
        if (!vaapi_decoder_decode_packet(&ve->decoder, &pkt, &decoded_surface)) {
            break;
        }

        if (ve->decoder.last_decoded_is_idr && ve->has_decoded_any) {
            ve->current_gop_id++;
        }
        ve->has_decoded_any = true;

        uint32_t surf_idx = 0;
        for (uint32_t i = 0; i < pool_size; ++i) {
            if (ve->decoder.surfaces[i] == decoded_surface) {
                surf_idx = i;
                break;
            }
        }

        ve->reorder_buf[ve->reorder_count++] = (video_reorder_entry_t){
            .surface = decoded_surface,
            .surf_idx = surf_idx,
            .pts_us = pkt.pts_us,
            .poc = ve->decoder.last_decoded_poc,
            .gop_id = ve->current_gop_id,
            .loop_id = ve->current_loop,
        };
    }

    int64_t frame_interval_us = 33333LL;
    if (ve->demuxer->fps_num > 0) {
        frame_interval_us =
            (1000000LL * (int64_t)ve->demuxer->fps_den) / (int64_t)ve->demuxer->fps_num;
    }
    double speed = (ve->playback_speed > 0.01f) ? (double)ve->playback_speed : 1.0;
    frame_interval_us = (int64_t)((double)frame_interval_us / speed);
    if (frame_interval_us < 1000LL)
        frame_interval_us = 1000LL;
    int64_t frame_interval_ns = frame_interval_us * 1000LL;

    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    int64_t now_ns = (int64_t)now_ts.tv_sec * 1000000000LL + now_ts.tv_nsec;
    int64_t target_pres_ns = now_ns + frame_interval_ns;

    output_node_t *pll_ref_out = select_pll_reference_output(ve);

    if (ve->reorder_count > 0) {
        uint32_t best_idx = 0;
        for (uint32_t i = 1; i < ve->reorder_count; ++i) {
            const video_reorder_entry_t *a = &ve->reorder_buf[i];
            const video_reorder_entry_t *b = &ve->reorder_buf[best_idx];
            if (a->gop_id < b->gop_id ||
                (a->gop_id == b->gop_id && a->poc < b->poc) ||
                (a->gop_id == b->gop_id && a->poc == b->poc && a->pts_us < b->pts_us)) {
                best_idx = i;
            }
        }

        video_reorder_entry_t pres = ve->reorder_buf[best_idx];
        if (best_idx + 1 < ve->reorder_count) {
            memmove(&ve->reorder_buf[best_idx], &ve->reorder_buf[best_idx + 1],
                    (ve->reorder_count - best_idx - 1) * sizeof(video_reorder_entry_t));
        }
        ve->reorder_count--;

        int64_t scaled_pts_ns = (int64_t)((double)(pres.pts_us * 1000LL) / speed);
        if (ve->playback_epoch_ns == 0 || pres.loop_id != ve->presented_loop_id ||
            pres.pts_us < ve->last_pts_us) {
            ve->playback_epoch_ns = now_ns - scaled_pts_ns;
            ve->presented_loop_id = pres.loop_id;
        }
        target_pres_ns = ve->playback_epoch_ns + scaled_pts_ns + frame_interval_ns;
        if (target_pres_ns <= now_ns - 2 * frame_interval_ns) {
            ve->playback_epoch_ns = now_ns - scaled_pts_ns;
            target_pres_ns = now_ns + frame_interval_ns;
        }

        ve->last_pts_us = pres.pts_us;
        ve->next_present_gop = pres.gop_id;
        ve->next_present_poc = pres.poc + 2;

        VASurfaceID export_surf = pres.surface;
        uint32_t buf_idx = pres.surf_idx;
        if (ve->decoder.has_vpp) {
            uint32_t vpp_pool = ve->decoder.num_vpp_surfaces > 0 ? ve->decoder.num_vpp_surfaces
                                                                 : WAYWAL_VA_VPP_POOL_SIZE;
            /* Always select the lowest-index free VPP surface so nvidia_drv_video's lazy
             * direct_realiseSurface allocates VRAM only for the 2 ping-pong buffers actually
             * needed by the compositor. */
            uint32_t vpp_idx = (ve->last_vpp_idx + 1) % vpp_pool;
            for (uint32_t k = 0; k < vpp_pool; ++k) {
                if (!ve->video_buffer_in_use[k]) {
                    vpp_idx = k;
                    break;
                }
            }
            ve->last_vpp_idx = vpp_idx;
            export_surf = ve->decoder.vpp_surfaces[vpp_idx];
            buf_idx = vpp_idx;
            vaapi_decoder_vpp_blit(&ve->decoder, pres.surface, export_surf);
        }

        struct wl_buffer *wl_buf = ensure_shared_video_buffer(ve, export_surf, buf_idx);
        if (wl_buf && ve->daemon_state) {
            bool attached_any = false;
            for (output_node_t *out = ve->daemon_state->outputs; out != NULL; out = out->next) {
                if (!is_output_active_for_video(ve, out) || !out->surface)
                    continue;
                configure_output_video_viewport(out, ve->decoder.width, ve->decoder.height);
                if (out == pll_ref_out && out->pres_sync.wp_pres) {
                    presentation_sync_request(&out->pres_sync);
                }
                wl_surface_attach(out->surface, wl_buf, 0, 0);
                wl_surface_damage_buffer(out->surface, 0, 0, INT32_MAX, INT32_MAX);
                wl_surface_commit(out->surface);
                attached_any = true;
            }
            if (attached_any) {
                ve->video_buffer_in_use[buf_idx] = true;
                wl_display_flush(ve->daemon_state->display);
            }
        }
    }

    int64_t next_interval_ns = target_pres_ns - now_ns;

    if (pll_ref_out && pll_ref_out->pres_sync.pll.phase_est_ns > 0 &&
        pll_ref_out->pres_sync.pll.period_est_ns > 0) {
        int64_t next_vblank_ns = presentation_pll_predict_next_vblank(&pll_ref_out->pres_sync.pll);
        int64_t period_ns = pll_ref_out->pres_sync.pll.period_est_ns;

        if (target_pres_ns >= next_vblank_ns && period_ns > 0) {
            int64_t cycles = (target_pres_ns - next_vblank_ns + (period_ns / 2)) / period_ns;
            if (cycles < 0)
                cycles = 0;
            int64_t aligned_vblank_ns = next_vblank_ns + cycles * period_ns;

            int64_t lead_time_ns = period_ns / 4;
            if (lead_time_ns > 1000000LL)
                lead_time_ns = 1000000LL;
            int64_t target_fire_ns = aligned_vblank_ns - lead_time_ns;

            if (target_fire_ns > now_ns) {
                next_interval_ns = target_fire_ns - now_ns;
            }
        }
    }
    if (next_interval_ns < 1000000LL)
        next_interval_ns = 1000000LL;

    struct itimerspec its = {.it_interval = {0, 0},
                             .it_value = {
                                 .tv_sec = next_interval_ns / 1000000000LL,
                                 .tv_nsec = next_interval_ns % 1000000000LL,
                             }};
    timerfd_settime(ve->timer_fd, 0, &its, NULL);
}

void video_engine_pause(video_engine_t *ve)
{
    if (!ve || ve->state != WAYWAL_VIDEO_STATE_PLAYING)
        return;

    struct itimerspec its = {0};
    timerfd_settime(ve->timer_fd, 0, &its, NULL);
    uint64_t dummy;
    while (read(ve->timer_fd, &dummy, sizeof(dummy)) > 0);
    ve->state = WAYWAL_VIDEO_STATE_PAUSED;
    WAYWAL_LOG_INFO("Hardware video playback paused (0%% VPU usage)");
}

void video_engine_resume(video_engine_t *ve)
{
    if (!ve || ve->state != WAYWAL_VIDEO_STATE_PAUSED)
        return;

    ve->state = WAYWAL_VIDEO_STATE_PLAYING;
    ve->playback_epoch_ns = 0;
    WAYWAL_LOG_INFO("Hardware video playback resumed");

    /* Arm timer to trigger next frame immediately */
    struct itimerspec its = {
        .it_interval = {0, 0}, .it_value = {.tv_sec = 0, .tv_nsec = 1000000L} /* 1ms */
    };
    uint64_t dummy;
    while (read(ve->timer_fd, &dummy, sizeof(dummy)) > 0);
    timerfd_settime(ve->timer_fd, 0, &its, NULL);
}

void video_engine_stop(video_engine_t *ve)
{
    if (!ve || (ve->state == WAYWAL_VIDEO_STATE_STOPPED && !ve->decoder.initialized))
        return;

    struct itimerspec its = {0};
    if (ve->timer_fd >= 0) {
        timerfd_settime(ve->timer_fd, 0, &its, NULL);
        uint64_t dummy;
        while (read(ve->timer_fd, &dummy, sizeof(dummy)) > 0);
    }

    ve->state = WAYWAL_VIDEO_STATE_STOPPED;
    ve->reorder_count = 0;
    ve->has_decoded_any = false;
    ve->playback_epoch_ns = 0;

    video_engine_release_wl_buffers(ve);

    if (ve->daemon_state) {
        for (output_node_t *out = ve->daemon_state->outputs; out != NULL; out = out->next) {
            out->vp_configured = false;
        }
    }

    if (ve->decoder.initialized) {
        vaapi_decoder_destroy(&ve->decoder);
    }

    if (ve->demuxer) {
        ve->demuxer->destroy(ve->demuxer);
        ve->demuxer = NULL;
    }

    ve->target_output = NULL;
    ve->has_target_filter = false;
    malloc_trim(0);
    WAYWAL_LOG_INFO("Hardware video playback stopped");
}
