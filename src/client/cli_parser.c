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
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static void cli_copy_cstr(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    if (dst == src) {
        dst[cap - 1] = '\0';
        return;
    }
    size_t i = 0;
    while (i + 1 < cap && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Reject silent truncation on CLI path and identifier buffers so malformed
 * arguments fail early instead of forwarding cut-off paths to the daemon. */
static bool cli_copy_bounded(char *dst, size_t cap, const char *src, const char *opt_name,
                             bool allow_empty)
{
    if (!dst || cap == 0 || !src) {
        fprintf(stderr, "Error: Missing value for %s\n", opt_name ? opt_name : "argument");
        return false;
    }
    size_t len = strlen(src);
    if (!allow_empty && len == 0) {
        fprintf(stderr, "Error: Empty value for %s\n", opt_name ? opt_name : "argument");
        return false;
    }
    if (len >= cap) {
        fprintf(stderr, "Error: Value for %s exceeds maximum length (%zu bytes)\n",
                opt_name ? opt_name : "argument", cap - 1U);
        return false;
    }
    memcpy(dst, src, len + 1U);
    return true;
}

static bool cli_parse_i64_range(const char *val, long long min_v, long long max_v,
                                long long *out_v)
{
    if (!val || val[0] == '\0' || !out_v) {
        return false;
    }
    if (isspace((unsigned char)val[0])) {
        return false;
    }
    char *endp = NULL;
    errno = 0;
    long long v = strtoll(val, &endp, 10);
    if (errno != 0 || !endp || endp == val || *endp != '\0' || v < min_v || v > max_v) {
        return false;
    }
    *out_v = v;
    return true;
}

static bool cli_parse_u64_strict(const char *val, uint64_t *out_v)
{
    if (!val || val[0] == '\0' || val[0] == '-' || isspace((unsigned char)val[0]) || !out_v) {
        return false;
    }
    char *endp = NULL;
    errno = 0;
    unsigned long long v = strtoull(val, &endp, 10);
    if (errno != 0 || !endp || endp == val || *endp != '\0') {
        return false;
    }
    *out_v = (uint64_t)v;
    return true;
}

static bool cli_parse_float_strict(const char *val, float *out_v)
{
    if (!val || val[0] == '\0' || isspace((unsigned char)val[0]) || !out_v) {
        return false;
    }
    char *endp = NULL;
    errno = 0;
    float v = strtof(val, &endp);
    /* Permit subnormal underflow clamping to 0.0f while rejecting overflow. */
    if ((errno == ERANGE && fabsf(v) > 1.0f) || !endp || endp == val || *endp != '\0' ||
        !isfinite(v)) {
        return false;
    }
    *out_v = v;
    return true;
}

static bool cli_parse_float_list(const char *val, size_t expected_count, float *out_vals)
{
    if (!val || val[0] == '\0' || !out_vals || expected_count == 0) {
        return false;
    }
    const char *p = val;
    for (size_t i = 0; i < expected_count; ++i) {
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            return false;
        }
        char *endp = NULL;
        errno = 0;
        float v = strtof(p, &endp);
        if ((errno == ERANGE && fabsf(v) > 1.0f) || !endp || endp == p || !isfinite(v)) {
            return false;
        }
        out_vals[i] = v;
        p = endp;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if (i + 1U < expected_count) {
            if (*p != ',') {
                return false;
            }
            p++;
        } else if (*p != '\0') {
            return false;
        }
    }
    return true;
}

static bool cli_is_valid_pixel_format(const char *val)
{
    if (!val || val[0] == '\0') {
        return false;
    }
    static const char *const k_formats[] = {
        "xrgb", "argb", "xbgr", "rgb", "bgr", "xrgb8888", "argb8888", "xbgr8888", "rgb888",
        "bgr888",
    };
    for (size_t i = 0; i < sizeof(k_formats) / sizeof(k_formats[0]); ++i) {
        if (strcasecmp(val, k_formats[i]) == 0) {
            return true;
        }
    }
    return false;
}

static uint32_t cli_fnv1a_32_n(const char *s, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

void cli_print_usage(const char *prog)
{
    printf("wwal - Modern high-performance Wayland wallpaper client (C23)\n");
    printf("Usage: %s [GLOBAL_OPTIONS] <COMMAND> [ARGS...]\n\n", prog);
    printf("Commands:\n");
    printf("  ping                       Check if wwald daemon is running and responsive\n");
    printf("  query                      List all detected Wayland outputs and current geometry\n");
    printf("  clear, color [COLOR] [OPT] Clear wallpaper to solid hex color (e.g. 000000)\n");
    printf("  restore                    Restore last saved wallpaper state\n");
    printf("  clear-cache                Clear cached transcoded media and palettes\n");
    printf("  init, daemon               Launch or initialize wallpaper daemon (swww/awww "
           "alias)\n");
    printf("  img <PATH|-> [OPTIONS]     Load and set wallpaper with GPU/SIMD transitions\n");
    printf("  pick [OPTIONS]             Launch unified icon-grid wallpaper/Workshop/shader "
           "picker\n");
    printf("  shader <PATH|PRESET>       Run live GLSL fragment shader or built-in preset\n");
    printf("  shader list [--json]       List built-in GLSL shader presets\n");
    printf("  shader preset <NAME>       Run built-in GLSL shader preset by name\n");
    printf("  workshop list              List installed Steam Workshop (431960) wallpapers\n");
    printf("  workshop info <ID|PATH>    Inspect Workshop project.json, scene.pkg, and "
           "properties\n");
    printf("  workshop sync-previews [D] Sync Workshop preview symlinks for external GUI "
           "pickers\n");
    printf("  workshop [apply] <ID|PATH> Render native Wallpaper Engine scene or video\n");
    printf("  theme [PATH] [OPTIONS]     Extract HCT Material You theme palette\n");
    printf("  stats [--json]             Display daemon telemetry and frame timing statistics\n");
    printf("  status [--json]            Inspect outputs, active media, MPRIS, and telemetry\n");
    printf("  set-uniform, uniform <N> V Mutate live shader uniform or Workshop property\n");
    printf("  subscribe                  Stream newline-delimited JSON state change events\n");
    printf("  audio [OPTIONS]            Update live audio volume and auto-mute ducking\n");
    printf("  slideshow [start] <DIR>    Start zero-idle-CPU timerfd wallpaper slideshow\n");
    printf("  slideshow stop|pause|resume|toggle|next|prev\n");
    printf("                             Control active wallpaper slideshow\n");
    printf("  next, prev, previous       Advance or rewind active slideshow directly\n");
    printf("  video <PATH> [OPTIONS]     Play hardware-accelerated video wallpaper (VA-API)\n");
    printf("  pause                      Pause video playback\n");
    printf("  unpause, resume            Resume video playback\n");
    printf("  toggle                     Toggle video playback pause/resume\n");
    printf("  kill                       Gracefully terminate the running wwald daemon\n");
    printf("  version                    Print version information\n");
    printf("  help                       Display this help message\n\n");
    printf("Picker & Slideshow Options:\n");
    printf("  --menu <MODE>              Picker backend: auto, rofi, fuzzel, walker, dmenu,\n");
    printf("                             stdout, or json\n");
    printf("  --dir, --path <DIR>        Directory path for picker or slideshow\n");
    printf("  -d, --interval, --duration <S>\n");
    printf("                             Seconds between wallpaper changes (default: 300)\n");
    printf("  -s, --shuffle, --random    Shuffle images into random playback order\n");
    printf("  --sort, --sorting <MODE>   Sort order: ascending, descending, mtime, random\n");
    printf("  --recursive / --no-recursive Enable or disable recursive directory scan\n\n");
    printf("Image, Shader & Transition Options:\n");
    printf("  --shader <PATH|PRESET>     Apply live GLSL fragment shader alongside img/command\n");
    printf("  --transition-type, -t <T>  none, simple, fade, wipe, grow, outer, wave, noise,\n");
    printf("                             crosszoom, slide, glitch, burn, ripple, pixelate,\n");
    printf("                             doom, swirl, cube, luma, light_leak, page_curl, custom,\n");
    printf("                             random, left, right, top, bottom, center, any\n");
    printf("  --transition-duration <S>  Duration in seconds (e.g. 1.2, 0.5) (default: 1.2)\n");
    printf("  --transition-step <N>      Transition step speed (swww/awww compatibility)\n");
    printf("  --transition-fps <FPS>     Target frame rate (default: 60)\n");
    printf("  --transition-angle <DEG>   Wipe/wave/slide angle in degrees (default: 0)\n");
    printf("  --transition-wave <F,A>    Wave frequency and amplitude (default: 20,0.05)\n");
    printf("  --transition-pos <POS>     Transition anchor: cursor, center, top, bottom, left,\n");
    printf("                             right, top-left (tl), top-right (tr), bottom-left (bl),\n");
    printf("                             bottom-right (br), or <X,Y> coordinates\n");
    printf("  --invert-y                 Invert Y coordinate of --transition-pos\n");
    printf("  --transition-bezier <C>    Cubic bezier control points X1,Y1,X2,Y2\n");
    printf("  --fill-color <HEX>         Background fill color for fit/center scaling\n");
    printf("  --resize <MODE>            swww resize mode: crop, fit, no, stretch\n");
    printf("  --no-resize                Center image without scaling (equivalent to --resize "
           "no)\n");
    printf("  --crop-gravity <GRAVITY>   Crop anchor for fill/crop: center, top, bottom, left,\n");
    printf("                             right, top-left, top-right, bottom-left, bottom-right,\n");
    printf("                             or <X,Y> normalized/percentage coordinates\n");
    printf("  -f, --filter <FILTER>      Nearest, Bilinear, CatmullRom, Mitchell, Lanczos3\n");
    printf("  -f, --format <FMT>         Pixel format override (xrgb, argb, xbgr, rgb, bgr)\n");
    printf("  --no-cache                 Bypass image/frame cache for this operation\n");
    printf("  --clear-cache              Clear cached media before applying\n");
    printf("  --transition-shader <FILE> Path to GLSL compute shader (*.comp) for custom "
           "transitions\n");
    printf("  -o, --output, --outputs <N> Target specific output(s) (comma-separated)\n");
    printf("  --sync-mode <MODE>         Multi-monitor sync: simultaneous or staggered\n");
    printf("  --stagger-delay <MS>       Delay in milliseconds between cascading monitors\n");
    printf("  --10bit                    Enable 10-bit wide-gamut scanout if supported\n");
    printf("  --scaling-mode, --mode <M> Scaling mode: fill, fit, stretch, center, tile, span;\n");
    printf("                             or theme/solar mode: light, dark, auto\n");
    printf("  --animate                  Animate GIF wallpaper using hardware video engine\n");
    printf("  --theme / --no-theme       Enable or disable automatic theme extraction\n");
    printf("  --theme-from-media         Extract theme dynamically from MPRIS2 album art\n");
    printf("  --no-theme-from-media      Disable MPRIS2 album art theme extraction\n");
    printf("  --theme-mode <dark|light|auto>\n");
    printf("                             Select dark, light, or solar auto Material You mode\n");
    printf("  --light / --dark           Shorthand for --theme-mode light / dark\n");
    printf("  --auto-day-night / --no-auto-day-night\n");
    printf("                             Toggle solar day/night theme and manifest evaluation\n");
    printf("  --latitude <DEG>           Solar latitude in degrees (-90.0..90.0)\n");
    printf("  --longitude <DEG>          Solar longitude in degrees (-180.0..180.0)\n");
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
    printf("Daemon & Global Options:\n");
    printf("  -a, --all                  Send command to all active daemon namespaces\n");
    printf("  -d, --daemon               Fork wwald into background (init/daemon subcommand)\n");
    printf("  --no-daemon                Stay in foreground (daemon) or skip auto-spawn\n");
    printf("  -j, --json                 Format query/status/stats/pick output as JSON\n");
    printf("  -n, --namespace <NAME>     Socket namespace (default: \"default\")\n");
    printf("  -q, --quiet                Suppress informational logs (daemon)\n");
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

static bool cli_parse_flag_add_outputs(cli_options_t *opts, const char *arg, bool strict_len)
{
    if (!opts || !arg || arg[0] == '\0') {
        return false;
    }
    const char *p = arg;
    bool added_any = false;
    while (*p != '\0') {
        const char *comma = strchr(p, ',');
        size_t span = comma ? (size_t)(comma - p) : strlen(p);
        size_t start = 0;
        while (start < span && p[start] == ' ') {
            start++;
        }
        size_t end = span;
        while (end > start && p[end - 1] == ' ') {
            end--;
        }
        size_t len = end - start;
        if (len > 0) {
            if (len >= sizeof(opts->outputs[0])) {
                if (strict_len) {
                    fprintf(stderr, "Error: Output name exceeds maximum length (%zu bytes)\n",
                            sizeof(opts->outputs[0]) - 1U);
                    return false;
                }
            } else if (opts->num_outputs < 16U) {
                memcpy(opts->outputs[opts->num_outputs], p + start, len);
                opts->outputs[opts->num_outputs][len] = '\0';
                if (opts->output[0] == '\0') {
                    memcpy(opts->output, p + start, len);
                    opts->output[len] = '\0';
                }
                opts->num_outputs++;
                added_any = true;
            }
        }
        if (!comma) {
            break;
        }
        p = comma + 1;
    }
    return strict_len ? added_any : true;
}

static void cli_recompute_step_duration(cli_options_t *opts)
{
    if (!opts || opts->transition_step == 0 || opts->has_transition_duration) {
        return;
    }
    uint32_t fps = opts->transition_fps > 0 ? opts->transition_fps : 60u;
    uint32_t frames = (255u + opts->transition_step - 1u) / opts->transition_step;
    uint32_t approx_ms = (frames * 1000u) / fps;
    if (approx_ms < 100u) {
        approx_ms = 100u;
    }
    if (approx_ms > 10000u) {
        approx_ms = 10000u;
    }
    opts->transition_duration_ms = approx_ms;
    opts->transition_duration = (float)approx_ms / 1000.0f;
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

typedef enum {
    CLI_FLAG_NONE = 0,
    CLI_FLAG_HELP,
    CLI_FLAG_VERSION,
    CLI_FLAG_VERBOSE,
    CLI_FLAG_QUIET,
    CLI_FLAG_DAEMON,
    CLI_FLAG_NO_DAEMON,
    CLI_FLAG_ALL,
    CLI_FLAG_INVERT_Y,
    CLI_FLAG_NO_CACHE,
    CLI_FLAG_CLEAR_CACHE,
    CLI_FLAG_NAMESPACE,
    CLI_FLAG_FORMAT,
    CLI_FLAG_CROP_GRAVITY,
    CLI_FLAG_TRANSITION_TYPE,
    CLI_FLAG_TRANSITION_DURATION,
    CLI_FLAG_TRANSITION_STEP,
    CLI_FLAG_TRANSITION_FPS,
    CLI_FLAG_TRANSITION_ANGLE,
    CLI_FLAG_TRANSITION_WAVE,
    CLI_FLAG_TRANSITION_POS,
    CLI_FLAG_TRANSITION_BEZIER,
    CLI_FLAG_FILL_COLOR,
    CLI_FLAG_TRANSITION_SHADER,
    CLI_FLAG_SHADER,
    CLI_FLAG_DEPTH_MAP,
    CLI_FLAG_OUTPUT,
    CLI_FLAG_SYNC_MODE,
    CLI_FLAG_STAGGER_DELAY,
    CLI_FLAG_10BIT,
    CLI_FLAG_NO_RESIZE,
    CLI_FLAG_RESIZE,
    CLI_FLAG_MODE,
    CLI_FLAG_FILTER,
    CLI_FLAG_F,
    CLI_FLAG_ANIMATE,
    CLI_FLAG_AUDIO,
    CLI_FLAG_THEME,
    CLI_FLAG_NO_THEME,
    CLI_FLAG_LIGHT,
    CLI_FLAG_DARK,
    CLI_FLAG_AUTO_DAY_NIGHT,
    CLI_FLAG_NO_AUTO_DAY_NIGHT,
    CLI_FLAG_THEME_MODE,
    CLI_FLAG_LATITUDE,
    CLI_FLAG_LONGITUDE,
    CLI_FLAG_SOURCE_COLOR_INDEX,
    CLI_FLAG_SEED_COLOR,
    CLI_FLAG_BACKDROP,
    CLI_FLAG_LAYER,
    CLI_FLAG_BATTERY_POLICY,
    CLI_FLAG_DEPTH_STRENGTH,
    CLI_FLAG_PROP,
    CLI_FLAG_JSON,
    CLI_FLAG_VOLUME,
    CLI_FLAG_MUTE_ON_AUDIO,
    CLI_FLAG_NO_MUTE_ON_AUDIO,
    CLI_FLAG_THEME_FROM_MEDIA,
    CLI_FLAG_NO_THEME_FROM_MEDIA,
    CLI_FLAG_LOOP,
    CLI_FLAG_SPEED,
    CLI_FLAG_SORT,
    CLI_FLAG_DURATION,
    CLI_FLAG_SHUFFLE,
    CLI_FLAG_RECURSIVE,
    CLI_FLAG_NO_RECURSIVE,
} cli_flag_id_t;

typedef struct {
    const char *name;
    cli_flag_id_t id;
    bool takes_val;
} cli_flag_spec_t;

typedef struct {
    const cli_flag_spec_t *spec;
    uint32_t hash;
} cli_flag_slot_t;

#define CLI_FLAG_TABLE_SLOTS 256U

static cli_flag_slot_t g_flag_table[CLI_FLAG_TABLE_SLOTS];
static atomic_int g_flag_table_state = 0;

static const cli_flag_spec_t k_common_flags[] = {
    {"-h", CLI_FLAG_HELP, false},
    {"--help", CLI_FLAG_HELP, false},
    {"-V", CLI_FLAG_VERSION, false},
    {"--version", CLI_FLAG_VERSION, false},
    {"-v", CLI_FLAG_VERBOSE, false},
    {"--verbose", CLI_FLAG_VERBOSE, false},
    {"-q", CLI_FLAG_QUIET, false},
    {"--quiet", CLI_FLAG_QUIET, false},
    {"-d", CLI_FLAG_DAEMON, false},
    {"--daemon", CLI_FLAG_DAEMON, false},
    {"--no-daemon", CLI_FLAG_NO_DAEMON, false},
    {"-a", CLI_FLAG_ALL, false},
    {"--all", CLI_FLAG_ALL, false},
    {"--invert-y", CLI_FLAG_INVERT_Y, false},
    {"--no-cache", CLI_FLAG_NO_CACHE, false},
    {"--clear-cache", CLI_FLAG_CLEAR_CACHE, false},
    {"-n", CLI_FLAG_NAMESPACE, true},
    {"--namespace", CLI_FLAG_NAMESPACE, true},
    {"--format", CLI_FLAG_FORMAT, true},
    {"--crop-gravity", CLI_FLAG_CROP_GRAVITY, true},
    {"-t", CLI_FLAG_TRANSITION_TYPE, true},
    {"--transition-type", CLI_FLAG_TRANSITION_TYPE, true},
    {"--transition-duration", CLI_FLAG_TRANSITION_DURATION, true},
    {"--transition-step", CLI_FLAG_TRANSITION_STEP, true},
    {"--transition-fps", CLI_FLAG_TRANSITION_FPS, true},
    {"--fps", CLI_FLAG_TRANSITION_FPS, true},
    {"--transition-angle", CLI_FLAG_TRANSITION_ANGLE, true},
    {"--transition-wave", CLI_FLAG_TRANSITION_WAVE, true},
    {"--transition-pos", CLI_FLAG_TRANSITION_POS, true},
    {"--transition-bezier", CLI_FLAG_TRANSITION_BEZIER, true},
    {"--fill-color", CLI_FLAG_FILL_COLOR, true},
    {"--transition-shader", CLI_FLAG_TRANSITION_SHADER, true},
    {"--shader", CLI_FLAG_SHADER, true},
    {"--depth-map", CLI_FLAG_DEPTH_MAP, true},
    {"-o", CLI_FLAG_OUTPUT, true},
    {"--output", CLI_FLAG_OUTPUT, true},
    {"--outputs", CLI_FLAG_OUTPUT, true},
    {"--sync-mode", CLI_FLAG_SYNC_MODE, true},
    {"--stagger-delay", CLI_FLAG_STAGGER_DELAY, true},
    {"--10bit", CLI_FLAG_10BIT, false},
    {"--no-resize", CLI_FLAG_NO_RESIZE, false},
    {"--resize", CLI_FLAG_RESIZE, true},
    {"--scaling-mode", CLI_FLAG_RESIZE, true},
    {"--mode", CLI_FLAG_MODE, true},
    {"--filter", CLI_FLAG_FILTER, true},
    {"-f", CLI_FLAG_F, true},
    {"--animate", CLI_FLAG_ANIMATE, false},
    {"--audio", CLI_FLAG_AUDIO, false},
    {"--theme", CLI_FLAG_THEME, false},
    {"--no-theme", CLI_FLAG_NO_THEME, false},
    {"--light", CLI_FLAG_LIGHT, false},
    {"--dark", CLI_FLAG_DARK, false},
    {"--auto-day-night", CLI_FLAG_AUTO_DAY_NIGHT, false},
    {"--no-auto-day-night", CLI_FLAG_NO_AUTO_DAY_NIGHT, false},
    {"--theme-mode", CLI_FLAG_THEME_MODE, true},
    {"--latitude", CLI_FLAG_LATITUDE, true},
    {"--longitude", CLI_FLAG_LONGITUDE, true},
    {"--source-color-index", CLI_FLAG_SOURCE_COLOR_INDEX, true},
    {"--seed-color", CLI_FLAG_SEED_COLOR, true},
    {"--backdrop", CLI_FLAG_BACKDROP, true},
    {"--layer", CLI_FLAG_LAYER, true},
    {"--battery-policy", CLI_FLAG_BATTERY_POLICY, true},
    {"--depth-strength", CLI_FLAG_DEPTH_STRENGTH, true},
    {"-p", CLI_FLAG_PROP, true},
    {"--prop", CLI_FLAG_PROP, true},
    {"-j", CLI_FLAG_JSON, false},
    {"--json", CLI_FLAG_JSON, false},
    {"--volume", CLI_FLAG_VOLUME, true},
    {"--mute-on-audio", CLI_FLAG_MUTE_ON_AUDIO, false},
    {"--no-mute-on-audio", CLI_FLAG_NO_MUTE_ON_AUDIO, false},
    {"--theme-from-media", CLI_FLAG_THEME_FROM_MEDIA, false},
    {"--no-theme-from-media", CLI_FLAG_NO_THEME_FROM_MEDIA, false},
    {"--loop", CLI_FLAG_LOOP, true},
    {"--speed", CLI_FLAG_SPEED, true},
    {"--sort", CLI_FLAG_SORT, true},
    {"--sorting", CLI_FLAG_SORT, true},
    {"--duration", CLI_FLAG_DURATION, true},
    {"--interval", CLI_FLAG_DURATION, true},
    {"--shuffle", CLI_FLAG_SHUFFLE, false},
    {"--random", CLI_FLAG_SHUFFLE, false},
    {"--recursive", CLI_FLAG_RECURSIVE, false},
    {"--no-recursive", CLI_FLAG_NO_RECURSIVE, false},
};

static void cli_flag_table_init(void)
{
    int state = atomic_load_explicit(&g_flag_table_state, memory_order_acquire);
    if (state == 2) {
        return;
    }
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&g_flag_table_state, &expected, 1,
                                                 memory_order_acq_rel, memory_order_acquire)) {
        while (atomic_load_explicit(&g_flag_table_state, memory_order_acquire) != 2) {
        }
        return;
    }
    for (size_t i = 0; i < sizeof(k_common_flags) / sizeof(k_common_flags[0]); ++i) {
        const cli_flag_spec_t *spec = &k_common_flags[i];
        uint32_t h = cli_fnv1a_32_n(spec->name, strlen(spec->name));
        size_t idx = h & (CLI_FLAG_TABLE_SLOTS - 1U);
        while (g_flag_table[idx].spec != NULL) {
            idx = (idx + 1U) & (CLI_FLAG_TABLE_SLOTS - 1U);
        }
        g_flag_table[idx].spec = spec;
        g_flag_table[idx].hash = h;
    }
    atomic_store_explicit(&g_flag_table_state, 2, memory_order_release);
}

static const cli_flag_spec_t *cli_lookup_flag(const char *arg, const char **out_inline_val)
{
    cli_flag_table_init();
    const char *eq = strchr(arg, '=');
    size_t key_len = eq ? (size_t)(eq - arg) : strlen(arg);
    if (key_len == 0) {
        return NULL;
    }
    uint32_t h = cli_fnv1a_32_n(arg, key_len);
    size_t idx = h & (CLI_FLAG_TABLE_SLOTS - 1U);
    while (g_flag_table[idx].spec != NULL) {
        const cli_flag_spec_t *spec = g_flag_table[idx].spec;
        if (g_flag_table[idx].hash == h && strncmp(spec->name, arg, key_len) == 0 &&
            spec->name[key_len] == '\0') {
            if (eq && !spec->takes_val) {
                return NULL;
            }
            *out_inline_val = eq ? (eq + 1) : NULL;
            return spec;
        }
        idx = (idx + 1U) & (CLI_FLAG_TABLE_SLOTS - 1U);
    }
    return NULL;
}

static bool cli_is_valid_backdrop_value(const char *val)
{
    if (!val || val[0] == '\0') {
        return false;
    }
    bool all_ws = true;
    for (const char *p = val; *p != '\0'; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
        if (!isspace(c)) {
            all_ws = false;
        }
    }
    if (all_ws) {
        return false;
    }
    if (strcasecmp(val, "none") == 0 || strcasecmp(val, "auto-blur") == 0 ||
        strcasecmp(val, "auto_blur") == 0) {
        return true;
    }
    /* Non-keyword backdrop values must resemble a path or existing file so typos
     * like '--backdrop blur' fail early instead of being treated as literal paths. */
    if (strchr(val, '/') != NULL || strchr(val, '.') != NULL || val[0] == '~' ||
        access(val, R_OK) == 0) {
        return true;
    }
    return false;
}

static bool parse_common_flag(int *optind_ptr, int argc, char *argv[], cli_options_t *opts)
{
    const char *arg = argv[*optind_ptr];
    const char *inline_val = NULL;
    const cli_flag_spec_t *spec = cli_lookup_flag(arg, &inline_val);
    if (!spec) {
        return false;
    }

    const char *val = inline_val;
    if (spec->takes_val && !val) {
        if (*optind_ptr + 1 >= argc) {
            return false;
        }
        (*optind_ptr)++;
        val = argv[*optind_ptr];
    }

    switch (spec->id) {
    case CLI_FLAG_HELP:
        opts->cmd = CLI_CMD_HELP;
        return true;
    case CLI_FLAG_VERSION:
        opts->cmd = CLI_CMD_VERSION;
        return true;
    case CLI_FLAG_VERBOSE:
        opts->verbose = true;
        return true;
    case CLI_FLAG_QUIET:
        opts->quiet = true;
        return true;
    case CLI_FLAG_DAEMON:
        opts->daemonize = true;
        opts->no_daemon = false;
        return true;
    case CLI_FLAG_NO_DAEMON:
        opts->no_daemon = true;
        opts->daemonize = false;
        return true;
    case CLI_FLAG_ALL:
        opts->all_namespaces = true;
        return true;
    case CLI_FLAG_INVERT_Y:
        opts->invert_y = true;
        return true;
    case CLI_FLAG_NO_CACHE:
        opts->no_cache = true;
        return true;
    case CLI_FLAG_CLEAR_CACHE:
        opts->clear_cache = true;
        return true;
    case CLI_FLAG_NAMESPACE:
        return cli_copy_bounded(opts->namespace_str, sizeof(opts->namespace_str), val,
                                spec->name, false);
    case CLI_FLAG_FORMAT:
        if (!cli_is_valid_pixel_format(val)) {
            fprintf(stderr, "Error: Invalid --format '%s' (expected xrgb, argb, xbgr, rgb, bgr)\n",
                    val ? val : "");
            return false;
        }
        return cli_copy_bounded(opts->pixel_format, sizeof(opts->pixel_format), val, spec->name,
                                false);
    case CLI_FLAG_CROP_GRAVITY:
        if (!waywal_parse_crop_gravity(val, &opts->crop_gravity_x, &opts->crop_gravity_y)) {
            fprintf(stderr, "Error: Invalid --crop-gravity value '%s'\n", val ? val : "");
            return false;
        }
        opts->has_crop_gravity = true;
        return true;
    case CLI_FLAG_TRANSITION_TYPE:
        if (!val || val[0] == '\0') {
            fprintf(stderr, "Error: Empty %s value\n", spec->name);
            return false;
        }
        apply_transition_type(opts, val);
        opts->has_transition_type = true;
        return true;
    case CLI_FLAG_TRANSITION_DURATION: {
        float dur_s = 0.0f;
        if (!cli_parse_float_strict(val, &dur_s) || dur_s < 0.0f || dur_s > 3600000.0f) {
            fprintf(stderr, "Error: Invalid --transition-duration value '%s'\n", val ? val : "");
            return false;
        }
        opts->transition_duration = dur_s;
        opts->transition_duration_ms = (uint32_t)(dur_s * 1000.0f);
        opts->has_transition_duration = true;
        return true;
    }
    case CLI_FLAG_TRANSITION_STEP: {
        long long step = 0;
        if (!cli_parse_i64_range(val, 1, 1000000, &step)) {
            fprintf(stderr, "Error: Invalid --transition-step value '%s'\n", val ? val : "");
            return false;
        }
        opts->transition_step = (uint32_t)step;
        cli_recompute_step_duration(opts);
        return true;
    }
    case CLI_FLAG_TRANSITION_FPS: {
        long long fps = 0;
        if (!cli_parse_i64_range(val, 1, 10000, &fps)) {
            fprintf(stderr, "Error: Invalid %s value '%s'\n", spec->name, val ? val : "");
            return false;
        }
        opts->transition_fps = (uint32_t)fps;
        opts->has_transition_fps = true;
        cli_recompute_step_duration(opts);
        return true;
    }
    case CLI_FLAG_TRANSITION_ANGLE: {
        float deg = 0.0f;
        if (!cli_parse_float_strict(val, &deg)) {
            fprintf(stderr, "Error: Invalid --transition-angle value '%s'\n", val ? val : "");
            return false;
        }
        set_angle_degrees(opts, deg);
        opts->has_transition_angle = true;
        return true;
    }
    case CLI_FLAG_TRANSITION_WAVE: {
        float wv[2] = {0.0f, 0.0f};
        if (!cli_parse_float_list(val, 2U, wv) || wv[0] < 0.0f || wv[1] < 0.0f) {
            fprintf(stderr, "Error: Invalid --transition-wave format (expected FREQ,AMP)\n");
            return false;
        }
        opts->transition_wave_freq = wv[0];
        opts->transition_wave_amp = wv[1];
        opts->has_transition_wave = true;
        return true;
    }
    case CLI_FLAG_TRANSITION_POS:
        if (!parse_transition_pos(val, &opts->transition_pos_x, &opts->transition_pos_y)) {
            fprintf(stderr, "Error: Invalid --transition-pos value '%s'\n", val ? val : "");
            return false;
        }
        opts->has_transition_pos = true;
        return true;
    case CLI_FLAG_TRANSITION_BEZIER: {
        float bz[4] = {0.0f};
        if (!cli_parse_float_list(val, 4U, bz)) {
            fprintf(stderr, "Error: Invalid --transition-bezier format (expected X1,Y1,X2,Y2)\n");
            return false;
        }
        opts->bezier = bezier_create(bz[0], bz[1], bz[2], bz[3]);
        opts->has_custom_bezier = true;
        opts->has_transition_bezier = true;
        return true;
    }
    case CLI_FLAG_FILL_COLOR:
        if (!parse_hex_color(val, &opts->clear_color)) {
            fprintf(stderr,
                    "Error: Invalid --fill-color hex format (expected RRGGBB or RRGGBBAA)\n");
            return false;
        }
        opts->has_fill_color = true;
        return true;
    case CLI_FLAG_TRANSITION_SHADER:
        if (!cli_copy_bounded(opts->custom_shader_path, sizeof(opts->custom_shader_path), val,
                              spec->name, false)) {
            return false;
        }
        opts->transition_type = WAYWAL_TRANSITION_CUSTOM;
        opts->has_transition_type = true;
        if (waywal_shader_is_audio_reactive(val)) {
            opts->audio_reactive = true;
        }
        return true;
    case CLI_FLAG_SHADER: {
        if (!val || val[0] == '\0') {
            fprintf(stderr, "Error: Empty --shader value\n");
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
        if (opts->cmd == CLI_CMD_SHADER &&
            !cli_copy_bounded(opts->filepath, sizeof(opts->filepath), effective, spec->name,
                              false)) {
            return false;
        }
        if (!cli_copy_bounded(opts->custom_shader_path, sizeof(opts->custom_shader_path), effective,
                              spec->name, false)) {
            return false;
        }
        if (waywal_shader_is_audio_reactive(val) || waywal_shader_is_audio_reactive(effective)) {
            opts->audio_reactive = true;
        }
        return true;
    }
    case CLI_FLAG_DEPTH_MAP:
        return cli_copy_bounded(opts->depth_map_path, sizeof(opts->depth_map_path), val,
                                spec->name, false);
    case CLI_FLAG_OUTPUT:
        if (opts->env_outputs_set) {
            opts->num_outputs = 0;
            opts->output[0] = '\0';
            opts->env_outputs_set = false;
        }
        return cli_parse_flag_add_outputs(opts, val, true);
    case CLI_FLAG_SYNC_MODE:
        if (strcasecmp(val, "staggered") == 0 || strcasecmp(val, "cascade") == 0) {
            opts->sync_mode = 1;
        } else if (strcasecmp(val, "simultaneous") == 0 || strcasecmp(val, "sync") == 0) {
            opts->sync_mode = 0;
        } else {
            fprintf(stderr,
                    "Error: Invalid --sync-mode '%s' (expected simultaneous or staggered)\n",
                    val ? val : "");
            return false;
        }
        opts->has_sync_mode = true;
        return true;
    case CLI_FLAG_STAGGER_DELAY: {
        long long d = 0;
        if (!cli_parse_i64_range(val, 0, 600000, &d)) {
            fprintf(stderr, "Error: Invalid --stagger-delay value '%s'\n", val ? val : "");
            return false;
        }
        opts->stagger_delay_ms = (uint32_t)d;
        opts->has_stagger_delay = true;
        return true;
    }
    case CLI_FLAG_10BIT:
        opts->enable_10bit = true;
        opts->has_10bit = true;
        return true;
    case CLI_FLAG_NO_RESIZE:
        opts->scaling = WAYWAL_SCALING_CENTER;
        opts->has_scaling_mode = true;
        return true;
    case CLI_FLAG_RESIZE:
        apply_scaling_mode(opts, val);
        opts->has_scaling_mode = true;
        return true;
    case CLI_FLAG_MODE:
        if (strcasecmp(val, "auto") == 0) {
            opts->auto_day_night = true;
            opts->has_auto_day_night = true;
            opts->has_theme_mode = false;
        } else if (opts->cmd == CLI_CMD_THEME || strcasecmp(val, "light") == 0 ||
                   strcasecmp(val, "dark") == 0) {
            if (strcasecmp(val, "light") != 0 && strcasecmp(val, "dark") != 0) {
                fprintf(stderr, "Error: Invalid theme --mode '%s' (expected dark, light, or auto)\n",
                        val);
                return false;
            }
            opts->theme_light_mode = (strcasecmp(val, "light") == 0);
            opts->has_theme_mode = true;
            opts->auto_day_night = false;
            opts->has_auto_day_night = true;
        } else {
            apply_scaling_mode(opts, val);
            opts->has_scaling_mode = true;
        }
        return true;
    case CLI_FLAG_FILTER:
        apply_filter_mode(opts, val);
        opts->has_filter_mode = true;
        return true;
    case CLI_FLAG_F:
        if (opts->cmd == CLI_CMD_DAEMON_ALIAS || cli_is_valid_pixel_format(val)) {
            if (!cli_is_valid_pixel_format(val)) {
                fprintf(stderr, "Error: Invalid -f format '%s' (expected xrgb, argb, xbgr, rgb, "
                                "bgr)\n",
                        val ? val : "");
                return false;
            }
            return cli_copy_bounded(opts->pixel_format, sizeof(opts->pixel_format), val, "-f",
                                    false);
        }
        apply_filter_mode(opts, val);
        opts->has_filter_mode = true;
        return true;
    case CLI_FLAG_ANIMATE:
        opts->animate = true;
        return true;
    case CLI_FLAG_AUDIO:
        opts->audio_reactive = true;
        return true;
    case CLI_FLAG_THEME:
        opts->extract_theme = true;
        opts->no_theme = false;
        return true;
    case CLI_FLAG_NO_THEME:
        opts->no_theme = true;
        opts->extract_theme = false;
        return true;
    case CLI_FLAG_LIGHT:
        opts->theme_light_mode = true;
        opts->has_theme_mode = true;
        opts->auto_day_night = false;
        opts->has_auto_day_night = true;
        return true;
    case CLI_FLAG_DARK:
        opts->theme_light_mode = false;
        opts->has_theme_mode = true;
        opts->auto_day_night = false;
        opts->has_auto_day_night = true;
        return true;
    case CLI_FLAG_AUTO_DAY_NIGHT:
        opts->auto_day_night = true;
        opts->has_auto_day_night = true;
        opts->has_theme_mode = false;
        return true;
    case CLI_FLAG_NO_AUTO_DAY_NIGHT:
        opts->auto_day_night = false;
        opts->has_auto_day_night = true;
        return true;
    case CLI_FLAG_THEME_MODE:
        if (strcasecmp(val, "auto") == 0) {
            opts->auto_day_night = true;
            opts->has_auto_day_night = true;
            opts->has_theme_mode = false;
        } else if (strcasecmp(val, "light") == 0 || strcasecmp(val, "dark") == 0) {
            opts->theme_light_mode = (strcasecmp(val, "light") == 0);
            opts->has_theme_mode = true;
            opts->auto_day_night = false;
            opts->has_auto_day_night = true;
        } else {
            fprintf(stderr, "Error: Invalid --theme-mode '%s' (expected dark, light, or auto)\n",
                    val ? val : "");
            return false;
        }
        return true;
    case CLI_FLAG_LATITUDE: {
        char *endp = NULL;
        errno = 0;
        double lat = val ? strtod(val, &endp) : NAN;
        if (!val || val[0] == '\0' || isspace((unsigned char)val[0]) || !endp || endp == val ||
            *endp != '\0' || !isfinite(lat) || (errno == ERANGE && fabs(lat) > 1.0) ||
            lat < -90.0 || lat > 90.0) {
            fprintf(stderr, "Error: Invalid --latitude '%s' (expected -90.0..90.0)\n",
                    val ? val : "");
            return false;
        }
        opts->solar_latitude = lat;
        opts->has_solar_lat = true;
        opts->has_solar_coords = true;
        return true;
    }
    case CLI_FLAG_LONGITUDE: {
        char *endp = NULL;
        errno = 0;
        double lon = val ? strtod(val, &endp) : NAN;
        if (!val || val[0] == '\0' || isspace((unsigned char)val[0]) || !endp || endp == val ||
            *endp != '\0' || !isfinite(lon) || (errno == ERANGE && fabs(lon) > 1.0) ||
            lon < -180.0 || lon > 180.0) {
            fprintf(stderr, "Error: Invalid --longitude '%s' (expected -180.0..180.0)\n",
                    val ? val : "");
            return false;
        }
        opts->solar_longitude = lon;
        opts->has_solar_lon = true;
        opts->has_solar_coords = true;
        return true;
    }
    case CLI_FLAG_SOURCE_COLOR_INDEX: {
        long long idx = 0;
        if (!cli_parse_i64_range(val, 0, 3, &idx)) {
            fprintf(stderr, "Error: Invalid --source-color-index '%s' (expected 0..3)\n",
                    val ? val : "");
            return false;
        }
        opts->source_color_index = (uint8_t)idx;
        opts->has_source_color_index = true;
        return true;
    }
    case CLI_FLAG_SEED_COLOR: {
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
    case CLI_FLAG_BACKDROP:
        if (!cli_is_valid_backdrop_value(val)) {
            fprintf(stderr, "Error: Invalid --backdrop '%s' (expected none, auto-blur, or path)\n",
                    val ? val : "");
            return false;
        }
        return cli_copy_bounded(opts->backdrop, sizeof(opts->backdrop), val, spec->name, false);
    case CLI_FLAG_LAYER:
        if (strcasecmp(val, "background") != 0 && strcasecmp(val, "bottom") != 0) {
            fprintf(stderr, "Error: Invalid --layer '%s' (expected background or bottom)\n",
                    val ? val : "");
            return false;
        }
        return cli_copy_bounded(opts->layer_name, sizeof(opts->layer_name), val, spec->name, false);
    case CLI_FLAG_BATTERY_POLICY:
        if (strcasecmp(val, "ignore") != 0 && strcasecmp(val, "reduce-fps") != 0 &&
            strcasecmp(val, "pause-video") != 0) {
            fprintf(stderr,
                    "Error: Invalid --battery-policy '%s' (expected ignore, reduce-fps, or "
                    "pause-video)\n",
                    val ? val : "");
            return false;
        }
        return cli_copy_bounded(opts->battery_policy, sizeof(opts->battery_policy), val, spec->name,
                                false);
    case CLI_FLAG_DEPTH_STRENGTH: {
        float strength = 0.0f;
        if (!cli_parse_float_strict(val, &strength) || strength <= 0.0f) {
            fprintf(stderr, "Error: Invalid --depth-strength value\n");
            return false;
        }
        opts->depth_strength = strength;
        return true;
    }
    case CLI_FLAG_PROP: {
        if (!val || val[0] == '\0' || val[0] == '=' || strchr(val, '=') == NULL) {
            fprintf(stderr, "Error: Invalid --prop format (expected key=value)\n");
            return false;
        }
        size_t cur_len = strlen(opts->workshop_props);
        size_t val_len = strlen(val);
        size_t sep_len = (cur_len > 0) ? 1U : 0U;
        if (cur_len + sep_len + val_len + 1U > sizeof(opts->workshop_props)) {
            fprintf(stderr, "Error: --prop overrides exceed buffer capacity (%zu bytes)\n",
                    sizeof(opts->workshop_props) - 1U);
            return false;
        }
        if (sep_len > 0) {
            opts->workshop_props[cur_len++] = ';';
        }
        memcpy(opts->workshop_props + cur_len, val, val_len + 1U);
        return true;
    }
    case CLI_FLAG_JSON:
        opts->json_output = true;
        return true;
    case CLI_FLAG_VOLUME: {
        float vol = 0.0f;
        if (!cli_parse_float_strict(val, &vol)) {
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
    case CLI_FLAG_MUTE_ON_AUDIO:
        opts->mute_on_audio = 1;
        return true;
    case CLI_FLAG_NO_MUTE_ON_AUDIO:
        opts->mute_on_audio = 0;
        return true;
    case CLI_FLAG_THEME_FROM_MEDIA:
        opts->theme_from_media = 1;
        return true;
    case CLI_FLAG_NO_THEME_FROM_MEDIA:
        opts->theme_from_media = 0;
        return true;
    case CLI_FLAG_LOOP: {
        uint64_t v = 0;
        if (!cli_parse_u64_strict(val, &v)) {
            fprintf(stderr, "Error: Invalid --loop value '%s'\n", val ? val : "");
            return false;
        }
        opts->video_loop_count = v;
        return true;
    }
    case CLI_FLAG_SPEED: {
        float s = 0.0f;
        if (!cli_parse_float_strict(val, &s) || s <= 0.0f) {
            fprintf(stderr, "Error: Invalid --speed value '%s'\n", val ? val : "");
            return false;
        }
        opts->video_speed = s;
        return true;
    }
    case CLI_FLAG_SORT:
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
    case CLI_FLAG_DURATION: {
        uint32_t sec = 0;
        if (!val || !waywal_parse_duration_seconds(val, &sec)) {
            fprintf(stderr, "Error: Invalid duration '%s'\n", val ? val : "");
            return false;
        }
        opts->slideshow_interval_s = sec;
        opts->has_slideshow_interval = true;
        return true;
    }
    case CLI_FLAG_SHUFFLE:
        opts->slideshow_random = true;
        opts->slideshow_sort = WAYWAL_SORT_RANDOM;
        opts->has_slideshow_sort = true;
        return true;
    case CLI_FLAG_RECURSIVE:
        opts->slideshow_recursive = true;
        opts->has_slideshow_recursive = true;
        return true;
    case CLI_FLAG_NO_RECURSIVE:
        opts->slideshow_recursive = false;
        opts->has_slideshow_recursive = true;
        return true;
    default:
        return false;
    }
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
    if (val && strlen(val) < sizeof(opts->namespace_str)) {
        cli_copy_cstr(opts->namespace_str, sizeof(opts->namespace_str), val);
    }

    val = get_compat_env("AWWW_TRANSITION", "SWWW_TRANSITION");
    if (val) {
        apply_transition_type(opts, val);
        opts->has_transition_type = true;
    }

    val = get_compat_env("AWWW_TRANSITION_FPS", "SWWW_TRANSITION_FPS");
    if (val) {
        long long fps = 0;
        if (cli_parse_i64_range(val, 1, 10000, &fps)) {
            opts->transition_fps = (uint32_t)fps;
            opts->has_transition_fps = true;
        }
    }

    val = get_compat_env("AWWW_TRANSITION_STEP", "SWWW_TRANSITION_STEP");
    if (val) {
        long long step = 0;
        if (cli_parse_i64_range(val, 1, 1000000, &step)) {
            opts->transition_step = (uint32_t)step;
            cli_recompute_step_duration(opts);
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
        if ((errno == 0 || (errno == ERANGE && fabsf(dur_s) <= 1.0f)) && endp && endp != val &&
            *endp == '\0' && isfinite(dur_s) && dur_s >= 0.0f && dur_s <= 3600000.0f) {
            opts->transition_duration = dur_s;
            opts->transition_duration_ms = (uint32_t)(dur_s * 1000.0f);
            opts->has_transition_duration = true;
        }
    }

    val = get_compat_env("AWWW_TRANSITION_ANGLE", "SWWW_TRANSITION_ANGLE");
    if (val) {
        float deg = 0.0f;
        if (cli_parse_float_strict(val, &deg)) {
            set_angle_degrees(opts, deg);
            opts->has_transition_angle = true;
        }
    }

    val = get_compat_env("AWWW_TRANSITION_POS", "SWWW_TRANSITION_POS");
    if (val) {
        if (parse_transition_pos(val, &opts->transition_pos_x, &opts->transition_pos_y)) {
            opts->has_transition_pos = true;
        }
    }

    val = get_compat_env("AWWW_TRANSITION_BEZIER", "SWWW_TRANSITION_BEZIER");
    if (val) {
        float bz[4] = {0.0f};
        if (cli_parse_float_list(val, 4U, bz)) {
            opts->bezier = bezier_create(bz[0], bz[1], bz[2], bz[3]);
            opts->has_custom_bezier = true;
            opts->has_transition_bezier = true;
        }
    }

    val = get_compat_env("AWWW_TRANSITION_WAVE", "SWWW_TRANSITION_WAVE");
    if (val) {
        float wv[2] = {0.0f, 0.0f};
        if (cli_parse_float_list(val, 2U, wv) && wv[0] >= 0.0f && wv[1] >= 0.0f) {
            opts->transition_wave_freq = wv[0];
            opts->transition_wave_amp = wv[1];
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
        (void)cli_parse_flag_add_outputs(opts, val, false);
        if (opts->num_outputs > 0) {
            opts->env_outputs_set = true;
        }
    }
}

typedef enum {
    CLI_SUBCMD_UNKNOWN = 0,
    CLI_SUBCMD_PING,
    CLI_SUBCMD_QUERY,
    CLI_SUBCMD_STATUS,
    CLI_SUBCMD_SUBSCRIBE,
    CLI_SUBCMD_AUDIO,
    CLI_SUBCMD_SET_UNIFORM,
    CLI_SUBCMD_RESTORE,
    CLI_SUBCMD_CLEAR_CACHE,
    CLI_SUBCMD_STATS,
    CLI_SUBCMD_DAEMON_ALIAS,
    CLI_SUBCMD_COLOR,
    CLI_SUBCMD_IMG,
    CLI_SUBCMD_SHADER,
    CLI_SUBCMD_WORKSHOP,
    CLI_SUBCMD_PICK,
    CLI_SUBCMD_THEME,
    CLI_SUBCMD_SLIDESHOW_STEP,
    CLI_SUBCMD_SLIDESHOW,
    CLI_SUBCMD_VIDEO,
    CLI_SUBCMD_CONTROL,
    CLI_SUBCMD_HELP,
    CLI_SUBCMD_VERSION,
} cli_subcmd_id_t;

typedef struct {
    const char *name;
    cli_subcmd_id_t id;
} cli_subcmd_spec_t;

typedef struct {
    const cli_subcmd_spec_t *spec;
    uint32_t hash;
} cli_subcmd_slot_t;

#define CLI_SUBCMD_TABLE_SLOTS 128U

static cli_subcmd_slot_t g_subcmd_table[CLI_SUBCMD_TABLE_SLOTS];
static atomic_int g_subcmd_table_state = 0;

static const cli_subcmd_spec_t k_subcmds[] = {
    {"ping", CLI_SUBCMD_PING},
    {"query", CLI_SUBCMD_QUERY},
    {"status", CLI_SUBCMD_STATUS},
    {"subscribe", CLI_SUBCMD_SUBSCRIBE},
    {"audio", CLI_SUBCMD_AUDIO},
    {"set-uniform", CLI_SUBCMD_SET_UNIFORM},
    {"uniform", CLI_SUBCMD_SET_UNIFORM},
    {"restore", CLI_SUBCMD_RESTORE},
    {"clear-cache", CLI_SUBCMD_CLEAR_CACHE},
    {"stats", CLI_SUBCMD_STATS},
    {"init", CLI_SUBCMD_DAEMON_ALIAS},
    {"daemon", CLI_SUBCMD_DAEMON_ALIAS},
    {"color", CLI_SUBCMD_COLOR},
    {"clear", CLI_SUBCMD_COLOR},
    {"img", CLI_SUBCMD_IMG},
    {"shader", CLI_SUBCMD_SHADER},
    {"workshop", CLI_SUBCMD_WORKSHOP},
    {"pick", CLI_SUBCMD_PICK},
    {"theme", CLI_SUBCMD_THEME},
    {"next", CLI_SUBCMD_SLIDESHOW_STEP},
    {"prev", CLI_SUBCMD_SLIDESHOW_STEP},
    {"previous", CLI_SUBCMD_SLIDESHOW_STEP},
    {"slideshow", CLI_SUBCMD_SLIDESHOW},
    {"video", CLI_SUBCMD_VIDEO},
    {"pause", CLI_SUBCMD_CONTROL},
    {"unpause", CLI_SUBCMD_CONTROL},
    {"resume", CLI_SUBCMD_CONTROL},
    {"toggle", CLI_SUBCMD_CONTROL},
    {"kill", CLI_SUBCMD_CONTROL},
    {"help", CLI_SUBCMD_HELP},
    {"version", CLI_SUBCMD_VERSION},
};

static void cli_subcmd_table_init(void)
{
    int state = atomic_load_explicit(&g_subcmd_table_state, memory_order_acquire);
    if (state == 2) {
        return;
    }
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&g_subcmd_table_state, &expected, 1,
                                                 memory_order_acq_rel, memory_order_acquire)) {
        while (atomic_load_explicit(&g_subcmd_table_state, memory_order_acquire) != 2) {
        }
        return;
    }
    for (size_t i = 0; i < sizeof(k_subcmds) / sizeof(k_subcmds[0]); ++i) {
        const cli_subcmd_spec_t *spec = &k_subcmds[i];
        uint32_t h = cli_fnv1a_32_n(spec->name, strlen(spec->name));
        size_t idx = h & (CLI_SUBCMD_TABLE_SLOTS - 1U);
        while (g_subcmd_table[idx].spec != NULL) {
            idx = (idx + 1U) & (CLI_SUBCMD_TABLE_SLOTS - 1U);
        }
        g_subcmd_table[idx].spec = spec;
        g_subcmd_table[idx].hash = h;
    }
    atomic_store_explicit(&g_subcmd_table_state, 2, memory_order_release);
}

static cli_subcmd_id_t cli_lookup_subcmd(const char *cmd_str)
{
    if (!cmd_str || cmd_str[0] == '\0') {
        return CLI_SUBCMD_UNKNOWN;
    }
    cli_subcmd_table_init();
    uint32_t h = cli_fnv1a_32_n(cmd_str, strlen(cmd_str));
    size_t idx = h & (CLI_SUBCMD_TABLE_SLOTS - 1U);
    while (g_subcmd_table[idx].spec != NULL) {
        const cli_subcmd_spec_t *spec = g_subcmd_table[idx].spec;
        if (g_subcmd_table[idx].hash == h && strcmp(spec->name, cmd_str) == 0) {
            return spec->id;
        }
        idx = (idx + 1U) & (CLI_SUBCMD_TABLE_SLOTS - 1U);
    }
    return CLI_SUBCMD_UNKNOWN;
}

static bool parse_remaining_common_flags(int optind, int argc, char *argv[], cli_options_t *opts,
                                         const char *err_prefix)
{
    while (optind < argc) {
        if (!parse_common_flag(&optind, argc, argv, opts)) {
            if (err_prefix) {
                fprintf(stderr, "Error: %s '%s'\n", err_prefix, argv[optind]);
            }
            return false;
        }
        optind++;
    }
    return true;
}

bool cli_parse(int argc, char *argv[], cli_options_t *opts)
{
    if (!opts || argc <= 0 || !argv)
        return false;

    memset(opts, 0, sizeof(*opts));
    cli_copy_cstr(opts->namespace_str, sizeof(opts->namespace_str), "default");
    opts->clear_color = (color_rgba_t){.r = 0, .g = 0, .b = 0, .a = 255};
    opts->transition_type = WAYWAL_TRANSITION_FADE;
    opts->transition_duration = 1.2f;
    opts->transition_duration_ms = 1200;
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
    opts->slideshow_recursive = true;
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
    switch (cli_lookup_subcmd(cmd_str)) {
    case CLI_SUBCMD_PING:
        opts->cmd = CLI_CMD_PING;
        if (!parse_remaining_common_flags(optind, argc, argv, opts, NULL))
            return false;
        break;
    case CLI_SUBCMD_QUERY:
        opts->cmd = CLI_CMD_QUERY;
        if (!parse_remaining_common_flags(optind, argc, argv, opts, NULL))
            return false;
        break;
    case CLI_SUBCMD_STATUS:
        opts->cmd = CLI_CMD_STATUS;
        opts->json_output = true;
        if (!parse_remaining_common_flags(optind, argc, argv, opts, NULL))
            return false;
        break;
    case CLI_SUBCMD_SUBSCRIBE:
        opts->cmd = CLI_CMD_SUBSCRIBE;
        if (!parse_remaining_common_flags(optind, argc, argv, opts, NULL))
            return false;
        break;
    case CLI_SUBCMD_AUDIO:
        opts->cmd = CLI_CMD_AUDIO;
        if (!parse_remaining_common_flags(optind, argc, argv, opts, NULL))
            return false;
        if (opts->volume < 0.0f && opts->mute_on_audio == -1 && opts->theme_from_media == -1) {
            fprintf(stderr, "Error: 'audio' requires at least one of --volume, --mute-on-audio, "
                            "or --theme-from-media\n");
            return false;
        }
        break;
    case CLI_SUBCMD_SET_UNIFORM:
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
                float v = 0.0f;
                if (!cli_parse_float_strict(arg, &v)) {
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
        break;
    case CLI_SUBCMD_RESTORE:
        opts->cmd = CLI_CMD_RESTORE;
        if (!parse_remaining_common_flags(optind, argc, argv, opts, NULL))
            return false;
        break;
    case CLI_SUBCMD_CLEAR_CACHE:
        opts->cmd = CLI_CMD_CLEAR_CACHE;
        if (!parse_remaining_common_flags(optind, argc, argv, opts, NULL))
            return false;
        break;
    case CLI_SUBCMD_STATS:
        opts->cmd = CLI_CMD_STATS;
        if (!parse_remaining_common_flags(optind, argc, argv, opts, NULL))
            return false;
        break;
    case CLI_SUBCMD_DAEMON_ALIAS:
        opts->cmd = CLI_CMD_DAEMON_ALIAS;
        if (strcmp(cmd_str, "init") == 0 && !opts->no_daemon) {
            opts->daemonize = true;
        }
        if (!parse_remaining_common_flags(optind, argc, argv, opts, NULL))
            return false;
        break;
    case CLI_SUBCMD_COLOR:
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
        break;
    case CLI_SUBCMD_IMG: {
        opts->cmd = CLI_CMD_IMG;
        bool has_path = false;
        while (optind < argc) {
            const char *arg = argv[optind];
            if (strcmp(arg, "-") == 0 && !has_path) {
                cli_copy_cstr(opts->filepath, sizeof(opts->filepath), "-");
                opts->read_stdin = true;
                has_path = true;
            } else if (arg[0] == '-') {
                if (!parse_common_flag(&optind, argc, argv, opts)) {
                    fprintf(stderr, "Error: Unknown option '%s'\n", arg);
                    return false;
                }
            } else if (!has_path) {
                if (!cli_copy_bounded(opts->filepath, sizeof(opts->filepath), arg, "image path",
                                      false)) {
                    return false;
                }
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
        break;
    }
    case CLI_SUBCMD_SHADER: {
        opts->cmd = CLI_CMD_SHADER;
        bool has_path = false;
        bool saw_preset_kw = false;
        while (optind < argc) {
            const char *arg = argv[optind];
            if (strcmp(arg, "--list") == 0) {
                if (has_path || saw_preset_kw) {
                    fprintf(stderr,
                            "Error: Cannot combine '--list' with a shader path or preset\n");
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
                const char *preset_check = (strncasecmp(arg, "preset:", 7) == 0) ? (arg + 7) : arg;
                if (opts->shader_preset_explicit &&
                    (preset_check[0] == '\0' || strchr(preset_check, '/') != NULL ||
                     strchr(preset_check, '\\') != NULL || strstr(preset_check, "..") != NULL)) {
                    fprintf(stderr, "Error: Invalid shader preset name '%s'\n", arg);
                    return false;
                }
                cli_copy_cstr(opts->shader_preset, sizeof(opts->shader_preset), arg);
                char query_buf[4200];
                if (opts->shader_preset_explicit && strncasecmp(arg, "preset:", 7) != 0) {
                    snprintf(query_buf, sizeof(query_buf), "preset:%s", arg);
                } else {
                    snprintf(query_buf, sizeof(query_buf), "%s", arg);
                }
                char resolved_preset[4096];
                bool resolved_ok =
                    waywal_shader_resolve_path(query_buf, resolved_preset, sizeof(resolved_preset));
                const char *effective = resolved_ok
                                            ? resolved_preset
                                            : (opts->shader_preset_explicit ? query_buf : arg);
                cli_copy_cstr(opts->filepath, sizeof(opts->filepath), effective);
                cli_copy_cstr(opts->custom_shader_path, sizeof(opts->custom_shader_path),
                              effective);
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
        break;
    }
    case CLI_SUBCMD_WORKSHOP: {
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
            if (!parse_remaining_common_flags(optind, argc, argv, opts, "Unknown workshop option")) {
                return false;
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
                    if (!cli_copy_bounded(opts->filepath, sizeof(opts->filepath), arg,
                                          "workshop target", false)) {
                        return false;
                    }
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
                    if (!cli_copy_bounded(opts->filepath, sizeof(opts->filepath), arg,
                                          "sync-previews dir", false)) {
                        return false;
                    }
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
                    if (!cli_copy_bounded(opts->filepath, sizeof(opts->filepath), arg,
                                          "workshop target", false)) {
                        return false;
                    }
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
        break;
    }
    case CLI_SUBCMD_PICK:
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
                if (!cli_copy_bounded(opts->pick_dir, sizeof(opts->pick_dir), val, "--dir",
                                      false)) {
                    return false;
                }
            } else if (arg[0] == '-') {
                if (!parse_common_flag(&optind, argc, argv, opts)) {
                    fprintf(stderr, "Error: Unknown pick option '%s'\n", arg);
                    return false;
                }
                if (opts->json_output && opts->pick_menu == WAYWAL_PICK_MENU_AUTO) {
                    opts->pick_menu = WAYWAL_PICK_MENU_JSON;
                }
            } else if (opts->pick_dir[0] == '\0') {
                if (!cli_copy_bounded(opts->pick_dir, sizeof(opts->pick_dir), arg, "pick dir",
                                      false)) {
                    return false;
                }
            } else {
                fprintf(stderr, "Error: Unexpected argument '%s'\n", arg);
                return false;
            }
            optind++;
        }
        break;
    case CLI_SUBCMD_THEME:
        opts->cmd = CLI_CMD_THEME;
        opts->extract_theme = true;
        while (optind < argc) {
            const char *arg = argv[optind];
            const char *val = NULL;
            if (match_opt_val(arg, "--img", &optind, argc, argv, &val) ||
                match_opt_val(arg, "--image", &optind, argc, argv, &val) ||
                match_opt_val(arg, "-i", &optind, argc, argv, &val)) {
                if (!cli_copy_bounded(opts->filepath, sizeof(opts->filepath), val, arg, false)) {
                    return false;
                }
            } else if (arg[0] == '-') {
                if (!parse_common_flag(&optind, argc, argv, opts)) {
                    fprintf(stderr, "Error: Unknown theme option '%s'\n", arg);
                    return false;
                }
            } else if (opts->filepath[0] == '\0') {
                if (!cli_copy_bounded(opts->filepath, sizeof(opts->filepath), arg, "theme target",
                                      false)) {
                    return false;
                }
            } else {
                fprintf(stderr, "Error: Unexpected argument '%s'\n", arg);
                return false;
            }
            optind++;
        }
        break;
    case CLI_SUBCMD_SLIDESHOW_STEP:
        opts->cmd = CLI_CMD_SLIDESHOW_CTRL;
        opts->slideshow_action =
            (strcmp(cmd_str, "next") == 0) ? WAYWAL_SLIDESHOW_NEXT : WAYWAL_SLIDESHOW_PREV;
        if (!parse_remaining_common_flags(optind, argc, argv, opts, "Unknown option")) {
            return false;
        }
        break;
    case CLI_SUBCMD_SLIDESHOW: {
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
            if (!parse_remaining_common_flags(optind, argc, argv, opts, "Unknown option")) {
                return false;
            }
        } else {
            opts->cmd = CLI_CMD_SLIDESHOW;
            bool saw_start_kw = false;
            if (strcmp(sub, "start") == 0) {
                saw_start_kw = true;
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
                    if (!cli_copy_bounded(opts->filepath, sizeof(opts->filepath), val, arg,
                                          false)) {
                        return false;
                    }
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
                } else if (opts->cmd == CLI_CMD_SLIDESHOW && opts->filepath[0] == '\0' &&
                           !saw_start_kw && strcmp(arg, "start") == 0) {
                    saw_start_kw = true;
                } else if (opts->cmd == CLI_CMD_SLIDESHOW && opts->filepath[0] == '\0' &&
                           !saw_start_kw &&
                           (strcmp(arg, "stop") == 0 || strcmp(arg, "pause") == 0 ||
                            strcmp(arg, "resume") == 0 || strcmp(arg, "unpause") == 0 ||
                            strcmp(arg, "toggle") == 0 || strcmp(arg, "next") == 0 ||
                            strcmp(arg, "prev") == 0 || strcmp(arg, "previous") == 0)) {
                    opts->cmd = CLI_CMD_SLIDESHOW_CTRL;
                    if (strcmp(arg, "stop") == 0) {
                        opts->slideshow_action = WAYWAL_SLIDESHOW_STOP;
                    } else if (strcmp(arg, "pause") == 0) {
                        opts->slideshow_action = WAYWAL_SLIDESHOW_PAUSE;
                    } else if (strcmp(arg, "resume") == 0 || strcmp(arg, "unpause") == 0) {
                        opts->slideshow_action = WAYWAL_SLIDESHOW_RESUME;
                    } else if (strcmp(arg, "toggle") == 0) {
                        opts->slideshow_action = WAYWAL_SLIDESHOW_TOGGLE;
                    } else if (strcmp(arg, "next") == 0) {
                        opts->slideshow_action = WAYWAL_SLIDESHOW_NEXT;
                    } else {
                        opts->slideshow_action = WAYWAL_SLIDESHOW_PREV;
                    }
                } else if (opts->cmd == CLI_CMD_SLIDESHOW && opts->filepath[0] == '\0') {
                    if (!cli_copy_bounded(opts->filepath, sizeof(opts->filepath), arg,
                                          "slideshow dir", false)) {
                        return false;
                    }
                } else {
                    fprintf(stderr, "Error: Unknown option '%s'\n", arg);
                    return false;
                }
                optind++;
            }
            if (opts->cmd == CLI_CMD_SLIDESHOW && opts->filepath[0] == '\0') {
                fprintf(stderr, "Error: 'slideshow' requires a directory path\n");
                return false;
            }
        }
        break;
    }
    case CLI_SUBCMD_VIDEO:
        opts->cmd = CLI_CMD_VIDEO;
        while (optind < argc) {
            const char *arg = argv[optind];
            /* Route --loop and --speed through parse_common_flag so strict
             * integer and finite positive speed checks apply consistently. */
            if (arg[0] == '-') {
                if (!parse_common_flag(&optind, argc, argv, opts)) {
                    fprintf(stderr, "Error: Unknown video option '%s'\n", arg);
                    return false;
                }
            } else if (opts->filepath[0] == '\0') {
                if (!cli_copy_bounded(opts->filepath, sizeof(opts->filepath), arg, "video path",
                                      false)) {
                    return false;
                }
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
        break;
    case CLI_SUBCMD_CONTROL:
        if (strcmp(cmd_str, "pause") == 0) {
            opts->cmd = CLI_CMD_PAUSE;
        } else if (strcmp(cmd_str, "unpause") == 0 || strcmp(cmd_str, "resume") == 0) {
            opts->cmd = CLI_CMD_UNPAUSE;
        } else if (strcmp(cmd_str, "toggle") == 0) {
            opts->cmd = CLI_CMD_TOGGLE;
        } else {
            opts->cmd = CLI_CMD_KILL;
        }
        if (!parse_remaining_common_flags(optind, argc, argv, opts, "Unknown option")) {
            return false;
        }
        break;
    case CLI_SUBCMD_HELP:
        opts->cmd = CLI_CMD_HELP;
        break;
    case CLI_SUBCMD_VERSION:
        opts->cmd = CLI_CMD_VERSION;
        break;
    default: {
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
            if (!cli_copy_bounded(opts->shader_preset, sizeof(opts->shader_preset), cmd_str,
                                  "shader preset", false) ||
                !cli_copy_bounded(opts->filepath, sizeof(opts->filepath), resolved_preset,
                                  "shader preset path", false) ||
                !cli_copy_bounded(opts->custom_shader_path, sizeof(opts->custom_shader_path),
                                  resolved_preset, "shader preset path", false)) {
                return false;
            }
            if (waywal_shader_is_audio_reactive(preset_query) ||
                waywal_shader_is_audio_reactive(resolved_preset)) {
                opts->audio_reactive = true;
            }
            if (!parse_remaining_common_flags(optind, argc, argv, opts, "Unknown shader option")) {
                return false;
            }
        } else {
            fprintf(stderr, "Error: Unknown command '%s'\n", cmd_str);
            return false;
        }
        break;
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

    if (opts->is_solid_color || (access(opts->filepath, F_OK) != 0 &&
                                 parse_hex_color(opts->filepath, &opts->clear_color))) {
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
            cli_copy_cstr(opts->filepath, sizeof(opts->filepath), resolved);
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
                    cli_copy_cstr(opts->filepath, sizeof(opts->filepath), ws_dir);
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
                    cli_copy_cstr(opts->filepath, sizeof(opts->filepath), resolved);
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
                if (!has_stat && waywal_shader_resolve_path(opts->filepath, resolved_preset,
                                                            sizeof(resolved_preset))) {
                    sh_p = resolved_preset;
                }
                if (waywal_shader_is_audio_reactive(opts->filepath) ||
                    (sh_p != opts->filepath && waywal_shader_is_audio_reactive(sh_p))) {
                    opts->audio_reactive = true;
                }
                if (sh_p != opts->filepath) {
                    cli_copy_cstr(opts->filepath, sizeof(opts->filepath), sh_p);
                }
                cli_copy_cstr(opts->custom_shader_path, sizeof(opts->custom_shader_path),
                              opts->filepath);
                return true;
            }
            if (strcasecmp(dot, ".xml") == 0 || strcasecmp(dot, ".json") == 0) {
                if (di == 1 && is_symlink) {
                    cli_copy_cstr(opts->filepath, sizeof(opts->filepath), resolved);
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
                cli_copy_cstr(opts->filepath, sizeof(opts->filepath), resolved_preset);
                cli_copy_cstr(opts->custom_shader_path, sizeof(opts->custom_shader_path),
                              resolved_preset);
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
