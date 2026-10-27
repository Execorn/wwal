// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/we_shader.h"

#include "waywal/log.h"

#include <GLES3/gl31.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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

void we_shader_extract_metadata(const char *src, we_shader_meta_t *inout_meta)
{
    if (!src || !inout_meta) {
        return;
    }
    const char *line = src;
    while (*line) {
        const char *eol = strchr(line, '\n');
        size_t line_len = eol ? (size_t)(eol - line) : strlen(line);

        char buf[1024];
        size_t copy_len = line_len < sizeof(buf) - 1 ? line_len : sizeof(buf) - 1;
        memcpy(buf, line, copy_len);
        buf[copy_len] = '\0';

        /* Check for // [COMBO] {"combo":"NAME","default":N} */
        const char *combo_tag = strstr(buf, "[COMBO]");
        if (combo_tag) {
            const char *json = strchr(combo_tag, '{');
            if (json) {
                char cname[64] = {0};
                int32_t cdef = 0;
                if (json_extract_string(json, "combo", cname, sizeof(cname))) {
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

static bool tracker_has_or_add(we_include_tracker_t *tr, const char *inc_name)
{
    char norm[128];
    const char *base = strrchr(inc_name, '/');
    const char *bslash = strrchr(inc_name, '\\');
    if (bslash && (!base || bslash > base)) {
        base = bslash;
    }
    base = base ? (base + 1) : inc_name;

    size_t i = 0;
    for (; base[i] != '\0' && i + 1 < sizeof(norm); i++) {
        norm[i] = (char)tolower((unsigned char)base[i]);
    }
    norm[i] = '\0';

    for (uint32_t j = 0; j < tr->count; j++) {
        if (strcmp(tr->included[j], norm) == 0) {
            return true;
        }
    }
    if (tr->count >= WE_SHADER_MAX_INCLUDES) {
        return true;
    }
    snprintf(tr->included[tr->count++], sizeof(tr->included[0]), "%s", norm);
    return false;
}

static bool expand_includes_recursive(const char *src, const we_pkg_t *pkg,
                                      we_include_tracker_t *tr, we_strbuf_t *out, int depth)
{
    if (!src || depth > 8) {
        return true;
    }

    const char *line = src;
    while (*line) {
        const char *eol = strchr(line, '\n');
        size_t line_len = eol ? (size_t)(eol - line) : strlen(line);

        const char *p = line;
        const char *line_end = line + line_len;
        while (p < line_end && (*p == ' ' || *p == '\t')) {
            p++;
        }

        bool is_include = false;
        bool is_version = false;
        char inc_path[256] = {0};
        if (p < line_end && *p == '#') {
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
                p += 7;
                const char *q1 = memchr(p, '"', (size_t)(line_end - p));
                if (!q1) {
                    q1 = memchr(p, '<', (size_t)(line_end - p));
                }
                if (q1) {
                    char close_ch = (*q1 == '<') ? '>' : '"';
                    const char *q2 = memchr(q1 + 1, close_ch, (size_t)(line_end - (q1 + 1)));
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

        if (is_version) {
            /* Strip embedded #version line so our #version 310 es stays first */
        } else if (is_include) {
            if (!tracker_has_or_add(tr, inc_path)) {
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
    int width; /* 1=float, 2=vec2, 3=vec3, 4=vec4 */
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
    while (*pp < end && (**pp == ' ' || **pp == '\t' || **pp == '\r')) {
        (*pp)++;
    }
}

static int eval_pp_or(const char **pp, const char *end, const we_shader_meta_t *meta,
                      const we_shader_combo_t *local_defs, uint32_t local_count);

static int eval_pp_primary(const char **pp, const char *end, const we_shader_meta_t *meta,
                           const we_shader_combo_t *local_defs, uint32_t local_count)
{
    pp_skip_ws(pp, end);
    if (*pp >= end) {
        return 0;
    }
    if (**pp == '(') {
        (*pp)++;
        int v = eval_pp_or(pp, end, meta, local_defs, local_count);
        pp_skip_ws(pp, end);
        if (*pp < end && **pp == ')') {
            (*pp)++;
        }
        return v;
    }
    if (isdigit((unsigned char)**pp)) {
        char *next = NULL;
        long v = strtol(*pp, &next, 0);
        if (next && next > *pp) {
            *pp = (next <= end) ? next : end;
        } else {
            (*pp)++;
        }
        return (int)v;
    }
    if (isalpha((unsigned char)**pp) || **pp == '_') {
        const char *id_start = *pp;
        while (*pp < end && (isalnum((unsigned char)**pp) || **pp == '_')) {
            (*pp)++;
        }
        int val = 0;
        pp_lookup_macro(id_start, (size_t)(*pp - id_start), meta, local_defs, local_count, &val);
        return val;
    }
    return 0;
}

static int eval_pp_unary(const char **pp, const char *end, const we_shader_meta_t *meta,
                         const we_shader_combo_t *local_defs, uint32_t local_count)
{
    pp_skip_ws(pp, end);
    if (*pp >= end) {
        return 0;
    }
    if (**pp == '!' && (*pp + 1 >= end || (*pp)[1] != '=')) {
        (*pp)++;
        return !eval_pp_unary(pp, end, meta, local_defs, local_count);
    }
    if (**pp == '+') {
        (*pp)++;
        return eval_pp_unary(pp, end, meta, local_defs, local_count);
    }
    if (**pp == '-') {
        (*pp)++;
        return -eval_pp_unary(pp, end, meta, local_defs, local_count);
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
        return pp_lookup_macro(id_start, id_len, meta, local_defs, local_count, NULL) ? 1 : 0;
    }
    return eval_pp_primary(pp, end, meta, local_defs, local_count);
}

static int eval_pp_add(const char **pp, const char *end, const we_shader_meta_t *meta,
                       const we_shader_combo_t *local_defs, uint32_t local_count)
{
    int lhs = eval_pp_unary(pp, end, meta, local_defs, local_count);
    for (;;) {
        pp_skip_ws(pp, end);
        if (*pp < end && **pp == '+') {
            (*pp)++;
            lhs += eval_pp_unary(pp, end, meta, local_defs, local_count);
        } else if (*pp < end && **pp == '-') {
            (*pp)++;
            lhs -= eval_pp_unary(pp, end, meta, local_defs, local_count);
        } else {
            break;
        }
    }
    return lhs;
}

static int eval_pp_rel(const char **pp, const char *end, const we_shader_meta_t *meta,
                       const we_shader_combo_t *local_defs, uint32_t local_count)
{
    int lhs = eval_pp_add(pp, end, meta, local_defs, local_count);
    for (;;) {
        pp_skip_ws(pp, end);
        if (end - *pp >= 2 && (*pp)[0] == '<' && (*pp)[1] == '=') {
            *pp += 2;
            lhs = (lhs <= eval_pp_add(pp, end, meta, local_defs, local_count)) ? 1 : 0;
        } else if (end - *pp >= 2 && (*pp)[0] == '>' && (*pp)[1] == '=') {
            *pp += 2;
            lhs = (lhs >= eval_pp_add(pp, end, meta, local_defs, local_count)) ? 1 : 0;
        } else if (*pp < end && **pp == '<') {
            (*pp)++;
            lhs = (lhs < eval_pp_add(pp, end, meta, local_defs, local_count)) ? 1 : 0;
        } else if (*pp < end && **pp == '>') {
            (*pp)++;
            lhs = (lhs > eval_pp_add(pp, end, meta, local_defs, local_count)) ? 1 : 0;
        } else {
            break;
        }
    }
    return lhs;
}

static int eval_pp_eq(const char **pp, const char *end, const we_shader_meta_t *meta,
                      const we_shader_combo_t *local_defs, uint32_t local_count)
{
    int lhs = eval_pp_rel(pp, end, meta, local_defs, local_count);
    for (;;) {
        pp_skip_ws(pp, end);
        if (end - *pp >= 2 && (*pp)[0] == '=' && (*pp)[1] == '=') {
            *pp += 2;
            lhs = (lhs == eval_pp_rel(pp, end, meta, local_defs, local_count)) ? 1 : 0;
        } else if (end - *pp >= 2 && (*pp)[0] == '!' && (*pp)[1] == '=') {
            *pp += 2;
            lhs = (lhs != eval_pp_rel(pp, end, meta, local_defs, local_count)) ? 1 : 0;
        } else {
            break;
        }
    }
    return lhs;
}

static int eval_pp_and(const char **pp, const char *end, const we_shader_meta_t *meta,
                       const we_shader_combo_t *local_defs, uint32_t local_count)
{
    int lhs = eval_pp_eq(pp, end, meta, local_defs, local_count);
    for (;;) {
        pp_skip_ws(pp, end);
        if (end - *pp >= 2 && (*pp)[0] == '&' && (*pp)[1] == '&') {
            *pp += 2;
            int rhs = eval_pp_eq(pp, end, meta, local_defs, local_count);
            lhs = (lhs && rhs) ? 1 : 0;
        } else {
            break;
        }
    }
    return lhs;
}

static int eval_pp_or(const char **pp, const char *end, const we_shader_meta_t *meta,
                      const we_shader_combo_t *local_defs, uint32_t local_count)
{
    int lhs = eval_pp_and(pp, end, meta, local_defs, local_count);
    for (;;) {
        pp_skip_ws(pp, end);
        if (end - *pp >= 2 && (*pp)[0] == '|' && (*pp)[1] == '|') {
            *pp += 2;
            int rhs = eval_pp_and(pp, end, meta, local_defs, local_count);
            lhs = (lhs || rhs) ? 1 : 0;
        } else {
            break;
        }
    }
    return lhs;
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

    const char *line = src;
    while (*line) {
        const char *eol = strchr(line, '\n');
        size_t line_len = eol ? (size_t)(eol - line) : strlen(line);

        const char *p = line;
        const char *end = line + line_len;
        /* Strip trailing // comments before parsing directives or declarations */
        for (const char *c = line; c + 1 < end; c++) {
            if (c[0] == '/' && c[1] == '/') {
                end = c;
                break;
            }
        }
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
        } else if (cur_active && end - p > 8 && strncmp(p, "varying", 7) == 0 &&
                   isspace((unsigned char)p[7])) {
            char decl[256];
            size_t dlen = (size_t)(end - (p + 7));
            if (dlen >= sizeof(decl)) {
                dlen = sizeof(decl) - 1;
            }
            memcpy(decl, p + 7, dlen);
            decl[dlen] = '\0';
            char *semi = strchr(decl, ';');
            if (semi) {
                *semi = '\0';
            }

            char tok1[64] = {0}, tok2[64] = {0}, tok3[64] = {0};
            int ntok = sscanf(decl, "%63s %63s %63s", tok1, tok2, tok3);
            const char *vtype = NULL;
            const char *vname = NULL;
            if (ntok == 3 && (strcmp(tok1, "highp") == 0 || strcmp(tok1, "mediump") == 0 ||
                              strcmp(tok1, "lowp") == 0)) {
                vtype = tok2;
                vname = tok3;
            } else if (ntok >= 2) {
                vtype = tok1;
                vname = tok2;
            }
            if (vtype && vname && strchr(vname, '[') == NULL) {
                int w = type_to_width(vtype);
                if (w > 0) {
                    bool updated = false;
                    for (uint32_t i = 0; i < *inout_count; i++) {
                        if (strcmp(list[i].name, vname) == 0) {
                            list[i].width = w;
                            updated = true;
                            break;
                        }
                    }
                    if (!updated && *inout_count < WE_SHADER_MAX_VARYINGS) {
                        snprintf(list[*inout_count].name, sizeof(list[0].name), "%s", vname);
                        list[*inout_count].width = w;
                        (*inout_count)++;
                    }
                }
            }
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

static bool append_line_with_varying_swizzles(we_strbuf_t *sb, const char *line, size_t line_len,
                                              const we_varying_swizzle_t *swizzles,
                                              uint32_t swizzle_count)
{
    const char *end = line + line_len;
    const char *code_end = end;
    for (const char *c = line; c + 1 < end; c++) {
        if (c[0] == '/' && c[1] == '/') {
            code_end = c;
            break;
        }
    }

    const char *cur = line;
    const char *seg_start = line;
    while (cur < code_end) {
        if (isalpha((unsigned char)*cur) || *cur == '_') {
            bool after_dot_or_id = (cur > line && (isalnum((unsigned char)cur[-1]) ||
                                                   cur[-1] == '_' || cur[-1] == '.'));
            const char *id_start = cur;
            while (cur < code_end && (isalnum((unsigned char)*cur) || *cur == '_')) {
                cur++;
            }
            size_t id_len = (size_t)(cur - id_start);

            if (!after_dot_or_id) {
                /* Check for user function definition `float saturate(` to avoid macro expansion */
                if (id_len == 8 && strncmp(id_start, "saturate", 8) == 0) {
                    const char *after = cur;
                    while (after < code_end && (*after == ' ' || *after == '\t')) {
                        after++;
                    }
                    if (after < code_end && *after == '(') {
                        /* Check if preceded by a GLSL type token on the same line (never `return`)
                         */
                        const char *prev_end = id_start;
                        while (prev_end > line && (prev_end[-1] == ' ' || prev_end[-1] == '\t')) {
                            prev_end--;
                        }
                        const char *prev_start = prev_end;
                        while (prev_start > line &&
                               (isalnum((unsigned char)prev_start[-1]) || prev_start[-1] == '_')) {
                            prev_start--;
                        }
                        size_t tlen = (size_t)(prev_end - prev_start);
                        bool is_type = (tlen == 5 && strncmp(prev_start, "float", 5) == 0) ||
                                       (tlen == 4 && (strncmp(prev_start, "vec2", 4) == 0 ||
                                                      strncmp(prev_start, "vec3", 4) == 0 ||
                                                      strncmp(prev_start, "vec4", 4) == 0 ||
                                                      strncmp(prev_start, "half", 4) == 0)) ||
                                       (tlen == 5 && (strncmp(prev_start, "half2", 5) == 0 ||
                                                      strncmp(prev_start, "half3", 5) == 0 ||
                                                      strncmp(prev_start, "half4", 5) == 0)) ||
                                       (tlen == 6 && (strncmp(prev_start, "float2", 6) == 0 ||
                                                      strncmp(prev_start, "float3", 6) == 0 ||
                                                      strncmp(prev_start, "float4", 6) == 0));
                        if (is_type) {
                            if (!sb_append_len(sb, seg_start, (size_t)(id_start - seg_start)) ||
                                !sb_append(sb, "we_user_saturate")) {
                                return false;
                            }
                            seg_start = cur;
                            continue;
                        }
                    }
                }

                for (uint32_t i = 0; i < swizzle_count; i++) {
                    if (swizzles[i].swiz[0] != '\0' && strlen(swizzles[i].name) == id_len &&
                        strncmp(swizzles[i].name, id_start, id_len) == 0) {
                        const char *after = cur;
                        while (after < code_end && (*after == ' ' || *after == '\t')) {
                            after++;
                        }
                        /* Only append .<swiz> if the shader didn't already write <vname>.<member>
                         */
                        if (after >= code_end || *after != '.') {
                            if (!sb_append_len(sb, seg_start, (size_t)(cur - seg_start)) ||
                                !sb_append(sb, ".") || !sb_append(sb, swizzles[i].swiz)) {
                                return false;
                            }
                            seg_start = cur;
                        }
                        break;
                    }
                }
            }
        } else {
            cur++;
        }
    }

    return sb_append_len(sb, seg_start, (size_t)(end - seg_start)) && sb_append(sb, "\n");
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

    const char *line = expanded_src;
    while (*line) {
        const char *eol = strchr(line, '\n');
        size_t line_len = eol ? (size_t)(eol - line) : strlen(line);

        const char *p = line;
        const char *end = line + line_len;
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }

        bool ok = true;
        if (!is_frag && end - p > 10 && strncmp(p, "attribute", 9) == 0 &&
            isspace((unsigned char)p[9])) {
            ok = sb_append_len(&sb, line, (size_t)(p - line)) && sb_append(&sb, "in") &&
                 sb_append_len(&sb, p + 9, (size_t)(end - (p + 9))) && sb_append(&sb, "\n");
        } else if (end - p > 8 && strncmp(p, "varying", 7) == 0 && isspace((unsigned char)p[7])) {
            char decl[256];
            size_t dlen = (size_t)(end - (p + 7));
            if (dlen >= sizeof(decl)) {
                dlen = sizeof(decl) - 1;
            }
            memcpy(decl, p + 7, dlen);
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
            int orig_width = vtype ? type_to_width(vtype) : 0;

            bool promote_to_vec4 = false;
            if (vname) {
                for (uint32_t i = 0; i < promoted_count; i++) {
                    if (strcmp(promoted_varyings[i].name, vname) == 0) {
                        promote_to_vec4 = true;
                        if (promoted_varyings[i].width > 0) {
                            orig_width = promoted_varyings[i].width;
                        }
                        break;
                    }
                }
            }

            if (promote_to_vec4 && vname) {
                const char *swiz = "";
                if (orig_width == 1) {
                    swiz = "x";
                } else if (orig_width == 2) {
                    swiz = "xy";
                } else if (orig_width == 3) {
                    swiz = "xyz";
                }

                ok = sb_append(&sb, "#undef ") && sb_append(&sb, vname) && sb_append(&sb, "\n") &&
                     sb_append_len(&sb, line, (size_t)(p - line)) &&
                     sb_append(&sb, is_frag ? "in " : "out ");
                if (ok && vprec) {
                    ok = sb_append(&sb, vprec) && sb_append(&sb, " ");
                }
                ok =
                    ok && sb_append(&sb, "vec4 ") && sb_append(&sb, vname) && sb_append(&sb, ";\n");
                if (ok && swiz[0] != '\0') {
                    ok = sb_append(&sb, "#define ") && sb_append(&sb, vname) &&
                         sb_append(&sb, " ") && sb_append(&sb, vname) && sb_append(&sb, ".") &&
                         sb_append(&sb, swiz) && sb_append(&sb, "\n#undef ") &&
                         sb_append(&sb, vname) && sb_append(&sb, "\n");
                }
            } else {
                ok = sb_append_len(&sb, line, (size_t)(p - line)) &&
                     sb_append(&sb, is_frag ? "in" : "out") &&
                     sb_append_len(&sb, p + 7, (size_t)(end - (p + 7))) && sb_append(&sb, "\n");
            }
        } else if (p < end && *p == '#') {
            /* If user shader defines #define saturate, undefine preamble saturate first */
            const char *q = p + 1;
            while (q < end && (*q == ' ' || *q == '\t')) {
                q++;
            }
            if (end - q > 7 && strncmp(q, "define", 6) == 0 && isspace((unsigned char)q[6])) {
                q += 6;
                while (q < end && (*q == ' ' || *q == '\t')) {
                    q++;
                }
                if (end - q >= 8 && strncmp(q, "saturate", 8) == 0 &&
                    (end - q == 8 || !(isalnum((unsigned char)q[8]) || q[8] == '_'))) {
                    ok = sb_append(&sb, "#undef saturate\n");
                }
            }
            ok = ok && sb_append_len(&sb, line, line_len) && sb_append(&sb, "\n");
        } else {
            ok = append_line_with_varying_swizzles(&sb, line, line_len, active_swizzles,
                                                   active_swizzle_count);
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
    "#ifndef saturate\n"
    "#define saturate(x) clamp(x, 0.0, 1.0)\n"
    "#endif\n"
    "#define log10(x) (log(x) * 0.4342944819032518)\n"
    "void sincos(float x, out float s, out float c) { s = sin(x); c = cos(x); }\n"
    "void sincos(vec2 x, out vec2 s, out vec2 c) { s = sin(x); c = cos(x); }\n"
    "void sincos(vec3 x, out vec3 s, out vec3 c) { s = sin(x); c = cos(x); }\n"
    "void sincos(vec4 x, out vec4 s, out vec4 c) { s = sin(x); c = cos(x); }\n"
    "float we_atan2(float y, float x) {\n"
    "    return (abs(x) < 1e-8 && abs(y) < 1e-8) ? 0.0 : atan(y, x);\n"
    "}\n"
    "vec2 we_atan2(vec2 y, vec2 x) {\n"
    "    return vec2(we_atan2(y.x, x.x), we_atan2(y.y, x.y));\n"
    "}\n"
    "vec3 we_atan2(vec3 y, vec3 x) {\n"
    "    return vec3(we_atan2(y.x, x.x), we_atan2(y.y, x.y), we_atan2(y.z, x.z));\n"
    "}\n"
    "vec4 we_atan2(vec4 y, vec4 x) {\n"
    "    return vec4(we_atan2(y.x, x.x), we_atan2(y.y, x.y), we_atan2(y.z, x.z), we_atan2(y.w, "
    "x.w));\n"
    "}\n"
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
    "vec4 we_mul(vec3 a, mat3x4 b) { return vec4(a, 1.0) * b; }\n"
    "vec3 we_mul(mat3x4 a, vec4 b) { return b * a; }\n"
    "vec4 we_mul(vec3 a, mat4x3 b) { return a * b; }\n"
    "vec3 we_mul(vec4 a, mat4x3 b) { return b * a; }\n"
    "vec3 we_mul(mat4x3 a, vec4 b) { return a * b; }\n"
    "vec3 we_mul(mat4x3 a, vec3 b) { return a * vec4(b, 1.0); }\n"
    "#define mul(a, b) we_mul(a, b)\n"
    "vec4 texSample2D(sampler2D s, vec2 uv) { return texture(s, uv); }\n"
    "vec4 texSample2D(sampler2D s, vec4 uv) { return texture(s, uv.xy); }\n"
    "vec4 texSample2DLod(sampler2D s, vec2 uv, float lod) { return textureLod(s, uv, lod); }\n"
    "vec4 texSample2DLod(sampler2D s, vec4 uv, float lod) { return textureLod(s, uv.xy, lod); }\n"
    "vec4 texture2D(sampler2D s, vec2 uv) { return texture(s, uv); }\n"
    "vec4 texture2D(sampler2D s, vec4 uv) { return texture(s, uv.xy); }\n"
    "vec4 texture2DLod(sampler2D s, vec2 uv, float lod) { return textureLod(s, uv, lod); }\n"
    "vec4 texture2DLod(sampler2D s, vec4 uv, float lod) { return textureLod(s, uv.xy, lod); }\n";

static const char WE_GLSL_PREAMBLE_VERT[] =
    "vec4 texSample2D(sampler2D s, vec2 uv, float bias) { return textureLod(s, uv, bias); }\n"
    "vec4 texSample2D(sampler2D s, vec4 uv, float bias) { return textureLod(s, uv.xy, bias); }\n"
    "vec4 texture2D(sampler2D s, vec2 uv, float bias) { return textureLod(s, uv, bias); }\n"
    "vec4 texture2D(sampler2D s, vec4 uv, float bias) { return textureLod(s, uv.xy, bias); }\n";

static const char WE_GLSL_PREAMBLE_FRAG[] =
    "#define ddx(x) dFdx(x)\n"
    "#define ddy(x) dFdy(x)\n"
    "#define ddxy(x) fwidth(x)\n"
    "vec4 texSample2D(sampler2D s, vec2 uv, float bias) { return texture(s, uv, bias); }\n"
    "vec4 texSample2D(sampler2D s, vec4 uv, float bias) { return texture(s, uv.xy, bias); }\n"
    "vec4 texture2D(sampler2D s, vec2 uv, float bias) { return texture(s, uv, bias); }\n"
    "vec4 texture2D(sampler2D s, vec4 uv, float bias) { return texture(s, uv.xy, bias); }\n"
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

    if (!sb_append(&sb, WE_GLSL_PREAMBLE_COMMON) ||
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

uint32_t we_shader_compile_program(const char *vert_glsl, const char *frag_glsl,
                                   const char *debug_name)
{
    if (!vert_glsl || !frag_glsl) {
        return 0;
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
    return (uint32_t)prog;
}
