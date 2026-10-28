// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "cli_parser.h"

#include "waywal/ipc_proto.h"
#include "waywal/path.h"
#include "waywal/toml_parser.h"
#include "waywal/we_scene.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

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
    printf("  pick [OPTIONS]             Launch unified icon-grid wallpaper/Workshop/shader "
           "picker\n");
    printf("  shader <PATH|PRESET>       Run live GLSL fragment shader or built-in preset\n");
    printf("  shader list [--json]       List built-in GLSL shader presets\n");
    printf("  shader preset <NAME>       Run built-in GLSL shader preset by name\n");
    printf("  workshop list              List installed Steam Workshop (431960) wallpapers\n");
    printf(
        "  workshop info <ID|PATH>    Inspect Workshop project.json, scene.pkg, and properties\n");
    printf("  workshop sync-previews [D] Sync Workshop preview symlinks for external GUI pickers\n");
    printf("  workshop [apply] <ID|PATH> Render native Wallpaper Engine scene or video\n");
    printf("  theme [PATH] [OPTIONS]     Extract HCT Material You theme palette\n");
    printf("  stats [--json]             Display daemon telemetry and frame timing statistics\n");
    printf("  status [--json]            Inspect outputs, active media, MPRIS, and telemetry\n");
    printf("  set-uniform <NAME> <V...>  Mutate live shader uniform or Workshop property\n");
    printf("  subscribe                  Stream newline-delimited JSON state change events\n");
    printf("  audio [OPTIONS]            Update live audio volume and auto-mute ducking\n");
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
    printf("  --invert-y                 Invert Y coordinate of --transition-pos\n");
    printf("  --transition-bezier <C>    Cubic bezier control points X1,Y1,X2,Y2\n");
    printf("  --fill-color <HEX>         Background fill color for fit/center scaling\n");
    printf("  --resize <MODE>            swww resize mode: crop, fit, no, stretch\n");
    printf(
        "  --no-resize                Center image without scaling (equivalent to --resize no)\n");
    printf("  --crop-gravity <GRAVITY>   Crop anchor for fill/crop: center, top, bottom, left,\n");
    printf("                             right, top-left, top-right, bottom-left, bottom-right,\n");
    printf("                             or <X,Y> normalized/percentage coordinates\n");
    printf("  --filter <FILTER>          Nearest, Bilinear, CatmullRom, Mitchell, Lanczos3\n");
    printf("  --no-cache                 Bypass image/frame cache for this operation\n");
    printf("  --clear-cache              Clear cached media before applying\n");
    printf("  --transition-shader <FILE> Path to GLSL compute shader (*.comp) for custom "
           "transitions\n");
    printf("  -o, --output, --outputs <N> Target specific output(s) (comma-separated)\n");
    printf("  --sync-mode <MODE>         Multi-monitor sync: simultaneous or staggered\n");
    printf("  --stagger-delay <MS>       Delay in milliseconds between cascading monitors\n");
    printf("  --10bit                    Enable 10-bit wide-gamut scanout if supported\n");
    printf("  --scaling-mode, --mode <M> Scaling mode: fill, fit, stretch, center, tile, span\n");
    printf("  --animate                  Animate GIF wallpaper using hardware video engine\n");
    printf("  --theme / --no-theme       Enable or disable automatic theme extraction\n");
    printf("  --theme-from-media         Extract theme dynamically from MPRIS2 album art\n");
    printf("  --no-theme-from-media      Disable MPRIS2 album art theme extraction\n");
    printf("  --theme-mode <dark|light>  Select dark or light Material You palette mode\n");
    printf("  --light / --dark           Shorthand for --theme-mode light / dark\n");
    printf("  --source-color-index <0..3> Select ranked HCT seed color index (default: 0)\n");
    printf("  --seed-color <HEX>         Override extracted seed with custom hex color\n");
    printf("  --backdrop <MODE>          Backdrop mode: none, auto-blur, or file path\n");
    printf("  --depth-map <FILE>         Path to 8-bit depth map for 2.5D parallax\n");
    printf("  --depth-strength <FLOAT>   Depth parallax strength multiplier (default: 1.0)\n");
    printf("  --layer <LAYER>            Layer-shell target: background or bottom\n");
    printf("  --battery-policy <POLICY>  Battery policy: ignore, reduce-fps, pause-video\n\n");
    printf("Workshop, Shader & Audio Options:\n");
    printf("  -p, --prop <KEY=VAL>       Override Wallpaper Engine Workshop user property\n");
    printf("  --audio                    Enable PipeWire/PulseAudio FFT audio reactivity\n");
    printf("  --volume <0.0..1.0>        Wallpaper audio volume (default: 0.0)\n");
    printf("  --mute-on-audio            Auto-duck volume when another app plays audio\n");
    printf("  --no-mute-on-audio         Disable automatic audio ducking\n");
    printf("  --fps <FPS>                Target shader frame rate (default: 60)\n\n");
    printf("Video Options:\n");
    printf("  --loop <COUNT>             Loop count (0 = infinite) (default: 0)\n");
    printf("  --speed <FLOAT>            Playback speed multiplier (default: 1.0)\n\n");
    printf("Global Options:\n");
    printf("  -a, --all                  Send command to all active daemon namespaces\n");
    printf("  -j, --json                 Format query/status/stats output as JSON\n");
    printf("  -n, --namespace <NAME>     Socket namespace (default: \"default\")\n");
    printf("  -v, --verbose              Enable verbose debug output\n");
    printf("  -V, --version              Print version information\n");
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

    while (*str && isspace((unsigned char)*str)) {
        str++;
    }
    if (*str == '\0') {
        return false;
    }

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

    const char *comma = strchr(str, ',');
    if (!comma) {
        return false;
    }
    char *end1 = NULL;
    float px = strtof(str, &end1);
    if (!end1 || end1 == str || !isfinite(px) || px < 0.0f) {
        return false;
    }
    bool pct1 = false;
    while (end1 < comma && isspace((unsigned char)*end1)) {
        end1++;
    }
    if (end1 < comma && *end1 == '%') {
        pct1 = true;
        end1++;
    }
    while (end1 < comma && isspace((unsigned char)*end1)) {
        end1++;
    }
    if (end1 != comma) {
        return false;
    }

    const char *s2 = comma + 1;
    while (*s2 && isspace((unsigned char)*s2)) {
        s2++;
    }
    char *end2 = NULL;
    float py = strtof(s2, &end2);
    if (!end2 || end2 == s2 || !isfinite(py) || py < 0.0f) {
        return false;
    }
    bool pct2 = false;
    while (*end2 && isspace((unsigned char)*end2)) {
        end2++;
    }
    if (*end2 == '%') {
        pct2 = true;
        end2++;
    }
    while (*end2 && isspace((unsigned char)*end2)) {
        end2++;
    }
    if (*end2 != '\0') {
        return false;
    }

    if (pct1) {
        if (px > 100.0f) {
            return false;
        }
        px *= 0.01f;
    }
    if (pct2) {
        if (py > 100.0f) {
            return false;
        }
        py *= 0.01f;
    }
    *out_x = px;
    *out_y = py;
    return true;
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
    } else if (strcasecmp(m, "fit") == 0 || strcasecmp(m, "contain") == 0 ||
               strcasecmp(m, "fit-border-color") == 0 || strcasecmp(m, "fit_border_color") == 0) {
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
    } else if (strcasecmp(f, "bilinear") == 0 || strcasecmp(f, "linear") == 0) {
        opts->filter = WAYWAL_FILTER_BILINEAR;
    } else if (strcasecmp(f, "catmullrom") == 0 || strcasecmp(f, "catmull-rom") == 0 ||
               strcasecmp(f, "mitchell") == 0) {
        opts->filter = WAYWAL_FILTER_MITCHELL;
    } else if (strcasecmp(f, "lanczos3") == 0 || strcasecmp(f, "lanczos") == 0) {
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

    if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
        opts->cmd = CLI_CMD_HELP;
        return true;
    }
    if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
        opts->cmd = CLI_CMD_VERSION;
        return true;
    }
    if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
        opts->verbose = true;
        return true;
    }
    if (strcmp(arg, "-q") == 0 || strcmp(arg, "--quiet") == 0) {
        opts->quiet = true;
        return true;
    }
    if (strcmp(arg, "-d") == 0 || strcmp(arg, "--daemon") == 0) {
        opts->daemonize = true;
        opts->no_daemon = false;
        return true;
    }
    if (strcmp(arg, "--no-daemon") == 0) {
        opts->no_daemon = true;
        opts->daemonize = false;
        return true;
    }
    if (strcmp(arg, "-a") == 0 || strcmp(arg, "--all") == 0) {
        opts->all_namespaces = true;
        return true;
    }
    if (strcmp(arg, "--invert-y") == 0) {
        opts->invert_y = true;
        return true;
    }
    if (strcmp(arg, "--no-cache") == 0) {
        opts->no_cache = true;
        return true;
    }
    if (strcmp(arg, "--clear-cache") == 0) {
        opts->clear_cache = true;
        return true;
    }
    if (match_opt_val(arg, "-n", optind_ptr, argc, argv, &val) ||
        match_opt_val(arg, "--namespace", optind_ptr, argc, argv, &val)) {
        strncpy(opts->namespace_str, val, sizeof(opts->namespace_str) - 1);
        opts->namespace_str[sizeof(opts->namespace_str) - 1] = '\0';
        return true;
    }
    if (match_opt_val(arg, "--format", optind_ptr, argc, argv, &val)) {
        strncpy(opts->pixel_format, val, sizeof(opts->pixel_format) - 1);
        opts->pixel_format[sizeof(opts->pixel_format) - 1] = '\0';
        return true;
    }
    if (match_opt_val(arg, "--crop-gravity", optind_ptr, argc, argv, &val)) {
        if (!waywal_parse_crop_gravity(val, &opts->crop_gravity_x, &opts->crop_gravity_y)) {
            fprintf(stderr, "Error: Invalid --crop-gravity value '%s'\n", val ? val : "");
            return false;
        }
        opts->has_crop_gravity = true;
        return true;
    }
    if (match_opt_val(arg, "--transition-type", optind_ptr, argc, argv, &val) ||
        match_opt_val(arg, "-t", optind_ptr, argc, argv, &val)) {
        apply_transition_type(opts, val);
        opts->has_transition_type = true;
        return true;
    }
    if (match_opt_val(arg, "--transition-duration", optind_ptr, argc, argv, &val)) {
        char *endp = NULL;
        float dur_s = strtof(val, &endp);
        if (!endp || endp == val || *endp != '\0' || !isfinite(dur_s) || dur_s < 0.0f ||
            dur_s > 3600000.0f) {
            fprintf(stderr, "Error: Invalid --transition-duration value '%s'\n", val ? val : "");
            return false;
        }
        opts->transition_duration = dur_s;
        opts->transition_duration_ms = (uint32_t)(dur_s * 1000.0f);
        opts->has_transition_duration = true;
        return true;
    }
    if (match_opt_val(arg, "--transition-step", optind_ptr, argc, argv, &val)) {
        int step = atoi(val);
        if (step > 0) {
            opts->transition_step = (uint32_t)step;
            if (!opts->has_transition_duration) {
                opts->transition_duration_ms = (uint32_t)(step * 16);
                opts->transition_duration = (float)opts->transition_duration_ms / 1000.0f;
            }
        }
        return true;
    }
    if (match_opt_val(arg, "--transition-fps", optind_ptr, argc, argv, &val) ||
        match_opt_val(arg, "--fps", optind_ptr, argc, argv, &val)) {
        int fps = atoi(val);
        if (fps > 0) {
            opts->transition_fps = (uint32_t)fps;
            opts->has_transition_fps = true;
        }
        return true;
    }
    if (match_opt_val(arg, "--transition-angle", optind_ptr, argc, argv, &val)) {
        float deg = strtof(val, NULL);
        set_angle_degrees(opts, deg);
        opts->has_transition_angle = true;
        return true;
    }
    if (match_opt_val(arg, "--transition-wave", optind_ptr, argc, argv, &val)) {
        if (sscanf(val, "%f,%f", &opts->transition_wave_freq, &opts->transition_wave_amp) == 2) {
            opts->has_transition_wave = true;
        }
        return true;
    }
    if (match_opt_val(arg, "--transition-pos", optind_ptr, argc, argv, &val)) {
        if (!parse_transition_pos(val, &opts->transition_pos_x, &opts->transition_pos_y)) {
            fprintf(stderr, "Error: Invalid --transition-pos value '%s'\n", val ? val : "");
            return false;
        }
        opts->has_transition_pos = true;
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
        opts->has_transition_bezier = true;
        return true;
    }
    if (match_opt_val(arg, "--fill-color", optind_ptr, argc, argv, &val)) {
        if (!parse_hex_color(val, &opts->clear_color)) {
            fprintf(stderr,
                    "Error: Invalid --fill-color hex format (expected RRGGBB or RRGGBBAA)\n");
            return false;
        }
        opts->has_fill_color = true;
        return true;
    }
    if (match_opt_val(arg, "--transition-shader", optind_ptr, argc, argv, &val)) {
        strncpy(opts->custom_shader_path, val, sizeof(opts->custom_shader_path) - 1);
        opts->custom_shader_path[sizeof(opts->custom_shader_path) - 1] = '\0';
        opts->transition_type = WAYWAL_TRANSITION_CUSTOM;
        opts->has_transition_type = true;
        if (waywal_shader_is_audio_reactive(val)) {
            opts->audio_reactive = true;
        }
        return true;
    }
    if (match_opt_val(arg, "--shader", optind_ptr, argc, argv, &val)) {
        if (!val || val[0] == '\0') {
            return false;
        }
        if (opts->cmd == CLI_CMD_NONE) {
            opts->cmd = CLI_CMD_SHADER;
        }
        if (strncasecmp(val, "preset:", 7) == 0) {
            opts->shader_preset_explicit = true;
        }
        char resolved_preset[4096];
        bool resolved_ok =
            waywal_shader_resolve_path(val, resolved_preset, sizeof(resolved_preset));
        if (opts->shader_preset_explicit && !resolved_ok) {
            fprintf(stderr,
                    "Error: Shader preset '%s' not found (run 'wwal shader list' to see presets)\n",
                    val);
            return false;
        }
        const char *effective = resolved_ok ? resolved_preset : val;
        if (opts->cmd == CLI_CMD_SHADER) {
            strncpy(opts->filepath, effective, sizeof(opts->filepath) - 1);
            opts->filepath[sizeof(opts->filepath) - 1] = '\0';
        }
        strncpy(opts->custom_shader_path, effective, sizeof(opts->custom_shader_path) - 1);
        opts->custom_shader_path[sizeof(opts->custom_shader_path) - 1] = '\0';
        if (waywal_shader_is_audio_reactive(val) || waywal_shader_is_audio_reactive(effective)) {
            opts->audio_reactive = true;
        }
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
        if (opts->env_outputs_set) {
            opts->num_outputs = 0;
            opts->output[0] = '\0';
            opts->env_outputs_set = false;
        }
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
        opts->has_scaling_mode = true;
        return true;
    }
    if (match_opt_val(arg, "--resize", optind_ptr, argc, argv, &val) ||
        match_opt_val(arg, "--scaling-mode", optind_ptr, argc, argv, &val)) {
        apply_scaling_mode(opts, val);
        opts->has_scaling_mode = true;
        return true;
    }
    if (match_opt_val(arg, "--mode", optind_ptr, argc, argv, &val)) {
        if (strcasecmp(val, "auto") == 0) {
            opts->auto_day_night = true;
            opts->has_auto_day_night = true;
            opts->has_theme_mode = false;
        } else if (opts->cmd == CLI_CMD_THEME || strcasecmp(val, "light") == 0 ||
                   strcasecmp(val, "dark") == 0) {
            opts->theme_light_mode = (strcasecmp(val, "light") == 0);
            opts->has_theme_mode = true;
            opts->auto_day_night = false;
            opts->has_auto_day_night = true;
        } else {
            apply_scaling_mode(opts, val);
            opts->has_scaling_mode = true;
        }
        return true;
    }
    if (match_opt_val(arg, "--filter", optind_ptr, argc, argv, &val)) {
        apply_filter_mode(opts, val);
        opts->has_filter_mode = true;
        return true;
    }
    if (match_opt_val(arg, "-f", optind_ptr, argc, argv, &val)) {
        if (opts->cmd == CLI_CMD_DAEMON_ALIAS || strcasecmp(val, "xrgb") == 0 ||
            strcasecmp(val, "xbgr") == 0 || strcasecmp(val, "rgb") == 0 ||
            strcasecmp(val, "bgr") == 0) {
            strncpy(opts->pixel_format, val, sizeof(opts->pixel_format) - 1);
            opts->pixel_format[sizeof(opts->pixel_format) - 1] = '\0';
        } else {
            apply_filter_mode(opts, val);
            opts->has_filter_mode = true;
        }
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
        opts->has_theme_mode = true;
        opts->auto_day_night = false;
        opts->has_auto_day_night = true;
        return true;
    }
    if (strcmp(arg, "--dark") == 0) {
        opts->theme_light_mode = false;
        opts->has_theme_mode = true;
        opts->auto_day_night = false;
        opts->has_auto_day_night = true;
        return true;
    }
    if (strcmp(arg, "--auto-day-night") == 0) {
        opts->auto_day_night = true;
        opts->has_auto_day_night = true;
        opts->has_theme_mode = false;
        return true;
    }
    if (strcmp(arg, "--no-auto-day-night") == 0) {
        opts->auto_day_night = false;
        opts->has_auto_day_night = true;
        return true;
    }
    if (match_opt_val(arg, "--theme-mode", optind_ptr, argc, argv, &val)) {
        if (strcasecmp(val, "auto") == 0) {
            opts->auto_day_night = true;
            opts->has_auto_day_night = true;
            opts->has_theme_mode = false;
        } else {
            opts->theme_light_mode = (strcasecmp(val, "light") == 0);
            opts->has_theme_mode = true;
            opts->auto_day_night = false;
            opts->has_auto_day_night = true;
        }
        return true;
    }
    if (match_opt_val(arg, "--latitude", optind_ptr, argc, argv, &val)) {
        char *endp = NULL;
        double lat = val ? strtod(val, &endp) : NAN;
        if (!val || !endp || endp == val || *endp != '\0' || !isfinite(lat) || lat < -90.0 ||
            lat > 90.0) {
            fprintf(stderr, "Error: Invalid --latitude '%s' (expected -90.0..90.0)\n",
                    val ? val : "");
            return false;
        }
        opts->solar_latitude = lat;
        opts->has_solar_lat = true;
        opts->has_solar_coords = true;
        return true;
    }
    if (match_opt_val(arg, "--longitude", optind_ptr, argc, argv, &val)) {
        char *endp = NULL;
        double lon = val ? strtod(val, &endp) : NAN;
        if (!val || !endp || endp == val || *endp != '\0' || !isfinite(lon) || lon < -180.0 ||
            lon > 180.0) {
            fprintf(stderr, "Error: Invalid --longitude '%s' (expected -180.0..180.0)\n",
                    val ? val : "");
            return false;
        }
        opts->solar_longitude = lon;
        opts->has_solar_lon = true;
        opts->has_solar_coords = true;
        return true;
    }
    if (match_opt_val(arg, "--source-color-index", optind_ptr, argc, argv, &val)) {
        if (!val || val[0] == '\0' || val[0] == '-') {
            fprintf(stderr, "Error: Invalid --source-color-index value (expected 0..3)\n");
            return false;
        }
        char *endptr = NULL;
        unsigned long idx = strtoul(val, &endptr, 10);
        if (!endptr || endptr == val || *endptr != '\0' || idx > 3UL) {
            fprintf(stderr, "Error: Invalid --source-color-index '%s' (expected 0..3)\n", val);
            return false;
        }
        opts->source_color_index = (uint8_t)idx;
        opts->has_source_color_index = true;
        return true;
    }
    if (match_opt_val(arg, "--seed-color", optind_ptr, argc, argv, &val)) {
        color_rgba_t sc = {0};
        if (!val || !parse_hex_color(val, &sc)) {
            fprintf(stderr, "Error: Invalid --seed-color '%s' (expected #RRGGBB or 0xRRGGBB)\n",
                    val ? val : "");
            return false;
        }
        opts->seed_color = ((uint32_t)sc.r << 16) | ((uint32_t)sc.g << 8) | (uint32_t)sc.b;
        opts->has_seed_color = true;
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
    if (match_opt_val(arg, "--prop", optind_ptr, argc, argv, &val) ||
        match_opt_val(arg, "-p", optind_ptr, argc, argv, &val)) {
        if (!val || val[0] == '\0' || strchr(val, '=') == NULL) {
            fprintf(stderr, "Error: Invalid --prop format (expected key=value)\n");
            return false;
        }
        size_t cur_len = strlen(opts->workshop_props);
        if (cur_len > 0 && cur_len + 1 < sizeof(opts->workshop_props)) {
            opts->workshop_props[cur_len++] = ';';
            opts->workshop_props[cur_len] = '\0';
        }
        snprintf(opts->workshop_props + cur_len, sizeof(opts->workshop_props) - cur_len, "%s", val);
        return true;
    }
    if (strcmp(arg, "--json") == 0 || strcmp(arg, "-j") == 0) {
        opts->json_output = true;
        return true;
    }
    if (match_opt_val(arg, "--volume", optind_ptr, argc, argv, &val)) {
        if (!val || val[0] == '\0') {
            fprintf(stderr, "Error: Invalid --volume value\n");
            return false;
        }
        char *endptr = NULL;
        float vol = strtof(val, &endptr);
        if (endptr == val || *endptr != '\0' || !isfinite(vol)) {
            fprintf(stderr, "Error: Invalid --volume value\n");
            return false;
        }
        if (vol < 0.0f)
            vol = 0.0f;
        if (vol > 1.0f)
            vol = 1.0f;
        opts->volume = vol;
        return true;
    }
    if (strcmp(arg, "--mute-on-audio") == 0) {
        opts->mute_on_audio = 1;
        return true;
    }
    if (strcmp(arg, "--no-mute-on-audio") == 0) {
        opts->mute_on_audio = 0;
        return true;
    }
    if (strcmp(arg, "--theme-from-media") == 0) {
        opts->theme_from_media = 1;
        return true;
    }
    if (strcmp(arg, "--no-theme-from-media") == 0) {
        opts->theme_from_media = 0;
        return true;
    }
    if (match_opt_val(arg, "--loop", optind_ptr, argc, argv, &val)) {
        if (!val || val[0] == '\0' || val[0] == '-') {
            fprintf(stderr, "Error: Invalid --loop value '%s'\n", val ? val : "");
            return false;
        }
        char *endptr = NULL;
        unsigned long long v = strtoull(val, &endptr, 10);
        if (endptr == val || *endptr != '\0') {
            fprintf(stderr, "Error: Invalid --loop value '%s'\n", val);
            return false;
        }
        opts->video_loop_count = (uint64_t)v;
        return true;
    }
    if (match_opt_val(arg, "--speed", optind_ptr, argc, argv, &val)) {
        if (!val || val[0] == '\0') {
            fprintf(stderr, "Error: Invalid --speed value\n");
            return false;
        }
        char *endptr = NULL;
        float s = strtof(val, &endptr);
        if (endptr == val || *endptr != '\0' || !isfinite(s) || s <= 0.0f) {
            fprintf(stderr, "Error: Invalid --speed value '%s'\n", val);
            return false;
        }
        opts->video_speed = s;
        return true;
    }
    if (match_opt_val(arg, "--sort", optind_ptr, argc, argv, &val) ||
        match_opt_val(arg, "--sorting", optind_ptr, argc, argv, &val)) {
        if (!val || !waywal_parse_sort_mode(val, &opts->slideshow_sort)) {
            fprintf(stderr,
                    "Error: Invalid --sort value '%s' (expected random, ascending, descending, or "
                    "mtime)\n",
                    val ? val : "");
            return false;
        }
        opts->has_slideshow_sort = true;
        opts->slideshow_random = (opts->slideshow_sort == WAYWAL_SORT_RANDOM);
        return true;
    }
    if (match_opt_val(arg, "--duration", optind_ptr, argc, argv, &val) ||
        match_opt_val(arg, "--interval", optind_ptr, argc, argv, &val)) {
        uint32_t sec = 0;
        if (!val || !waywal_parse_duration_seconds(val, &sec)) {
            fprintf(stderr, "Error: Invalid duration '%s'\n", val ? val : "");
            return false;
        }
        opts->slideshow_interval_s = sec;
        opts->has_slideshow_interval = true;
        return true;
    }
    if (strcmp(arg, "--shuffle") == 0 || strcmp(arg, "--random") == 0) {
        opts->slideshow_random = true;
        opts->slideshow_sort = WAYWAL_SORT_RANDOM;
        opts->has_slideshow_sort = true;
        return true;
    }

    return false;
}

static const char *get_compat_env(const char *awww_key, const char *swww_key)
{
    const char *v = getenv(awww_key);
    if (v && v[0] != '\0') {
        return v;
    }
    v = getenv(swww_key);
    if (v && v[0] != '\0') {
        return v;
    }
    return NULL;
}

static void apply_compat_env_vars(cli_options_t *opts)
{
    const char *val = NULL;

    val = get_compat_env("AWWW_NAMESPACE", "SWWW_NAMESPACE");
    if (val) {
        strncpy(opts->namespace_str, val, sizeof(opts->namespace_str) - 1);
        opts->namespace_str[sizeof(opts->namespace_str) - 1] = '\0';
    }

    val = get_compat_env("AWWW_TRANSITION", "SWWW_TRANSITION");
    if (val) {
        apply_transition_type(opts, val);
        opts->has_transition_type = true;
    }

    val = get_compat_env("AWWW_TRANSITION_FPS", "SWWW_TRANSITION_FPS");
    if (val) {
        int fps = atoi(val);
        if (fps > 0) {
            opts->transition_fps = (uint32_t)fps;
            opts->has_transition_fps = true;
        }
    }

    val = get_compat_env("AWWW_TRANSITION_STEP", "SWWW_TRANSITION_STEP");
    if (val) {
        int step = atoi(val);
        if (step > 0) {
            opts->transition_step = (uint32_t)step;
            opts->transition_duration_ms = (uint32_t)(step * 16);
            opts->transition_duration = (float)opts->transition_duration_ms / 1000.0f;
        }
    }

    val = get_compat_env("AWWW_TRANSITION_DURATION", "SWWW_TRANSITION_DURATION");
    if (val && val[0] != '\0') {
        char *endp = NULL;
        errno = 0;
        float dur_s = strtof(val, &endp);
        while (endp && *endp && isspace((unsigned char)*endp)) {
            endp++;
        }
        if (errno == 0 && endp && endp != val && *endp == '\0' && isfinite(dur_s) &&
            dur_s >= 0.0f && dur_s <= 3600000.0f) {
            opts->transition_duration = dur_s;
            opts->transition_duration_ms = (uint32_t)(dur_s * 1000.0f);
            opts->has_transition_duration = true;
        }
    }

    val = get_compat_env("AWWW_TRANSITION_ANGLE", "SWWW_TRANSITION_ANGLE");
    if (val) {
        set_angle_degrees(opts, strtof(val, NULL));
        opts->has_transition_angle = true;
    }

    val = get_compat_env("AWWW_TRANSITION_POS", "SWWW_TRANSITION_POS");
    if (val) {
        if (parse_transition_pos(val, &opts->transition_pos_x, &opts->transition_pos_y)) {
            opts->has_transition_pos = true;
        }
    }

    val = get_compat_env("AWWW_TRANSITION_BEZIER", "SWWW_TRANSITION_BEZIER");
    if (val) {
        float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
        if (sscanf(val, "%f,%f,%f,%f", &x1, &y1, &x2, &y2) == 4) {
            opts->bezier = bezier_create(x1, y1, x2, y2);
            opts->has_custom_bezier = true;
            opts->has_transition_bezier = true;
        }
    }

    val = get_compat_env("AWWW_TRANSITION_WAVE", "SWWW_TRANSITION_WAVE");
    if (val) {
        if (sscanf(val, "%f,%f", &opts->transition_wave_freq, &opts->transition_wave_amp) == 2) {
            opts->has_transition_wave = true;
        }
    }

    val = get_compat_env("AWWW_INVERT_Y", "SWWW_INVERT_Y");
    if (!val) {
        val = get_compat_env("AWWW_TRANSITION_INVERT_Y", "SWWW_TRANSITION_INVERT_Y");
    }
    if (val) {
        if (strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0 || strcasecmp(val, "yes") == 0) {
            opts->invert_y = true;
        }
    }

    val = get_compat_env("AWWW_CROP_GRAVITY", "SWWW_CROP_GRAVITY");
    if (val) {
        if (waywal_parse_crop_gravity(val, &opts->crop_gravity_x, &opts->crop_gravity_y)) {
            opts->has_crop_gravity = true;
        }
    }

    val = get_compat_env("AWWW_FILTER", "SWWW_FILTER");
    if (val) {
        apply_filter_mode(opts, val);
        opts->has_filter_mode = true;
    }

    val = get_compat_env("AWWW_RESIZE", "SWWW_RESIZE");
    if (val) {
        apply_scaling_mode(opts, val);
        opts->has_scaling_mode = true;
    }

    val = get_compat_env("AWWW_FILL_COLOR", "SWWW_FILL_COLOR");
    if (val) {
        if (parse_hex_color(val, &opts->clear_color)) {
            opts->has_fill_color = true;
        }
    }

    val = get_compat_env("AWWW_OUTPUTS", "SWWW_OUTPUTS");
    if (val) {
        add_output_target(opts, val);
        if (opts->num_outputs > 0) {
            opts->env_outputs_set = true;
        }
    }
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
    opts->crop_gravity_x = 0.5f;
    opts->crop_gravity_y = 0.5f;
    opts->has_crop_gravity = false;
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
    opts->slideshow_sort = WAYWAL_SORT_ASCENDING;
    opts->volume = -1.0f;
    opts->mute_on_audio = -1;
    opts->theme_from_media = -1;

    apply_compat_env_vars(opts);

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
            if (opts->cmd == CLI_CMD_HELP || opts->cmd == CLI_CMD_VERSION)
                return true;
        }
        return true;
    }

    int optind = 1;
    while (optind < argc && argv[optind][0] == '-') {
        if (!parse_common_flag(&optind, argc, argv, opts)) {
            return false;
        }
        if (opts->cmd == CLI_CMD_HELP || opts->cmd == CLI_CMD_VERSION)
            return true;
        optind++;
    }

    if (optind >= argc) {
        if (opts->cmd == CLI_CMD_SHADER && opts->filepath[0] != '\0') {
            if (opts->invert_y && !opts->pos_y_inverted && opts->transition_pos_y >= 0.0f &&
                opts->transition_pos_y <= 1.0f) {
                opts->transition_pos_y = 1.0f - opts->transition_pos_y;
                opts->pos_y_inverted = true;
            }
            return true;
        }
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
    } else if (strcmp(cmd_str, "status") == 0) {
        opts->cmd = CLI_CMD_STATUS;
        opts->json_output = true;
        while (optind < argc) {
            if (!parse_common_flag(&optind, argc, argv, opts))
                return false;
            optind++;
        }
    } else if (strcmp(cmd_str, "subscribe") == 0) {
        opts->cmd = CLI_CMD_SUBSCRIBE;
        while (optind < argc) {
            if (!parse_common_flag(&optind, argc, argv, opts))
                return false;
            optind++;
        }
    } else if (strcmp(cmd_str, "audio") == 0) {
        opts->cmd = CLI_CMD_AUDIO;
        while (optind < argc) {
            if (!parse_common_flag(&optind, argc, argv, opts))
                return false;
            optind++;
        }
        if (opts->volume < 0.0f && opts->mute_on_audio == -1 && opts->theme_from_media == -1) {
            fprintf(stderr, "Error: 'audio' requires at least one of --volume, --mute-on-audio, "
                            "or --theme-from-media\n");
            return false;
        }
    } else if (strcmp(cmd_str, "set-uniform") == 0 || strcmp(cmd_str, "uniform") == 0) {
        opts->cmd = CLI_CMD_SET_UNIFORM;
        while (optind < argc) {
            const char *arg = argv[optind];
            bool is_neg_number =
                (arg[0] == '-' && (isdigit((unsigned char)arg[1]) || arg[1] == '.'));
            if (arg[0] == '-' && !is_neg_number) {
                if (!parse_common_flag(&optind, argc, argv, opts)) {
                    fprintf(stderr, "Error: Unknown set-uniform option '%s'\n", arg);
                    return false;
                }
            } else if (opts->uniform_name[0] == '\0') {
                size_t name_len = strlen(arg);
                if (name_len == 0 || name_len >= sizeof(opts->uniform_name) ||
                    !(isalpha((unsigned char)arg[0]) || arg[0] == '_')) {
                    fprintf(stderr,
                            "Error: Invalid uniform name '%s' (must start with letter/underscore "
                            "and be < %zu chars)\n",
                            arg, sizeof(opts->uniform_name));
                    return false;
                }
                memcpy(opts->uniform_name, arg, name_len + 1);
            } else {
                if (opts->uniform_components >= 4) {
                    fprintf(stderr, "Error: Too many uniform components (max 4)\n");
                    return false;
                }
                char *endptr = NULL;
                float v = strtof(arg, &endptr);
                if (endptr == arg || *endptr != '\0' || !isfinite(v)) {
                    fprintf(stderr, "Error: Invalid uniform float value '%s'\n", arg);
                    return false;
                }
                opts->uniform_values[opts->uniform_components++] = v;
            }
            optind++;
        }
        if (opts->uniform_name[0] == '\0' || opts->uniform_components == 0) {
            fprintf(stderr, "Error: 'set-uniform' requires <NAME> and 1..4 float values\n");
            return false;
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
    } else if (strcmp(cmd_str, "init") == 0 || strcmp(cmd_str, "daemon") == 0) {
        opts->cmd = CLI_CMD_DAEMON_ALIAS;
        if (strcmp(cmd_str, "init") == 0 && !opts->no_daemon) {
            opts->daemonize = true;
        }
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
        if (!opts->read_stdin && access(opts->filepath, F_OK) != 0 &&
            parse_hex_color(opts->filepath, &opts->clear_color)) {
            opts->is_solid_color = true;
        }
    } else if (strcmp(cmd_str, "shader") == 0) {
        opts->cmd = CLI_CMD_SHADER;
        bool has_path = false;
        bool saw_preset_kw = false;
        while (optind < argc) {
            const char *arg = argv[optind];
            if (strcmp(arg, "--list") == 0) {
                if (has_path || saw_preset_kw) {
                    fprintf(stderr, "Error: Cannot combine '--list' with a shader path or preset\n");
                    return false;
                }
                opts->shader_list = true;
            } else if (strcmp(arg, "--preset") == 0) {
                if (has_path || opts->shader_list || saw_preset_kw) {
                    fprintf(stderr, "Error: Unexpected '--preset' option\n");
                    return false;
                }
                saw_preset_kw = true;
                opts->shader_preset_explicit = true;
            } else if (arg[0] == '-') {
                if (!parse_common_flag(&optind, argc, argv, opts)) {
                    fprintf(stderr, "Error: Unknown shader option '%s'\n", arg);
                    return false;
                }
            } else if (!has_path && !opts->shader_list && !saw_preset_kw &&
                       strcmp(arg, "list") == 0) {
                opts->shader_list = true;
            } else if (!has_path && !opts->shader_list && !saw_preset_kw &&
                       strcmp(arg, "preset") == 0) {
                saw_preset_kw = true;
                opts->shader_preset_explicit = true;
            } else if (!has_path && !opts->shader_list) {
                if (strncasecmp(arg, "preset:", 7) == 0) {
                    opts->shader_preset_explicit = true;
                }
                const char *preset_check =
                    (strncasecmp(arg, "preset:", 7) == 0) ? (arg + 7) : arg;
                if (opts->shader_preset_explicit &&
                    (preset_check[0] == '\0' || strchr(preset_check, '/') != NULL ||
                     strchr(preset_check, '\\') != NULL || strstr(preset_check, "..") != NULL)) {
                    fprintf(stderr, "Error: Invalid shader preset name '%s'\n", arg);
                    return false;
                }
                strncpy(opts->shader_preset, arg, sizeof(opts->shader_preset) - 1);
                opts->shader_preset[sizeof(opts->shader_preset) - 1] = '\0';
                char query_buf[4200];
                if (opts->shader_preset_explicit && strncasecmp(arg, "preset:", 7) != 0) {
                    snprintf(query_buf, sizeof(query_buf), "preset:%s", arg);
                } else {
                    snprintf(query_buf, sizeof(query_buf), "%s", arg);
                }
                char resolved_preset[4096];
                bool resolved_ok =
                    waywal_shader_resolve_path(query_buf, resolved_preset, sizeof(resolved_preset));
                const char *effective =
                    resolved_ok ? resolved_preset
                                : (opts->shader_preset_explicit ? query_buf : arg);
                strncpy(opts->filepath, effective, sizeof(opts->filepath) - 1);
                opts->filepath[sizeof(opts->filepath) - 1] = '\0';
                strncpy(opts->custom_shader_path, effective,
                        sizeof(opts->custom_shader_path) - 1);
                opts->custom_shader_path[sizeof(opts->custom_shader_path) - 1] = '\0';
                if (opts->shader_preset_explicit && !resolved_ok) {
                    fprintf(stderr,
                            "Error: Shader preset '%s' not found (run 'wwal shader list' to see "
                            "presets)\n",
                            arg);
                    return false;
                }
                if (waywal_shader_is_audio_reactive(query_buf) ||
                    waywal_shader_is_audio_reactive(effective)) {
                    opts->audio_reactive = true;
                }
                has_path = true;
            } else {
                fprintf(stderr, "Error: Unexpected argument '%s'\n", arg);
                return false;
            }
            optind++;
        }
        if (!opts->shader_list && !has_path) {
            if (saw_preset_kw) {
                fprintf(stderr,
                        "Error: 'shader preset' requires a preset name (see 'wwal shader list')\n");
            } else {
                fprintf(stderr,
                        "Error: 'shader' command requires a path, preset name, or 'list'\n");
            }
            return false;
        }
    } else if (strcmp(cmd_str, "workshop") == 0) {
        opts->cmd = CLI_CMD_WORKSHOP;
        if (optind >= argc) {
            fprintf(stderr,
                     "Error: 'workshop' requires a subcommand (list, info, apply) or ID/path\n");
            return false;
        }
        const char *sub = argv[optind];
        if (strcmp(sub, "list") == 0) {
            opts->workshop_subcmd = WAYWAL_WORKSHOP_LIST;
            optind++;
            while (optind < argc) {
                if (!parse_common_flag(&optind, argc, argv, opts)) {
                    fprintf(stderr, "Error: Unknown workshop option '%s'\n", argv[optind]);
                    return false;
                }
                optind++;
            }
        } else if (strcmp(sub, "info") == 0) {
            opts->workshop_subcmd = WAYWAL_WORKSHOP_INFO;
            optind++;
            bool has_id = false;
            while (optind < argc) {
                const char *arg = argv[optind];
                if (arg[0] == '-') {
                    if (!parse_common_flag(&optind, argc, argv, opts)) {
                        fprintf(stderr, "Error: Unknown workshop option '%s'\n", arg);
                        return false;
                    }
                } else if (!has_id) {
                    strncpy(opts->filepath, arg, sizeof(opts->filepath) - 1);
                    opts->filepath[sizeof(opts->filepath) - 1] = '\0';
                    has_id = true;
                } else {
                    fprintf(stderr, "Error: Unexpected argument '%s'\n", arg);
                    return false;
                }
                optind++;
            }
            if (!has_id) {
                fprintf(stderr,
                        "Error: 'workshop info' requires a Workshop ID or directory path\n");
                return false;
            }
        } else if (strcmp(sub, "sync-previews") == 0) {
            opts->workshop_subcmd = WAYWAL_WORKSHOP_SYNC_PREVIEWS;
            optind++;
            bool has_dir = false;
            while (optind < argc) {
                const char *arg = argv[optind];
                if (arg[0] == '-') {
                    if (!parse_common_flag(&optind, argc, argv, opts)) {
                        fprintf(stderr, "Error: Unknown workshop option '%s'\n", arg);
                        return false;
                    }
                } else if (!has_dir) {
                    strncpy(opts->filepath, arg, sizeof(opts->filepath) - 1);
                    opts->filepath[sizeof(opts->filepath) - 1] = '\0';
                    has_dir = true;
                } else {
                    fprintf(stderr, "Error: Unexpected argument '%s'\n", arg);
                    return false;
                }
                optind++;
            }
        } else {
            opts->workshop_subcmd = WAYWAL_WORKSHOP_APPLY;
            if (strcmp(sub, "apply") == 0) {
                optind++;
            }
            bool has_id = false;
            while (optind < argc) {
                const char *arg = argv[optind];
                if (arg[0] == '-') {
                    if (!parse_common_flag(&optind, argc, argv, opts)) {
                        fprintf(stderr, "Error: Unknown workshop option '%s'\n", arg);
                        return false;
                    }
                } else if (!has_id) {
                    strncpy(opts->filepath, arg, sizeof(opts->filepath) - 1);
                    opts->filepath[sizeof(opts->filepath) - 1] = '\0';
                    has_id = true;
                } else {
                    fprintf(stderr, "Error: Unexpected argument '%s'\n", arg);
                    return false;
                }
                optind++;
            }
            if (!has_id) {
                fprintf(stderr,
                        "Error: 'workshop apply' requires a Workshop ID or directory path\n");
                return false;
            }
        }
    } else if (strcmp(cmd_str, "pick") == 0) {
        opts->cmd = CLI_CMD_PICK;
        opts->pick_menu = WAYWAL_PICK_MENU_AUTO;
        while (optind < argc) {
            const char *arg = argv[optind];
            const char *val = NULL;
            if (match_opt_val(arg, "--menu", &optind, argc, argv, &val)) {
                if (strcasecmp(val, "auto") == 0) {
                    opts->pick_menu = WAYWAL_PICK_MENU_AUTO;
                } else if (strcasecmp(val, "rofi") == 0) {
                    opts->pick_menu = WAYWAL_PICK_MENU_ROFI;
                } else if (strcasecmp(val, "fuzzel") == 0) {
                    opts->pick_menu = WAYWAL_PICK_MENU_FUZZEL;
                } else if (strcasecmp(val, "walker") == 0) {
                    opts->pick_menu = WAYWAL_PICK_MENU_WALKER;
                } else if (strcasecmp(val, "dmenu") == 0) {
                    opts->pick_menu = WAYWAL_PICK_MENU_DMENU;
                } else if (strcasecmp(val, "stdout") == 0) {
                    opts->pick_menu = WAYWAL_PICK_MENU_STDOUT;
                } else if (strcasecmp(val, "json") == 0) {
                    opts->pick_menu = WAYWAL_PICK_MENU_JSON;
                } else {
                    fprintf(stderr,
                            "Error: Unknown --menu mode '%s' (expected auto, rofi, fuzzel, walker, "
                            "dmenu, stdout, or json)\n",
                            val);
                    return false;
                }
            } else if (match_opt_val(arg, "--dir", &optind, argc, argv, &val)) {
                if (!val || val[0] == '\0') {
                    fprintf(stderr, "Error: --dir requires a non-empty directory path\n");
                    return false;
                }
                strncpy(opts->pick_dir, val, sizeof(opts->pick_dir) - 1);
                opts->pick_dir[sizeof(opts->pick_dir) - 1] = '\0';
            } else if (arg[0] == '-') {
                if (!parse_common_flag(&optind, argc, argv, opts)) {
                    fprintf(stderr, "Error: Unknown pick option '%s'\n", arg);
                    return false;
                }
                if (opts->json_output && opts->pick_menu == WAYWAL_PICK_MENU_AUTO) {
                    opts->pick_menu = WAYWAL_PICK_MENU_JSON;
                }
            } else if (opts->pick_dir[0] == '\0') {
                strncpy(opts->pick_dir, arg, sizeof(opts->pick_dir) - 1);
                opts->pick_dir[sizeof(opts->pick_dir) - 1] = '\0';
            } else {
                fprintf(stderr, "Error: Unexpected argument '%s'\n", arg);
                return false;
            }
            optind++;
        }
    } else if (strcmp(cmd_str, "theme") == 0) {
        opts->cmd = CLI_CMD_THEME;
        opts->extract_theme = true;
        while (optind < argc) {
            const char *arg = argv[optind];
            const char *val = NULL;
            if (match_opt_val(arg, "--img", &optind, argc, argv, &val) ||
                match_opt_val(arg, "--image", &optind, argc, argv, &val) ||
                match_opt_val(arg, "-i", &optind, argc, argv, &val)) {
                if (!val || val[0] == '\0') {
                    fprintf(stderr, "Error: '%s' requires a file path or preset name\n", arg);
                    return false;
                }
                strncpy(opts->filepath, val, sizeof(opts->filepath) - 1);
                opts->filepath[sizeof(opts->filepath) - 1] = '\0';
            } else if (arg[0] == '-') {
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
    } else if (strcmp(cmd_str, "next") == 0 || strcmp(cmd_str, "prev") == 0 ||
               strcmp(cmd_str, "previous") == 0) {
        opts->cmd = CLI_CMD_SLIDESHOW_CTRL;
        opts->slideshow_action =
            (strcmp(cmd_str, "next") == 0) ? WAYWAL_SLIDESHOW_NEXT : WAYWAL_SLIDESHOW_PREV;
        while (optind < argc) {
            if (!parse_common_flag(&optind, argc, argv, opts)) {
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
        if (strcmp(sub, "stop") == 0 || strcmp(sub, "pause") == 0 || strcmp(sub, "resume") == 0 ||
            strcmp(sub, "unpause") == 0 || strcmp(sub, "toggle") == 0 || strcmp(sub, "next") == 0 ||
            strcmp(sub, "prev") == 0 || strcmp(sub, "previous") == 0) {
            opts->cmd = CLI_CMD_SLIDESHOW_CTRL;
            if (strcmp(sub, "stop") == 0) {
                opts->slideshow_action = WAYWAL_SLIDESHOW_STOP;
            } else if (strcmp(sub, "pause") == 0) {
                opts->slideshow_action = WAYWAL_SLIDESHOW_PAUSE;
            } else if (strcmp(sub, "resume") == 0 || strcmp(sub, "unpause") == 0) {
                opts->slideshow_action = WAYWAL_SLIDESHOW_RESUME;
            } else if (strcmp(sub, "toggle") == 0) {
                opts->slideshow_action = WAYWAL_SLIDESHOW_TOGGLE;
            } else if (strcmp(sub, "next") == 0) {
                opts->slideshow_action = WAYWAL_SLIDESHOW_NEXT;
            } else {
                opts->slideshow_action = WAYWAL_SLIDESHOW_PREV;
            }
            optind++;
            while (optind < argc) {
                if (!parse_common_flag(&optind, argc, argv, opts)) {
                    fprintf(stderr, "Error: Unknown option '%s'\n", argv[optind]);
                    return false;
                }
                optind++;
            }
        } else {
            opts->cmd = CLI_CMD_SLIDESHOW;
            if (strcmp(sub, "start") == 0) {
                optind++;
            }

            while (optind < argc) {
                const char *arg = argv[optind];
                const char *val = NULL;
                if (match_opt_val(arg, "--interval", &optind, argc, argv, &val) ||
                    match_opt_val(arg, "--duration", &optind, argc, argv, &val) ||
                    match_opt_val(arg, "-d", &optind, argc, argv, &val)) {
                    uint32_t sec = 0;
                    if (!val || !waywal_parse_duration_seconds(val, &sec)) {
                        fprintf(stderr, "Error: Invalid duration '%s'\n", val ? val : "");
                        return false;
                    }
                    opts->slideshow_interval_s = sec;
                    opts->has_slideshow_interval = true;
                } else if (match_opt_val(arg, "--dir", &optind, argc, argv, &val) ||
                           match_opt_val(arg, "--path", &optind, argc, argv, &val)) {
                    if (!val || val[0] == '\0') {
                        fprintf(stderr, "Error: '--dir' requires a directory path\n");
                        return false;
                    }
                    strncpy(opts->filepath, val, sizeof(opts->filepath) - 1);
                    opts->filepath[sizeof(opts->filepath) - 1] = '\0';
                } else if (strcmp(arg, "--shuffle") == 0 || strcmp(arg, "--random") == 0 ||
                           strcmp(arg, "-s") == 0) {
                    opts->slideshow_random = true;
                    opts->slideshow_sort = WAYWAL_SORT_RANDOM;
                    opts->has_slideshow_sort = true;
                } else if (arg[0] == '-') {
                    if (!parse_common_flag(&optind, argc, argv, opts)) {
                        fprintf(stderr, "Error: Unknown option '%s'\n", arg);
                        return false;
                    }
                } else if (opts->filepath[0] == '\0') {
                    strncpy(opts->filepath, arg, sizeof(opts->filepath) - 1);
                    opts->filepath[sizeof(opts->filepath) - 1] = '\0';
                } else {
                    fprintf(stderr, "Error: Unknown option '%s'\n", arg);
                    return false;
                }
                optind++;
            }
            if (opts->filepath[0] == '\0') {
                fprintf(stderr, "Error: 'slideshow' requires a directory path\n");
                return false;
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
    } else if (strcmp(cmd_str, "pause") == 0 || strcmp(cmd_str, "unpause") == 0 ||
               strcmp(cmd_str, "toggle") == 0 || strcmp(cmd_str, "kill") == 0) {
        if (strcmp(cmd_str, "pause") == 0) {
            opts->cmd = CLI_CMD_PAUSE;
        } else if (strcmp(cmd_str, "unpause") == 0) {
            opts->cmd = CLI_CMD_UNPAUSE;
        } else if (strcmp(cmd_str, "toggle") == 0) {
            opts->cmd = CLI_CMD_TOGGLE;
        } else {
            opts->cmd = CLI_CMD_KILL;
        }
        while (optind < argc) {
            if (!parse_common_flag(&optind, argc, argv, opts)) {
                fprintf(stderr, "Error: Unknown option '%s'\n", argv[optind]);
                return false;
            }
            optind++;
        }
    } else if (strcmp(cmd_str, "help") == 0) {
        opts->cmd = CLI_CMD_HELP;
    } else if (strcmp(cmd_str, "version") == 0) {
        opts->cmd = CLI_CMD_VERSION;
    } else {
        char preset_query[4200];
        if (strncasecmp(cmd_str, "preset:", 7) == 0) {
            snprintf(preset_query, sizeof(preset_query), "%s", cmd_str);
        } else {
            snprintf(preset_query, sizeof(preset_query), "preset:%s", cmd_str);
        }
        char resolved_preset[4096];
        if (strchr(cmd_str, '/') == NULL && strchr(cmd_str, '\\') == NULL &&
            waywal_shader_resolve_path(preset_query, resolved_preset, sizeof(resolved_preset))) {
            opts->cmd = CLI_CMD_SHADER;
            opts->shader_preset_explicit = true;
            strncpy(opts->shader_preset, cmd_str, sizeof(opts->shader_preset) - 1);
            opts->shader_preset[sizeof(opts->shader_preset) - 1] = '\0';
            strncpy(opts->filepath, resolved_preset, sizeof(opts->filepath) - 1);
            opts->filepath[sizeof(opts->filepath) - 1] = '\0';
            strncpy(opts->custom_shader_path, resolved_preset,
                    sizeof(opts->custom_shader_path) - 1);
            opts->custom_shader_path[sizeof(opts->custom_shader_path) - 1] = '\0';
            if (waywal_shader_is_audio_reactive(preset_query) ||
                waywal_shader_is_audio_reactive(resolved_preset)) {
                opts->audio_reactive = true;
            }
            while (optind < argc) {
                if (!parse_common_flag(&optind, argc, argv, opts)) {
                    fprintf(stderr, "Error: Unknown shader option '%s'\n", argv[optind]);
                    return false;
                }
                optind++;
            }
        } else {
            fprintf(stderr, "Error: Unknown command '%s'\n", cmd_str);
            return false;
        }
    }

    if (opts->invert_y && !opts->pos_y_inverted && opts->transition_pos_y >= 0.0f &&
        opts->transition_pos_y <= 1.0f) {
        opts->transition_pos_y = 1.0f - opts->transition_pos_y;
        opts->pos_y_inverted = true;
    }

    return true;
}

static bool dir_contains_workshop_marker(const char *dir)
{
    char probe[4096];
    struct stat st;
    if (snprintf(probe, sizeof(probe), "%s/project.json", dir) < (int)sizeof(probe) &&
        stat(probe, &st) == 0 && S_ISREG(st.st_mode)) {
        return true;
    }
    if (snprintf(probe, sizeof(probe), "%s/scene.pkg", dir) < (int)sizeof(probe) &&
        stat(probe, &st) == 0 && S_ISREG(st.st_mode)) {
        return true;
    }
    if (snprintf(probe, sizeof(probe), "%s/gifscene.pkg", dir) < (int)sizeof(probe) &&
        stat(probe, &st) == 0 && S_ISREG(st.st_mode)) {
        return true;
    }
    return false;
}

static bool is_workshop_marker_or_preview_name(const char *filename)
{
    if (!filename || filename[0] == '\0') {
        return false;
    }
    if (strcasecmp(filename, "project.json") == 0 || strcasecmp(filename, "scene.pkg") == 0 ||
        strcasecmp(filename, "gifscene.pkg") == 0) {
        return true;
    }
    if (strncasecmp(filename, "preview.", 8) == 0) {
        return true;
    }
    return false;
}

bool cli_auto_dispatch_img(cli_options_t *opts)
{
    if (!opts || opts->cmd != CLI_CMD_IMG || opts->read_stdin || opts->filepath[0] == '\0') {
        return false;
    }

    if (opts->is_solid_color ||
        (access(opts->filepath, F_OK) != 0 && parse_hex_color(opts->filepath, &opts->clear_color))) {
        opts->is_solid_color = true;
        if (opts->transition_type == WAYWAL_TRANSITION_NONE || opts->transition_duration_ms == 0) {
            opts->cmd = CLI_CMD_COLOR;
        } else {
            opts->cmd = CLI_CMD_IMG;
        }
        return true;
    }

    struct stat st;
    bool has_stat = (stat(opts->filepath, &st) == 0);

    struct stat lst;
    bool is_symlink = (lstat(opts->filepath, &lst) == 0 && S_ISLNK(lst.st_mode));
    char real_buf[4096];
    const char *resolved = realpath(opts->filepath, real_buf) ? real_buf : opts->filepath;

    if (has_stat && S_ISDIR(st.st_mode)) {
        if (dir_contains_workshop_marker(opts->filepath)) {
            opts->cmd = CLI_CMD_WORKSHOP;
            opts->workshop_subcmd = WAYWAL_WORKSHOP_APPLY;
            return true;
        }
        if (dir_contains_workshop_marker(resolved)) {
            opts->cmd = CLI_CMD_WORKSHOP;
            opts->workshop_subcmd = WAYWAL_WORKSHOP_APPLY;
            strncpy(opts->filepath, resolved, sizeof(opts->filepath) - 1);
            opts->filepath[sizeof(opts->filepath) - 1] = '\0';
            return true;
        }
        opts->cmd = CLI_CMD_SLIDESHOW;
        return true;
    }

    if (!has_stat || S_ISREG(st.st_mode)) {
        const char *orig_base = strrchr(opts->filepath, '/');
        orig_base = orig_base ? (orig_base + 1) : opts->filepath;
        const char *res_base = strrchr(resolved, '/');
        res_base = res_base ? (res_base + 1) : resolved;

        if (has_stat) {
            char ws_dir[4096];
            if (we_workshop_resolve_dir(opts->filepath, ws_dir, sizeof(ws_dir))) {
                bool is_ws_entry = is_workshop_marker_or_preview_name(orig_base) ||
                                   is_workshop_marker_or_preview_name(res_base);
                if (!is_ws_entry) {
                    char prev_file[4096];
                    if (we_workshop_find_preview_file(ws_dir, prev_file, sizeof(prev_file))) {
                        const char *prev_base = strrchr(prev_file, '/');
                        prev_base = prev_base ? (prev_base + 1) : prev_file;
                        if (strcasecmp(orig_base, prev_base) == 0 ||
                            strcasecmp(res_base, prev_base) == 0) {
                            is_ws_entry = true;
                        }
                    }
                }
                if (is_ws_entry) {
                    opts->cmd = CLI_CMD_WORKSHOP;
                    opts->workshop_subcmd = WAYWAL_WORKSHOP_APPLY;
                    strncpy(opts->filepath, ws_dir, sizeof(opts->filepath) - 1);
                    opts->filepath[sizeof(opts->filepath) - 1] = '\0';
                    return true;
                }
            }
        }

        const char *dots[2] = {strrchr(orig_base, '.'), strrchr(res_base, '.')};
        for (int di = 0; di < 2; ++di) {
            const char *dot = dots[di];
            if (!dot)
                continue;
            if (strcasecmp(dot, ".mp4") == 0 || strcasecmp(dot, ".webm") == 0 ||
                strcasecmp(dot, ".mkv") == 0 || strcasecmp(dot, ".mov") == 0 ||
                strcasecmp(dot, ".avi") == 0 || strcasecmp(dot, ".m4v") == 0) {
                opts->cmd = CLI_CMD_VIDEO;
                if (di == 1 && is_symlink) {
                    strncpy(opts->filepath, resolved, sizeof(opts->filepath) - 1);
                    opts->filepath[sizeof(opts->filepath) - 1] = '\0';
                }
                return true;
            }
            if (strcasecmp(dot, ".frag") == 0 || strcasecmp(dot, ".glsl") == 0 ||
                strcasecmp(dot, ".comp") == 0 || strcasecmp(dot, ".shader") == 0) {
                opts->cmd = CLI_CMD_SHADER;
                if (strncasecmp(opts->filepath, "preset:", 7) == 0) {
                    opts->shader_preset_explicit = true;
                }
                char resolved_preset[4096];
                const char *sh_p = (di == 1 && is_symlink) ? resolved : opts->filepath;
                if (!has_stat &&
                    waywal_shader_resolve_path(opts->filepath, resolved_preset,
                                               sizeof(resolved_preset))) {
                    sh_p = resolved_preset;
                }
                if (waywal_shader_is_audio_reactive(opts->filepath) ||
                    (sh_p != opts->filepath && waywal_shader_is_audio_reactive(sh_p))) {
                    opts->audio_reactive = true;
                }
                if (sh_p != opts->filepath) {
                    strncpy(opts->filepath, sh_p, sizeof(opts->filepath) - 1);
                    opts->filepath[sizeof(opts->filepath) - 1] = '\0';
                }
                strncpy(opts->custom_shader_path, opts->filepath,
                        sizeof(opts->custom_shader_path) - 1);
                opts->custom_shader_path[sizeof(opts->custom_shader_path) - 1] = '\0';
                return true;
            }
            if (strcasecmp(dot, ".xml") == 0 || strcasecmp(dot, ".json") == 0) {
                if (di == 1 && is_symlink) {
                    strncpy(opts->filepath, resolved, sizeof(opts->filepath) - 1);
                    opts->filepath[sizeof(opts->filepath) - 1] = '\0';
                }
                return true;
            }
        }

        if (!has_stat) {
            char resolved_preset[4096];
            if (waywal_shader_resolve_path(opts->filepath, resolved_preset,
                                           sizeof(resolved_preset))) {
                opts->cmd = CLI_CMD_SHADER;
                if (strncasecmp(opts->filepath, "preset:", 7) == 0) {
                    opts->shader_preset_explicit = true;
                }
                if (waywal_shader_is_audio_reactive(opts->filepath) ||
                    waywal_shader_is_audio_reactive(resolved_preset)) {
                    opts->audio_reactive = true;
                }
                strncpy(opts->filepath, resolved_preset, sizeof(opts->filepath) - 1);
                opts->filepath[sizeof(opts->filepath) - 1] = '\0';
                strncpy(opts->custom_shader_path, resolved_preset,
                        sizeof(opts->custom_shader_path) - 1);
                opts->custom_shader_path[sizeof(opts->custom_shader_path) - 1] = '\0';
                return true;
            }
            if (strncasecmp(opts->filepath, "preset:", 7) == 0) {
                opts->cmd = CLI_CMD_SHADER;
                opts->shader_preset_explicit = true;
                return true;
            }
        }
    }

    return false;
}

bool waywal_cli_parse(int argc, char *argv[], waywal_cli_opts_t *opts)
{
    return cli_parse(argc, argv, opts);
}
