// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/toml_parser.h"

#include "waywal/log.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
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
    if (strcasecmp(s, "fill") == 0 || strcasecmp(s, "crop") == 0)
        return WAYWAL_SCALING_FILL;
    if (strcasecmp(s, "fit") == 0)
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

static bool parse_float_val(toml_lexer_t *lex, float *out_val)
{
    char buf[64];
    if (!parse_number_token(lex, buf, sizeof(buf))) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    float v = strtof(buf, &end);
    if (errno != 0 || !end || *end != '\0') {
        set_error(lex, "invalid float '%s'", buf);
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
        if (hex_len != 6 && hex_len != 8) {
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
        if (hex_len == 6) {
            v = (v << 8) | 0xFFU;
        }
        *out_color = (uint32_t)v;
        return true;
    }
    return parse_uint32_val(lex, out_color);
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
    } else if (strcasecmp(s, "center") == 0 || strcasecmp(s, "middle") == 0) {
        tr->pos_x = 0.5f;
        tr->pos_y = 0.5f;
    } else if (strcasecmp(s, "top") == 0) {
        tr->pos_x = 0.5f;
        tr->pos_y = 0.0f;
    } else if (strcasecmp(s, "bottom") == 0) {
        tr->pos_x = 0.5f;
        tr->pos_y = 1.0f;
    } else if (strcasecmp(s, "left") == 0) {
        tr->pos_x = 0.0f;
        tr->pos_y = 0.5f;
    } else if (strcasecmp(s, "right") == 0) {
        tr->pos_x = 1.0f;
        tr->pos_y = 0.5f;
    } else if (strcasecmp(s, "top-left") == 0 || strcasecmp(s, "topleft") == 0 ||
               strcasecmp(s, "tl") == 0) {
        tr->pos_x = 0.0f;
        tr->pos_y = 0.0f;
    } else if (strcasecmp(s, "top-right") == 0 || strcasecmp(s, "topright") == 0 ||
               strcasecmp(s, "tr") == 0) {
        tr->pos_x = 1.0f;
        tr->pos_y = 0.0f;
    } else if (strcasecmp(s, "bottom-left") == 0 || strcasecmp(s, "bottomleft") == 0 ||
               strcasecmp(s, "bl") == 0) {
        tr->pos_x = 0.0f;
        tr->pos_y = 1.0f;
    } else if (strcasecmp(s, "bottom-right") == 0 || strcasecmp(s, "bottomright") == 0 ||
               strcasecmp(s, "br") == 0) {
        tr->pos_x = 1.0f;
        tr->pos_y = 1.0f;
    } else {
        float px = 0.5f, py = 0.5f;
        if (sscanf(s, "%f,%f", &px, &py) == 2) {
            tr->pos_x = px;
            tr->pos_y = py;
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

static waywal_output_rule_t *get_or_create_output_rule(waywal_config_t *cfg, const char *name,
                                                       toml_lexer_t *lex)
{
    for (size_t i = 0; i < cfg->output_count; ++i) {
        if (strcmp(cfg->outputs[i].output_name, name) == 0) {
            return &cfg->outputs[i];
        }
    }
    if (cfg->output_count >= WAYWAL_CONFIG_MAX_OUTPUTS) {
        set_error(lex, "exceeded maximum output rules (%u)", WAYWAL_CONFIG_MAX_OUTPUTS);
        return NULL;
    }
    waywal_output_rule_t *rule = &cfg->outputs[cfg->output_count++];
    memset(rule, 0, sizeof(*rule));
    copy_str_safe(rule->output_name, sizeof(rule->output_name), name);
    rule->scaling = WAYWAL_SCALING_FILL;
    copy_str_safe(rule->scaling_str, sizeof(rule->scaling_str), "fill");
    rule->filter = WAYWAL_FILTER_LANCZOS3;
    copy_str_safe(rule->filter_str, sizeof(rule->filter_str), "lanczos3");
    rule->fps = cfg->transition.fps;
    copy_str_safe(rule->mode, sizeof(rule->mode), "image");
    rule->interval = 300U;
    rule->shuffle = true;
    rule->audio = false;
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
    copy_str_safe(rule->transition_type, sizeof(rule->transition_type), cfg->transition.type);
    rule->transition_type_enum = cfg->transition.type_enum;
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

    while (lex.pos < lex.len) {
        skip_ws_and_comments(&lex);
        if (lex.pos >= lex.len) {
            break;
        }

        if (lex.src[lex.pos] == '[') {
            lex.pos++;
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
            if (lex.pos >= lex.len || lex.src[lex.pos] != ']') {
                set_error(&lex, "expected ']' after table header");
                return false;
            }
            lex.pos++;

            if (!consume_line_remainder(&lex)) {
                return false;
            }

            cur_output = NULL;
            cur_workspace = NULL;

            if (!has_subkey) {
                if (strcmp(primary, "daemon") == 0 || strcmp(primary, "audio") == 0 ||
                    strcmp(primary, "mpris") == 0) {
                    section = SECTION_DAEMON;
                } else if (strcmp(primary, "theme") == 0) {
                    section = SECTION_THEME;
                } else if (strcmp(primary, "transition") == 0) {
                    section = SECTION_TRANSITION;
                } else if (strcmp(primary, "hooks") == 0) {
                    section = SECTION_HOOKS;
                } else {
                    set_error(&lex, "unknown table '[%s]'", primary);
                    return false;
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
                if (!parse_uint32_val(&lex, &out_cfg->daemon.idle_timeout_sec))
                    return false;
            } else if (strcmp(key, "ten_bit") == 0 || strcmp(key, "10bit") == 0) {
                if (!parse_bool_val(&lex, &out_cfg->daemon.ten_bit))
                    return false;
            } else if (strcmp(key, "backdrop") == 0) {
                if (!parse_quoted_string(&lex, out_cfg->daemon.backdrop,
                                         sizeof(out_cfg->daemon.backdrop))) {
                    return false;
                }
            } else if (strcmp(key, "workshop_dir") == 0 || strcmp(key, "workshop_path") == 0) {
                if (!parse_quoted_string(&lex, out_cfg->daemon.workshop_dir,
                                         sizeof(out_cfg->daemon.workshop_dir))) {
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
                } else {
                    set_error(&lex, "invalid theme.mode '%s' (expected 'dark' or 'light')",
                              out_cfg->theme.mode);
                    return false;
                }
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
                }
            } else if (strcmp(key, "duration") == 0) {
                if (!parse_float_val(&lex, &out_cfg->transition.duration))
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
                if (!parse_quoted_string(&lex, cur_output->path, sizeof(cur_output->path))) {
                    return false;
                }
            } else if (strcmp(key, "mode") == 0) {
                if (!parse_quoted_string(&lex, cur_output->mode, sizeof(cur_output->mode))) {
                    return false;
                }
            } else if (strcmp(key, "scaling") == 0) {
                if (!parse_quoted_string(&lex, cur_output->scaling_str,
                                         sizeof(cur_output->scaling_str))) {
                    return false;
                }
                cur_output->scaling = parse_scaling_str(cur_output->scaling_str);
            } else if (strcmp(key, "filter") == 0) {
                if (!parse_quoted_string(&lex, cur_output->filter_str,
                                         sizeof(cur_output->filter_str))) {
                    return false;
                }
                cur_output->filter = parse_filter_str(cur_output->filter_str);
            } else if (strcmp(key, "fill_color") == 0 || strcmp(key, "color") == 0) {
                if (!parse_color_val(&lex, &cur_output->fill_color)) {
                    return false;
                }
            } else if (strcmp(key, "fps") == 0) {
                if (!parse_uint32_val(&lex, &cur_output->fps)) {
                    return false;
                }
            } else if (strcmp(key, "interval") == 0) {
                if (!parse_uint32_val(&lex, &cur_output->interval)) {
                    return false;
                }
            } else if (strcmp(key, "shuffle") == 0) {
                if (!parse_bool_val(&lex, &cur_output->shuffle)) {
                    return false;
                }
            } else if (strcmp(key, "audio") == 0) {
                if (!parse_bool_val(&lex, &cur_output->audio)) {
                    return false;
                }
            } else if (strcmp(key, "volume") == 0) {
                if (!parse_float_val(&lex, &cur_output->volume)) {
                    return false;
                }
                if (cur_output->volume < 0.0f)
                    cur_output->volume = 0.0f;
                if (cur_output->volume > 1.0f)
                    cur_output->volume = 1.0f;
            } else if (strcmp(key, "mute_on_audio") == 0) {
                if (!parse_bool_val(&lex, &cur_output->mute_on_audio)) {
                    return false;
                }
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
                if (!parse_quoted_string(&lex, cur_workspace->path, sizeof(cur_workspace->path))) {
                    return false;
                }
            } else if (strcmp(key, "transition_type") == 0 || strcmp(key, "transition") == 0) {
                if (!parse_quoted_string(&lex, cur_workspace->transition_type,
                                         sizeof(cur_workspace->transition_type))) {
                    return false;
                }
                cur_workspace->transition_type_enum =
                    parse_transition_type_str(cur_workspace->transition_type);
            } else {
                set_error(&lex, "unknown key '%s' in [workspace.\"%s\"]", key,
                          cur_workspace->workspace_name);
                return false;
            }
        } else {
            set_error(&lex, "key '%s' outside of any table section", key);
            return false;
        }

        if (!consume_line_remainder(&lex)) {
            return false;
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
