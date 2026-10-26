// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_VIDEO_DECODER_H
#define WAYWAL_VIDEO_DECODER_H

#include "waywal/demuxer.h"
#include "waywal/vaapi_dec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WAYWAL_VIDEO_BACKEND_VAAPI = 0,
    WAYWAL_VIDEO_BACKEND_VULKAN_VIDEO,
    WAYWAL_VIDEO_BACKEND_V4L2_M2M,
    WAYWAL_VIDEO_BACKEND_COUNT
} waywal_video_backend_type_t;

typedef struct {
    const char *backend_name;
    waywal_video_backend_type_t backend_type;
    bool (*init)(void *ctx, int drm_render_fd, waywal_codec_t codec, uint32_t width,
                 uint32_t height, const uint8_t *extradata, size_t extradata_size,
                 bool allow_direct_nv12);
    bool (*decode_packet)(void *ctx, const demux_packet_t *pkt, uint32_t *out_surface);
    bool (*export_prime)(void *ctx, uint32_t surface, vaapi_prime_frame_t *out_frame);
    void (*release_frame)(vaapi_prime_frame_t *frame);
    void (*reset)(void *ctx);
    void (*destroy)(void *ctx);
} waywal_video_decoder_ops_t;

extern const waywal_video_decoder_ops_t g_waywal_vaapi_decoder_ops;
extern const waywal_video_decoder_ops_t g_waywal_vulkan_video_decoder_ops;
extern const waywal_video_decoder_ops_t g_waywal_v4l2_m2m_decoder_ops;

const waywal_video_decoder_ops_t *waywal_video_decoder_get_ops(waywal_video_backend_type_t backend);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_VIDEO_DECODER_H */
