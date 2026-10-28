// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_TOML_PARSER_H
#define WAYWAL_TOML_PARSER_H

#include "waywal/palette_hct.h"
#include "waywal/path.h"
#include "waywal/state_store.h"
#include "waywal/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAYWAL_CONFIG_MAX_OUTPUTS    16U
#define WAYWAL_CONFIG_MAX_WORKSPACES 32U
#define WAYWAL_MAX_TEMPLATES         64U
#define WAYWAL_CONFIG_MAX_TEMPLATES  WAYWAL_MAX_TEMPLATES
#define WAYWAL_CONFIG_MAX_SCHEDULE   32U
#define WAYWAL_MAX_SCHEDULES         WAYWAL_CONFIG_MAX_SCHEDULE

typedef enum : uint32_t {
    WAYWAL_LAYER_BACKGROUND = 0,
    WAYWAL_LAYER_BOTTOM = 1,
} waywal_layer_pos_t;

#ifndef WAYWAL_BATTERY_POLICY_DEFINED
#define WAYWAL_BATTERY_POLICY_DEFINED
typedef enum : uint32_t {
    WAYWAL_BATTERY_IGNORE = 0,
    WAYWAL_BATTERY_REDUCE_FPS = 1,
    WAYWAL_BATTERY_PAUSE_VIDEO = 2,
} waywal_battery_policy_t;
#endif

typedef struct {
    char layer[32];
    waywal_layer_pos_t layer_pos;
#ifndef __cplusplus
    union {
        char namespace[64];
        char namespace_str[64];
    };
#else
    char namespace_str[64];
#endif
    char battery_policy[32];
    waywal_battery_policy_t battery_policy_mode;
    union {
        bool auto_pause_fullscreen;
        bool pause_on_fullscreen;
    };
    union {
        bool auto_pause_maximized;
        bool pause_on_maximized;
    };
    uint32_t idle_timeout_sec;
    bool ten_bit;
    char backdrop[WAYWAL_PATH_MAX];
    char workshop_dir[WAYWAL_PATH_MAX];
    bool restore_on_start;
    bool pause_on_occluded_workspace;
    bool pause_on_idle;
    float volume;
    bool mute_on_audio;
    bool theme_from_media;
    char crop_gravity[32];
    float crop_gravity_x;
    float crop_gravity_y;
    bool has_crop_gravity;
    union {
        uint32_t interval;
        uint32_t interval_sec;
        uint32_t slideshow_interval;
    };
    union {
        char sorting[32];
        char slideshow_sorting[32];
    };
    union {
        waywal_sort_mode_t sort_mode;
        waywal_sort_mode_t sorting_mode;
        waywal_sort_mode_t sort;
        waywal_sort_mode_t slideshow_sort;
    };
    union {
        bool shuffle;
        bool slideshow_shuffle;
    };
    union {
        bool recursive;
        bool slideshow_recursive;
    };
    union {
        bool has_interval;
        bool has_slideshow_interval;
    };
    union {
        bool has_sorting;
        bool has_slideshow_sort;
    };
    union {
        bool has_shuffle;
        bool has_slideshow_shuffle;
    };
    bool has_slideshow_recursive;
    bool auto_day_night;
    bool caelestia_sync;
} waywal_daemon_config_t;

typedef struct {
    union {
        bool enabled;
        bool enable;
    };
    char mode[16];
    waywal_theme_mode_t theme_mode;
    bool caelestia_sync;
    union {
        bool tty_sequences;
        bool terminal_osc;
    };
    char scheme_variant[32];
    float contrast;
    bool export_css;
    bool export_hyprland;
    bool theme_from_media;
    uint8_t source_color_index;
    uint32_t seed_color;
    bool has_seed_color;
    bool pywal_compat;
    bool auto_day_night;
} waywal_theme_config_t;

typedef struct {
    char name[64];
    char input_path[1024];
    char output_path[1024];
    char post_hook[1024];
    bool enabled;
} waywal_template_rule_t;

typedef struct {
    char type[32];
    uint32_t type_enum;
    float duration;
    uint32_t fps;
    float bezier[4];
    float angle;
    float pos_x;
    float pos_y;
    float wave_x;
    float wave_y;
    uint32_t step;
    char pos[32];
    char sync_mode[32];
    uint32_t stagger_delay_ms;
    char crop_gravity[32];
    float crop_gravity_x;
    float crop_gravity_y;
    bool has_crop_gravity;
    bool invert_y;
    bool pos_y_inverted;
} waywal_transition_config_t;

typedef struct {
    union {
        char output_name[64];
        char name[64];
    };
    char path[WAYWAL_PATH_MAX];
    waywal_scaling_mode_t scaling;
    char scaling_str[32];
    waywal_filter_mode_t filter;
    char filter_str[32];
    union {
        uint32_t fill_color;
        uint32_t color;
    };
    uint32_t fps;
    char mode[32];
    union {
        uint32_t interval;
        uint32_t interval_sec;
    };
    bool shuffle;
    bool audio;
    float volume;
    bool mute_on_audio;
    char crop_gravity[32];
    float crop_gravity_x;
    float crop_gravity_y;
    bool has_crop_gravity;
    bool invert_y;
    bool has_invert_y;
    char sorting[32];
    union {
        waywal_sort_mode_t sort_mode;
        waywal_sort_mode_t sorting_mode;
        waywal_sort_mode_t sort;
    };
    union {
        bool has_sorting;
        bool has_sort_mode;
    };
    bool has_shuffle;
    bool has_interval;
    bool has_mode;
    bool has_scaling;
    bool has_filter;
    union {
        bool has_fill_color;
        bool has_color;
    };
    bool has_explicit_color_key;
    bool recursive;
    bool has_recursive;
    bool has_path;
    bool has_fps;
    bool has_audio;
    bool has_volume;
    bool has_mute_on_audio;
} waywal_output_rule_t;

typedef struct {
    union {
        char workspace_name[64];
        char name[64];
    };
    union {
        char output_name[64];
        char output[64];
    };
    char path[WAYWAL_PATH_MAX];
    union {
        char transition_type[32];
        char transition[32];
    };
    uint32_t transition_type_enum;
    union {
        bool has_transition_type;
        bool has_transition;
    };
    char mode[32];
    bool has_mode;
    waywal_scaling_mode_t scaling;
    char scaling_str[32];
    bool has_scaling;
    waywal_filter_mode_t filter;
    char filter_str[32];
    bool has_filter;
    union {
        uint32_t interval;
        uint32_t interval_sec;
    };
    bool has_interval;
    bool shuffle;
    char sorting[32];
    union {
        waywal_sort_mode_t sort_mode;
        waywal_sort_mode_t sorting_mode;
        waywal_sort_mode_t sort;
    };
    union {
        bool has_sorting;
        bool has_sort_mode;
    };
    bool recursive;
    bool has_recursive;
    uint32_t fps;
    bool has_fps;
    bool audio;
    bool has_audio;
    char prop_overrides[2048];
    union {
        uint32_t fill_color;
        uint32_t color;
    };
    union {
        bool has_fill_color;
        bool has_color;
    };
    float transition_duration;
    bool has_transition_duration;
    float transition_angle;
    bool has_transition_angle;
    bool directional;
    char crop_gravity[32];
    float crop_gravity_x;
    float crop_gravity_y;
    bool has_crop_gravity;
} waywal_workspace_rule_t;

typedef struct {
    char on_theme_change[512];
    char on_wallpaper_change[512];
} waywal_hooks_config_t;

typedef struct {
    bool enabled;
    double latitude;
    double longitude;
    bool has_coordinates;
    union {
        bool auto_day_night;
        bool auto_theme_mode;
    };
    char dawn_path[WAYWAL_PATH_MAX];
    char day_path[WAYWAL_PATH_MAX];
    char dusk_path[WAYWAL_PATH_MAX];
    char night_path[WAYWAL_PATH_MAX];
    char manifest_path[WAYWAL_PATH_MAX];
    char sunrise_str[16];
    char sunset_str[16];
    int32_t sunrise_fallback_min;
    int32_t sunset_fallback_min;
} waywal_solar_config_t;

typedef struct {
    char phase[32];
    union {
        char time_hhmm[16];
        char start[16];
    };
    char path[WAYWAL_PATH_MAX];
    char mode[32];
    char output[64];
    char theme_mode[16];
    int32_t time_minutes;
    bool has_phase;
    bool has_time;
    bool has_theme_mode;
    bool theme_light_mode;
} waywal_schedule_rule_t;

typedef waywal_schedule_rule_t waywal_schedule_entry_t;

typedef struct waywal_config {
    union {
        waywal_daemon_config_t daemon;
        waywal_daemon_config_t general;
    };
    waywal_theme_config_t theme;
    waywal_transition_config_t transition;
    waywal_hooks_config_t hooks;

    waywal_output_rule_t outputs[WAYWAL_CONFIG_MAX_OUTPUTS];
    union {
        size_t output_count;
        size_t num_outputs;
    };

    waywal_workspace_rule_t workspaces[WAYWAL_CONFIG_MAX_WORKSPACES];
    union {
        size_t workspace_count;
        size_t num_workspaces;
    };

    waywal_template_rule_t templates[WAYWAL_MAX_TEMPLATES];
    union {
        size_t template_count;
        size_t num_templates;
    };

    waywal_solar_config_t solar;
    union {
        waywal_schedule_rule_t schedule[WAYWAL_CONFIG_MAX_SCHEDULE];
        waywal_schedule_entry_t schedules[WAYWAL_CONFIG_MAX_SCHEDULE];
    };
    union {
        size_t schedule_count;
        size_t num_schedule;
        size_t num_schedules;
    };
    bool auto_day_night;
} waywal_config_t;

void waywal_config_init_defaults(waywal_config_t *out_cfg);

bool waywal_parse_crop_gravity(const char *str, float *out_x, float *out_y);
bool waywal_parse_duration_seconds(const char *str, uint32_t *out_seconds);
bool waywal_parse_sort_mode(const char *str, waywal_sort_mode_t *out_mode);
const waywal_output_rule_t *waywal_config_match_output(const waywal_config_t *cfg,
                                                       const char *output_name);

bool waywal_config_parse_string(const char *toml_src, waywal_config_t *out_cfg, char *err_buf,
                                size_t err_cap);

bool waywal_config_load_file(const char *file_path, waywal_config_t *out_cfg, char *err_buf,
                             size_t err_cap);

bool waywal_config_load_default(waywal_config_t *out_cfg);

#ifdef __cplusplus
}
#endif

#endif
