// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/we_shader.h"
#include "waywal/we_builtin_effects.h"

#include "waywal/log.h"

#include <GLES3/gl31.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* =========================================================================
 * Built-in Wallpaper Engine Shader Headers & Fallback Shaders
 * ========================================================================= */

static const char WE_BUILTIN_COMMON_H[] =
    "#ifndef WE_COMMON_H_INCLUDED\n"
    "#define WE_COMMON_H_INCLUDED\n"
    "#ifndef M_PI\n"
    "#define M_PI 3.14159265358979323846\n"
    "#endif\n"
    "#ifndef M_PI_2\n"
    "#define M_PI_2 1.57079632679489661923\n"
    "#endif\n"
    "#ifndef M_TWO_PI\n"
    "#define M_TWO_PI 6.28318530717958647692\n"
    "#endif\n"
    "vec3 rgb2hsv(vec3 c) {\n"
    "    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);\n"
    "    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));\n"
    "    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));\n"
    "    float d = q.x - min(q.w, q.y);\n"
    "    float e = 1.0e-10;\n"
    "    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);\n"
    "}\n"
    "vec3 hsv2rgb(vec3 c) {\n"
    "    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);\n"
    "    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);\n"
    "    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);\n"
    "}\n"
    "vec2 rotateVec2(vec2 v, float r) {\n"
    "    float cs = cos(r);\n"
    "    float sn = sin(r);\n"
    "    return vec2(v.x * cs - v.y * sn, v.x * sn + v.y * cs);\n"
    "}\n"
    "vec2 rotateVec2(vec4 v, float r) {\n"
    "    return rotateVec2(v.xy, r);\n"
    "}\n"
    "float greyscale(vec3 c) {\n"
    "    return dot(c, vec3(0.11, 0.59, 0.3));\n"
    "}\n"
    "#endif\n";

static const char WE_BUILTIN_COMMON_VERTEX_H[] =
    "#ifndef WE_COMMON_VERTEX_H_INCLUDED\n"
    "#define WE_COMMON_VERTEX_H_INCLUDED\n"
    "vec3 ComputePeelPosition(vec3 pos) {\n"
    "    return pos;\n"
    "}\n"
    "vec3 ComputePeelPosition(vec3 pos, float weight, float time, float speed) {\n"
    "    return pos + vec3(sin(time * speed) * weight, cos(time * speed) * weight, 0.0);\n"
    "}\n"
    "#endif\n";

static const char WE_BUILTIN_COMMON_FRAGMENT_H[] =
    "#ifndef WE_COMMON_FRAGMENT_H_INCLUDED\n"
    "#define WE_COMMON_FRAGMENT_H_INCLUDED\n"
    "vec3 DecompressNormal(vec4 n) {\n"
    "    vec3 normal;\n"
    "    normal.xy = n.yw * 2.0 - 1.0;\n"
    "    normal.z = sqrt(max(0.0, 1.0 - dot(normal.xy, normal.xy)));\n"
    "    return normal;\n"
    "}\n"
    "vec3 DecompressNormalWithMask(vec4 n) {\n"
    "    return DecompressNormal(n);\n"
    "}\n"
    "vec4 ConvertSampleR8(vec4 s) {\n"
    "    return vec4(1.0, 1.0, 1.0, s.r);\n"
    "}\n"
    "vec4 ConvertTexture0Format(vec4 s) {\n"
    "#ifdef TEX0FORMAT\n"
    "#if TEX0FORMAT == 8\n"
    "    return vec4(s.rrr, s.a);\n"
    "#elif TEX0FORMAT == 9\n"
    "    return vec4(1.0, 1.0, 1.0, s.r);\n"
    "#endif\n"
    "#endif\n"
    "    return s;\n"
    "}\n"
    "#endif\n";

static const char WE_BUILTIN_COMMON_BLENDING_H[] =
    "#ifndef WE_COMMON_BLENDING_H_INCLUDED\n"
    "#define WE_COMMON_BLENDING_H_INCLUDED\n"
    "#include \"common.h\"\n"
    "#ifndef BLENDMODE\n"
    "#define BLENDMODE 0\n"
    "#endif\n"
    "vec3 ApplyBlending(int mode, vec3 a, vec3 b, float t) {\n"
    "    vec3 r = b;\n"
    "    if (mode == 0) { r = b; }\n"
    "    else if (mode == 1) { r = min(a, b); }\n"
    "    else if (mode == 2) { r = a * b; }\n"
    "    else if (mode == 3) { r = 1.0 - min(vec3(1.0), (1.0 - a) / max(b, vec3(1e-5))); }\n"
    "    else if (mode == 4) { r = max(a + b - 1.0, vec3(0.0)); }\n"
    "    else if (mode == 5) {\n"
    "        r = (dot(a, vec3(0.299, 0.587, 0.114)) < dot(b, vec3(0.299, 0.587, 0.114))) ? a : b;\n"
    "    }\n"
    "    else if (mode == 6) { r = max(a, b); }\n"
    "    else if (mode == 7) { r = 1.0 - (1.0 - a) * (1.0 - b); }\n"
    "    else if (mode == 8) { r = min(vec3(1.0), a / max(1.0 - b, vec3(1e-5))); }\n"
    "    else if (mode == 9) { r = a + b; }\n"
    "    else if (mode == 10) {\n"
    "        r = (dot(a, vec3(0.299, 0.587, 0.114)) > dot(b, vec3(0.299, 0.587, 0.114))) ? a : b;\n"
    "    }\n"
    "    else if (mode == 11) {\n"
    "        r = mix(2.0 * a * b, 1.0 - 2.0 * (1.0 - a) * (1.0 - b), step(vec3(0.5), a));\n"
    "    }\n"
    "    else if (mode == 12) {\n"
    "        r = mix(2.0 * a * b + a * a * (1.0 - 2.0 * b),\n"
    "                sqrt(max(a, vec3(0.0))) * (2.0 * b - 1.0) + 2.0 * a * (1.0 - b),\n"
    "                step(vec3(0.5), b));\n"
    "    }\n"
    "    else if (mode == 13) {\n"
    "        r = mix(2.0 * a * b, 1.0 - 2.0 * (1.0 - a) * (1.0 - b), step(vec3(0.5), b));\n"
    "    }\n"
    "    else if (mode == 14) {\n"
    "        r = mix(1.0 - min(vec3(1.0), (1.0 - a) / max(2.0 * b, vec3(1e-5))),\n"
    "                min(vec3(1.0), a / max(2.0 * (1.0 - b), vec3(1e-5))),\n"
    "                step(vec3(0.5), b));\n"
    "    }\n"
    "    else if (mode == 15) { r = clamp(a + 2.0 * b - 1.0, 0.0, 1.0); }\n"
    "    else if (mode == 16) {\n"
    "        r = mix(min(a, 2.0 * b), max(a, 2.0 * b - 1.0), step(vec3(0.5), b));\n"
    "    }\n"
    "    else if (mode == 17) { r = step(vec3(1.0), a + b); }\n"
    "    else if (mode == 18) { r = abs(a - b); }\n"
    "    else if (mode == 19) { r = a + b - 2.0 * a * b; }\n"
    "    else if (mode == 20) { r = max(a - b, vec3(0.0)); }\n"
    "    else if (mode == 21) { r = a / max(b, vec3(1e-5)); }\n"
    "    else if (mode == 22) {\n"
    "        vec3 ha = rgb2hsv(a); vec3 hb = rgb2hsv(b);\n"
    "        r = hsv2rgb(vec3(hb.x, ha.y, ha.z));\n"
    "    }\n"
    "    else if (mode == 23) {\n"
    "        vec3 ha = rgb2hsv(a); vec3 hb = rgb2hsv(b);\n"
    "        r = hsv2rgb(vec3(ha.x, hb.y, ha.z));\n"
    "    }\n"
    "    else if (mode == 24) {\n"
    "        vec3 ha = rgb2hsv(a); vec3 hb = rgb2hsv(b);\n"
    "        r = hsv2rgb(vec3(hb.x, hb.y, ha.z));\n"
    "    }\n"
    "    else if (mode == 25) {\n"
    "        vec3 ha = rgb2hsv(a); vec3 hb = rgb2hsv(b);\n"
    "        r = hsv2rgb(vec3(ha.x, ha.y, hb.z));\n"
    "    }\n"
    "    else if (mode == 26) { r = b * dot(a, vec3(0.299, 0.587, 0.114)); }\n"
    "    else if (mode == 27) { r = min(vec3(1.0), (a * a) / max(1.0 - b, vec3(1e-5))); }\n"
    "    else if (mode == 28) { r = min(vec3(1.0), (b * b) / max(1.0 - a, vec3(1e-5))); }\n"
    "    else if (mode == 29) { r = 1.0 - abs(1.0 - a - b); }\n"
    "    else if (mode == 30) { r = min(a, b) - max(a, b) + vec3(1.0); }\n"
    "    return mix(a, r, t);\n"
    "}\n"
    "#endif\n";

static const char WE_BUILTIN_COMMON_PARTICLES_H[] =
    "#ifndef WE_COMMON_PARTICLES_H_INCLUDED\n"
    "#define WE_COMMON_PARTICLES_H_INCLUDED\n"
    "void ComputeParticleTangents(vec3 dir, vec3 eye, out vec3 right, out vec3 up) {\n"
    "    right = normalize(cross(dir, vec3(0.0, 0.0, 1.0)));\n"
    "    up = normalize(dir);\n"
    "}\n"
    "#endif\n";

static const char WE_BUILTIN_GENERICIMAGE2_VERT[] =
    "// [COMBO] {\"combo\":\"SPRITESHEET\",\"default\":0}\n"
    "uniform mat4 g_ModelViewProjectionMatrix;\n"
    "uniform vec2 g_Texture0Translation;\n"
    "uniform vec4 g_Texture0Rotation;\n"
    "attribute vec3 a_Position;\n"
    "attribute vec2 a_TexCoord;\n"
    "varying vec4 v_TexCoord;\n"
    "void main() {\n"
    "    gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n"
    "    v_TexCoord = a_TexCoord.xyxy;\n"
    "#if SPRITESHEET == 1\n"
    "    v_TexCoord.xy = g_Texture0Translation + a_TexCoord.x * g_Texture0Rotation.xy +\n"
    "                    a_TexCoord.y * g_Texture0Rotation.zw;\n"
    "#endif\n"
    "}\n";

static const char WE_BUILTIN_GENERICIMAGE2_FRAG[] =
    "// [COMBO] {\"combo\":\"BLENDMODE\",\"default\":0}\n"
    "#include \"common.h\"\n"
    "#include \"common_fragment.h\"\n"
    "#include \"common_blending.h\"\n"
    "uniform sampler2D g_Texture0;\n"
    "uniform float g_Alpha; // {\"default\":1.0}\n"
    "uniform vec3 g_Color; // {\"default\":\"1 1 1\"}\n"
    "uniform vec4 g_Color4; // {\"default\":\"1 1 1 1\"}\n"
    "uniform float g_UserAlpha; // {\"default\":1.0}\n"
    "uniform float g_Brightness; // {\"default\":1.0}\n"
    "uniform float g_Contrast; // {\"default\":1.0}\n"
    "uniform float g_Saturation; // {\"default\":1.0}\n"
    "varying vec4 v_TexCoord;\n"
    "void main() {\n"
    "    vec4 albedo = texSample2D(g_Texture0, v_TexCoord.xy);\n"
    "    albedo = ConvertTexture0Format(albedo);\n"
    "#if BLENDMODE > 0\n"
    "    albedo.rgb = ApplyBlending(BLENDMODE, albedo.rgb, g_Color, 1.0) * g_Brightness;\n"
    "#else\n"
    "    albedo.rgb *= g_Color * g_Brightness;\n"
    "#endif\n"
    "    albedo.a *= g_Alpha * g_UserAlpha;\n"
    "    gl_FragColor = albedo;\n"
    "}\n";

const char *we_shader_get_builtin(const char *path)
{
    if (!path || path[0] == '\0') {
        return NULL;
    }
    const char *base = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!base || bslash > base)) {
        base = bslash;
    }
    base = base ? (base + 1) : path;

    if (strcasecmp(base, "common.h") == 0) {
        return WE_BUILTIN_COMMON_H;
    }
    if (strcasecmp(base, "common_vertex.h") == 0) {
        return WE_BUILTIN_COMMON_VERTEX_H;
    }
    if (strcasecmp(base, "common_fragment.h") == 0) {
        return WE_BUILTIN_COMMON_FRAGMENT_H;
    }
    if (strcasecmp(base, "common_blending.h") == 0) {
        return WE_BUILTIN_COMMON_BLENDING_H;
    }
    if (strcasecmp(base, "common_particles.h") == 0) {
        return WE_BUILTIN_COMMON_PARTICLES_H;
    }
    if (strcasecmp(base, "genericimage2.vert") == 0 || strcasecmp(base, "passthrough.vert") == 0 ||
        strcasecmp(base, "default.vert") == 0) {
        return WE_BUILTIN_GENERICIMAGE2_VERT;
    }
    if (strcasecmp(base, "genericimage2.frag") == 0 || strcasecmp(base, "passthrough.frag") == 0 ||
        strcasecmp(base, "default.frag") == 0) {
        return WE_BUILTIN_GENERICIMAGE2_FRAG;
    }

    /* Resolve built-in clean-room effects before falling back to genericimage2 */
    const we_builtin_effect_desc_t *eff = we_builtin_effect_find_by_slug(path);
    if (eff) {
        bool is_frag = true;
        const char *dot = strrchr(base, '.');
        if (dot && (strcasecmp(dot, ".vert") == 0 || strcasecmp(dot, ".vs") == 0)) {
            is_frag = false;
        }
        const char *src =
            we_builtin_effect_get_stage_glsl((we_builtin_effect_id_t)eff->id, is_frag);
        if (src) {
            return src;
        }
    }

    /* Workshop or Studio effect referencing effects/... resolves to fallback */
    if (strstr(path, "effects/") != NULL || strstr(path, "effects\\") != NULL) {
        const char *dot = strrchr(base, '.');
        if (dot && (strcasecmp(dot, ".vert") == 0 || strcasecmp(dot, ".vs") == 0)) {
            return WE_BUILTIN_GENERICIMAGE2_VERT;
        }
        return WE_BUILTIN_GENERICIMAGE2_FRAG;
    }

    return NULL;
}

/* =========================================================================
 * Dynamic String Builder
 * ========================================================================= */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} we_strbuf_t;

static bool sb_init(we_strbuf_t *sb, size_t initial_cap)
{
    sb->len = 0;
    sb->cap = initial_cap > 256 ? initial_cap : 256;
    sb->buf = malloc(sb->cap);
    if (!sb->buf) {
        sb->cap = 0;
        return false;
    }
    sb->buf[0] = '\0';
    return true;
}

static bool sb_append_len(we_strbuf_t *sb, const char *s, size_t n)
{
    if (n == 0) {
        return true;
    }
    if (sb->len + n + 1 > sb->cap) {
        size_t new_cap = sb->cap * 2;
        while (new_cap < sb->len + n + 1) {
            new_cap *= 2;
        }
        char *nb = realloc(sb->buf, new_cap);
        if (!nb) {
            return false;
        }
        sb->buf = nb;
        sb->cap = new_cap;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
    return true;
}

static bool sb_append(we_strbuf_t *sb, const char *s)
{
    return s ? sb_append_len(sb, s, strlen(s)) : true;
}

/* =========================================================================
 * Lightweight Inline JSON Comment Extractor
 * ========================================================================= */

static const char *json_find_key_value(const char *json, const char *key)
{
    if (!json || !key || key[0] == '\0') {
        return NULL;
    }
    char pattern[128];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(pattern)) {
        return NULL;
    }
    size_t pat_len = (size_t)n;
    const char *search = json;
    while ((search = strstr(search, pattern)) != NULL) {
        const char *p = search + pat_len;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == ':') {
            p++;
            while (*p && isspace((unsigned char)*p)) {
                p++;
            }
            return p;
        }
        search += pat_len;
    }
    return NULL;
}

static bool json_extract_string(const char *json, const char *key, char *out, size_t out_cap)
{
    if (!json || !key || !out || out_cap == 0) {
        return false;
    }
    const char *p = json_find_key_value(json, key);
    if (!p || *p != '"') {
        return false;
    }
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_cap) {
        if (*p == '\\' && p[1] != '\0') {
            p++;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (*p == '"');
}

static bool json_extract_int(const char *json, const char *key, int32_t *out_val)
{
    if (!json || !key || !out_val) {
        return false;
    }
    const char *p = json_find_key_value(json, key);
    if (!p) {
        return false;
    }
    while (*p && (isspace((unsigned char)*p) || *p == '"')) {
        p++;
    }
    if (strncmp(p, "true", 4) == 0) {
        *out_val = 1;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out_val = 0;
        return true;
    }
    char *endptr = NULL;
    long v = strtol(p, &endptr, 10);
    if (endptr == p) {
        return false;
    }
    *out_val = (int32_t)v;
    return true;
}

static uint32_t json_extract_floats(const char *json, const char *key, float out_vals[4])
{
    if (!json || !key || !out_vals) {
        return 0;
    }
    const char *p = json_find_key_value(json, key);
    if (!p) {
        return 0;
    }

    if (*p == '"') {
        /* Space-separated float string like "1 0.5 0.2" */
        p++;
        uint32_t count = 0;
        while (*p && *p != '"' && count < 4) {
            while (*p && (isspace((unsigned char)*p) || *p == ',')) {
                p++;
            }
            if (*p == '"' || *p == '\0') {
                break;
            }
            char *endptr = NULL;
            float v = strtof(p, &endptr);
            if (endptr == p) {
                return 0;
            }
            out_vals[count++] = v;
            p = endptr;
        }
        return count;
    } else if (*p == '[') {
        p++;
        uint32_t count = 0;
        while (*p && *p != ']' && count < 4) {
            while (*p && (isspace((unsigned char)*p) || *p == ',')) {
                p++;
            }
            if (*p == ']' || *p == '\0') {
                break;
            }
            char *endptr = NULL;
            float v = strtof(p, &endptr);
            if (endptr == p) {
                break;
            }
            out_vals[count++] = v;
            p = endptr;
        }
        return count;
    } else if (strncmp(p, "true", 4) == 0) {
        out_vals[0] = 1.0f;
        return 1;
    } else if (strncmp(p, "false", 5) == 0) {
        out_vals[0] = 0.0f;
        return 1;
    } else {
        char *endptr = NULL;
        float v = strtof(p, &endptr);
        if (endptr != p) {
            out_vals[0] = v;
            return 1;
        }
    }
    return 0;
}

static bool is_valid_glsl_ident(const char *name)
{
    if (!name || name[0] == '\0') {
        return false;
    }
    if (!isalpha((unsigned char)name[0]) && name[0] != '_') {
        return false;
    }
    for (size_t i = 1; name[i] != '\0'; i++) {
        if (!isalnum((unsigned char)name[i]) && name[i] != '_') {
            return false;
        }
    }
    return true;
}

static void meta_set_combo(we_shader_meta_t *meta, const char *name, int32_t val, bool overwrite)
{
    if (!meta || !is_valid_glsl_ident(name)) {
        return;
    }
    for (uint32_t i = 0; i < meta->combo_count; i++) {
        if (strcmp(meta->combos[i].name, name) == 0) {
            if (overwrite) {
                meta->combos[i].value = val;
            }
            return;
        }
    }
    if (meta->combo_count < WE_SHADER_MAX_COMBOS) {
        snprintf(meta->combos[meta->combo_count].name, sizeof(meta->combos[0].name), "%s", name);
        meta->combos[meta->combo_count].value = val;
        meta->combo_count++;
    }
}

static void mask_block_comments_preserve_line_comments(char *buf, size_t len, bool *inout_in_block)
{
    size_t i = 0;
    while (i < len) {
        if (*inout_in_block) {
            if (i + 1 < len && buf[i] == '*' && buf[i + 1] == '/') {
                buf[i] = ' ';
                buf[i + 1] = ' ';
                i += 2;
                *inout_in_block = false;
            } else {
                if (buf[i] != '\n' && buf[i] != '\r') {
                    buf[i] = ' ';
                }
                i++;
            }
        } else {
            if (buf[i] == '"') {
                i++;
                while (i < len && buf[i] != '"') {
                    if (buf[i] == '\\' && i + 1 < len) {
                        i += 2;
                    } else {
                        i++;
                    }
                }
                if (i < len) {
                    i++;
                }
            } else if (i + 1 < len && buf[i] == '/' && buf[i + 1] == '/') {
                while (i < len && buf[i] != '\n') {
                    i++;
                }
            } else if (i + 1 < len && buf[i] == '/' && buf[i + 1] == '*') {
                buf[i] = ' ';
                buf[i + 1] = ' ';
                i += 2;
                *inout_in_block = true;
            } else {
                i++;
            }
        }
    }
}

static void build_masked_code_view(const char *src, size_t len, char *code_view,
                                   bool *inout_in_block)
{
    memcpy(code_view, src, len);
    code_view[len] = '\0';
    size_t i = 0;
    while (i < len) {
        if (*inout_in_block) {
            if (i + 1 < len && src[i] == '*' && src[i + 1] == '/') {
                code_view[i] = ' ';
                code_view[i + 1] = ' ';
                i += 2;
                *inout_in_block = false;
            } else {
                if (code_view[i] != '\n' && code_view[i] != '\r') {
                    code_view[i] = ' ';
                }
                i++;
            }
        } else {
            if (src[i] == '"') {
                code_view[i++] = ' ';
                while (i < len && src[i] != '"') {
                    if (src[i] == '\\' && i + 1 < len) {
                        code_view[i] = ' ';
                        code_view[i + 1] = ' ';
                        i += 2;
                    } else {
                        if (code_view[i] != '\n' && code_view[i] != '\r') {
                            code_view[i] = ' ';
                        }
                        i++;
                    }
                }
                if (i < len) {
                    code_view[i++] = ' ';
                }
            } else if (i + 1 < len && src[i] == '/' && src[i + 1] == '/') {
                while (i < len && src[i] != '\n') {
                    if (code_view[i] != '\r') {
                        code_view[i] = ' ';
                    }
                    i++;
                }
            } else if (i + 1 < len && src[i] == '/' && src[i + 1] == '*') {
                code_view[i] = ' ';
                code_view[i + 1] = ' ';
                i += 2;
                *inout_in_block = true;
            } else {
                i++;
            }
        }
    }
}

void we_shader_extract_metadata(const char *src, we_shader_meta_t *inout_meta)
{
    if (!src || !inout_meta) {
        return;
    }
    bool in_block_comment = false;
    const char *line = src;
    while (*line) {
        const char *eol = strchr(line, '\n');
        size_t line_len = eol ? (size_t)(eol - line) : strlen(line);

        char stack_buf[1024];
        char *buf = (line_len < sizeof(stack_buf)) ? stack_buf : (char *)malloc(line_len + 1);
        if (!buf) {
            return;
        }
        memcpy(buf, line, line_len);
        buf[line_len] = '\0';
        mask_block_comments_preserve_line_comments(buf, line_len, &in_block_comment);

        /* Check for // [COMBO] {"combo":"NAME","default":N} */
        const char *line_comment = strstr(buf, "//");
        const char *combo_tag = line_comment ? strstr(line_comment, "[COMBO]") : NULL;
        if (combo_tag) {
            const char *json = strchr(combo_tag, '{');
            if (json) {
                char cname[64] = {0};
                int32_t cdef = 0;
                if (json_extract_string(json, "combo", cname, sizeof(cname)) ||
                    json_extract_string(json, "name", cname, sizeof(cname))) {
                    json_extract_int(json, "default", &cdef);
                    meta_set_combo(inout_meta, cname, cdef, false);
                }
            }
        }

        /* Check for uniform <type> <name>; // { ... } */
        const char *p = buf;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if (strncmp(p, "uniform", 7) == 0 && isspace((unsigned char)p[7])) {
            const char *comment = strstr(p, "//");
            const char *semi = strchr(p, ';');
            if (semi && (!comment || semi < comment)) {
                /* Tokenize uniform declaration before ';' */
                char decl[256];
                size_t dlen = (size_t)(semi - (p + 7));
                if (dlen >= sizeof(decl)) {
                    dlen = sizeof(decl) - 1;
                }
                memcpy(decl, p + 7, dlen);
                decl[dlen] = '\0';

                /* Strip optional initializer before '=' */
                char *eq_decl = strchr(decl, '=');
                if (eq_decl) {
                    *eq_decl = '\0';
                }

                /* Strip optional precision qualifiers */
                char tok1[64] = {0}, tok2[64] = {0}, tok3[64] = {0};
                int ntok = sscanf(decl, "%63s %63s %63s", tok1, tok2, tok3);
                const char *utype = NULL;
                const char *uname = NULL;
                if (ntok == 3 && (strcmp(tok1, "highp") == 0 || strcmp(tok1, "mediump") == 0 ||
                                  strcmp(tok1, "lowp") == 0)) {
                    utype = tok2;
                    uname = tok3;
                } else if (ntok >= 2) {
                    utype = tok1;
                    uname = tok2;
                }

                if (utype && uname) {
                    char clean_name[64];
                    snprintf(clean_name, sizeof(clean_name), "%s", uname);
                    char *bracket = strchr(clean_name, '[');
                    if (bracket) {
                        *bracket = '\0';
                    }
                    char *eq = strchr(clean_name, '=');
                    if (eq) {
                        *eq = '\0';
                    }

                    /* Find or allocate uniform metadata entry */
                    we_shader_uniform_meta_t *um = NULL;
                    for (uint32_t i = 0; i < inout_meta->uniform_count; i++) {
                        if (strcmp(inout_meta->uniforms[i].name, clean_name) == 0) {
                            um = &inout_meta->uniforms[i];
                            break;
                        }
                    }
                    if (!um && inout_meta->uniform_count < WE_SHADER_MAX_UNIFORMS) {
                        um = &inout_meta->uniforms[inout_meta->uniform_count++];
                        memset(um, 0, sizeof(*um));
                        snprintf(um->name, sizeof(um->name), "%.*s", (int)(sizeof(um->name) - 1),
                                 clean_name);
                        snprintf(um->type, sizeof(um->type), "%.*s", (int)(sizeof(um->type) - 1),
                                 utype);
                        um->sampler_slot = -1;
                        if (strncmp(clean_name, "g_Texture", 9) == 0 && clean_name[9] >= '0' &&
                            clean_name[9] <= '7' && clean_name[10] == '\0') {
                            um->sampler_slot = clean_name[9] - '0';
                        }
                    }

                    if (um && comment) {
                        const char *json = strchr(comment, '{');
                        if (json) {
                            json_extract_string(json, "material", um->material_key,
                                                sizeof(um->material_key));
                            json_extract_string(json, "combo", um->combo_name,
                                                sizeof(um->combo_name));
                            if (strcmp(um->type, "sampler2D") == 0) {
                                json_extract_string(json, "default", um->default_tex,
                                                    sizeof(um->default_tex));
                            } else {
                                uint32_t nf = json_extract_floats(json, "default", um->default_val);
                                if (nf > 0) {
                                    um->num_components = nf;
                                    um->has_default_val = true;
                                }
                            }
                        }
                    }
                }
            }
        }

        if (buf != stack_buf) {
            free(buf);
        }
        if (!eol) {
            break;
        }
        line = eol + 1;
    }
}

/* =========================================================================
 * #include / #require Recursive Expansion (Deduplicated per Stage)
 * ========================================================================= */

typedef struct {
    char included[WE_SHADER_MAX_INCLUDES][128];
    uint32_t count;
} we_include_tracker_t;

typedef enum {
    WE_INC_NEW = 0,
    WE_INC_ALREADY_SEEN = 1,
    WE_INC_OVERFLOW = 2,
} we_inc_status_t;

static void normalize_include_path(const char *inc_name, char *norm, size_t norm_cap)
{
    if (!inc_name || !norm || norm_cap == 0) {
        return;
    }
    const char *p = inc_name;
    while (*p != '\0') {
        if (*p == '/' || *p == '\\' || *p == ' ' || *p == '\t') {
            p++;
        } else if (p[0] == '.' && (p[1] == '/' || p[1] == '\\')) {
            p += 2;
        } else {
            break;
        }
    }
    if (strncasecmp(p, "shaders/", 8) == 0 || strncasecmp(p, "shaders\\", 8) == 0) {
        p += 8;
        while (*p == '/' || *p == '\\' || (p[0] == '.' && (p[1] == '/' || p[1] == '\\'))) {
            if (*p == '.') {
                p += 2;
            } else {
                p++;
            }
        }
    }

    size_t out = 0;
    bool prev_slash = false;
    for (; *p != '\0' && out + 1 < norm_cap; ++p) {
        char c = (*p == '\\') ? '/' : (char)tolower((unsigned char)*p);
        if (c == '/') {
            if (prev_slash) {
                continue;
            }
            prev_slash = true;
        } else {
            prev_slash = false;
        }
        norm[out++] = c;
    }
    while (out > 0 && (norm[out - 1] == ' ' || norm[out - 1] == '\t' || norm[out - 1] == '/')) {
        out--;
    }
    norm[out] = '\0';
}

static we_inc_status_t tracker_check_or_add(we_include_tracker_t *tr, const char *inc_name)
{
    char norm[128];
    normalize_include_path(inc_name, norm, sizeof(norm));
    if (norm[0] == '\0') {
        return WE_INC_ALREADY_SEEN;
    }
    for (uint32_t j = 0; j < tr->count; j++) {
        if (strcmp(tr->included[j], norm) == 0) {
            return WE_INC_ALREADY_SEEN;
        }
    }
    if (tr->count >= WE_SHADER_MAX_INCLUDES) {
        return WE_INC_OVERFLOW;
    }
    snprintf(tr->included[tr->count++], sizeof(tr->included[0]), "%s", norm);
    return WE_INC_NEW;
}

static bool expand_includes_recursive(const char *src, const we_pkg_t *pkg,
                                      we_include_tracker_t *tr, we_strbuf_t *out, int depth)
{
    if (!src || depth > 8) {
        return true;
    }

    bool in_block_comment = false;
    const char *line = src;
    while (*line) {
        const char *eol = strchr(line, '\n');
        size_t line_len = eol ? (size_t)(eol - line) : strlen(line);

        char stack_view[1024];
        char *code_view =
            (line_len < sizeof(stack_view)) ? stack_view : (char *)malloc(line_len + 1);
        if (!code_view) {
            return false;
        }
        bool line_started_in_block = in_block_comment;
        mask_block_comments_preserve_line_comments(memcpy(code_view, line, line_len), line_len,
                                                   &in_block_comment);
        code_view[line_len] = '\0';

        const char *p = code_view;
        const char *line_end = code_view + line_len;
        for (const char *c = code_view; c + 1 < line_end; ++c) {
            if (c[0] == '/' && c[1] == '/') {
                line_end = c;
                break;
            }
        }
        while (p < line_end && (*p == ' ' || *p == '\t')) {
            p++;
        }

        bool is_include = false;
        bool is_version = false;
        char inc_path[256] = {0};
        if (!line_started_in_block && p < line_end && *p == '#') {
            p++;
            while (p < line_end && (*p == ' ' || *p == '\t')) {
                p++;
            }
            if (line_end - p >= 7 && strncmp(p, "version", 7) == 0 &&
                (p + 7 == line_end || isspace((unsigned char)p[7]))) {
                is_version = true;
            } else if ((line_end - p >= 7 && strncmp(p, "include", 7) == 0 &&
                        (p + 7 < line_end &&
                         (p[7] == ' ' || p[7] == '\t' || p[7] == '"' || p[7] == '<'))) ||
                       (line_end - p >= 7 && strncmp(p, "require", 7) == 0 &&
                        (p + 7 < line_end &&
                         (p[7] == ' ' || p[7] == '\t' || p[7] == '"' || p[7] == '<')))) {
                size_t raw_off = (size_t)((p + 7) - code_view);
                const char *raw_p = line + raw_off;
                const char *raw_end = line + (size_t)(line_end - code_view);
                const char *q1 = memchr(raw_p, '"', (size_t)(raw_end - raw_p));
                if (!q1) {
                    q1 = memchr(raw_p, '<', (size_t)(raw_end - raw_p));
                }
                if (q1) {
                    char close_ch = (*q1 == '<') ? '>' : '"';
                    const char *q2 = memchr(q1 + 1, close_ch, (size_t)(raw_end - (q1 + 1)));
                    if (q2 && q2 > q1 + 1) {
                        size_t plen = (size_t)(q2 - (q1 + 1));
                        if (plen < sizeof(inc_path)) {
                            memcpy(inc_path, q1 + 1, plen);
                            inc_path[plen] = '\0';
                            is_include = true;
                        }
                    }
                }
            }
        }

        if (code_view != stack_view) {
            free(code_view);
        }

        if (is_version) {
            /* Strip embedded #version line so our #version 310 es stays first */
        } else if (is_include) {
            we_inc_status_t st = tracker_check_or_add(tr, inc_path);
            if (st == WE_INC_OVERFLOW) {
                return false;
            }
            if (st == WE_INC_NEW) {
                bool owned = false;
                size_t pkg_sz = 0;
                const uint8_t *pkg_data = NULL;
                if (pkg) {
                    pkg_data = we_pkg_resolve(pkg, inc_path, &pkg_sz, &owned);
                    if (!pkg_data) {
                        char prefixed[300];
                        snprintf(prefixed, sizeof(prefixed), "shaders/%s", inc_path);
                        pkg_data = we_pkg_resolve(pkg, prefixed, &pkg_sz, &owned);
                    }
                }
                if (pkg_data && pkg_sz > 0) {
                    char *inc_str = malloc(pkg_sz + 1);
                    if (!inc_str) {
                        if (owned) {
                            free((void *)pkg_data);
                        }
                        return false;
                    }
                    memcpy(inc_str, pkg_data, pkg_sz);
                    inc_str[pkg_sz] = '\0';
                    if (owned) {
                        free((void *)pkg_data);
                    }
                    bool ok = expand_includes_recursive(inc_str, pkg, tr, out, depth + 1);
                    free(inc_str);
                    if (!ok) {
                        return false;
                    }
                } else {
                    if (owned && pkg_data) {
                        free((void *)pkg_data);
                    }
                    const char *builtin = we_shader_get_builtin(inc_path);
                    if (builtin) {
                        if (!expand_includes_recursive(builtin, pkg, tr, out, depth + 1)) {
                            return false;
                        }
                    }
                }
            }
        } else {
            if (!sb_append_len(out, line, line_len) || !sb_append_len(out, "\n", 1)) {
                return false;
            }
        }

        if (!eol) {
            break;
        }
        line = eol + 1;
    }
    return true;
}

/* =========================================================================
 * Varying Reconciliation & Legacy Keyword Rewriting
 * ========================================================================= */

typedef struct {
    char name[64];
    int width;       /* 1=float, 2=vec2, 3=vec3, 4=vec4 */
    char interp[16]; /* "flat", "centroid", or "" */
} we_varying_info_t;

static int type_to_width(const char *type)
{
    if (strcmp(type, "float") == 0) {
        return 1;
    }
    if (strcmp(type, "vec2") == 0 || strcmp(type, "float2") == 0) {
        return 2;
    }
    if (strcmp(type, "vec3") == 0 || strcmp(type, "float3") == 0) {
        return 3;
    }
    if (strcmp(type, "vec4") == 0 || strcmp(type, "float4") == 0) {
        return 4;
    }
    if (strcmp(type, "int") == 0 || strcmp(type, "uint") == 0) {
        return 1;
    }
    if (strcmp(type, "ivec2") == 0 || strcmp(type, "uvec2") == 0) {
        return 2;
    }
    if (strcmp(type, "ivec3") == 0 || strcmp(type, "uvec3") == 0) {
        return 3;
    }
    if (strcmp(type, "ivec4") == 0 || strcmp(type, "uvec4") == 0) {
        return 4;
    }
    return 0;
}

static bool pp_lookup_macro(const char *name, size_t len, const we_shader_meta_t *meta,
                            const we_shader_combo_t *local_defs, uint32_t local_count, int *out_val)
{
    if (out_val) {
        *out_val = 0;
    }
    if (!name || len == 0) {
        return false;
    }
    for (uint32_t i = local_count; i > 0; i--) {
        const we_shader_combo_t *c = &local_defs[i - 1];
        if (strlen(c->name) == len && strncmp(c->name, name, len) == 0) {
            if (out_val) {
                *out_val = c->value;
            }
            return true;
        }
    }
    if (meta) {
        for (uint32_t i = 0; i < meta->combo_count; i++) {
            const we_shader_combo_t *c = &meta->combos[i];
            if (strlen(c->name) == len && strncmp(c->name, name, len) == 0) {
                if (out_val) {
                    *out_val = c->value;
                }
                return true;
            }
        }
    }
    return false;
}

static void pp_skip_ws(const char **pp, const char *end)
{
    while (*pp < end) {
        if (**pp == ' ' || **pp == '\t' || **pp == '\r') {
            (*pp)++;
        } else if (**pp == '\\' && *pp + 1 < end && (*pp)[1] == '\n') {
            *pp += 2;
        } else if (**pp == '\\' && *pp + 2 < end && (*pp)[1] == '\r' && (*pp)[2] == '\n') {
            *pp += 3;
        } else if (*pp + 1 < end && (*pp)[0] == '/' && (*pp)[1] == '*') {
            *pp += 2;
            while (*pp + 1 < end && !((*pp)[0] == '*' && (*pp)[1] == '/')) {
                (*pp)++;
            }
            if (*pp + 1 < end) {
                *pp += 2;
            }
        } else {
            break;
        }
    }
}

static int64_t eval_pp_expr(const char **pp, const char *end, const we_shader_meta_t *meta,
                            const we_shader_combo_t *local_defs, uint32_t local_count);

static int64_t eval_pp_primary(const char **pp, const char *end, const we_shader_meta_t *meta,
                               const we_shader_combo_t *local_defs, uint32_t local_count)
{
    pp_skip_ws(pp, end);
    if (*pp >= end) {
        return 0LL;
    }
    if (**pp == '(') {
        (*pp)++;
        int64_t v = eval_pp_expr(pp, end, meta, local_defs, local_count);
        pp_skip_ws(pp, end);
        if (*pp < end && **pp == ')') {
            (*pp)++;
        }
        return v;
    }
    if (isdigit((unsigned char)**pp)) {
        char *next = NULL;
        long long v = strtoll(*pp, &next, 0);
        if (next && next > *pp) {
            *pp = (next <= end) ? next : end;
            while (*pp < end && (**pp == 'u' || **pp == 'U' || **pp == 'l' || **pp == 'L')) {
                (*pp)++;
            }
        } else {
            (*pp)++;
        }
        return (int64_t)v;
    }
    if (end - *pp >= 7 && strncmp(*pp, "defined", 7) == 0 &&
        (*pp + 7 == end || !(isalnum((unsigned char)(*pp)[7]) || (*pp)[7] == '_'))) {
        *pp += 7;
        pp_skip_ws(pp, end);
        bool has_paren = false;
        if (*pp < end && **pp == '(') {
            has_paren = true;
            (*pp)++;
            pp_skip_ws(pp, end);
        }
        const char *id_start = *pp;
        while (*pp < end && (isalnum((unsigned char)**pp) || **pp == '_')) {
            (*pp)++;
        }
        size_t id_len = (size_t)(*pp - id_start);
        if (has_paren) {
            pp_skip_ws(pp, end);
            if (*pp < end && **pp == ')') {
                (*pp)++;
            }
        }
        return pp_lookup_macro(id_start, id_len, meta, local_defs, local_count, NULL) ? 1LL : 0LL;
    }
    if (isalpha((unsigned char)**pp) || **pp == '_') {
        const char *id_start = *pp;
        while (*pp < end && (isalnum((unsigned char)**pp) || **pp == '_')) {
            (*pp)++;
        }
        int val = 0;
        pp_lookup_macro(id_start, (size_t)(*pp - id_start), meta, local_defs, local_count, &val);
        return (int64_t)val;
    }
    return 0LL;
}

static int64_t eval_pp_unary(const char **pp, const char *end, const we_shader_meta_t *meta,
                             const we_shader_combo_t *local_defs, uint32_t local_count)
{
    pp_skip_ws(pp, end);
    if (*pp >= end) {
        return 0LL;
    }
    if (**pp == '!' && (*pp + 1 >= end || (*pp)[1] != '=')) {
        (*pp)++;
        return !eval_pp_unary(pp, end, meta, local_defs, local_count);
    }
    if (**pp == '~') {
        (*pp)++;
        return ~eval_pp_unary(pp, end, meta, local_defs, local_count);
    }
    if (**pp == '+') {
        (*pp)++;
        return +eval_pp_unary(pp, end, meta, local_defs, local_count);
    }
    if (**pp == '-') {
        (*pp)++;
        return -eval_pp_unary(pp, end, meta, local_defs, local_count);
    }
    return eval_pp_primary(pp, end, meta, local_defs, local_count);
}

static int64_t eval_pp_mul(const char **pp, const char *end, const we_shader_meta_t *meta,
                           const we_shader_combo_t *local_defs, uint32_t local_count)
{
    int64_t lhs = eval_pp_unary(pp, end, meta, local_defs, local_count);
    for (;;) {
        pp_skip_ws(pp, end);
        if (*pp < end && **pp == '*') {
            (*pp)++;
            lhs *= eval_pp_unary(pp, end, meta, local_defs, local_count);
        } else if (*pp < end && **pp == '/' &&
                   (*pp + 1 >= end || ((*pp)[1] != '/' && (*pp)[1] != '*'))) {
            (*pp)++;
            int64_t rhs = eval_pp_unary(pp, end, meta, local_defs, local_count);
            lhs = (rhs != 0) ? (lhs / rhs) : 0LL;
        } else if (*pp < end && **pp == '%') {
            (*pp)++;
            int64_t rhs = eval_pp_unary(pp, end, meta, local_defs, local_count);
            lhs = (rhs != 0) ? (lhs % rhs) : 0LL;
        } else {
            break;
        }
    }
    return lhs;
}

static int64_t eval_pp_add(const char **pp, const char *end, const we_shader_meta_t *meta,
                           const we_shader_combo_t *local_defs, uint32_t local_count)
{
    int64_t lhs = eval_pp_mul(pp, end, meta, local_defs, local_count);
    for (;;) {
        pp_skip_ws(pp, end);
        if (*pp < end && **pp == '+') {
            (*pp)++;
            lhs += eval_pp_mul(pp, end, meta, local_defs, local_count);
        } else if (*pp < end && **pp == '-') {
            (*pp)++;
            lhs -= eval_pp_mul(pp, end, meta, local_defs, local_count);
        } else {
            break;
        }
    }
    return lhs;
}

static int64_t eval_pp_shift(const char **pp, const char *end, const we_shader_meta_t *meta,
                             const we_shader_combo_t *local_defs, uint32_t local_count)
{
    int64_t lhs = eval_pp_add(pp, end, meta, local_defs, local_count);
    for (;;) {
        pp_skip_ws(pp, end);
        if (end - *pp >= 2 && (*pp)[0] == '<' && (*pp)[1] == '<') {
            *pp += 2;
            int64_t shift = eval_pp_add(pp, end, meta, local_defs, local_count) & 63;
            lhs = lhs << shift;
        } else if (end - *pp >= 2 && (*pp)[0] == '>' && (*pp)[1] == '>') {
            *pp += 2;
            int64_t shift = eval_pp_add(pp, end, meta, local_defs, local_count) & 63;
            lhs = lhs >> shift;
        } else {
            break;
        }
    }
    return lhs;
}

static int64_t eval_pp_rel(const char **pp, const char *end, const we_shader_meta_t *meta,
                           const we_shader_combo_t *local_defs, uint32_t local_count)
{
    int64_t lhs = eval_pp_shift(pp, end, meta, local_defs, local_count);
    for (;;) {
        pp_skip_ws(pp, end);
        if (end - *pp >= 2 && (*pp)[0] == '<' && (*pp)[1] == '=') {
            *pp += 2;
            lhs = (lhs <= eval_pp_shift(pp, end, meta, local_defs, local_count)) ? 1LL : 0LL;
        } else if (end - *pp >= 2 && (*pp)[0] == '>' && (*pp)[1] == '=') {
            *pp += 2;
            lhs = (lhs >= eval_pp_shift(pp, end, meta, local_defs, local_count)) ? 1LL : 0LL;
        } else if (*pp < end && **pp == '<' && (*pp + 1 >= end || (*pp)[1] != '<')) {
            (*pp)++;
            lhs = (lhs < eval_pp_shift(pp, end, meta, local_defs, local_count)) ? 1LL : 0LL;
        } else if (*pp < end && **pp == '>' && (*pp + 1 >= end || (*pp)[1] != '>')) {
            (*pp)++;
            lhs = (lhs > eval_pp_shift(pp, end, meta, local_defs, local_count)) ? 1LL : 0LL;
        } else {
            break;
        }
    }
    return lhs;
}

static int64_t eval_pp_eq(const char **pp, const char *end, const we_shader_meta_t *meta,
                          const we_shader_combo_t *local_defs, uint32_t local_count)
{
    int64_t lhs = eval_pp_rel(pp, end, meta, local_defs, local_count);
    for (;;) {
        pp_skip_ws(pp, end);
        if (end - *pp >= 2 && (*pp)[0] == '=' && (*pp)[1] == '=') {
            *pp += 2;
            lhs = (lhs == eval_pp_rel(pp, end, meta, local_defs, local_count)) ? 1LL : 0LL;
        } else if (end - *pp >= 2 && (*pp)[0] == '!' && (*pp)[1] == '=') {
            *pp += 2;
            lhs = (lhs != eval_pp_rel(pp, end, meta, local_defs, local_count)) ? 1LL : 0LL;
        } else {
            break;
        }
    }
    return lhs;
}

static int64_t eval_pp_bit_and(const char **pp, const char *end, const we_shader_meta_t *meta,
                               const we_shader_combo_t *local_defs, uint32_t local_count)
{
    int64_t lhs = eval_pp_eq(pp, end, meta, local_defs, local_count);
    for (;;) {
        pp_skip_ws(pp, end);
        if (*pp < end && **pp == '&' && (*pp + 1 >= end || (*pp)[1] != '&')) {
            (*pp)++;
            lhs &= eval_pp_eq(pp, end, meta, local_defs, local_count);
        } else {
            break;
        }
    }
    return lhs;
}

static int64_t eval_pp_bit_xor(const char **pp, const char *end, const we_shader_meta_t *meta,
                               const we_shader_combo_t *local_defs, uint32_t local_count)
{
    int64_t lhs = eval_pp_bit_and(pp, end, meta, local_defs, local_count);
    for (;;) {
        pp_skip_ws(pp, end);
        if (*pp < end && **pp == '^') {
            (*pp)++;
            lhs ^= eval_pp_bit_and(pp, end, meta, local_defs, local_count);
        } else {
            break;
        }
    }
    return lhs;
}

static int64_t eval_pp_bit_or(const char **pp, const char *end, const we_shader_meta_t *meta,
                              const we_shader_combo_t *local_defs, uint32_t local_count)
{
    int64_t lhs = eval_pp_bit_xor(pp, end, meta, local_defs, local_count);
    for (;;) {
        pp_skip_ws(pp, end);
        if (*pp < end && **pp == '|' && (*pp + 1 >= end || (*pp)[1] != '|')) {
            (*pp)++;
            lhs |= eval_pp_bit_xor(pp, end, meta, local_defs, local_count);
        } else {
            break;
        }
    }
    return lhs;
}

static int64_t eval_pp_and(const char **pp, const char *end, const we_shader_meta_t *meta,
                           const we_shader_combo_t *local_defs, uint32_t local_count)
{
    int64_t lhs = eval_pp_bit_or(pp, end, meta, local_defs, local_count);
    for (;;) {
        pp_skip_ws(pp, end);
        if (end - *pp >= 2 && (*pp)[0] == '&' && (*pp)[1] == '&') {
            *pp += 2;
            int64_t rhs = eval_pp_bit_or(pp, end, meta, local_defs, local_count);
            lhs = (lhs && rhs) ? 1LL : 0LL;
        } else {
            break;
        }
    }
    return lhs;
}

static int64_t eval_pp_or_internal(const char **pp, const char *end, const we_shader_meta_t *meta,
                                   const we_shader_combo_t *local_defs, uint32_t local_count)
{
    int64_t lhs = eval_pp_and(pp, end, meta, local_defs, local_count);
    for (;;) {
        pp_skip_ws(pp, end);
        if (end - *pp >= 2 && (*pp)[0] == '|' && (*pp)[1] == '|') {
            *pp += 2;
            int64_t rhs = eval_pp_and(pp, end, meta, local_defs, local_count);
            lhs = (lhs || rhs) ? 1LL : 0LL;
        } else {
            break;
        }
    }
    return lhs;
}

static int64_t eval_pp_ternary(const char **pp, const char *end, const we_shader_meta_t *meta,
                               const we_shader_combo_t *local_defs, uint32_t local_count)
{
    int64_t cond = eval_pp_or_internal(pp, end, meta, local_defs, local_count);
    pp_skip_ws(pp, end);
    if (*pp < end && **pp == '?') {
        (*pp)++;
        int64_t true_val = eval_pp_expr(pp, end, meta, local_defs, local_count);
        pp_skip_ws(pp, end);
        if (*pp < end && **pp == ':') {
            (*pp)++;
            int64_t false_val = eval_pp_ternary(pp, end, meta, local_defs, local_count);
            return cond ? true_val : false_val;
        }
        return cond ? true_val : 0LL;
    }
    return cond;
}

static int64_t eval_pp_expr(const char **pp, const char *end, const we_shader_meta_t *meta,
                            const we_shader_combo_t *local_defs, uint32_t local_count)
{
    return eval_pp_ternary(pp, end, meta, local_defs, local_count);
}

static int eval_pp_or(const char **pp, const char *end, const we_shader_meta_t *meta,
                      const we_shader_combo_t *local_defs, uint32_t local_count)
{
    return (int)eval_pp_expr(pp, end, meta, local_defs, local_count);
}

static void collect_varyings(const char *src, const we_shader_meta_t *meta, we_varying_info_t *list,
                             uint32_t *inout_count)
{
    if (!src || !list || !inout_count) {
        return;
    }

    typedef struct {
        bool parent_active;
        bool branch_active;
        bool seen_true;
    } pp_cond_frame_t;

    pp_cond_frame_t cond_stack[32];
    uint32_t cond_depth = 0;
    we_shader_combo_t local_defs[64];
    uint32_t local_count = 0;
    bool in_block_comment = false;

    const char *line = src;
    while (*line) {
        const char *eol = strchr(line, '\n');
        while (eol) {
            const char *check = eol - 1;
            while (check >= line && (*check == ' ' || *check == '\t' || *check == '\r')) {
                check--;
            }
            if (check >= line && *check == '\\') {
                eol = strchr(eol + 1, '\n');
            } else {
                break;
            }
        }
        size_t line_len = eol ? (size_t)(eol - line) : strlen(line);

        char stack_view[1024];
        char *code_view =
            (line_len < sizeof(stack_view)) ? stack_view : (char *)malloc(line_len + 1);
        if (!code_view) {
            return;
        }
        build_masked_code_view(line, line_len, code_view, &in_block_comment);

        const char *p = code_view;
        const char *end = code_view + line_len;
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }

        bool cur_active = (cond_depth == 0) ? true : cond_stack[cond_depth - 1].branch_active;

        if (p < end && *p == '#') {
            p++;
            pp_skip_ws(&p, end);
            const char *kw = p;
            while (p < end && isalpha((unsigned char)*p)) {
                p++;
            }
            size_t kw_len = (size_t)(p - kw);
            pp_skip_ws(&p, end);

            if (kw_len == 5 && strncmp(kw, "ifdef", 5) == 0) {
                const char *id = p;
                while (p < end && (isalnum((unsigned char)*p) || *p == '_')) {
                    p++;
                }
                bool cond = cur_active && pp_lookup_macro(id, (size_t)(p - id), meta, local_defs,
                                                          local_count, NULL);
                if (cond_depth < 32) {
                    cond_stack[cond_depth++] = (pp_cond_frame_t){
                        .parent_active = cur_active, .branch_active = cond, .seen_true = cond};
                }
            } else if (kw_len == 6 && strncmp(kw, "ifndef", 6) == 0) {
                const char *id = p;
                while (p < end && (isalnum((unsigned char)*p) || *p == '_')) {
                    p++;
                }
                bool cond = cur_active && !pp_lookup_macro(id, (size_t)(p - id), meta, local_defs,
                                                           local_count, NULL);
                if (cond_depth < 32) {
                    cond_stack[cond_depth++] = (pp_cond_frame_t){
                        .parent_active = cur_active, .branch_active = cond, .seen_true = cond};
                }
            } else if (kw_len == 2 && strncmp(kw, "if", 2) == 0) {
                bool cond = cur_active && (eval_pp_or(&p, end, meta, local_defs, local_count) != 0);
                if (cond_depth < 32) {
                    cond_stack[cond_depth++] = (pp_cond_frame_t){
                        .parent_active = cur_active, .branch_active = cond, .seen_true = cond};
                }
            } else if (kw_len == 4 && strncmp(kw, "elif", 4) == 0 && cond_depth > 0) {
                pp_cond_frame_t *top = &cond_stack[cond_depth - 1];
                if (!top->parent_active || top->seen_true) {
                    top->branch_active = false;
                } else {
                    bool cond = (eval_pp_or(&p, end, meta, local_defs, local_count) != 0);
                    top->branch_active = cond;
                    top->seen_true = cond;
                }
            } else if (kw_len == 4 && strncmp(kw, "else", 4) == 0 && cond_depth > 0) {
                pp_cond_frame_t *top = &cond_stack[cond_depth - 1];
                top->branch_active = top->parent_active && !top->seen_true;
                top->seen_true = true;
            } else if (kw_len == 5 && strncmp(kw, "endif", 5) == 0 && cond_depth > 0) {
                cond_depth--;
            } else if (cur_active && kw_len == 6 && strncmp(kw, "define", 6) == 0) {
                const char *id = p;
                while (p < end && (isalnum((unsigned char)*p) || *p == '_')) {
                    p++;
                }
                size_t id_len = (size_t)(p - id);
                if (id_len > 0 && id_len < sizeof(local_defs[0].name) && (p >= end || *p != '(')) {
                    pp_skip_ws(&p, end);
                    int val = (p < end) ? eval_pp_or(&p, end, meta, local_defs, local_count) : 1;
                    if (local_count < 64) {
                        memcpy(local_defs[local_count].name, id, id_len);
                        local_defs[local_count].name[id_len] = '\0';
                        local_defs[local_count].value = val;
                        local_count++;
                    }
                }
            } else if (cur_active && kw_len == 5 && strncmp(kw, "undef", 5) == 0) {
                const char *id = p;
                while (p < end && (isalnum((unsigned char)*p) || *p == '_')) {
                    p++;
                }
                size_t id_len = (size_t)(p - id);
                for (uint32_t i = 0; i < local_count;) {
                    if (strlen(local_defs[i].name) == id_len &&
                        strncmp(local_defs[i].name, id, id_len) == 0) {
                        local_defs[i] = local_defs[--local_count];
                    } else {
                        i++;
                    }
                }
            }
        } else if (cur_active) {
            char decl[256];
            size_t dlen = (size_t)(end - p);
            if (dlen >= sizeof(decl)) {
                dlen = sizeof(decl) - 1;
            }
            memcpy(decl, p, dlen);
            decl[dlen] = '\0';
            char *semi = strchr(decl, ';');
            if (semi) {
                *semi = '\0';
            }
            char t[5][64];
            memset(t, 0, sizeof(t));
            int ntok = sscanf(decl, "%63s %63s %63s %63s %63s", t[0], t[1], t[2], t[3], t[4]);
            if (ntok >= 2 && semi != NULL) {
                int idx = 0;
                char interp[16] = {0};
                if (strcmp(t[idx], "flat") == 0 || strcmp(t[idx], "centroid") == 0 ||
                    strcmp(t[idx], "smooth") == 0) {
                    snprintf(interp, sizeof(interp), "%.15s", t[idx]);
                    idx++;
                }
                bool is_varying_decl = false;
                if (idx < ntok && (strcmp(t[idx], "varying") == 0 || strcmp(t[idx], "in") == 0 ||
                                   strcmp(t[idx], "out") == 0)) {
                    is_varying_decl = true;
                    idx++;
                }
                if (is_varying_decl && idx < ntok) {
                    if (strcmp(t[idx], "highp") == 0 || strcmp(t[idx], "mediump") == 0 ||
                        strcmp(t[idx], "lowp") == 0) {
                        idx++;
                    }
                    if (idx < ntok) {
                        const char *vtype = t[idx++];
                        const char *vname = (idx < ntok) ? t[idx] : NULL;
                        if (vtype && vname && strchr(vname, '[') == NULL &&
                            strchr(vname, '(') == NULL) {
                            int w = type_to_width(vtype);
                            if (w > 0) {
                                if (interp[0] == '\0' && (strncmp(vtype, "int", 3) == 0 ||
                                                          strncmp(vtype, "uint", 4) == 0 ||
                                                          strncmp(vtype, "ivec", 4) == 0 ||
                                                          strncmp(vtype, "uvec", 4) == 0)) {
                                    snprintf(interp, sizeof(interp), "flat");
                                }
                                bool updated = false;
                                for (uint32_t i = 0; i < *inout_count; i++) {
                                    if (strcmp(list[i].name, vname) == 0) {
                                        list[i].width = w;
                                        if (interp[0] != '\0') {
                                            snprintf(list[i].interp, sizeof(list[i].interp),
                                                     "%.15s", interp);
                                        }
                                        updated = true;
                                        break;
                                    }
                                }
                                if (!updated && *inout_count < WE_SHADER_MAX_VARYINGS) {
                                    snprintf(list[*inout_count].name, sizeof(list[0].name), "%s",
                                             vname);
                                    list[*inout_count].width = w;
                                    snprintf(list[*inout_count].interp, sizeof(list[0].interp),
                                             "%.15s", interp);
                                    (*inout_count)++;
                                }
                            }
                        }
                    }
                }
            }
        }
        if (code_view != stack_view) {
            free(code_view);
        }
        if (!eol) {
            break;
        }
        line = eol + 1;
    }
}

typedef struct {
    char name[64];
    char swiz[8];
} we_varying_swizzle_t;

static bool is_glsl_or_hlsl_type_token(const char *tok, size_t len, const char **out_canonical)
{
    static const struct {
        const char *src;
        const char *dst;
    } map[] = {
        {"float", "float"}, {"half", "float"},  {"fixed", "float"}, {"int", "int"},
        {"uint", "uint"},   {"dword", "uint"},  {"bool", "bool"},   {"vec2", "vec2"},
        {"vec3", "vec3"},   {"vec4", "vec4"},   {"float2", "vec2"}, {"float3", "vec3"},
        {"float4", "vec4"}, {"half2", "vec2"},  {"half3", "vec3"},  {"half4", "vec4"},
        {"fixed2", "vec2"}, {"fixed3", "vec3"}, {"fixed4", "vec4"}, {"ivec2", "ivec2"},
        {"ivec3", "ivec3"}, {"ivec4", "ivec4"}, {"int2", "ivec2"},  {"int3", "ivec3"},
        {"int4", "ivec4"},  {"uvec2", "uvec2"}, {"uvec3", "uvec3"}, {"uvec4", "uvec4"},
        {"uint2", "uvec2"}, {"uint3", "uvec3"}, {"uint4", "uvec4"}, {"bvec2", "bvec2"},
        {"bvec3", "bvec3"}, {"bvec4", "bvec4"},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        if (strlen(map[i].src) == len && strncmp(tok, map[i].src, len) == 0) {
            if (out_canonical) {
                *out_canonical = map[i].dst;
            }
            return true;
        }
    }
    return false;
}

static bool append_line_with_varying_swizzles(we_strbuf_t *sb, const char *line,
                                              const char *code_view, size_t line_len,
                                              const we_varying_swizzle_t *swizzles,
                                              uint32_t swizzle_count)
{
    size_t cur = 0;
    size_t seg_start = 0;
    while (cur < line_len) {
        if (isalpha((unsigned char)code_view[cur]) || code_view[cur] == '_') {
            size_t id_start = cur;
            while (cur < line_len &&
                   (isalnum((unsigned char)code_view[cur]) || code_view[cur] == '_')) {
                cur++;
            }
            size_t id_len = cur - id_start;

            size_t before = id_start;
            while (before > 0 && (code_view[before - 1] == ' ' || code_view[before - 1] == '\t')) {
                before--;
            }
            if (before > 0 && code_view[before - 1] == '.') {
                continue;
            }

            size_t prev_end = before;
            size_t prev_start = prev_end;
            while (prev_start > 0 && (isalnum((unsigned char)code_view[prev_start - 1]) ||
                                      code_view[prev_start - 1] == '_')) {
                prev_start--;
            }
            size_t prev_len = prev_end - prev_start;
            bool preceded_by_type = (prev_len > 0 && is_glsl_or_hlsl_type_token(
                                                         code_view + prev_start, prev_len, NULL));

            /* Check for user function definition `float saturate(` to avoid macro expansion */
            if (id_len == 8 && strncmp(code_view + id_start, "saturate", 8) == 0) {
                size_t after = cur;
                while (after < line_len && (code_view[after] == ' ' || code_view[after] == '\t')) {
                    after++;
                }
                if (after < line_len && code_view[after] == '(' && preceded_by_type) {
                    if (!sb_append_len(sb, line + seg_start, id_start - seg_start) ||
                        !sb_append(sb, "we_user_saturate")) {
                        return false;
                    }
                    seg_start = cur;
                    continue;
                }
            }

            if (preceded_by_type) {
                continue;
            }

            for (uint32_t i = 0; i < swizzle_count; i++) {
                if (swizzles[i].swiz[0] != '\0' && strlen(swizzles[i].name) == id_len &&
                    strncmp(swizzles[i].name, code_view + id_start, id_len) == 0) {
                    size_t after = cur;
                    while (after < line_len &&
                           (code_view[after] == ' ' || code_view[after] == '\t')) {
                        after++;
                    }
                    if (after >= line_len || code_view[after] != '.') {
                        if (!sb_append_len(sb, line + seg_start, cur - seg_start) ||
                            !sb_append(sb, ".") || !sb_append(sb, swizzles[i].swiz)) {
                            return false;
                        }
                        seg_start = cur;
                    }
                    break;
                }
            }
        } else {
            cur++;
        }
    }

    return sb_append_len(sb, line + seg_start, line_len - seg_start) && sb_append(sb, "\n");
}

static void strip_hlsl_bracket_attributes_masked(char *line, char *code_view, size_t len)
{
    size_t pos = 0;
    while (pos < len) {
        char *p = (char *)memchr(code_view + pos, '[', len - pos);
        if (!p) {
            break;
        }
        size_t open_off = (size_t)(p - code_view);
        char *close = (char *)memchr(p + 1, ']', len - (open_off + 1));
        if (!close) {
            break;
        }
        size_t close_off = (size_t)(close - code_view);
        size_t attr_len = close_off - open_off + 1;
        const char *attr = p + 1;
        while (attr < close && (*attr == ' ' || *attr == '\t')) {
            attr++;
        }
        size_t rem = (size_t)(close - attr);
        if ((rem >= 6 && strncmp(attr, "unroll", 6) == 0) ||
            (rem >= 4 && strncmp(attr, "loop", 4) == 0) ||
            (rem >= 6 && strncmp(attr, "branch", 6) == 0) ||
            (rem >= 7 && strncmp(attr, "flatten", 7) == 0) ||
            (rem >= 7 && strncmp(attr, "fastopt", 7) == 0)) {
            memset(line + open_off, ' ', attr_len);
            memset(code_view + open_off, ' ', attr_len);
            pos = close_off + 1;
        } else {
            pos = open_off + 1;
        }
    }
}

static void strip_hlsl_storage_masked(char *line, char *code_view, size_t len)
{
    char *p = code_view;
    while ((p = strstr(p, "static")) != NULL) {
        size_t off = (size_t)(p - code_view);
        bool word_start = (off == 0 || !(isalnum((unsigned char)code_view[off - 1]) ||
                                         code_view[off - 1] == '_'));
        bool word_end = (off + 6 >= len || !(isalnum((unsigned char)code_view[off + 6]) ||
                                             code_view[off + 6] == '_'));
        if (word_start && word_end) {
            size_t after = off + 6;
            while (after < len && (code_view[after] == ' ' || code_view[after] == '\t')) {
                after++;
            }
            size_t before = off;
            while (before > 0 && (code_view[before - 1] == ' ' || code_view[before - 1] == '\t')) {
                before--;
            }
            if (after + 5 <= len && strncmp(code_view + after, "const", 5) == 0 &&
                (after + 5 == len ||
                 !(isalnum((unsigned char)code_view[after + 5]) || code_view[after + 5] == '_'))) {
                memset(line + off, ' ', 6);
                memset(code_view + off, ' ', 6);
            } else if (before == 0 || code_view[before - 1] == ';' ||
                       code_view[before - 1] == '{' || code_view[before - 1] == '}') {
                memset(line + off, ' ', 6);
                memset(code_view + off, ' ', 6);
            }
        }
        p += 6;
    }
}

typedef struct {
    char name[64];
    char glsl_type[16];
} we_var_type_entry_t;

static void record_var_types_from_code_line(const char *code_view, size_t len,
                                            we_var_type_entry_t *entries, uint32_t *inout_count,
                                            uint32_t cap)
{
    size_t i = 0;
    while (i < len) {
        if (!(isalpha((unsigned char)code_view[i]) || code_view[i] == '_')) {
            i++;
            continue;
        }
        size_t t_start = i;
        while (i < len && (isalnum((unsigned char)code_view[i]) || code_view[i] == '_')) {
            i++;
        }
        size_t t_len = i - t_start;
        const char *canon_type = NULL;
        if (!is_glsl_or_hlsl_type_token(code_view + t_start, t_len, &canon_type)) {
            continue;
        }
        size_t j = i;
        while (j < len && (code_view[j] == ' ' || code_view[j] == '\t')) {
            j++;
        }
        if (j >= len || !(isalpha((unsigned char)code_view[j]) || code_view[j] == '_')) {
            continue;
        }
        size_t n_start = j;
        while (j < len && (isalnum((unsigned char)code_view[j]) || code_view[j] == '_')) {
            j++;
        }
        size_t n_len = j - n_start;
        if (n_len == 0 || n_len >= sizeof(entries[0].name)) {
            continue;
        }
        size_t k = j;
        while (k < len && (code_view[k] == ' ' || code_view[k] == '\t')) {
            k++;
        }
        if (k < len && code_view[k] == '(') {
            continue;
        }
        char vname[64];
        memcpy(vname, code_view + n_start, n_len);
        vname[n_len] = '\0';
        bool updated = false;
        for (uint32_t idx = 0; idx < *inout_count; ++idx) {
            if (strcmp(entries[idx].name, vname) == 0) {
                snprintf(entries[idx].glsl_type, sizeof(entries[idx].glsl_type), "%s", canon_type);
                updated = true;
                break;
            }
        }
        if (!updated && *inout_count < cap) {
            snprintf(entries[*inout_count].name, sizeof(entries[0].name), "%s", vname);
            snprintf(entries[*inout_count].glsl_type, sizeof(entries[0].glsl_type), "%s",
                     canon_type);
            (*inout_count)++;
        }
        i = j;
    }
}

static const char *lookup_inferred_var_type(const we_var_type_entry_t *entries, uint32_t count,
                                            const char *vname, bool inside_subscript)
{
    for (uint32_t i = count; i > 0; --i) {
        if (strcmp(entries[i - 1].name, vname) == 0) {
            return entries[i - 1].glsl_type;
        }
    }
    return inside_subscript ? "int" : "float";
}

static bool line_starts_control_or_pp(const char *code_view, size_t len)
{
    size_t i = 0;
    while (i < len && (code_view[i] == ' ' || code_view[i] == '\t')) {
        i++;
    }
    if (i >= len || code_view[i] == '#') {
        return true;
    }
    static const char *const kw[] = {"if", "else", "for", "while", "do", "switch", "case"};
    for (size_t k = 0; k < sizeof(kw) / sizeof(kw[0]); ++k) {
        size_t klen = strlen(kw[k]);
        if (i + klen <= len && strncmp(code_view + i, kw[k], klen) == 0 &&
            (i + klen == len ||
             !(isalnum((unsigned char)code_view[i + klen]) || code_view[i + klen] == '_'))) {
            return true;
        }
    }
    return false;
}

static void update_unbraced_control_state(const char *code_view, size_t len,
                                          bool *inout_expects_unbraced)
{
    size_t s = 0;
    while (s < len && (code_view[s] == ' ' || code_view[s] == '\t' || code_view[s] == '\r')) {
        s++;
    }
    size_t e = len;
    while (e > s &&
           (code_view[e - 1] == ' ' || code_view[e - 1] == '\t' || code_view[e - 1] == '\r')) {
        e--;
    }
    if (s == e || code_view[s] == '#') {
        return;
    }
    char last = code_view[e - 1];
    if (last == '{' || last == '}' || last == ';') {
        *inout_expects_unbraced = false;
        return;
    }
    if (last == ')' && line_starts_control_or_pp(code_view + s, e - s)) {
        *inout_expects_unbraced = true;
        return;
    }
    if ((e - s == 4 && strncmp(code_view + s, "else", 4) == 0) ||
        (e - s == 2 && strncmp(code_view + s, "do", 2) == 0)) {
        *inout_expects_unbraced = true;
        return;
    }
    if ((e - s > 4 && strncmp(code_view + e - 4, "else", 4) == 0 &&
         !(isalnum((unsigned char)code_view[e - 5]) || code_view[e - 5] == '_')) ||
        (e - s > 2 && strncmp(code_view + e - 2, "do", 2) == 0 &&
         !(isalnum((unsigned char)code_view[e - 3]) || code_view[e - 3] == '_'))) {
        *inout_expects_unbraced = true;
        return;
    }
    *inout_expects_unbraced = false;
}

static bool span_contains_standalone_ident(const char *s, const char *end, const char *ident,
                                           size_t ident_len)
{
    const char *p = s;
    while (p < end) {
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *id_start = p;
            while (p < end && (isalnum((unsigned char)*p) || *p == '_')) {
                p++;
            }
            size_t len = (size_t)(p - id_start);
            if (len == ident_len && strncmp(id_start, ident, ident_len) == 0) {
                const char *b = id_start;
                while (b > s && (b[-1] == ' ' || b[-1] == '\t')) {
                    b--;
                }
                if (b == s || b[-1] != '.') {
                    return true;
                }
            }
        } else {
            p++;
        }
    }
    return false;
}

static bool hoist_mul_side_effects(we_strbuf_t *sb, const char *line, const char *code_view,
                                   size_t line_len, uint32_t *tmp_counter,
                                   bool prev_expects_unbraced, const we_var_type_entry_t *var_types,
                                   uint32_t var_type_count, char **out_rewritten)
{
    *out_rewritten = NULL;
    if (prev_expects_unbraced || line_starts_control_or_pp(code_view, line_len) ||
        strstr(code_view, "mul") == NULL) {
        return true;
    }

    we_strbuf_t rebuilt;
    bool any_hoisted = false;
    size_t emit_off = 0;

    const char *p = code_view;
    const char *view_end = code_view + line_len;
    while ((p = strstr(p, "mul")) != NULL) {
        if (p > code_view && (isalnum((unsigned char)p[-1]) || p[-1] == '_' || p[-1] == '.')) {
            p += 3;
            continue;
        }
        const char *open_p = p + 3;
        while (open_p < view_end && (*open_p == ' ' || *open_p == '\t')) {
            open_p++;
        }
        if (open_p >= view_end || *open_p != '(') {
            p += 3;
            continue;
        }

        const char *cur = open_p + 1;
        int paren_depth = 1;
        int bracket_depth = 0;
        const char *comma_p = NULL;
        bool multi_comma = false;
        while (cur < view_end && paren_depth > 0) {
            if (*cur == '(') {
                paren_depth++;
            } else if (*cur == ')') {
                paren_depth--;
                if (paren_depth == 0) {
                    cur++;
                    break;
                }
            } else if (*cur == '[') {
                bracket_depth++;
            } else if (*cur == ']' && bracket_depth > 0) {
                bracket_depth--;
            } else if (*cur == ',' && paren_depth == 1 && bracket_depth == 0) {
                if (!comma_p) {
                    comma_p = cur;
                } else {
                    multi_comma = true;
                }
            }
            cur++;
        }
        if (paren_depth != 0 || !comma_p || multi_comma) {
            p += 3;
            continue;
        }
        const char *mul_close = cur - 1;

        int sub_depth = 0;
        const char *inc = open_p + 1;
        while (inc + 1 < mul_close) {
            if (*inc == '[') {
                sub_depth++;
                inc++;
                continue;
            }
            if (*inc == ']' && sub_depth > 0) {
                sub_depth--;
                inc++;
                continue;
            }
            if ((inc[0] == '+' && inc[1] == '+') || (inc[0] == '-' && inc[1] == '-')) {
                bool is_inc = (inc[0] == '+');
                bool in_first_arg = (inc < comma_p);
                const char *other_s = in_first_arg ? (comma_p + 1) : (open_p + 1);
                const char *other_e = in_first_arg ? mul_close : comma_p;

                const char *back = inc;
                while (back > open_p + 1 && (back[-1] == ' ' || back[-1] == '\t')) {
                    back--;
                }
                if (back > open_p + 1 && (isalnum((unsigned char)back[-1]) || back[-1] == '_')) {
                    const char *v_end = back;
                    const char *v_start = back - 1;
                    while (v_start > open_p + 1 &&
                           (isalnum((unsigned char)v_start[-1]) || v_start[-1] == '_')) {
                        v_start--;
                    }
                    const char *dot_chk = v_start;
                    while (dot_chk > open_p + 1 && (dot_chk[-1] == ' ' || dot_chk[-1] == '\t')) {
                        dot_chk--;
                    }
                    bool is_member = (dot_chk > open_p + 1 && dot_chk[-1] == '.');
                    size_t v_len = (size_t)(v_end - v_start);
                    if (!is_member && v_len > 0 && v_len < 64 &&
                        span_contains_standalone_ident(other_s, other_e, v_start, v_len)) {
                        char vname[64];
                        memcpy(vname, v_start, v_len);
                        vname[v_len] = '\0';
                        const char *vtype = lookup_inferred_var_type(var_types, var_type_count,
                                                                     vname, sub_depth > 0);
                        char tmp_name[32];
                        snprintf(tmp_name, sizeof(tmp_name), "_we_tmp_%u", (*tmp_counter)++);
                        if (!sb_append(sb, vtype) || !sb_append(sb, " ") ||
                            !sb_append(sb, tmp_name) || !sb_append(sb, " = ") ||
                            !sb_append(sb, vname) || !sb_append(sb, ";\n") ||
                            !sb_append(sb, vname) || !sb_append(sb, is_inc ? "++;\n" : "--;\n")) {
                            if (any_hoisted) {
                                free(rebuilt.buf);
                            }
                            return false;
                        }
                        if (!any_hoisted) {
                            if (!sb_init(&rebuilt, line_len + 64)) {
                                return false;
                            }
                            any_hoisted = true;
                        }
                        size_t rep_start = (size_t)(v_start - code_view);
                        size_t rep_end = (size_t)((inc + 2) - code_view);
                        if (!sb_append_len(&rebuilt, line + emit_off, rep_start - emit_off) ||
                            !sb_append(&rebuilt, tmp_name)) {
                            free(rebuilt.buf);
                            return false;
                        }
                        emit_off = rep_end;
                    }
                    inc += 2;
                } else {
                    const char *v_start = inc + 2;
                    while (v_start < mul_close && (*v_start == ' ' || *v_start == '\t')) {
                        v_start++;
                    }
                    const char *v_end = v_start;
                    while (v_end < mul_close && (isalnum((unsigned char)*v_end) || *v_end == '_')) {
                        v_end++;
                    }
                    bool is_member = (v_end < mul_close && *v_end == '.');
                    size_t v_len = (size_t)(v_end - v_start);
                    if (!is_member && v_len > 0 && v_len < 64 &&
                        span_contains_standalone_ident(other_s, other_e, v_start, v_len)) {
                        char vname[64];
                        memcpy(vname, v_start, v_len);
                        vname[v_len] = '\0';
                        if (!sb_append(sb, is_inc ? "++" : "--") || !sb_append(sb, vname) ||
                            !sb_append(sb, ";\n")) {
                            if (any_hoisted) {
                                free(rebuilt.buf);
                            }
                            return false;
                        }
                        if (!any_hoisted) {
                            if (!sb_init(&rebuilt, line_len + 64)) {
                                return false;
                            }
                            any_hoisted = true;
                        }
                        size_t rep_start = (size_t)(inc - code_view);
                        size_t rep_end = (size_t)(v_end - code_view);
                        if (!sb_append_len(&rebuilt, line + emit_off, rep_start - emit_off) ||
                            !sb_append(&rebuilt, vname)) {
                            free(rebuilt.buf);
                            return false;
                        }
                        emit_off = rep_end;
                    }
                    inc = v_end;
                }
            } else {
                inc++;
            }
        }
        p = cur;
    }

    if (any_hoisted) {
        if (!sb_append_len(&rebuilt, line + emit_off, line_len - emit_off)) {
            free(rebuilt.buf);
            return false;
        }
        *out_rewritten = rebuilt.buf;
    }
    return true;
}

/* Rewrites legacy attribute/varying lines and optionally promotes specific varyings to vec4 */
static char *transform_stage_declarations(const char *expanded_src, bool is_frag,
                                          const we_varying_info_t *promoted_varyings,
                                          uint32_t promoted_count)
{
    we_strbuf_t sb;
    if (!sb_init(&sb, strlen(expanded_src) + 512)) {
        return NULL;
    }

    we_varying_swizzle_t active_swizzles[WE_SHADER_MAX_VARYINGS];
    uint32_t active_swizzle_count = 0;
    for (uint32_t i = 0; i < promoted_count && active_swizzle_count < WE_SHADER_MAX_VARYINGS; i++) {
        const char *swiz = "";
        if (promoted_varyings[i].width == 1) {
            swiz = "x";
        } else if (promoted_varyings[i].width == 2) {
            swiz = "xy";
        } else if (promoted_varyings[i].width == 3) {
            swiz = "xyz";
        }
        if (swiz[0] != '\0') {
            snprintf(active_swizzles[active_swizzle_count].name, sizeof(active_swizzles[0].name),
                     "%s", promoted_varyings[i].name);
            snprintf(active_swizzles[active_swizzle_count].swiz, sizeof(active_swizzles[0].swiz),
                     "%s", swiz);
            active_swizzle_count++;
        }
    }

    we_var_type_entry_t var_types[128];
    uint32_t var_type_count = 0;
    uint32_t tmp_counter = 0;
    bool in_block_comment = false;
    bool prev_expects_unbraced = false;
    const char *line = expanded_src;
    while (*line) {
        const char *eol = strchr(line, '\n');
        size_t line_len = eol ? (size_t)(eol - line) : strlen(line);

        char stack_buf[1024];
        char stack_view[1024];
        char *lbuf = (line_len < sizeof(stack_buf)) ? stack_buf : (char *)malloc(line_len + 1);
        char *code_view =
            (line_len < sizeof(stack_view)) ? stack_view : (char *)malloc(line_len + 1);
        if (!lbuf || !code_view) {
            if (lbuf && lbuf != stack_buf) {
                free(lbuf);
            }
            if (code_view && code_view != stack_view) {
                free(code_view);
            }
            free(sb.buf);
            return NULL;
        }
        memcpy(lbuf, line, line_len);
        lbuf[line_len] = '\0';
        bool line_start_in_block = in_block_comment;
        build_masked_code_view(lbuf, line_len, code_view, &in_block_comment);

        strip_hlsl_bracket_attributes_masked(lbuf, code_view, line_len);
        strip_hlsl_storage_masked(lbuf, code_view, line_len);
        record_var_types_from_code_line(code_view, line_len, var_types, &var_type_count, 128u);

        char *rewritten = NULL;
        if (!hoist_mul_side_effects(&sb, lbuf, code_view, line_len, &tmp_counter,
                                    prev_expects_unbraced, var_types, var_type_count, &rewritten)) {
            if (lbuf != stack_buf) {
                free(lbuf);
            }
            if (code_view != stack_view) {
                free(code_view);
            }
            free(sb.buf);
            return NULL;
        }
        if (rewritten) {
            if (lbuf != stack_buf) {
                free(lbuf);
            }
            if (code_view != stack_view) {
                free(code_view);
            }
            lbuf = rewritten;
            line_len = strlen(lbuf);
            code_view = (line_len < sizeof(stack_view)) ? stack_view : (char *)malloc(line_len + 1);
            if (!code_view) {
                free(lbuf);
                free(sb.buf);
                return NULL;
            }
            bool tmp_block = line_start_in_block;
            build_masked_code_view(lbuf, line_len, code_view, &tmp_block);
        }

        const char *vp = code_view;
        const char *vend = code_view + line_len;
        while (vp < vend && (*vp == ' ' || *vp == '\t')) {
            vp++;
        }
        size_t lead_off = (size_t)(vp - code_view);

        bool ok = true;
        if (!is_frag && vend - vp > 10 && strncmp(vp, "attribute", 9) == 0 &&
            isspace((unsigned char)vp[9])) {
            ok = sb_append_len(&sb, lbuf, lead_off) && sb_append(&sb, "in") &&
                 sb_append_len(&sb, lbuf + lead_off + 9, line_len - (lead_off + 9)) &&
                 sb_append(&sb, "\n");
        } else if (vend - vp > 0) {
            const char *qual = NULL;
            const char *cur = vp;
            if (vend - cur > 5 && strncmp(cur, "flat", 4) == 0 && isspace((unsigned char)cur[4])) {
                qual = "flat";
                cur += 4;
                while (cur < vend && (*cur == ' ' || *cur == '\t')) {
                    cur++;
                }
            } else if (vend - cur > 9 && strncmp(cur, "centroid", 8) == 0 &&
                       isspace((unsigned char)cur[8])) {
                qual = "centroid";
                cur += 8;
                while (cur < vend && (*cur == ' ' || *cur == '\t')) {
                    cur++;
                }
            } else if (vend - cur > 7 && strncmp(cur, "smooth", 6) == 0 &&
                       isspace((unsigned char)cur[6])) {
                qual = "smooth";
                cur += 6;
                while (cur < vend && (*cur == ' ' || *cur == '\t')) {
                    cur++;
                }
            }

            if (vend - cur > 8 && strncmp(cur, "varying", 7) == 0 &&
                isspace((unsigned char)cur[7])) {
                char decl[256];
                size_t dlen = (size_t)(vend - (cur + 7));
                if (dlen >= sizeof(decl)) {
                    dlen = sizeof(decl) - 1;
                }
                memcpy(decl, cur + 7, dlen);
                decl[dlen] = '\0';

                char tok1[64] = {0}, tok2[64] = {0}, tok3[64] = {0};
                char *semi = strchr(decl, ';');
                if (semi) {
                    *semi = '\0';
                }
                int ntok = sscanf(decl, "%63s %63s %63s", tok1, tok2, tok3);
                const char *vprec = NULL;
                const char *vtype = NULL;
                const char *vname = NULL;
                if (ntok == 3 && (strcmp(tok1, "highp") == 0 || strcmp(tok1, "mediump") == 0 ||
                                  strcmp(tok1, "lowp") == 0)) {
                    vprec = tok1;
                    vtype = tok2;
                    vname = tok3;
                } else if (ntok >= 2) {
                    vtype = tok1;
                    vname = tok2;
                }
                const char *interp = qual ? qual : "";
                if (interp[0] == '\0' && vtype &&
                    (strncmp(vtype, "int", 3) == 0 || strncmp(vtype, "uint", 4) == 0 ||
                     strncmp(vtype, "ivec", 4) == 0 || strncmp(vtype, "uvec", 4) == 0)) {
                    interp = "flat";
                }

                bool promote_to_vec4 = false;
                if (vname) {
                    for (uint32_t i = 0; i < promoted_count; i++) {
                        if (strcmp(promoted_varyings[i].name, vname) == 0) {
                            promote_to_vec4 = true;
                            if (promoted_varyings[i].interp[0] != '\0') {
                                interp = promoted_varyings[i].interp;
                            }
                            break;
                        }
                    }
                }

                if (promote_to_vec4 && vname) {
                    if (interp[0] != '\0') {
                        ok = sb_append(&sb, interp) && sb_append(&sb, " ");
                    }
                    ok = ok && sb_append(&sb, is_frag ? "in " : "out ");
                    if (ok && vprec) {
                        ok = sb_append(&sb, vprec) && sb_append(&sb, " ");
                    }
                    ok = ok && sb_append(&sb, "vec4 ") && sb_append(&sb, vname) &&
                         sb_append(&sb, ";\n");
                } else {
                    if (interp[0] != '\0') {
                        ok = sb_append(&sb, interp) && sb_append(&sb, " ");
                    }
                    ok = ok && sb_append(&sb, is_frag ? "in " : "out ");
                    if (ok && vprec) {
                        ok = sb_append(&sb, vprec) && sb_append(&sb, " ");
                    }
                    if (ok && vtype && vname) {
                        ok = sb_append(&sb, vtype) && sb_append(&sb, " ") &&
                             sb_append(&sb, vname) && sb_append(&sb, ";\n");
                    }
                }
            } else if (!line_start_in_block && vp < vend && *vp == '#') {
                const char *q = vp + 1;
                while (q < vend && (*q == ' ' || *q == '\t')) {
                    q++;
                }
                if (vend - q > 7 && strncmp(q, "define", 6) == 0 && isspace((unsigned char)q[6])) {
                    q += 6;
                    while (q < vend && (*q == ' ' || *q == '\t')) {
                        q++;
                    }
                    if (vend - q >= 8 && strncmp(q, "saturate", 8) == 0 &&
                        (vend - q == 8 || !(isalnum((unsigned char)q[8]) || q[8] == '_'))) {
                        ok = sb_append(&sb, "#undef saturate\n");
                    }
                }
                ok = ok && sb_append_len(&sb, lbuf, line_len) && sb_append(&sb, "\n");
            } else {
                ok = append_line_with_varying_swizzles(&sb, lbuf, code_view, line_len,
                                                       active_swizzles, active_swizzle_count);
            }
        } else {
            ok = sb_append_len(&sb, lbuf, line_len) && sb_append(&sb, "\n");
        }

        update_unbraced_control_state(code_view, line_len, &prev_expects_unbraced);

        if (lbuf != stack_buf) {
            free(lbuf);
        }
        if (code_view != stack_view) {
            free(code_view);
        }
        if (!ok) {
            free(sb.buf);
            return NULL;
        }

        if (!eol) {
            break;
        }
        line = eol + 1;
    }

    return sb.buf;
}

/* =========================================================================
 * GLSL ES 3.10 Compatibility Preamble & Stage Assembly
 * ========================================================================= */

static const char WE_GLSL_PREAMBLE_COMMON[] =
    "#version 310 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "precision highp sampler2D;\n"
    "#define float2 vec2\n"
    "#define float3 vec3\n"
    "#define float4 vec4\n"
    "#define float2x2 mat2\n"
    "#define float3x3 mat3\n"
    "#define float4x4 mat4\n"
    "#define float4x3 mat4x3\n"
    "#define float3x4 mat3x4\n"
    "#define int2 ivec2\n"
    "#define int3 ivec3\n"
    "#define int4 ivec4\n"
    "#define uint2 uvec2\n"
    "#define uint3 uvec3\n"
    "#define uint4 uvec4\n"
    "#define bool2 bvec2\n"
    "#define bool3 bvec3\n"
    "#define bool4 bvec4\n"
    "#define half float\n"
    "#define half2 vec2\n"
    "#define half3 vec3\n"
    "#define half4 vec4\n"
    "#define CAST2(x) vec2(x)\n"
    "#define CAST3(x) vec3(x)\n"
    "#define CAST4(x) vec4(x)\n"
    "#define CAST2X2(x) mat2(x)\n"
    "#define CAST3X3(x) mat3(x)\n"
    "#define CAST4X4(x) mat4(x)\n"
    "#define CAST4X3(x) mat4x3(x)\n"
    "#define CAST3X4(x) mat3x4(x)\n"
    "#define frac(x) fract(x)\n"
    "#define fmod(x, y) mod(x, y)\n"
    "#define rsqrt(x) inversesqrt(x)\n"
    "#define lerp(x, y, s) mix(x, y, s)\n"
    "#define mad(a, b, c) (((a) * (b)) + (c))\n"
    "#ifndef saturate\n"
    "#define saturate(x) clamp(x, 0.0, 1.0)\n"
    "#endif\n"
    "#define log10(x) (log(x) * 0.4342944819032518)\n"
    "float rcp(float x) { return 1.0 / x; }\n"
    "vec2 rcp(vec2 x) { return vec2(1.0) / x; }\n"
    "vec3 rcp(vec3 x) { return vec3(1.0) / x; }\n"
    "vec4 rcp(vec4 x) { return vec4(1.0) / x; }\n"
    "void sincos(float x, out float s, out float c) { s = sin(x); c = cos(x); }\n"
    "void sincos(vec2 x, out vec2 s, out vec2 c) { s = sin(x); c = cos(x); }\n"
    "void sincos(vec3 x, out vec3 s, out vec3 c) { s = sin(x); c = cos(x); }\n"
    "void sincos(vec4 x, out vec4 s, out vec4 c) { s = sin(x); c = cos(x); }\n"
    "float we_atan2(float y, float x) {\n"
    "    return (abs(x) < 1e-8 && abs(y) < 1e-8) ? 0.0 : atan(y, x);\n"
    "}\n"
    "vec2 we_atan2(vec2 y, vec2 x) { return vec2(we_atan2(y.x, x.x), we_atan2(y.y, x.y)); }\n"
    "vec2 we_atan2(vec2 y, float x) { return vec2(we_atan2(y.x, x), we_atan2(y.y, x)); }\n"
    "vec2 we_atan2(float y, vec2 x) { return vec2(we_atan2(y, x.x), we_atan2(y, x.y)); }\n"
    "vec3 we_atan2(vec3 y, vec3 x) { return vec3(we_atan2(y.x, x.x), we_atan2(y.y, x.y), "
    "we_atan2(y.z, x.z)); }\n"
    "vec3 we_atan2(vec3 y, float x) { return vec3(we_atan2(y.x, x), we_atan2(y.y, x), "
    "we_atan2(y.z, x)); }\n"
    "vec3 we_atan2(float y, vec3 x) { return vec3(we_atan2(y, x.x), we_atan2(y, x.y), we_atan2(y, "
    "x.z)); }\n"
    "vec4 we_atan2(vec4 y, vec4 x) {\n"
    "    return vec4(we_atan2(y.x, x.x), we_atan2(y.y, x.y), we_atan2(y.z, x.z), we_atan2(y.w, "
    "x.w));\n"
    "}\n"
    "vec4 we_atan2(vec4 y, float x) { return vec4(we_atan2(y.x, x), we_atan2(y.y, x), "
    "we_atan2(y.z, x), we_atan2(y.w, x)); }\n"
    "vec4 we_atan2(float y, vec4 x) { return vec4(we_atan2(y, x.x), we_atan2(y, x.y), we_atan2(y, "
    "x.z), we_atan2(y, x.w)); }\n"
    "#define atan2(y, x) we_atan2(y, x)\n"
    "float we_mul(float a, float b) { return a * b; }\n"
    "vec2 we_mul(vec2 a, float b) { return a * b; }\n"
    "vec2 we_mul(float a, vec2 b) { return a * b; }\n"
    "float we_mul(vec2 a, vec2 b) { return dot(a, b); }\n"
    "vec2 we_mul(vec2 a, mat2 b) { return b * a; }\n"
    "vec2 we_mul(mat2 a, vec2 b) { return a * b; }\n"
    "mat2 we_mul(mat2 a, mat2 b) { return a * b; }\n"
    "vec3 we_mul(vec3 a, float b) { return a * b; }\n"
    "vec3 we_mul(float a, vec3 b) { return a * b; }\n"
    "float we_mul(vec3 a, vec3 b) { return dot(a, b); }\n"
    "vec3 we_mul(vec3 a, mat3 b) { return b * a; }\n"
    "vec3 we_mul(mat3 a, vec3 b) { return a * b; }\n"
    "mat3 we_mul(mat3 a, mat3 b) { return a * b; }\n"
    "vec4 we_mul(vec4 a, float b) { return a * b; }\n"
    "vec4 we_mul(float a, vec4 b) { return a * b; }\n"
    "float we_mul(vec4 a, vec4 b) { return dot(a, b); }\n"
    "vec4 we_mul(vec4 a, mat4 b) { return b * a; }\n"
    "vec4 we_mul(mat4 a, vec4 b) { return a * b; }\n"
    "mat4 we_mul(mat4 a, mat4 b) { return a * b; }\n"
    "vec3 we_mul(vec4 a, mat3x4 b) { return a * b; }\n"
    "vec4 we_mul(mat3x4 a, vec3 b) { return a * b; }\n"
    "vec3 we_mul(vec3 a, mat3x4 b) { return vec4(a, 1.0) * b; }\n"
    "vec3 we_mul(mat3x4 a, vec4 b) { return b * a; }\n"
    "vec4 we_mul(vec3 a, mat4x3 b) { return a * b; }\n"
    "vec3 we_mul(vec4 a, mat4x3 b) { return b * a; }\n"
    "vec3 we_mul(mat4x3 a, vec4 b) { return a * b; }\n"
    "vec3 we_mul(mat4x3 a, vec3 b) { return a * vec4(b, 1.0); }\n"
    "#define mul(a, b) we_mul(a, b)\n";

static const char WE_GLSL_PREAMBLE_SAMPLERS[] =
    "vec4 texSample2D(sampler2D s, vec2 uv) { return texture(s, uv); }\n"
    "vec4 texSample2D(sampler2D s, vec4 uv) { return texture(s, uv.xy); }\n"
    "vec4 tex2D(sampler2D s, vec2 uv) { return texture(s, uv); }\n"
    "vec4 tex2D(sampler2D s, vec4 uv) { return texture(s, uv.xy); }\n"
    "vec4 we_texture2D(sampler2D s, vec2 uv) { return texture(s, uv); }\n"
    "vec4 we_texture2D(sampler2D s, vec4 uv) { return texture(s, uv.xy); }\n"
    "vec4 texSample2DLod(sampler2D s, vec2 uv, float lod) { return textureLod(s, uv, lod); }\n"
    "vec4 texSample2DLod(sampler2D s, vec4 uv, float lod) { return textureLod(s, uv.xy, lod); }\n"
    "vec4 texSample2DLod(sampler2D s, vec4 uv_lod) { return textureLod(s, uv_lod.xy, uv_lod.w); }\n"
    "vec4 we_texture2DLod(sampler2D s, vec2 uv, float lod) { return textureLod(s, uv, lod); }\n"
    "vec4 we_texture2DLod(sampler2D s, vec4 uv, float lod) { return textureLod(s, uv.xy, lod); }\n"
    "vec4 we_texture2DLod(sampler2D s, vec4 uv_lod) { return textureLod(s, uv_lod.xy, uv_lod.w); "
    "}\n"
    "vec4 tex2Dlod(sampler2D s, vec4 uv_lod) { return textureLod(s, uv_lod.xy, uv_lod.w); }\n"
    "vec4 tex2Dlod(sampler2D s, vec2 uv, float lod) { return textureLod(s, uv, lod); }\n"
    "vec4 tex2Dproj(sampler2D s, vec3 uv) { return textureProj(s, uv); }\n"
    "vec4 tex2Dproj(sampler2D s, vec4 uv) { return textureProj(s, uv); }\n"
    "vec4 tex2Dgrad(sampler2D s, vec2 uv, vec2 dx, vec2 dy) { return textureGrad(s, uv, dx, dy); "
    "}\n"
    "vec4 tex2Dgrad(sampler2D s, vec4 uv, vec2 dx, vec2 dy) { return textureGrad(s, uv.xy, dx, "
    "dy); }\n"
    "vec4 texSample2DGrad(sampler2D s, vec2 uv, vec2 dx, vec2 dy) { return textureGrad(s, uv, dx, "
    "dy); }\n"
    "vec4 texSample2DGrad(sampler2D s, vec4 uv, vec2 dx, vec2 dy) { return textureGrad(s, uv.xy, "
    "dx, dy); }\n"
    "vec4 we_texture2DGrad(sampler2D s, vec2 uv, vec2 dx, vec2 dy) { return textureGrad(s, uv, dx, "
    "dy); }\n"
    "vec4 we_texture2DGrad(sampler2D s, vec4 uv, vec2 dx, vec2 dy) { return textureGrad(s, uv.xy, "
    "dx, dy); }\n";

static const char WE_GLSL_PREAMBLE_VERT[] =
    "vec4 texSample2D(sampler2D s, vec2 uv, float bias) { return textureLod(s, uv, bias); }\n"
    "vec4 texSample2D(sampler2D s, vec4 uv, float bias) { return textureLod(s, uv.xy, bias); }\n"
    "vec4 we_texture2D(sampler2D s, vec2 uv, float bias) { return textureLod(s, uv, bias); }\n"
    "vec4 we_texture2D(sampler2D s, vec4 uv, float bias) { return textureLod(s, uv.xy, bias); }\n"
    "vec4 tex2Dbias(sampler2D s, vec4 uv_bias) { return textureLod(s, uv_bias.xy, uv_bias.w); }\n"
    "vec4 tex2Dbias(sampler2D s, vec2 uv, float bias) { return textureLod(s, uv, bias); }\n"
    "#define texture2D we_texture2D\n"
    "#define texture2DLod we_texture2DLod\n"
    "#define texture2DGrad we_texture2DGrad\n";

static const char WE_GLSL_PREAMBLE_FRAG[] =
    "#define ddx(x) dFdx(x)\n"
    "#define ddy(x) dFdy(x)\n"
    "#define ddxy(x) fwidth(x)\n"
    "vec4 texSample2D(sampler2D s, vec2 uv, float bias) { return texture(s, uv, bias); }\n"
    "vec4 texSample2D(sampler2D s, vec4 uv, float bias) { return texture(s, uv.xy, bias); }\n"
    "vec4 we_texture2D(sampler2D s, vec2 uv, float bias) { return texture(s, uv, bias); }\n"
    "vec4 we_texture2D(sampler2D s, vec4 uv, float bias) { return texture(s, uv.xy, bias); }\n"
    "vec4 tex2Dbias(sampler2D s, vec4 uv_bias) { return texture(s, uv_bias.xy, uv_bias.w); }\n"
    "vec4 tex2Dbias(sampler2D s, vec2 uv, float bias) { return texture(s, uv, bias); }\n"
    "#define texture2D we_texture2D\n"
    "#define texture2DLod we_texture2DLod\n"
    "#define texture2DGrad we_texture2DGrad\n"
    "void clip(float x) { if (x < 0.0) discard; }\n"
    "void clip(vec2 x) { if (any(lessThan(x, vec2(0.0)))) discard; }\n"
    "void clip(vec3 x) { if (any(lessThan(x, vec3(0.0)))) discard; }\n"
    "void clip(vec4 x) { if (any(lessThan(x, vec4(0.0)))) discard; }\n"
    "out highp vec4 we_FragColor;\n"
    "#define gl_FragColor we_FragColor\n";

static void apply_combo_rules(we_shader_meta_t *meta, const we_shader_combo_t *override_combos,
                              size_t override_combo_count, uint32_t bound_tex_mask,
                              bool tex0_is_gif)
{
    /* 1. Auto-enable sampler combos when slot is bound or has a default texture */
    for (uint32_t i = 0; i < meta->uniform_count; i++) {
        const we_shader_uniform_meta_t *um = &meta->uniforms[i];
        if (um->combo_name[0] != '\0') {
            bool slot_bound =
                (um->sampler_slot >= 0 && (bound_tex_mask & (1u << um->sampler_slot)) != 0);
            bool has_def_tex = (um->default_tex[0] != '\0');
            if (slot_bound || has_def_tex) {
                meta_set_combo(meta, um->combo_name, 1, true);
            }
        }
    }

    /* 2. Auto-enable SPRITESHEET = 1 if tex_0 is an animated GIF/spritesheet */
    if (tex0_is_gif) {
        meta_set_combo(meta, "SPRITESHEET", 1, true);
    }

    /* 3. Apply explicit material/effect combo overrides */
    if (override_combos) {
        for (size_t i = 0; i < override_combo_count; i++) {
            meta_set_combo(meta, override_combos[i].name, override_combos[i].value, true);
        }
    }
}

static char *assemble_final_stage(const char *transformed_body, bool is_frag,
                                  const we_shader_meta_t *meta)
{
    we_strbuf_t sb;
    if (!sb_init(&sb, strlen(transformed_body) + 2048)) {
        return NULL;
    }

    if (!sb_append(&sb, WE_GLSL_PREAMBLE_COMMON) || !sb_append(&sb, WE_GLSL_PREAMBLE_SAMPLERS) ||
        !sb_append(&sb, is_frag ? WE_GLSL_PREAMBLE_FRAG : WE_GLSL_PREAMBLE_VERT)) {
        free(sb.buf);
        return NULL;
    }

    for (uint32_t i = 0; i < meta->combo_count; i++) {
        char def_line[160];
        snprintf(def_line, sizeof(def_line), "#ifndef %s\n#define %s %d\n#endif\n",
                 meta->combos[i].name, meta->combos[i].name, meta->combos[i].value);
        if (!sb_append(&sb, def_line)) {
            free(sb.buf);
            return NULL;
        }
    }

    if (!sb_append(&sb, transformed_body)) {
        free(sb.buf);
        return NULL;
    }
    return sb.buf;
}

char *we_shader_preprocess_stage(const char *raw_src, bool is_frag, const we_pkg_t *pkg,
                                 const we_shader_combo_t *override_combos,
                                 size_t override_combo_count, uint32_t bound_tex_mask,
                                 bool tex0_is_gif, we_shader_meta_t *inout_meta)
{
    if (!raw_src) {
        return NULL;
    }

    we_shader_meta_t local_meta;
    memset(&local_meta, 0, sizeof(local_meta));
    we_shader_meta_t *meta = inout_meta ? inout_meta : &local_meta;

    we_include_tracker_t tracker;
    memset(&tracker, 0, sizeof(tracker));
    we_strbuf_t expanded;
    if (!sb_init(&expanded, strlen(raw_src) + 1024)) {
        return NULL;
    }
    if (!expand_includes_recursive(raw_src, pkg, &tracker, &expanded, 0)) {
        free(expanded.buf);
        return NULL;
    }

    we_shader_extract_metadata(expanded.buf, meta);
    apply_combo_rules(meta, override_combos, override_combo_count, bound_tex_mask, tex0_is_gif);

    char *transformed = transform_stage_declarations(expanded.buf, is_frag, NULL, 0);
    free(expanded.buf);
    if (!transformed) {
        return NULL;
    }

    char *final_glsl = assemble_final_stage(transformed, is_frag, meta);
    free(transformed);
    return final_glsl;
}

bool we_shader_preprocess_pair(const char *vert_raw, const char *frag_raw, const we_pkg_t *pkg,
                               const we_shader_combo_t *override_combos,
                               size_t override_combo_count, uint32_t bound_tex_mask,
                               bool tex0_is_gif, we_shader_preprocessed_t *out_prep)
{
    if (!out_prep) {
        return false;
    }
    memset(out_prep, 0, sizeof(*out_prep));

    const char *v_src = vert_raw ? vert_raw : WE_BUILTIN_GENERICIMAGE2_VERT;
    const char *f_src = frag_raw ? frag_raw : WE_BUILTIN_GENERICIMAGE2_FRAG;

    we_include_tracker_t v_tr, f_tr;
    memset(&v_tr, 0, sizeof(v_tr));
    memset(&f_tr, 0, sizeof(f_tr));

    we_strbuf_t v_exp, f_exp;
    if (!sb_init(&v_exp, strlen(v_src) + 1024) || !sb_init(&f_exp, strlen(f_src) + 1024)) {
        free(v_exp.buf);
        free(f_exp.buf);
        return false;
    }

    if (!expand_includes_recursive(v_src, pkg, &v_tr, &v_exp, 0) ||
        !expand_includes_recursive(f_src, pkg, &f_tr, &f_exp, 0)) {
        free(v_exp.buf);
        free(f_exp.buf);
        return false;
    }

    /* Extract metadata from both stages before applying combo rules */
    we_shader_extract_metadata(v_exp.buf, &out_prep->meta);
    we_shader_extract_metadata(f_exp.buf, &out_prep->meta);
    apply_combo_rules(&out_prep->meta, override_combos, override_combo_count, bound_tex_mask,
                      tex0_is_gif);

    /* Reconcile mismatched varyings between .vert and .frag */
    we_varying_info_t v_vars[WE_SHADER_MAX_VARYINGS];
    we_varying_info_t f_vars[WE_SHADER_MAX_VARYINGS];
    uint32_t v_var_count = 0, f_var_count = 0;
    collect_varyings(v_exp.buf, &out_prep->meta, v_vars, &v_var_count);
    collect_varyings(f_exp.buf, &out_prep->meta, f_vars, &f_var_count);

    we_varying_info_t promoted_v[WE_SHADER_MAX_VARYINGS];
    we_varying_info_t promoted_f[WE_SHADER_MAX_VARYINGS];
    uint32_t promoted_count = 0;
    for (uint32_t i = 0; i < v_var_count; i++) {
        for (uint32_t j = 0; j < f_var_count; j++) {
            if (strcmp(v_vars[i].name, f_vars[j].name) == 0 && v_vars[i].width != f_vars[j].width) {
                if (promoted_count < WE_SHADER_MAX_VARYINGS) {
                    promoted_v[promoted_count] = v_vars[i];
                    promoted_f[promoted_count] = f_vars[j];
                    promoted_count++;
                }
                break;
            }
        }
    }

    char *v_trans = transform_stage_declarations(v_exp.buf, false, promoted_v, promoted_count);
    char *f_trans = transform_stage_declarations(f_exp.buf, true, promoted_f, promoted_count);
    free(v_exp.buf);
    free(f_exp.buf);

    if (!v_trans || !f_trans) {
        free(v_trans);
        free(f_trans);
        return false;
    }

    out_prep->vert_glsl = assemble_final_stage(v_trans, false, &out_prep->meta);
    out_prep->frag_glsl = assemble_final_stage(f_trans, true, &out_prep->meta);
    free(v_trans);
    free(f_trans);

    if (!out_prep->vert_glsl || !out_prep->frag_glsl) {
        we_shader_preprocessed_free(out_prep);
        return false;
    }
    return true;
}

void we_shader_preprocessed_free(we_shader_preprocessed_t *prep)
{
    if (!prep) {
        return;
    }
    free(prep->vert_glsl);
    free(prep->frag_glsl);
    memset(prep, 0, sizeof(*prep));
}

static GLuint compile_gl_stage(GLenum stage, const char *src, const char *debug_name)
{
    GLuint sh = glCreateShader(stage);
    if (!sh) {
        return 0;
    }
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log_buf[1024] = {0};
        glGetShaderInfoLog(sh, sizeof(log_buf), NULL, log_buf);
        WAYWAL_LOG_WARN("we_shader: failed to compile %s (%s): %s",
                        stage == GL_VERTEX_SHADER ? "VERT" : "FRAG",
                        debug_name ? debug_name : "shader", log_buf);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

typedef struct {
    uint32_t magic;
    uint32_t format;
    uint32_t length;
} we_prog_cache_hdr_t;

#define WE_PROG_CACHE_MAGIC 0x57455042u

static uint64_t fnv1a_step_str(uint64_t h, const char *s)
{
    if (!s) {
        return h;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; ++p) {
        h ^= (uint64_t)(*p);
        h *= 1099511628211ULL;
    }
    h ^= 0xFFULL;
    h *= 1099511628211ULL;
    return h;
}

static bool resolve_we_shader_cache_path(uint64_t hash, char *out_path, size_t cap)
{
    const char *xdg = getenv("XDG_CACHE_HOME");
    char base[4096];
    if (xdg && xdg[0] != '\0') {
        snprintf(base, sizeof(base), "%s", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || home[0] == '\0') {
            return false;
        }
        snprintf(base, sizeof(base), "%s/.cache", home);
        (void)mkdir(base, 0700);
    }
    char dir1[4200];
    char dir2[4300];
    snprintf(dir1, sizeof(dir1), "%s/wwal", base);
    (void)mkdir(dir1, 0700);
    snprintf(dir2, sizeof(dir2), "%s/shaders", dir1);
    (void)mkdir(dir2, 0700);
    int n = snprintf(out_path, cap, "%s/we_%016llx.bin", dir2, (unsigned long long)hash);
    return (n > 0 && (size_t)n < cap);
}

static _Atomic uint32_t s_we_cache_tmp_seq = 0u;

static bool we_read_all_no_eintr(int fd, void *buf, size_t len)
{
    uint8_t *dst = (uint8_t *)buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, dst + got, len - got);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        got += (size_t)n;
    }
    return true;
}

static bool we_write_all_no_eintr(int fd, const void *buf, size_t len)
{
    const uint8_t *src = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, src + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static GLuint try_load_we_cached_program(const char *cache_path)
{
    int fd = open(cache_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    struct stat st;
    we_prog_cache_hdr_t hdr;
    if (fstat(fd, &st) != 0 || st.st_size <= (off_t)sizeof(hdr) ||
        !we_read_all_no_eintr(fd, &hdr, sizeof(hdr)) || hdr.magic != WE_PROG_CACHE_MAGIC ||
        hdr.length == 0 || hdr.length > 16u * 1024u * 1024u ||
        st.st_size != (off_t)(sizeof(hdr) + hdr.length)) {
        close(fd);
        return 0;
    }
    uint8_t *buf = (uint8_t *)malloc(hdr.length);
    if (!buf) {
        close(fd);
        return 0;
    }
    bool ok_rd = we_read_all_no_eintr(fd, buf, hdr.length);
    close(fd);
    if (!ok_rd) {
        free(buf);
        return 0;
    }
    GLuint prog = glCreateProgram();
    if (!prog) {
        free(buf);
        return 0;
    }
    while (glGetError() != GL_NO_ERROR) {
    }
    glProgramBinary(prog, (GLenum)hdr.format, buf, (GLsizei)hdr.length);
    free(buf);
    GLenum err = glGetError();
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (err != GL_NO_ERROR || !linked) {
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static void save_we_cached_program(GLuint prog, const char *cache_path)
{
    GLint bin_len = 0;
    glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &bin_len);
    if (bin_len <= 0 || bin_len > 16 * 1024 * 1024) {
        return;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)bin_len);
    if (!buf) {
        return;
    }
    GLsizei actual_len = 0;
    GLenum bin_fmt = 0;
    glGetProgramBinary(prog, bin_len, &actual_len, &bin_fmt, buf);
    if (actual_len <= 0 || actual_len > bin_len) {
        free(buf);
        return;
    }
    char tmp_path[4480];
    uint32_t seq = atomic_fetch_add_explicit(&s_we_cache_tmp_seq, 1u, memory_order_relaxed);
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d.%u", cache_path, (int)getpid(), seq) >=
        (int)sizeof(tmp_path)) {
        free(buf);
        return;
    }
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
        we_prog_cache_hdr_t hdr = {
            .magic = WE_PROG_CACHE_MAGIC,
            .format = (uint32_t)bin_fmt,
            .length = (uint32_t)actual_len,
        };
        bool ok = we_write_all_no_eintr(fd, &hdr, sizeof(hdr)) &&
                  we_write_all_no_eintr(fd, buf, (size_t)actual_len);
        if (close(fd) != 0) {
            ok = false;
        }
        if (!ok || rename(tmp_path, cache_path) != 0) {
            unlink(tmp_path);
        }
    }
    free(buf);
}

uint32_t we_shader_compile_program(const char *vert_glsl, const char *frag_glsl,
                                   const char *debug_name)
{
    if (!vert_glsl || !frag_glsl) {
        return 0;
    }

    char cache_path[4400] = {0};
    uint64_t h = 14695981039346656037ULL;
    const char *gl_vendor = (const char *)glGetString(GL_VENDOR);
    const char *gl_renderer = (const char *)glGetString(GL_RENDERER);
    const char *gl_version = (const char *)glGetString(GL_VERSION);
    h = fnv1a_step_str(h, vert_glsl);
    h = fnv1a_step_str(h, frag_glsl);
    h = fnv1a_step_str(h, gl_vendor ? gl_vendor : "unknown_vendor");
    h = fnv1a_step_str(h, gl_renderer ? gl_renderer : "unknown_renderer");
    h = fnv1a_step_str(h, gl_version ? gl_version : "unknown_version");
    bool have_cache_path = resolve_we_shader_cache_path(h, cache_path, sizeof(cache_path));
    if (have_cache_path) {
        GLuint cached_prog = try_load_we_cached_program(cache_path);
        if (cached_prog != 0) {
            return (uint32_t)cached_prog;
        }
    }

    GLuint vs = compile_gl_stage(GL_VERTEX_SHADER, vert_glsl, debug_name);
    if (!vs) {
        return 0;
    }
    GLuint fs = compile_gl_stage(GL_FRAGMENT_SHADER, frag_glsl, debug_name);
    if (!fs) {
        glDeleteShader(vs);
        return 0;
    }

    GLuint prog = glCreateProgram();
    if (!prog) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }

    glProgramParameteri(prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);

    /* Bind deterministic Wallpaper Engine vertex attribute locations before linking */
    glBindAttribLocation(prog, 0, "a_Position");
    glBindAttribLocation(prog, 1, "a_TexCoordVec4");
    glBindAttribLocation(prog, 1, "a_TexCoord");
    glBindAttribLocation(prog, 2, "a_Color");
    glBindAttribLocation(prog, 3, "a_Normal");
    glBindAttribLocation(prog, 4, "a_Tangent4");
    glBindAttribLocation(prog, 5, "a_BlendIndices");
    glBindAttribLocation(prog, 6, "a_BlendWeights");

    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log_buf[1024] = {0};
        glGetProgramInfoLog(prog, sizeof(log_buf), NULL, log_buf);
        WAYWAL_LOG_WARN("we_shader: failed to link program (%s): %s",
                        debug_name ? debug_name : "shader", log_buf);
        glDeleteProgram(prog);
        return 0;
    }
    if (have_cache_path) {
        save_we_cached_program(prog, cache_path);
    }
    return (uint32_t)prog;
}
