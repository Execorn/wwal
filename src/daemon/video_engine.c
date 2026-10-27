// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/video_engine.h"

#include "linux-dmabuf-v1-client-protocol.h"
#include "wayland_core.h"
#include "waywal/log.h"
#include "waywal/power_governor.h"
#include "waywal/render_engine.h"
#include "waywal/shader_engine.h"
#include "waywal/we_scene.h"

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
    int *in_use = (int *)data;
    if (in_use && *in_use > 0) {
        (*in_use)--;
    }
}

static const struct wl_buffer_listener g_video_buffer_listener = {
    .release = video_buffer_release,
};

#ifdef HAVE_FFMPEG_SWDEC
#include <dlfcn.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#define SWDEC_GBM_RING_SIZE 4

typedef struct {
    void *lib_avcodec;
    void *lib_avutil;
    void *lib_swscale;

    const AVCodec *(*p_avcodec_find_decoder)(enum AVCodecID id);
    AVCodecContext *(*p_avcodec_alloc_context3)(const AVCodec *codec);
    void (*p_avcodec_free_context)(AVCodecContext **avctx);
    int (*p_avcodec_open2)(AVCodecContext *avctx, const AVCodec *codec, AVDictionary **options);
    int (*p_avcodec_send_packet)(AVCodecContext *avctx, const AVPacket *avpkt);
    int (*p_avcodec_receive_frame)(AVCodecContext *avctx, AVFrame *frame);
    void (*p_avcodec_flush_buffers)(AVCodecContext *avctx);

    AVPacket *(*p_av_packet_alloc)(void);
    void (*p_av_packet_free)(AVPacket **pkt);
    void (*p_av_packet_unref)(AVPacket *pkt);
    AVFrame *(*p_av_frame_alloc)(void);
    void (*p_av_frame_free)(AVFrame **frame);
    void (*p_av_frame_unref)(AVFrame *frame);
    void *(*p_av_mallocz)(size_t size);

    struct SwsContext *(*p_sws_getContext)(int srcW, int srcH, enum AVPixelFormat srcFormat,
                                           int dstW, int dstH, enum AVPixelFormat dstFormat,
                                           int flags, SwsFilter *srcFilter, SwsFilter *dstFilter,
                                           const double *param);
    int (*p_sws_scale)(struct SwsContext *c, const uint8_t *const srcSlice[], const int srcStride[],
                       int srcSliceY, int srcSliceH, uint8_t *const dst[], const int dstStride[]);
    void (*p_sws_freeContext)(struct SwsContext *swsContext);

    AVCodecContext *codec_ctx;
    AVPacket *pkt;
    AVFrame *frame;
    struct SwsContext *sws_ctx;
    uint8_t *rgba_buf;
    struct gbm_bo *gbm_bos[SWDEC_GBM_RING_SIZE];
    struct wl_buffer *wl_bufs[SWDEC_GBM_RING_SIZE];
    int buf_busy[SWDEC_GBM_RING_SIZE];
    uint32_t ring_idx;
    int last_ready_ring_idx;
    bool has_gbm_ring;
    bool eof_flushing;
    uint32_t width;
    uint32_t height;
    int last_src_fmt;
} ffmpeg_swdec_t;

static void *dlopen_any(const char *const *names, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        void *h = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);
        if (h) {
            return h;
        }
    }
    return NULL;
}

static bool swdec_load_symbols(ffmpeg_swdec_t *sw)
{
    static const char *const avcodec_names[] = {"libavcodec.so.61", "libavcodec.so.60",
                                                "libavcodec.so.59", "libavcodec.so"};
    static const char *const avutil_names[] = {"libavutil.so.59", "libavutil.so.58",
                                               "libavutil.so.57", "libavutil.so"};
    static const char *const swscale_names[] = {"libswscale.so.8", "libswscale.so.7",
                                                "libswscale.so.6", "libswscale.so"};

    sw->lib_avutil = dlopen_any(avutil_names, sizeof(avutil_names) / sizeof(avutil_names[0]));
    sw->lib_avcodec = dlopen_any(avcodec_names, sizeof(avcodec_names) / sizeof(avcodec_names[0]));
    sw->lib_swscale = dlopen_any(swscale_names, sizeof(swscale_names) / sizeof(swscale_names[0]));
    if (!sw->lib_avutil || !sw->lib_avcodec || !sw->lib_swscale) {
        return false;
    }

#define RESOLVE_SYM(handle, field, sym)                                                            \
    do {                                                                                           \
        *(void **)(&sw->field) = dlsym(handle, sym);                                               \
        if (!sw->field)                                                                            \
            return false;                                                                          \
    } while (0)

    RESOLVE_SYM(sw->lib_avcodec, p_avcodec_find_decoder, "avcodec_find_decoder");
    RESOLVE_SYM(sw->lib_avcodec, p_avcodec_alloc_context3, "avcodec_alloc_context3");
    RESOLVE_SYM(sw->lib_avcodec, p_avcodec_free_context, "avcodec_free_context");
    RESOLVE_SYM(sw->lib_avcodec, p_avcodec_open2, "avcodec_open2");
    RESOLVE_SYM(sw->lib_avcodec, p_avcodec_send_packet, "avcodec_send_packet");
    RESOLVE_SYM(sw->lib_avcodec, p_avcodec_receive_frame, "avcodec_receive_frame");
    RESOLVE_SYM(sw->lib_avcodec, p_avcodec_flush_buffers, "avcodec_flush_buffers");
    RESOLVE_SYM(sw->lib_avcodec, p_av_packet_alloc, "av_packet_alloc");
    RESOLVE_SYM(sw->lib_avcodec, p_av_packet_free, "av_packet_free");
    RESOLVE_SYM(sw->lib_avcodec, p_av_packet_unref, "av_packet_unref");

    RESOLVE_SYM(sw->lib_avutil, p_av_frame_alloc, "av_frame_alloc");
    RESOLVE_SYM(sw->lib_avutil, p_av_frame_free, "av_frame_free");
    RESOLVE_SYM(sw->lib_avutil, p_av_frame_unref, "av_frame_unref");
    RESOLVE_SYM(sw->lib_avutil, p_av_mallocz, "av_mallocz");

    RESOLVE_SYM(sw->lib_swscale, p_sws_getContext, "sws_getContext");
    RESOLVE_SYM(sw->lib_swscale, p_sws_scale, "sws_scale");
    RESOLVE_SYM(sw->lib_swscale, p_sws_freeContext, "sws_freeContext");

#undef RESOLVE_SYM
    return true;
}

static void swdec_destroy(video_engine_t *ve)
{
    if (!ve || !ve->swdec_ctx)
        return;
    ffmpeg_swdec_t *sw = (ffmpeg_swdec_t *)ve->swdec_ctx;
    for (int i = 0; i < SWDEC_GBM_RING_SIZE; ++i) {
        if (sw->wl_bufs[i]) {
            wl_buffer_destroy(sw->wl_bufs[i]);
            sw->wl_bufs[i] = NULL;
        }
        if (sw->gbm_bos[i]) {
            gbm_bo_destroy(sw->gbm_bos[i]);
            sw->gbm_bos[i] = NULL;
        }
    }
    if (sw->sws_ctx && sw->p_sws_freeContext) {
        sw->p_sws_freeContext(sw->sws_ctx);
    }
    if (sw->frame && sw->p_av_frame_free) {
        sw->p_av_frame_free(&sw->frame);
    }
    if (sw->pkt && sw->p_av_packet_free) {
        sw->p_av_packet_free(&sw->pkt);
    }
    if (sw->codec_ctx && sw->p_avcodec_free_context) {
        sw->p_avcodec_free_context(&sw->codec_ctx);
    }
    if (sw->lib_swscale) {
        dlclose(sw->lib_swscale);
    }
    if (sw->lib_avcodec) {
        dlclose(sw->lib_avcodec);
    }
    if (sw->lib_avutil) {
        dlclose(sw->lib_avutil);
    }
    free(sw->rgba_buf);
    free(sw);
    ve->swdec_ctx = NULL;
    ve->using_swdec = false;
}

static bool swdec_init(video_engine_t *ve, const demuxer_t *demux)
{
    if (!ve || !demux || demux->width == 0 || demux->height == 0)
        return false;

    enum AVCodecID cid = AV_CODEC_ID_NONE;
    switch (demux->codec) {
    case WAYWAL_CODEC_H264:
        cid = AV_CODEC_ID_H264;
        break;
    case WAYWAL_CODEC_HEVC:
        cid = AV_CODEC_ID_HEVC;
        break;
    case WAYWAL_CODEC_VP9:
        cid = AV_CODEC_ID_VP9;
        break;
    case WAYWAL_CODEC_AV1:
        cid = AV_CODEC_ID_AV1;
        break;
    default:
        return false;
    }

    ffmpeg_swdec_t *sw = (ffmpeg_swdec_t *)calloc(1, sizeof(ffmpeg_swdec_t));
    if (!sw)
        return false;

    if (!swdec_load_symbols(sw)) {
        ve->swdec_ctx = sw;
        swdec_destroy(ve);
        return false;
    }

    const AVCodec *codec = sw->p_avcodec_find_decoder(cid);
    if (!codec) {
        ve->swdec_ctx = sw;
        swdec_destroy(ve);
        return false;
    }

    sw->codec_ctx = sw->p_avcodec_alloc_context3(codec);
    sw->pkt = sw->p_av_packet_alloc();
    sw->frame = sw->p_av_frame_alloc();
    sw->width = demux->width;
    sw->height = demux->height;
    sw->last_src_fmt = -1;
    sw->last_ready_ring_idx = -1;

    if (!sw->codec_ctx || !sw->pkt || !sw->frame) {
        ve->swdec_ctx = sw;
        swdec_destroy(ve);
        return false;
    }

    sw->codec_ctx->width = (int)demux->width;
    sw->codec_ctx->height = (int)demux->height;
    sw->codec_ctx->thread_count = 2;

    if (demux->extradata && demux->extradata_size > 0) {
        sw->codec_ctx->extradata =
            (uint8_t *)sw->p_av_mallocz(demux->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (sw->codec_ctx->extradata) {
            memcpy(sw->codec_ctx->extradata, demux->extradata, demux->extradata_size);
            sw->codec_ctx->extradata_size = (int)demux->extradata_size;
        }
    }

    if (sw->p_avcodec_open2(sw->codec_ctx, codec, NULL) < 0) {
        ve->swdec_ctx = sw;
        swdec_destroy(ve);
        return false;
    }

    if (ve->daemon_state && ve->daemon_state->dmabuf_ctx.gbm &&
        ve->daemon_state->dmabuf_ctx.dmabuf_proto && ve->daemon_state->dmabuf_ctx.map_supported) {
        bool ring_ok = true;
        for (int i = 0; i < SWDEC_GBM_RING_SIZE; ++i) {
            sw->gbm_bos[i] =
                gbm_bo_create(ve->daemon_state->dmabuf_ctx.gbm, sw->width, sw->height,
                              DRM_FORMAT_XRGB8888, GBM_BO_USE_LINEAR | GBM_BO_USE_WRITE);
            if (!sw->gbm_bos[i]) {
                ring_ok = false;
                break;
            }
            int bo_fd = gbm_bo_get_fd(sw->gbm_bos[i]);
            if (bo_fd < 0) {
                ring_ok = false;
                break;
            }
            uint32_t stride = gbm_bo_get_stride(sw->gbm_bos[i]);
            uint64_t mod = DRM_FORMAT_MOD_LINEAR;
            struct zwp_linux_buffer_params_v1 *params =
                zwp_linux_dmabuf_v1_create_params(ve->daemon_state->dmabuf_ctx.dmabuf_proto);
            if (!params) {
                close(bo_fd);
                ring_ok = false;
                break;
            }
            zwp_linux_buffer_params_v1_add(params, bo_fd, 0, 0, stride, (uint32_t)(mod >> 32),
                                           (uint32_t)(mod & 0xFFFFFFFFULL));
            sw->wl_bufs[i] = zwp_linux_buffer_params_v1_create_immed(
                params, (int32_t)sw->width, (int32_t)sw->height, DRM_FORMAT_XRGB8888, 0);
            zwp_linux_buffer_params_v1_destroy(params);
            close(bo_fd);
            if (!sw->wl_bufs[i]) {
                ring_ok = false;
                break;
            }
            wl_buffer_add_listener(sw->wl_bufs[i], &g_video_buffer_listener, &sw->buf_busy[i]);
        }
        if (!ring_ok) {
            for (int i = 0; i < SWDEC_GBM_RING_SIZE; ++i) {
                if (sw->wl_bufs[i]) {
                    wl_buffer_destroy(sw->wl_bufs[i]);
                    sw->wl_bufs[i] = NULL;
                }
                if (sw->gbm_bos[i]) {
                    gbm_bo_destroy(sw->gbm_bos[i]);
                    sw->gbm_bos[i] = NULL;
                }
                sw->buf_busy[i] = false;
            }
        }
        sw->has_gbm_ring = ring_ok;
    }

    if (!sw->has_gbm_ring) {
        sw->rgba_buf = (uint8_t *)malloc((size_t)demux->width * (size_t)demux->height * 4ULL);
        if (!sw->rgba_buf) {
            ve->swdec_ctx = sw;
            swdec_destroy(ve);
            return false;
        }
    }

    ve->swdec_ctx = sw;
    ve->using_swdec = true;
    WAYWAL_LOG_INFO(
        "Initialized FFmpeg software video decoder fallback (%ux%u, codec=%d, gbm_ring=%d)",
        sw->width, sw->height, (int)demux->codec, (int)sw->has_gbm_ring);
    return true;
}

static bool swdec_decode_and_present(video_engine_t *ve, int64_t *out_pts_us)
{
    if (!ve || !ve->swdec_ctx || !ve->demuxer)
        return false;
    ffmpeg_swdec_t *sw = (ffmpeg_swdec_t *)ve->swdec_ctx;

    for (int tries = 0; tries < 32; ++tries) {
        if (sw->p_avcodec_receive_frame(sw->codec_ctx, sw->frame) == 0) {
            if (sw->frame->format != sw->last_src_fmt || !sw->sws_ctx) {
                if (sw->sws_ctx) {
                    sw->p_sws_freeContext(sw->sws_ctx);
                }
                sw->sws_ctx = sw->p_sws_getContext((int)sw->width, (int)sw->height,
                                                   (enum AVPixelFormat)sw->frame->format,
                                                   (int)sw->width, (int)sw->height, AV_PIX_FMT_BGRA,
                                                   SWS_BILINEAR, NULL, NULL, NULL);
                sw->last_src_fmt = sw->frame->format;
            }
            if (sw->sws_ctx) {
                sw->last_ready_ring_idx = -1;
                bool mapped_gbm = false;
                if (sw->has_gbm_ring) {
                    int chosen = -1;
                    for (int i = 0; i < SWDEC_GBM_RING_SIZE; ++i) {
                        int cand = (int)((sw->ring_idx + (uint32_t)i) % SWDEC_GBM_RING_SIZE);
                        if (!sw->buf_busy[cand]) {
                            chosen = cand;
                            break;
                        }
                    }
                    if (chosen < 0) {
                        chosen = (int)(sw->ring_idx % SWDEC_GBM_RING_SIZE);
                    }
                    sw->ring_idx = (uint32_t)(chosen + 1) % SWDEC_GBM_RING_SIZE;

                    uint32_t map_stride = 0;
                    void *map_data = NULL;
                    void *ptr = gbm_bo_map(sw->gbm_bos[chosen], 0, 0, sw->width, sw->height,
                                           GBM_BO_TRANSFER_WRITE, &map_stride, &map_data);
                    if (ptr && map_data && map_stride >= sw->width * 4U) {
                        uint8_t *dst_planes[4] = {(uint8_t *)ptr, NULL, NULL, NULL};
                        int dst_strides[4] = {(int)map_stride, 0, 0, 0};
                        sw->p_sws_scale(sw->sws_ctx, (const uint8_t *const *)sw->frame->data,
                                        sw->frame->linesize, 0, (int)sw->height, dst_planes,
                                        dst_strides);
                        gbm_bo_unmap(sw->gbm_bos[chosen], map_data);
                        sw->last_ready_ring_idx = chosen;
                        mapped_gbm = true;
                    }
                }
                if (!mapped_gbm) {
                    if (!sw->rgba_buf) {
                        sw->rgba_buf =
                            (uint8_t *)malloc((size_t)sw->width * (size_t)sw->height * 4U);
                    }
                    if (!sw->rgba_buf) {
                        sw->p_av_frame_unref(sw->frame);
                        return false;
                    }
                    uint8_t *dst_planes[4] = {sw->rgba_buf, NULL, NULL, NULL};
                    int dst_strides[4] = {(int)(sw->width * 4U), 0, 0, 0};
                    sw->p_sws_scale(sw->sws_ctx, (const uint8_t *const *)sw->frame->data,
                                    sw->frame->linesize, 0, (int)sw->height, dst_planes,
                                    dst_strides);
                }
                if (out_pts_us) {
                    *out_pts_us = sw->frame->pts;
                }
                sw->p_av_frame_unref(sw->frame);
                return true;
            }
            sw->p_av_frame_unref(sw->frame);
        }

        demux_packet_t dpkt;
        if (!ve->demuxer->read_packet(ve->demuxer, &dpkt)) {
            if (!sw->eof_flushing) {
                sw->eof_flushing = true;
                sw->p_avcodec_send_packet(sw->codec_ctx, NULL);
                continue;
            }
            sw->eof_flushing = false;
            if (ve->loop_count > 0 && ve->current_loop + 1 >= ve->loop_count) {
                ve->current_loop++;
                video_engine_stop(ve);
                return false;
            }
            ve->current_loop++;
            ve->demuxer->seek_us(ve->demuxer, 0);
            sw->p_avcodec_flush_buffers(sw->codec_ctx);
            if (!ve->demuxer->read_packet(ve->demuxer, &dpkt)) {
                video_engine_stop(ve);
                return false;
            }
        }

        sw->p_av_packet_unref(sw->pkt);
        sw->pkt->data = (uint8_t *)(uintptr_t)dpkt.data;
        sw->pkt->size = (int)dpkt.size;
        sw->pkt->pts = dpkt.pts_us;
        sw->pkt->dts = dpkt.dts_us;
        sw->p_avcodec_send_packet(sw->codec_ctx, sw->pkt);
    }
    return false;
}
#endif

static bool is_output_active_for_video(const video_engine_t *ve, const output_node_t *out)
{
    if (!ve || !out)
        return false;
    if (out->video_eng)
        return (out->video_eng == ve);
    if (ve->daemon_state && ve != &ve->daemon_state->video_engine)
        return (ve->target_output != NULL && out == ve->target_output);
    if (out->shader_active || out->anim_active)
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
    output_node_t *best_unpaused = NULL;
    output_node_t *best_any = NULL;
    for (output_node_t *out = ve->daemon_state->outputs; out != NULL; out = out->next) {
        if (!is_output_active_for_video(ve, out) || !out->surface)
            continue;
        if (!best_any || out->refresh_mhz > best_any->refresh_mhz) {
            best_any = out;
        }
        bool paused = ve->daemon_state->power_gov && waywal_power_governor_should_pause_output(
                                                         ve->daemon_state->power_gov, out->name);
        if (!paused && (!best_unpaused || out->refresh_mhz > best_unpaused->refresh_mhz)) {
            best_unpaused = out;
        }
    }
    return best_unpaused ? best_unpaused : best_any;
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
        WAYWAL_LOG_INFO(
            "Created shared video wl_buffer %p for surface %u (idx %u, %ux%u, fmt 0x%08x)",
            (void *)wl_buf, export_surf, buf_idx, prime_frame.width, prime_frame.height,
            prime_frame.drm_format);
        wl_buffer_add_listener(wl_buf, &g_video_buffer_listener, &ve->video_buffer_in_use[buf_idx]);
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
    ve->last_export_surf = VA_INVALID_SURFACE;

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

static void video_engine_release_wl_buffers(video_engine_t *ve);

void video_engine_destroy(video_engine_t *ve)
{
    if (!ve)
        return;
    video_engine_stop(ve);

    if (ve->decoder.initialized) {
        video_engine_release_wl_buffers(ve);
        vaapi_decoder_destroy(&ve->decoder);
    }

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

    int32_t dst_w = out->logical_width > 0 ? (int32_t)out->logical_width : out->width;
    int32_t dst_h = out->logical_height > 0 ? (int32_t)out->logical_height : out->height;

    if (out->scaling_mode == WAYWAL_SCALING_SPAN && out->state) {
        layer_shell_compute_span_bounds(out->state, out);
    }

    if (out->vp_configured && out->vp_dst_w == dst_w && out->vp_dst_h == dst_h &&
        out->vp_src_w == vid_w && out->vp_src_h == vid_h &&
        out->vp_scaling_mode == out->scaling_mode && out->scaling_mode != WAYWAL_SCALING_SPAN) {
        return;
    }

    wp_viewport_set_destination(out->viewport, dst_w, dst_h);

    if (out->scaling_mode == WAYWAL_SCALING_SPAN) {
        double tot_w = (out->span_total_w > 0) ? (double)out->span_total_w : (double)out->width;
        double tot_h = (out->span_total_h > 0) ? (double)out->span_total_h : (double)out->height;
        double r_src = (double)vid_w / (double)vid_h;
        double r_dst = tot_w / tot_h;
        double crop_w, crop_h, off_x, off_y;

        if (r_src > r_dst) {
            crop_h = (double)vid_h;
            crop_w = crop_h * r_dst;
            off_x = ((double)vid_w - crop_w) * 0.5;
            off_y = 0.0;
        } else {
            crop_w = (double)vid_w;
            crop_h = crop_w / r_dst;
            off_x = 0.0;
            off_y = ((double)vid_h - crop_h) * 0.5;
        }

        double sub_off_x = off_x + ((double)out->span_off_x / tot_w) * crop_w;
        double sub_off_y = off_y + ((double)out->span_off_y / tot_h) * crop_h;
        double sub_crop_w = ((double)out->width / tot_w) * crop_w;
        double sub_crop_h = ((double)out->height / tot_h) * crop_h;
        if (sub_crop_w < 1.0)
            sub_crop_w = 1.0;
        if (sub_crop_h < 1.0)
            sub_crop_h = 1.0;
        if (sub_off_x < 0.0)
            sub_off_x = 0.0;
        if (sub_off_y < 0.0)
            sub_off_y = 0.0;
        if (sub_off_x + sub_crop_w > (double)vid_w)
            sub_crop_w = (double)vid_w - sub_off_x;
        if (sub_off_y + sub_crop_h > (double)vid_h)
            sub_crop_h = (double)vid_h - sub_off_y;

        wp_viewport_set_source(out->viewport, wl_fixed_from_double(sub_off_x),
                               wl_fixed_from_double(sub_off_y), wl_fixed_from_double(sub_crop_w),
                               wl_fixed_from_double(sub_crop_h));
    } else if (out->scaling_mode == WAYWAL_SCALING_FILL) {
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

    out->vp_dst_w = dst_w;
    out->vp_dst_h = dst_h;
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
        while (read(ve->timer_fd, &dummy, sizeof(dummy)) > 0)
            ;
    }
    ve->state = WAYWAL_VIDEO_STATE_STOPPED;
    ve->reorder_count = 0;
    ve->eof_draining = false;
    ve->has_decoded_any = false;
    ve->playback_epoch_ns = 0;
    ve->last_export_surf = VA_INVALID_SURFACE;
#ifdef HAVE_FFMPEG_SWDEC
    swdec_destroy(ve);
#endif
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
        profile_matches =
            (dec->va_profile == VAProfileH264Main || dec->va_profile == VAProfileH264High ||
             dec->va_profile == VAProfileH264ConstrainedBaseline);
        break;
    case WAYWAL_CODEC_HEVC:
        profile_matches =
            (dec->va_profile == VAProfileHEVCMain || dec->va_profile == VAProfileHEVCMain10);
        break;
    case WAYWAL_CODEC_VP9:
        profile_matches =
            (dec->va_profile == VAProfileVP9Profile0 || dec->va_profile == VAProfileVP9Profile2);
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
    bool allow_direct_nv12 = has_nv12 && (!ve->decoder.initialized || !ve->decoder.is_nvidia_vaapi);
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
                for (output_node_t *node = ve->daemon_state->outputs; node != NULL;
                     node = node->next) {
                    if (is_output_active_for_video(ve, node)) {
                        output_node_render_color(ve->daemon_state, node,
                                                 ve->daemon_state->current_color);
                    }
                }
            }
            video_engine_release_wl_buffers(ve);
            vaapi_decoder_destroy(&ve->decoder);
        }
        ve->decoder.allow_direct_p010 = has_p010;
        if (!vaapi_decoder_init_with_params(
                &ve->decoder, drm_fd, ve->demuxer->codec, ve->demuxer->width, ve->demuxer->height,
                ve->demuxer->extradata, ve->demuxer->extradata_size, allow_direct_nv12)) {
#ifdef HAVE_FFMPEG_SWDEC
            if (!swdec_init(ve, ve->demuxer)) {
                WAYWAL_LOG_ERR(
                    "Failed to initialize both VA-API hardware and FFmpeg software video decoder");
                ve->demuxer->destroy(ve->demuxer);
                ve->demuxer = NULL;
                ve->state = WAYWAL_VIDEO_STATE_STOPPED;
                ve->reorder_count = 0;
                ve->has_decoded_any = false;
                return false;
            }
#else
            WAYWAL_LOG_ERR("Failed to initialize hardware VA-API decoder for video");
            ve->demuxer->destroy(ve->demuxer);
            ve->demuxer = NULL;
            ve->state = WAYWAL_VIDEO_STATE_STOPPED;
            ve->reorder_count = 0;
            ve->has_decoded_any = false;
            return false;
#endif
        }
    }

    ve->target_output = out;
    ve->state = WAYWAL_VIDEO_STATE_PLAYING;
    ve->current_loop = 0;
    ve->presented_loop_id = 0;
    ve->last_pts_us = 0;
    ve->playback_epoch_ns = 0;
    ve->reorder_count = 0;
    ve->eof_draining = false;
    ve->current_gop_id = 0;
    ve->next_present_gop = 0;
    ve->next_present_poc = 0;
    ve->has_decoded_any = false;
    ve->last_vpp_idx = 0;
    ve->last_export_surf = VA_INVALID_SURFACE;
    memset(ve->combined_busy_mask, 0, sizeof(ve->combined_busy_mask));

    if (ve->daemon_state && !ve->using_swdec) {
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
        ve->filepath[0] = '\0';
        video_engine_release_wl_buffers(ve);
        if (ve->decoder.initialized) {
            vaapi_decoder_destroy(&ve->decoder);
        }
        return false;
    }

    if (!video_engine_start_playback(ve, out)) {
        ve->filepath[0] = '\0';
        return false;
    }
    snprintf(ve->filepath, sizeof(ve->filepath), "%s", filepath);
    return true;
}

bool video_engine_load_mem(video_engine_t *ve, int memfd, size_t size, output_node_t *out)
{
    if (!ve || memfd < 0 || size == 0)
        return false;

    bool saved_filter = ve->has_target_filter;
    video_engine_prepare_reload(ve);
    ve->has_target_filter = saved_filter;
    ve->filepath[0] = '\0';

    void *mapped = mmap(NULL, size, PROT_READ, PROT_READ ? MAP_SHARED : MAP_SHARED, memfd, 0);
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
            if (ve->video_buffer_in_use[i] > 0) {
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
    if (ve->eof_draining)
        return true;

    uint32_t target_depth = 1;
    if (ve->demuxer) {
        if (ve->demuxer->codec == WAYWAL_CODEC_HEVC) {
            if (ve->decoder.cached_hevc_sps.valid) {
                target_depth = (uint32_t)ve->decoder.cached_hevc_sps.sps_max_num_reorder_pics + 1U;
            } else {
                target_depth = 3U;
            }
        } else if (ve->demuxer->codec == WAYWAL_CODEC_H264) {
            if (ve->decoder.cached_sps.valid && (ve->decoder.cached_sps.profile_idc == 66 ||
                                                 ve->decoder.cached_sps.num_ref_frames <= 1 ||
                                                 ve->decoder.cached_sps.pic_order_cnt_type == 2)) {
                target_depth = 1U;
            } else {
                target_depth = 4U;
            }
        }
    }
    if (target_depth > WAYWAL_VIDEO_REORDER_CAP - 1U) {
        target_depth = WAYWAL_VIDEO_REORDER_CAP - 1U;
    }
    if (ve->reorder_count >= target_depth)
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

    if (ve->next_present_poc == 0) {
        for (uint32_t i = 0; i < ve->reorder_count; ++i) {
            if (ve->reorder_buf[i].gop_id == ve->next_present_gop && ve->reorder_buf[i].poc <= 0) {
                return true;
            }
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

    output_node_t *pll_ref_out = select_pll_reference_output(ve);

    uint32_t base_fps = (ve->demuxer->fps_num > 0 && ve->demuxer->fps_den > 0)
                            ? (ve->demuxer->fps_num / ve->demuxer->fps_den)
                            : 30U;
    if (base_fps == 0)
        base_fps = 30U;

    uint32_t eff_fps = base_fps;
    if (ve->daemon_state && ve->daemon_state->power_gov && ve->has_decoded_any) {
        uint32_t max_out_fps = 0;
        bool found_active_out = false;
        for (output_node_t *out = ve->daemon_state->outputs; out != NULL; out = out->next) {
            if (!is_output_active_for_video(ve, out) || !out->surface)
                continue;
            found_active_out = true;
            uint32_t out_fps = waywal_power_governor_effective_fps(
                ve->daemon_state->power_gov, out->name[0] ? out->name : NULL, base_fps);
            if (out_fps > max_out_fps)
                max_out_fps = out_fps;
        }
        eff_fps = found_active_out
                      ? max_out_fps
                      : waywal_power_governor_effective_fps(ve->daemon_state->power_gov,
                                                            pll_ref_out ? pll_ref_out->name : NULL,
                                                            base_fps);
        if (eff_fps == 0) {
            /* All target outputs occluded or paused by power governor: check again in 250ms */
            struct itimerspec its_idle = {.it_interval = {0, 0},
                                          .it_value = {.tv_sec = 0, .tv_nsec = 250000000L}};
            timerfd_settime(ve->timer_fd, 0, &its_idle, NULL);
            return;
        }
    }

#ifdef HAVE_FFMPEG_SWDEC
    if (ve->using_swdec && ve->swdec_ctx) {
        int64_t sw_pts_us = 0;
        bool first_frame = !ve->has_decoded_any;
        if (swdec_decode_and_present(ve, &sw_pts_us)) {
            ve->has_decoded_any = true;
            ffmpeg_swdec_t *sw = (ffmpeg_swdec_t *)ve->swdec_ctx;
            if (ve->daemon_state) {
                render_engine_bump_upload_gen(&ve->daemon_state->render_engine);
                for (output_node_t *out = ve->daemon_state->outputs; out != NULL; out = out->next) {
                    if (!is_output_active_for_video(ve, out) || !out->surface)
                        continue;
                    if (!first_frame && ve->daemon_state->power_gov &&
                        waywal_power_governor_should_pause_output(ve->daemon_state->power_gov,
                                                                  out->name)) {
                        continue;
                    }
                    if (sw->has_gbm_ring && sw->last_ready_ring_idx >= 0 &&
                        sw->wl_bufs[sw->last_ready_ring_idx]) {
                        int r_idx = sw->last_ready_ring_idx;
                        sw->buf_busy[r_idx]++;
                        configure_output_video_viewport(out, sw->width, sw->height);
                        wl_surface_attach(out->surface, sw->wl_bufs[r_idx], 0, 0);
                        wl_surface_damage_buffer(out->surface, 0, 0, (int32_t)sw->width,
                                                 (int32_t)sw->height);
                        wl_surface_commit(out->surface);
                        out->has_image = true;
                    } else if (sw->rgba_buf) {
                        output_node_render_image(ve->daemon_state, out, sw->rgba_buf, sw->width,
                                                 sw->height);
                    }
                }
                wl_display_flush(ve->daemon_state->display);
            }
        }
        if (ve->state != WAYWAL_VIDEO_STATE_PLAYING)
            return;

        int64_t sw_interval_us = 1000000LL / (int64_t)eff_fps;
        double sw_speed = (ve->playback_speed > 0.01f) ? (double)ve->playback_speed : 1.0;
        sw_interval_us = (int64_t)((double)sw_interval_us / sw_speed);
        if (sw_interval_us < 1000LL)
            sw_interval_us = 1000LL;
        int64_t sw_interval_ns = sw_interval_us * 1000LL;
        struct itimerspec sw_its = {.it_interval = {0, 0},
                                    .it_value = {
                                        .tv_sec = sw_interval_ns / 1000000000LL,
                                        .tv_nsec = sw_interval_ns % 1000000000LL,
                                    }};
        timerfd_settime(ve->timer_fd, 0, &sw_its, NULL);
        return;
    }
#endif

    uint32_t pool_size =
        ve->decoder.num_surfaces > 0 ? ve->decoder.num_surfaces : WAYWAL_VA_SURFACE_POOL_SIZE;

    if (ve->eof_draining && ve->reorder_count == 0) {
        ve->eof_draining = false;
        if (ve->loop_count > 0 && ve->current_loop + 1 >= ve->loop_count) {
            ve->current_loop++;
            WAYWAL_LOG_INFO("Video loop count (%lu) reached, stopping playback", ve->loop_count);
            video_engine_stop(ve);
            return;
        }
        ve->current_loop++;
        ve->demuxer->seek_us(ve->demuxer, 0);
        vaapi_decoder_reset(&ve->decoder);
        ve->current_gop_id++;
    }

    while (!ve->eof_draining && ve->reorder_count < WAYWAL_VIDEO_REORDER_CAP) {
        if (reorder_has_ready_frame(ve)) {
            break;
        }

        demux_packet_t pkt;
        if (!ve->demuxer->read_packet(ve->demuxer, &pkt)) {
            if (ve->reorder_count > 0) {
                /* Drain remaining B-frame / reorder_buf frames in POC order before looping */
                ve->eof_draining = true;
                break;
            }
            if (ve->loop_count > 0 && ve->current_loop + 1 >= ve->loop_count) {
                ve->current_loop++;
                WAYWAL_LOG_INFO("Video loop count (%lu) reached, stopping playback",
                                ve->loop_count);
                video_engine_stop(ve);
                return;
            }
            ve->current_loop++;

            ve->demuxer->seek_us(ve->demuxer, 0);
            vaapi_decoder_reset(&ve->decoder);
            ve->current_gop_id++;
            if (!ve->demuxer->read_packet(ve->demuxer, &pkt)) {
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
    if (eff_fps < base_fps && eff_fps > 0) {
        int64_t throttled_us = 1000000LL / (int64_t)eff_fps;
        if (throttled_us > frame_interval_us) {
            frame_interval_us = throttled_us;
        }
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

    if (ve->reorder_count > 0) {
        uint32_t best_idx = 0;
        for (uint32_t i = 1; i < ve->reorder_count; ++i) {
            const video_reorder_entry_t *a = &ve->reorder_buf[i];
            const video_reorder_entry_t *b = &ve->reorder_buf[best_idx];
            if (a->gop_id < b->gop_id || (a->gop_id == b->gop_id && a->poc < b->poc) ||
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
        ve->next_present_poc = pres.poc + 1;

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
                if (ve->video_buffer_in_use[k] <= 0) {
                    vpp_idx = k;
                    break;
                }
            }
            ve->last_vpp_idx = vpp_idx;
            export_surf = ve->decoder.vpp_surfaces[vpp_idx];
            buf_idx = vpp_idx;
            vaapi_decoder_vpp_blit(&ve->decoder, pres.surface, export_surf);
        }
        ve->last_export_surf = export_surf;

        struct wl_buffer *wl_buf = ensure_shared_video_buffer(ve, export_surf, buf_idx);
        if (wl_buf && ve->daemon_state) {
            int attached_count = 0;
            for (output_node_t *out = ve->daemon_state->outputs; out != NULL; out = out->next) {
                if (!is_output_active_for_video(ve, out) || !out->surface)
                    continue;
                if (ve->daemon_state->power_gov && out->has_image &&
                    waywal_power_governor_should_pause_output(ve->daemon_state->power_gov,
                                                              out->name)) {
                    continue;
                }
                configure_output_video_viewport(out, ve->decoder.width, ve->decoder.height);
                if (out == pll_ref_out && out->pres_sync.wp_pres) {
                    presentation_sync_request(&out->pres_sync);
                }
                wl_surface_attach(out->surface, wl_buf, 0, 0);
                wl_surface_damage_buffer(out->surface, 0, 0, INT32_MAX, INT32_MAX);
                wl_surface_commit(out->surface);
                out->has_image = true;
                attached_count++;
            }
            if (attached_count > 0) {
                ve->video_buffer_in_use[buf_idx] += attached_count;
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
    while (read(ve->timer_fd, &dummy, sizeof(dummy)) > 0)
        ;
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
    while (read(ve->timer_fd, &dummy, sizeof(dummy)) > 0)
        ;
    timerfd_settime(ve->timer_fd, 0, &its, NULL);
}

void video_engine_stop(video_engine_t *ve)
{
    if (!ve || (ve->state == WAYWAL_VIDEO_STATE_STOPPED && !ve->demuxer && !ve->using_swdec))
        return;

    struct itimerspec its = {0};
    if (ve->timer_fd >= 0) {
        timerfd_settime(ve->timer_fd, 0, &its, NULL);
        uint64_t dummy;
        while (read(ve->timer_fd, &dummy, sizeof(dummy)) > 0)
            ;
    }

    ve->state = WAYWAL_VIDEO_STATE_STOPPED;
    ve->reorder_count = 0;
    ve->eof_draining = false;
    ve->has_decoded_any = false;
    ve->playback_epoch_ns = 0;
    ve->last_export_surf = VA_INVALID_SURFACE;
    ve->filepath[0] = '\0';

    video_engine_release_wl_buffers(ve);

#ifdef HAVE_FFMPEG_SWDEC
    swdec_destroy(ve);
#endif

    if (ve->daemon_state) {
        for (output_node_t *out = ve->daemon_state->outputs; out != NULL; out = out->next) {
            out->vp_configured = false;
            if (out->video_eng == ve) {
                out->video_eng = NULL;
            }
        }
    }

    if (ve->decoder.initialized) {
        if (ve->daemon_state && ve == &ve->daemon_state->video_engine) {
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
        } else {
            vaapi_decoder_destroy(&ve->decoder);
        }
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

bool video_engine_snapshot_output(video_engine_t *ve, output_node_t *out)
{
    if (!ve || !out || !ve->daemon_state || out->width <= 0 || out->height <= 0)
        return false;

    daemon_state_t *state = ve->daemon_state;
    uint32_t w = (uint32_t)out->width;
    uint32_t h = (uint32_t)out->height;
    uint64_t src_mod = state->dmabuf_ctx.preferred_modifier;

    if (!out->has_source_old_bo || out->source_old_bo.width != w ||
        out->source_old_bo.height != h) {
        if (out->has_source_old_bo) {
            if (out->shader_eng) {
                waywal_shader_engine_forget_bo(out->shader_eng, &out->source_old_bo);
            }
            if (state->shader_eng && state->shader_eng != out->shader_eng) {
                waywal_shader_engine_forget_bo(state->shader_eng, &out->source_old_bo);
            }
            if (out->we_scene) {
                waywal_we_scene_forget_bo(out->we_scene, &out->source_old_bo);
            }
            if (state->we_scene && state->we_scene != out->we_scene) {
                waywal_we_scene_forget_bo(state->we_scene, &out->source_old_bo);
            }
            render_engine_release_bo(&state->render_engine, &out->source_old_bo);
            dmabuf_bo_free(&out->source_old_bo);
            out->has_source_old_bo = false;
        }
        if (dmabuf_bo_allocate(&state->dmabuf_ctx, &out->source_old_bo, w, h, DRM_FORMAT_ARGB8888,
                               src_mod)) {
            out->has_source_old_bo = true;
        }
    }
    if (!out->has_source_old_bo)
        return false;

    layer_shell_compute_span_bounds(state, out);
    render_engine_set_span_bounds(&state->render_engine, out->span_off_x, out->span_off_y,
                                  out->span_total_w, out->span_total_h);
    uint32_t clear_col = ((uint32_t)out->fill_color.a << 24) | ((uint32_t)out->fill_color.r << 16) |
                         ((uint32_t)out->fill_color.g << 8) | (uint32_t)out->fill_color.b;

    if (ve->decoder.initialized && ve->last_export_surf != VA_INVALID_SURFACE) {
        vaapi_prime_frame_t prime_frame;
        if (vaapi_decoder_export_prime(&ve->decoder, ve->last_export_surf, &prime_frame)) {
            bool ok = false;
            if (prime_frame.num_planes >= 1 && prime_frame.num_planes <= WAYWAL_MAX_BUFFER_PLANES &&
                (prime_frame.drm_format == DRM_FORMAT_XRGB8888 ||
                 prime_frame.drm_format == DRM_FORMAT_ARGB8888)) {
                dmabuf_bo_t tmp_bo;
                memset(&tmp_bo, 0, sizeof(tmp_bo));
                for (int p = 0; p < WAYWAL_MAX_BUFFER_PLANES; ++p)
                    tmp_bo.fd[p] = -1;
                tmp_bo.num_planes = prime_frame.num_planes;
                tmp_bo.width = prime_frame.width;
                tmp_bo.height = prime_frame.height;
                for (uint32_t p = 0; p < prime_frame.num_planes; ++p) {
                    tmp_bo.fd[p] = prime_frame.fds[p];
                    tmp_bo.stride[p] = prime_frame.strides[p];
                    tmp_bo.offset[p] = prime_frame.offsets[p];
                }
                tmp_bo.drm_format = prime_frame.drm_format;
                tmp_bo.modifier = prime_frame.modifiers[0];
                ok = render_engine_copy_bo(&state->render_engine, &out->source_old_bo, &tmp_bo,
                                           out->scaling_mode, clear_col, true);
                render_engine_release_bo(&state->render_engine, &tmp_bo);
            }
            vaapi_prime_frame_close(&prime_frame);
            if (ok) {
                out->has_image = true;
                out->last_presented_bo = &out->source_old_bo;
                return true;
            }
        }
    }

#ifdef HAVE_FFMPEG_SWDEC
    if (ve->using_swdec && ve->swdec_ctx) {
        ffmpeg_swdec_t *sw = (ffmpeg_swdec_t *)ve->swdec_ctx;
        if (sw->has_gbm_ring && sw->last_ready_ring_idx >= 0 &&
            sw->gbm_bos[sw->last_ready_ring_idx]) {
            struct gbm_bo *gbo = sw->gbm_bos[sw->last_ready_ring_idx];
            int fd = gbm_bo_get_fd(gbo);
            if (fd >= 0) {
                dmabuf_bo_t tmp_bo;
                memset(&tmp_bo, 0, sizeof(tmp_bo));
                for (int p = 0; p < WAYWAL_MAX_BUFFER_PLANES; ++p)
                    tmp_bo.fd[p] = -1;
                tmp_bo.fd[0] = fd;
                tmp_bo.num_planes = 1;
                tmp_bo.width = sw->width;
                tmp_bo.height = sw->height;
                tmp_bo.stride[0] = gbm_bo_get_stride(gbo);
                tmp_bo.drm_format = DRM_FORMAT_XRGB8888;
                tmp_bo.modifier = DRM_FORMAT_MOD_LINEAR;
                bool ok = render_engine_copy_bo(&state->render_engine, &out->source_old_bo, &tmp_bo,
                                                out->scaling_mode, clear_col, true);
                render_engine_release_bo(&state->render_engine, &tmp_bo);
                close(fd);
                if (ok) {
                    out->has_image = true;
                    out->last_presented_bo = &out->source_old_bo;
                    return true;
                }
            }
        }
        if (sw->rgba_buf && sw->width > 0 && sw->height > 0) {
            if (render_engine_upload_static(&state->render_engine, sw->rgba_buf, sw->width,
                                            sw->height, &out->source_old_bo, out->scaling_mode,
                                            clear_col)) {
                out->has_image = true;
                out->last_presented_bo = &out->source_old_bo;
                return true;
            }
        }
    }
#endif

    return false;
}
