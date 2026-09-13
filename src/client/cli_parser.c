#include "cli_parser.h"

#include <ctype.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cli_print_usage(const char *prog)
{
    printf("wwal - Modern high-performance Wayland wallpaper client (C23)\n");
    printf("Usage: %s [GLOBAL_OPTIONS] <COMMAND> [ARGS...]\n\n", prog);
    printf("Commands:\n");
    printf("  ping             Check if wwald daemon is running and responsive\n");
    printf("  query            List all detected Wayland outputs and current geometry\n");
    printf("  clear [COLOR]    Clear wallpaper to solid hex color (e.g. 000000 or 00ff00)\n");
    printf("  img <PATH> [OPTIONS] Load and set wallpaper with GPU/SIMD transitions\n");
    printf(
        "  video <PATH> [OPTIONS] Load and play hardware-accelerated video wallpaper (VA-API)\n");
    printf("  pause            Pause video playback\n");
    printf("  unpause          Resume video playback\n");
    printf("  toggle           Toggle video playback pause/resume\n");
    printf("  kill             Gracefully terminate the running wwald daemon\n");
    printf("  help             Display this help message\n\n");
    printf("Video Options:\n");
    printf("  --loop <COUNT>             Loop count (0 = infinite) (default: 0)\n");
    printf("  --speed <FLOAT>            Playback speed multiplier (default: 1.0)\n\n");
    printf("Image Transition Options:\n");
    printf("  --transition-type <TYPE>   none, simple, fade, wipe, grow, outer, wave, noise, "
           "crosszoom, slide, glitch, burn, ripple, pixelate, doom, swirl, cube, luma, light_leak, "
           "page_curl (default: fade)\n");
    printf("  --transition-duration <S>  Duration in seconds (e.g. 1.0, 0.5) (default: 1.0)\n");
    printf("  --transition-fps <FPS>     Target frame rate (default: 60)\n");
    printf("  --transition-angle <DEG>   Wipe/wave/slide angle in degrees (default: 0)\n");
    printf("  --transition-wave <F,A>    Wave frequency/scale and amplitude/intensity (default: "
           "20,0.05)\n");
    printf("  --transition-pos <X,Y>     Center coordinate 0.0-1.0 (default: 0.5,0.5)\n\n");
    printf("Global Options:\n");
    printf("  -n, --namespace <NAME>  Socket namespace (default: \"default\")\n");
    printf("  -v, --verbose           Enable verbose output\n");
    printf("  -h, --help              Print help information\n");
}

static bool parse_hex_color(const char *str, color_rgba_t *out_color)
{
    if (!str || !out_color)
        return false;
    if (str[0] == '#')
        str++;

    size_t len = strlen(str);
    if (len != 6 && len != 8) {
        return false;
    }

    for (size_t i = 0; i < len; ++i) {
        if (!isxdigit((unsigned char)str[i])) {
            return false;
        }
    }

    unsigned int r = 0, g = 0, b = 0, a = 255;
    if (len == 6) {
        if (sscanf(str, "%02x%02x%02x", &r, &g, &b) != 3) {
            return false;
        }
    } else {
        if (sscanf(str, "%02x%02x%02x%02x", &r, &g, &b, &a) != 4) {
            return false;
        }
    }

    out_color->r = (uint8_t)r;
    out_color->g = (uint8_t)g;
    out_color->b = (uint8_t)b;
    out_color->a = (uint8_t)a;
    return true;
}

static uint32_t parse_transition_type(const char *str)
{
    if (!str)
        return 2;
    if (strcmp(str, "none") == 0)
        return 0;
    if (strcmp(str, "simple") == 0)
        return 1;
    if (strcmp(str, "fade") == 0)
        return 2;
    if (strcmp(str, "wipe") == 0)
        return 3;
    if (strcmp(str, "grow") == 0)
        return 4;
    if (strcmp(str, "outer") == 0)
        return 5;
    if (strcmp(str, "wave") == 0)
        return 6;
    if (strcmp(str, "noise") == 0)
        return 7;
    if (strcmp(str, "crosszoom") == 0)
        return 8;
    if (strcmp(str, "slide") == 0)
        return 9;
    if (strcmp(str, "glitch") == 0)
        return 10;
    if (strcmp(str, "burn") == 0)
        return 11;
    if (strcmp(str, "ripple") == 0)
        return 12;
    if (strcmp(str, "pixelate") == 0)
        return 13;
    if (strcmp(str, "doom") == 0)
        return 14;
    if (strcmp(str, "swirl") == 0)
        return 15;
    if (strcmp(str, "cube") == 0)
        return 16;
    if (strcmp(str, "luma") == 0)
        return 17;
    if (strcmp(str, "light_leak") == 0)
        return 18;
    if (strcmp(str, "page_curl") == 0)
        return 19;
    return 2;
}

bool cli_parse(int argc, char *argv[], cli_options_t *opts)
{
    if (!opts)
        return false;
    memset(opts, 0, sizeof(*opts));
    strncpy(opts->namespace_str, "default", sizeof(opts->namespace_str) - 1);
    opts->clear_color = (color_rgba_t){.r = 0, .g = 0, .b = 0, .a = 255};
    opts->transition_type = 2; /* WAYWAL_TRANSITION_FADE */
    opts->transition_duration_ms = 1000;
    opts->transition_fps = 60;
    opts->transition_angle_rad = 0.0f;
    opts->transition_wave_freq = 20.0f;
    opts->transition_wave_amp = 0.05f;
    opts->transition_pos_x = 0.5f;
    opts->transition_pos_y = 0.5f;
    opts->video_loop_count = 0;
    opts->video_speed = 1.0f;

    static const struct option long_options[] = {{"namespace", required_argument, NULL, 'n'},
                                                 {"verbose", no_argument, NULL, 'v'},
                                                 {"help", no_argument, NULL, 'h'},
                                                 {NULL, 0, NULL, 0}};

    int opt;
    optind = 1;
    while ((opt = getopt_long(argc, argv, "+n:vh", long_options, NULL)) != -1) {
        switch (opt) {
        case 'n':
            strncpy(opts->namespace_str, optarg, sizeof(opts->namespace_str) - 1);
            opts->namespace_str[sizeof(opts->namespace_str) - 1] = '\0';
            break;
        case 'v':
            opts->verbose = true;
            break;
        case 'h':
            opts->cmd = CLI_CMD_HELP;
            return true;
        default:
            return false;
        }
    }

    if (optind >= argc) {
        opts->cmd = CLI_CMD_HELP;
        return true;
    }

    const char *cmd_str = argv[optind++];
    if (strcmp(cmd_str, "ping") == 0) {
        opts->cmd = CLI_CMD_PING;
    } else if (strcmp(cmd_str, "query") == 0) {
        opts->cmd = CLI_CMD_QUERY;
    } else if (strcmp(cmd_str, "clear") == 0) {
        opts->cmd = CLI_CMD_CLEAR;
        if (optind < argc) {
            if (!parse_hex_color(argv[optind++], &opts->clear_color)) {
                fprintf(stderr, "Error: Invalid hex color format (expected RRGGBB or RRGGBBAA)\n");
                return false;
            }
        }
    } else if (strcmp(cmd_str, "img") == 0) {
        opts->cmd = CLI_CMD_IMG;
        if (optind >= argc) {
            fprintf(stderr, "Error: 'img' command requires a path to an image file\n");
            return false;
        }
        strncpy(opts->filepath, argv[optind++], sizeof(opts->filepath) - 1);
        opts->filepath[sizeof(opts->filepath) - 1] = '\0';

        /* Parse remaining optional transition flags */
        while (optind < argc) {
            const char *arg = argv[optind++];
            if (strcmp(arg, "--transition-type") == 0 && optind < argc) {
                opts->transition_type = parse_transition_type(argv[optind++]);
            } else if (strcmp(arg, "--transition-duration") == 0 && optind < argc) {
                float dur_s = strtof(argv[optind++], NULL);
                if (dur_s > 0.0f)
                    opts->transition_duration_ms = (uint32_t)(dur_s * 1000.0f);
            } else if (strcmp(arg, "--transition-step") == 0 && optind < argc) {
                int step = atoi(argv[optind++]);
                if (step > 0)
                    opts->transition_duration_ms = (uint32_t)(step * 16);
            } else if (strcmp(arg, "--transition-fps") == 0 && optind < argc) {
                int fps = atoi(argv[optind++]);
                if (fps > 0)
                    opts->transition_fps = (uint32_t)fps;
            } else if (strcmp(arg, "--transition-angle") == 0 && optind < argc) {
                float deg = strtof(argv[optind++], NULL);
                opts->transition_angle_rad = deg * 3.1415926535f / 180.0f;
            } else if (strcmp(arg, "--transition-wave") == 0 && optind < argc) {
                sscanf(argv[optind++], "%f,%f", &opts->transition_wave_freq,
                       &opts->transition_wave_amp);
            } else if (strcmp(arg, "--transition-pos") == 0 && optind < argc) {
                sscanf(argv[optind++], "%f,%f", &opts->transition_pos_x, &opts->transition_pos_y);
            }
        }
    } else if (strcmp(cmd_str, "video") == 0) {
        opts->cmd = CLI_CMD_VIDEO;
        if (optind >= argc) {
            fprintf(stderr, "Error: 'video' command requires a path to a video file\n");
            return false;
        }
        strncpy(opts->filepath, argv[optind++], sizeof(opts->filepath) - 1);
        opts->filepath[sizeof(opts->filepath) - 1] = '\0';

        while (optind < argc) {
            const char *arg = argv[optind++];
            if (strcmp(arg, "--loop") == 0 && optind < argc) {
                opts->video_loop_count = (uint64_t)strtoull(argv[optind++], NULL, 10);
            } else if (strcmp(arg, "--speed") == 0 && optind < argc) {
                opts->video_speed = strtof(argv[optind++], NULL);
            }
        }
    } else if (strcmp(cmd_str, "pause") == 0) {
        opts->cmd = CLI_CMD_PAUSE;
    } else if (strcmp(cmd_str, "unpause") == 0) {
        opts->cmd = CLI_CMD_UNPAUSE;
    } else if (strcmp(cmd_str, "toggle") == 0) {
        opts->cmd = CLI_CMD_TOGGLE;
    } else if (strcmp(cmd_str, "kill") == 0) {
        opts->cmd = CLI_CMD_KILL;
    } else if (strcmp(cmd_str, "help") == 0) {
        opts->cmd = CLI_CMD_HELP;
    } else {
        fprintf(stderr, "Error: Unknown command '%s'\n", cmd_str);
        return false;
    }

    return true;
}
