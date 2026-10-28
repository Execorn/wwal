// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_SLIDESHOW_H
#define WAYWAL_SLIDESHOW_H

#include "waywal/ipc_proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct daemon_state;

typedef struct slideshow_engine {
    bool active;
    bool paused;
    int timer_fd;
    uint32_t interval_s;
    uint32_t transition_type;
    uint32_t transition_duration_ms;
    uint32_t transition_fps;
    float transition_angle_rad;
    float transition_wave_freq;
    float transition_wave_amp;
    float transition_center_x;
    float transition_center_y;
    bool random_order;
    uint32_t num_target_outputs;
    char target_outputs[16][64];
    uint32_t scaling_mode; /* waywal_scaling_mode_t */
    uint32_t filter_mode;  /* waywal_filter_mode_t */
    bezier_curve_t bezier;
    bool has_custom_bezier;
    uint8_t color[4];
    bool ten_bit;
    float crop_gravity_x;
    float crop_gravity_y;
    uint8_t has_crop_gravity;
    uint8_t invert_y;
    uint8_t pos_y_inverted;
    bool no_cache;
    bool extract_theme;
    bool no_theme;
    bool has_theme_mode;
    bool theme_light_mode;
    bool has_source_color_index;
    uint8_t source_color_index;
    bool has_seed_color;
    uint32_t seed_color;
    uint8_t auto_day_night;
    uint32_t sort_mode; /* waywal_sort_mode_t */
    bool has_sort_mode;
    bool recursive;
    char last_played_path[4096];

    char root_path[4096];
    char **file_list;
    size_t num_files;
    size_t capacity_files;
    size_t current_file_idx;

    struct daemon_state *daemon_state;
} slideshow_engine_t;

bool slideshow_init(slideshow_engine_t *ss, struct daemon_state *state);
bool slideshow_clone(slideshow_engine_t *dst, const slideshow_engine_t *src);
void slideshow_destroy(slideshow_engine_t *ss);

bool slideshow_start(slideshow_engine_t *ss, const waywal_slideshow_payload_t *payload);
bool slideshow_rescan(slideshow_engine_t *ss);
void slideshow_stop(slideshow_engine_t *ss);
void slideshow_pause(slideshow_engine_t *ss);
void slideshow_resume(slideshow_engine_t *ss);
void slideshow_toggle(slideshow_engine_t *ss);
void slideshow_next(slideshow_engine_t *ss);
void slideshow_prev(slideshow_engine_t *ss);
bool slideshow_control(slideshow_engine_t *ss, uint32_t action);

/* Called by main epoll/io_uring event loop when timer_fd triggers */
void slideshow_dispatch_tick(slideshow_engine_t *ss);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_SLIDESHOW_H */
