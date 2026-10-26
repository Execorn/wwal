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
    bool restore_on_start;
    bool pause_on_occluded_workspace;
    bool pause_on_idle;
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
} waywal_theme_config_t;

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
    uint32_t fill_color;
    uint32_t fps;
    char mode[32];
    uint32_t interval;
    bool shuffle;
    bool audio;
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
} waywal_workspace_rule_t;

typedef struct {
    char on_theme_change[512];
    char on_wallpaper_change[512];
} waywal_hooks_config_t;

typedef struct {
    waywal_daemon_config_t daemon;
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
} waywal_config_t;

void waywal_config_init_defaults(waywal_config_t *out_cfg);

bool waywal_config_parse_string(const char *toml_src, waywal_config_t *out_cfg, char *err_buf,
                                size_t err_cap);

bool waywal_config_load_file(const char *file_path, waywal_config_t *out_cfg, char *err_buf,
                             size_t err_cap);

bool waywal_config_load_default(waywal_config_t *out_cfg);

#ifdef __cplusplus
}
#endif

#endif
