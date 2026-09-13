#ifndef WAYWAL_VIDEO_ENGINE_H
#define WAYWAL_VIDEO_ENGINE_H

#include <stdbool.h>
#include <stdint.h>
#include "waywal/demuxer.h"
#include "waywal/vaapi_dec.h"

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

typedef struct {
    waywal_video_state_t state;
    demuxer_t           *demuxer;
    vaapi_decoder_t      decoder;
    int                  timer_fd;       /* timerfd for frame pacing */
    int64_t              start_time_us;
    int64_t              last_pts_us;
    uint64_t             loop_count;     /* 0 = infinite loop */
    uint64_t             current_loop;
    float                playback_speed; /* default 1.0f */

    struct output_node  *target_output;  /* NULL = all outputs */
    struct daemon_state *daemon_state;

    bool                 seamless_looping;
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
