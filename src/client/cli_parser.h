#ifndef WAYWAL_CLI_PARSER_H
#define WAYWAL_CLI_PARSER_H

#include "waywal/types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLI_CMD_NONE,
    CLI_CMD_PING,
    CLI_CMD_QUERY,
    CLI_CMD_CLEAR,
    CLI_CMD_IMG,
    CLI_CMD_VIDEO,
    CLI_CMD_PAUSE,
    CLI_CMD_UNPAUSE,
    CLI_CMD_TOGGLE,
    CLI_CMD_SLIDESHOW,
    CLI_CMD_SLIDESHOW_CTRL,
    CLI_CMD_KILL,
    CLI_CMD_HELP,
} cli_cmd_type_t;

typedef struct {
    cli_cmd_type_t cmd;
    char namespace_str[64];
    char filepath[4096];
    color_rgba_t clear_color;
    bool verbose;

    /* Transition parameters */
    uint32_t transition_type;        /* waywal_transition_type_t */
    uint32_t transition_duration_ms; /* in milliseconds */
    uint32_t transition_fps;         /* FPS target (default: 60) */
    float transition_angle_rad;
    float transition_wave_freq;
    float transition_wave_amp;
    float transition_pos_x;
    float transition_pos_y;

    /* Custom shader options */
    char custom_shader_path[4096];
    char *custom_shader_src;
    size_t custom_shader_len;

    /* Multi-monitor targeting and synchronization */
    uint32_t num_outputs;
    char outputs[16][64];
    uint32_t sync_mode;        /* 0 = simultaneous, 1 = staggered */
    uint32_t stagger_delay_ms; /* delay in ms */

    /* 10-bit color option */
    bool enable_10bit;

    /* Scaling / aspect ratio mode */
    uint32_t scaling_mode; /* waywal_scaling_mode_t (default: 0 = WAYWAL_SCALING_FILL) */

    /* Video parameters */
    uint64_t video_loop_count; /* 0 = infinite loop */
    float video_speed;         /* default 1.0f */

    /* Slideshow parameters */
    uint32_t slideshow_action;     /* waywal_slideshow_action_t */
    uint32_t slideshow_interval_s; /* interval in seconds */
    bool slideshow_random;         /* random shuffle order */
} cli_options_t;

bool cli_parse(int argc, char *argv[], cli_options_t *opts);
void cli_print_usage(const char *prog);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_CLI_PARSER_H */
