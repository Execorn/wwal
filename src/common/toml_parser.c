// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/toml_parser.h"

#include "waywal/log.h"
#include "waywal/solar_schedule.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

typedef enum {
    SECTION_ROOT = 0,
    SECTION_DAEMON,
    SECTION_THEME,
    SECTION_TRANSITION,
    SECTION_OUTPUT,
    SECTION_WORKSPACE,
    SECTION_HOOKS,
    SECTION_TEMPLATE,
    SECTION_SOLAR,
    SECTION_SCHEDULE,
} toml_section_kind_t;

typedef struct {
    const char *src;
    size_t pos;
    size_t len;
    int line;
    char *err_buf;
    size_t err_cap;
} toml_lexer_t;

static void set_error(toml_lexer_t *lex, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void set_error(toml_lexer_t *lex, const char *fmt, ...)
{
    if (!lex || !lex->err_buf || lex->err_cap == 0) {
        return;
    }
    int prefix = snprintf(lex->err_buf, lex->err_cap, "line %d: ", lex->line);
    if (prefix < 0 || (size_t)prefix >= lex->err_cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(lex->err_buf + prefix, lex->err_cap - (size_t)prefix, fmt, ap);
    va_end(ap);
}

static void copy_str_safe(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static uint32_t parse_transition_type_str(const char *s)
{
    if (!s)
        return 2U;
    if (strcasecmp(s, "none") == 0)
        return 0U;
    if (strcasecmp(s, "simple") == 0)
        return 1U;
    if (strcasecmp(s, "fade") == 0)
        return 2U;
    if (strcasecmp(s, "wipe") == 0)
        return 3U;
    if (strcasecmp(s, "grow") == 0 || strcasecmp(s, "center") == 0)
        return 4U;
    if (strcasecmp(s, "outer") == 0)
        return 5U;
    if (strcasecmp(s, "wave") == 0)
        return 6U;
    if (strcasecmp(s, "noise") == 0 || strcasecmp(s, "dissolve") == 0)
        return 7U;
    if (strcasecmp(s, "crosszoom") == 0)
        return 8U;
    if (strcasecmp(s, "slide") == 0 || strcasecmp(s, "left") == 0 || strcasecmp(s, "right") == 0 ||
        strcasecmp(s, "top") == 0 || strcasecmp(s, "bottom") == 0)
        return 9U;
    if (strcasecmp(s, "glitch") == 0)
        return 10U;
    if (strcasecmp(s, "burn") == 0)
        return 11U;
    if (strcasecmp(s, "ripple") == 0)
        return 12U;
    if (strcasecmp(s, "pixelate") == 0)
        return 13U;
    if (strcasecmp(s, "doom") == 0)
        return 14U;
    if (strcasecmp(s, "swirl") == 0)
        return 15U;
    if (strcasecmp(s, "cube") == 0)
        return 16U;
    if (strcasecmp(s, "luma") == 0)
        return 17U;
    if (strcasecmp(s, "light_leak") == 0 || strcasecmp(s, "light-leak") == 0)
        return 18U;
    if (strcasecmp(s, "page_curl") == 0 || strcasecmp(s, "page-curl") == 0)
        return 19U;
    if (strcasecmp(s, "custom") == 0)
        return 20U;
    if (strcasecmp(s, "random") == 0 || strcasecmp(s, "any") == 0)
        return 21U;
    return 2U;
}

static waywal_scaling_mode_t parse_scaling_str(const char *s)
{
    if (!s)
        return WAYWAL_SCALING_FILL;
    if (strcasecmp(s, "fill") == 0 || strcasecmp(s, "crop") == 0 || strcasecmp(s, "cover") == 0)
        return WAYWAL_SCALING_FILL;
    if (strcasecmp(s, "fit") == 0 || strcasecmp(s, "fit-border-color") == 0 ||
        strcasecmp(s, "contain") == 0)
        return WAYWAL_SCALING_FIT;
    if (strcasecmp(s, "stretch") == 0)
        return WAYWAL_SCALING_STRETCH;
    if (strcasecmp(s, "center") == 0 || strcasecmp(s, "no") == 0)
        return WAYWAL_SCALING_CENTER;
    if (strcasecmp(s, "tile") == 0)
        return WAYWAL_SCALING_TILE;
    if (strcasecmp(s, "span") == 0)
        return WAYWAL_SCALING_SPAN;
    return WAYWAL_SCALING_FILL;
}

static bool is_wpaperd_scaling_mode_str(const char *s)
{
    if (!s || s[0] == '\0')
        return false;
    return (strcasecmp(s, "fill") == 0 || strcasecmp(s, "crop") == 0 ||
            strcasecmp(s, "cover") == 0 || strcasecmp(s, "fit") == 0 ||
            strcasecmp(s, "fit-border-color") == 0 || strcasecmp(s, "contain") == 0 ||
            strcasecmp(s, "stretch") == 0 || strcasecmp(s, "center") == 0 ||
            strcasecmp(s, "no") == 0 || strcasecmp(s, "tile") == 0 || strcasecmp(s, "span") == 0);
}

bool waywal_parse_sort_mode(const char *str, waywal_sort_mode_t *out_mode)
{
    if (!str || !out_mode)
        return false;
    while (*str && isspace((unsigned char)*str))
        str++;
    if (*str == '\0')
        return false;
    char trimmed[32];
    size_t len = strlen(str);
    while (len > 0 && isspace((unsigned char)str[len - 1]))
        len--;
    if (len == 0 || len >= sizeof(trimmed))
        return false;
    memcpy(trimmed, str, len);
    trimmed[len] = '\0';
    if (strcasecmp(trimmed, "random") == 0 || strcasecmp(trimmed, "shuffle") == 0) {
        *out_mode = WAYWAL_SORT_RANDOM;
        return true;
    }
    if (strcasecmp(trimmed, "ascending") == 0 || strcasecmp(trimmed, "asc") == 0 ||
        strcasecmp(trimmed, "name") == 0 || strcasecmp(trimmed, "alphabetical") == 0 ||
        strcasecmp(trimmed, "alpha") == 0) {
        *out_mode = WAYWAL_SORT_ASCENDING;
        return true;
    }
    if (strcasecmp(trimmed, "descending") == 0 || strcasecmp(trimmed, "desc") == 0 ||
        strcasecmp(trimmed, "reverse") == 0) {
        *out_mode = WAYWAL_SORT_DESCENDING;
        return true;
    }
    if (strcasecmp(trimmed, "mtime") == 0 || strcasecmp(trimmed, "modified") == 0 ||
        strcasecmp(trimmed, "date") == 0 || strcasecmp(trimmed, "time") == 0) {
        *out_mode = WAYWAL_SORT_MTIME;
        return true;
    }
    return false;
}

static bool has_video_ext_cfg(const char *path)
{
    if (!path)
        return false;
    const char *dot = strrchr(path, '.');
    if (!dot)
        return false;
    return (strcasecmp(dot, ".mp4") == 0 || strcasecmp(dot, ".webm") == 0 ||
            strcasecmp(dot, ".mkv") == 0 || strcasecmp(dot, ".mov") == 0 ||
            strcasecmp(dot, ".avi") == 0 || strcasecmp(dot, ".m4v") == 0);
}

static bool has_shader_ext_cfg(const char *path)
{
    if (!path || path[0] == '\0')
        return false;
    const char *dot = strrchr(path, '.');
    if (dot && (strcasecmp(dot, ".frag") == 0 || strcasecmp(dot, ".glsl") == 0 ||
                strcasecmp(dot, ".comp") == 0 || strcasecmp(dot, ".shader") == 0)) {
        return true;
    }
    if (strncasecmp(path, "preset:", 7) == 0) {
        return true;
    }
    char resolved[WAYWAL_PATH_MAX];
    if (!strchr(path, '/') && waywal_shader_resolve_path(path, resolved, sizeof(resolved))) {
        return true;
    }
    return false;
}

static bool has_image_ext_cfg(const char *path)
{
    if (!path)
        return false;
    const char *dot = strrchr(path, '.');
    if (!dot)
        return false;
    return (strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".jpg") == 0 ||
            strcasecmp(dot, ".jpeg") == 0 || strcasecmp(dot, ".webp") == 0 ||
            strcasecmp(dot, ".bmp") == 0 || strcasecmp(dot, ".tga") == 0 ||
            strcasecmp(dot, ".gif") == 0 || strcasecmp(dot, ".avif") == 0 ||
            strcasecmp(dot, ".jxl") == 0 || strcasecmp(dot, ".svg") == 0 ||
            strcasecmp(dot, ".pnm") == 0 || strcasecmp(dot, ".ppm") == 0 ||
            strcasecmp(dot, ".pgm") == 0 || strcasecmp(dot, ".qoi") == 0 ||
            strcasecmp(dot, ".hdr") == 0 || strcasecmp(dot, ".psd") == 0);
}

static bool has_any_ext_cfg(const char *path)
{
    if (!path)
        return false;
    const char *slash = strrchr(path, '/');
    const char *base = slash ? (slash + 1) : path;
    const char *dot = strrchr(base, '.');
    return (dot != NULL && dot != base && dot[1] != '\0');
}

static bool is_workshop_dir_cfg(const char *path)
{
    if (!path || path[0] == '\0')
        return false;
    size_t plen = strlen(path);
    if ((plen > 13 && strcasecmp(path + plen - 13, "/project.json") == 0) ||
        (plen > 10 && strcasecmp(path + plen - 10, "/scene.pkg") == 0)) {
        return true;
    }
    char probe[WAYWAL_PATH_MAX + 32];
    struct stat st;
    snprintf(probe, sizeof(probe), "%s/project.json", path);
    if (stat(probe, &st) == 0 && S_ISREG(st.st_mode))
        return true;
    snprintf(probe, sizeof(probe), "%s/scene.pkg", path);
    if (stat(probe, &st) == 0 && S_ISREG(st.st_mode))
        return true;
    snprintf(probe, sizeof(probe), "%s/gifscene.pkg", path);
    if (stat(probe, &st) == 0 && S_ISREG(st.st_mode))
        return true;
    snprintf(probe, sizeof(probe), "%s/scene.json", path);
    if (stat(probe, &st) == 0 && S_ISREG(st.st_mode))
        return true;
    return false;
}

static bool is_dir_cfg(const char *path)
{
    if (!path || path[0] == '\0')
        return false;
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

static waywal_filter_mode_t parse_filter_str(const char *s)
{
    if (!s)
        return WAYWAL_FILTER_LANCZOS3;
    if (strcasecmp(s, "lanczos3") == 0 || strcasecmp(s, "lanczos") == 0) {
        return WAYWAL_FILTER_LANCZOS3;
    }
    if (strcasecmp(s, "mitchell") == 0 || strcasecmp(s, "catmull-rom") == 0) {
        return WAYWAL_FILTER_MITCHELL;
    }
    if (strcasecmp(s, "bilinear") == 0 || strcasecmp(s, "linear") == 0) {
        return WAYWAL_FILTER_BILINEAR;
    }
    if (strcasecmp(s, "nearest") == 0) {
        return WAYWAL_FILTER_NEAREST;
    }
    return WAYWAL_FILTER_LANCZOS3;
}

void waywal_config_init_defaults(waywal_config_t *out_cfg)
{
    if (!out_cfg) {
        return;
    }
    memset(out_cfg, 0, sizeof(*out_cfg));

    copy_str_safe(out_cfg->daemon.layer, sizeof(out_cfg->daemon.layer), "background");
    out_cfg->daemon.layer_pos = WAYWAL_LAYER_BACKGROUND;
    copy_str_safe(out_cfg->daemon.namespace_str, sizeof(out_cfg->daemon.namespace_str), "waywal");
    copy_str_safe(out_cfg->daemon.battery_policy, sizeof(out_cfg->daemon.battery_policy),
                  "reduce-fps");
    out_cfg->daemon.battery_policy_mode = WAYWAL_BATTERY_REDUCE_FPS;
    out_cfg->daemon.auto_pause_fullscreen = true;
    out_cfg->daemon.auto_pause_maximized = false;
    out_cfg->daemon.idle_timeout_sec = 300U;
    out_cfg->daemon.ten_bit = false;
    copy_str_safe(out_cfg->daemon.backdrop, sizeof(out_cfg->daemon.backdrop), "none");
    out_cfg->daemon.restore_on_start = true;
    out_cfg->daemon.pause_on_occluded_workspace = true;
    out_cfg->daemon.pause_on_idle = true;
    out_cfg->daemon.interval = 300U;
    out_cfg->daemon.sort_mode = WAYWAL_SORT_RANDOM;
    copy_str_safe(out_cfg->daemon.sorting, sizeof(out_cfg->daemon.sorting), "random");
    out_cfg->daemon.shuffle = true;
    out_cfg->daemon.recursive = true;

    out_cfg->theme.enabled = true;
    copy_str_safe(out_cfg->theme.mode, sizeof(out_cfg->theme.mode), "dark");
    out_cfg->theme.theme_mode = WAYWAL_THEME_DARK;
    out_cfg->theme.caelestia_sync = true;
    out_cfg->theme.tty_sequences = true;
    copy_str_safe(out_cfg->theme.scheme_variant, sizeof(out_cfg->theme.scheme_variant),
                  "tonal_spot");
    out_cfg->theme.contrast = 0.0f;
    out_cfg->theme.export_css = true;
    out_cfg->theme.export_hyprland = true;
    out_cfg->theme.source_color_index = 0U;
    out_cfg->theme.seed_color = 0U;
    out_cfg->theme.has_seed_color = false;
    out_cfg->theme.pywal_compat = true;

    copy_str_safe(out_cfg->transition.type, sizeof(out_cfg->transition.type), "fade");
    out_cfg->transition.type_enum = 2U;
    out_cfg->transition.duration = 0.7f;
    out_cfg->transition.fps = 144U;
    out_cfg->transition.bezier[0] = 0.25f;
    out_cfg->transition.bezier[1] = 0.10f;
    out_cfg->transition.bezier[2] = 0.25f;
    out_cfg->transition.bezier[3] = 1.00f;
    out_cfg->transition.angle = 0.0f;
    out_cfg->transition.pos_x = 0.5f;
    out_cfg->transition.pos_y = 0.5f;
    out_cfg->transition.wave_x = 0.0f;
    out_cfg->transition.wave_y = 0.0f;
    out_cfg->transition.step = 90U;
    copy_str_safe(out_cfg->transition.pos, sizeof(out_cfg->transition.pos), "center");
    copy_str_safe(out_cfg->transition.sync_mode, sizeof(out_cfg->transition.sync_mode),
                  "independent");
    out_cfg->transition.stagger_delay_ms = 0U;
    copy_str_safe(out_cfg->transition.crop_gravity, sizeof(out_cfg->transition.crop_gravity),
                  "center");
    out_cfg->transition.crop_gravity_x = 0.5f;
    out_cfg->transition.crop_gravity_y = 0.5f;
    out_cfg->transition.has_crop_gravity = false;

    copy_str_safe(out_cfg->daemon.crop_gravity, sizeof(out_cfg->daemon.crop_gravity), "center");
    out_cfg->daemon.crop_gravity_x = 0.5f;
    out_cfg->daemon.crop_gravity_y = 0.5f;
    out_cfg->daemon.has_crop_gravity = false;

    out_cfg->solar.enabled = false;
    out_cfg->solar.latitude = 0.0;
    out_cfg->solar.longitude = 0.0;
    out_cfg->solar.has_coordinates = false;
    out_cfg->solar.auto_day_night = false;
    out_cfg->solar.sunrise_fallback_min = 6 * 60 + 30;
    out_cfg->solar.sunset_fallback_min = 18 * 60 + 30;
    copy_str_safe(out_cfg->solar.sunrise_str, sizeof(out_cfg->solar.sunrise_str), "06:30");
    copy_str_safe(out_cfg->solar.sunset_str, sizeof(out_cfg->solar.sunset_str), "18:30");
}

static void skip_horizontal_ws(toml_lexer_t *lex)
{
    while (lex->pos < lex->len) {
        char c = lex->src[lex->pos];
        if (c == ' ' || c == '\t' || c == '\r') {
            lex->pos++;
        } else {
            break;
        }
    }
}

static void skip_ws_and_comments(toml_lexer_t *lex)
{
    while (lex->pos < lex->len) {
        char c = lex->src[lex->pos];
        if (c == ' ' || c == '\t' || c == '\r') {
            lex->pos++;
        } else if (c == '\n') {
            lex->pos++;
            lex->line++;
        } else if (c == '#') {
            while (lex->pos < lex->len && lex->src[lex->pos] != '\n') {
                lex->pos++;
            }
        } else {
            break;
        }
    }
}

static bool is_bare_key_char(char c) { return isalnum((unsigned char)c) || c == '_' || c == '-'; }

static bool parse_quoted_string(toml_lexer_t *lex, char *out_buf, size_t out_cap)
{
    if (lex->pos >= lex->len) {
        set_error(lex, "unexpected end of input while expecting string");
        return false;
    }

    char quote = lex->src[lex->pos];
    if (quote != '"' && quote != '\'') {
        set_error(lex, "expected quoted string");
        return false;
    }
    lex->pos++;

    size_t w = 0;
    while (lex->pos < lex->len) {
        char c = lex->src[lex->pos++];
        if (c == quote) {
            if (out_cap > 0) {
                out_buf[w < out_cap ? w : (out_cap - 1)] = '\0';
            }
            return true;
        }
        if (c == '\n') {
            set_error(lex, "unterminated string literal");
            return false;
        }
        if (quote == '"' && c == '\\') {
            if (lex->pos >= lex->len) {
                set_error(lex, "unterminated escape sequence in string");
                return false;
            }
            char esc = lex->src[lex->pos++];
            switch (esc) {
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            case '\\':
                c = '\\';
                break;
            case '"':
                c = '"';
                break;
            default:
                c = esc;
                break;
            }
        }
        if (w + 1 >= out_cap) {
            set_error(lex, "string literal exceeds maximum length (%zu)",
                      out_cap > 0 ? out_cap - 1 : 0);
            return false;
        }
        out_buf[w++] = c;
    }

    set_error(lex, "unterminated string at end of input");
    return false;
}

static bool parse_key_segment(toml_lexer_t *lex, char *out_buf, size_t out_cap)
{
    skip_horizontal_ws(lex);
    if (lex->pos >= lex->len) {
        set_error(lex, "expected key");
        return false;
    }

    char c = lex->src[lex->pos];
    if (c == '"' || c == '\'') {
        return parse_quoted_string(lex, out_buf, out_cap);
    }

    if (!is_bare_key_char(c)) {
        set_error(lex, "invalid character '%c' in key", c);
        return false;
    }

    size_t w = 0;
    while (lex->pos < lex->len && is_bare_key_char(lex->src[lex->pos])) {
        if (w + 1 >= out_cap) {
            set_error(lex, "key segment exceeds maximum length (%zu)",
                      out_cap > 0 ? out_cap - 1 : 0);
            return false;
        }
        out_buf[w++] = lex->src[lex->pos];
        lex->pos++;
    }
    if (out_cap > 0) {
        out_buf[w] = '\0';
    }
    return true;
}

static bool parse_number_token(toml_lexer_t *lex, char *num_buf, size_t num_cap)
{
    skip_horizontal_ws(lex);
    size_t w = 0;
    while (lex->pos < lex->len) {
        char c = lex->src[lex->pos];
        if (isalnum((unsigned char)c) || c == '+' || c == '-' || c == '.') {
            if (w + 1 >= num_cap) {
                set_error(lex, "numeric token exceeds maximum length");
                return false;
            }
            num_buf[w++] = c;
            lex->pos++;
        } else if (c == '_') {
            lex->pos++;
        } else {
            break;
        }
    }
    num_buf[w] = '\0';
    if (w == 0) {
        set_error(lex, "expected numeric value");
        return false;
    }
    return true;
}

static bool parse_bool_val(toml_lexer_t *lex, bool *out_val)
{
    skip_horizontal_ws(lex);
    if (lex->pos + 4 <= lex->len && strncmp(lex->src + lex->pos, "true", 4) == 0 &&
        (lex->pos + 4 == lex->len || !is_bare_key_char(lex->src[lex->pos + 4]))) {
        lex->pos += 4;
        *out_val = true;
        return true;
    }
    if (lex->pos + 5 <= lex->len && strncmp(lex->src + lex->pos, "false", 5) == 0 &&
        (lex->pos + 5 == lex->len || !is_bare_key_char(lex->src[lex->pos + 5]))) {
        lex->pos += 5;
        *out_val = false;
        return true;
    }
    set_error(lex, "expected boolean ('true' or 'false')");
    return false;
}

static bool parse_uint32_val(toml_lexer_t *lex, uint32_t *out_val)
{
    char buf[64];
    if (!parse_number_token(lex, buf, sizeof(buf))) {
        return false;
    }
    if (buf[0] == '-') {
        set_error(lex, "expected non-negative integer, got '%s'", buf);
        return false;
    }
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(buf, &end, 0);
    if (errno != 0 || !end || *end != '\0' || v > UINT32_MAX) {
        set_error(lex, "invalid integer '%s'", buf);
        return false;
    }
    *out_val = (uint32_t)v;
    return true;
}

bool waywal_parse_duration_seconds(const char *str, uint32_t *out_seconds)
{
    if (!str || !out_seconds) {
        return false;
    }
    const char *p = str;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p == '\0' || *p == '-') {
        return false;
    }

    uint64_t total = 0;
    bool parsed_any = false;

    while (*p) {
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (!isdigit((unsigned char)*p)) {
            return false;
        }

        uint64_t val = 0;
        while (*p && isdigit((unsigned char)*p)) {
            uint64_t digit = (uint64_t)(*p - '0');
            if (val > (UINT64_MAX - digit) / 10ULL) {
                return false;
            }
            val = val * 10ULL + digit;
            p++;
        }

        while (*p && isspace((unsigned char)*p)) {
            p++;
        }

        uint64_t mult = 1ULL;
        if (*p && isalpha((unsigned char)*p)) {
            const char *u_start = p;
            while (*p && isalpha((unsigned char)*p)) {
                p++;
            }
            size_t u_len = (size_t)(p - u_start);
            if ((u_len == 1 && (u_start[0] == 's' || u_start[0] == 'S')) ||
                (u_len == 3 && strncasecmp(u_start, "sec", 3) == 0) ||
                (u_len == 4 && strncasecmp(u_start, "secs", 4) == 0) ||
                (u_len == 6 && strncasecmp(u_start, "second", 6) == 0) ||
                (u_len == 7 && strncasecmp(u_start, "seconds", 7) == 0)) {
                mult = 1ULL;
            } else if ((u_len == 1 && (u_start[0] == 'm' || u_start[0] == 'M')) ||
                       (u_len == 3 && strncasecmp(u_start, "min", 3) == 0) ||
                       (u_len == 4 && strncasecmp(u_start, "mins", 4) == 0) ||
                       (u_len == 6 && strncasecmp(u_start, "minute", 6) == 0) ||
                       (u_len == 7 && strncasecmp(u_start, "minutes", 7) == 0)) {
                mult = 60ULL;
            } else if ((u_len == 1 && (u_start[0] == 'h' || u_start[0] == 'H')) ||
                       (u_len == 2 && strncasecmp(u_start, "hr", 2) == 0) ||
                       (u_len == 3 && strncasecmp(u_start, "hrs", 3) == 0) ||
                       (u_len == 4 && strncasecmp(u_start, "hour", 4) == 0) ||
                       (u_len == 5 && strncasecmp(u_start, "hours", 5) == 0)) {
                mult = 3600ULL;
            } else if ((u_len == 1 && (u_start[0] == 'd' || u_start[0] == 'D')) ||
                       (u_len == 3 && strncasecmp(u_start, "day", 3) == 0) ||
                       (u_len == 4 && strncasecmp(u_start, "days", 4) == 0)) {
                mult = 86400ULL;
            } else {
                return false;
            }
        } else {
            while (*p && isspace((unsigned char)*p)) {
                p++;
            }
            if (*p != '\0') {
                return false;
            }
        }

        if (val > (uint64_t)UINT32_MAX / mult) {
            return false;
        }
        uint64_t term = val * mult;
        if (total > (uint64_t)UINT32_MAX - term) {
            return false;
        }
        total += term;
        parsed_any = true;
    }

    if (!parsed_any || total == 0 || total > UINT32_MAX) {
        return false;
    }
    *out_seconds = (uint32_t)total;
    return true;
}

static bool parse_duration_val(toml_lexer_t *lex, uint32_t *out_seconds)
{
    skip_horizontal_ws(lex);
    if (lex->pos < lex->len && (lex->src[lex->pos] == '"' || lex->src[lex->pos] == '\'')) {
        char s[64];
        if (!parse_quoted_string(lex, s, sizeof(s))) {
            return false;
        }
        if (!waywal_parse_duration_seconds(s, out_seconds)) {
            set_error(lex, "invalid duration string '%s'", s);
            return false;
        }
        return true;
    }
    char buf[64];
    if (!parse_number_token(lex, buf, sizeof(buf))) {
        return false;
    }
    if (waywal_parse_duration_seconds(buf, out_seconds)) {
        return true;
    }
    set_error(lex, "invalid duration '%s'", buf);
    return false;
}

static bool is_zero_duration_token(const char *s)
{
    if (!s) {
        return false;
    }
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    if (strcmp(s, "0") == 0 || strcasecmp(s, "0s") == 0 || strcasecmp(s, "0sec") == 0 ||
        strcasecmp(s, "0secs") == 0 || strcasecmp(s, "0m") == 0) {
        return true;
    }
    return false;
}

static bool parse_idle_timeout_val(toml_lexer_t *lex, uint32_t *out_seconds)
{
    skip_horizontal_ws(lex);
    if (lex->pos < lex->len && (lex->src[lex->pos] == '"' || lex->src[lex->pos] == '\'')) {
        char s[64];
        if (!parse_quoted_string(lex, s, sizeof(s))) {
            return false;
        }
        if (is_zero_duration_token(s)) {
            *out_seconds = 0;
            return true;
        }
        if (!waywal_parse_duration_seconds(s, out_seconds)) {
            set_error(lex, "invalid idle_timeout_sec '%s'", s);
            return false;
        }
        return true;
    }
    char buf[64];
    if (!parse_number_token(lex, buf, sizeof(buf))) {
        return false;
    }
    if (is_zero_duration_token(buf)) {
        *out_seconds = 0;
        return true;
    }
    if (waywal_parse_duration_seconds(buf, out_seconds)) {
        return true;
    }
    set_error(lex, "invalid idle_timeout_sec '%s'", buf);
    return false;
}

static bool parse_float_val(toml_lexer_t *lex, float *out_val)
{
    char buf[64];
    if (!parse_number_token(lex, buf, sizeof(buf))) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    float v = strtof(buf, &end);
    if (errno != 0 || !end || *end != '\0' || !isfinite(v)) {
        set_error(lex, "invalid float '%s'", buf);
        return false;
    }
    *out_val = v;
    return true;
}

static bool parse_float_duration_val(toml_lexer_t *lex, float *out_seconds)
{
    skip_horizontal_ws(lex);
    if (lex->pos < lex->len && (lex->src[lex->pos] == '"' || lex->src[lex->pos] == '\'')) {
        char s[64];
        if (!parse_quoted_string(lex, s, sizeof(s))) {
            return false;
        }
        char *end = NULL;
        errno = 0;
        float v = strtof(s, &end);
        if (errno == 0 && end && end != s && isfinite(v)) {
            while (*end && isspace((unsigned char)*end)) {
                end++;
            }
            char unit[16];
            size_t ulen = 0;
            while (*end && !isspace((unsigned char)*end) && ulen + 1 < sizeof(unit)) {
                unit[ulen++] = *end++;
            }
            unit[ulen] = '\0';
            while (*end && isspace((unsigned char)*end)) {
                end++;
            }
            if (*end == '\0') {
                if (unit[0] == '\0' || strcasecmp(unit, "s") == 0 || strcasecmp(unit, "sec") == 0 ||
                    strcasecmp(unit, "secs") == 0) {
                    if (v >= 0.0f && v <= 3600000.0f) {
                        *out_seconds = v;
                        return true;
                    }
                } else if (strcasecmp(unit, "ms") == 0) {
                    float sec_v = v / 1000.0f;
                    if (v >= 0.0f && isfinite(sec_v) && sec_v <= 3600000.0f) {
                        *out_seconds = sec_v;
                        return true;
                    }
                }
            }
        }
        uint32_t secs = 0;
        if (waywal_parse_duration_seconds(s, &secs) && (float)secs <= 3600000.0f) {
            *out_seconds = (float)secs;
            return true;
        }
        set_error(lex, "invalid transition duration '%s'", s);
        return false;
    }
    float v = 0.0f;
    if (!parse_float_val(lex, &v)) {
        return false;
    }
    if (!isfinite(v) || v < 0.0f || v > 3600000.0f) {
        set_error(lex, "transition duration out of range");
        return false;
    }
    *out_seconds = v;
    return true;
}

static bool parse_double_val(toml_lexer_t *lex, double *out_val)
{
    char buf[64];
    if (!parse_number_token(lex, buf, sizeof(buf))) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    double v = strtod(buf, &end);
    if (errno != 0 || !end || *end != '\0' || !isfinite(v)) {
        set_error(lex, "invalid double '%s'", buf);
        return false;
    }
    *out_val = v;
    return true;
}

static bool parse_float_array(toml_lexer_t *lex, float *out_arr, size_t expected_count)
{
    skip_horizontal_ws(lex);
    if (lex->pos >= lex->len || lex->src[lex->pos] != '[') {
        set_error(lex, "expected '[' to start float array");
        return false;
    }
    lex->pos++;

    size_t count = 0;
    while (lex->pos < lex->len) {
        skip_ws_and_comments(lex);
        if (lex->pos < lex->len && lex->src[lex->pos] == ']') {
            lex->pos++;
            if (count != expected_count) {
                set_error(lex, "expected %zu floats in array, got %zu", expected_count, count);
                return false;
            }
            return true;
        }

        if (count >= expected_count) {
            set_error(lex, "array exceeds expected length %zu", expected_count);
            return false;
        }

        if (!parse_float_val(lex, &out_arr[count])) {
            return false;
        }
        count++;

        skip_ws_and_comments(lex);
        if (lex->pos < lex->len && lex->src[lex->pos] == ',') {
            lex->pos++;
        } else if (lex->pos < lex->len && lex->src[lex->pos] == ']') {
            continue;
        } else {
            set_error(lex, "expected ',' or ']' in array");
            return false;
        }
    }

    set_error(lex, "unterminated array");
    return false;
}

static bool parse_color_val(toml_lexer_t *lex, uint32_t *out_color)
{
    skip_horizontal_ws(lex);
    if (lex->pos < lex->len && (lex->src[lex->pos] == '"' || lex->src[lex->pos] == '\'')) {
        char s[64];
        if (!parse_quoted_string(lex, s, sizeof(s))) {
            return false;
        }
        const char *p = s;
        if (*p == '#')
            p++;
        else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
            p += 2;
        size_t hex_len = strlen(p);
        if (hex_len != 3 && hex_len != 6 && hex_len != 8) {
            set_error(lex, "invalid hex color string '%s'", s);
            return false;
        }
        for (size_t i = 0; i < hex_len; ++i) {
            if (!isxdigit((unsigned char)p[i])) {
                set_error(lex, "invalid hex color string '%s'", s);
                return false;
            }
        }
        char *end = NULL;
        errno = 0;
        unsigned long v = strtoul(p, &end, 16);
        if (errno != 0 || !end || *end != '\0' || v > UINT32_MAX) {
            set_error(lex, "invalid hex color string '%s'", s);
            return false;
        }
        if (hex_len == 3) {
            uint32_t r = (uint32_t)((v >> 8) & 0xFU);
            uint32_t g = (uint32_t)((v >> 4) & 0xFU);
            uint32_t b = (uint32_t)(v & 0xFU);
            r = (r << 4) | r;
            g = (g << 4) | g;
            b = (b << 4) | b;
            v = (unsigned long)((r << 16) | (g << 8) | b);
        }
        if (hex_len == 3 || hex_len == 6) {
            v = (v << 8) | 0xFFU;
        }
        *out_color = (uint32_t)v;
        return true;
    }
    return parse_uint32_val(lex, out_color);
}

static bool parse_rgb24_color_val(toml_lexer_t *lex, uint32_t *out_rgb24)
{
    skip_horizontal_ws(lex);
    if (lex->pos < lex->len && (lex->src[lex->pos] == '"' || lex->src[lex->pos] == '\'')) {
        char s[64];
        if (!parse_quoted_string(lex, s, sizeof(s))) {
            return false;
        }
        const char *p = s;
        if (*p == '#')
            p++;
        else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
            p += 2;
        size_t hex_len = strlen(p);
        if (hex_len != 3 && hex_len != 6 && hex_len != 8) {
            set_error(lex, "invalid hex seed_color '%s'", s);
            return false;
        }
        for (size_t i = 0; i < hex_len; ++i) {
            if (!isxdigit((unsigned char)p[i])) {
                set_error(lex, "invalid hex seed_color '%s'", s);
                return false;
            }
        }
        char *end = NULL;
        errno = 0;
        unsigned long v = strtoul(p, &end, 16);
        if (errno != 0 || !end || *end != '\0' || v > UINT32_MAX) {
            set_error(lex, "invalid hex seed_color '%s'", s);
            return false;
        }
        if (hex_len == 3) {
            uint32_t r = (uint32_t)((v >> 8) & 0xFU);
            uint32_t g = (uint32_t)((v >> 4) & 0xFU);
            uint32_t b = (uint32_t)(v & 0xFU);
            r = (r << 4) | r;
            g = (g << 4) | g;
            b = (b << 4) | b;
            v = (unsigned long)((r << 16) | (g << 8) | b);
        } else if (hex_len == 8) {
            v = (v >> 8) & 0xFFFFFFUL;
        }
        *out_rgb24 = (uint32_t)(v & 0xFFFFFFUL);
        return true;
    }
    uint32_t raw = 0;
    if (!parse_uint32_val(lex, &raw)) {
        return false;
    }
    *out_rgb24 = raw & 0xFFFFFFU;
    return true;
}

bool waywal_parse_crop_gravity(const char *str, float *out_x, float *out_y)
{
    if (!str || !out_x || !out_y) {
        return false;
    }
    while (*str && isspace((unsigned char)*str)) {
        str++;
    }
    if (*str == '\0') {
        return false;
    }
    char trimmed[64];
    size_t len = strlen(str);
    while (len > 0 && isspace((unsigned char)str[len - 1])) {
        len--;
    }
    if (len < sizeof(trimmed)) {
        memcpy(trimmed, str, len);
        trimmed[len] = '\0';
        if (strcasecmp(trimmed, "center") == 0 || strcasecmp(trimmed, "middle") == 0 ||
            strcasecmp(trimmed, "c") == 0) {
            *out_x = 0.5f;
            *out_y = 0.5f;
            return true;
        }
        if (strcasecmp(trimmed, "top") == 0 || strcasecmp(trimmed, "north") == 0 ||
            strcasecmp(trimmed, "n") == 0 || strcasecmp(trimmed, "t") == 0) {
            *out_x = 0.5f;
            *out_y = 0.0f;
            return true;
        }
        if (strcasecmp(trimmed, "bottom") == 0 || strcasecmp(trimmed, "south") == 0 ||
            strcasecmp(trimmed, "s") == 0 || strcasecmp(trimmed, "b") == 0) {
            *out_x = 0.5f;
            *out_y = 1.0f;
            return true;
        }
        if (strcasecmp(trimmed, "left") == 0 || strcasecmp(trimmed, "west") == 0 ||
            strcasecmp(trimmed, "w") == 0 || strcasecmp(trimmed, "l") == 0) {
            *out_x = 0.0f;
            *out_y = 0.5f;
            return true;
        }
        if (strcasecmp(trimmed, "right") == 0 || strcasecmp(trimmed, "east") == 0 ||
            strcasecmp(trimmed, "e") == 0 || strcasecmp(trimmed, "r") == 0) {
            *out_x = 1.0f;
            *out_y = 0.5f;
            return true;
        }
        if (strcasecmp(trimmed, "top-left") == 0 || strcasecmp(trimmed, "topleft") == 0 ||
            strcasecmp(trimmed, "northwest") == 0 || strcasecmp(trimmed, "north-west") == 0 ||
            strcasecmp(trimmed, "tl") == 0 || strcasecmp(trimmed, "nw") == 0) {
            *out_x = 0.0f;
            *out_y = 0.0f;
            return true;
        }
        if (strcasecmp(trimmed, "top-right") == 0 || strcasecmp(trimmed, "topright") == 0 ||
            strcasecmp(trimmed, "northeast") == 0 || strcasecmp(trimmed, "north-east") == 0 ||
            strcasecmp(trimmed, "tr") == 0 || strcasecmp(trimmed, "ne") == 0) {
            *out_x = 1.0f;
            *out_y = 0.0f;
            return true;
        }
        if (strcasecmp(trimmed, "bottom-left") == 0 || strcasecmp(trimmed, "bottomleft") == 0 ||
            strcasecmp(trimmed, "southwest") == 0 || strcasecmp(trimmed, "south-west") == 0 ||
            strcasecmp(trimmed, "bl") == 0 || strcasecmp(trimmed, "sw") == 0) {
            *out_x = 0.0f;
            *out_y = 1.0f;
            return true;
        }
        if (strcasecmp(trimmed, "bottom-right") == 0 || strcasecmp(trimmed, "bottomright") == 0 ||
            strcasecmp(trimmed, "southeast") == 0 || strcasecmp(trimmed, "south-east") == 0 ||
            strcasecmp(trimmed, "br") == 0 || strcasecmp(trimmed, "se") == 0) {
            *out_x = 1.0f;
            *out_y = 1.0f;
            return true;
        }
    }

    const char *comma = strchr(str, ',');
    if (!comma) {
        return false;
    }
    char *end1 = NULL;
    float gx = strtof(str, &end1);
    if (!end1 || end1 == str) {
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
    float gy = strtof(s2, &end2);
    if (!end2 || end2 == s2) {
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
        gx *= 0.01f;
    }
    if (pct2) {
        gy *= 0.01f;
    }
    if (!isfinite(gx) || !isfinite(gy) || gx < 0.0f || gx > 1.0f || gy < 0.0f || gy > 1.0f) {
        return false;
    }
    *out_x = gx;
    *out_y = gy;
    return true;
}

static void apply_pos_string(waywal_transition_config_t *tr)
{
    if (!tr || tr->pos[0] == '\0') {
        return;
    }
    const char *s = tr->pos;
    if (strcasecmp(s, "cursor") == 0 || strcasecmp(s, "mouse") == 0) {
        tr->pos_x = -1.0f;
        tr->pos_y = -1.0f;
        tr->pos_y_inverted = false;
    } else if (waywal_parse_crop_gravity(s, &tr->pos_x, &tr->pos_y)) {
        tr->pos_y_inverted = false;
        return;
    } else {
        float px = 0.5f, py = 0.5f;
        if (sscanf(s, "%f , %f", &px, &py) == 2 && isfinite(px) && isfinite(py) && px >= 0.0f &&
            py >= 0.0f) {
            tr->pos_x = px;
            tr->pos_y = py;
            tr->pos_y_inverted = false;
        }
    }
}

static bool consume_line_remainder(toml_lexer_t *lex)
{
    skip_horizontal_ws(lex);
    if (lex->pos >= lex->len) {
        return true;
    }
    char c = lex->src[lex->pos];
    if (c == '#') {
        while (lex->pos < lex->len && lex->src[lex->pos] != '\n') {
            lex->pos++;
        }
        return true;
    }
    if (c == '\n') {
        return true;
    }
    set_error(lex, "unexpected trailing character '%c' on line", c);
    return false;
}

const waywal_output_rule_t *waywal_config_match_output(const waywal_config_t *cfg,
                                                       const char *output_name)
{
    if (!cfg || !output_name) {
        return NULL;
    }
    for (size_t i = 0; i < cfg->output_count; ++i) {
        if (strcmp(cfg->outputs[i].output_name, output_name) == 0) {
            return &cfg->outputs[i];
        }
    }
    for (size_t i = 0; i < cfg->output_count; ++i) {
        if (strcmp(cfg->outputs[i].output_name, "*") == 0 ||
            strcasecmp(cfg->outputs[i].output_name, "default") == 0 ||
            strcasecmp(cfg->outputs[i].output_name, "any") == 0) {
            return &cfg->outputs[i];
        }
    }
    return NULL;
}

static waywal_output_rule_t *get_or_create_output_rule(waywal_config_t *cfg, const char *name,
                                                       toml_lexer_t *lex)
{
    const char *canonical = name;
    if (strcasecmp(name, "default") == 0 || strcasecmp(name, "any") == 0) {
        canonical = "*";
    }
    for (size_t i = 0; i < cfg->output_count; ++i) {
        if (strcmp(cfg->outputs[i].output_name, canonical) == 0) {
            return &cfg->outputs[i];
        }
    }
    if (cfg->output_count >= WAYWAL_CONFIG_MAX_OUTPUTS) {
        set_error(lex, "exceeded maximum output rules (%u)", WAYWAL_CONFIG_MAX_OUTPUTS);
        return NULL;
    }
    waywal_output_rule_t *rule = &cfg->outputs[cfg->output_count++];
    memset(rule, 0, sizeof(*rule));
    copy_str_safe(rule->output_name, sizeof(rule->output_name), canonical);
    rule->scaling = WAYWAL_SCALING_FILL;
    copy_str_safe(rule->scaling_str, sizeof(rule->scaling_str), "fill");
    rule->filter = WAYWAL_FILTER_LANCZOS3;
    copy_str_safe(rule->filter_str, sizeof(rule->filter_str), "lanczos3");
    rule->fps = cfg->transition.fps;
    copy_str_safe(rule->mode, sizeof(rule->mode), "image");
    rule->interval = cfg->daemon.has_slideshow_interval ? cfg->daemon.slideshow_interval : 300U;
    rule->shuffle = cfg->daemon.has_slideshow_shuffle ? cfg->daemon.slideshow_shuffle : true;
    rule->sort_mode =
        cfg->daemon.has_slideshow_sort ? cfg->daemon.slideshow_sort : WAYWAL_SORT_RANDOM;
    copy_str_safe(rule->sorting, sizeof(rule->sorting),
                  cfg->daemon.has_slideshow_sort ? cfg->daemon.slideshow_sorting : "random");
    rule->recursive = cfg->daemon.has_slideshow_recursive ? cfg->daemon.slideshow_recursive : true;
    rule->audio = false;
    rule->invert_y = cfg->transition.invert_y;
    return rule;
}

static waywal_workspace_rule_t *get_or_create_workspace_rule(waywal_config_t *cfg, const char *name,
                                                             toml_lexer_t *lex)
{
    for (size_t i = 0; i < cfg->workspace_count; ++i) {
        if (strcmp(cfg->workspaces[i].workspace_name, name) == 0) {
            return &cfg->workspaces[i];
        }
    }
    if (cfg->workspace_count >= WAYWAL_CONFIG_MAX_WORKSPACES) {
        set_error(lex, "exceeded maximum workspace rules (%u)", WAYWAL_CONFIG_MAX_WORKSPACES);
        return NULL;
    }
    waywal_workspace_rule_t *rule = &cfg->workspaces[cfg->workspace_count++];
    memset(rule, 0, sizeof(*rule));
    copy_str_safe(rule->workspace_name, sizeof(rule->workspace_name), name);
    copy_str_safe(rule->mode, sizeof(rule->mode), "image");
    rule->scaling = WAYWAL_SCALING_FILL;
    copy_str_safe(rule->scaling_str, sizeof(rule->scaling_str), "fill");
    rule->filter = WAYWAL_FILTER_LANCZOS3;
    copy_str_safe(rule->filter_str, sizeof(rule->filter_str), "lanczos3");
    rule->interval = cfg->daemon.has_slideshow_interval ? cfg->daemon.slideshow_interval : 300U;
    rule->shuffle = cfg->daemon.has_slideshow_shuffle ? cfg->daemon.slideshow_shuffle : true;
    rule->sort_mode =
        cfg->daemon.has_slideshow_sort ? cfg->daemon.slideshow_sort : WAYWAL_SORT_RANDOM;
    copy_str_safe(rule->sorting, sizeof(rule->sorting),
                  cfg->daemon.has_slideshow_sort ? cfg->daemon.slideshow_sorting : "random");
    rule->recursive = cfg->daemon.has_slideshow_recursive ? cfg->daemon.slideshow_recursive : true;
    rule->fps = cfg->transition.fps;
    copy_str_safe(rule->transition_type, sizeof(rule->transition_type), cfg->transition.type);
    rule->transition_type_enum = cfg->transition.type_enum;
    rule->transition_duration = cfg->transition.duration;
    rule->transition_angle = cfg->transition.angle;
    rule->directional = true;
    return rule;
}

static waywal_template_rule_t *get_or_create_template_rule(waywal_config_t *cfg, const char *name,
                                                           toml_lexer_t *lex)
{
    for (size_t i = 0; i < cfg->template_count; ++i) {
        if (strcmp(cfg->templates[i].name, name) == 0) {
            return &cfg->templates[i];
        }
    }
    if (cfg->template_count >= WAYWAL_MAX_TEMPLATES) {
        set_error(lex, "exceeded maximum template rules (%u)", WAYWAL_MAX_TEMPLATES);
        return NULL;
    }
    waywal_template_rule_t *rule = &cfg->templates[cfg->template_count++];
    memset(rule, 0, sizeof(*rule));
    copy_str_safe(rule->name, sizeof(rule->name), name);
    rule->enabled = true;
    return rule;
}

static waywal_template_rule_t *alloc_template_rule(waywal_config_t *cfg, toml_lexer_t *lex)
{
    if (cfg->template_count >= WAYWAL_MAX_TEMPLATES) {
        set_error(lex, "exceeded maximum template rules (%u)", WAYWAL_MAX_TEMPLATES);
        return NULL;
    }
    size_t idx = cfg->template_count++;
    waywal_template_rule_t *rule = &cfg->templates[idx];
    memset(rule, 0, sizeof(*rule));
    snprintf(rule->name, sizeof(rule->name), "template_%zu", idx);
    rule->enabled = true;
    return rule;
}

static waywal_schedule_rule_t *alloc_schedule_rule(waywal_config_t *cfg, toml_lexer_t *lex)
{
    if (cfg->schedule_count >= WAYWAL_CONFIG_MAX_SCHEDULE) {
        set_error(lex, "exceeded maximum schedule rules (%u)", WAYWAL_CONFIG_MAX_SCHEDULE);
        return NULL;
    }
    waywal_schedule_rule_t *rule = &cfg->schedule[cfg->schedule_count++];
    memset(rule, 0, sizeof(*rule));
    copy_str_safe(rule->output, sizeof(rule->output), "*");
    copy_str_safe(rule->mode, sizeof(rule->mode), "fill");
    rule->time_minutes = -1;
    return rule;
}

bool waywal_config_parse_string(const char *toml_src, waywal_config_t *out_cfg, char *err_buf,
                                size_t err_cap)
{
    if (err_buf && err_cap > 0) {
        err_buf[0] = '\0';
    }
    if (!toml_src || !out_cfg) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "null argument passed to waywal_config_parse_string");
        }
        return false;
    }

    waywal_config_init_defaults(out_cfg);

    size_t start_offset = 0;
    if ((unsigned char)toml_src[0] == 0xEF && (unsigned char)toml_src[1] == 0xBB &&
        (unsigned char)toml_src[2] == 0xBF) {
        start_offset = 3;
    }

    toml_lexer_t lex = {
        .src = toml_src,
        .pos = start_offset,
        .len = strlen(toml_src),
        .line = 1,
        .err_buf = err_buf,
        .err_cap = err_cap,
    };

    toml_section_kind_t section = SECTION_ROOT;
    waywal_output_rule_t *cur_output = NULL;
    waywal_workspace_rule_t *cur_workspace = NULL;
    waywal_template_rule_t *cur_template = NULL;
    waywal_schedule_rule_t *cur_schedule = NULL;
    bool has_solar_lat = false;
    bool has_solar_lon = false;

    while (lex.pos < lex.len) {
        skip_ws_and_comments(&lex);
        if (lex.pos >= lex.len) {
            break;
        }

        if (lex.src[lex.pos] == '[') {
            bool is_array_table = (lex.pos + 1 < lex.len && lex.src[lex.pos + 1] == '[');
            lex.pos += is_array_table ? 2 : 1;
            char primary[64];
            if (!parse_key_segment(&lex, primary, sizeof(primary))) {
                return false;
            }

            skip_horizontal_ws(&lex);
            char subkey[64] = {0};
            bool has_subkey = false;

            if (lex.pos < lex.len && lex.src[lex.pos] == '.') {
                lex.pos++;
                if (!parse_key_segment(&lex, subkey, sizeof(subkey))) {
                    return false;
                }
                has_subkey = true;
            }

            skip_horizontal_ws(&lex);
            if (is_array_table) {
                if (lex.pos + 1 >= lex.len || lex.src[lex.pos] != ']' ||
                    lex.src[lex.pos + 1] != ']') {
                    set_error(&lex, "expected ']]' after array-of-tables header");
                    return false;
                }
                lex.pos += 2;
            } else {
                if (lex.pos >= lex.len || lex.src[lex.pos] != ']') {
                    set_error(&lex, "expected ']' after table header");
                    return false;
                }
                lex.pos++;
            }

            if (!consume_line_remainder(&lex)) {
                return false;
            }

            cur_output = NULL;
            cur_workspace = NULL;
            cur_template = NULL;
            cur_schedule = NULL;

            if (is_array_table &&
                (has_subkey ||
                 (strcmp(primary, "schedule") != 0 && strcmp(primary, "schedules") != 0 &&
                  strcmp(primary, "template") != 0 && strcmp(primary, "templates") != 0))) {
                set_error(&lex, "unsupported array table '[[%s]]'", primary);
                return false;
            }

            if (!has_subkey) {
                if (strcmp(primary, "daemon") == 0 || strcmp(primary, "general") == 0 ||
                    strcmp(primary, "slideshow") == 0 || strcmp(primary, "power") == 0 ||
                    strcmp(primary, "audio") == 0 || strcmp(primary, "mpris") == 0 ||
                    strcmp(primary, "backdrop") == 0) {
                    section = SECTION_DAEMON;
                } else if (strcasecmp(primary, "default") == 0 || strcasecmp(primary, "any") == 0) {
                    section = SECTION_OUTPUT;
                    cur_output = get_or_create_output_rule(out_cfg, "*", &lex);
                    if (!cur_output) {
                        return false;
                    }
                } else if (strcmp(primary, "theme") == 0) {
                    section = SECTION_THEME;
                } else if (strcmp(primary, "transition") == 0) {
                    section = SECTION_TRANSITION;
                } else if (strcmp(primary, "hooks") == 0) {
                    section = SECTION_HOOKS;
                } else if (strcmp(primary, "solar") == 0) {
                    section = SECTION_SOLAR;
                    out_cfg->solar.enabled = true;
                } else if (strcmp(primary, "schedule") == 0 || strcmp(primary, "schedules") == 0) {
                    section = SECTION_SCHEDULE;
                    if (is_array_table) {
                        cur_schedule = alloc_schedule_rule(out_cfg, &lex);
                        if (!cur_schedule)
                            return false;
                    }
                } else if (is_array_table && (strcmp(primary, "template") == 0 ||
                                              strcmp(primary, "templates") == 0)) {
                    section = SECTION_TEMPLATE;
                    cur_template = alloc_template_rule(out_cfg, &lex);
                    if (!cur_template)
                        return false;
                } else if (primary[0] == '\0' || strcmp(primary, "output") == 0 ||
                           strcmp(primary, "workspace") == 0 || strcmp(primary, "template") == 0 ||
                           strcmp(primary, "templates") == 0) {
                    set_error(&lex, "unknown table '[%s]'", primary);
                    return false;
                } else {
                    /* Support wpaperd-style top-level output tables: [DP-1], [HDMI-A-1], [eDP-1] */
                    section = SECTION_OUTPUT;
                    cur_output = get_or_create_output_rule(out_cfg, primary, &lex);
                    if (!cur_output) {
                        return false;
                    }
                }
            } else {
                if (strcmp(primary, "output") == 0) {
                    section = SECTION_OUTPUT;
                    cur_output = get_or_create_output_rule(out_cfg, subkey, &lex);
                    if (!cur_output) {
                        return false;
                    }
                } else if (strcmp(primary, "workspace") == 0) {
                    section = SECTION_WORKSPACE;
                    cur_workspace = get_or_create_workspace_rule(out_cfg, subkey, &lex);
                    if (!cur_workspace) {
                        return false;
                    }
                } else if (strcmp(primary, "template") == 0 || strcmp(primary, "templates") == 0) {
                    section = SECTION_TEMPLATE;
                    cur_template = get_or_create_template_rule(out_cfg, subkey, &lex);
                    if (!cur_template) {
                        return false;
                    }
                } else if (strcmp(primary, "schedule") == 0 || strcmp(primary, "schedules") == 0) {
                    section = SECTION_SCHEDULE;
                    cur_schedule = alloc_schedule_rule(out_cfg, &lex);
                    if (!cur_schedule) {
                        return false;
                    }
                    waywal_solar_phase_t ph;
                    int32_t mins = -1;
                    if (waywal_solar_phase_from_string(subkey, &ph)) {
                        copy_str_safe(cur_schedule->phase, sizeof(cur_schedule->phase), subkey);
                        cur_schedule->has_phase = true;
                    } else if (waywal_parse_hhmm_minutes(subkey, &mins)) {
                        copy_str_safe(cur_schedule->time_hhmm, sizeof(cur_schedule->time_hhmm),
                                      subkey);
                        cur_schedule->time_minutes = mins;
                        cur_schedule->has_time = true;
                    }
                } else {
                    set_error(&lex, "unknown subtable '[%s.\"%s\"]'", primary, subkey);
                    return false;
                }
            }
            continue;
        }

        char key[64];
        if (!parse_key_segment(&lex, key, sizeof(key))) {
            return false;
        }

        skip_horizontal_ws(&lex);
        if (lex.pos >= lex.len || lex.src[lex.pos] != '=') {
            set_error(&lex, "expected '=' after key '%s'", key);
            return false;
        }
        lex.pos++;
        skip_horizontal_ws(&lex);

        if (section == SECTION_DAEMON) {
            if (strcmp(key, "layer") == 0) {
                if (!parse_quoted_string(&lex, out_cfg->daemon.layer,
                                         sizeof(out_cfg->daemon.layer))) {
                    return false;
                }
                if (strcasecmp(out_cfg->daemon.layer, "background") == 0) {
                    out_cfg->daemon.layer_pos = WAYWAL_LAYER_BACKGROUND;
                } else if (strcasecmp(out_cfg->daemon.layer, "bottom") == 0) {
                    out_cfg->daemon.layer_pos = WAYWAL_LAYER_BOTTOM;
                } else {
                    set_error(&lex, "invalid daemon.layer '%s' (expected 'background' or 'bottom')",
                              out_cfg->daemon.layer);
                    return false;
                }
            } else if (strcmp(key, "namespace") == 0) {
                if (!parse_quoted_string(&lex, out_cfg->daemon.namespace_str,
                                         sizeof(out_cfg->daemon.namespace_str))) {
                    return false;
                }
            } else if (strcmp(key, "battery_policy") == 0) {
                if (!parse_quoted_string(&lex, out_cfg->daemon.battery_policy,
                                         sizeof(out_cfg->daemon.battery_policy))) {
                    return false;
                }
                if (strcasecmp(out_cfg->daemon.battery_policy, "ignore") == 0) {
                    out_cfg->daemon.battery_policy_mode = WAYWAL_BATTERY_IGNORE;
                } else if (strcasecmp(out_cfg->daemon.battery_policy, "reduce-fps") == 0) {
                    out_cfg->daemon.battery_policy_mode = WAYWAL_BATTERY_REDUCE_FPS;
                } else if (strcasecmp(out_cfg->daemon.battery_policy, "pause-video") == 0) {
                    out_cfg->daemon.battery_policy_mode = WAYWAL_BATTERY_PAUSE_VIDEO;
                } else {
                    set_error(&lex, "invalid daemon.battery_policy '%s'",
                              out_cfg->daemon.battery_policy);
                    return false;
                }
            } else if (strcmp(key, "auto_pause_fullscreen") == 0 ||
                       strcmp(key, "pause_on_fullscreen") == 0) {
                if (!parse_bool_val(&lex, &out_cfg->daemon.auto_pause_fullscreen))
                    return false;
            } else if (strcmp(key, "auto_pause_maximized") == 0 ||
                       strcmp(key, "pause_on_maximized") == 0) {
                if (!parse_bool_val(&lex, &out_cfg->daemon.auto_pause_maximized))
                    return false;
            } else if (strcmp(key, "idle_timeout_sec") == 0) {
                if (!parse_idle_timeout_val(&lex, &out_cfg->daemon.idle_timeout_sec))
                    return false;
            } else if (strcmp(key, "interval") == 0 || strcmp(key, "duration") == 0) {
                if (!parse_duration_val(&lex, &out_cfg->daemon.slideshow_interval))
                    return false;
                out_cfg->daemon.has_slideshow_interval = true;
            } else if (strcmp(key, "sorting") == 0 || strcmp(key, "sort") == 0 ||
                       strcmp(key, "order") == 0) {
                if (!parse_quoted_string(&lex, out_cfg->daemon.slideshow_sorting,
                                         sizeof(out_cfg->daemon.slideshow_sorting))) {
                    return false;
                }
                if (!waywal_parse_sort_mode(out_cfg->daemon.slideshow_sorting,
                                            &out_cfg->daemon.slideshow_sort)) {
                    set_error(&lex, "invalid sorting mode '%s'", out_cfg->daemon.slideshow_sorting);
                    return false;
                }
                out_cfg->daemon.has_slideshow_sort = true;
                out_cfg->daemon.slideshow_shuffle =
                    (out_cfg->daemon.slideshow_sort == WAYWAL_SORT_RANDOM);
            } else if (strcmp(key, "shuffle") == 0) {
                if (!parse_bool_val(&lex, &out_cfg->daemon.slideshow_shuffle))
                    return false;
                out_cfg->daemon.has_slideshow_shuffle = true;
                if (!out_cfg->daemon.has_slideshow_sort) {
                    out_cfg->daemon.slideshow_sort = out_cfg->daemon.slideshow_shuffle
                                                         ? WAYWAL_SORT_RANDOM
                                                         : WAYWAL_SORT_ASCENDING;
                }
            } else if (strcmp(key, "recursive") == 0) {
                if (!parse_bool_val(&lex, &out_cfg->daemon.slideshow_recursive))
                    return false;
                out_cfg->daemon.has_slideshow_recursive = true;
            } else if (strcmp(key, "ten_bit") == 0 || strcmp(key, "10bit") == 0) {
                if (!parse_bool_val(&lex, &out_cfg->daemon.ten_bit))
                    return false;
            } else if (strcmp(key, "backdrop") == 0) {
                char raw_backdrop[WAYWAL_PATH_MAX];
                if (!parse_quoted_string(&lex, raw_backdrop, sizeof(raw_backdrop))) {
                    return false;
                }
                if (strcasecmp(raw_backdrop, "none") == 0 ||
                    strcasecmp(raw_backdrop, "auto-blur") == 0 ||
                    strcasecmp(raw_backdrop, "auto_blur") == 0) {
                    copy_str_safe(out_cfg->daemon.backdrop, sizeof(out_cfg->daemon.backdrop),
                                  raw_backdrop);
                } else if (!waywal_expand_path(raw_backdrop, out_cfg->daemon.backdrop,
                                               sizeof(out_cfg->daemon.backdrop))) {
                    set_error(&lex, "invalid or unexpandable daemon.backdrop path '%s'",
                              raw_backdrop);
                    return false;
                }
            } else if (strcmp(key, "workshop_dir") == 0 || strcmp(key, "workshop_path") == 0) {
                char raw_ws[WAYWAL_PATH_MAX];
                if (!parse_quoted_string(&lex, raw_ws, sizeof(raw_ws))) {
                    return false;
                }
                if (!waywal_expand_path(raw_ws, out_cfg->daemon.workshop_dir,
                                        sizeof(out_cfg->daemon.workshop_dir))) {
                    set_error(&lex, "invalid or unexpandable daemon.workshop_dir '%s'", raw_ws);
                    return false;
                }
            } else if (strcmp(key, "restore_on_start") == 0) {
                if (!parse_bool_val(&lex, &out_cfg->daemon.restore_on_start))
                    return false;
            } else if (strcmp(key, "pause_on_occluded_workspace") == 0) {
                if (!parse_bool_val(&lex, &out_cfg->daemon.pause_on_occluded_workspace))
                    return false;
            } else if (strcmp(key, "pause_on_idle") == 0) {
                if (!parse_bool_val(&lex, &out_cfg->daemon.pause_on_idle))
                    return false;
            } else if (strcmp(key, "volume") == 0) {
                if (!parse_float_val(&lex, &out_cfg->daemon.volume))
                    return false;
                if (out_cfg->daemon.volume < 0.0f)
                    out_cfg->daemon.volume = 0.0f;
                if (out_cfg->daemon.volume > 1.0f)
                    out_cfg->daemon.volume = 1.0f;
            } else if (strcmp(key, "mute_on_audio") == 0) {
                if (!parse_bool_val(&lex, &out_cfg->daemon.mute_on_audio))
                    return false;
            } else if (strcmp(key, "theme_from_media") == 0) {
                if (!parse_bool_val(&lex, &out_cfg->daemon.theme_from_media))
                    return false;
                out_cfg->theme.theme_from_media = out_cfg->daemon.theme_from_media;
            } else if (strcmp(key, "crop_gravity") == 0 || strcmp(key, "crop-gravity") == 0) {
                if (!parse_quoted_string(&lex, out_cfg->daemon.crop_gravity,
                                         sizeof(out_cfg->daemon.crop_gravity))) {
                    return false;
                }
                if (!waywal_parse_crop_gravity(out_cfg->daemon.crop_gravity,
                                               &out_cfg->daemon.crop_gravity_x,
                                               &out_cfg->daemon.crop_gravity_y)) {
                    set_error(&lex, "invalid daemon.crop_gravity '%s'",
                              out_cfg->daemon.crop_gravity);
                    return false;
                }
                out_cfg->daemon.has_crop_gravity = true;
            } else if (strcmp(key, "auto_day_night") == 0 || strcmp(key, "auto-day-night") == 0 ||
                       strcmp(key, "auto_theme_mode") == 0) {
                bool adn = false;
                if (!parse_bool_val(&lex, &adn))
                    return false;
                out_cfg->auto_day_night = adn;
                out_cfg->daemon.auto_day_night = adn;
                out_cfg->theme.auto_day_night = adn;
                out_cfg->solar.auto_day_night = adn;
            } else {
                set_error(&lex, "unknown key '%s' in [daemon]", key);
                return false;
            }
        } else if (section == SECTION_THEME) {
            if (strcmp(key, "enabled") == 0 || strcmp(key, "enable") == 0) {
                if (!parse_bool_val(&lex, &out_cfg->theme.enabled))
                    return false;
            } else if (strcmp(key, "mode") == 0) {
                if (!parse_quoted_string(&lex, out_cfg->theme.mode, sizeof(out_cfg->theme.mode))) {
                    return false;
                }
                if (strcasecmp(out_cfg->theme.mode, "dark") == 0) {
                    out_cfg->theme.theme_mode = WAYWAL_THEME_DARK;
                } else if (strcasecmp(out_cfg->theme.mode, "light") == 0) {
                    out_cfg->theme.theme_mode = WAYWAL_THEME_LIGHT;
                } else if (strcasecmp(out_cfg->theme.mode, "auto") == 0) {
                    out_cfg->auto_day_night = true;
                    out_cfg->daemon.auto_day_night = true;
                    out_cfg->theme.auto_day_night = true;
                    out_cfg->solar.auto_day_night = true;
                } else {
                    set_error(&lex, "invalid theme.mode '%s' (expected 'dark', 'light', or 'auto')",
                              out_cfg->theme.mode);
                    return false;
                }
            } else if (strcmp(key, "auto_day_night") == 0 || strcmp(key, "auto-day-night") == 0 ||
                       strcmp(key, "auto_theme_mode") == 0) {
                bool adn = false;
                if (!parse_bool_val(&lex, &adn))
                    return false;
                out_cfg->auto_day_night = adn;
                out_cfg->daemon.auto_day_night = adn;
                out_cfg->theme.auto_day_night = adn;
                out_cfg->solar.auto_day_night = adn;
            } else if (strcmp(key, "caelestia_sync") == 0) {
                if (!parse_bool_val(&lex, &out_cfg->theme.caelestia_sync))
                    return false;
            } else if (strcmp(key, "tty_sequences") == 0 || strcmp(key, "terminal_osc") == 0) {
                if (!parse_bool_val(&lex, &out_cfg->theme.tty_sequences))
                    return false;
            } else if (strcmp(key, "scheme_variant") == 0) {
                if (!parse_quoted_string(&lex, out_cfg->theme.scheme_variant,
                                         sizeof(out_cfg->theme.scheme_variant))) {
                    return false;
                }
            } else if (strcmp(key, "contrast") == 0) {
                if (!parse_float_val(&lex, &out_cfg->theme.contrast))
                    return false;
            } else if (strcmp(key, "export_css") == 0) {
                if (!parse_bool_val(&lex, &out_cfg->theme.export_css))
                    return false;
            } else if (strcmp(key, "export_hyprland") == 0) {
                if (!parse_bool_val(&lex, &out_cfg->theme.export_hyprland))
                    return false;
            } else if (strcmp(key, "theme_from_media") == 0) {
                if (!parse_bool_val(&lex, &out_cfg->theme.theme_from_media))
                    return false;
                out_cfg->daemon.theme_from_media = out_cfg->theme.theme_from_media;
            } else if (strcmp(key, "source_color_index") == 0 ||
                       strcmp(key, "source-color-index") == 0) {
                uint32_t idx = 0;
                if (!parse_uint32_val(&lex, &idx))
                    return false;
                if (idx > 3U) {
                    set_error(&lex, "invalid theme.source_color_index '%u' (expected 0..3)", idx);
                    return false;
                }
                out_cfg->theme.source_color_index = (uint8_t)idx;
            } else if (strcmp(key, "seed_color") == 0 || strcmp(key, "seed-color") == 0) {
                uint32_t rgb = 0;
                if (!parse_rgb24_color_val(&lex, &rgb))
                    return false;
                out_cfg->theme.seed_color = rgb;
                out_cfg->theme.has_seed_color = true;
            } else if (strcmp(key, "pywal_compat") == 0 || strcmp(key, "export_pywal") == 0) {
                if (!parse_bool_val(&lex, &out_cfg->theme.pywal_compat))
                    return false;
            } else {
                set_error(&lex, "unknown key '%s' in [theme]", key);
                return false;
            }
        } else if (section == SECTION_TRANSITION) {
            if (strcmp(key, "type") == 0) {
                if (!parse_quoted_string(&lex, out_cfg->transition.type,
                                         sizeof(out_cfg->transition.type))) {
                    return false;
                }
                out_cfg->transition.type_enum = parse_transition_type_str(out_cfg->transition.type);
                if (strcasecmp(out_cfg->transition.type, "left") == 0) {
                    out_cfg->transition.angle = 180.0f;
                } else if (strcasecmp(out_cfg->transition.type, "right") == 0) {
                    out_cfg->transition.angle = 0.0f;
                } else if (strcasecmp(out_cfg->transition.type, "top") == 0) {
                    out_cfg->transition.angle = 90.0f;
                } else if (strcasecmp(out_cfg->transition.type, "bottom") == 0) {
                    out_cfg->transition.angle = 270.0f;
                } else if (strcasecmp(out_cfg->transition.type, "center") == 0) {
                    out_cfg->transition.pos_x = 0.5f;
                    out_cfg->transition.pos_y = 0.5f;
                    out_cfg->transition.pos_y_inverted = false;
                }
            } else if (strcmp(key, "duration") == 0) {
                if (!parse_float_duration_val(&lex, &out_cfg->transition.duration))
                    return false;
            } else if (strcmp(key, "fps") == 0) {
                if (!parse_uint32_val(&lex, &out_cfg->transition.fps))
                    return false;
            } else if (strcmp(key, "bezier") == 0) {
                if (!parse_float_array(&lex, out_cfg->transition.bezier, 4))
                    return false;
            } else if (strcmp(key, "angle") == 0) {
                if (!parse_float_val(&lex, &out_cfg->transition.angle))
                    return false;
            } else if (strcmp(key, "pos") == 0) {
                if (!parse_quoted_string(&lex, out_cfg->transition.pos,
                                         sizeof(out_cfg->transition.pos))) {
                    return false;
                }
                apply_pos_string(&out_cfg->transition);
            } else if (strcmp(key, "pos_x") == 0) {
                if (!parse_float_val(&lex, &out_cfg->transition.pos_x))
                    return false;
            } else if (strcmp(key, "pos_y") == 0) {
                if (!parse_float_val(&lex, &out_cfg->transition.pos_y))
                    return false;
                out_cfg->transition.pos_y_inverted = false;
            } else if (strcmp(key, "wave_x") == 0) {
                if (!parse_float_val(&lex, &out_cfg->transition.wave_x))
                    return false;
            } else if (strcmp(key, "wave_y") == 0) {
                if (!parse_float_val(&lex, &out_cfg->transition.wave_y))
                    return false;
            } else if (strcmp(key, "step") == 0) {
                if (!parse_uint32_val(&lex, &out_cfg->transition.step))
                    return false;
            } else if (strcmp(key, "sync_mode") == 0) {
                if (!parse_quoted_string(&lex, out_cfg->transition.sync_mode,
                                         sizeof(out_cfg->transition.sync_mode))) {
                    return false;
                }
            } else if (strcmp(key, "stagger_delay_ms") == 0) {
                if (!parse_uint32_val(&lex, &out_cfg->transition.stagger_delay_ms))
                    return false;
            } else if (strcmp(key, "crop_gravity") == 0 || strcmp(key, "crop-gravity") == 0) {
                if (!parse_quoted_string(&lex, out_cfg->transition.crop_gravity,
                                         sizeof(out_cfg->transition.crop_gravity))) {
                    return false;
                }
                if (!waywal_parse_crop_gravity(out_cfg->transition.crop_gravity,
                                               &out_cfg->transition.crop_gravity_x,
                                               &out_cfg->transition.crop_gravity_y)) {
                    set_error(&lex, "invalid transition.crop_gravity '%s'",
                              out_cfg->transition.crop_gravity);
                    return false;
                }
                out_cfg->transition.has_crop_gravity = true;
            } else if (strcmp(key, "invert_y") == 0 || strcmp(key, "invert-y") == 0) {
                if (!parse_bool_val(&lex, &out_cfg->transition.invert_y))
                    return false;
            } else {
                set_error(&lex, "unknown key '%s' in [transition]", key);
                return false;
            }
        } else if (section == SECTION_HOOKS) {
            if (strcmp(key, "on_theme_change") == 0) {
                if (!parse_quoted_string(&lex, out_cfg->hooks.on_theme_change,
                                         sizeof(out_cfg->hooks.on_theme_change))) {
                    return false;
                }
            } else if (strcmp(key, "on_wallpaper_change") == 0) {
                if (!parse_quoted_string(&lex, out_cfg->hooks.on_wallpaper_change,
                                         sizeof(out_cfg->hooks.on_wallpaper_change))) {
                    return false;
                }
            } else {
                set_error(&lex, "unknown key '%s' in [hooks]", key);
                return false;
            }
        } else if (section == SECTION_OUTPUT && cur_output) {
            if (strcmp(key, "path") == 0 || strcmp(key, "image") == 0 ||
                strcmp(key, "media_path") == 0) {
                char raw_out_path[WAYWAL_PATH_MAX];
                if (!parse_quoted_string(&lex, raw_out_path, sizeof(raw_out_path))) {
                    return false;
                }
                if (!waywal_expand_path(raw_out_path, cur_output->path,
                                        sizeof(cur_output->path))) {
                    set_error(&lex, "invalid or unexpandable output.path '%s'", raw_out_path);
                    return false;
                }
                cur_output->has_path = true;
            } else if (strcmp(key, "mode") == 0) {
                char raw_mode[32];
                if (!parse_quoted_string(&lex, raw_mode, sizeof(raw_mode))) {
                    return false;
                }
                if (is_wpaperd_scaling_mode_str(raw_mode)) {
                    copy_str_safe(cur_output->scaling_str, sizeof(cur_output->scaling_str),
                                  raw_mode);
                    cur_output->scaling = parse_scaling_str(raw_mode);
                    cur_output->has_scaling = true;
                } else {
                    copy_str_safe(cur_output->mode, sizeof(cur_output->mode), raw_mode);
                    cur_output->has_mode = true;
                }
            } else if (strcmp(key, "scaling") == 0) {
                if (!parse_quoted_string(&lex, cur_output->scaling_str,
                                         sizeof(cur_output->scaling_str))) {
                    return false;
                }
                cur_output->scaling = parse_scaling_str(cur_output->scaling_str);
                cur_output->has_scaling = true;
            } else if (strcmp(key, "filter") == 0) {
                if (!parse_quoted_string(&lex, cur_output->filter_str,
                                         sizeof(cur_output->filter_str))) {
                    return false;
                }
                cur_output->filter = parse_filter_str(cur_output->filter_str);
                cur_output->has_filter = true;
            } else if (strcmp(key, "fill_color") == 0 || strcmp(key, "color") == 0) {
                if (!parse_color_val(&lex, &cur_output->fill_color)) {
                    return false;
                }
                cur_output->has_fill_color = true;
                if (strcmp(key, "color") == 0) {
                    cur_output->has_explicit_color_key = true;
                }
            } else if (strcmp(key, "fps") == 0) {
                if (!parse_uint32_val(&lex, &cur_output->fps)) {
                    return false;
                }
                cur_output->has_fps = true;
            } else if (strcmp(key, "interval") == 0 || strcmp(key, "duration") == 0) {
                if (!parse_duration_val(&lex, &cur_output->interval)) {
                    return false;
                }
                cur_output->has_interval = true;
            } else if (strcmp(key, "sorting") == 0 || strcmp(key, "sort") == 0 ||
                       strcmp(key, "order") == 0) {
                if (!parse_quoted_string(&lex, cur_output->sorting, sizeof(cur_output->sorting))) {
                    return false;
                }
                if (!waywal_parse_sort_mode(cur_output->sorting, &cur_output->sort_mode)) {
                    set_error(&lex, "invalid output.sorting '%s'", cur_output->sorting);
                    return false;
                }
                cur_output->has_sort_mode = true;
                cur_output->shuffle = (cur_output->sort_mode == WAYWAL_SORT_RANDOM);
                cur_output->has_shuffle = true;
            } else if (strcmp(key, "shuffle") == 0) {
                if (!parse_bool_val(&lex, &cur_output->shuffle)) {
                    return false;
                }
                cur_output->has_shuffle = true;
                if (!cur_output->has_sort_mode) {
                    cur_output->sort_mode =
                        cur_output->shuffle ? WAYWAL_SORT_RANDOM : WAYWAL_SORT_ASCENDING;
                }
            } else if (strcmp(key, "recursive") == 0) {
                if (!parse_bool_val(&lex, &cur_output->recursive)) {
                    return false;
                }
                cur_output->has_recursive = true;
            } else if (strcmp(key, "audio") == 0) {
                if (!parse_bool_val(&lex, &cur_output->audio)) {
                    return false;
                }
                cur_output->has_audio = true;
            } else if (strcmp(key, "volume") == 0) {
                if (!parse_float_val(&lex, &cur_output->volume)) {
                    return false;
                }
                if (cur_output->volume < 0.0f)
                    cur_output->volume = 0.0f;
                if (cur_output->volume > 1.0f)
                    cur_output->volume = 1.0f;
                cur_output->has_volume = true;
            } else if (strcmp(key, "mute_on_audio") == 0) {
                if (!parse_bool_val(&lex, &cur_output->mute_on_audio)) {
                    return false;
                }
                cur_output->has_mute_on_audio = true;
            } else if (strcmp(key, "crop_gravity") == 0 || strcmp(key, "crop-gravity") == 0) {
                if (!parse_quoted_string(&lex, cur_output->crop_gravity,
                                         sizeof(cur_output->crop_gravity))) {
                    return false;
                }
                if (!waywal_parse_crop_gravity(cur_output->crop_gravity,
                                               &cur_output->crop_gravity_x,
                                               &cur_output->crop_gravity_y)) {
                    set_error(&lex, "invalid output.crop_gravity '%s'", cur_output->crop_gravity);
                    return false;
                }
                cur_output->has_crop_gravity = true;
            } else if (strcmp(key, "invert_y") == 0 || strcmp(key, "invert-y") == 0) {
                if (!parse_bool_val(&lex, &cur_output->invert_y)) {
                    return false;
                }
                cur_output->has_invert_y = true;
            } else {
                set_error(&lex, "unknown key '%s' in [output.\"%s\"]", key,
                          cur_output->output_name);
                return false;
            }
        } else if (section == SECTION_WORKSPACE && cur_workspace) {
            if (strcmp(key, "output") == 0 || strcmp(key, "output_name") == 0) {
                if (!parse_quoted_string(&lex, cur_workspace->output_name,
                                         sizeof(cur_workspace->output_name))) {
                    return false;
                }
            } else if (strcmp(key, "path") == 0 || strcmp(key, "image") == 0 ||
                       strcmp(key, "media_path") == 0) {
                char raw_ws_path[WAYWAL_PATH_MAX];
                if (!parse_quoted_string(&lex, raw_ws_path, sizeof(raw_ws_path))) {
                    return false;
                }
                if (!waywal_expand_path(raw_ws_path, cur_workspace->path,
                                        sizeof(cur_workspace->path))) {
                    set_error(&lex, "invalid or unexpandable workspace.path '%s'", raw_ws_path);
                    return false;
                }
            } else if (strcmp(key, "mode") == 0) {
                char raw_mode[32];
                if (!parse_quoted_string(&lex, raw_mode, sizeof(raw_mode))) {
                    return false;
                }
                if (is_wpaperd_scaling_mode_str(raw_mode)) {
                    copy_str_safe(cur_workspace->scaling_str, sizeof(cur_workspace->scaling_str),
                                  raw_mode);
                    cur_workspace->scaling = parse_scaling_str(raw_mode);
                    cur_workspace->has_scaling = true;
                } else {
                    copy_str_safe(cur_workspace->mode, sizeof(cur_workspace->mode), raw_mode);
                    cur_workspace->has_mode = true;
                }
            } else if (strcmp(key, "scaling") == 0) {
                if (!parse_quoted_string(&lex, cur_workspace->scaling_str,
                                         sizeof(cur_workspace->scaling_str))) {
                    return false;
                }
                cur_workspace->scaling = parse_scaling_str(cur_workspace->scaling_str);
                cur_workspace->has_scaling = true;
            } else if (strcmp(key, "filter") == 0) {
                if (!parse_quoted_string(&lex, cur_workspace->filter_str,
                                         sizeof(cur_workspace->filter_str))) {
                    return false;
                }
                cur_workspace->filter = parse_filter_str(cur_workspace->filter_str);
                cur_workspace->has_filter = true;
            } else if (strcmp(key, "fill_color") == 0 || strcmp(key, "color") == 0) {
                if (!parse_color_val(&lex, &cur_workspace->fill_color)) {
                    return false;
                }
                cur_workspace->has_fill_color = true;
            } else if (strcmp(key, "interval") == 0 || strcmp(key, "duration") == 0) {
                if (!parse_duration_val(&lex, &cur_workspace->interval)) {
                    return false;
                }
                cur_workspace->has_interval = true;
            } else if (strcmp(key, "sorting") == 0 || strcmp(key, "sort") == 0 ||
                       strcmp(key, "order") == 0) {
                if (!parse_quoted_string(&lex, cur_workspace->sorting,
                                         sizeof(cur_workspace->sorting))) {
                    return false;
                }
                if (!waywal_parse_sort_mode(cur_workspace->sorting, &cur_workspace->sort_mode)) {
                    set_error(&lex, "invalid workspace.sorting '%s'", cur_workspace->sorting);
                    return false;
                }
                cur_workspace->has_sort_mode = true;
                cur_workspace->shuffle = (cur_workspace->sort_mode == WAYWAL_SORT_RANDOM);
            } else if (strcmp(key, "shuffle") == 0) {
                if (!parse_bool_val(&lex, &cur_workspace->shuffle)) {
                    return false;
                }
                if (!cur_workspace->has_sort_mode) {
                    cur_workspace->sort_mode =
                        cur_workspace->shuffle ? WAYWAL_SORT_RANDOM : WAYWAL_SORT_ASCENDING;
                    copy_str_safe(cur_workspace->sorting, sizeof(cur_workspace->sorting),
                                  cur_workspace->shuffle ? "random" : "ascending");
                }
            } else if (strcmp(key, "recursive") == 0) {
                if (!parse_bool_val(&lex, &cur_workspace->recursive)) {
                    return false;
                }
                cur_workspace->has_recursive = true;
            } else if (strcmp(key, "fps") == 0) {
                if (!parse_uint32_val(&lex, &cur_workspace->fps)) {
                    return false;
                }
                cur_workspace->has_fps = true;
            } else if (strcmp(key, "audio") == 0) {
                if (!parse_bool_val(&lex, &cur_workspace->audio)) {
                    return false;
                }
                cur_workspace->has_audio = true;
            } else if (strcmp(key, "transition_type") == 0 || strcmp(key, "transition") == 0) {
                if (!parse_quoted_string(&lex, cur_workspace->transition_type,
                                         sizeof(cur_workspace->transition_type))) {
                    return false;
                }
                cur_workspace->transition_type_enum =
                    parse_transition_type_str(cur_workspace->transition_type);
                cur_workspace->has_transition_type = true;
                if (strcasecmp(cur_workspace->transition_type, "left") == 0) {
                    cur_workspace->transition_angle = 180.0f;
                    cur_workspace->has_transition_angle = true;
                } else if (strcasecmp(cur_workspace->transition_type, "right") == 0) {
                    cur_workspace->transition_angle = 0.0f;
                    cur_workspace->has_transition_angle = true;
                } else if (strcasecmp(cur_workspace->transition_type, "top") == 0) {
                    cur_workspace->transition_angle = 90.0f;
                    cur_workspace->has_transition_angle = true;
                } else if (strcasecmp(cur_workspace->transition_type, "bottom") == 0) {
                    cur_workspace->transition_angle = 270.0f;
                    cur_workspace->has_transition_angle = true;
                }
            } else if (strcmp(key, "transition_duration") == 0) {
                if (!parse_float_duration_val(&lex, &cur_workspace->transition_duration)) {
                    return false;
                }
                cur_workspace->has_transition_duration = true;
            } else if (strcmp(key, "transition_angle") == 0 || strcmp(key, "angle") == 0) {
                if (!parse_float_val(&lex, &cur_workspace->transition_angle)) {
                    return false;
                }
                cur_workspace->has_transition_angle = true;
            } else if (strcmp(key, "directional") == 0) {
                if (!parse_bool_val(&lex, &cur_workspace->directional)) {
                    return false;
                }
            } else if (strcmp(key, "crop_gravity") == 0 || strcmp(key, "crop-gravity") == 0) {
                if (!parse_quoted_string(&lex, cur_workspace->crop_gravity,
                                         sizeof(cur_workspace->crop_gravity))) {
                    return false;
                }
                if (!waywal_parse_crop_gravity(cur_workspace->crop_gravity,
                                               &cur_workspace->crop_gravity_x,
                                               &cur_workspace->crop_gravity_y)) {
                    set_error(&lex, "invalid workspace.crop_gravity '%s'",
                              cur_workspace->crop_gravity);
                    return false;
                }
                cur_workspace->has_crop_gravity = true;
            } else if (strcmp(key, "props") == 0 || strcmp(key, "prop_overrides") == 0) {
                if (!parse_quoted_string(&lex, cur_workspace->prop_overrides,
                                         sizeof(cur_workspace->prop_overrides))) {
                    return false;
                }
            } else {
                set_error(&lex, "unknown key '%s' in [workspace.\"%s\"]", key,
                          cur_workspace->workspace_name);
                return false;
            }
        } else if (section == SECTION_TEMPLATE && cur_template) {
            if (strcmp(key, "name") == 0) {
                if (!parse_quoted_string(&lex, cur_template->name, sizeof(cur_template->name))) {
                    return false;
                }
            } else if (strcmp(key, "input_path") == 0 || strcmp(key, "input") == 0 ||
                       strcmp(key, "template") == 0 || strcmp(key, "src") == 0) {
                char raw_path[1024];
                if (!parse_quoted_string(&lex, raw_path, sizeof(raw_path))) {
                    return false;
                }
                if (!waywal_expand_path(raw_path, cur_template->input_path,
                                         sizeof(cur_template->input_path))) {
                    set_error(&lex, "expanded input_path exceeds maximum length");
                    return false;
                }
            } else if (strcmp(key, "output_path") == 0 || strcmp(key, "output") == 0 ||
                       strcmp(key, "target") == 0 || strcmp(key, "dst") == 0) {
                char raw_path[1024];
                if (!parse_quoted_string(&lex, raw_path, sizeof(raw_path))) {
                    return false;
                }
                if (!waywal_expand_path(raw_path, cur_template->output_path,
                                         sizeof(cur_template->output_path))) {
                    set_error(&lex, "expanded output_path exceeds maximum length");
                    return false;
                }
            } else if (strcmp(key, "post_hook") == 0 || strcmp(key, "hook") == 0) {
                if (!parse_quoted_string(&lex, cur_template->post_hook,
                                         sizeof(cur_template->post_hook))) {
                    return false;
                }
            } else if (strcmp(key, "enabled") == 0 || strcmp(key, "enable") == 0) {
                if (!parse_bool_val(&lex, &cur_template->enabled)) {
                    return false;
                }
            } else {
                set_error(&lex, "unknown key '%s' in [template.\"%s\"]", key, cur_template->name);
                return false;
            }
        } else if (section == SECTION_SOLAR) {
            if (strcmp(key, "enabled") == 0 || strcmp(key, "enable") == 0) {
                if (!parse_bool_val(&lex, &out_cfg->solar.enabled))
                    return false;
            } else if (strcmp(key, "latitude") == 0 || strcmp(key, "lat") == 0) {
                double lat = 0.0;
                if (!parse_double_val(&lex, &lat) || lat < -90.0 || lat > 90.0) {
                    set_error(&lex, "invalid solar.latitude (expected -90.0..90.0)");
                    return false;
                }
                out_cfg->solar.latitude = lat;
                has_solar_lat = true;
                out_cfg->solar.has_coordinates = (has_solar_lat && has_solar_lon);
            } else if (strcmp(key, "longitude") == 0 || strcmp(key, "lon") == 0 ||
                       strcmp(key, "lng") == 0) {
                double lon = 0.0;
                if (!parse_double_val(&lex, &lon) || lon < -180.0 || lon > 180.0) {
                    set_error(&lex, "invalid solar.longitude (expected -180.0..180.0)");
                    return false;
                }
                out_cfg->solar.longitude = lon;
                has_solar_lon = true;
                out_cfg->solar.has_coordinates = (has_solar_lat && has_solar_lon);
            } else if (strcmp(key, "auto_day_night") == 0 || strcmp(key, "auto-day-night") == 0 ||
                       strcmp(key, "auto_theme_mode") == 0) {
                bool adn = false;
                if (!parse_bool_val(&lex, &adn))
                    return false;
                out_cfg->solar.auto_day_night = adn;
                out_cfg->theme.auto_day_night = adn;
                out_cfg->daemon.auto_day_night = adn;
                out_cfg->auto_day_night = adn;
            } else if (strcmp(key, "dawn_path") == 0 || strcmp(key, "dawn") == 0) {
                char raw_p[WAYWAL_PATH_MAX];
                if (!parse_quoted_string(&lex, raw_p, sizeof(raw_p)) ||
                    !waywal_expand_path(raw_p, out_cfg->solar.dawn_path,
                                        sizeof(out_cfg->solar.dawn_path))) {
                    set_error(&lex, "invalid solar.dawn_path");
                    return false;
                }
            } else if (strcmp(key, "day_path") == 0 || strcmp(key, "day") == 0) {
                char raw_p[WAYWAL_PATH_MAX];
                if (!parse_quoted_string(&lex, raw_p, sizeof(raw_p)) ||
                    !waywal_expand_path(raw_p, out_cfg->solar.day_path,
                                        sizeof(out_cfg->solar.day_path))) {
                    set_error(&lex, "invalid solar.day_path");
                    return false;
                }
            } else if (strcmp(key, "dusk_path") == 0 || strcmp(key, "dusk") == 0) {
                char raw_p[WAYWAL_PATH_MAX];
                if (!parse_quoted_string(&lex, raw_p, sizeof(raw_p)) ||
                    !waywal_expand_path(raw_p, out_cfg->solar.dusk_path,
                                        sizeof(out_cfg->solar.dusk_path))) {
                    set_error(&lex, "invalid solar.dusk_path");
                    return false;
                }
            } else if (strcmp(key, "night_path") == 0 || strcmp(key, "night") == 0) {
                char raw_p[WAYWAL_PATH_MAX];
                if (!parse_quoted_string(&lex, raw_p, sizeof(raw_p)) ||
                    !waywal_expand_path(raw_p, out_cfg->solar.night_path,
                                        sizeof(out_cfg->solar.night_path))) {
                    set_error(&lex, "invalid solar.night_path");
                    return false;
                }
            } else if (strcmp(key, "manifest_path") == 0 || strcmp(key, "manifest") == 0 ||
                       strcmp(key, "path") == 0) {
                char raw_p[WAYWAL_PATH_MAX];
                if (!parse_quoted_string(&lex, raw_p, sizeof(raw_p)) ||
                    !waywal_expand_path(raw_p, out_cfg->solar.manifest_path,
                                        sizeof(out_cfg->solar.manifest_path))) {
                    set_error(&lex, "invalid solar.manifest_path");
                    return false;
                }
            } else if (strcmp(key, "sunrise") == 0) {
                if (!parse_quoted_string(&lex, out_cfg->solar.sunrise_str,
                                         sizeof(out_cfg->solar.sunrise_str)) ||
                    !waywal_parse_hhmm_minutes(out_cfg->solar.sunrise_str,
                                               &out_cfg->solar.sunrise_fallback_min)) {
                    set_error(&lex, "invalid solar.sunrise (expected HH:MM)");
                    return false;
                }
            } else if (strcmp(key, "sunset") == 0) {
                if (!parse_quoted_string(&lex, out_cfg->solar.sunset_str,
                                         sizeof(out_cfg->solar.sunset_str)) ||
                    !waywal_parse_hhmm_minutes(out_cfg->solar.sunset_str,
                                               &out_cfg->solar.sunset_fallback_min)) {
                    set_error(&lex, "invalid solar.sunset (expected HH:MM)");
                    return false;
                }
            } else {
                set_error(&lex, "unknown key '%s' in [solar]", key);
                return false;
            }
        } else if (section == SECTION_SCHEDULE) {
            waywal_solar_phase_t shorthand_ph;
            int32_t shorthand_mins = -1;
            bool is_shorthand_ph =
                (!cur_schedule && strcmp(key, "phase") != 0 && strcmp(key, "time") != 0 &&
                 strcmp(key, "start") != 0 && strcmp(key, "path") != 0 &&
                 strcmp(key, "image") != 0 && strcmp(key, "file") != 0 &&
                 strcmp(key, "mode") != 0 && strcmp(key, "output") != 0 &&
                 strcmp(key, "theme_mode") != 0 &&
                 waywal_solar_phase_from_string(key, &shorthand_ph));
            bool is_shorthand_tm =
                (!cur_schedule && !is_shorthand_ph &&
                 waywal_parse_hhmm_minutes(key, &shorthand_mins));

            if (is_shorthand_ph || is_shorthand_tm) {
                char raw_p[WAYWAL_PATH_MAX];
                if (!parse_quoted_string(&lex, raw_p, sizeof(raw_p))) {
                    return false;
                }
                waywal_schedule_rule_t *r = alloc_schedule_rule(out_cfg, &lex);
                if (!r)
                    return false;
                if (!waywal_expand_path(raw_p, r->path, sizeof(r->path))) {
                    set_error(&lex, "invalid schedule path '%s'", raw_p);
                    return false;
                }
                if (is_shorthand_ph) {
                    copy_str_safe(r->phase, sizeof(r->phase), key);
                    r->has_phase = true;
                } else {
                    copy_str_safe(r->time_hhmm, sizeof(r->time_hhmm), key);
                    r->time_minutes = shorthand_mins;
                    r->has_time = true;
                }
            } else {
                if (!cur_schedule) {
                    cur_schedule = alloc_schedule_rule(out_cfg, &lex);
                    if (!cur_schedule)
                        return false;
                }
                if (strcmp(key, "phase") == 0) {
                    if (!parse_quoted_string(&lex, cur_schedule->phase,
                                             sizeof(cur_schedule->phase))) {
                        return false;
                    }
                    waywal_solar_phase_t ph;
                    if (!waywal_solar_phase_from_string(cur_schedule->phase, &ph)) {
                        set_error(&lex, "invalid schedule.phase '%s'", cur_schedule->phase);
                        return false;
                    }
                    cur_schedule->has_phase = true;
                } else if (strcmp(key, "time") == 0 || strcmp(key, "start") == 0) {
                    if (!parse_quoted_string(&lex, cur_schedule->time_hhmm,
                                             sizeof(cur_schedule->time_hhmm)) ||
                        !waywal_parse_hhmm_minutes(cur_schedule->time_hhmm,
                                                   &cur_schedule->time_minutes)) {
                        set_error(&lex, "invalid schedule.%s '%s' (expected HH:MM)", key,
                                  cur_schedule->time_hhmm);
                        return false;
                    }
                    cur_schedule->has_time = true;
                } else if (strcmp(key, "path") == 0 || strcmp(key, "image") == 0 ||
                           strcmp(key, "file") == 0 || strcmp(key, "media_path") == 0) {
                    char raw_p[WAYWAL_PATH_MAX];
                    if (!parse_quoted_string(&lex, raw_p, sizeof(raw_p)) ||
                        !waywal_expand_path(raw_p, cur_schedule->path,
                                            sizeof(cur_schedule->path))) {
                        set_error(&lex, "invalid schedule.path '%s'", raw_p);
                        return false;
                    }
                } else if (strcmp(key, "mode") == 0) {
                    if (!parse_quoted_string(&lex, cur_schedule->mode,
                                             sizeof(cur_schedule->mode))) {
                        return false;
                    }
                    if (strcasecmp(cur_schedule->mode, "light") == 0) {
                        copy_str_safe(cur_schedule->theme_mode, sizeof(cur_schedule->theme_mode),
                                      "light");
                        cur_schedule->has_theme_mode = true;
                        cur_schedule->theme_light_mode = true;
                    } else if (strcasecmp(cur_schedule->mode, "dark") == 0) {
                        copy_str_safe(cur_schedule->theme_mode, sizeof(cur_schedule->theme_mode),
                                      "dark");
                        cur_schedule->has_theme_mode = true;
                        cur_schedule->theme_light_mode = false;
                    } else if (strcasecmp(cur_schedule->mode, "auto") == 0) {
                        copy_str_safe(cur_schedule->theme_mode, sizeof(cur_schedule->theme_mode),
                                      "auto");
                        cur_schedule->has_theme_mode = false;
                    }
                } else if (strcmp(key, "output") == 0 || strcmp(key, "output_name") == 0) {
                    if (!parse_quoted_string(&lex, cur_schedule->output,
                                             sizeof(cur_schedule->output))) {
                        return false;
                    }
                } else if (strcmp(key, "theme_mode") == 0) {
                    if (!parse_quoted_string(&lex, cur_schedule->theme_mode,
                                             sizeof(cur_schedule->theme_mode))) {
                        return false;
                    }
                    if (strcasecmp(cur_schedule->theme_mode, "light") == 0) {
                        cur_schedule->has_theme_mode = true;
                        cur_schedule->theme_light_mode = true;
                    } else if (strcasecmp(cur_schedule->theme_mode, "dark") == 0) {
                        cur_schedule->has_theme_mode = true;
                        cur_schedule->theme_light_mode = false;
                    } else if (strcasecmp(cur_schedule->theme_mode, "auto") == 0) {
                        cur_schedule->has_theme_mode = false;
                    } else {
                        set_error(&lex, "invalid schedule.theme_mode '%s'",
                                  cur_schedule->theme_mode);
                        return false;
                    }
                } else {
                    set_error(&lex, "unknown key '%s' in [schedule]", key);
                    return false;
                }
            }
        } else {
            set_error(&lex, "key '%s' outside of any table section", key);
            return false;
        }

        if (!consume_line_remainder(&lex)) {
            return false;
        }
    }

    if (out_cfg->transition.invert_y && !out_cfg->transition.pos_y_inverted &&
        out_cfg->transition.pos_y >= 0.0f && out_cfg->transition.pos_y <= 1.0f) {
        out_cfg->transition.pos_y = 1.0f - out_cfg->transition.pos_y;
        out_cfg->transition.pos_y_inverted = true;
    }

    const waywal_output_rule_t *wild_out = NULL;
    for (size_t i = 0; i < out_cfg->output_count; ++i) {
        if (strcmp(out_cfg->outputs[i].output_name, "*") == 0) {
            wild_out = &out_cfg->outputs[i];
            break;
        }
    }

    for (size_t i = 0; i < out_cfg->output_count; ++i) {
        waywal_output_rule_t *r = &out_cfg->outputs[i];
        if (!r->has_invert_y) {
            r->invert_y = (wild_out && wild_out != r && wild_out->has_invert_y)
                              ? wild_out->invert_y
                              : out_cfg->transition.invert_y;
        }
        if (wild_out && wild_out != r) {
            bool r_is_standalone_color =
                r->has_explicit_color_key && !r->has_path && !r->has_scaling &&
                (!r->has_mode || strcasecmp(r->mode, "color") == 0);
            bool r_had_own_target = (r->has_path || r_is_standalone_color);
            if (!r_had_own_target && wild_out->has_path) {
                copy_str_safe(r->path, sizeof(r->path), wild_out->path);
                r->has_path = true;
            }
            if (!r->has_scaling && wild_out->has_scaling) {
                r->scaling = wild_out->scaling;
                copy_str_safe(r->scaling_str, sizeof(r->scaling_str), wild_out->scaling_str);
                r->has_scaling = true;
            }
            if (!r->has_filter && wild_out->has_filter) {
                r->filter = wild_out->filter;
                copy_str_safe(r->filter_str, sizeof(r->filter_str), wild_out->filter_str);
                r->has_filter = true;
            }
            if (!r->has_fill_color && wild_out->has_fill_color) {
                r->fill_color = wild_out->fill_color;
                r->has_fill_color = true;
            }
            if (!r->has_fps && wild_out->has_fps) {
                r->fps = wild_out->fps;
                r->has_fps = true;
            }
            if (!r->has_interval && wild_out->has_interval) {
                r->interval = wild_out->interval;
                r->has_interval = true;
            }
            if (!r->has_sort_mode && wild_out->has_sort_mode) {
                r->sort_mode = wild_out->sort_mode;
                copy_str_safe(r->sorting, sizeof(r->sorting), wild_out->sorting);
                r->shuffle = wild_out->shuffle;
                r->has_sort_mode = true;
                r->has_shuffle = true;
            } else if (!r->has_shuffle && wild_out->has_shuffle) {
                r->shuffle = wild_out->shuffle;
                r->sort_mode = r->shuffle ? WAYWAL_SORT_RANDOM : WAYWAL_SORT_ASCENDING;
                r->has_shuffle = true;
            }
            if (!r->has_recursive && wild_out->has_recursive) {
                r->recursive = wild_out->recursive;
                r->has_recursive = true;
            }
            if (!r->has_audio && wild_out->has_audio) {
                r->audio = wild_out->audio;
                r->has_audio = true;
            }
            if (!r->has_volume && wild_out->has_volume) {
                r->volume = wild_out->volume;
                r->has_volume = true;
            }
            if (!r->has_mute_on_audio && wild_out->has_mute_on_audio) {
                r->mute_on_audio = wild_out->mute_on_audio;
                r->has_mute_on_audio = true;
            }
            if (!r->has_crop_gravity && wild_out->has_crop_gravity) {
                copy_str_safe(r->crop_gravity, sizeof(r->crop_gravity), wild_out->crop_gravity);
                r->crop_gravity_x = wild_out->crop_gravity_x;
                r->crop_gravity_y = wild_out->crop_gravity_y;
                r->has_crop_gravity = true;
            }
            if (!r->has_mode && wild_out->has_mode && !r_had_own_target) {
                copy_str_safe(r->mode, sizeof(r->mode), wild_out->mode);
                r->has_mode = true;
            }
        }
        if (!r->has_interval && out_cfg->daemon.has_slideshow_interval) {
            r->interval = out_cfg->daemon.slideshow_interval;
            r->has_interval = true;
        }
        if (!r->has_sort_mode && out_cfg->daemon.has_slideshow_sort) {
            r->sort_mode = out_cfg->daemon.slideshow_sort;
            copy_str_safe(r->sorting, sizeof(r->sorting), out_cfg->daemon.slideshow_sorting);
            r->shuffle = out_cfg->daemon.slideshow_shuffle;
            r->has_sort_mode = true;
            r->has_shuffle = true;
        } else if (!r->has_shuffle && out_cfg->daemon.has_slideshow_shuffle) {
            r->shuffle = out_cfg->daemon.slideshow_shuffle;
            r->sort_mode = r->shuffle ? WAYWAL_SORT_RANDOM : WAYWAL_SORT_ASCENDING;
            r->has_shuffle = true;
        }
        if (!r->has_recursive && out_cfg->daemon.has_slideshow_recursive) {
            r->recursive = out_cfg->daemon.slideshow_recursive;
            r->has_recursive = true;
        }

        if (!r->has_mode) {
            if (r->path[0] == '\0' && r->has_fill_color) {
                copy_str_safe(r->mode, sizeof(r->mode), "color");
            } else if (r->path[0] != '\0') {
                if (is_workshop_dir_cfg(r->path)) {
                    copy_str_safe(r->mode, sizeof(r->mode), "workshop");
                } else if (is_dir_cfg(r->path)) {
                    copy_str_safe(r->mode, sizeof(r->mode), "slideshow");
                } else if (has_video_ext_cfg(r->path)) {
                    copy_str_safe(r->mode, sizeof(r->mode), "video");
                } else if (has_shader_ext_cfg(r->path)) {
                    copy_str_safe(r->mode, sizeof(r->mode), "shader");
                } else if (has_image_ext_cfg(r->path)) {
                    copy_str_safe(r->mode, sizeof(r->mode), "image");
                } else if (r->has_interval || r->has_sort_mode || r->has_shuffle ||
                           !has_any_ext_cfg(r->path)) {
                    copy_str_safe(r->mode, sizeof(r->mode), "slideshow");
                } else {
                    copy_str_safe(r->mode, sizeof(r->mode), "image");
                }
            }
        }
    }

    for (size_t i = 0; i < out_cfg->workspace_count; ++i) {
        waywal_workspace_rule_t *w = &out_cfg->workspaces[i];
        if (!w->has_interval) {
            if (wild_out && wild_out->has_interval) {
                w->interval = wild_out->interval;
            } else if (out_cfg->daemon.has_slideshow_interval) {
                w->interval = out_cfg->daemon.slideshow_interval;
            }
        }
        if (!w->has_sort_mode) {
            if (wild_out && wild_out->has_sort_mode) {
                w->sort_mode = wild_out->sort_mode;
                copy_str_safe(w->sorting, sizeof(w->sorting), wild_out->sorting);
                w->shuffle = wild_out->shuffle;
            } else if (out_cfg->daemon.has_slideshow_sort) {
                w->sort_mode = out_cfg->daemon.slideshow_sort;
                copy_str_safe(w->sorting, sizeof(w->sorting), out_cfg->daemon.slideshow_sorting);
                w->shuffle = out_cfg->daemon.slideshow_shuffle;
            }
        }
        if (!w->has_recursive) {
            if (wild_out && wild_out->has_recursive) {
                w->recursive = wild_out->recursive;
                w->has_recursive = true;
            } else if (out_cfg->daemon.has_slideshow_recursive) {
                w->recursive = out_cfg->daemon.slideshow_recursive;
                w->has_recursive = true;
            }
        }
        if (!w->has_transition_type) {
            copy_str_safe(w->transition_type, sizeof(w->transition_type), out_cfg->transition.type);
            w->transition_type_enum = out_cfg->transition.type_enum;
        }
        if (!w->has_transition_duration) {
            w->transition_duration = out_cfg->transition.duration;
        }
        if (!w->has_transition_angle) {
            w->transition_angle = out_cfg->transition.angle;
        }
        if (!w->has_fps) {
            w->fps = out_cfg->transition.fps;
        }
        if (!w->has_crop_gravity && out_cfg->transition.has_crop_gravity) {
            copy_str_safe(w->crop_gravity, sizeof(w->crop_gravity),
                          out_cfg->transition.crop_gravity);
            w->crop_gravity_x = out_cfg->transition.crop_gravity_x;
            w->crop_gravity_y = out_cfg->transition.crop_gravity_y;
            w->has_crop_gravity = true;
        }
        if (!w->has_mode) {
            if (w->path[0] == '\0' && w->has_fill_color) {
                copy_str_safe(w->mode, sizeof(w->mode), "color");
            } else if (w->path[0] != '\0') {
                if (is_workshop_dir_cfg(w->path)) {
                    copy_str_safe(w->mode, sizeof(w->mode), "workshop");
                } else if (is_dir_cfg(w->path)) {
                    copy_str_safe(w->mode, sizeof(w->mode), "slideshow");
                } else if (has_video_ext_cfg(w->path)) {
                    copy_str_safe(w->mode, sizeof(w->mode), "video");
                } else if (has_shader_ext_cfg(w->path)) {
                    copy_str_safe(w->mode, sizeof(w->mode), "shader");
                } else if (has_image_ext_cfg(w->path)) {
                    copy_str_safe(w->mode, sizeof(w->mode), "image");
                } else if (w->has_interval || w->has_sort_mode || !has_any_ext_cfg(w->path)) {
                    copy_str_safe(w->mode, sizeof(w->mode), "slideshow");
                } else {
                    copy_str_safe(w->mode, sizeof(w->mode), "image");
                }
            }
        }
    }

    return true;
}

bool waywal_config_load_file(const char *file_path, waywal_config_t *out_cfg, char *err_buf,
                             size_t err_cap)
{
    if (!file_path || !out_cfg) {
        return false;
    }

    int fd = open(file_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "cannot open '%s': %s", file_path, strerror(errno));
        }
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0 || st.st_size > (1024 * 1024)) {
        close(fd);
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "invalid file size for '%s'", file_path);
        }
        return false;
    }

    size_t sz = (size_t)st.st_size;
    char *buf = (char *)malloc(sz + 1);
    if (!buf) {
        close(fd);
        return false;
    }

    size_t off = 0;
    while (off < sz) {
        ssize_t r = read(fd, buf + off, sz - off);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            return false;
        }
        if (r == 0)
            break;
        off += (size_t)r;
    }
    buf[off] = '\0';
    close(fd);

    bool ok = waywal_config_parse_string(buf, out_cfg, err_buf, err_cap);
    free(buf);
    return ok;
}

bool waywal_config_load_default(waywal_config_t *out_cfg)
{
    if (!out_cfg) {
        return false;
    }
    waywal_config_init_defaults(out_cfg);

    path_buf_t path;
    path_buf_init(&path);

    const char *env_cfg = getenv("WWAL_CONFIG_PATH");
    if (env_cfg && env_cfg[0] != '\0') {
        path_buf_from_str(&path, env_cfg);
    } else {
        const char *xdg_cfg = getenv("XDG_CONFIG_HOME");
        if (xdg_cfg && xdg_cfg[0] != '\0') {
            path_buf_from_str(&path, xdg_cfg);
            path_buf_push(&path, "wwal/config.toml");
        } else {
            const char *home = getenv("HOME");
            if (!home || home[0] == '\0') {
                return true;
            }
            path_buf_from_str(&path, home);
            path_buf_push(&path, ".config/wwal/config.toml");
        }
    }

    struct stat st;
    if (stat(path_buf_cstr(&path), &st) != 0) {
        return true;
    }

    char err_buf[256] = {0};
    if (!waywal_config_load_file(path_buf_cstr(&path), out_cfg, err_buf, sizeof(err_buf))) {
        WAYWAL_LOG_ERR("Failed to parse %s: %s", path_buf_cstr(&path), err_buf);
        return false;
    }
    return true;
}
