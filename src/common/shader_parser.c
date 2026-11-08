// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "waywal/shader_parser.h"

#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

void waywal_shader_meta_init(waywal_shader_meta_t *meta)
{
    if (!meta) {
        return;
    }
    memset(meta, 0, sizeof(*meta));
    meta->global_scale = 1.0f;
    meta->global_fps = 0;
    meta->has_multi_pass = false;

    for (uint32_t p = 0; p < WAYWAL_MAX_SHADER_PASSES; ++p) {
        meta->passes[p].res_scale = 1.0f;
        meta->passes[p].target_fps = 0;
        for (uint32_t c = 0; c < WAYWAL_MAX_SHADER_CHANNELS; ++c) {
            meta->passes[p].channels[c].target = WAYWAL_CHAN_TARGET_NONE;
            meta->passes[p].channels[c].image_path[0] = '\0';
        }
    }
}

const char *waywal_shader_uniform_type_str(waywal_shader_uniform_type_t type)
{
    switch (type) {
    case WAYWAL_UNIFORM_TYPE_FLOAT:
        return "float";
    case WAYWAL_UNIFORM_TYPE_BOOL:
        return "bool";
    case WAYWAL_UNIFORM_TYPE_INT:
        return "int";
    case WAYWAL_UNIFORM_TYPE_VEC2:
        return "vec2";
    case WAYWAL_UNIFORM_TYPE_VEC3:
        return "vec3";
    case WAYWAL_UNIFORM_TYPE_VEC4:
        return "vec4";
    case WAYWAL_UNIFORM_TYPE_COLOR:
        return "vec3";
    default:
        return "float";
    }
}

const waywal_shader_uniform_def_t *waywal_shader_find_uniform(const waywal_shader_meta_t *meta,
                                                              const char *name)
{
    if (!meta || !name || name[0] == '\0') {
        return NULL;
    }
    for (uint32_t i = 0; i < meta->uniform_count; ++i) {
        if (strcmp(meta->uniforms[i].name, name) == 0) {
            return &meta->uniforms[i];
        }
    }
    return NULL;
}

static waywal_shader_uniform_def_t *find_or_add_uniform(waywal_shader_meta_t *meta,
                                                        const char *name)
{
    if (!meta || !name || name[0] == '\0') {
        return NULL;
    }
    for (uint32_t i = 0; i < meta->uniform_count; ++i) {
        if (strcmp(meta->uniforms[i].name, name) == 0) {
            return &meta->uniforms[i];
        }
    }
    if (meta->uniform_count >= WAYWAL_MAX_SHADER_UNIFORMS) {
        return NULL;
    }
    waywal_shader_uniform_def_t *u = &meta->uniforms[meta->uniform_count++];
    memset(u, 0, sizeof(*u));
    snprintf(u->name, sizeof(u->name), "%s", name);
    return u;
}

static const char *skip_ws(const char *s, const char *end)
{
    while (s < end && ((unsigned char)*s <= ' ' || *s == '\t' || *s == '\r')) {
        s++;
    }
    return s;
}

static const char *trim_right(const char *s, const char *end)
{
    while (end > s && ((unsigned char)*(end - 1) <= ' ' || *(end - 1) == '\t' ||
                       *(end - 1) == '\r' || *(end - 1) == '\n')) {
        end--;
    }
    return end;
}

static void make_friendly_label(const char *name, char *out_label, size_t cap)
{
    if (!name || cap == 0) {
        return;
    }
    const char *p = name;
    /* Strip common prefixes like u_, i_, s_ */
    if ((p[0] == 'u' || p[0] == 'i' || p[0] == 's') && p[1] == '_') {
        p += 2;
    }

    size_t out_idx = 0;
    bool capitalize_next = true;

    while (*p && out_idx + 1 < cap) {
        if (*p == '_') {
            if (out_idx > 0 && out_label[out_idx - 1] != ' ') {
                out_label[out_idx++] = ' ';
            }
            capitalize_next = true;
        } else if (capitalize_next) {
            out_label[out_idx++] = (char)toupper((unsigned char)*p);
            capitalize_next = false;
        } else if (isupper((unsigned char)*p) && out_idx > 0 && !isupper((unsigned char)p[-1]) &&
                   out_label[out_idx - 1] != ' ') {
            if (out_idx + 2 < cap) {
                out_label[out_idx++] = ' ';
            }
            out_label[out_idx++] = *p;
        } else {
            out_label[out_idx++] = *p;
        }
        p++;
    }
    out_label[out_idx] = '\0';
}

static int match_pass_section(const char *line, size_t len)
{
    const char *end = line + len;
    const char *s = skip_ws(line, end);
    size_t slen = (size_t)(end - s);

    if (slen >= 2 && s[0] == '/' && s[1] == '/') {
        s += 2;
        s = skip_ws(s, end);
        slen = (size_t)(end - s);

        if (slen >= 8 && strncasecmp(s, "[COMMON]", 8) == 0) {
            return (int)WAYWAL_SHADER_PASS_COMMON;
        }
        if (slen >= 7 && strncasecmp(s, "[IMAGE]", 7) == 0) {
            return (int)WAYWAL_SHADER_PASS_IMAGE;
        }
        if (slen >= 10 && strncasecmp(s, "[BUFFER A]", 10) == 0) {
            return (int)WAYWAL_SHADER_PASS_BUFFER_A;
        }
        if (slen >= 10 && strncasecmp(s, "[BUFFER B]", 10) == 0) {
            return (int)WAYWAL_SHADER_PASS_BUFFER_B;
        }
        if (slen >= 10 && strncasecmp(s, "[BUFFER C]", 10) == 0) {
            return (int)WAYWAL_SHADER_PASS_BUFFER_C;
        }
        if (slen >= 10 && strncasecmp(s, "[BUFFER D]", 10) == 0) {
            return (int)WAYWAL_SHADER_PASS_BUFFER_D;
        }
        if (slen >= 10 && strncasecmp(s, "[BUFFER_A]", 10) == 0) {
            return (int)WAYWAL_SHADER_PASS_BUFFER_A;
        }
        if (slen >= 10 && strncasecmp(s, "[BUFFER_B]", 10) == 0) {
            return (int)WAYWAL_SHADER_PASS_BUFFER_B;
        }
        if (slen >= 10 && strncasecmp(s, "[BUFFER_C]", 10) == 0) {
            return (int)WAYWAL_SHADER_PASS_BUFFER_C;
        }
        if (slen >= 10 && strncasecmp(s, "[BUFFER_D]", 10) == 0) {
            return (int)WAYWAL_SHADER_PASS_BUFFER_D;
        }
        if (slen >= 9 && strncasecmp(s, "[BUFFERA]", 9) == 0) {
            return (int)WAYWAL_SHADER_PASS_BUFFER_A;
        }
        if (slen >= 9 && strncasecmp(s, "[BUFFERB]", 9) == 0) {
            return (int)WAYWAL_SHADER_PASS_BUFFER_B;
        }
        if (slen >= 9 && strncasecmp(s, "[BUFFERC]", 9) == 0) {
            return (int)WAYWAL_SHADER_PASS_BUFFER_C;
        }
        if (slen >= 9 && strncasecmp(s, "[BUFFERD]", 9) == 0) {
            return (int)WAYWAL_SHADER_PASS_BUFFER_D;
        }
    } else if (slen >= 14 && strncasecmp(s, "#pragma waywal", 14) == 0) {
        s += 14;
        s = skip_ws(s, end);
        slen = (size_t)(end - s);
        if (slen >= 5 && strncasecmp(s, "pass", 4) == 0 && (s[4] == ' ' || s[4] == '\t')) {
            s += 4;
            s = skip_ws(s, end);
            slen = (size_t)(end - s);
            if (slen >= 6 && strncasecmp(s, "common", 6) == 0) {
                return (int)WAYWAL_SHADER_PASS_COMMON;
            }
            if (slen >= 5 && strncasecmp(s, "image", 5) == 0) {
                return (int)WAYWAL_SHADER_PASS_IMAGE;
            }
            if (slen >= 8 && strncasecmp(s, "buffer_a", 8) == 0) {
                return (int)WAYWAL_SHADER_PASS_BUFFER_A;
            }
            if (slen >= 8 && strncasecmp(s, "buffer_b", 8) == 0) {
                return (int)WAYWAL_SHADER_PASS_BUFFER_B;
            }
            if (slen >= 8 && strncasecmp(s, "buffer_c", 8) == 0) {
                return (int)WAYWAL_SHADER_PASS_BUFFER_C;
            }
            if (slen >= 8 && strncasecmp(s, "buffer_d", 8) == 0) {
                return (int)WAYWAL_SHADER_PASS_BUFFER_D;
            }
            if (slen >= 7 && strncasecmp(s, "bufferA", 7) == 0) {
                return (int)WAYWAL_SHADER_PASS_BUFFER_A;
            }
            if (slen >= 7 && strncasecmp(s, "bufferB", 7) == 0) {
                return (int)WAYWAL_SHADER_PASS_BUFFER_B;
            }
            if (slen >= 7 && strncasecmp(s, "bufferC", 7) == 0) {
                return (int)WAYWAL_SHADER_PASS_BUFFER_C;
            }
            if (slen >= 7 && strncasecmp(s, "bufferD", 7) == 0) {
                return (int)WAYWAL_SHADER_PASS_BUFFER_D;
            }
        }
    }
    return -2;
}

static void parse_channel_target(const char *val_str, size_t val_len,
                                 waywal_shader_channel_t *channel)
{
    while (val_len > 0 &&
           (val_str[0] == ' ' || val_str[0] == '\t' || val_str[0] == ':' || val_str[0] == '=')) {
        val_str++;
        val_len--;
    }
    while (val_len > 0 &&
           ((unsigned char)val_str[val_len - 1] <= ' ' || val_str[val_len - 1] == ';')) {
        val_len--;
    }
    if (val_len == 0) {
        return;
    }

    if (val_len == 5 && strncasecmp(val_str, "audio", 5) == 0) {
        channel->target = WAYWAL_CHAN_TARGET_AUDIO;
    } else if (val_len == 5 && strncasecmp(val_str, "noise", 5) == 0) {
        channel->target = WAYWAL_CHAN_TARGET_NOISE;
    } else if (val_len == 9 && strncasecmp(val_str, "wallpaper", 9) == 0) {
        channel->target = WAYWAL_CHAN_TARGET_WALLPAPER;
    } else if ((val_len == 9 && strncasecmp(val_str, "album_art", 9) == 0) ||
               (val_len == 9 && strncasecmp(val_str, "mpris_art", 9) == 0)) {
        channel->target = WAYWAL_CHAN_TARGET_ALBUM_ART;
    } else if (val_len == 5 && strncasecmp(val_str, "depth", 5) == 0) {
        channel->target = WAYWAL_CHAN_TARGET_DEPTH;
    } else if (val_len == 8 && strncasecmp(val_str, "keyboard", 8) == 0) {
        channel->target = WAYWAL_CHAN_TARGET_KEYBOARD;
    } else if (val_len == 7 && strncasecmp(val_str, "cubemap", 7) == 0) {
        channel->target = WAYWAL_CHAN_TARGET_CUBEMAP;
    } else if ((val_len == 7 && strncasecmp(val_str, "bufferA", 7) == 0) ||
               (val_len == 8 && strncasecmp(val_str, "buffer_a", 8) == 0)) {
        channel->target = WAYWAL_CHAN_TARGET_BUFFER_A;
    } else if ((val_len == 7 && strncasecmp(val_str, "bufferB", 7) == 0) ||
               (val_len == 8 && strncasecmp(val_str, "buffer_b", 8) == 0)) {
        channel->target = WAYWAL_CHAN_TARGET_BUFFER_B;
    } else if ((val_len == 7 && strncasecmp(val_str, "bufferC", 7) == 0) ||
               (val_len == 8 && strncasecmp(val_str, "buffer_c", 8) == 0)) {
        channel->target = WAYWAL_CHAN_TARGET_BUFFER_C;
    } else if ((val_len == 7 && strncasecmp(val_str, "bufferD", 7) == 0) ||
               (val_len == 8 && strncasecmp(val_str, "buffer_d", 8) == 0)) {
        channel->target = WAYWAL_CHAN_TARGET_BUFFER_D;
    } else if (val_len > 6 && strncasecmp(val_str, "image(", 6) == 0) {
        const char *q1 = (const char *)memchr(val_str + 6, '"', val_len - 6);
        if (q1) {
            const char *q2 =
                (const char *)memchr(q1 + 1, '"', (size_t)(val_str + val_len - (q1 + 1)));
            if (q2 && (size_t)(q2 - (q1 + 1)) < sizeof(channel->image_path)) {
                size_t plen = (size_t)(q2 - (q1 + 1));
                memcpy(channel->image_path, q1 + 1, plen);
                channel->image_path[plen] = '\0';
                channel->target = WAYWAL_CHAN_TARGET_IMAGE;
            }
        }
    } else if (val_len >= 2 && val_str[0] == '"' && val_str[val_len - 1] == '"') {
        size_t plen = val_len - 2;
        if (plen < sizeof(channel->image_path)) {
            memcpy(channel->image_path, val_str + 1, plen);
            channel->image_path[plen] = '\0';
            channel->target = WAYWAL_CHAN_TARGET_IMAGE;
        }
    } else {
        /* Unquoted path or custom texture identifier */
        if (val_len < sizeof(channel->image_path)) {
            memcpy(channel->image_path, val_str, val_len);
            channel->image_path[val_len] = '\0';
            channel->target = WAYWAL_CHAN_TARGET_IMAGE;
        }
    }
}

static void apply_channel_binding(waywal_shader_meta_t *meta, int target_pass, int ch,
                                  const waywal_shader_channel_t *channel)
{
    if (ch < 0 || ch >= (int)WAYWAL_MAX_SHADER_CHANNELS) {
        return;
    }
    if (target_pass >= 0 && target_pass < (int)WAYWAL_MAX_SHADER_PASSES) {
        meta->passes[target_pass].channels[ch] = *channel;
    } else {
        for (uint32_t p = 0; p < WAYWAL_MAX_SHADER_PASSES; ++p) {
            meta->passes[p].channels[ch] = *channel;
        }
    }
}

static void apply_scale(waywal_shader_meta_t *meta, int target_pass, float scale)
{
    if (!isfinite(scale) || scale <= 0.0f) {
        return;
    }
    if (scale < 0.1f) {
        scale = 0.1f;
    }
    if (scale > 2.0f) {
        scale = 2.0f;
    }

    if (target_pass >= 0 && target_pass < (int)WAYWAL_MAX_SHADER_PASSES) {
        meta->passes[target_pass].res_scale = scale;
    } else {
        meta->global_scale = scale;
        for (uint32_t p = 0; p < WAYWAL_MAX_SHADER_PASSES; ++p) {
            meta->passes[p].res_scale = scale;
        }
    }
}

static void apply_fps(waywal_shader_meta_t *meta, int target_pass, uint32_t fps)
{
    if (fps > 1000) {
        fps = 1000;
    }
    if (target_pass >= 0 && target_pass < (int)WAYWAL_MAX_SHADER_PASSES) {
        meta->passes[target_pass].target_fps = fps;
    } else {
        meta->global_fps = fps;
        for (uint32_t p = 0; p < WAYWAL_MAX_SHADER_PASSES; ++p) {
            meta->passes[p].target_fps = fps;
        }
    }
}

static bool parse_floats(const char *str, const char *end, float *out, uint32_t max_count,
                         uint32_t *out_count)
{
    uint32_t count = 0;
    const char *p = str;

    while (p < end && count < max_count) {
        p = skip_ws(p, end);
        if (p >= end || *p == ']' || *p == ')' || *p == ';') {
            break;
        }
        char *endptr = NULL;
        float val = strtof(p, &endptr);
        if (endptr == p) {
            /* Try skipping non-numeric characters like vec3( or commas */
            if (*p == ',' || *p == '(' || *p == '[' || *p == '=') {
                p++;
                continue;
            }
            break;
        }
        out[count++] = val;
        p = endptr;
        p = skip_ws(p, end);
        if (p < end && *p == ',') {
            p++;
        }
    }
    *out_count = count;
    return count > 0;
}

static void parse_uniform_line(waywal_shader_meta_t *meta, const char *s, const char *end)
{
    s = skip_ws(s, end);
    if (s >= end) {
        return;
    }

    /* Skip optional leading "@uniform" or "uniform" */
    if (end - s >= 8 && strncasecmp(s, "@uniform", 8) == 0) {
        s += 8;
    } else if (end - s >= 7 && strncasecmp(s, "uniform", 7) == 0 &&
               ((unsigned char)s[7] <= ' ' || s[7] == '\t')) {
        s += 7;
    }
    s = skip_ws(s, end);
    if (s >= end) {
        return;
    }

    /* Parse type */
    char type_buf[32] = {0};
    size_t tidx = 0;
    while (s < end && (isalnum((unsigned char)*s) || *s == '_') && tidx + 1 < sizeof(type_buf)) {
        type_buf[tidx++] = *s++;
    }
    type_buf[tidx] = '\0';
    s = skip_ws(s, end);

    waywal_shader_uniform_type_t type = WAYWAL_UNIFORM_TYPE_FLOAT;
    uint32_t components = 1;

    if (strcasecmp(type_buf, "float") == 0) {
        type = WAYWAL_UNIFORM_TYPE_FLOAT;
        components = 1;
    } else if (strcasecmp(type_buf, "bool") == 0) {
        type = WAYWAL_UNIFORM_TYPE_BOOL;
        components = 1;
    } else if (strcasecmp(type_buf, "int") == 0) {
        type = WAYWAL_UNIFORM_TYPE_INT;
        components = 1;
    } else if (strcasecmp(type_buf, "vec2") == 0) {
        type = WAYWAL_UNIFORM_TYPE_VEC2;
        components = 2;
    } else if (strcasecmp(type_buf, "vec3") == 0) {
        type = WAYWAL_UNIFORM_TYPE_VEC3;
        components = 3;
    } else if (strcasecmp(type_buf, "vec4") == 0) {
        type = WAYWAL_UNIFORM_TYPE_VEC4;
        components = 4;
    } else if (strcasecmp(type_buf, "color") == 0 || strcasecmp(type_buf, "color3") == 0) {
        type = WAYWAL_UNIFORM_TYPE_COLOR;
        components = 3;
    } else if (strcasecmp(type_buf, "color4") == 0) {
        type = WAYWAL_UNIFORM_TYPE_COLOR;
        components = 4;
    } else {
        /* Default to float */
        type = WAYWAL_UNIFORM_TYPE_FLOAT;
        components = 1;
    }

    /* Parse name */
    char name_buf[64] = {0};
    size_t nidx = 0;
    while (s < end && (isalnum((unsigned char)*s) || *s == '_') && nidx + 1 < sizeof(name_buf)) {
        name_buf[nidx++] = *s++;
    }
    name_buf[nidx] = '\0';
    if (name_buf[0] == '\0') {
        return;
    }

    /* If vec3 or vec4 and name contains color/tint/rgb, mark as COLOR */
    if ((type == WAYWAL_UNIFORM_TYPE_VEC3 || type == WAYWAL_UNIFORM_TYPE_VEC4) &&
        (strcasestr(name_buf, "color") || strcasestr(name_buf, "tint") ||
         strcasestr(name_buf, "rgb"))) {
        type = WAYWAL_UNIFORM_TYPE_COLOR;
    }

    waywal_shader_uniform_def_t *u = find_or_add_uniform(meta, name_buf);
    if (!u) {
        return;
    }
    u->type = type;
    u->num_components = components;

    /* Parse optional inline label if before '=' */
    s = skip_ws(s, end);
    char explicit_label[128] = {0};
    if (s < end && *s == '"') {
        s++;
        size_t lidx = 0;
        while (s < end && *s != '"' && lidx + 1 < sizeof(explicit_label)) {
            explicit_label[lidx++] = *s++;
        }
        explicit_label[lidx] = '\0';
        if (s < end && *s == '"') {
            s++;
        }
        s = skip_ws(s, end);
    }

    /* Parse default value after '=' */
    if (s < end && *s == '=') {
        s++;
        s = skip_ws(s, end);

        if (type == WAYWAL_UNIFORM_TYPE_BOOL) {
            if (end - s >= 4 && strncasecmp(s, "true", 4) == 0) {
                u->default_val[0] = 1.0f;
                s += 4;
            } else if (end - s >= 5 && strncasecmp(s, "false", 5) == 0) {
                u->default_val[0] = 0.0f;
                s += 5;
            } else {
                char *endp = NULL;
                u->default_val[0] = strtof(s, &endp) != 0.0f ? 1.0f : 0.0f;
                s = endp;
            }
        } else {
            /* Skip vector constructor if present: e.g. vec3( */
            if (end - s >= 4 && (strncasecmp(s, "vec2", 4) == 0 || strncasecmp(s, "vec3", 4) == 0 ||
                                 strncasecmp(s, "vec4", 4) == 0)) {
                s += 4;
            }
            if (s < end && *s == '(') {
                s++;
            }
            uint32_t parsed_count = 0;
            parse_floats(s, end, u->default_val, components, &parsed_count);
            /* Skip until closing paren or semicolon */
            while (s < end && *s != ';' && *s != '[' && *s != '/') {
                s++;
            }
        }
    }

    if (s < end && *s == ';') {
        s++;
        s = skip_ws(s, end);
    }

    /* Parse range bounds [min, max] or [min, max, step] */
    if (s < end && *s == '[') {
        s++;
        float bounds[4] = {0};
        uint32_t bcount = 0;
        if (parse_floats(s, end, bounds, 3, &bcount) && bcount >= 2) {
            u->has_range = true;
            for (uint32_t c = 0; c < components; ++c) {
                u->min_val[c] = bounds[0];
                u->max_val[c] = bounds[1];
            }
            if (bcount >= 3) {
                u->step = bounds[2];
            }
        }
        while (s < end && *s != ']') {
            s++;
        }
        if (s < end && *s == ']') {
            s++;
        }
        s = skip_ws(s, end);
    }

    /* Set default range bounds if none specified */
    if (!u->has_range) {
        if (type == WAYWAL_UNIFORM_TYPE_FLOAT) {
            u->min_val[0] = 0.0f;
            u->max_val[0] = (u->default_val[0] > 1.0f) ? u->default_val[0] * 2.0f : 1.0f;
        } else if (type == WAYWAL_UNIFORM_TYPE_INT) {
            u->min_val[0] = 0.0f;
            u->max_val[0] = (u->default_val[0] > 10.0f) ? u->default_val[0] * 2.0f : 10.0f;
        } else if (type == WAYWAL_UNIFORM_TYPE_BOOL) {
            u->min_val[0] = 0.0f;
            u->max_val[0] = 1.0f;
        } else {
            for (uint32_t c = 0; c < components; ++c) {
                u->min_val[c] = 0.0f;
                u->max_val[c] = 1.0f;
            }
        }
    }

    /* Parse trailing comment or quoted label if present */
    s = skip_ws(s, end);
    if (explicit_label[0] != '\0') {
        snprintf(u->label, sizeof(u->label), "%s", explicit_label);
    } else {
        if (s < end && *s == '/' && s + 1 < end && s[1] == '/') {
            s += 2;
            s = skip_ws(s, end);
            if (s < end && *s == '"') {
                s++;
                size_t lidx = 0;
                while (s < end && *s != '"' && lidx + 1 < sizeof(u->label)) {
                    u->label[lidx++] = *s++;
                }
                u->label[lidx] = '\0';
            } else if (s < end) {
                const char *lend = trim_right(s, end);
                size_t llen = (size_t)(lend - s);
                if (llen > 0 && llen < sizeof(u->label)) {
                    memcpy(u->label, s, llen);
                    u->label[llen] = '\0';
                }
            }
        }
        if (u->label[0] == '\0') {
            make_friendly_label(name_buf, u->label, sizeof(u->label));
        }
    }
}

bool waywal_shader_parse_meta(const char *src, size_t len, waywal_shader_meta_t *out_meta)
{
    if (!out_meta) {
        return false;
    }
    waywal_shader_meta_init(out_meta);
    if (!src || len == 0) {
        return true;
    }

    int cur_pass = -1;
    bool in_uniform_block = false;
    waywal_shader_uniform_def_t active_block_uniform;
    memset(&active_block_uniform, 0, sizeof(active_block_uniform));

    const char *p = src;
    const char *total_end = src + len;

    while (p < total_end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(total_end - p));
        const char *line_end = nl ? nl : total_end;

        const char *s = skip_ws(p, line_end);
        const char *send = trim_right(s, line_end);
        size_t slen = (size_t)(send - s);

        int sec = match_pass_section(s, slen);
        if (sec >= -1) {
            cur_pass = sec;
            if (cur_pass >= 0 && cur_pass < (int)WAYWAL_MAX_SHADER_PASSES &&
                cur_pass != (int)WAYWAL_SHADER_PASS_IMAGE) {
                out_meta->has_multi_pass = true;
            }
            in_uniform_block = false;
        } else if (slen >= 2 && s[0] == '/' && s[1] == '/') {
            const char *c = skip_ws(s + 2, send);
            size_t clen = (size_t)(send - c);

            if (clen >= 9 && strncasecmp(c, "[UNIFORM]", 9) == 0) {
                in_uniform_block = true;
                if (active_block_uniform.name[0] != '\0') {
                    waywal_shader_uniform_def_t *dest =
                        find_or_add_uniform(out_meta, active_block_uniform.name);
                    if (dest) {
                        *dest = active_block_uniform;
                    }
                    memset(&active_block_uniform, 0, sizeof(active_block_uniform));
                }
            } else if (in_uniform_block) {
                /* Key-value attributes in [UNIFORM] block */
                if (clen > 5 && strncasecmp(c, "type:", 5) == 0) {
                    const char *v = skip_ws(c + 5, send);
                    if (strncasecmp(v, "float", 5) == 0) {
                        active_block_uniform.type = WAYWAL_UNIFORM_TYPE_FLOAT;
                        active_block_uniform.num_components = 1;
                    } else if (strncasecmp(v, "bool", 4) == 0) {
                        active_block_uniform.type = WAYWAL_UNIFORM_TYPE_BOOL;
                        active_block_uniform.num_components = 1;
                    } else if (strncasecmp(v, "int", 3) == 0) {
                        active_block_uniform.type = WAYWAL_UNIFORM_TYPE_INT;
                        active_block_uniform.num_components = 1;
                    } else if (strncasecmp(v, "vec2", 4) == 0) {
                        active_block_uniform.type = WAYWAL_UNIFORM_TYPE_VEC2;
                        active_block_uniform.num_components = 2;
                    } else if (strncasecmp(v, "vec3", 4) == 0) {
                        active_block_uniform.type = WAYWAL_UNIFORM_TYPE_VEC3;
                        active_block_uniform.num_components = 3;
                    } else if (strncasecmp(v, "vec4", 4) == 0) {
                        active_block_uniform.type = WAYWAL_UNIFORM_TYPE_VEC4;
                        active_block_uniform.num_components = 4;
                    } else if (strncasecmp(v, "color", 5) == 0) {
                        active_block_uniform.type = WAYWAL_UNIFORM_TYPE_COLOR;
                        active_block_uniform.num_components = 3;
                    }
                } else if (clen > 5 && strncasecmp(c, "name:", 5) == 0) {
                    const char *v = skip_ws(c + 5, send);
                    size_t vlen = (size_t)(send - v);
                    if (vlen < sizeof(active_block_uniform.name)) {
                        memcpy(active_block_uniform.name, v, vlen);
                        active_block_uniform.name[vlen] = '\0';
                    }
                } else if (clen > 6 && strncasecmp(c, "label:", 6) == 0) {
                    const char *v = skip_ws(c + 6, send);
                    size_t vlen = (size_t)(send - v);
                    if (vlen < sizeof(active_block_uniform.label)) {
                        memcpy(active_block_uniform.label, v, vlen);
                        active_block_uniform.label[vlen] = '\0';
                    }
                } else if (clen > 8 && strncasecmp(c, "default:", 8) == 0) {
                    const char *v = skip_ws(c + 8, send);
                    uint32_t cnt = 0;
                    parse_floats(v, send, active_block_uniform.default_val, 4, &cnt);
                } else if (clen > 4 && strncasecmp(c, "min:", 4) == 0) {
                    const char *v = skip_ws(c + 4, send);
                    uint32_t cnt = 0;
                    parse_floats(v, send, active_block_uniform.min_val, 4, &cnt);
                    active_block_uniform.has_range = true;
                } else if (clen > 4 && strncasecmp(c, "max:", 4) == 0) {
                    const char *v = skip_ws(c + 4, send);
                    uint32_t cnt = 0;
                    parse_floats(v, send, active_block_uniform.max_val, 4, &cnt);
                    active_block_uniform.has_range = true;
                } else if (clen > 5 && strncasecmp(c, "step:", 5) == 0) {
                    const char *v = skip_ws(c + 5, send);
                    active_block_uniform.step = strtof(v, NULL);
                }
            }

            /* Single-line @uniform directive */
            if (clen >= 8 && strncasecmp(c, "@uniform", 8) == 0) {
                parse_uniform_line(out_meta, c, send);
            } else if (clen > 6 && strncasecmp(c, "@title", 6) == 0) {
                const char *v = skip_ws(c + 6, send);
                if (v < send && *v == ':') {
                    v = skip_ws(v + 1, send);
                }
                size_t vlen = (size_t)(send - v);
                if (vlen < sizeof(out_meta->title)) {
                    memcpy(out_meta->title, v, vlen);
                    out_meta->title[vlen] = '\0';
                }
            } else if (clen > 7 && strncasecmp(c, "@author", 7) == 0) {
                const char *v = skip_ws(c + 7, send);
                if (v < send && *v == ':') {
                    v = skip_ws(v + 1, send);
                }
                size_t vlen = (size_t)(send - v);
                if (vlen < sizeof(out_meta->author)) {
                    memcpy(out_meta->author, v, vlen);
                    out_meta->author[vlen] = '\0';
                }
            } else if (clen > 12 && strncasecmp(c, "@description", 12) == 0) {
                const char *v = skip_ws(c + 12, send);
                if (v < send && *v == ':') {
                    v = skip_ws(v + 1, send);
                }
                size_t vlen = (size_t)(send - v);
                if (vlen < sizeof(out_meta->description)) {
                    memcpy(out_meta->description, v, vlen);
                    out_meta->description[vlen] = '\0';
                }
            } else if (clen > 5 && strncasecmp(c, "@desc", 5) == 0) {
                const char *v = skip_ws(c + 5, send);
                if (v < send && *v == ':') {
                    v = skip_ws(v + 1, send);
                }
                size_t vlen = (size_t)(send - v);
                if (vlen < sizeof(out_meta->description)) {
                    memcpy(out_meta->description, v, vlen);
                    out_meta->description[vlen] = '\0';
                }
            } else if (clen > 8 && strncasecmp(c, "@license", 8) == 0) {
                const char *v = skip_ws(c + 8, send);
                if (v < send && *v == ':') {
                    v = skip_ws(v + 1, send);
                }
                size_t vlen = (size_t)(send - v);
                if (vlen < sizeof(out_meta->license)) {
                    memcpy(out_meta->license, v, vlen);
                    out_meta->license[vlen] = '\0';
                }
            } else if (clen > 6 && strncasecmp(c, "@scale", 6) == 0 &&
                       (c[6] == ':' || c[6] == '=' || c[6] == ' ' || c[6] == '\t')) {
                const char *v = skip_ws(c + 7, send);
                apply_scale(out_meta, cur_pass, strtof(v, NULL));
            } else if (clen > 11 && strncasecmp(c, "@wwal_scale", 11) == 0 &&
                       (c[11] == ':' || c[11] == '=' || c[11] == ' ' || c[11] == '\t')) {
                const char *v = skip_ws(c + 12, send);
                apply_scale(out_meta, cur_pass, strtof(v, NULL));
            } else if (clen > 4 && strncasecmp(c, "@fps", 4) == 0 &&
                       (c[4] == ':' || c[4] == '=' || c[4] == ' ' || c[4] == '\t')) {
                const char *v = skip_ws(c + 5, send);
                apply_fps(out_meta, cur_pass, (uint32_t)strtoul(v, NULL, 10));
            } else if (clen > 9 && strncasecmp(c, "@wwal_fps", 9) == 0 &&
                       (c[9] == ':' || c[9] == '=' || c[9] == ' ' || c[9] == '\t')) {
                const char *v = skip_ws(c + 10, send);
                apply_fps(out_meta, cur_pass, (uint32_t)strtoul(v, NULL, 10));
            } else {
                /* Check for channel directives like // iChannel0: audio or // [CHANNEL0] audio */
                int ch = -1;
                const char *target_str = NULL;
                if (clen > 10 && strncasecmp(c, "[CHANNEL", 8) == 0 && c[8] >= '0' && c[8] <= '3' &&
                    c[9] == ']') {
                    ch = c[8] - '0';
                    target_str = c + 10;
                } else if (clen > 9 && strncasecmp(c, "iChannel", 8) == 0 && c[8] >= '0' &&
                           c[8] <= '3' &&
                           (c[9] == ':' || c[9] == '=' || c[9] == ' ' || c[9] == '\t')) {
                    ch = c[8] - '0';
                    target_str = c + 9;
                } else if (clen > 8 && strncasecmp(c, "channel", 7) == 0 && c[7] >= '0' &&
                           c[7] <= '3' &&
                           (c[8] == ':' || c[8] == '=' || c[8] == ' ' || c[8] == '\t')) {
                    ch = c[7] - '0';
                    target_str = c + 8;
                }

                if (ch >= 0 && target_str) {
                    waywal_shader_channel_t chan;
                    memset(&chan, 0, sizeof(chan));
                    parse_channel_target(target_str, (size_t)(send - target_str), &chan);
                    apply_channel_binding(out_meta, cur_pass, ch, &chan);
                }
            }
        } else if (slen >= 14 && strncasecmp(s, "#pragma waywal", 14) == 0) {
            const char *c = skip_ws(s + 14, send);
            size_t clen = (size_t)(send - c);

            int target_pass = cur_pass;
            if (clen > 8 && strncasecmp(c, "bufferA.", 8) == 0) {
                target_pass = (int)WAYWAL_SHADER_PASS_BUFFER_A;
                c += 8;
                clen -= 8;
            } else if (clen > 9 && strncasecmp(c, "buffer_a.", 9) == 0) {
                target_pass = (int)WAYWAL_SHADER_PASS_BUFFER_A;
                c += 9;
                clen -= 9;
            } else if (clen > 8 && strncasecmp(c, "bufferB.", 8) == 0) {
                target_pass = (int)WAYWAL_SHADER_PASS_BUFFER_B;
                c += 8;
                clen -= 8;
            } else if (clen > 9 && strncasecmp(c, "buffer_b.", 9) == 0) {
                target_pass = (int)WAYWAL_SHADER_PASS_BUFFER_B;
                c += 9;
                clen -= 9;
            } else if (clen > 8 && strncasecmp(c, "bufferC.", 8) == 0) {
                target_pass = (int)WAYWAL_SHADER_PASS_BUFFER_C;
                c += 8;
                clen -= 8;
            } else if (clen > 9 && strncasecmp(c, "buffer_c.", 9) == 0) {
                target_pass = (int)WAYWAL_SHADER_PASS_BUFFER_C;
                c += 9;
                clen -= 9;
            } else if (clen > 8 && strncasecmp(c, "bufferD.", 8) == 0) {
                target_pass = (int)WAYWAL_SHADER_PASS_BUFFER_D;
                c += 8;
                clen -= 8;
            } else if (clen > 9 && strncasecmp(c, "buffer_d.", 9) == 0) {
                target_pass = (int)WAYWAL_SHADER_PASS_BUFFER_D;
                c += 9;
                clen -= 9;
            } else if (clen > 6 && strncasecmp(c, "image.", 6) == 0) {
                target_pass = (int)WAYWAL_SHADER_PASS_IMAGE;
                c += 6;
                clen -= 6;
            }

            int ch = -1;
            const char *target_str = NULL;
            if (clen >= 9 && strncasecmp(c, "iChannel", 8) == 0 && c[8] >= '0' && c[8] <= '3') {
                ch = c[8] - '0';
                target_str = c + 9;
            } else if (clen >= 8 && strncasecmp(c, "channel", 7) == 0 && c[7] >= '0' &&
                       c[7] <= '3') {
                ch = c[7] - '0';
                target_str = c + 8;
            }

            if (ch >= 0 && target_str) {
                waywal_shader_channel_t chan;
                memset(&chan, 0, sizeof(chan));
                parse_channel_target(target_str, (size_t)(send - target_str), &chan);
                apply_channel_binding(out_meta, target_pass, ch, &chan);
            } else if (clen >= 5 && strncasecmp(c, "scale", 5) == 0) {
                const char *v = skip_ws(c + 5, send);
                if (v < send && (*v == '=' || *v == ':')) {
                    v = skip_ws(v + 1, send);
                }
                apply_scale(out_meta, target_pass, strtof(v, NULL));
            } else if (clen >= 3 && strncasecmp(c, "fps", 3) == 0) {
                const char *v = skip_ws(c + 3, send);
                if (v < send && (*v == '=' || *v == ':')) {
                    v = skip_ws(v + 1, send);
                }
                apply_fps(out_meta, target_pass, (uint32_t)strtoul(v, NULL, 10));
            }
        }

        if (!nl) {
            break;
        }
        p = nl + 1;
    }

    if (in_uniform_block && active_block_uniform.name[0] != '\0') {
        waywal_shader_uniform_def_t *dest =
            find_or_add_uniform(out_meta, active_block_uniform.name);
        if (dest) {
            *dest = active_block_uniform;
        }
    }

    return true;
}

bool waywal_shader_parse_file(const char *filepath, waywal_shader_meta_t *out_meta)
{
    if (!filepath || !out_meta) {
        return false;
    }
    waywal_shader_meta_init(out_meta);

    int fd = open(filepath, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0 || st.st_size > 10 * 1024 * 1024) {
        close(fd);
        return false;
    }

    size_t size = (size_t)st.st_size;
    char *buf = (char *)malloc(size + 1);
    if (!buf) {
        close(fd);
        return false;
    }

    size_t read_bytes = 0;
    while (read_bytes < size) {
        ssize_t n = read(fd, buf + read_bytes, size - read_bytes);
        if (n < 0) {
            free(buf);
            close(fd);
            return false;
        }
        if (n == 0) {
            break;
        }
        read_bytes += (size_t)n;
    }
    close(fd);
    buf[read_bytes] = '\0';

    bool ok = waywal_shader_parse_meta(buf, read_bytes, out_meta);
    free(buf);
    return ok;
}

size_t waywal_shader_format_uniform_decls(const waywal_shader_meta_t *meta, char *out_buf,
                                          size_t out_cap)
{
    if (!meta || !out_buf || out_cap == 0) {
        return 0;
    }
    size_t written = 0;
    out_buf[0] = '\0';

    for (uint32_t i = 0; i < meta->uniform_count; ++i) {
        const waywal_shader_uniform_def_t *u = &meta->uniforms[i];
        const char *type_name = waywal_shader_uniform_type_str(u->type);
        int n =
            snprintf(out_buf + written, out_cap - written, "uniform %s %s;\n", type_name, u->name);
        if (n < 0 || (size_t)n >= out_cap - written) {
            break;
        }
        written += (size_t)n;
    }
    return written;
}
