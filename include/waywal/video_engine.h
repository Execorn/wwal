// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_VIDEO_ENGINE_H
#define WAYWAL_VIDEO_ENGINE_H

#include "waywal/demuxer.h"
#include "waywal/vaapi_dec.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WAYWAL_VIDEO_STATE_STOPPED = 0,
    WAYWAL_VIDEO_STATE_PLAYING,
    WAYWAL_VIDEO_STATE_PAUSED,
} waywal_video_state_t;

struct daemon_state;
struct output_node;

#define WAYWAL_VIDEO_REORDER_CAP 6

typedef struct {
    VASurfaceID surface;
    uint32_t surf_idx;
    int64_t pts_us;
    int32_t poc;
    uint64_t gop_id;
    uint64_t loop_id;
} video_reorder_entry_t;

typedef struct {
    waywal_video_state_t state;
    demuxer_t *demuxer;
    vaapi_decoder_t decoder;
    int timer_fd; /* timerfd for frame pacing */
    int64_t start_time_us;
    int64_t last_pts_us;
    int64_t playback_epoch_ns;
    uint64_t presented_loop_id;
    uint64_t loop_count; /* 0 = infinite loop */
    uint64_t current_loop;
    float playback_speed; /* default 1.0f */

    video_reorder_entry_t reorder_buf[WAYWAL_VIDEO_REORDER_CAP];
    uint32_t reorder_count;
    uint64_t current_gop_id;
    uint64_t next_present_gop;
    int32_t next_present_poc;
    bool has_decoded_any;
    struct wl_buffer *video_wl_buffers[WAYWAL_VA_SURFACE_POOL_SIZE];
    bool video_buffer_in_use[WAYWAL_VA_SURFACE_POOL_SIZE];
    bool combined_busy_mask[WAYWAL_VA_SURFACE_POOL_SIZE];
    uint32_t last_vpp_idx;

    struct output_node *target_output; /* NULL = all or filtered outputs */
    bool has_target_filter;
    struct daemon_state *daemon_state;

    bool seamless_looping;
    bool eof_draining;
    bool using_swdec;
    void *swdec_ctx;
} video_engine_t;

bool video_engine_init(video_engine_t *ve, struct daemon_state *state);
void video_engine_destroy(video_engine_t *ve);

bool video_engine_load_file(video_engine_t *ve, const char *filepath, struct output_node *out);
bool video_engine_load_mem(video_engine_t *ve, int memfd, size_t size, struct output_node *out);

/* Called by main epoll loop when timer_fd is readable */
void video_engine_dispatch_frame(video_engine_t *ve);

void video_engine_pause(video_engine_t *ve);
void video_engine_resume(video_engine_t *ve);
void video_engine_stop(video_engine_t *ve);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_VIDEO_ENGINE_H */
