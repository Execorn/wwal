// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "cli_parser.h"

#include "waywal/ipc_proto.h"

#include <ctype.h>
#include <math.h>
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
    printf("  restore                    Restore last saved wallpaper state\n");
    printf("  clear-cache                Clear cached transcoded media and palettes\n");
    printf(
        "  init                       Launch or initialize wallpaper daemon (swww/awww alias)\n");
    printf("  img <PATH|-> [OPTIONS]     Load and set wallpaper with GPU/SIMD transitions\n");
    printf("  shader <PATH> [OPTIONS]    Run live GLSL fragment shader wallpaper\n");
    printf("  theme [PATH] [OPTIONS]     Extract HCT Material You theme palette\n");
    printf("  stats                      Display daemon telemetry and frame timing statistics\n");
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
    printf("  -d, --interval <SECONDS>   Seconds between wallpaper changes (default: 300)\n");
    printf("  -s, --shuffle, --random    Shuffle images into random playback order\n\n");
    printf("Image & Transition Options:\n");
    printf("  --transition-type <TYPE>   none, simple, fade, wipe, grow, outer, wave, noise,\n");
    printf("                             crosszoom, slide, glitch, burn, ripple, pixelate,\n");
    printf(
        "                             doom, swirl, cube, luma, light_leak, page_curl, custom,\n");
    printf("                             random, left, right, top, bottom, center, any\n");
    printf("  --transition-duration <S>  Duration in seconds (e.g. 1.0, 0.5) (default: 1.0)\n");
    printf("  --transition-step <N>      Transition step speed (swww/awww compatibility)\n");
    printf("  --transition-fps <FPS>     Target frame rate (default: 60)\n");
    printf("  --transition-angle <DEG>   Wipe/wave/slide angle in degrees (default: 0)\n");
    printf("  --transition-wave <F,A>    Wave frequency and amplitude (default: 20,0.05)\n");
    printf("  --transition-pos <POS>     Transition anchor: cursor, center, top, bottom, left,\n");
    printf(
        "                             right, top-left (tl), top-right (tr), bottom-left (bl),\n");
    printf("                             bottom-right (br), or <X,Y> coordinates\n");
    printf("  --transition-bezier <C>    Cubic bezier control points X1,Y1,X2,Y2\n");
    printf("  --fill-color <HEX>         Background fill color for fit/center scaling\n");
    printf("  --resize <MODE>            swww resize mode: crop, fit, no, stretch\n");
    printf(
        "  --no-resize                Center image without scaling (equivalent to --resize no)\n");
    printf("  --filter <FILTER>          Nearest, Bilinear, CatmullRom, Mitchell, Lanczos3\n");
    printf("  --transition-shader <FILE> Path to GLSL compute shader (*.comp) for custom "
           "transitions\n");
    printf("  -o, --output, --outputs <N> Target specific output(s) (comma-separated)\n");
    printf("  --sync-mode <MODE>         Multi-monitor sync: simultaneous or staggered\n");
    printf("  --stagger-delay <MS>       Delay in milliseconds between cascading monitors\n");
    printf("  --10bit                    Enable 10-bit wide-gamut scanout if supported\n");
    printf("  --scaling-mode, --mode <M> Scaling mode: fill, fit, stretch, center, tile\n");
    printf("  --animate                  Animate GIF wallpaper using hardware video engine\n");
    printf("  --theme / --no-theme       Enable or disable automatic theme extraction\n");
    printf("  --theme-mode <dark|light>  Select dark or light Material You palette mode\n");
    printf("  --backdrop <MODE>          Backdrop mode: none, auto-blur, or file path\n");
    printf("  --depth-map <FILE>         Path to 8-bit depth map for 2.5D parallax\n");
    printf("  --depth-strength <FLOAT>   Depth parallax strength multiplier (default: 1.0)\n");
    printf("  --layer <LAYER>            Layer-shell target: background or bottom\n");
    printf("  --battery-policy <POLICY>  Battery policy: ignore, reduce-fps, pause-video\n\n");
    printf("Shader Options:\n");
    printf("  --audio                    Enable PipeWire/PulseAudio FFT audio reactivity\n");
    printf("  --fps <FPS>                Target shader frame rate (default: 60)\n\n");
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
    if (str[0] == '#') {
        str++;
    } else if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        str += 2;
    }

    size_t len = strlen(str);
    if (len != 3 && len != 6 && len != 8) {
        return false;
    }

    for (size_t i = 0; i < len; ++i) {
        if (!isxdigit((unsigned char)str[i])) {
            return false;
        }
    }

    unsigned int r = 0, g = 0, b = 0, a = 255;
    if (len == 3) {
        if (sscanf(str, "%1x%1x%1x", &r, &g, &b) != 3) {
            return false;
        }
        r = (r << 4) | r;
        g = (g << 4) | g;
        b = (b << 4) | b;
    } else if (len == 6) {
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

static void set_angle_degrees(cli_options_t *opts, float deg)
{
    opts->transition_angle = deg;
    opts->transition_angle_rad = deg * 3.1415926535f / 180.0f;
}

static void apply_transition_type(cli_options_t *opts, const char *str)
{
    if (!opts || !str) {
        if (opts)
            opts->transition_type = WAYWAL_TRANSITION_FADE;
        return;
    }
    if (strcasecmp(str, "none") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_NONE;
    } else if (strcasecmp(str, "simple") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_SIMPLE;
    } else if (strcasecmp(str, "fade") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_FADE;
    } else if (strcasecmp(str, "wipe") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_WIPE;
    } else if (strcasecmp(str, "grow") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_GROW;
    } else if (strcasecmp(str, "outer") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_OUTER;
    } else if (strcasecmp(str, "wave") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_WAVE;
    } else if (strcasecmp(str, "noise") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_NOISE;
    } else if (strcasecmp(str, "crosszoom") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_CROSSZOOM;
    } else if (strcasecmp(str, "slide") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_SLIDE;
    } else if (strcasecmp(str, "glitch") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_GLITCH;
    } else if (strcasecmp(str, "burn") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_BURN;
    } else if (strcasecmp(str, "ripple") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_RIPPLE;
    } else if (strcasecmp(str, "pixelate") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_PIXELATE;
    } else if (strcasecmp(str, "doom") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_DOOM;
    } else if (strcasecmp(str, "swirl") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_SWIRL;
    } else if (strcasecmp(str, "cube") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_CUBE;
    } else if (strcasecmp(str, "luma") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_LUMA;
    } else if (strcasecmp(str, "light_leak") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_LIGHT_LEAK;
    } else if (strcasecmp(str, "page_curl") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_PAGE_CURL;
    } else if (strcasecmp(str, "custom") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_CUSTOM;
    } else if (strcasecmp(str, "random") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_RANDOM;
    } else if (strcasecmp(str, "left") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_SLIDE;
        set_angle_degrees(opts, 180.0f);
    } else if (strcasecmp(str, "right") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_SLIDE;
        set_angle_degrees(opts, 0.0f);
    } else if (strcasecmp(str, "top") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_SLIDE;
        set_angle_degrees(opts, 90.0f);
    } else if (strcasecmp(str, "bottom") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_SLIDE;
        set_angle_degrees(opts, 270.0f);
    } else if (strcasecmp(str, "center") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_GROW;
        opts->transition_pos_x = 0.5f;
        opts->transition_pos_y = 0.5f;
    } else if (strcasecmp(str, "any") == 0) {
        opts->transition_type = WAYWAL_TRANSITION_GROW;
    } else {
        opts->transition_type = WAYWAL_TRANSITION_FADE;
    }
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
    if (strcasecmp(str, "top-left") == 0 || strcasecmp(str, "topleft") == 0 ||
        strcasecmp(str, "tl") == 0) {
        *out_x = 0.0f;
        *out_y = 0.0f;
        return true;
    }
    if (strcasecmp(str, "top-right") == 0 || strcasecmp(str, "topright") == 0 ||
        strcasecmp(str, "tr") == 0) {
        *out_x = 1.0f;
        *out_y = 0.0f;
        return true;
    }
    if (strcasecmp(str, "bottom-left") == 0 || strcasecmp(str, "bottomleft") == 0 ||
        strcasecmp(str, "bl") == 0) {
        *out_x = 0.0f;
        *out_y = 1.0f;
        return true;
    }
    if (strcasecmp(str, "bottom-right") == 0 || strcasecmp(str, "bottomright") == 0 ||
        strcasecmp(str, "br") == 0) {
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
    char *saveptr = NULL;
    char *token = strtok_r(tmp, ",", &saveptr);
    while (token != NULL && opts->num_outputs < 16) {
        while (*token == ' ')
            token++;
        size_t len = strlen(token);
        while (len > 0 && token[len - 1] == ' ')
            token[--len] = '\0';
        if (len > 0) {
            strncpy(opts->outputs[opts->num_outputs], token, sizeof(opts->outputs[0]) - 1);
            opts->outputs[opts->num_outputs][sizeof(opts->outputs[0]) - 1] = '\0';
            if (opts->output[0] == '\0') {
                strncpy(opts->output, token, sizeof(opts->output) - 1);
                opts->output[sizeof(opts->output) - 1] = '\0';
            }
            opts->num_outputs++;
        }
        token = strtok_r(NULL, ",", &saveptr);
    }
}

static void apply_scaling_mode(cli_options_t *opts, const char *m)
{
    if (!opts || !m)
        return;
    if (strcasecmp(m, "fill") == 0 || strcasecmp(m, "crop") == 0 || strcasecmp(m, "cover") == 0) {
        opts->scaling_mode = WAYWAL_SCALING_FILL;
    } else if (strcasecmp(m, "fit") == 0 || strcasecmp(m, "contain") == 0) {
        opts->scaling_mode = WAYWAL_SCALING_FIT;
    } else if (strcasecmp(m, "stretch") == 0) {
        opts->scaling_mode = WAYWAL_SCALING_STRETCH;
    } else if (strcasecmp(m, "center") == 0 || strcasecmp(m, "no") == 0) {
        opts->scaling_mode = WAYWAL_SCALING_CENTER;
    } else if (strcasecmp(m, "tile") == 0) {
        opts->scaling_mode = WAYWAL_SCALING_TILE;
    } else if (strcasecmp(m, "span") == 0) {
        opts->scaling_mode = WAYWAL_SCALING_SPAN;
    } else {
        fprintf(stderr, "Warning: Unknown scaling mode '%s', defaulting to fill\n", m);
        opts->scaling_mode = WAYWAL_SCALING_FILL;
    }
}

static void apply_filter_mode(cli_options_t *opts, const char *f)
{
    if (!opts || !f)
        return;
    if (strcasecmp(f, "nearest") == 0) {
        opts->filter = WAYWAL_FILTER_NEAREST;
    } else if (strcasecmp(f, "bilinear") == 0) {
        opts->filter = WAYWAL_FILTER_BILINEAR;
    } else if (strcasecmp(f, "catmullrom") == 0 || strcasecmp(f, "mitchell") == 0 ||
               strcasecmp(f, "lanczos3") == 0 || strcasecmp(f, "lanczos") == 0) {
        opts->filter = WAYWAL_FILTER_LANCZOS3;
    } else {
        opts->filter = WAYWAL_FILTER_LANCZOS3;
    }
}

static bool match_opt_val(const char *arg, const char *opt_name, int *optind_ptr, int argc,
                          char *argv[], const char **out_val)
{
    size_t len = strlen(opt_name);
    if (strcmp(arg, opt_name) == 0) {
        if (*optind_ptr + 1 >= argc)
            return false;
        (*optind_ptr)++;
        *out_val = argv[*optind_ptr];
        return true;
    }
    if (strncmp(arg, opt_name, len) == 0 && arg[len] == '=') {
        *out_val = arg + len + 1;
        return true;
    }
    return false;
}

static bool parse_common_flag(int *optind_ptr, int argc, char *argv[], cli_options_t *opts)
{
    const char *arg = argv[*optind_ptr];
    const char *val = NULL;

    if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0 || strcmp(arg, "-V") == 0 ||
        strcmp(arg, "--version") == 0) {
        opts->cmd = CLI_CMD_HELP;
        return true;
    }
    if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
        opts->verbose = true;
        return true;
    }
    if (strcmp(arg, "-q") == 0 || strcmp(arg, "--quiet") == 0 || strcmp(arg, "--no-daemon") == 0) {
        return true;
    }
    if (match_opt_val(arg, "-n", optind_ptr, argc, argv, &val) ||
        match_opt_val(arg, "--namespace", optind_ptr, argc, argv, &val)) {
        strncpy(opts->namespace_str, val, sizeof(opts->namespace_str) - 1);
        opts->namespace_str[sizeof(opts->namespace_str) - 1] = '\0';
        return true;
    }
    if (match_opt_val(arg, "--format", optind_ptr, argc, argv, &val)) {
        return true;
    }
    if (match_opt_val(arg, "--transition-type", optind_ptr, argc, argv, &val) ||
        match_opt_val(arg, "-t", optind_ptr, argc, argv, &val)) {
        apply_transition_type(opts, val);
        return true;
    }
    if (match_opt_val(arg, "--transition-duration", optind_ptr, argc, argv, &val)) {
        float dur_s = strtof(val, NULL);
        if (dur_s >= 0.0f) {
            opts->transition_duration = dur_s;
            opts->transition_duration_ms = (uint32_t)(dur_s * 1000.0f);
        }
        return true;
    }
    if (match_opt_val(arg, "--transition-step", optind_ptr, argc, argv, &val)) {
        int step = atoi(val);
        if (step > 0) {
            opts->transition_step = (uint32_t)step;
            opts->transition_duration_ms = (uint32_t)(step * 16);
        }
        return true;
    }
    if (match_opt_val(arg, "--transition-fps", optind_ptr, argc, argv, &val) ||
        match_opt_val(arg, "--fps", optind_ptr, argc, argv, &val)) {
        int fps = atoi(val);
        if (fps > 0)
            opts->transition_fps = (uint32_t)fps;
        return true;
    }
    if (match_opt_val(arg, "--transition-angle", optind_ptr, argc, argv, &val)) {
        float deg = strtof(val, NULL);
        set_angle_degrees(opts, deg);
        return true;
    }
    if (match_opt_val(arg, "--transition-wave", optind_ptr, argc, argv, &val)) {
        sscanf(val, "%f,%f", &opts->transition_wave_freq, &opts->transition_wave_amp);
        return true;
    }
    if (match_opt_val(arg, "--transition-pos", optind_ptr, argc, argv, &val)) {
        parse_transition_pos(val, &opts->transition_pos_x, &opts->transition_pos_y);
        return true;
    }
    if (match_opt_val(arg, "--transition-bezier", optind_ptr, argc, argv, &val)) {
        float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
        if (sscanf(val, "%f,%f,%f,%f", &x1, &y1, &x2, &y2) != 4) {
            fprintf(stderr, "Error: Invalid --transition-bezier format (expected X1,Y1,X2,Y2)\n");
            return false;
        }
        opts->bezier = bezier_create(x1, y1, x2, y2);
        opts->has_custom_bezier = true;
        return true;
    }
    if (match_opt_val(arg, "--fill-color", optind_ptr, argc, argv, &val)) {
        if (!parse_hex_color(val, &opts->clear_color)) {
            fprintf(stderr,
                    "Error: Invalid --fill-color hex format (expected RRGGBB or RRGGBBAA)\n");
            return false;
        }
        return true;
    }
    if (match_opt_val(arg, "--transition-shader", optind_ptr, argc, argv, &val)) {
        strncpy(opts->custom_shader_path, val, sizeof(opts->custom_shader_path) - 1);
        opts->custom_shader_path[sizeof(opts->custom_shader_path) - 1] = '\0';
        opts->transition_type = WAYWAL_TRANSITION_CUSTOM;
        return true;
    }
    if (match_opt_val(arg, "--depth-map", optind_ptr, argc, argv, &val)) {
        if (!val || val[0] == '\0') {
            return false;
        }
        strncpy(opts->depth_map_path, val, sizeof(opts->depth_map_path) - 1);
        opts->depth_map_path[sizeof(opts->depth_map_path) - 1] = '\0';
        return true;
    }
    if (match_opt_val(arg, "-o", optind_ptr, argc, argv, &val) ||
        match_opt_val(arg, "--output", optind_ptr, argc, argv, &val) ||
        match_opt_val(arg, "--outputs", optind_ptr, argc, argv, &val)) {
        add_output_target(opts, val);
        return true;
    }
    if (match_opt_val(arg, "--sync-mode", optind_ptr, argc, argv, &val)) {
        if (strcmp(val, "staggered") == 0 || strcmp(val, "cascade") == 0) {
            opts->sync_mode = 1;
        } else {
            opts->sync_mode = 0;
        }
        return true;
    }
    if (match_opt_val(arg, "--stagger-delay", optind_ptr, argc, argv, &val)) {
        int d = atoi(val);
        if (d >= 0)
            opts->stagger_delay_ms = (uint32_t)d;
        return true;
    }
    if (strcmp(arg, "--10bit") == 0) {
        opts->enable_10bit = true;
        return true;
    }
    if (strcmp(arg, "--no-resize") == 0) {
        opts->scaling = WAYWAL_SCALING_CENTER;
        return true;
    }
    if (match_opt_val(arg, "--resize", optind_ptr, argc, argv, &val) ||
        match_opt_val(arg, "--scaling-mode", optind_ptr, argc, argv, &val)) {
        apply_scaling_mode(opts, val);
        return true;
    }
    if (match_opt_val(arg, "--mode", optind_ptr, argc, argv, &val)) {
        if (opts->cmd == CLI_CMD_THEME || strcasecmp(val, "light") == 0 ||
            strcasecmp(val, "dark") == 0) {
            opts->theme_light_mode = (strcasecmp(val, "light") == 0);
        } else {
            apply_scaling_mode(opts, val);
        }
        return true;
    }
    if (match_opt_val(arg, "--filter", optind_ptr, argc, argv, &val) ||
        match_opt_val(arg, "-f", optind_ptr, argc, argv, &val)) {
        apply_filter_mode(opts, val);
        return true;
    }
    if (strcmp(arg, "--animate") == 0) {
        opts->animate = true;
        return true;
    }
    if (strcmp(arg, "--audio") == 0) {
        opts->audio_reactive = true;
        return true;
    }
    if (strcmp(arg, "--theme") == 0) {
        opts->extract_theme = true;
        opts->no_theme = false;
        return true;
    }
    if (strcmp(arg, "--no-theme") == 0) {
        opts->no_theme = true;
        opts->extract_theme = false;
        return true;
    }
    if (strcmp(arg, "--light") == 0) {
        opts->theme_light_mode = true;
        return true;
    }
    if (strcmp(arg, "--dark") == 0) {
        opts->theme_light_mode = false;
        return true;
    }
    if (match_opt_val(arg, "--theme-mode", optind_ptr, argc, argv, &val)) {
        opts->theme_light_mode = (strcasecmp(val, "light") == 0);
        return true;
    }
    if (match_opt_val(arg, "--backdrop", optind_ptr, argc, argv, &val)) {
        strncpy(opts->backdrop, val, sizeof(opts->backdrop) - 1);
        opts->backdrop[sizeof(opts->backdrop) - 1] = '\0';
        return true;
    }
    if (match_opt_val(arg, "--layer", optind_ptr, argc, argv, &val)) {
        strncpy(opts->layer_name, val, sizeof(opts->layer_name) - 1);
        opts->layer_name[sizeof(opts->layer_name) - 1] = '\0';
        return true;
    }
    if (match_opt_val(arg, "--battery-policy", optind_ptr, argc, argv, &val)) {
        strncpy(opts->battery_policy, val, sizeof(opts->battery_policy) - 1);
        opts->battery_policy[sizeof(opts->battery_policy) - 1] = '\0';
        return true;
    }
    if (match_opt_val(arg, "--depth-strength", optind_ptr, argc, argv, &val)) {
        if (!val || val[0] == '\0') {
            fprintf(stderr, "Error: Invalid --depth-strength value\n");
            return false;
        }
        char *endptr = NULL;
        float strength = strtof(val, &endptr);
        if (endptr == val || *endptr != '\0' || !isfinite(strength) || strength <= 0.0f) {
            fprintf(stderr, "Error: Invalid --depth-strength value\n");
            return false;
        }
        opts->depth_strength = strength;
        return true;
    }

    return false;
}

bool cli_parse(int argc, char *argv[], cli_options_t *opts)
{
    if (!opts || argc <= 0 || !argv)
        return false;

    memset(opts, 0, sizeof(*opts));
    strncpy(opts->namespace_str, "default", sizeof(opts->namespace_str) - 1);
    opts->clear_color = (color_rgba_t){.r = 0, .g = 0, .b = 0, .a = 255};
    opts->transition_type = WAYWAL_TRANSITION_FADE;
    opts->transition_duration = 1.0f;
    opts->transition_duration_ms = 1000;
    opts->transition_fps = 60;
    opts->transition_step = 0;
    opts->transition_angle_rad = 0.0f;
    opts->transition_angle = 0.0f;
    opts->transition_wave_freq = 0.0f;
    opts->transition_wave_amp = 0.0f;
    opts->transition_pos_x = 0.5f;
    opts->transition_pos_y = 0.5f;
    opts->bezier = BEZIER_DEFAULT;
    opts->has_custom_bezier = false;
    opts->depth_strength = 1.0f;
    opts->sync_mode = 0;
    opts->stagger_delay_ms = 150;
    opts->scaling_mode = WAYWAL_SCALING_FILL;
    opts->filter = WAYWAL_FILTER_LANCZOS3;
    opts->video_loop_count = 0;
    opts->video_speed = 1.0f;
    opts->slideshow_interval_s = 300;
    opts->slideshow_random = false;

    const char *prog = argv[0] ? argv[0] : "wwal";
    const char *base = strrchr(prog, '/');
    base = base ? (base + 1) : prog;

    if (strcmp(base, "swww") == 0 || strcmp(base, "awww") == 0 ||
        strcmp(base, "swww-daemon") == 0 || strcmp(base, "awww-daemon") == 0) {
        opts->is_swww_compat = true;
    }

    if (strcmp(base, "swww-daemon") == 0 || strcmp(base, "awww-daemon") == 0) {
        opts->cmd = CLI_CMD_DAEMON_ALIAS;
        for (int idx = 1; idx < argc; ++idx) {
            if (!parse_common_flag(&idx, argc, argv, opts)) {
                return false;
            }
            if (opts->cmd == CLI_CMD_HELP)
                return true;
        }
        return true;
    }

    int optind = 1;
    while (optind < argc && argv[optind][0] == '-') {
        if (!parse_common_flag(&optind, argc, argv, opts)) {
            return false;
        }
        if (opts->cmd == CLI_CMD_HELP)
            return true;
        optind++;
    }

    if (optind >= argc) {
        opts->cmd = CLI_CMD_HELP;
        return true;
    }

    const char *cmd_str = argv[optind++];
    if (strcmp(cmd_str, "ping") == 0) {
        opts->cmd = CLI_CMD_PING;
        while (optind < argc) {
            if (!parse_common_flag(&optind, argc, argv, opts))
                return false;
            optind++;
        }
    } else if (strcmp(cmd_str, "query") == 0) {
        opts->cmd = CLI_CMD_QUERY;
        while (optind < argc) {
            if (!parse_common_flag(&optind, argc, argv, opts))
                return false;
            optind++;
        }
    } else if (strcmp(cmd_str, "restore") == 0) {
        opts->cmd = CLI_CMD_RESTORE;
        while (optind < argc) {
            if (!parse_common_flag(&optind, argc, argv, opts))
                return false;
            optind++;
        }
    } else if (strcmp(cmd_str, "clear-cache") == 0) {
        opts->cmd = CLI_CMD_CLEAR_CACHE;
        while (optind < argc) {
            if (!parse_common_flag(&optind, argc, argv, opts))
                return false;
            optind++;
        }
    } else if (strcmp(cmd_str, "stats") == 0) {
        opts->cmd = CLI_CMD_STATS;
        while (optind < argc) {
            if (!parse_common_flag(&optind, argc, argv, opts))
                return false;
            optind++;
        }
    } else if (strcmp(cmd_str, "init") == 0) {
        opts->cmd = CLI_CMD_DAEMON_ALIAS;
        while (optind < argc) {
            if (!parse_common_flag(&optind, argc, argv, opts))
                return false;
            optind++;
        }
    } else if (strcmp(cmd_str, "clear") == 0) {
        opts->cmd = CLI_CMD_COLOR;
        while (optind < argc) {
            const char *arg = argv[optind];
            if (arg[0] == '-') {
                if (!parse_common_flag(&optind, argc, argv, opts)) {
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
        bool has_path = false;
        while (optind < argc) {
            const char *arg = argv[optind];
            if (strcmp(arg, "-") == 0 && !has_path) {
                strncpy(opts->filepath, "-", sizeof(opts->filepath) - 1);
                opts->filepath[sizeof(opts->filepath) - 1] = '\0';
                opts->read_stdin = true;
                has_path = true;
            } else if (arg[0] == '-') {
                if (!parse_common_flag(&optind, argc, argv, opts)) {
                    fprintf(stderr, "Error: Unknown option '%s'\n", arg);
                    return false;
                }
            } else if (!has_path) {
                strncpy(opts->filepath, arg, sizeof(opts->filepath) - 1);
                opts->filepath[sizeof(opts->filepath) - 1] = '\0';
                has_path = true;
            } else {
                fprintf(stderr, "Error: Unexpected argument '%s'\n", arg);
                return false;
            }
            optind++;
        }
        if (!has_path) {
            fprintf(stderr, "Error: 'img' command requires a path to an image file\n");
            return false;
        }
    } else if (strcmp(cmd_str, "shader") == 0) {
        opts->cmd = CLI_CMD_SHADER;
        bool has_path = false;
        while (optind < argc) {
            const char *arg = argv[optind];
            if (arg[0] == '-') {
                if (!parse_common_flag(&optind, argc, argv, opts)) {
                    fprintf(stderr, "Error: Unknown shader option '%s'\n", arg);
                    return false;
                }
            } else if (!has_path) {
                strncpy(opts->filepath, arg, sizeof(opts->filepath) - 1);
                opts->filepath[sizeof(opts->filepath) - 1] = '\0';
                strncpy(opts->custom_shader_path, arg, sizeof(opts->custom_shader_path) - 1);
                opts->custom_shader_path[sizeof(opts->custom_shader_path) - 1] = '\0';
                has_path = true;
            } else {
                fprintf(stderr, "Error: Unexpected argument '%s'\n", arg);
                return false;
            }
            optind++;
        }
        if (!has_path) {
            fprintf(stderr, "Error: 'shader' command requires a path to a shader file\n");
            return false;
        }
    } else if (strcmp(cmd_str, "theme") == 0) {
        opts->cmd = CLI_CMD_THEME;
        opts->extract_theme = true;
        while (optind < argc) {
            const char *arg = argv[optind];
            if (arg[0] == '-') {
                if (!parse_common_flag(&optind, argc, argv, opts)) {
                    fprintf(stderr, "Error: Unknown theme option '%s'\n", arg);
                    return false;
                }
            } else if (opts->filepath[0] == '\0') {
                strncpy(opts->filepath, arg, sizeof(opts->filepath) - 1);
                opts->filepath[sizeof(opts->filepath) - 1] = '\0';
            } else {
                fprintf(stderr, "Error: Unexpected argument '%s'\n", arg);
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
                const char *val = NULL;
                if (match_opt_val(arg, "--interval", &optind, argc, argv, &val) ||
                    match_opt_val(arg, "-d", &optind, argc, argv, &val)) {
                    int sec = atoi(val);
                    if (sec > 0)
                        opts->slideshow_interval_s = (uint32_t)sec;
                } else if (strcmp(arg, "--shuffle") == 0 || strcmp(arg, "--random") == 0 ||
                           strcmp(arg, "-s") == 0) {
                    opts->slideshow_random = true;
                } else if (!parse_common_flag(&optind, argc, argv, opts)) {
                    fprintf(stderr, "Error: Unknown option '%s'\n", arg);
                    return false;
                }
                optind++;
            }
        }
    } else if (strcmp(cmd_str, "video") == 0) {
        opts->cmd = CLI_CMD_VIDEO;
        while (optind < argc) {
            const char *arg = argv[optind];
            const char *val = NULL;
            if (match_opt_val(arg, "--loop", &optind, argc, argv, &val)) {
                opts->video_loop_count = (uint64_t)strtoull(val, NULL, 10);
            } else if (match_opt_val(arg, "--speed", &optind, argc, argv, &val)) {
                opts->video_speed = strtof(val, NULL);
            } else if (arg[0] == '-') {
                if (!parse_common_flag(&optind, argc, argv, opts)) {
                    fprintf(stderr, "Error: Unknown video option '%s'\n", arg);
                    return false;
                }
            } else if (opts->filepath[0] == '\0') {
                strncpy(opts->filepath, arg, sizeof(opts->filepath) - 1);
                opts->filepath[sizeof(opts->filepath) - 1] = '\0';
            } else {
                fprintf(stderr, "Error: Unknown video option '%s'\n", arg);
                return false;
            }
            optind++;
        }
        if (opts->filepath[0] == '\0') {
            fprintf(stderr, "Error: 'video' command requires a path to a video file\n");
            return false;
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

bool waywal_cli_parse(int argc, char *argv[], waywal_cli_opts_t *opts)
{
    return cli_parse(argc, argv, opts);
}
