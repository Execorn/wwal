#include "cli_parser.h"

#include "waywal/ipc_proto.h"

#include <ctype.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void cli_print_usage(const char *prog)
{
    printf("wwal - Modern high-performance Wayland wallpaper client (C23)\n");
    printf("Usage: %s [GLOBAL_OPTIONS] <COMMAND> [ARGS...]\n\n", prog);
    printf("Commands:\n");
    printf("  ping                       Check if wwald daemon is running and responsive\n");
    printf("  query                      List all detected Wayland outputs and current geometry\n");
    printf("  clear [COLOR] [OPTIONS]    Clear wallpaper to solid hex color (e.g. 000000 or "
           "00ff00)\n");
    printf("  img <PATH> [OPTIONS]       Load and set wallpaper with GPU/SIMD transitions\n");
    printf("  slideshow <DIR> [OPTIONS]  Start zero-idle-CPU timerfd wallpaper slideshow\n");
    printf("  slideshow stop             Stop active wallpaper slideshow\n");
    printf("  slideshow pause            Pause active slideshow\n");
    printf("  slideshow resume           Resume paused slideshow\n");
    printf("  slideshow toggle           Toggle pause/resume state\n");
    printf("  slideshow next             Advance slideshow to next image immediately\n");
    printf("  slideshow prev             Step slideshow to previous image\n");
    printf("  video <PATH> [OPTIONS]     Play hardware-accelerated video wallpaper (VA-API)\n");
    printf("  pause                      Pause video playback\n");
    printf("  unpause                    Resume video playback\n");
    printf("  toggle                     Toggle video playback pause/resume\n");
    printf("  kill                       Gracefully terminate the running wwald daemon\n");
    printf("  help                       Display this help message\n\n");
    printf("Slideshow Options:\n");
    printf("  --interval <SECONDS>       Seconds between wallpaper changes (default: 300)\n");
    printf("  --shuffle, --random        Shuffle images into random playback order\n\n");
    printf("Image & Transition Options:\n");
    printf("  --transition-type <TYPE>   none, simple, fade, wipe, grow, outer, wave, noise,\n");
    printf("                             crosszoom, slide, glitch, burn, ripple, pixelate,\n");
    printf("                             doom, swirl, cube, luma, light_leak, page_curl, custom, "
           "random (default: fade)\n");
    printf("  --transition-duration <S>  Duration in seconds (e.g. 1.0, 0.5) (default: 1.0)\n");
    printf("  --transition-fps <FPS>     Target frame rate (default: 60)\n");
    printf("  --transition-angle <DEG>   Wipe/wave/slide angle in degrees (default: 0)\n");
    printf("  --transition-wave <F,A>    Wave frequency and amplitude (default: 20,0.05)\n");
    printf("  --transition-pos <POS>     Transition anchor: cursor, center, top, bottom, left,\n");
    printf("                             right, top-left, top-right, bottom-left, bottom-right,\n");
    printf("                             or <X,Y> coordinates 0.0-1.0 (default: center)\n");
    printf("  --transition-shader <FILE> Path to GLSL compute shader (*.comp) for custom "
           "transitions\n");
    printf("  -o, --output <NAME>        Target specific output(s) (can be specified multiple "
           "times or comma-separated)\n");
    printf("  --sync-mode <MODE>         Multi-monitor sync: simultaneous or staggered (default: "
           "simultaneous)\n");
    printf("  --stagger-delay <MS>       Delay in milliseconds between cascading monitors "
           "(default: 150)\n");
    printf("  --10bit                    Enable 10-bit wide-gamut scanout if supported by "
           "compositor\n");
    printf("  --scaling-mode, --mode <M> Scaling mode: fill, fit, stretch, center, tile "
           "(default: fill)\n\n");
    printf("Video Options:\n");
    printf("  --loop <COUNT>             Loop count (0 = infinite) (default: 0)\n");
    printf("  --speed <FLOAT>            Playback speed multiplier (default: 1.0)\n\n");
    printf("Global Options:\n");
    printf("  -n, --namespace <NAME>     Socket namespace (default: \"default\")\n");
    printf("  -v, --verbose              Enable verbose debug output\n");
    printf("  -h, --help                 Print help information\n");
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
    if (strcmp(str, "custom") == 0)
        return 20;
    if (strcmp(str, "random") == 0)
        return 21;
    return 2;
}

static bool parse_transition_pos(const char *str, float *out_x, float *out_y)
{
    if (!str || !out_x || !out_y)
        return false;

    if (strcasecmp(str, "cursor") == 0 || strcasecmp(str, "mouse") == 0) {
        *out_x = -1.0f;
        *out_y = -1.0f;
        return true;
    }
    if (strcasecmp(str, "center") == 0 || strcasecmp(str, "middle") == 0) {
        *out_x = 0.5f;
        *out_y = 0.5f;
        return true;
    }
    if (strcasecmp(str, "top") == 0) {
        *out_x = 0.5f;
        *out_y = 0.0f;
        return true;
    }
    if (strcasecmp(str, "bottom") == 0) {
        *out_x = 0.5f;
        *out_y = 1.0f;
        return true;
    }
    if (strcasecmp(str, "left") == 0) {
        *out_x = 0.0f;
        *out_y = 0.5f;
        return true;
    }
    if (strcasecmp(str, "right") == 0) {
        *out_x = 1.0f;
        *out_y = 0.5f;
        return true;
    }
    if (strcasecmp(str, "top-left") == 0 || strcasecmp(str, "topleft") == 0) {
        *out_x = 0.0f;
        *out_y = 0.0f;
        return true;
    }
    if (strcasecmp(str, "top-right") == 0 || strcasecmp(str, "topright") == 0) {
        *out_x = 1.0f;
        *out_y = 0.0f;
        return true;
    }
    if (strcasecmp(str, "bottom-left") == 0 || strcasecmp(str, "bottomleft") == 0) {
        *out_x = 0.0f;
        *out_y = 1.0f;
        return true;
    }
    if (strcasecmp(str, "bottom-right") == 0 || strcasecmp(str, "bottomright") == 0) {
        *out_x = 1.0f;
        *out_y = 1.0f;
        return true;
    }

    return (sscanf(str, "%f,%f", out_x, out_y) == 2);
}

static void add_output_target(cli_options_t *opts, const char *arg)
{
    if (!opts || !arg || !arg[0])
        return;
    char tmp[256];
    strncpy(tmp, arg, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *token = strtok(tmp, ",");
    while (token != NULL && opts->num_outputs < 16) {
        while (*token == ' ')
            token++;
        size_t len = strlen(token);
        while (len > 0 && token[len - 1] == ' ')
            token[--len] = '\0';
        if (len > 0) {
            strncpy(opts->outputs[opts->num_outputs], token, sizeof(opts->outputs[0]) - 1);
            opts->outputs[opts->num_outputs][sizeof(opts->outputs[0]) - 1] = '\0';
            opts->num_outputs++;
        }
        token = strtok(NULL, ",");
    }
}

static bool parse_transition_flag(int *optind_ptr, int argc, char *argv[], cli_options_t *opts)
{
    const char *arg = argv[*optind_ptr];
    if (strcmp(arg, "--transition-type") == 0 && *optind_ptr + 1 < argc) {
        (*optind_ptr)++;
        opts->transition_type = parse_transition_type(argv[*optind_ptr]);
        return true;
    }
    if (strcmp(arg, "--transition-duration") == 0 && *optind_ptr + 1 < argc) {
        (*optind_ptr)++;
        float dur_s = strtof(argv[*optind_ptr], NULL);
        if (dur_s > 0.0f)
            opts->transition_duration_ms = (uint32_t)(dur_s * 1000.0f);
        return true;
    }
    if (strcmp(arg, "--transition-step") == 0 && *optind_ptr + 1 < argc) {
        (*optind_ptr)++;
        int step = atoi(argv[*optind_ptr]);
        if (step > 0)
            opts->transition_duration_ms = (uint32_t)(step * 16);
        return true;
    }
    if (strcmp(arg, "--transition-fps") == 0 && *optind_ptr + 1 < argc) {
        (*optind_ptr)++;
        int fps = atoi(argv[*optind_ptr]);
        if (fps > 0)
            opts->transition_fps = (uint32_t)fps;
        return true;
    }
    if (strcmp(arg, "--transition-angle") == 0 && *optind_ptr + 1 < argc) {
        (*optind_ptr)++;
        float deg = strtof(argv[*optind_ptr], NULL);
        opts->transition_angle_rad = deg * 3.1415926535f / 180.0f;
        return true;
    }
    if (strcmp(arg, "--transition-wave") == 0 && *optind_ptr + 1 < argc) {
        (*optind_ptr)++;
        sscanf(argv[*optind_ptr], "%f,%f", &opts->transition_wave_freq, &opts->transition_wave_amp);
        return true;
    }
    if (strcmp(arg, "--transition-pos") == 0 && *optind_ptr + 1 < argc) {
        (*optind_ptr)++;
        parse_transition_pos(argv[*optind_ptr], &opts->transition_pos_x, &opts->transition_pos_y);
        return true;
    }
    if (strcmp(arg, "--transition-shader") == 0 && *optind_ptr + 1 < argc) {
        (*optind_ptr)++;
        strncpy(opts->custom_shader_path, argv[*optind_ptr], sizeof(opts->custom_shader_path) - 1);
        opts->custom_shader_path[sizeof(opts->custom_shader_path) - 1] = '\0';
        opts->transition_type = 20; /* WAYWAL_TRANSITION_CUSTOM */
        return true;
    }
    if ((strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0) && *optind_ptr + 1 < argc) {
        (*optind_ptr)++;
        add_output_target(opts, argv[*optind_ptr]);
        return true;
    }
    if (strcmp(arg, "--sync-mode") == 0 && *optind_ptr + 1 < argc) {
        (*optind_ptr)++;
        const char *mode = argv[*optind_ptr];
        if (strcmp(mode, "staggered") == 0 || strcmp(mode, "cascade") == 0) {
            opts->sync_mode = 1;
        } else {
            opts->sync_mode = 0;
        }
        return true;
    }
    if (strcmp(arg, "--stagger-delay") == 0 && *optind_ptr + 1 < argc) {
        (*optind_ptr)++;
        int d = atoi(argv[*optind_ptr]);
        if (d >= 0)
            opts->stagger_delay_ms = (uint32_t)d;
        return true;
    }
    if (strcmp(arg, "--10bit") == 0) {
        opts->enable_10bit = true;
        return true;
    }
    if ((strcmp(arg, "--scaling-mode") == 0 || strcmp(arg, "--mode") == 0) &&
        *optind_ptr + 1 < argc) {
        (*optind_ptr)++;
        const char *m = argv[*optind_ptr];
        if (strcasecmp(m, "fill") == 0 || strcasecmp(m, "crop") == 0 ||
            strcasecmp(m, "cover") == 0) {
            opts->scaling_mode = WAYWAL_SCALING_FILL;
        } else if (strcasecmp(m, "fit") == 0 || strcasecmp(m, "contain") == 0) {
            opts->scaling_mode = WAYWAL_SCALING_FIT;
        } else if (strcasecmp(m, "stretch") == 0) {
            opts->scaling_mode = WAYWAL_SCALING_STRETCH;
        } else if (strcasecmp(m, "center") == 0) {
            opts->scaling_mode = WAYWAL_SCALING_CENTER;
        } else if (strcasecmp(m, "tile") == 0) {
            opts->scaling_mode = WAYWAL_SCALING_TILE;
        } else {
            fprintf(stderr, "Warning: Unknown scaling mode '%s', defaulting to fill\n", m);
            opts->scaling_mode = WAYWAL_SCALING_FILL;
        }
        return true;
    }

    return false;
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
    opts->sync_mode = 0;
    opts->stagger_delay_ms = 150;
    opts->scaling_mode = WAYWAL_SCALING_FILL; /* 0 = Fill / Cover */
    opts->video_loop_count = 0;
    opts->video_speed = 1.0f;
    opts->slideshow_interval_s = 300;
    opts->slideshow_random = false;

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
        while (optind < argc) {
            const char *arg = argv[optind];
            if (arg[0] == '-') {
                if (!parse_transition_flag(&optind, argc, argv, opts)) {
                    fprintf(stderr, "Error: Unknown option '%s'\n", arg);
                    return false;
                }
            } else {
                if (!parse_hex_color(arg, &opts->clear_color)) {
                    fprintf(stderr,
                            "Error: Invalid hex color format (expected RRGGBB or RRGGBBAA)\n");
                    return false;
                }
            }
            optind++;
        }
    } else if (strcmp(cmd_str, "img") == 0) {
        opts->cmd = CLI_CMD_IMG;
        if (optind >= argc) {
            fprintf(stderr, "Error: 'img' command requires a path to an image file\n");
            return false;
        }
        strncpy(opts->filepath, argv[optind++], sizeof(opts->filepath) - 1);
        opts->filepath[sizeof(opts->filepath) - 1] = '\0';

        while (optind < argc) {
            if (!parse_transition_flag(&optind, argc, argv, opts)) {
                fprintf(stderr, "Error: Unknown option '%s'\n", argv[optind]);
                return false;
            }
            optind++;
        }
    } else if (strcmp(cmd_str, "slideshow") == 0) {
        if (optind >= argc) {
            fprintf(stderr, "Error: 'slideshow' requires a subcommand or directory path\n");
            return false;
        }
        const char *sub = argv[optind];
        if (strcmp(sub, "stop") == 0) {
            opts->cmd = CLI_CMD_SLIDESHOW_CTRL;
            opts->slideshow_action = WAYWAL_SLIDESHOW_STOP;
            optind++;
        } else if (strcmp(sub, "pause") == 0) {
            opts->cmd = CLI_CMD_SLIDESHOW_CTRL;
            opts->slideshow_action = WAYWAL_SLIDESHOW_PAUSE;
            optind++;
        } else if (strcmp(sub, "resume") == 0 || strcmp(sub, "unpause") == 0) {
            opts->cmd = CLI_CMD_SLIDESHOW_CTRL;
            opts->slideshow_action = WAYWAL_SLIDESHOW_RESUME;
            optind++;
        } else if (strcmp(sub, "toggle") == 0) {
            opts->cmd = CLI_CMD_SLIDESHOW_CTRL;
            opts->slideshow_action = WAYWAL_SLIDESHOW_TOGGLE;
            optind++;
        } else if (strcmp(sub, "next") == 0) {
            opts->cmd = CLI_CMD_SLIDESHOW_CTRL;
            opts->slideshow_action = WAYWAL_SLIDESHOW_NEXT;
            optind++;
        } else if (strcmp(sub, "prev") == 0) {
            opts->cmd = CLI_CMD_SLIDESHOW_CTRL;
            opts->slideshow_action = WAYWAL_SLIDESHOW_PREV;
            optind++;
        } else {
            /* Start slideshow */
            opts->cmd = CLI_CMD_SLIDESHOW;
            if (strcmp(sub, "start") == 0) {
                optind++;
                if (optind >= argc) {
                    fprintf(stderr, "Error: 'slideshow start' requires a directory path\n");
                    return false;
                }
            }
            strncpy(opts->filepath, argv[optind++], sizeof(opts->filepath) - 1);
            opts->filepath[sizeof(opts->filepath) - 1] = '\0';

            while (optind < argc) {
                const char *arg = argv[optind];
                if ((strcmp(arg, "--interval") == 0 || strcmp(arg, "-d") == 0) &&
                    optind + 1 < argc) {
                    optind++;
                    int sec = atoi(argv[optind]);
                    if (sec > 0)
                        opts->slideshow_interval_s = (uint32_t)sec;
                } else if (strcmp(arg, "--shuffle") == 0 || strcmp(arg, "--random") == 0 ||
                           strcmp(arg, "-s") == 0) {
                    opts->slideshow_random = true;
                } else if (!parse_transition_flag(&optind, argc, argv, opts)) {
                    fprintf(stderr, "Error: Unknown option '%s'\n", arg);
                    return false;
                }
                optind++;
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
            const char *arg = argv[optind];
            if (strcmp(arg, "--loop") == 0 && optind + 1 < argc) {
                optind++;
                opts->video_loop_count = (uint64_t)strtoull(argv[optind], NULL, 10);
            } else if (strcmp(arg, "--speed") == 0 && optind + 1 < argc) {
                optind++;
                opts->video_speed = strtof(argv[optind], NULL);
            } else if ((strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0) &&
                       optind + 1 < argc) {
                optind++;
                add_output_target(opts, argv[optind]);
            } else {
                fprintf(stderr, "Error: Unknown video option '%s'\n", arg);
                return false;
            }
            optind++;
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
