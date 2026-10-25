#ifndef WAYWAL_CLI_PARSER_H
#define WAYWAL_CLI_PARSER_H

#include "waywal/bezier.h"
#include "waywal/state_store.h"
#include "waywal/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef WAYWAL_RENDER_ENGINE_H
#ifndef WAYWAL_TRANSITION_TYPE_DEFINED
#define WAYWAL_TRANSITION_TYPE_DEFINED
typedef enum {
    WAYWAL_TRANSITION_NONE = 0,
    WAYWAL_TRANSITION_SIMPLE = 1,
    WAYWAL_TRANSITION_FADE = 2,
    WAYWAL_TRANSITION_WIPE = 3,
    WAYWAL_TRANSITION_GROW = 4,
    WAYWAL_TRANSITION_OUTER = 5,
    WAYWAL_TRANSITION_WAVE = 6,
    WAYWAL_TRANSITION_NOISE = 7,
    WAYWAL_TRANSITION_CROSSZOOM = 8,
    WAYWAL_TRANSITION_SLIDE = 9,
    WAYWAL_TRANSITION_GLITCH = 10,
    WAYWAL_TRANSITION_BURN = 11,
    WAYWAL_TRANSITION_RIPPLE = 12,
    WAYWAL_TRANSITION_PIXELATE = 13,
    WAYWAL_TRANSITION_DOOM = 14,
    WAYWAL_TRANSITION_SWIRL = 15,
    WAYWAL_TRANSITION_CUBE = 16,
    WAYWAL_TRANSITION_LUMA = 17,
    WAYWAL_TRANSITION_LIGHT_LEAK = 18,
    WAYWAL_TRANSITION_PAGE_CURL = 19,
    WAYWAL_TRANSITION_CUSTOM = 20,
    WAYWAL_TRANSITION_RANDOM = 21,
} waywal_transition_type_t;
#endif
#endif

typedef enum {
    CLI_CMD_NONE,
    CLI_CMD_PING,
    CLI_CMD_QUERY,
    CLI_CMD_CLEAR,
    CLI_CMD_COLOR = CLI_CMD_CLEAR,
    CLI_CMD_IMG,
    CLI_CMD_VIDEO,
    CLI_CMD_PAUSE,
    CLI_CMD_UNPAUSE,
    CLI_CMD_TOGGLE,
    CLI_CMD_SLIDESHOW,
    CLI_CMD_SLIDESHOW_CTRL,
    CLI_CMD_KILL,
    CLI_CMD_HELP,
    CLI_CMD_SHADER,
    CLI_CMD_THEME,
    CLI_CMD_STATS,
    CLI_CMD_RESTORE,
    CLI_CMD_CLEAR_CACHE,
    CLI_CMD_DAEMON_ALIAS,
} cli_cmd_type_t;

typedef cli_cmd_type_t waywal_cli_cmd_t;

typedef struct {
    cli_cmd_type_t cmd;
    char namespace_str[64];
    union {
        char filepath[4096];
        char path[4096];
        char image_path[4096];
    };
    color_rgba_t clear_color;
    bool verbose;

    bool is_swww_compat;
    bool audio_reactive;
    bool extract_theme;
    bool no_theme;
    bool theme_light_mode;
    char backdrop[256];
    char layer_name[32];
    char battery_policy[32];
    bool read_stdin;

    /* Transition parameters */
    uint32_t transition_type;        /* waywal_transition_type_t */
    uint32_t transition_duration_ms; /* in milliseconds */
    float transition_duration;
    union {
        uint32_t transition_fps; /* FPS target (default: 60) */
        uint32_t fps;
    };
    uint32_t transition_step;
    float transition_angle_rad;
    union {
        float transition_angle;
        float angle;
    };
    union {
        float transition_wave_freq;
        float transition_wave_x;
    };
    union {
        float transition_wave_amp;
        float transition_wave_y;
    };
    union {
        float transition_pos_x;
        float pos_x;
    };
    union {
        float transition_pos_y;
        float pos_y;
    };
    bezier_curve_t bezier;
    bool has_custom_bezier;

    /* Custom shader options */
    char custom_shader_path[4096];
    char *custom_shader_src;
    size_t custom_shader_len;

    /* Multi-monitor targeting and synchronization */
    uint32_t num_outputs;
    char outputs[16][64];
    char output[64];
    uint32_t sync_mode;        /* 0 = simultaneous, 1 = staggered */
    uint32_t stagger_delay_ms; /* delay in ms */

    /* 10-bit color option */
    union {
        bool enable_10bit;
        bool ten_bit;
    };

    /* Scaling / aspect ratio mode */
    union {
        uint32_t scaling_mode; /* waywal_scaling_mode_t (default: 0 = WAYWAL_SCALING_FILL) */
        waywal_scaling_mode_t scaling;
    };

    /* Resampling filter mode */
    union {
        waywal_filter_mode_t filter;
        waywal_filter_mode_t filter_mode;
    };

    /* Video parameters */
    uint64_t video_loop_count; /* 0 = infinite loop */
    float video_speed;         /* default 1.0f */
    bool animate;              /* route animated GIF to hardware video engine */

    /* Slideshow parameters */
    uint32_t slideshow_action;     /* waywal_slideshow_action_t */
    uint32_t slideshow_interval_s; /* interval in seconds */
    bool slideshow_random;         /* random shuffle order */
} cli_options_t;

typedef cli_options_t waywal_cli_opts_t;

bool cli_parse(int argc, char *argv[], cli_options_t *opts);
bool waywal_cli_parse(int argc, char *argv[], waywal_cli_opts_t *opts);
void cli_print_usage(const char *prog);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_CLI_PARSER_H */
