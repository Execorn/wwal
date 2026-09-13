#include "waywal/video_engine.h"

#include "linux-dmabuf-v1-client-protocol.h"
#include "wayland_core.h"
#include "waywal/log.h"

#include <errno.h>
#include <fcntl.h>
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

static struct wl_buffer *ensure_output_video_buffer(video_engine_t *ve, output_node_t *out,
                                                    VASurfaceID surface, uint32_t surf_idx)
{
    if (!ve || !out || surf_idx >= WAYWAL_VA_SURFACE_POOL_SIZE)
        return NULL;

    if (out->video_wl_buffers[surf_idx]) {
        return out->video_wl_buffers[surf_idx];
    }

    struct zwp_linux_dmabuf_v1 *dmabuf_proto = ve->daemon_state->dmabuf_ctx.dmabuf_proto;
    if (!dmabuf_proto)
        return NULL;

    vaapi_prime_frame_t prime_frame;
    if (!vaapi_decoder_export_prime(&ve->decoder, surface, &prime_frame)) {
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
        wl_buffer_add_listener(wl_buf, &g_video_buffer_listener,
                               &out->video_buffer_in_use[surf_idx]);
        out->video_wl_buffers[surf_idx] = wl_buf;
        out->video_buffer_in_use[surf_idx] = false;
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

static bool video_engine_start_playback(video_engine_t *ve, output_node_t *out)
{
    if (!ve || !ve->demuxer)
        return false;

    int drm_fd = ve->daemon_state->dmabuf_ctx.drm_fd;
    if (!vaapi_decoder_init(&ve->decoder, drm_fd, ve->demuxer->codec, ve->demuxer->width,
                            ve->demuxer->height)) {
        WAYWAL_LOG_ERR("Failed to initialize hardware VA-API decoder for video");
        ve->demuxer->destroy(ve->demuxer);
        ve->demuxer = NULL;
        ve->state = WAYWAL_VIDEO_STATE_STOPPED;
        return false;
    }

    ve->target_output = out;
    ve->state = WAYWAL_VIDEO_STATE_PLAYING;
    ve->current_loop = 0;
    ve->last_pts_us = 0;

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

    if (ve->state != WAYWAL_VIDEO_STATE_STOPPED) {
        video_engine_stop(ve);
    }

    ve->demuxer = demuxer_open_file(filepath);
    if (!ve->demuxer) {
        WAYWAL_LOG_ERR("Failed to demux video file '%s'", filepath);
        return false;
    }

    return video_engine_start_playback(ve, out);
}

bool video_engine_load_mem(video_engine_t *ve, int memfd, size_t size, output_node_t *out)
{
    if (!ve || memfd < 0 || size == 0)
        return false;

    if (ve->state != WAYWAL_VIDEO_STATE_STOPPED) {
        video_engine_stop(ve);
    }

    void *mapped = mmap(NULL, size, PROT_READ, MAP_SHARED, memfd, 0);
    if (mapped == MAP_FAILED) {
        WAYWAL_LOG_ERR("mmap failed for video memfd: %s", strerror(errno));
        return false;
    }

    ve->demuxer = demuxer_open_mem((const uint8_t *)mapped, size);
    if (!ve->demuxer) {
        WAYWAL_LOG_ERR("Failed to demux video from memory buffer");
        munmap(mapped, size);
        return false;
    }

    typedef struct {
        int file_fd;
        bool owns_mmap;
    } demuxer_common_state_t;
    demuxer_common_state_t *state = (demuxer_common_state_t *)ve->demuxer->internal_state;
    if (state) {
        state->owns_mmap = true;
    }

    return video_engine_start_playback(ve, out);
}

void video_engine_dispatch_frame(video_engine_t *ve)
{
    if (!ve || ve->state != WAYWAL_VIDEO_STATE_PLAYING || !ve->demuxer) {
        return;
    }

    /* Drain timerfd notifications */
    uint64_t expirations = 0;
    ssize_t s = read(ve->timer_fd, &expirations, sizeof(expirations));
    (void)s;

    demux_packet_t pkt;
    if (!ve->demuxer->read_packet(ve->demuxer, &pkt)) {
        /* EOF reached: check loop limit */
        ve->current_loop++;
        if (ve->loop_count > 0 && ve->current_loop >= ve->loop_count) {
            WAYWAL_LOG_INFO("Video loop count (%lu) reached, stopping playback", ve->loop_count);
            video_engine_stop(ve);
            return;
        }

        /* Seamless looping: rewind demuxer and read Frame 0 */
        ve->demuxer->seek_us(ve->demuxer, 0);
        if (!ve->demuxer->read_packet(ve->demuxer, &pkt)) {
            WAYWAL_LOG_WARN("Failed to seek to start of stream on video loop");
            video_engine_stop(ve);
            return;
        }
    }

    ve->last_pts_us = pkt.pts_us;

    /* Submit packet to hardware VPU decoder */
    VASurfaceID decoded_surface = VA_INVALID_SURFACE;
    if (vaapi_decoder_decode_packet(&ve->decoder, &pkt, &decoded_surface)) {
        /* Determine pool surface index */
        uint32_t surf_idx = 0;
        for (uint32_t i = 0; i < WAYWAL_VA_SURFACE_POOL_SIZE; ++i) {
            if (ve->decoder.surfaces[i] == decoded_surface) {
                surf_idx = i;
                break;
            }
        }

        /* Attach and commit persistent wl_buffer to target output(s) (BUG-07, ARCH-04) */
        output_node_t *out = ve->target_output ? ve->target_output : ve->daemon_state->outputs;
        while (out != NULL) {
            if (out->surface) {
                struct wl_buffer *wl_buf =
                    ensure_output_video_buffer(ve, out, decoded_surface, surf_idx);
                if (wl_buf) {
                    if (out->viewport) {
                        wp_viewport_set_destination(out->viewport, out->width, out->height);
                        wp_viewport_set_source(out->viewport, wl_fixed_from_int(-1),
                                               wl_fixed_from_int(-1), wl_fixed_from_int(-1),
                                               wl_fixed_from_int(-1));
                    }
                    if (out->pres_sync.wp_pres) {
                        presentation_sync_request(&out->pres_sync);
                    }
                    wl_surface_attach(out->surface, wl_buf, 0, 0);
                    wl_surface_damage_buffer(out->surface, 0, 0, INT32_MAX, INT32_MAX);
                    wl_surface_commit(out->surface);
                    out->video_buffer_in_use[surf_idx] = true;
                }
            }
            out = ve->target_output ? NULL : out->next;
        }

        wl_display_flush(ve->daemon_state->display);
    }

    /* Compute next frame interval with high monotonic precision */
    int64_t frame_interval_us = 33333LL; /* Default ~30fps */
    if (ve->demuxer->fps_num > 0) {
        frame_interval_us =
            (1000000LL * (int64_t)ve->demuxer->fps_den) / (int64_t)ve->demuxer->fps_num;
    }

    if (ve->playback_speed > 0.01f) {
        frame_interval_us = (int64_t)((float)frame_interval_us / ve->playback_speed);
    }
    if (frame_interval_us < 1000LL)
        frame_interval_us = 1000LL;

    int64_t next_interval_ns = frame_interval_us * 1000LL;

    /* VRR PLL VBlank Pacing: snap frame intervals to discrete VBlank multiples */
    output_node_t *target_out = ve->target_output
                                    ? ve->target_output
                                    : (ve->daemon_state ? ve->daemon_state->outputs : NULL);
    if (target_out && target_out->pres_sync.pll.phase_est_ns > 0 &&
        target_out->pres_sync.pll.period_est_ns > 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t now_ns = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
        int64_t next_vblank_ns = presentation_pll_predict_next_vblank(&target_out->pres_sync.pll);
        int64_t period_ns = target_out->pres_sync.pll.period_est_ns;

        /* Target presentation timestamp is current time + video frame interval */
        int64_t target_pres_ns = now_ns + next_interval_ns;

        if (target_pres_ns > next_vblank_ns) {
            int64_t cycles = (target_pres_ns - next_vblank_ns + (period_ns / 2)) / period_ns;
            if (cycles < 1)
                cycles = 1;
            int64_t aligned_vblank_ns = next_vblank_ns + cycles * period_ns;

            /* Wake up ~1.0 ms (or 1/4 refresh cycle) prior to aligned hardware VBlank */
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
    ve->state = WAYWAL_VIDEO_STATE_PAUSED;
    WAYWAL_LOG_INFO("Hardware video playback paused (0%% VPU usage)");
}

void video_engine_resume(video_engine_t *ve)
{
    if (!ve || ve->state != WAYWAL_VIDEO_STATE_PAUSED)
        return;

    ve->state = WAYWAL_VIDEO_STATE_PLAYING;
    WAYWAL_LOG_INFO("Hardware video playback resumed");

    /* Arm timer to trigger next frame immediately */
    struct itimerspec its = {
        .it_interval = {0, 0}, .it_value = {.tv_sec = 0, .tv_nsec = 1000000L} /* 1ms */
    };
    timerfd_settime(ve->timer_fd, 0, &its, NULL);
}

void video_engine_stop(video_engine_t *ve)
{
    if (!ve || ve->state == WAYWAL_VIDEO_STATE_STOPPED)
        return;

    struct itimerspec its = {0};
    if (ve->timer_fd >= 0) {
        timerfd_settime(ve->timer_fd, 0, &its, NULL);
    }

    ve->state = WAYWAL_VIDEO_STATE_STOPPED;

    /* Clean up persistent video buffer pool across all outputs (BUG-07, ARCH-04) */
    output_node_t *out = ve->daemon_state ? ve->daemon_state->outputs : NULL;
    while (out != NULL) {
        for (uint32_t i = 0; i < WAYWAL_VA_SURFACE_POOL_SIZE; ++i) {
            if (out->video_wl_buffers[i]) {
                wl_buffer_destroy(out->video_wl_buffers[i]);
                out->video_wl_buffers[i] = NULL;
                out->video_buffer_in_use[i] = false;
            }
        }
        out = out->next;
    }

    if (ve->decoder.initialized) {
        vaapi_decoder_destroy(&ve->decoder);
    }

    if (ve->demuxer) {
        ve->demuxer->destroy(ve->demuxer);
        ve->demuxer = NULL;
    }

    ve->target_output = NULL;
    WAYWAL_LOG_INFO("Hardware video playback stopped");
}
