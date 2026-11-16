// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file shader_graph.c
 * @brief Visual Node-Based Shader Graph DAG & Multi-Pass GLSL ES 3.10 Compiler
 *
 * Implements the Shader Graph DAG data structures, lifecycle APIs,
 * Tarjan's O(|V| + |E|) Strongly Connected Components (SCC) cycle detector,
 * spatial convolution barrier multi-pass scheduler, Kahn's topological sort
 * with Sethi-Ullman register pressure minimization, Single Static Assignment (SSA)
 * GLSL ES 3.10 code generator, and .wwgraph JSON serialization/deserialization.
 */

#include "waywal/shader_graph.h"

#include <ctype.h>
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline float finite_or_zero(float v) { return isfinite(v) ? v : 0.0f; }

static inline int32_t safe_f32_to_i32(float v)
{
    double d = (double)v;
    if (!isfinite(d))
        return 0;
    if (d >= (double)INT32_MAX)
        return INT32_MAX;
    if (d <= (double)INT32_MIN)
        return INT32_MIN;
    return (int32_t)d;
}

static size_t utf8_clamp_boundary(const char *s, size_t len)
{
    if (!s || len == 0u) {
        return 0u;
    }
    unsigned char last = (unsigned char)s[len - 1u];
    if (last < 0x80u) {
        return len;
    }
    if ((last & 0xC0u) == 0xC0u) {
        return len - 1u;
    }
    size_t back = 1u;
    while (back < len && back < 4u && ((unsigned char)s[len - 1u - back] & 0xC0u) == 0x80u) {
        back++;
    }
    if (back >= len || back >= 4u) {
        return len;
    }
    size_t start = len - 1u - back;
    unsigned char lead = (unsigned char)s[start];
    size_t expected = 0u;
    if ((lead & 0xE0u) == 0xC0u) {
        expected = 2u;
    } else if ((lead & 0xF0u) == 0xE0u) {
        expected = 3u;
    } else if ((lead & 0xF8u) == 0xF0u) {
        expected = 4u;
    } else {
        return len;
    }
    return (len - start < expected) ? start : len;
}

static void safe_utf8_copy(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0u) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t slen = strlen(src);
    if (slen < cap) {
        memcpy(dst, src, slen + 1u);
        return;
    }
    size_t copy_len = utf8_clamp_boundary(src, cap - 1u);
    memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
}

/* ========================================================================= */
/* Dynamic String Buffer Helper for Codegen & Serialization                  */
/* ========================================================================= */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool oom;
} str_buf_t;

static bool str_buf_init(str_buf_t *sb, size_t initial_cap)
{
    if (!sb)
        return false;
    memset(sb, 0, sizeof(*sb));
    size_t cap = (initial_cap > 256u) ? initial_cap : 256u;
    sb->data = (char *)malloc(cap);
    if (!sb->data) {
        sb->oom = true;
        return false;
    }
    sb->cap = cap;
    sb->data[0] = '\0';
    return true;
}

static inline void str_buf_free(str_buf_t *sb)
{
    if (!sb)
        return;
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    sb->oom = false;
}

static bool str_buf_ensure(str_buf_t *sb, size_t extra)
{
    if (!sb || sb->oom || !sb->data)
        return false;
    if (extra > SIZE_MAX - sb->len - 1u) {
        sb->oom = true;
        return false;
    }
    size_t needed = sb->len + extra + 1u;
    if (needed > sb->cap) {
        size_t new_cap = (sb->cap <= SIZE_MAX / 2u) ? (sb->cap * 2u) : SIZE_MAX;
        if (new_cap < needed) {
            new_cap = (needed <= SIZE_MAX - 1024u) ? (needed + 1024u) : needed;
        }
        char *new_data = (char *)realloc(sb->data, new_cap);
        if (!new_data) {
            sb->oom = true;
            return false;
        }
        sb->data = new_data;
        sb->cap = new_cap;
    }
    return true;
}

static void str_buf_append(str_buf_t *sb, const char *str)
{
    if (!str || !sb || sb->oom || !sb->data)
        return;
    size_t slen = strlen(str);
    if (str_buf_ensure(sb, slen)) {
        memcpy(sb->data + sb->len, str, slen);
        sb->len += slen;
        sb->data[sb->len] = '\0';
    }
}

static void str_buf_append_fmt(str_buf_t *sb, const char *fmt, ...)
{
    if (!sb || sb->oom || !sb->data)
        return;
    size_t avail = (sb->cap > sb->len) ? (sb->cap - sb->len) : 0;

    va_list args;
    va_start(args, fmt);
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(sb->data + sb->len, avail, fmt, args_copy);
    va_end(args_copy);

    if (needed < 0) {
        sb->oom = true;
        if (sb->data && sb->len < sb->cap)
            sb->data[sb->len] = '\0';
    } else if ((size_t)needed < avail) {
        sb->len += (size_t)needed;
    } else {
        if (sb->len < sb->cap)
            sb->data[sb->len] = '\0';
        if (str_buf_ensure(sb, (size_t)needed)) {
            vsnprintf(sb->data + sb->len, (size_t)needed + 1u, fmt, args);
            sb->len += (size_t)needed;
        }
    }
    va_end(args);
}

/* Branchless 8-byte detector for bytes < 0x20, '"' (0x22), or '\\' (0x5c). */
static inline bool swar_has_json_special(uint64_t x)
{
    const uint64_t ones = 0x0101010101010101ULL;
    const uint64_t highs = 0x8080808080808080ULL;
    uint64_t lt_20 = (x - 0x2020202020202020ULL) & ~x & highs;
    uint64_t xq = x ^ 0x2222222222222222ULL;
    uint64_t eq_q = (xq - ones) & ~xq & highs;
    uint64_t xb = x ^ 0x5c5c5c5c5c5c5c5cULL;
    uint64_t eq_b = (xb - ones) & ~xb & highs;
    return (lt_20 | eq_q | eq_b) != 0ULL;
}

static void str_buf_append_json_escaped(str_buf_t *sb, const char *str)
{
    if (!sb || sb->oom || !sb->data)
        return;
    const char *s = str ? str : "";
    size_t slen = strlen(s);
    if (slen > (SIZE_MAX - 4u) / 6u) {
        sb->oom = true;
        return;
    }
    size_t worst = slen * 6u + 3u;
    if (!str_buf_ensure(sb, worst))
        return;

    char *dst = sb->data + sb->len;
    *dst++ = '"';
    const unsigned char *p = (const unsigned char *)s;
    const unsigned char *s_end = p + slen;

    while ((size_t)(s_end - p) >= 8u) {
        uint64_t word;
        memcpy(&word, p, sizeof(word));
        if (swar_has_json_special(word))
            break;
        memcpy(dst, p, 8u);
        dst += 8u;
        p += 8u;
    }

    for (; p < s_end; ++p) {
        if ((size_t)(s_end - p) >= 8u) {
            uint64_t word;
            memcpy(&word, p, sizeof(word));
            if (!swar_has_json_special(word)) {
                memcpy(dst, p, 8u);
                dst += 8u;
                p += 7u;
                continue;
            }
        }
        unsigned char c = *p;
        switch (c) {
        case '"':
            *dst++ = '\\';
            *dst++ = '"';
            break;
        case '\\':
            *dst++ = '\\';
            *dst++ = '\\';
            break;
        case '\b':
            *dst++ = '\\';
            *dst++ = 'b';
            break;
        case '\f':
            *dst++ = '\\';
            *dst++ = 'f';
            break;
        case '\n':
            *dst++ = '\\';
            *dst++ = 'n';
            break;
        case '\r':
            *dst++ = '\\';
            *dst++ = 'r';
            break;
        case '\t':
            *dst++ = '\\';
            *dst++ = 't';
            break;
        default:
            if (c < 0x20) {
                static const char hex[] = "0123456789abcdef";
                *dst++ = '\\';
                *dst++ = 'u';
                *dst++ = '0';
                *dst++ = '0';
                *dst++ = hex[(c >> 4) & 0x0F];
                *dst++ = hex[c & 0x0F];
            } else {
                *dst++ = (char)c;
            }
            break;
        }
    }
    *dst++ = '"';
    *dst = '\0';
    sb->len = (size_t)(dst - sb->data);
}

/* ========================================================================= */
/* Node Pin Configuration by Opcode                                          */
/* ========================================================================= */

static void set_pin(waywal_graph_pin_t *pin, const char *name, const char *label, uint32_t pin_type,
                    uint32_t is_output, float def0, float def1, float def2, float def3)
{
    memset(pin, 0, sizeof(*pin));
    if (name) {
        safe_utf8_copy(pin->name, sizeof(pin->name), name);
    }
    if (label) {
        safe_utf8_copy(pin->label, sizeof(pin->label), label);
    }
    pin->pin_type = pin_type;
    pin->is_output = is_output;
    pin->default_val[0] = def0;
    pin->default_val[1] = def1;
    pin->default_val[2] = def2;
    pin->default_val[3] = def3;
    pin->flags = 0;
    pin->reserved = 0;
}

static void node_init_default_pins(waywal_graph_node_t *node)
{
    node->input_pin_count = 0;
    node->output_pin_count = 0;
    memset(node->inputs, 0, sizeof(node->inputs));
    memset(node->outputs, 0, sizeof(node->outputs));

    switch (node->opcode) {
    /* Input Sources (0..9) */
    case WAYWAL_NODE_IN_UV:
        set_pin(&node->outputs[0], "uv", "UV Coordinates", WAYWAL_PIN_VEC2, 1, 0, 0, 0, 0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_IN_TIME:
        set_pin(&node->outputs[0], "time", "Time (seconds)", WAYWAL_PIN_FLOAT, 1, 0, 0, 0, 0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_IN_RESOLUTION:
        set_pin(&node->outputs[0], "resolution", "Resolution (px)", WAYWAL_PIN_VEC2, 1, 1920, 1080,
                0, 0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_IN_POINTER:
        set_pin(&node->outputs[0], "pointer", "Pointer Pos (px)", WAYWAL_PIN_VEC2, 1, 0, 0, 0, 0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_IN_AUDIO_FFT:
        set_pin(&node->inputs[0], "band", "Band Index (0..3)", WAYWAL_PIN_INT, 0, 0, 0, 0, 0);
        node->input_pin_count = 1;
        set_pin(&node->outputs[0], "spectrum", "Audio Spectrum", WAYWAL_PIN_AUDIO_SPECTRUM, 1, 0, 0,
                0, 0);
        set_pin(&node->outputs[1], "level", "Band Amplitude", WAYWAL_PIN_FLOAT, 1, 0, 0, 0, 0);
        node->output_pin_count = 2;
        break;

    case WAYWAL_NODE_IN_TEXTURE2D:
        set_pin(&node->inputs[0], "uv", "UV Coordinates", WAYWAL_PIN_VEC2, 0, 0, 0, 0, 0);
        node->input_pin_count = 1;
        set_pin(&node->outputs[0], "color", "RGBA Color", WAYWAL_PIN_VEC4, 1, 1, 1, 1, 1);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_IN_PASS_BUFFER: /* Opcode 6 / WAYWAL_NODE_BUFFER_PASS */
        set_pin(&node->inputs[0], "uv", "UV Coordinates", WAYWAL_PIN_VEC2, 0, 0, 0, 0, 0);
        node->input_pin_count = 1;
        set_pin(&node->outputs[0], "color", "Previous Frame Color", WAYWAL_PIN_VEC4, 1, 0, 0, 0, 1);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_IN_DEPTH_MAP:
        set_pin(&node->inputs[0], "uv", "UV Coordinates", WAYWAL_PIN_VEC2, 0, 0, 0, 0, 0);
        node->input_pin_count = 1;
        set_pin(&node->outputs[0], "depth", "Depth Value", WAYWAL_PIN_FLOAT, 1, 1, 0, 0, 0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_IN_USER_PROP:
        set_pin(&node->outputs[0], "val", "User Property Value", WAYWAL_PIN_VEC4, 1,
                node->const_val[0], node->const_val[1], node->const_val[2], node->const_val[3]);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_IN_CONSTANT:
        set_pin(&node->outputs[0], "val", "Constant Value", WAYWAL_PIN_VEC4, 1, node->const_val[0],
                node->const_val[1], node->const_val[2], node->const_val[3]);
        node->output_pin_count = 1;
        break;

    /* Math & Vector Operators (10..29) */
    case WAYWAL_NODE_MATH_ADD:
    case WAYWAL_NODE_MATH_SUB:
    case WAYWAL_NODE_MATH_MUL:
    case WAYWAL_NODE_MATH_DIV:
        set_pin(&node->inputs[0], "a", "Operand A", WAYWAL_PIN_VEC4, 0, 0, 0, 0, 0);
        set_pin(&node->inputs[1], "b", "Operand B", WAYWAL_PIN_VEC4, 0, 1, 1, 1, 1);
        node->input_pin_count = 2;
        set_pin(&node->outputs[0], "result", "Result", WAYWAL_PIN_VEC4, 1, 0, 0, 0, 0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_MATH_FMA:
        set_pin(&node->inputs[0], "a", "Multiplier A", WAYWAL_PIN_VEC4, 0, 1, 1, 1, 1);
        set_pin(&node->inputs[1], "b", "Multiplier B", WAYWAL_PIN_VEC4, 0, 1, 1, 1, 1);
        set_pin(&node->inputs[2], "c", "Addend C", WAYWAL_PIN_VEC4, 0, 0, 0, 0, 0);
        node->input_pin_count = 3;
        set_pin(&node->outputs[0], "result", "Result", WAYWAL_PIN_VEC4, 1, 0, 0, 0, 0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_MATH_MIX:
        set_pin(&node->inputs[0], "a", "Value A", WAYWAL_PIN_VEC4, 0, 0, 0, 0, 0);
        set_pin(&node->inputs[1], "b", "Value B", WAYWAL_PIN_VEC4, 0, 1, 1, 1, 1);
        set_pin(&node->inputs[2], "t", "Weight (0..1)", WAYWAL_PIN_FLOAT, 0, 0.5f, 0, 0, 0);
        node->input_pin_count = 3;
        set_pin(&node->outputs[0], "result", "Blended Result", WAYWAL_PIN_VEC4, 1, 0, 0, 0, 0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_MATH_SMOOTHSTEP:
        set_pin(&node->inputs[0], "edge0", "Lower Edge", WAYWAL_PIN_FLOAT, 0, 0, 0, 0, 0);
        set_pin(&node->inputs[1], "edge1", "Upper Edge", WAYWAL_PIN_FLOAT, 0, 1, 0, 0, 0);
        set_pin(&node->inputs[2], "x", "Value X", WAYWAL_PIN_FLOAT, 0, 0.5f, 0, 0, 0);
        node->input_pin_count = 3;
        set_pin(&node->outputs[0], "result", "Hermite Result", WAYWAL_PIN_FLOAT, 1, 0, 0, 0, 0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_MATH_CLAMP:
        set_pin(&node->inputs[0], "x", "Input X", WAYWAL_PIN_VEC4, 0, 0.5f, 0, 0, 0);
        set_pin(&node->inputs[1], "min", "Min Bound", WAYWAL_PIN_VEC4, 0, 0, 0, 0, 0);
        set_pin(&node->inputs[2], "max", "Max Bound", WAYWAL_PIN_VEC4, 0, 1, 1, 1, 1);
        node->input_pin_count = 3;
        set_pin(&node->outputs[0], "result", "Clamped Result", WAYWAL_PIN_VEC4, 1, 0, 0, 0, 0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_MATH_SIN:
    case WAYWAL_NODE_MATH_COS:
    case WAYWAL_NODE_MATH_NORMALIZE:
    case WAYWAL_NODE_MATH_FRACT:
    case WAYWAL_NODE_MATH_FLOOR:
    case WAYWAL_NODE_MATH_ABS:
        set_pin(&node->inputs[0], "in", "Input", WAYWAL_PIN_VEC4, 0, 0, 0, 0, 0);
        node->input_pin_count = 1;
        set_pin(&node->outputs[0], "out", "Output", WAYWAL_PIN_VEC4, 1, 0, 0, 0, 0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_MATH_LENGTH:
        set_pin(&node->inputs[0], "v", "Vector", WAYWAL_PIN_VEC4, 0, 0, 0, 0, 0);
        node->input_pin_count = 1;
        set_pin(&node->outputs[0], "length", "Scalar Length", WAYWAL_PIN_FLOAT, 1, 0, 0, 0, 0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_MATH_POW:
        set_pin(&node->inputs[0], "x", "Base", WAYWAL_PIN_VEC4, 0, 1, 1, 1, 1);
        set_pin(&node->inputs[1], "y", "Exponent", WAYWAL_PIN_FLOAT, 0, 2.0f, 0, 0, 0);
        node->input_pin_count = 2;
        set_pin(&node->outputs[0], "result", "Result", WAYWAL_PIN_VEC4, 1, 0, 0, 0, 0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_MATH_DOT:
        set_pin(&node->inputs[0], "a", "Vector A", WAYWAL_PIN_VEC4, 0, 0, 0, 0, 0);
        set_pin(&node->inputs[1], "b", "Vector B", WAYWAL_PIN_VEC4, 0, 0, 0, 0, 0);
        node->input_pin_count = 2;
        set_pin(&node->outputs[0], "dot", "Scalar Dot Product", WAYWAL_PIN_FLOAT, 1, 0, 0, 0, 0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_MATH_CROSS:
        set_pin(&node->inputs[0], "a", "Vector A (vec3)", WAYWAL_PIN_VEC3, 0, 1, 0, 0, 0);
        set_pin(&node->inputs[1], "b", "Vector B (vec3)", WAYWAL_PIN_VEC3, 0, 0, 1, 0, 0);
        node->input_pin_count = 2;
        set_pin(&node->outputs[0], "cross", "Cross Product (vec3)", WAYWAL_PIN_VEC3, 1, 0, 0, 1, 0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_MATH_DISTANCE:
        set_pin(&node->inputs[0], "a", "Point A", WAYWAL_PIN_VEC2, 0, 0, 0, 0, 0);
        set_pin(&node->inputs[1], "b", "Point B", WAYWAL_PIN_VEC2, 0, 1, 1, 0, 0);
        node->input_pin_count = 2;
        set_pin(&node->outputs[0], "distance", "Euclidean Distance", WAYWAL_PIN_FLOAT, 1, 0, 0, 0,
                0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_VEC_SWIZZLE_COMBINE:
        set_pin(&node->inputs[0], "x", "X Component", WAYWAL_PIN_FLOAT, 0, 0, 0, 0, 0);
        set_pin(&node->inputs[1], "y", "Y Component", WAYWAL_PIN_FLOAT, 0, 0, 0, 0, 0);
        set_pin(&node->inputs[2], "z", "Z Component", WAYWAL_PIN_FLOAT, 0, 0, 0, 0, 0);
        set_pin(&node->inputs[3], "w", "W Component", WAYWAL_PIN_FLOAT, 0, 1, 0, 0, 0);
        node->input_pin_count = 4;
        set_pin(&node->outputs[0], "vec2", "XY Vector (vec2)", WAYWAL_PIN_VEC2, 1, 0, 0, 0, 0);
        set_pin(&node->outputs[1], "vec3", "XYZ Vector (vec3)", WAYWAL_PIN_VEC3, 1, 0, 0, 0, 0);
        set_pin(&node->outputs[2], "vec4", "XYZW Vector (vec4)", WAYWAL_PIN_VEC4, 1, 0, 0, 0, 0);
        node->output_pin_count = 3;
        break;

    /* Procedural Noise, UV & SDF Primitives (30..39) */
    case WAYWAL_NODE_NOISE_SIMPLEX2D:
        set_pin(&node->inputs[0], "uv", "UV Coordinates", WAYWAL_PIN_VEC2, 0, 0, 0, 0, 0);
        set_pin(&node->inputs[1], "scale", "Frequency Scale", WAYWAL_PIN_FLOAT, 0, 5.0f, 0, 0, 0);
        node->input_pin_count = 2;
        set_pin(&node->outputs[0], "noise", "Simplex Noise (-1..1)", WAYWAL_PIN_FLOAT, 1, 0, 0, 0,
                0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_NOISE_CURL2D:
        set_pin(&node->inputs[0], "uv", "UV Coordinates", WAYWAL_PIN_VEC2, 0, 0, 0, 0, 0);
        set_pin(&node->inputs[1], "time", "Time / Phase", WAYWAL_PIN_FLOAT, 0, 0, 0, 0, 0);
        set_pin(&node->inputs[2], "scale", "Frequency Scale", WAYWAL_PIN_FLOAT, 0,
                (node->const_val[0] != 0.0f) ? node->const_val[0] : 3.5f, 0, 0, 0);
        node->input_pin_count = 3;
        set_pin(&node->outputs[0], "velocity", "Curl Velocity (vec2)", WAYWAL_PIN_VEC2, 1, 0, 0, 0,
                0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_NOISE_VORONOI2D:
        set_pin(&node->inputs[0], "uv", "UV Coordinates", WAYWAL_PIN_VEC2, 0, 0, 0, 0, 0);
        set_pin(&node->inputs[1], "scale", "Cell Density", WAYWAL_PIN_FLOAT, 0, 8.0f, 0, 0, 0);
        node->input_pin_count = 2;
        set_pin(&node->outputs[0], "dist", "Distance to Border", WAYWAL_PIN_FLOAT, 1, 0, 0, 0, 0);
        set_pin(&node->outputs[1], "cell_id", "Cell Seed (vec2)", WAYWAL_PIN_VEC2, 1, 0, 0, 0, 0);
        node->output_pin_count = 2;
        break;

    case WAYWAL_NODE_NOISE_FBM2D:
        set_pin(&node->inputs[0], "uv", "UV Coordinates", WAYWAL_PIN_VEC2, 0, 0, 0, 0, 0);
        set_pin(&node->inputs[1], "octaves", "Octave Count", WAYWAL_PIN_INT, 0, 4, 0, 0, 0);
        set_pin(&node->inputs[2], "lacunarity", "Lacunarity", WAYWAL_PIN_FLOAT, 0, 2.0f, 0, 0, 0);
        node->input_pin_count = 3;
        set_pin(&node->outputs[0], "noise", "FBM Noise (0..1)", WAYWAL_PIN_FLOAT, 1, 0, 0, 0, 0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_SDF_CIRCLE:
        set_pin(&node->inputs[0], "uv", "Centered UV (vec2)", WAYWAL_PIN_VEC2, 0, 0, 0, 0, 0);
        set_pin(&node->inputs[1], "radius", "Radius", WAYWAL_PIN_FLOAT, 0, 0.25f, 0, 0, 0);
        node->input_pin_count = 2;
        set_pin(&node->outputs[0], "dist", "Signed Distance", WAYWAL_PIN_FLOAT, 1, 0, 0, 0, 0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_SDF_BOX:
        set_pin(&node->inputs[0], "uv", "Centered UV (vec2)", WAYWAL_PIN_VEC2, 0, 0, 0, 0, 0);
        set_pin(&node->inputs[1], "size", "Half Size (vec2)", WAYWAL_PIN_VEC2, 0, 0.25f, 0.25f, 0,
                0);
        node->input_pin_count = 2;
        set_pin(&node->outputs[0], "dist", "Signed Distance", WAYWAL_PIN_FLOAT, 1, 0, 0, 0, 0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_SDF_RING:
        set_pin(&node->inputs[0], "uv", "Centered UV (vec2)", WAYWAL_PIN_VEC2, 0, 0, 0, 0, 0);
        set_pin(&node->inputs[1], "radius", "Major Radius", WAYWAL_PIN_FLOAT, 0, 0.3f, 0, 0, 0);
        set_pin(&node->inputs[2], "thickness", "Half Thickness", WAYWAL_PIN_FLOAT, 0, 0.05f, 0, 0,
                0);
        node->input_pin_count = 3;
        set_pin(&node->outputs[0], "dist", "Signed Distance", WAYWAL_PIN_FLOAT, 1, 0, 0, 0, 0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_UV_POLAR:
        set_pin(&node->inputs[0], "uv", "UV Coordinates", WAYWAL_PIN_VEC2, 0, 0, 0, 0, 0);
        set_pin(&node->inputs[1], "center", "Pole Center (vec2)", WAYWAL_PIN_VEC2, 0, 0.5f, 0.5f, 0,
                0);
        node->input_pin_count = 2;
        set_pin(&node->outputs[0], "polar_uv", "Angle / Radius UV", WAYWAL_PIN_VEC2, 1, 0, 0, 0, 0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_UV_TWIRL:
        set_pin(&node->inputs[0], "uv", "UV Coordinates", WAYWAL_PIN_VEC2, 0, 0, 0, 0, 0);
        set_pin(&node->inputs[1], "center", "Twirl Center (vec2)", WAYWAL_PIN_VEC2, 0, 0.5f, 0.5f,
                0, 0);
        set_pin(&node->inputs[2], "strength", "Vortex Strength", WAYWAL_PIN_FLOAT, 0, 2.0f, 0, 0,
                0);
        set_pin(&node->inputs[3], "radius", "Twirl Radius", WAYWAL_PIN_FLOAT, 0, 0.5f, 0, 0, 0);
        node->input_pin_count = 4;
        set_pin(&node->outputs[0], "warped_uv", "Twirled UV (vec2)", WAYWAL_PIN_VEC2, 1, 0, 0, 0,
                0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_UV_TILE:
        set_pin(&node->inputs[0], "uv", "UV Coordinates", WAYWAL_PIN_VEC2, 0, 0, 0, 0, 0);
        set_pin(&node->inputs[1], "tiling", "UV Repeat (vec2)", WAYWAL_PIN_VEC2, 0, 2.0f, 2.0f, 0,
                0);
        node->input_pin_count = 2;
        set_pin(&node->outputs[0], "tiled_uv", "Tiled UV (vec2)", WAYWAL_PIN_VEC2, 1, 0, 0, 0, 0);
        node->output_pin_count = 1;
        break;

    /* Color Science, Lighting & Effects (40..48) */
    case WAYWAL_NODE_COLOR_OKLCH_TO_SRGB:
        set_pin(&node->inputs[0], "oklch", "OkLCH Color (vec4)", WAYWAL_PIN_VEC4, 0, 0.7f, 0.15f,
                1.2f, 1.0f);
        node->input_pin_count = 1;
        set_pin(&node->outputs[0], "srgb", "sRGB Color (vec4)", WAYWAL_PIN_VEC4, 1, 0, 0, 0, 1.0f);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_COLOR_SRGB_TO_OKLCH:
        set_pin(&node->inputs[0], "srgb", "sRGB Color (vec4)", WAYWAL_PIN_VEC4, 0, 1, 1, 1, 1);
        node->input_pin_count = 1;
        set_pin(&node->outputs[0], "oklch", "OkLCH Color (vec4)", WAYWAL_PIN_VEC4, 1, 0, 0, 0,
                1.0f);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_COLOR_HUE_SHIFT:
        set_pin(&node->inputs[0], "color", "RGBA Color", WAYWAL_PIN_VEC4, 0, 1, 0, 0, 1);
        set_pin(&node->inputs[1], "shift", "Hue Angle (radians)", WAYWAL_PIN_FLOAT, 0, 1.0f, 0, 0,
                0);
        node->input_pin_count = 2;
        set_pin(&node->outputs[0], "color", "Shifted Color", WAYWAL_PIN_VEC4, 1, 0, 0, 0, 1);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_COLOR_CONTRAST_SAT:
        set_pin(&node->inputs[0], "color", "RGBA Color", WAYWAL_PIN_VEC4, 0, 0.5f, 0.5f, 0.5f, 1);
        set_pin(&node->inputs[1], "contrast", "Contrast Multiplier", WAYWAL_PIN_FLOAT, 0, 1.2f, 0,
                0, 0);
        set_pin(&node->inputs[2], "saturation", "Saturation Multiplier", WAYWAL_PIN_FLOAT, 0, 1.3f,
                0, 0, 0);
        node->input_pin_count = 3;
        set_pin(&node->outputs[0], "color", "Graded Color", WAYWAL_PIN_VEC4, 1, 0, 0, 0, 1);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_FILTER_DUAL_KAWASE:
        set_pin(&node->inputs[0], "color", "Input Color (vec4)", WAYWAL_PIN_VEC4, 0, 0, 0, 0, 1);
        set_pin(&node->inputs[1], "uv", "UV Coordinates", WAYWAL_PIN_VEC2, 0, 0, 0, 0, 0);
        set_pin(&node->inputs[2], "offset", "Filter Radius (px)", WAYWAL_PIN_FLOAT, 0,
                (node->const_val[0] != 0.0f) ? node->const_val[0] : 2.5f, 0, 0, 0);
        node->input_pin_count = 3;
        set_pin(&node->outputs[0], "color", "Blurred / Glow Color", WAYWAL_PIN_VEC4, 1, 0, 0, 0, 1);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_FILTER_CHROMATIC_ABERR:
        set_pin(&node->inputs[0], "color", "Input Color (vec4)", WAYWAL_PIN_VEC4, 0, 0, 0, 0, 1);
        set_pin(&node->inputs[1], "uv", "UV Coordinates", WAYWAL_PIN_VEC2, 0, 0, 0, 0, 0);
        set_pin(&node->inputs[2], "intensity", "Dispersion Scale", WAYWAL_PIN_FLOAT, 0,
                (node->const_val[0] != 0.0f) ? node->const_val[0] : 0.015f, 0, 0, 0);
        node->input_pin_count = 3;
        set_pin(&node->outputs[0], "color", "Dispersed Color", WAYWAL_PIN_VEC4, 1, 0, 0, 0, 1);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_LIGHT_NORMAL_FROM_HEIGHT:
        set_pin(&node->inputs[0], "height", "Height Map Value", WAYWAL_PIN_FLOAT, 0, 0.5f, 0, 0, 0);
        set_pin(&node->inputs[1], "uv", "UV Coordinates", WAYWAL_PIN_VEC2, 0, 0, 0, 0, 0);
        set_pin(&node->inputs[2], "strength", "Normal Bump Scale", WAYWAL_PIN_FLOAT, 0,
                (node->const_val[0] != 0.0f) ? node->const_val[0] : 1.5f, 0, 0, 0);
        node->input_pin_count = 3;
        set_pin(&node->outputs[0], "normal", "Surface Normal (vec3)", WAYWAL_PIN_VEC3, 1, 0, 0, 1,
                0);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_LIGHT_BLINN_PHONG:
        set_pin(&node->inputs[0], "normal", "Surface Normal (vec3)", WAYWAL_PIN_VEC3, 0, 0, 0, 1,
                0);
        set_pin(&node->inputs[1], "light_dir", "Light Direction (vec3)", WAYWAL_PIN_VEC3, 0, 0.577f,
                0.577f, 0.577f, 0);
        set_pin(&node->inputs[2], "view_dir", "View Direction (vec3)", WAYWAL_PIN_VEC3, 0, 0, 0, 1,
                0);
        set_pin(&node->inputs[3], "albedo", "Albedo Color (vec4)", WAYWAL_PIN_VEC4, 0, 0.8f, 0.8f,
                0.8f, 1.0f);
        set_pin(&node->inputs[4], "shininess", "Specular Exponent", WAYWAL_PIN_FLOAT, 0, 32.0f, 0,
                0, 0);
        node->input_pin_count = 5;
        set_pin(&node->outputs[0], "color", "Shaded Lit Color", WAYWAL_PIN_VEC4, 1, 0, 0, 0, 1);
        node->output_pin_count = 1;
        break;

    case WAYWAL_NODE_BUILTIN_EFFECT: {
        bool has_cv = (node->const_val[0] != 0.0f || node->const_val[1] != 0.0f ||
                       node->const_val[2] != 0.0f);
        set_pin(&node->inputs[0], "uv", "UV Coordinates", WAYWAL_PIN_VEC2, 0, 0, 0, 0, 0);
        set_pin(&node->inputs[1], "param0", "Effect Parameter 0", WAYWAL_PIN_FLOAT, 0,
                has_cv ? node->const_val[0] : 1.0f, 0, 0, 0);
        set_pin(&node->inputs[2], "param1", "Effect Parameter 1", WAYWAL_PIN_FLOAT, 0,
                has_cv ? node->const_val[1] : 0.03f, 0, 0, 0);
        set_pin(&node->inputs[3], "param2", "Effect Parameter 2", WAYWAL_PIN_FLOAT, 0,
                has_cv ? node->const_val[2] : 12.0f, 0, 0, 0);
        node->input_pin_count = 4;
        set_pin(&node->outputs[0], "color", "Effect Output Color", WAYWAL_PIN_VEC4, 1, 0, 0, 0, 1);
        node->output_pin_count = 1;
        break;
    }

    /* Output Pass Sinks (49..50) */
    case WAYWAL_NODE_OUT_BUFFER_PASS:
        set_pin(&node->inputs[0], "color", "Intermediate RGBA16F", WAYWAL_PIN_VEC4, 0, 0, 0, 0, 1);
        node->input_pin_count = 1;
        node->output_pin_count = 0;
        break;

    case WAYWAL_NODE_OUT_IMAGE_PASS:
        set_pin(&node->inputs[0], "color", "Final Screen Color", WAYWAL_PIN_VEC4, 0, 0, 0, 0, 1);
        node->input_pin_count = 1;
        node->output_pin_count = 0;
        break;

    default:
        break;
    }
}

/* ========================================================================= */
/* Heap Lifecycle APIs                                                       */
/* ========================================================================= */

waywal_shader_graph_t *waywal_shader_graph_create(void)
{
    waywal_shader_graph_t *graph =
        (waywal_shader_graph_t *)calloc(1, sizeof(waywal_shader_graph_t));
    if (!graph) {
        return NULL;
    }
    waywal_shader_graph_init(graph, "Untitled Shader Graph");
    return graph;
}

void waywal_shader_graph_destroy(waywal_shader_graph_t *graph)
{
    if (graph) {
        free(graph);
    }
}

waywal_shader_graph_t *waywal_shader_graph_alloc(void) { return waywal_shader_graph_create(); }

void waywal_shader_graph_free(waywal_shader_graph_t *graph) { waywal_shader_graph_destroy(graph); }

void waywal_shader_graph_init(waywal_shader_graph_t *graph, const char *name)
{
    if (!graph)
        return;
    memset(graph, 0, sizeof(*graph));
    graph->schema_version = WAYWAL_GRAPH_SCHEMA_VERSION;
    graph->flags = WAYWAL_GRAPH_FLAG_AUTO_COMPILE;
    if (name && name[0] != '\0') {
        safe_utf8_copy(graph->graph_name, sizeof(graph->graph_name), name);
    } else {
        safe_utf8_copy(graph->graph_name, sizeof(graph->graph_name), "Untitled Shader Graph");
    }
    safe_utf8_copy(graph->author, sizeof(graph->author), "wwal");
}

/* ========================================================================= */
/* Graph Mutation & Editing APIs                                             */
/* ========================================================================= */

waywal_graph_node_t *waywal_shader_graph_find_node(waywal_shader_graph_t *graph, uint32_t node_id)
{
    if (!graph || node_id == 0)
        return NULL;
    for (uint32_t i = 0; i < graph->node_count; ++i) {
        if (graph->nodes[i].node_id == node_id) {
            return &graph->nodes[i];
        }
    }
    return NULL;
}

const waywal_graph_node_t *waywal_shader_graph_find_node_const(const waywal_shader_graph_t *graph,
                                                               uint32_t node_id)
{
    if (!graph || node_id == 0)
        return NULL;
    for (uint32_t i = 0; i < graph->node_count; ++i) {
        if (graph->nodes[i].node_id == node_id) {
            return &graph->nodes[i];
        }
    }
    return NULL;
}

#define GRAPH_DENSE_ID_CAP 1024

typedef struct {
    uint32_t id;
    int16_t idx;
} sparse_id_slot_t;

static inline int32_t lookup_node_idx_fast(const sparse_id_slot_t sparse_map[512], bool has_sparse,
                                           const int16_t dense_id_to_idx[GRAPH_DENSE_ID_CAP],
                                           uint32_t node_id)
{
    if (node_id == 0u) {
        return -1;
    }
    if (node_id < GRAPH_DENSE_ID_CAP) {
        return (int32_t)dense_id_to_idx[node_id];
    }
    if (!has_sparse) {
        return -1;
    }
    uint32_t slot = (node_id * 2654435761u) & 511u;
    while (sparse_map[slot].id != 0u && sparse_map[slot].id != node_id) {
        slot = (slot + 1u) & 511u;
    }
    return (sparse_map[slot].id == node_id) ? (int32_t)sparse_map[slot].idx : -1;
}

static void sync_connected_pin_flags(waywal_shader_graph_t *graph)
{
    if (!graph)
        return;
    uint32_t ncnt =
        (graph->node_count <= WAYWAL_GRAPH_MAX_NODES) ? graph->node_count : WAYWAL_GRAPH_MAX_NODES;
    uint32_t ecnt =
        (graph->edge_count <= WAYWAL_GRAPH_MAX_EDGES) ? graph->edge_count : WAYWAL_GRAPH_MAX_EDGES;

    int16_t dense_id_to_idx[GRAPH_DENSE_ID_CAP];
    memset(dense_id_to_idx, 0xFF, sizeof(dense_id_to_idx));

    bool has_sparse = false;
    sparse_id_slot_t sparse_map[512];

    for (uint32_t i = 0; i < ncnt; ++i) {
        waywal_graph_node_t *n = &graph->nodes[i];
        uint32_t nid = n->node_id;
        if (nid > 0u && nid < GRAPH_DENSE_ID_CAP) {
            dense_id_to_idx[nid] = (int16_t)i;
        } else if (nid >= GRAPH_DENSE_ID_CAP) {
            if (!has_sparse) {
                memset(sparse_map, 0, sizeof(sparse_map));
                has_sparse = true;
            }
            uint32_t slot = (nid * 2654435761u) & 511u;
            while (sparse_map[slot].id != 0u && sparse_map[slot].id != nid) {
                slot = (slot + 1u) & 511u;
            }
            sparse_map[slot].id = nid;
            sparse_map[slot].idx = (int16_t)i;
        }
        for (uint32_t p = 0; p < n->input_pin_count && p < WAYWAL_GRAPH_MAX_PINS_PER_NODE; ++p)
            n->inputs[p].flags &= ~WAYWAL_PIN_FLAG_CONNECTED;
        for (uint32_t p = 0; p < n->output_pin_count && p < WAYWAL_GRAPH_MAX_PINS_PER_NODE; ++p)
            n->outputs[p].flags &= ~WAYWAL_PIN_FLAG_CONNECTED;
    }

    for (uint32_t i = 0; i < ecnt; ++i) {
        const waywal_graph_edge_t *e = &graph->edges[i];
        int32_t s_idx =
            lookup_node_idx_fast(sparse_map, has_sparse, dense_id_to_idx, e->src_node_id);
        int32_t d_idx =
            lookup_node_idx_fast(sparse_map, has_sparse, dense_id_to_idx, e->dst_node_id);

        if (s_idx >= 0 && (uint32_t)s_idx < ncnt) {
            waywal_graph_node_t *sn = &graph->nodes[s_idx];
            if (e->src_pin_index < sn->output_pin_count &&
                e->src_pin_index < WAYWAL_GRAPH_MAX_PINS_PER_NODE) {
                sn->outputs[e->src_pin_index].flags |= WAYWAL_PIN_FLAG_CONNECTED;
            }
        }
        if (d_idx >= 0 && (uint32_t)d_idx < ncnt) {
            waywal_graph_node_t *dn = &graph->nodes[d_idx];
            if (e->dst_pin_index < dn->input_pin_count &&
                e->dst_pin_index < WAYWAL_GRAPH_MAX_PINS_PER_NODE) {
                dn->inputs[e->dst_pin_index].flags |= WAYWAL_PIN_FLAG_CONNECTED;
            }
        }
    }
}

int32_t waywal_shader_graph_add_node(waywal_shader_graph_t *graph, uint32_t opcode,
                                     const char *title, float x, float y)
{
    if (!graph || graph->node_count >= WAYWAL_GRAPH_MAX_NODES ||
        opcode > WAYWAL_NODE_OUT_IMAGE_PASS) {
        return -1;
    }

    uint32_t max_id = 0;
    for (uint32_t i = 0; i < graph->node_count; ++i) {
        if (graph->nodes[i].node_id > max_id) {
            max_id = graph->nodes[i].node_id;
        }
    }
    if (max_id >= (uint32_t)INT32_MAX)
        return -1;
    uint32_t new_id = max_id + 1;

    waywal_graph_node_t *n = &graph->nodes[graph->node_count];
    memset(n, 0, sizeof(*n));
    n->node_id = new_id;
    n->opcode = opcode;
    if (title && title[0] != '\0') {
        safe_utf8_copy(n->title, sizeof(n->title), title);
    } else {
        snprintf(n->title, sizeof(n->title), "Node %u", new_id);
    }
    n->canvas_pos[0] = x;
    n->canvas_pos[1] = y;
    n->canvas_size[0] = 180.0f;
    n->canvas_size[1] = 120.0f;
    n->const_val[0] = 0.0f;
    n->const_val[1] = 0.0f;
    n->const_val[2] = 0.0f;
    n->const_val[3] = 1.0f;

    if (opcode == WAYWAL_NODE_OUT_IMAGE_PASS) {
        n->target_pass = 4; /* IMAGE_PASS */
    } else if (opcode == WAYWAL_NODE_OUT_BUFFER_PASS) {
        n->target_pass = 0; /* BUFFER_A */
    }

    node_init_default_pins(n);
    graph->node_count++;
    return (int32_t)new_id;
}

bool waywal_shader_graph_remove_node(waywal_shader_graph_t *graph, uint32_t node_id)
{
    if (!graph || node_id == 0)
        return false;

    int32_t node_idx = -1;
    for (uint32_t i = 0; i < graph->node_count; ++i) {
        if (graph->nodes[i].node_id == node_id) {
            node_idx = (int32_t)i;
            break;
        }
    }
    if (node_idx < 0)
        return false;

    /* Remove all edges connected to this node */
    uint32_t edge_w = 0;
    for (uint32_t i = 0; i < graph->edge_count; ++i) {
        if (graph->edges[i].src_node_id == node_id || graph->edges[i].dst_node_id == node_id) {
            continue;
        }
        if (edge_w != i) {
            graph->edges[edge_w] = graph->edges[i];
        }
        edge_w++;
    }
    graph->edge_count = edge_w;

    /* Compact nodes array */
    for (uint32_t i = (uint32_t)node_idx; i + 1 < graph->node_count; ++i) {
        graph->nodes[i] = graph->nodes[i + 1];
    }
    graph->node_count--;
    sync_connected_pin_flags(graph);
    return true;
}

bool waywal_pin_types_compatible(uint32_t src_type, uint32_t dst_type)
{
    /* Exact match is always allowed */
    if (src_type == dst_type) {
        return true;
    }

    /* Sampler2D is non-coercible: cannot connect with any scalar/vector type */
    if (src_type == WAYWAL_PIN_SAMPLER2D || dst_type == WAYWAL_PIN_SAMPLER2D) {
        return false;
    }

    /* Widening Coercion Lattice (Lossless promotion):
     * float -> vec2, vec3, vec4
     * vec2  -> vec3, vec4
     * vec3  -> vec4 (with opaque alpha = 1.0)
     * int   -> float, vec2, vec3, vec4
     * bool  -> int, float
     * audio -> float, vec4 */
    if (src_type == WAYWAL_PIN_FLOAT) {
        return (dst_type == WAYWAL_PIN_VEC2 || dst_type == WAYWAL_PIN_VEC3 ||
                dst_type == WAYWAL_PIN_VEC4);
    }
    if (src_type == WAYWAL_PIN_VEC2) {
        return (dst_type == WAYWAL_PIN_VEC3 || dst_type == WAYWAL_PIN_VEC4 ||
                dst_type == WAYWAL_PIN_FLOAT);
    }
    if (src_type == WAYWAL_PIN_VEC3) {
        return (dst_type == WAYWAL_PIN_VEC4 || dst_type == WAYWAL_PIN_VEC2 ||
                dst_type == WAYWAL_PIN_FLOAT);
    }
    if (src_type == WAYWAL_PIN_VEC4) {
        return (dst_type == WAYWAL_PIN_VEC3 || dst_type == WAYWAL_PIN_VEC2 ||
                dst_type == WAYWAL_PIN_FLOAT);
    }
    if (src_type == WAYWAL_PIN_AUDIO_SPECTRUM) {
        return (dst_type == WAYWAL_PIN_FLOAT || dst_type == WAYWAL_PIN_VEC4);
    }
    if (src_type == WAYWAL_PIN_INT) {
        return (dst_type == WAYWAL_PIN_FLOAT || dst_type == WAYWAL_PIN_VEC2 ||
                dst_type == WAYWAL_PIN_VEC3 || dst_type == WAYWAL_PIN_VEC4 ||
                dst_type == WAYWAL_PIN_BOOL);
    }
    if (src_type == WAYWAL_PIN_BOOL) {
        return (dst_type == WAYWAL_PIN_INT || dst_type == WAYWAL_PIN_FLOAT);
    }

    return false;
}

int32_t waywal_shader_graph_connect(waywal_shader_graph_t *graph, uint32_t src_node_id,
                                    uint32_t src_pin_idx, uint32_t dst_node_id,
                                    uint32_t dst_pin_idx)
{
    if (!graph) {
        return -1;
    }

    waywal_graph_node_t *src_node = waywal_shader_graph_find_node(graph, src_node_id);
    waywal_graph_node_t *dst_node = waywal_shader_graph_find_node(graph, dst_node_id);
    if (!src_node || !dst_node) {
        return -1;
    }
    if (src_pin_idx >= src_node->output_pin_count || dst_pin_idx >= dst_node->input_pin_count) {
        return -1;
    }

    /* Verify pin type compatibility across coercion lattice */
    uint32_t src_type = src_node->outputs[src_pin_idx].pin_type;
    uint32_t dst_type = dst_node->inputs[dst_pin_idx].pin_type;
    if (!waywal_pin_types_compatible(src_type, dst_type)) {
        return -1;
    }

    /* Validate cycle safety BEFORE mutating or disconnecting any existing incoming wire. */
    if (waywal_shader_graph_would_create_cycle(graph, src_node_id, dst_node_id)) {
        return -1;
    }

    bool has_existing_dst = false;
    uint32_t max_eid = 0;
    for (uint32_t i = 0; i < graph->edge_count; ++i) {
        if (graph->edges[i].edge_id > max_eid) {
            max_eid = graph->edges[i].edge_id;
        }
        if (graph->edges[i].dst_node_id == dst_node_id &&
            graph->edges[i].dst_pin_index == dst_pin_idx) {
            has_existing_dst = true;
        }
    }
    if (!has_existing_dst && graph->edge_count >= WAYWAL_GRAPH_MAX_EDGES) {
        return -1;
    }
    if (max_eid >= (uint32_t)INT32_MAX) {
        return -1;
    }
    uint32_t new_eid = max_eid + 1u;

    /* Single-input constraint: remove existing incoming wire to (dst_node_id, dst_pin_idx) */
    if (has_existing_dst) {
        waywal_shader_graph_disconnect_pins(graph, dst_node_id, dst_pin_idx);
    }
    if (graph->edge_count >= WAYWAL_GRAPH_MAX_EDGES) {
        return -1;
    }

    uint32_t edge_idx = graph->edge_count;
    waywal_graph_edge_t *e = &graph->edges[edge_idx];
    memset(e, 0, sizeof(*e));
    e->edge_id = new_eid;
    e->src_node_id = src_node_id;
    e->src_pin_index = src_pin_idx;
    e->dst_node_id = dst_node_id;
    e->dst_pin_index = dst_pin_idx;
    e->swizzle_mask = 0;
    e->flags = 0;
    e->reserved = 0;
    graph->edge_count++;

    dst_node->inputs[dst_pin_idx].flags |= WAYWAL_PIN_FLAG_CONNECTED;
    src_node->outputs[src_pin_idx].flags |= WAYWAL_PIN_FLAG_CONNECTED;
    return (int32_t)new_eid;
}

bool waywal_shader_graph_disconnect(waywal_shader_graph_t *graph, uint32_t edge_id)
{
    if (!graph || edge_id == 0)
        return false;

    int32_t target_idx = -1;
    for (uint32_t i = 0; i < graph->edge_count; ++i) {
        if (graph->edges[i].edge_id == edge_id) {
            target_idx = (int32_t)i;
            break;
        }
    }
    if (target_idx < 0)
        return false;

    for (uint32_t i = (uint32_t)target_idx; i + 1 < graph->edge_count; ++i) {
        graph->edges[i] = graph->edges[i + 1];
    }
    graph->edge_count--;
    sync_connected_pin_flags(graph);
    return true;
}

bool waywal_shader_graph_disconnect_pins(waywal_shader_graph_t *graph, uint32_t dst_node_id,
                                         uint32_t dst_pin_idx)
{
    if (!graph)
        return false;
    uint32_t edge_w = 0;
    bool found = false;
    for (uint32_t i = 0; i < graph->edge_count; ++i) {
        if (graph->edges[i].dst_node_id == dst_node_id &&
            graph->edges[i].dst_pin_index == dst_pin_idx) {
            found = true;
            continue;
        }
        if (edge_w != i) {
            graph->edges[edge_w] = graph->edges[i];
        }
        edge_w++;
    }
    if (found) {
        graph->edge_count = edge_w;
        sync_connected_pin_flags(graph);
    }
    return found;
}

/* ========================================================================= */
/* Compact O(|V| + |E|) Adjacency Index & Tarjan SCC Cycle Detector          */
/* ========================================================================= */

/* Fixed 10 KB stack adjacency table avoids 492 KB waywal_shader_graph_t copies
 * and turns repeated O(|V| * |E|) node/wire scans into O(1) array lookups. */
typedef struct {
    int16_t dense_id_to_idx[GRAPH_DENSE_ID_CAP];
    int16_t edge_src_idx[WAYWAL_GRAPH_MAX_EDGES];
    int16_t edge_dst_idx[WAYWAL_GRAPH_MAX_EDGES];
    int16_t head_out[WAYWAL_GRAPH_MAX_NODES];
    int16_t next_out[WAYWAL_GRAPH_MAX_EDGES];
    int16_t head_in[WAYWAL_GRAPH_MAX_NODES];
    int16_t next_in[WAYWAL_GRAPH_MAX_EDGES];
    int16_t pin_in_edge[WAYWAL_GRAPH_MAX_NODES][WAYWAL_GRAPH_MAX_PINS_PER_NODE];
} graph_adj_t;

static inline int16_t graph_adj_find_idx(const waywal_shader_graph_t *graph, const graph_adj_t *adj,
                                         uint32_t node_id)
{
    if (node_id == 0)
        return -1;
    if (node_id < GRAPH_DENSE_ID_CAP) {
        return adj->dense_id_to_idx[node_id];
    }
    uint32_t ncnt =
        (graph->node_count <= WAYWAL_GRAPH_MAX_NODES) ? graph->node_count : WAYWAL_GRAPH_MAX_NODES;
    for (uint32_t i = 0; i < ncnt; ++i) {
        if (graph->nodes[i].node_id == node_id) {
            return (int16_t)i;
        }
    }
    return -1;
}

static void graph_adj_build(const waywal_shader_graph_t *graph, graph_adj_t *adj)
{
    memset(adj, 0xFF, sizeof(*adj));
    uint32_t ncnt =
        (graph->node_count <= WAYWAL_GRAPH_MAX_NODES) ? graph->node_count : WAYWAL_GRAPH_MAX_NODES;
    uint32_t ecnt =
        (graph->edge_count <= WAYWAL_GRAPH_MAX_EDGES) ? graph->edge_count : WAYWAL_GRAPH_MAX_EDGES;

    for (uint32_t i = 0; i < ncnt; ++i) {
        uint32_t nid = graph->nodes[i].node_id;
        if (nid > 0 && nid < GRAPH_DENSE_ID_CAP) {
            adj->dense_id_to_idx[nid] = (int16_t)i;
        }
    }

    for (uint32_t e = 0; e < ecnt; ++e) {
        int16_t s = graph_adj_find_idx(graph, adj, graph->edges[e].src_node_id);
        int16_t d = graph_adj_find_idx(graph, adj, graph->edges[e].dst_node_id);
        adj->edge_src_idx[e] = s;
        adj->edge_dst_idx[e] = d;
        if (s >= 0 && d >= 0) {
            adj->next_out[e] = adj->head_out[s];
            adj->head_out[s] = (int16_t)e;
            adj->next_in[e] = adj->head_in[d];
            adj->head_in[d] = (int16_t)e;
            uint32_t dp = graph->edges[e].dst_pin_index;
            if (dp < WAYWAL_GRAPH_MAX_PINS_PER_NODE) {
                adj->pin_in_edge[d][dp] = (int16_t)e;
            }
        }
    }
}

typedef struct {
    int32_t index;
    int32_t lowlink;
    bool on_stack;
} tarjan_node_state_t;

typedef struct {
    uint16_t node_idx;
    int16_t next_edge;
} tarjan_frame_t;

static bool has_cycle_with_adj(const waywal_shader_graph_t *graph, const graph_adj_t *adj,
                               uint32_t *out_cycle_node_id)
{
    uint32_t ncnt =
        (graph->node_count <= WAYWAL_GRAPH_MAX_NODES) ? graph->node_count : WAYWAL_GRAPH_MAX_NODES;
    tarjan_node_state_t states[WAYWAL_GRAPH_MAX_NODES];
    uint16_t scc_stack[WAYWAL_GRAPH_MAX_NODES];
    tarjan_frame_t dfs_stack[WAYWAL_GRAPH_MAX_NODES];
    uint32_t scc_top = 0;
    int32_t dfs_clock = 0;

    for (uint32_t i = 0; i < ncnt; ++i) {
        states[i].index = -1;
        states[i].lowlink = -1;
        states[i].on_stack = false;
    }

    for (uint32_t root = 0; root < ncnt; ++root) {
        if (states[root].index != -1) {
            continue;
        }
        uint32_t dfs_top = 0;
        states[root].index = dfs_clock;
        states[root].lowlink = dfs_clock;
        dfs_clock++;
        scc_stack[scc_top++] = (uint16_t)root;
        states[root].on_stack = true;
        dfs_stack[dfs_top++] =
            (tarjan_frame_t){.node_idx = (uint16_t)root, .next_edge = adj->head_out[root]};

        while (dfs_top > 0u) {
            tarjan_frame_t *frame = &dfs_stack[dfs_top - 1u];
            uint32_t u_idx = frame->node_idx;
            int16_t e = frame->next_edge;

            if (e >= 0) {
                frame->next_edge = adj->next_out[e];
                int16_t v_idx_s = adj->edge_dst_idx[e];
                if (v_idx_s < 0) {
                    continue;
                }
                uint32_t v_idx = (uint32_t)v_idx_s;

                /* Self-loops are never valid temporal latches even on IN_PASS_BUFFER. */
                if (v_idx == u_idx) {
                    if (out_cycle_node_id) {
                        *out_cycle_node_id = graph->nodes[u_idx].node_id;
                    }
                    return true;
                }

                /* Temporal feedback latches sample frame (t - 1) and break combinational cycles. */
                if (graph->nodes[u_idx].opcode == WAYWAL_NODE_IN_PASS_BUFFER ||
                    (graph->edges[e].flags & WAYWAL_EDGE_FLAG_FEEDBACK_LATCH)) {
                    continue;
                }

                if (states[v_idx].index == -1) {
                    states[v_idx].index = dfs_clock;
                    states[v_idx].lowlink = dfs_clock;
                    dfs_clock++;
                    scc_stack[scc_top++] = (uint16_t)v_idx;
                    states[v_idx].on_stack = true;
                    dfs_stack[dfs_top++] = (tarjan_frame_t){.node_idx = (uint16_t)v_idx,
                                                            .next_edge = adj->head_out[v_idx]};
                } else if (states[v_idx].on_stack) {
                    if (states[v_idx].index < states[u_idx].lowlink) {
                        states[u_idx].lowlink = states[v_idx].index;
                    }
                }
            } else {
                dfs_top--;
                if (dfs_top > 0u) {
                    uint32_t parent_idx = dfs_stack[dfs_top - 1u].node_idx;
                    if (states[u_idx].lowlink < states[parent_idx].lowlink) {
                        states[parent_idx].lowlink = states[u_idx].lowlink;
                    }
                }
                if (states[u_idx].lowlink == states[u_idx].index) {
                    uint32_t scc_size = 0;
                    uint32_t popped_idx = 0;
                    do {
                        if (scc_top == 0u) {
                            break;
                        }
                        popped_idx = scc_stack[--scc_top];
                        states[popped_idx].on_stack = false;
                        scc_size++;
                    } while (popped_idx != u_idx);

                    if (scc_size > 1u) {
                        if (out_cycle_node_id) {
                            *out_cycle_node_id = graph->nodes[u_idx].node_id;
                        }
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

bool waywal_shader_graph_has_cycle(const waywal_shader_graph_t *graph, uint32_t *out_cycle_node_id)
{
    if (!graph || graph->node_count == 0) {
        return false;
    }
    graph_adj_t adj;
    graph_adj_build(graph, &adj);
    return has_cycle_with_adj(graph, &adj, out_cycle_node_id);
}

bool waywal_shader_graph_would_create_cycle(const waywal_shader_graph_t *graph,
                                            uint32_t src_node_id, uint32_t dst_node_id)
{
    if (!graph || src_node_id == 0u || dst_node_id == 0u || graph->node_count == 0u) {
        return false;
    }
    const waywal_graph_node_t *src_node = waywal_shader_graph_find_node_const(graph, src_node_id);
    const waywal_graph_node_t *dst_node = waywal_shader_graph_find_node_const(graph, dst_node_id);
    if (!src_node || !dst_node) {
        return false;
    }
    if (src_node_id == dst_node_id) {
        return true;
    }
    if (src_node->opcode == WAYWAL_NODE_IN_PASS_BUFFER) {
        return false;
    }

    graph_adj_t adj;
    graph_adj_build(graph, &adj);
    int16_t src_idx = graph_adj_find_idx(graph, &adj, src_node_id);
    int16_t dst_idx = graph_adj_find_idx(graph, &adj, dst_node_id);
    if (src_idx < 0 || dst_idx < 0) {
        return false;
    }

    bool visited[WAYWAL_GRAPH_MAX_NODES] = {false};
    uint16_t stack[WAYWAL_GRAPH_MAX_NODES];
    uint32_t top = 0;
    visited[dst_idx] = true;
    stack[top++] = (uint16_t)dst_idx;

    while (top > 0u) {
        uint16_t u = stack[--top];
        if (graph->nodes[u].opcode == WAYWAL_NODE_IN_PASS_BUFFER) {
            continue;
        }
        for (int16_t e = adj.head_out[u]; e >= 0; e = adj.next_out[e]) {
            if (graph->edges[e].flags & WAYWAL_EDGE_FLAG_FEEDBACK_LATCH) {
                continue;
            }
            int16_t v = adj.edge_dst_idx[e];
            if (v < 0) {
                continue;
            }
            if (v == src_idx) {
                return true;
            }
            if (!visited[v]) {
                visited[v] = true;
                stack[top++] = (uint16_t)v;
            }
        }
    }
    return false;
}

/* ========================================================================= */
/* Spatial Convolution Barrier Detection & Multi-Pass Scheduling             */
/* ========================================================================= */

static bool is_spatial_barrier_op(uint32_t opcode, const char *prop_or_asset_key)
{
    if (opcode == WAYWAL_NODE_FILTER_DUAL_KAWASE || opcode == WAYWAL_NODE_FILTER_CHROMATIC_ABERR ||
        opcode == WAYWAL_NODE_LIGHT_NORMAL_FROM_HEIGHT) {
        return true;
    }
    if (opcode == WAYWAL_NODE_BUILTIN_EFFECT && prop_or_asset_key) {
        if (strstr(prop_or_asset_key, "waterripple") || strstr(prop_or_asset_key, "waterwaves") ||
            strstr(prop_or_asset_key, "bloom") || strstr(prop_or_asset_key, "blur") ||
            strstr(prop_or_asset_key, "godrays") || strstr(prop_or_asset_key, "sharpen_cas")) {
            return true;
        }
    }
    return false;
}

static uint32_t resolve_buffer_pass_index(const waywal_graph_node_t *node)
{
    if (node->prop_or_asset_key[0] != '\0') {
        if (strstr(node->prop_or_asset_key, "BUFFER_B") ||
            strstr(node->prop_or_asset_key, "buffer_b"))
            return 1;
        if (strstr(node->prop_or_asset_key, "BUFFER_C") ||
            strstr(node->prop_or_asset_key, "buffer_c"))
            return 2;
        if (strstr(node->prop_or_asset_key, "BUFFER_D") ||
            strstr(node->prop_or_asset_key, "buffer_d"))
            return 3;
        if (strstr(node->prop_or_asset_key, "BUFFER_A") ||
            strstr(node->prop_or_asset_key, "buffer_a"))
            return 0;
    }
    return (node->target_pass <= 3) ? node->target_pass : 0;
}

static inline bool is_texture_backed_source_op(uint32_t opcode)
{
    return (opcode == WAYWAL_NODE_IN_PASS_BUFFER || opcode == WAYWAL_NODE_IN_TEXTURE2D ||
            opcode == WAYWAL_NODE_IN_DEPTH_MAP);
}

static inline bool edge_requires_barrier_split(const waywal_shader_graph_t *graph, uint32_t src_idx,
                                               uint32_t dst_idx, const waywal_graph_edge_t *edge)
{
    const waywal_graph_node_t *src = &graph->nodes[src_idx];
    const waywal_graph_node_t *dst = &graph->nodes[dst_idx];
    if (dst->flags & WAYWAL_NODE_FLAG_MUTED_BYPASS) {
        return false;
    }
    if (is_texture_backed_source_op(src->opcode)) {
        return false;
    }
    if (!is_spatial_barrier_op(dst->opcode, dst->prop_or_asset_key)) {
        return false;
    }
    if (dst->opcode == WAYWAL_NODE_BUILTIN_EFFECT) {
        /* Pin 0 on BUILTIN_EFFECT is UV; only split if an upstream RGBA producer was coerced into
         * it. */
        uint32_t sp = edge->src_pin_index;
        return (edge->dst_pin_index == 0 && sp < src->output_pin_count &&
                src->outputs[sp].pin_type == WAYWAL_PIN_VEC4);
    }
    return (edge->dst_pin_index == 0);
}

typedef struct {
    uint16_t node_idx;
    uint8_t next_pin;
} sethi_frame_t;

static void compute_sethi_ullman_all(const waywal_shader_graph_t *graph, const graph_adj_t *adj,
                                     uint32_t ncnt, const bool *reachable,
                                     uint32_t weights[WAYWAL_GRAPH_MAX_NODES])
{
    bool visiting[WAYWAL_GRAPH_MAX_NODES] = {false};
    sethi_frame_t stack[WAYWAL_GRAPH_MAX_NODES];

    for (uint32_t root = 0; root < ncnt; ++root) {
        if (!reachable[root] || weights[root] > 0u) {
            continue;
        }
        uint32_t top = 0;
        visiting[root] = true;
        stack[top++] = (sethi_frame_t){.node_idx = (uint16_t)root, .next_pin = 0u};

        while (top > 0u) {
            sethi_frame_t *f = &stack[top - 1u];
            uint32_t u = f->node_idx;
            const waywal_graph_node_t *node = &graph->nodes[u];
            uint32_t pcnt = (node->input_pin_count <= WAYWAL_GRAPH_MAX_PINS_PER_NODE)
                                ? node->input_pin_count
                                : WAYWAL_GRAPH_MAX_PINS_PER_NODE;

            if (f->next_pin < pcnt) {
                uint32_t p = f->next_pin++;
                int16_t e_idx = adj->pin_in_edge[u][p];
                if (e_idx < 0 || (graph->edges[e_idx].flags & WAYWAL_EDGE_FLAG_FEEDBACK_LATCH)) {
                    continue;
                }
                int16_t s_idx = adj->edge_src_idx[e_idx];
                if (s_idx >= 0 && reachable[s_idx] &&
                    graph->nodes[s_idx].opcode != WAYWAL_NODE_IN_PASS_BUFFER &&
                    weights[s_idx] == 0u && !visiting[s_idx]) {
                    visiting[s_idx] = true;
                    stack[top++] = (sethi_frame_t){.node_idx = (uint16_t)s_idx, .next_pin = 0u};
                }
                continue;
            }

            top--;
            visiting[u] = false;

            uint32_t child_weights[WAYWAL_GRAPH_MAX_PINS_PER_NODE];
            uint32_t child_count = 0;
            for (uint32_t p = 0; p < pcnt; ++p) {
                int16_t e_idx = adj->pin_in_edge[u][p];
                if (e_idx < 0) {
                    continue;
                }
                if (graph->edges[e_idx].flags & WAYWAL_EDGE_FLAG_FEEDBACK_LATCH) {
                    child_weights[child_count++] = 1u;
                    continue;
                }
                int16_t s_idx = adj->edge_src_idx[e_idx];
                if (s_idx >= 0 && reachable[s_idx]) {
                    if (graph->nodes[s_idx].opcode == WAYWAL_NODE_IN_PASS_BUFFER) {
                        child_weights[child_count++] = 1u;
                    } else {
                        uint32_t w = weights[s_idx];
                        child_weights[child_count++] = (w > 0u) ? w : 1u;
                    }
                }
            }

            if (child_count == 0u) {
                weights[u] = 1u;
                continue;
            }

            for (uint32_t i = 0; i < child_count; ++i) {
                for (uint32_t j = i + 1u; j < child_count; ++j) {
                    if (child_weights[j] > child_weights[i]) {
                        uint32_t tmp = child_weights[i];
                        child_weights[i] = child_weights[j];
                        child_weights[j] = tmp;
                    }
                }
            }

            uint32_t max_weight = 1u;
            for (uint32_t i = 0; i < child_count; ++i) {
                uint32_t w = child_weights[i] + i;
                if (w > max_weight) {
                    max_weight = w;
                }
            }
            weights[u] = max_weight;
        }
    }
}

static void compute_node_passes_readonly(const waywal_shader_graph_t *graph_in,
                                         const graph_adj_t *adj,
                                         bool reachable[WAYWAL_GRAPH_MAX_NODES],
                                         uint32_t out_target_pass[WAYWAL_GRAPH_MAX_NODES],
                                         uint32_t *out_active_pass_mask)
{
    uint32_t ncnt = (graph_in->node_count <= WAYWAL_GRAPH_MAX_NODES) ? graph_in->node_count
                                                                     : WAYWAL_GRAPH_MAX_NODES;
    uint32_t ecnt = (graph_in->edge_count <= WAYWAL_GRAPH_MAX_EDGES) ? graph_in->edge_count
                                                                     : WAYWAL_GRAPH_MAX_EDGES;
    memset(reachable, 0, sizeof(bool) * WAYWAL_GRAPH_MAX_NODES);
    memset(out_target_pass, 0, sizeof(uint32_t) * WAYWAL_GRAPH_MAX_NODES);

    bool reaches_image_sink[WAYWAL_GRAPH_MAX_NODES] = {false};
    int8_t explicit_buf_pass[WAYWAL_GRAPH_MAX_NODES];
    memset(explicit_buf_pass, -1, sizeof(explicit_buf_pass));

    uint16_t bfs_stack[WAYWAL_GRAPH_MAX_NODES];
    uint32_t img_top = 0;
    bool any_sink = false;

    for (uint32_t i = 0; i < ncnt; ++i) {
        if (graph_in->nodes[i].opcode == WAYWAL_NODE_OUT_IMAGE_PASS) {
            any_sink = true;
            reaches_image_sink[i] = true;
            reachable[i] = true;
            bfs_stack[img_top++] = (uint16_t)i;
        }
    }
    while (img_top > 0u) {
        uint16_t cur = bfs_stack[--img_top];
        for (int16_t e = adj->head_in[cur]; e >= 0; e = adj->next_in[e]) {
            int16_t s = adj->edge_src_idx[e];
            if (s >= 0 && !reaches_image_sink[s]) {
                reaches_image_sink[s] = true;
                reachable[s] = true;
                bfs_stack[img_top++] = (uint16_t)s;
            }
        }
    }

    for (uint32_t i = 0; i < ncnt; ++i) {
        if (graph_in->nodes[i].opcode != WAYWAL_NODE_OUT_BUFFER_PASS) {
            continue;
        }
        any_sink = true;
        int8_t bpass = (int8_t)resolve_buffer_pass_index(&graph_in->nodes[i]);
        if (explicit_buf_pass[i] >= 0) {
            continue;
        }
        uint32_t top = 0;
        explicit_buf_pass[i] = bpass;
        reachable[i] = true;
        bfs_stack[top++] = (uint16_t)i;
        while (top > 0u) {
            uint16_t cur = bfs_stack[--top];
            for (int16_t e = adj->head_in[cur]; e >= 0; e = adj->next_in[e]) {
                int16_t s = adj->edge_src_idx[e];
                if (s >= 0 && explicit_buf_pass[s] < 0) {
                    explicit_buf_pass[s] = bpass;
                    reachable[s] = true;
                    bfs_stack[top++] = (uint16_t)s;
                }
            }
        }
    }

    if (!any_sink) {
        for (uint32_t i = 0; i < ncnt; ++i) {
            reachable[i] = true;
            reaches_image_sink[i] = true;
        }
    }

    /* Single O(|V| + |E|) topological pass for barrier stage propagation & leaf pull-up. */
    uint16_t in_deg[WAYWAL_GRAPH_MAX_NODES] = {0};
    uint16_t topo[WAYWAL_GRAPH_MAX_NODES];
    uint32_t reach_cnt = 0;
    for (uint32_t e = 0; e < ecnt; ++e) {
        int16_t s = adj->edge_src_idx[e];
        int16_t d = adj->edge_dst_idx[e];
        if (s < 0 || d < 0 || !reachable[s] || !reachable[d]) {
            continue;
        }
        if (graph_in->nodes[s].opcode == WAYWAL_NODE_IN_PASS_BUFFER ||
            (graph_in->edges[e].flags & WAYWAL_EDGE_FLAG_FEEDBACK_LATCH)) {
            continue;
        }
        in_deg[d]++;
    }
    uint32_t q_head = 0;
    uint32_t q_tail = 0;
    for (uint32_t i = 0; i < ncnt; ++i) {
        if (!reachable[i]) {
            continue;
        }
        reach_cnt++;
        if (in_deg[i] == 0u) {
            topo[q_tail++] = (uint16_t)i;
        }
    }
    uint32_t barrier_stage[WAYWAL_GRAPH_MAX_NODES] = {0};
    while (q_head < q_tail) {
        uint16_t u = topo[q_head++];
        if (graph_in->nodes[u].opcode == WAYWAL_NODE_IN_PASS_BUFFER) {
            continue;
        }
        uint32_t base_stage = barrier_stage[u];
        for (int16_t e = adj->head_out[u]; e >= 0; e = adj->next_out[e]) {
            if (graph_in->edges[e].flags & WAYWAL_EDGE_FLAG_FEEDBACK_LATCH) {
                continue;
            }
            int16_t d = adj->edge_dst_idx[e];
            if (d < 0 || !reachable[d]) {
                continue;
            }
            bool split = edge_requires_barrier_split(graph_in, u, (uint32_t)d, &graph_in->edges[e]);
            uint32_t new_s = base_stage + (split ? 1u : 0u);
            if (new_s > 4u) {
                new_s = 4u;
            }
            if (new_s > barrier_stage[d]) {
                barrier_stage[d] = new_s;
            }
            if (--in_deg[d] == 0u) {
                topo[q_tail++] = (uint16_t)d;
            }
        }
    }

    if (q_tail < reach_cnt) {
        /* Defensive fallback if an unvalidated cyclic graph reaches compile_glsl directly. */
        for (uint32_t iter = 0; iter < ncnt; ++iter) {
            bool changed = false;
            for (uint32_t e = 0; e < ecnt; ++e) {
                int16_t s = adj->edge_src_idx[e];
                int16_t d = adj->edge_dst_idx[e];
                if (s < 0 || d < 0 || !reachable[s] || !reachable[d]) {
                    continue;
                }
                if (graph_in->nodes[s].opcode == WAYWAL_NODE_IN_PASS_BUFFER ||
                    (graph_in->edges[e].flags & WAYWAL_EDGE_FLAG_FEEDBACK_LATCH)) {
                    continue;
                }
                bool split = edge_requires_barrier_split(graph_in, (uint32_t)s, (uint32_t)d,
                                                         &graph_in->edges[e]);
                uint32_t new_s = barrier_stage[s] + (split ? 1u : 0u);
                if (new_s > 4u) {
                    new_s = 4u;
                }
                if (new_s > barrier_stage[d]) {
                    barrier_stage[d] = new_s;
                    changed = true;
                }
            }
            if (!changed) {
                break;
            }
        }
    }

    /* Pull non-barrier leaf inputs up to their consumer stage in reverse topological order. */
    for (uint32_t k = q_tail; k > 0u; --k) {
        uint32_t u = topo[k - 1u];
        if (!reaches_image_sink[u]) {
            continue;
        }
        uint32_t op = graph_in->nodes[u].opcode;
        if (op == WAYWAL_NODE_OUT_IMAGE_PASS || op == WAYWAL_NODE_OUT_BUFFER_PASS ||
            op == WAYWAL_NODE_IN_PASS_BUFFER) {
            continue;
        }
        uint32_t min_dst_stage = UINT32_MAX;
        bool has_consumer = false;
        bool has_split_out = false;
        for (int16_t e = adj->head_out[u]; e >= 0; e = adj->next_out[e]) {
            if (graph_in->edges[e].flags & WAYWAL_EDGE_FLAG_FEEDBACK_LATCH) {
                continue;
            }
            int16_t d = adj->edge_dst_idx[e];
            if (d < 0 || !reachable[d]) {
                continue;
            }
            has_consumer = true;
            if (edge_requires_barrier_split(graph_in, u, (uint32_t)d, &graph_in->edges[e])) {
                has_split_out = true;
                break;
            }
            if (barrier_stage[d] < min_dst_stage) {
                min_dst_stage = barrier_stage[d];
            }
        }
        if (has_consumer && !has_split_out && min_dst_stage < UINT32_MAX &&
            min_dst_stage > barrier_stage[u]) {
            barrier_stage[u] = min_dst_stage;
        }
    }

    uint32_t max_image_stage = 0;
    for (uint32_t i = 0; i < ncnt; ++i) {
        if (reachable[i] && reaches_image_sink[i] && barrier_stage[i] > max_image_stage) {
            max_image_stage = barrier_stage[i];
        }
    }

    uint32_t mask = 0;
    for (uint32_t i = 0; i < ncnt; ++i) {
        if (!reachable[i]) {
            out_target_pass[i] = (graph_in->nodes[i].target_pass < WAYWAL_GRAPH_MAX_PASSES)
                                     ? graph_in->nodes[i].target_pass
                                     : 4u;
            continue;
        }
        uint32_t op = graph_in->nodes[i].opcode;
        if (op == WAYWAL_NODE_OUT_IMAGE_PASS) {
            out_target_pass[i] = 4;
        } else if (op == WAYWAL_NODE_OUT_BUFFER_PASS) {
            out_target_pass[i] = resolve_buffer_pass_index(&graph_in->nodes[i]);
        } else if (!reaches_image_sink[i] && explicit_buf_pass[i] >= 0) {
            out_target_pass[i] = (uint32_t)explicit_buf_pass[i];
        } else if (max_image_stage == 0) {
            out_target_pass[i] = 4;
        } else {
            out_target_pass[i] = (barrier_stage[i] >= max_image_stage)
                                     ? 4u
                                     : ((barrier_stage[i] <= 3u) ? barrier_stage[i] : 3u);
        }
        mask |= (1u << out_target_pass[i]);
    }

    if (mask == 0) {
        mask = (1u << 4);
    }
    if (out_active_pass_mask) {
        *out_active_pass_mask = mask;
    }
}

bool waywal_shader_graph_analyze(const waywal_shader_graph_t *graph_in,
                                 waywal_graph_schedule_t *out_schedule)
{
    if (!graph_in || !out_schedule)
        return false;
    memset(out_schedule, 0, sizeof(*out_schedule));

    uint32_t ncnt = (graph_in->node_count <= WAYWAL_GRAPH_MAX_NODES) ? graph_in->node_count
                                                                     : WAYWAL_GRAPH_MAX_NODES;

    if (ncnt == 0) {
        return true;
    }

    graph_adj_t adj;
    graph_adj_build(graph_in, &adj);

    uint32_t cycle_node = 0;
    if (has_cycle_with_adj(graph_in, &adj, &cycle_node)) {
        out_schedule->has_cycle_error = 1;
        out_schedule->cycle_node_id = cycle_node;
        snprintf(out_schedule->error_msg, sizeof(out_schedule->error_msg),
                 "Directed cycle detected involving node ID %u", cycle_node);
        return false;
    }

    bool reachable[WAYWAL_GRAPH_MAX_NODES];
    uint32_t target_pass[WAYWAL_GRAPH_MAX_NODES];
    compute_node_passes_readonly(graph_in, &adj, reachable, target_pass,
                                 &out_schedule->active_pass_mask);

    uint32_t sethi_ullman[WAYWAL_GRAPH_MAX_NODES] = {0};
    compute_sethi_ullman_all(graph_in, &adj, ncnt, reachable, sethi_ullman);

    uint32_t scheduled_count = 0;
    for (uint32_t pass = 0; pass < WAYWAL_GRAPH_MAX_PASSES; ++pass) {
        if (!(out_schedule->active_pass_mask & (1u << pass))) {
            continue;
        }

        uint32_t in_degree[WAYWAL_GRAPH_MAX_NODES] = {0};
        uint16_t ready[WAYWAL_GRAPH_MAX_NODES];
        uint32_t ready_len = 0;

        for (uint32_t i = 0; i < ncnt; ++i) {
            if (!reachable[i] || target_pass[i] != pass)
                continue;
            for (int16_t e = adj.head_in[i]; e >= 0; e = adj.next_in[e]) {
                if (graph_in->edges[e].flags & WAYWAL_EDGE_FLAG_FEEDBACK_LATCH)
                    continue;
                int16_t s = adj.edge_src_idx[e];
                if (s >= 0 && reachable[s] && target_pass[s] == pass &&
                    graph_in->nodes[s].opcode != WAYWAL_NODE_IN_PASS_BUFFER) {
                    in_degree[i]++;
                }
            }
            if (in_degree[i] == 0) {
                ready[ready_len++] = (uint16_t)i;
            }
        }

        while (ready_len > 0) {
            uint32_t best_pos = 0;
            uint32_t best_idx = ready[0];
            uint32_t best_weight = sethi_ullman[best_idx];

            for (uint32_t r = 1; r < ready_len; ++r) {
                uint32_t cand = ready[r];
                if (sethi_ullman[cand] > best_weight ||
                    (sethi_ullman[cand] == best_weight && cand < best_idx)) {
                    best_pos = r;
                    best_idx = cand;
                    best_weight = sethi_ullman[cand];
                }
            }

            ready[best_pos] = ready[--ready_len];
            out_schedule->sorted_node_indices[scheduled_count++] = best_idx;

            if (graph_in->nodes[best_idx].opcode == WAYWAL_NODE_IN_PASS_BUFFER) {
                continue;
            }

            for (int16_t e = adj.head_out[best_idx]; e >= 0; e = adj.next_out[e]) {
                if (graph_in->edges[e].flags & WAYWAL_EDGE_FLAG_FEEDBACK_LATCH)
                    continue;
                int16_t d = adj.edge_dst_idx[e];
                if (d >= 0 && reachable[d] && target_pass[d] == pass && in_degree[d] > 0) {
                    in_degree[d]--;
                    if (in_degree[d] == 0) {
                        ready[ready_len++] = (uint16_t)d;
                    }
                }
            }
        }
    }

    out_schedule->sorted_count = scheduled_count;

    uint32_t alu_ops = 0;
    uint32_t tex_fetches = 0;

    for (uint32_t k = 0; k < scheduled_count; ++k) {
        uint32_t idx = out_schedule->sorted_node_indices[k];
        uint32_t op = graph_in->nodes[idx].opcode;

        if (op >= WAYWAL_NODE_MATH_ADD && op <= WAYWAL_NODE_MATH_ABS) {
            alu_ops += (op == WAYWAL_NODE_MATH_FMA || op == WAYWAL_NODE_MATH_MIX ||
                        op == WAYWAL_NODE_MATH_SMOOTHSTEP)
                           ? 3
                           : 1;
        } else if (op >= WAYWAL_NODE_NOISE_SIMPLEX2D && op <= WAYWAL_NODE_NOISE_FBM2D) {
            alu_ops += (op == WAYWAL_NODE_NOISE_FBM2D) ? 64 : 24;
        } else if (op >= WAYWAL_NODE_COLOR_OKLCH_TO_SRGB && op <= WAYWAL_NODE_COLOR_SRGB_TO_OKLCH) {
            alu_ops += 14;
        } else if (op == WAYWAL_NODE_LIGHT_BLINN_PHONG) {
            alu_ops += 20;
        } else if (op == WAYWAL_NODE_IN_TEXTURE2D || op == WAYWAL_NODE_IN_PASS_BUFFER ||
                   op == WAYWAL_NODE_IN_DEPTH_MAP) {
            tex_fetches += 1;
        } else if (op == WAYWAL_NODE_FILTER_DUAL_KAWASE) {
            tex_fetches += 5;
            alu_ops += 8;
        } else if (op == WAYWAL_NODE_FILTER_CHROMATIC_ABERR) {
            tex_fetches += 3;
            alu_ops += 6;
        } else if (op == WAYWAL_NODE_LIGHT_NORMAL_FROM_HEIGHT) {
            tex_fetches += 4;
            alu_ops += 12;
        } else if (op == WAYWAL_NODE_BUILTIN_EFFECT) {
            tex_fetches += 2;
            alu_ops += 30;
        }
    }

    out_schedule->estimated_alu_ops = alu_ops;
    out_schedule->estimated_tex_fetches = tex_fetches;
    return true;
}

/* ========================================================================= */
/* GLSL ES 3.10 Code Generator (Single Static Assignment SSA)                */
/* ========================================================================= */

static inline const char *resolve_pass_buffer_sampler(const char *key, uint32_t fallback_pass)
{
    if (key && key[0] != '\0') {
        if (strstr(key, "BUFFER_B") || strstr(key, "buffer_b"))
            return "u_BufferB";
        if (strstr(key, "BUFFER_C") || strstr(key, "buffer_c"))
            return "u_BufferC";
        if (strstr(key, "BUFFER_D") || strstr(key, "buffer_d"))
            return "u_BufferD";
        if (strstr(key, "BUFFER_A") || strstr(key, "buffer_a"))
            return "u_BufferA";
        if (strstr(key, "DEPTH") || strstr(key, "depth"))
            return "u_DepthMap";
    }
    switch (fallback_pass) {
    case 1:
        return "u_BufferB";
    case 2:
        return "u_BufferC";
    case 3:
        return "u_BufferD";
    default:
        return "u_BufferA";
    }
}

static const char *resolve_spatial_sampler(const waywal_shader_graph_t *graph,
                                           const graph_adj_t *adj, const uint32_t *node_pass,
                                           uint32_t node_idx, uint32_t pin_idx,
                                           bool *out_has_sampler)
{
    if (pin_idx < WAYWAL_GRAPH_MAX_PINS_PER_NODE) {
        int16_t e_idx = adj->pin_in_edge[node_idx][pin_idx];
        if (e_idx >= 0) {
            int16_t s_idx = adj->edge_src_idx[e_idx];
            if (s_idx >= 0) {
                const waywal_graph_node_t *src = &graph->nodes[s_idx];
                if (node_pass[s_idx] != node_pass[node_idx]) {
                    if (out_has_sampler)
                        *out_has_sampler = true;
                    return resolve_pass_buffer_sampler(NULL, node_pass[s_idx]);
                }
                if (src->opcode == WAYWAL_NODE_IN_PASS_BUFFER ||
                    src->opcode == WAYWAL_NODE_IN_TEXTURE2D) {
                    if (out_has_sampler)
                        *out_has_sampler = true;
                    return resolve_pass_buffer_sampler(src->prop_or_asset_key, 0);
                }
                if (src->opcode == WAYWAL_NODE_IN_DEPTH_MAP) {
                    if (out_has_sampler)
                        *out_has_sampler = true;
                    return "u_DepthMap";
                }
                if (out_has_sampler)
                    *out_has_sampler = false;
                return "u_BufferA";
            }
        }
    }
    if (out_has_sampler)
        *out_has_sampler = false;
    return "u_BufferA";
}

static bool key_contains_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle || needle[0] == '\0')
        return false;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p != '\0'; ++p) {
        size_t i = 0;
        while (i < nlen && p[i] != '\0' &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            ++i;
        }
        if (i == nlen)
            return true;
    }
    return false;
}

enum {
    WWAL_HLP_OKLCH_TO_SRGB = (1u << 0),
    WWAL_HLP_SRGB_TO_OKLCH = (1u << 1),
    WWAL_HLP_HASH21 = (1u << 2),
    WWAL_HLP_SIMPLEX2D = (1u << 3),
    WWAL_HLP_CURL2D = (1u << 4),
    WWAL_HLP_VORONOI2D = (1u << 5),
    WWAL_HLP_FBM2D = (1u << 6),
    WWAL_HLP_SDF_BOX = (1u << 7),
    WWAL_HLP_UV_POLAR = (1u << 8),
    WWAL_HLP_UV_TWIRL = (1u << 9),
    WWAL_HLP_HUE_SHIFT = (1u << 10),
    WWAL_HLP_CONTRAST_SAT = (1u << 11),
    WWAL_HLP_DUAL_KAWASE = (1u << 12),
    WWAL_HLP_CHROMATIC_ABERR = (1u << 13),
    WWAL_HLP_NORMAL_HEIGHT_TEX = (1u << 14),
    WWAL_HLP_NORMAL_HEIGHT = (1u << 15),
    WWAL_HLP_BLINN_PHONG = (1u << 16),
    WWAL_HLP_FX_WATERRIPPLE = (1u << 17),
    WWAL_HLP_FX_WATERWAVES = (1u << 18),
    WWAL_HLP_FX_CAS_SHARPEN = (1u << 19),
    WWAL_HLP_FX_VIGNETTE = (1u << 20),
    WWAL_HLP_FX_CRT_PIXELATE = (1u << 21),
    WWAL_HLP_FX_GODRAYS = (1u << 22),
    WWAL_HLP_SAFE_POW = (1u << 23),
};

static uint32_t helper_mask_for_builtin_key(const char *key)
{
    if (!key || key[0] == '\0' || key_contains_ci(key, "waterripple") ||
        key_contains_ci(key, "ripple")) {
        return WWAL_HLP_FX_WATERRIPPLE;
    }
    if (key_contains_ci(key, "water") || key_contains_ci(key, "refract") ||
        key_contains_ci(key, "rain")) {
        return WWAL_HLP_FX_WATERWAVES;
    }
    if (key_contains_ci(key, "bloom") || key_contains_ci(key, "halo") ||
        key_contains_ci(key, "flare") || key_contains_ci(key, "shine") ||
        key_contains_ci(key, "blur") || key_contains_ci(key, "depth_of_field")) {
        return WWAL_HLP_DUAL_KAWASE;
    }
    if (key_contains_ci(key, "chromatic") || key_contains_ci(key, "vhs")) {
        return WWAL_HLP_CHROMATIC_ABERR;
    }
    if (key_contains_ci(key, "sharpen") || key_contains_ci(key, "edge") ||
        key_contains_ci(key, "outline")) {
        return WWAL_HLP_FX_CAS_SHARPEN;
    }
    if (key_contains_ci(key, "vignette") || key_contains_ci(key, "iris")) {
        return WWAL_HLP_FX_VIGNETTE;
    }
    if (key_contains_ci(key, "crt") || key_contains_ci(key, "pixelate") ||
        key_contains_ci(key, "halftone")) {
        return WWAL_HLP_FX_CRT_PIXELATE;
    }
    if (key_contains_ci(key, "twirl") || key_contains_ci(key, "fisheye") ||
        key_contains_ci(key, "pendulum") || key_contains_ci(key, "perspective")) {
        return WWAL_HLP_UV_TWIRL;
    }
    if (key_contains_ci(key, "foliage") || key_contains_ci(key, "scroll") ||
        key_contains_ci(key, "shake") || key_contains_ci(key, "skew") ||
        key_contains_ci(key, "pulse")) {
        return 0u;
    }
    if (key_contains_ci(key, "hsv") || key_contains_ci(key, "tint") ||
        key_contains_ci(key, "colorgrading")) {
        return WWAL_HLP_HUE_SHIFT;
    }
    if (key_contains_ci(key, "contrast")) {
        return WWAL_HLP_CONTRAST_SAT;
    }
    if (key_contains_ci(key, "godrays") || key_contains_ci(key, "lightshafts") ||
        key_contains_ci(key, "aurora") || key_contains_ci(key, "fire") ||
        key_contains_ci(key, "nitro") || key_contains_ci(key, "cloud")) {
        return WWAL_HLP_FX_GODRAYS;
    }
    return WWAL_HLP_FX_WATERWAVES;
}

static uint32_t helper_mask_for_node(const waywal_shader_graph_t *graph, const graph_adj_t *adj,
                                     const uint32_t *node_pass, uint32_t idx)
{
    const waywal_graph_node_t *node = &graph->nodes[idx];
    if ((node->flags & WAYWAL_NODE_FLAG_MUTED_BYPASS) && node->output_pin_count > 0) {
        return 0u;
    }
    switch (node->opcode) {
    case WAYWAL_NODE_MATH_POW:
        return WWAL_HLP_SAFE_POW;
    case WAYWAL_NODE_NOISE_SIMPLEX2D:
        return WWAL_HLP_SIMPLEX2D;
    case WAYWAL_NODE_NOISE_CURL2D:
        return WWAL_HLP_CURL2D;
    case WAYWAL_NODE_NOISE_VORONOI2D:
        return WWAL_HLP_VORONOI2D;
    case WAYWAL_NODE_NOISE_FBM2D:
        return WWAL_HLP_FBM2D;
    case WAYWAL_NODE_SDF_BOX:
        return WWAL_HLP_SDF_BOX;
    case WAYWAL_NODE_UV_POLAR:
        return WWAL_HLP_UV_POLAR;
    case WAYWAL_NODE_UV_TWIRL:
        return WWAL_HLP_UV_TWIRL;
    case WAYWAL_NODE_COLOR_OKLCH_TO_SRGB:
        return WWAL_HLP_OKLCH_TO_SRGB;
    case WAYWAL_NODE_COLOR_SRGB_TO_OKLCH:
        return WWAL_HLP_SRGB_TO_OKLCH;
    case WAYWAL_NODE_COLOR_HUE_SHIFT:
        return WWAL_HLP_HUE_SHIFT;
    case WAYWAL_NODE_COLOR_CONTRAST_SAT:
        return WWAL_HLP_CONTRAST_SAT;
    case WAYWAL_NODE_FILTER_DUAL_KAWASE: {
        bool has_smp = false;
        (void)resolve_spatial_sampler(graph, adj, node_pass, idx, 0, &has_smp);
        return has_smp ? WWAL_HLP_DUAL_KAWASE : 0u;
    }
    case WAYWAL_NODE_FILTER_CHROMATIC_ABERR: {
        bool has_smp = false;
        (void)resolve_spatial_sampler(graph, adj, node_pass, idx, 0, &has_smp);
        return has_smp ? WWAL_HLP_CHROMATIC_ABERR : 0u;
    }
    case WAYWAL_NODE_LIGHT_NORMAL_FROM_HEIGHT: {
        bool has_smp = false;
        (void)resolve_spatial_sampler(graph, adj, node_pass, idx, 0, &has_smp);
        return (adj->pin_in_edge[idx][0] >= 0 && has_smp) ? WWAL_HLP_NORMAL_HEIGHT_TEX
                                                          : WWAL_HLP_NORMAL_HEIGHT;
    }
    case WAYWAL_NODE_LIGHT_BLINN_PHONG:
        return WWAL_HLP_BLINN_PHONG;
    case WAYWAL_NODE_BUILTIN_EFFECT:
        return helper_mask_for_builtin_key(node->prop_or_asset_key);
    default:
        return 0u;
    }
}

/* ========================================================================= */
/* Hanson Monotonic Bump Arena & Fast Ryu Float Emitter (PERF-039)           */
/* ========================================================================= */

typedef struct wwal_bump_chunk {
    struct wwal_bump_chunk *next;
    size_t used;
    size_t cap;
    uint8_t *data;
} wwal_bump_chunk_t;

typedef struct {
    wwal_bump_chunk_t *head;
    wwal_bump_chunk_t *tail;
    wwal_bump_chunk_t *current;
    wwal_bump_chunk_t *tls_chunk;
    size_t default_chunk_bytes;
} wwal_bump_arena_t;

/* 128 KiB per-thread primary slab eliminates glibc arena lock contention during compile. */
static _Thread_local struct {
    bool in_use;
    wwal_bump_chunk_t chunk;
    _Alignas(16) uint8_t storage[128u * 1024u];
} g_compile_tls_slab;

static void wwal_bump_arena_init_compile(wwal_bump_arena_t *arena)
{
    arena->default_chunk_bytes = 128u * 1024u;
    if (!g_compile_tls_slab.in_use) {
        g_compile_tls_slab.in_use = true;
        g_compile_tls_slab.chunk.next = NULL;
        g_compile_tls_slab.chunk.used = 0u;
        g_compile_tls_slab.chunk.cap = sizeof(g_compile_tls_slab.storage);
        g_compile_tls_slab.chunk.data = g_compile_tls_slab.storage;
        arena->head = &g_compile_tls_slab.chunk;
        arena->tail = &g_compile_tls_slab.chunk;
        arena->current = &g_compile_tls_slab.chunk;
        arena->tls_chunk = &g_compile_tls_slab.chunk;
    } else {
        arena->head = NULL;
        arena->tail = NULL;
        arena->current = NULL;
        arena->tls_chunk = NULL;
    }
}

static inline size_t wwal_ptr_align_pad(const uint8_t *base, size_t used, size_t align)
{
    uintptr_t misalign = ((uintptr_t)base + (uintptr_t)used) & (uintptr_t)(align - 1u);
    return (misalign != 0u) ? (align - (size_t)misalign) : 0u;
}

static void *wwal_bump_alloc(wwal_bump_arena_t *arena, size_t size, size_t align)
{
    if (!arena || size == 0u || align == 0u || (align & (align - 1u)) != 0u) {
        return NULL;
    }
    wwal_bump_chunk_t *c = arena->current;
    if (c) {
        size_t pad = wwal_ptr_align_pad(c->data, c->used, align);
        size_t rem = c->cap - c->used;
        if (pad <= rem && size <= rem - pad) {
            void *ptr = c->data + c->used + pad;
            c->used += pad + size;
            return ptr;
        }
    }
    if (align - 1u > SIZE_MAX - size) {
        return NULL;
    }
    size_t need = size + (align - 1u);
    size_t min_cap = (arena->default_chunk_bytes > 4096u) ? arena->default_chunk_bytes : 4096u;
    size_t cap = (need > min_cap) ? need : min_cap;
    if (cap > SIZE_MAX - sizeof(wwal_bump_chunk_t) - 16u) {
        return NULL;
    }
    wwal_bump_chunk_t *nc = (wwal_bump_chunk_t *)malloc(sizeof(wwal_bump_chunk_t) + 16u + cap);
    if (!nc) {
        return NULL;
    }
    uintptr_t raw = (uintptr_t)(nc + 1);
    nc->data = (uint8_t *)((raw + 15u) & ~(uintptr_t)15u);
    nc->next = NULL;
    nc->used = 0u;
    nc->cap = cap;

    if (!arena->head) {
        arena->head = nc;
        arena->tail = nc;
    } else {
        arena->tail->next = nc;
        arena->tail = nc;
    }
    arena->current = nc;

    size_t pad = wwal_ptr_align_pad(nc->data, 0u, align);
    void *ptr = nc->data + pad;
    nc->used = pad + size;
    return ptr;
}

static void wwal_bump_arena_free(wwal_bump_arena_t *arena)
{
    if (!arena) {
        return;
    }
    wwal_bump_chunk_t *cur = arena->head;
    while (cur) {
        wwal_bump_chunk_t *nxt = cur->next;
        if (cur != arena->tls_chunk) {
            free(cur);
        }
        cur = nxt;
    }
    if (arena->tls_chunk) {
        g_compile_tls_slab.in_use = false;
    }
    arena->head = NULL;
    arena->tail = NULL;
    arena->current = NULL;
    arena->tls_chunk = NULL;
}

/* Ryu binary32 shortest decimal formatter emitting GLSL-valid float literals (always '.' or 'e').
 */
#define RYU_F32_POW5_INV_BITCOUNT 59
#define RYU_F32_POW5_BITCOUNT     61

static const uint64_t k_ryu_f32_pow5_inv[31] = {
    0x0800000000000001ULL, 0x0666666666666667ULL, 0x051eb851eb851eb9ULL, 0x04189374bc6a7efaULL,
    0x068db8bac710cb2aULL, 0x053e2d6238da3c22ULL, 0x0431bde82d7b634eULL, 0x06b5fca6af2bd216ULL,
    0x055e63b88c230e78ULL, 0x044b82fa09b5a52dULL, 0x06df37f675ef6eaeULL, 0x057f5ff85e592558ULL,
    0x0465e6604b7a8447ULL, 0x0709709a125da071ULL, 0x05a126e1a84ae6c1ULL, 0x0480ebe7b9d58567ULL,
    0x0734aca5f6226f0bULL, 0x05c3bd5191b525a3ULL, 0x049c97747490eae9ULL, 0x0760f253edb4ab0eULL,
    0x05e72843249088d8ULL, 0x04b8ed0283a6d3e0ULL, 0x078e480405d7b966ULL, 0x060b6cd004ac9452ULL,
    0x04d5f0a66a23a9dbULL, 0x07bcb43d769f762bULL, 0x063090312bb2c4efULL, 0x04f3a68dbc8f03f3ULL,
    0x07ec3daf94180651ULL, 0x065697bfa9acd1daULL, 0x051212ffbaf0a7e2ULL,
};

static const uint64_t k_ryu_f32_pow5_fwd[47] = {
    0x1000000000000000ULL, 0x1400000000000000ULL, 0x1900000000000000ULL, 0x1f40000000000000ULL,
    0x1388000000000000ULL, 0x186a000000000000ULL, 0x1e84800000000000ULL, 0x1312d00000000000ULL,
    0x17d7840000000000ULL, 0x1dcd650000000000ULL, 0x12a05f2000000000ULL, 0x174876e800000000ULL,
    0x1d1a94a200000000ULL, 0x12309ce540000000ULL, 0x16bcc41e90000000ULL, 0x1c6bf52634000000ULL,
    0x11c37937e0800000ULL, 0x16345785d8a00000ULL, 0x1bc16d674ec80000ULL, 0x1158e460913d0000ULL,
    0x15af1d78b58c4000ULL, 0x1b1ae4d6e2ef5000ULL, 0x10f0cf064dd59200ULL, 0x152d02c7e14af680ULL,
    0x1a784379d99db420ULL, 0x108b2a2c28029094ULL, 0x14adf4b7320334b9ULL, 0x19d971e4fe8401e7ULL,
    0x1027e72f1f128130ULL, 0x1431e0fae6d7217cULL, 0x193e5939a08ce9dbULL, 0x1f8def8808b02452ULL,
    0x13b8b5b5056e16b3ULL, 0x18a6e32246c99c60ULL, 0x1ed09bead87c0378ULL, 0x13426172c74d822bULL,
    0x1812f9cf7920e2b6ULL, 0x1e17b84357691b64ULL, 0x12ced32a16a1b11eULL, 0x178287f49c4a1d66ULL,
    0x1d6329f1c35ca4bfULL, 0x125dfa371a19e6f7ULL, 0x16f578c4e0a060b5ULL, 0x1cb2d6f618c878e3ULL,
    0x11efc659cf7d4b8dULL, 0x166bb7f0435c9e71ULL, 0x1c06a5ec5433c60dULL,
};

static inline uint32_t ryu_pow5bits(int32_t e)
{
    return (e == 0) ? 1u : (uint32_t)((((uint32_t)e * 1217359u) >> 19) + 1u);
}

static inline uint32_t ryu_mul_shift32(uint32_t m, uint64_t factor, int32_t shift)
{
    uint64_t bits0 = (uint64_t)m * (uint32_t)factor;
    uint64_t bits1 = (uint64_t)m * (uint32_t)(factor >> 32);
    uint64_t sum = (bits0 >> 32) + bits1;
    return (uint32_t)(sum >> (shift - 32));
}

static inline uint32_t ryu_pow5_factor(uint32_t v)
{
    uint32_t count = 0;
    while (v > 0u && (v % 5u) == 0u) {
        v /= 5u;
        count++;
    }
    return count;
}

static inline uint32_t ryu_dec_len9(uint32_t v)
{
    if (v >= 100000000u)
        return 9u;
    if (v >= 10000000u)
        return 8u;
    if (v >= 1000000u)
        return 7u;
    if (v >= 100000u)
        return 6u;
    if (v >= 10000u)
        return 5u;
    if (v >= 1000u)
        return 4u;
    if (v >= 100u)
        return 3u;
    if (v >= 10u)
        return 2u;
    return 1u;
}

static size_t ryu_f32_to_glsl(float f, char out[24])
{
    uint32_t bits = 0;
    memcpy(&bits, &f, sizeof(bits));
    bool sign = ((bits >> 31) & 1u) != 0u;
    uint32_t ieee_m = bits & ((1u << 23) - 1u);
    uint32_t ieee_e = (bits >> 23) & 0xFFu;

    if ((ieee_e == 0u && ieee_m == 0u) || ieee_e == 0xFFu) {
        memcpy(out, "0.0", 3u);
        return 3u;
    }

    int32_t e2;
    uint32_t m2;
    if (ieee_e == 0u) {
        e2 = 1 - 127 - 23 - 2;
        m2 = ieee_m;
    } else {
        e2 = (int32_t)ieee_e - 127 - 23 - 2;
        m2 = (1u << 23) | ieee_m;
    }
    bool accept_bounds = (m2 & 1u) == 0u;
    uint32_t mv = 4u * m2;
    uint32_t mp = 4u * m2 + 2u;
    uint32_t mm_shift = (ieee_m != 0u || ieee_e <= 1u) ? 1u : 0u;
    uint32_t mm = 4u * m2 - 1u - mm_shift;

    uint32_t vr, vp, vm;
    int32_t e10;
    bool vm_tz = false;
    bool vr_tz = false;
    uint8_t last_digit = 0;

    if (e2 >= 0) {
        uint32_t q = ((uint32_t)e2 * 78913u) >> 18;
        e10 = (int32_t)q;
        int32_t k = RYU_F32_POW5_INV_BITCOUNT + (int32_t)ryu_pow5bits((int32_t)q) - 1;
        int32_t i = -e2 + (int32_t)q + k;
        vr = ryu_mul_shift32(mv, k_ryu_f32_pow5_inv[q], i);
        vp = ryu_mul_shift32(mp, k_ryu_f32_pow5_inv[q], i);
        vm = ryu_mul_shift32(mm, k_ryu_f32_pow5_inv[q], i);
        if (q != 0u && (vp - 1u) / 10u <= vm / 10u) {
            int32_t l = RYU_F32_POW5_INV_BITCOUNT + (int32_t)ryu_pow5bits((int32_t)(q - 1u)) - 1;
            last_digit = (uint8_t)(ryu_mul_shift32(mv, k_ryu_f32_pow5_inv[q - 1u],
                                                   -e2 + (int32_t)q - 1 + l) %
                                   10u);
        }
        if (q <= 9u) {
            if (mv % 5u == 0u) {
                vr_tz = (ryu_pow5_factor(mv) >= q);
            } else if (accept_bounds) {
                vm_tz = (ryu_pow5_factor(mm) >= q);
            } else {
                vp -= (ryu_pow5_factor(mp) >= q) ? 1u : 0u;
            }
        }
    } else {
        uint32_t q = ((uint32_t)(-e2) * 732923u) >> 20;
        e10 = (int32_t)q + e2;
        int32_t i = -e2 - (int32_t)q;
        int32_t k = (int32_t)ryu_pow5bits(i) - RYU_F32_POW5_BITCOUNT;
        int32_t j = (int32_t)q - k;
        vr = ryu_mul_shift32(mv, k_ryu_f32_pow5_fwd[i], j);
        vp = ryu_mul_shift32(mp, k_ryu_f32_pow5_fwd[i], j);
        vm = ryu_mul_shift32(mm, k_ryu_f32_pow5_fwd[i], j);
        if (q != 0u && (vp - 1u) / 10u <= vm / 10u) {
            j = (int32_t)q - 1 - ((int32_t)ryu_pow5bits(i + 1) - RYU_F32_POW5_BITCOUNT);
            last_digit = (uint8_t)(ryu_mul_shift32(mv, k_ryu_f32_pow5_fwd[i + 1], j) % 10u);
        }
        if (q <= 1u) {
            vr_tz = true;
            if (accept_bounds) {
                vm_tz = (mm_shift == 1u);
            } else {
                --vp;
            }
        } else if (q < 31u) {
            vr_tz = ((mv & ((1u << (q - 1u)) - 1u)) == 0u);
        }
    }

    uint32_t removed = 0;
    if (vm_tz || vr_tz) {
        while (vp / 10u > vm / 10u) {
            vm_tz &= ((vm % 10u) == 0u);
            vr_tz &= (last_digit == 0u);
            last_digit = (uint8_t)(vr % 10u);
            vr /= 10u;
            vp /= 10u;
            vm /= 10u;
            ++removed;
        }
        if (vm_tz) {
            while ((vm % 10u) == 0u) {
                vr_tz &= (last_digit == 0u);
                last_digit = (uint8_t)(vr % 10u);
                vr /= 10u;
                vp /= 10u;
                vm /= 10u;
                ++removed;
            }
        }
        if (vr_tz && last_digit == 5u && (vr % 2u) == 0u) {
            last_digit = 4u;
        }
        vr += ((vr == vm && (!accept_bounds || !vm_tz)) || last_digit >= 5u) ? 1u : 0u;
    } else {
        while (vp / 10u > vm / 10u) {
            last_digit = (uint8_t)(vr % 10u);
            vr /= 10u;
            vp /= 10u;
            vm /= 10u;
            ++removed;
        }
        vr += (vr == vm || last_digit >= 5u) ? 1u : 0u;
    }

    uint32_t olength = ryu_dec_len9(vr);
    int32_t kk = e10 + (int32_t)removed + (int32_t)olength;
    size_t idx = 0;
    if (sign) {
        out[idx++] = '-';
    }

    char digits[12];
    uint32_t tmp = vr;
    for (int32_t d = (int32_t)olength - 1; d >= 0; --d) {
        digits[d] = (char)('0' + (tmp % 10u));
        tmp /= 10u;
    }

    if (kk >= 1 && kk <= 7) {
        if ((int32_t)olength <= kk) {
            memcpy(out + idx, digits, olength);
            idx += olength;
            for (int32_t z = (int32_t)olength; z < kk; ++z) {
                out[idx++] = '0';
            }
            out[idx++] = '.';
            out[idx++] = '0';
        } else {
            memcpy(out + idx, digits, (size_t)kk);
            idx += (size_t)kk;
            out[idx++] = '.';
            memcpy(out + idx, digits + kk, olength - (uint32_t)kk);
            idx += olength - (uint32_t)kk;
        }
    } else if (kk > -4 && kk <= 0) {
        out[idx++] = '0';
        out[idx++] = '.';
        for (int32_t z = 0; z < -kk; ++z) {
            out[idx++] = '0';
        }
        memcpy(out + idx, digits, olength);
        idx += olength;
    } else {
        out[idx++] = digits[0];
        out[idx++] = '.';
        if (olength > 1u) {
            memcpy(out + idx, digits + 1, olength - 1u);
            idx += olength - 1u;
        } else {
            out[idx++] = '0';
        }
        out[idx++] = 'e';
        int32_t eval = kk - 1;
        if (eval < 0) {
            out[idx++] = '-';
            eval = -eval;
        }
        if (eval >= 10) {
            out[idx++] = (char)('0' + eval / 10);
            out[idx++] = (char)('0' + eval % 10);
        } else {
            out[idx++] = (char)('0' + eval);
        }
    }
    return idx;
}

typedef struct {
    wwal_bump_arena_t *arena;
    char *data;
    size_t len;
    size_t cap;
    bool oom;
} arena_str_t;

static bool ab_init(arena_str_t *ab, wwal_bump_arena_t *arena, size_t init_cap)
{
    ab->arena = arena;
    ab->len = 0u;
    ab->oom = false;
    ab->cap = (init_cap > 4096u) ? init_cap : 4096u;
    ab->data = (char *)wwal_bump_alloc(arena, ab->cap, 16u);
    if (!ab->data) {
        ab->oom = true;
        ab->cap = 0u;
        return false;
    }
    return true;
}

static bool ab_grow(arena_str_t *ab, size_t extra)
{
    if (!ab || ab->oom || !ab->data) {
        return false;
    }
    if (extra > SIZE_MAX - ab->len - 1u) {
        ab->oom = true;
        return false;
    }
    size_t needed = ab->len + extra + 1u;
    if (needed <= ab->cap) {
        return true;
    }
    size_t new_cap = (ab->cap <= SIZE_MAX / 2u) ? (ab->cap * 2u) : SIZE_MAX;
    if (new_cap < needed) {
        new_cap = (needed <= SIZE_MAX - 4096u) ? (needed + 4096u) : needed;
    }
    /* Extend in-place when this buffer sits at the active bump frontier. */
    wwal_bump_chunk_t *cur = ab->arena->current;
    if (cur && (uint8_t *)ab->data + ab->cap == cur->data + cur->used) {
        size_t delta = new_cap - ab->cap;
        if (delta <= cur->cap - cur->used) {
            cur->used += delta;
            ab->cap = new_cap;
            return true;
        }
    }
    char *nd = (char *)wwal_bump_alloc(ab->arena, new_cap, 16u);
    if (!nd) {
        ab->oom = true;
        return false;
    }
    memcpy(nd, ab->data, ab->len);
    ab->data = nd;
    ab->cap = new_cap;
    return true;
}

static inline void ab_append_n(arena_str_t *ab, const char *s, size_t n)
{
    if (!s || n == 0u) {
        return;
    }
    if (ab->len + n + 1u > ab->cap && !ab_grow(ab, n)) {
        return;
    }
    memcpy(ab->data + ab->len, s, n);
    ab->len += n;
}

#define ab_append_lit(ab, lit) ab_append_n((ab), (lit), sizeof(lit) - 1u)

static inline void ab_append_cstr(arena_str_t *ab, const char *s)
{
    if (s && s[0] != '\0') {
        ab_append_n(ab, s, strlen(s));
    }
}

static inline void ab_append_u32(arena_str_t *ab, uint32_t v)
{
    char tmp[12];
    size_t pos = sizeof(tmp);
    do {
        tmp[--pos] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);
    ab_append_n(ab, tmp + pos, sizeof(tmp) - pos);
}

static inline void ab_append_f32(arena_str_t *ab, float f)
{
    char tmp[24];
    size_t n = ryu_f32_to_glsl(f, tmp);
    ab_append_n(ab, tmp, n);
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} stack_writer_t;

static inline void sw_append_n(stack_writer_t *w, const char *s, size_t n)
{
    if (!s || n == 0u || w->len >= w->cap - 1u) {
        return;
    }
    size_t avail = w->cap - 1u - w->len;
    size_t copy_n = (n < avail) ? n : avail;
    memcpy(w->data + w->len, s, copy_n);
    w->len += copy_n;
}

#define sw_append_lit(w, lit) sw_append_n((w), (lit), sizeof(lit) - 1u)

static inline void sw_append_u32(stack_writer_t *w, uint32_t v)
{
    char tmp[12];
    size_t pos = sizeof(tmp);
    do {
        tmp[--pos] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);
    sw_append_n(w, tmp + pos, sizeof(tmp) - pos);
}

static inline void sw_append_i32(stack_writer_t *w, int32_t v)
{
    if (v < 0) {
        sw_append_n(w, "-", 1u);
    }
    uint32_t mag = (v < 0) ? (0u - (uint32_t)v) : (uint32_t)v;
    sw_append_u32(w, mag);
}

static inline void sw_append_f32(stack_writer_t *w, float f)
{
    char tmp[24];
    size_t n = ryu_f32_to_glsl(f, tmp);
    sw_append_n(w, tmp, n);
}

static void emit_glsl_helpers(arena_str_t *sb, uint32_t helper_mask)
{
    if (helper_mask & (WWAL_HLP_CURL2D | WWAL_HLP_FBM2D))
        helper_mask |= WWAL_HLP_SIMPLEX2D;
    if (helper_mask & (WWAL_HLP_SIMPLEX2D | WWAL_HLP_VORONOI2D))
        helper_mask |= WWAL_HLP_HASH21;
    if (helper_mask & WWAL_HLP_HUE_SHIFT)
        helper_mask |= (WWAL_HLP_SRGB_TO_OKLCH | WWAL_HLP_OKLCH_TO_SRGB);

    if (helper_mask == 0u)
        return;

    if (helper_mask & WWAL_HLP_SAFE_POW) {
        ab_append_lit(sb,
                      "vec4 wwal_safe_pow(vec4 base, vec4 exp_v) {\n"
                      "    bvec4 pos_base = greaterThan(base, vec4(0.0));\n"
                      "    bvec4 pos_exp = greaterThan(exp_v, vec4(0.0));\n"
                      "    vec4 safe_base = mix(vec4(1.0), base, pos_base);\n"
                      "    vec4 p = pow(safe_base, exp_v);\n"
                      "    vec4 zero_base = mix(vec4(0.0), vec4(1.0), equal(exp_v, vec4(0.0)));\n"
                      "    return mix(mix(zero_base, vec4(0.0), pos_exp), p, pos_base);\n"
                      "}\n\n");
    }
    if (helper_mask & WWAL_HLP_OKLCH_TO_SRGB) {
        ab_append_lit(
            sb, "vec3 wwal_linear_to_srgb(vec3 lin) {\n"
                "    vec3 c = clamp(lin, 0.0, 1.0);\n"
                "    vec3 lo = c * 12.92;\n"
                "    vec3 hi = 1.055 * pow(max(c, vec3(0.0031308)), vec3(1.0 / 2.4)) - 0.055;\n"
                "    return clamp(mix(hi, lo, lessThanEqual(c, vec3(0.0031308))), 0.0, 1.0);\n"
                "}\n\n"
                "vec3 wwal_oklch_to_srgb(vec4 oklch) {\n"
                "    float L = oklch.x, C = oklch.y, h = oklch.z;\n"
                "    float a = C * cos(h), b = C * sin(h);\n"
                "    float l_ = L + 0.3963377774 * a + 0.2158037573 * b;\n"
                "    float m_ = L - 0.1055613458 * a - 0.0638541728 * b;\n"
                "    float s_ = L - 0.0894841775 * a - 1.2914855480 * b;\n"
                "    float l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;\n"
                "    float r = +4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;\n"
                "    float g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;\n"
                "    float bl = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;\n"
                "    return wwal_linear_to_srgb(vec3(r, g, bl));\n"
                "}\n\n");
    }
    if (helper_mask & WWAL_HLP_SRGB_TO_OKLCH) {
        ab_append_lit(
            sb, "vec3 wwal_srgb_to_linear(vec3 srgb) {\n"
                "    vec3 c = max(srgb, vec3(0.0));\n"
                "    vec3 lo = c / 12.92;\n"
                "    vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));\n"
                "    return mix(hi, lo, lessThanEqual(c, vec3(0.04045)));\n"
                "}\n\n"
                "vec3 wwal_srgb_to_oklch(vec4 srgb) {\n"
                "    vec3 c = wwal_srgb_to_linear(srgb.rgb);\n"
                "    float l = 0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b;\n"
                "    float m = 0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b;\n"
                "    float s = 0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b;\n"
                "    float l_ = pow(max(l, 0.0), 0.3333333333);\n"
                "    float m_ = pow(max(m, 0.0), 0.3333333333);\n"
                "    float s_ = pow(max(s, 0.0), 0.3333333333);\n"
                "    float L = 0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_;\n"
                "    float a = 1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_;\n"
                "    float b = 0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_;\n"
                "    float C = sqrt(max(a * a + b * b, 0.0));\n"
                "    float h = (abs(a) < 1e-6 && abs(b) < 1e-6) ? 0.0 : atan(b, a);\n"
                "    return vec3(L, C, h);\n"
                "}\n\n");
    }
    if (helper_mask & WWAL_HLP_HASH21) {
        ab_append_lit(sb, "float wwal_hash21(vec2 p) {\n"
                          "    vec3 p3 = fract(vec3(p.xyx) * 0.1031);\n"
                          "    p3 += dot(p3, p3.yzx + 33.33);\n"
                          "    return fract((p3.x + p3.y) * p3.z);\n"
                          "}\n\n");
    }
    if (helper_mask & WWAL_HLP_SIMPLEX2D) {
        ab_append_lit(sb,
                      "float wwal_simplex2d(vec2 p) {\n"
                      "    const float K1 = 0.366025404;\n"
                      "    const float K2 = 0.211324865;\n"
                      "    vec2 i = floor(p + (p.x + p.y) * K1);\n"
                      "    vec2 a = p - i + (i.x + i.y) * K2;\n"
                      "    float m = step(a.y, a.x);\n"
                      "    vec2 o = vec2(m, 1.0 - m);\n"
                      "    vec2 b = a - o + K2;\n"
                      "    vec2 c = a - 1.0 + 2.0 * K2;\n"
                      "    vec3 h = max(0.5 - vec3(dot(a, a), dot(b, b), dot(c, c)), 0.0);\n"
                      "    vec3 n = h * h * h * h * vec3(dot(a, vec2(wwal_hash21(i) - 0.5, "
                      "wwal_hash21(i + 13.0) - 0.5)),\n"
                      "                                  dot(b, vec2(wwal_hash21(i + o) - 0.5, "
                      "wwal_hash21(i + o + 13.0) - 0.5)),\n"
                      "                                  dot(c, vec2(wwal_hash21(i + 1.0) - 0.5, "
                      "wwal_hash21(i + 14.0) - 0.5)));\n"
                      "    return dot(n, vec3(70.0));\n"
                      "}\n\n");
    }
    if (helper_mask & WWAL_HLP_CURL2D) {
        ab_append_lit(sb, "vec2 wwal_curl2d(vec2 p, float t) {\n"
                          "    const float eps = 0.005;\n"
                          "    float n1 = wwal_simplex2d(p + vec2(0.0, eps) + vec2(t * 0.1));\n"
                          "    float n2 = wwal_simplex2d(p - vec2(0.0, eps) + vec2(t * 0.1));\n"
                          "    float n3 = wwal_simplex2d(p + vec2(eps, 0.0) + vec2(t * 0.1));\n"
                          "    float n4 = wwal_simplex2d(p - vec2(eps, 0.0) + vec2(t * 0.1));\n"
                          "    float dy = (n1 - n2) / (2.0 * eps);\n"
                          "    float dx = (n3 - n4) / (2.0 * eps);\n"
                          "    return vec2(-dy, dx);\n"
                          "}\n\n");
    }
    if (helper_mask & WWAL_HLP_VORONOI2D) {
        ab_append_lit(sb,
                      "vec2 wwal_voronoi2d(vec2 p) {\n"
                      "    vec2 n = floor(p);\n"
                      "    vec2 f = fract(p);\n"
                      "    float m_dist = 8.0;\n"
                      "    vec2 m_point = vec2(0.0);\n"
                      "    for (int j = -1; j <= 1; ++j) {\n"
                      "        for (int i = -1; i <= 1; ++i) {\n"
                      "            vec2 g = vec2(float(i), float(j));\n"
                      "            vec2 o = vec2(wwal_hash21(n + g), wwal_hash21(n + g + 17.0));\n"
                      "            vec2 diff = g + o - f;\n"
                      "            float d = dot(diff, diff);\n"
                      "            if (d < m_dist) {\n"
                      "                m_dist = d;\n"
                      "                m_point = n + g;\n"
                      "            }\n"
                      "        }\n"
                      "    }\n"
                      "    return vec2(sqrt(m_dist), length(m_point));\n"
                      "}\n\n");
    }
    if (helper_mask & WWAL_HLP_FBM2D) {
        ab_append_lit(sb, "float wwal_fbm2d(vec2 p, int octaves, float lacunarity) {\n"
                          "    float val = 0.0;\n"
                          "    float amp = 0.5;\n"
                          "    float freq = 1.0;\n"
                          "    for (int i = 0; i < octaves && i < 8; ++i) {\n"
                          "        val += amp * wwal_simplex2d(p * freq);\n"
                          "        freq *= lacunarity;\n"
                          "        amp *= 0.5;\n"
                          "    }\n"
                          "    return val;\n"
                          "}\n\n");
    }
    if (helper_mask & WWAL_HLP_SDF_BOX) {
        ab_append_lit(sb, "float wwal_sdf_box(vec2 p, vec2 b) {\n"
                          "    vec2 d = abs(p) - b;\n"
                          "    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);\n"
                          "}\n\n");
    }
    if (helper_mask & WWAL_HLP_UV_POLAR) {
        ab_append_lit(sb, "vec2 wwal_uv_polar(vec2 uv, vec2 center) {\n"
                          "    vec2 d = uv - center;\n"
                          "    float r = length(d);\n"
                          "    float a = atan(d.y, d.x) / 6.28318530718 + 0.5;\n"
                          "    return vec2(a, r);\n"
                          "}\n\n");
    }
    if (helper_mask & WWAL_HLP_UV_TWIRL) {
        ab_append_lit(sb,
                      "vec2 wwal_uv_twirl(vec2 uv, vec2 center, float strength, float radius) {\n"
                      "    vec2 d = uv - center;\n"
                      "    float dist = length(d);\n"
                      "    float r_safe = max(radius, 1e-4);\n"
                      "    if (dist < r_safe) {\n"
                      "        float percent = (r_safe - dist) / r_safe;\n"
                      "        float theta = percent * percent * strength;\n"
                      "        float s = sin(theta), c = cos(theta);\n"
                      "        d = vec2(dot(d, vec2(c, -s)), dot(d, vec2(s, c)));\n"
                      "    }\n"
                      "    return center + d;\n"
                      "}\n\n");
    }
    if (helper_mask & WWAL_HLP_HUE_SHIFT) {
        ab_append_lit(sb, "vec4 wwal_hue_shift(vec4 col, float shift) {\n"
                          "    vec3 oklch = wwal_srgb_to_oklch(col);\n"
                          "    oklch.z += shift;\n"
                          "    return vec4(wwal_oklch_to_srgb(vec4(oklch, 1.0)), col.a);\n"
                          "}\n\n");
    }
    if (helper_mask & WWAL_HLP_CONTRAST_SAT) {
        ab_append_lit(sb, "vec4 wwal_contrast_saturation(vec4 col, float c, float s) {\n"
                          "    vec3 rgb = (col.rgb - 0.5) * c + 0.5;\n"
                          "    float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));\n"
                          "    rgb = mix(vec3(luma), rgb, s);\n"
                          "    return vec4(clamp(rgb, 0.0, 1.0), col.a);\n"
                          "}\n\n");
    }
    if (helper_mask & WWAL_HLP_DUAL_KAWASE) {
        ab_append_lit(
            sb, "vec4 wwal_dual_kawase(sampler2D tex, vec2 uv, float offset) {\n"
                "    vec2 halfpixel = max(offset, 0.0) / max(u_Resolution, vec2(1.0));\n"
                "    vec4 sum = texture(tex, uv) * 4.0;\n"
                "    sum += texture(tex, clamp(uv - halfpixel.xy, vec2(0.0), vec2(1.0)));\n"
                "    sum += texture(tex, clamp(uv + halfpixel.xy, vec2(0.0), vec2(1.0)));\n"
                "    sum += texture(tex, clamp(uv + vec2(halfpixel.x, -halfpixel.y), vec2(0.0), "
                "vec2(1.0)));\n"
                "    sum += texture(tex, clamp(uv - vec2(halfpixel.x, -halfpixel.y), vec2(0.0), "
                "vec2(1.0)));\n"
                "    return sum * 0.125;\n"
                "}\n\n");
    }
    if (helper_mask & WWAL_HLP_CHROMATIC_ABERR) {
        ab_append_lit(sb,
                      "vec4 wwal_chromatic_aberration(sampler2D tex, vec2 uv, float intensity) {\n"
                      "    vec2 dir = (uv - vec2(0.5)) * intensity;\n"
                      "    float r = texture(tex, clamp(uv + dir, vec2(0.0), vec2(1.0))).r;\n"
                      "    vec4 center = texture(tex, uv);\n"
                      "    float b = texture(tex, clamp(uv - dir, vec2(0.0), vec2(1.0))).b;\n"
                      "    return vec4(r, center.g, b, center.a);\n"
                      "}\n\n");
    }
    if (helper_mask & WWAL_HLP_NORMAL_HEIGHT_TEX) {
        ab_append_lit(
            sb, "vec3 wwal_normal_from_height_tex(sampler2D tex, vec2 uv, float strength) {\n"
                "    vec2 texel = 1.0 / max(u_Resolution, vec2(1.0));\n"
                "    float hL = texture(tex, uv - vec2(texel.x, 0.0)).x;\n"
                "    float hR = texture(tex, uv + vec2(texel.x, 0.0)).x;\n"
                "    float hD = texture(tex, uv - vec2(0.0, texel.y)).x;\n"
                "    float hU = texture(tex, uv + vec2(0.0, texel.y)).x;\n"
                "    return normalize(vec3((hL - hR) * strength, (hD - hU) * strength, 1.0));\n"
                "}\n\n");
    }
    if (helper_mask & WWAL_HLP_NORMAL_HEIGHT) {
        ab_append_lit(sb, "vec3 wwal_normal_from_height(float h, vec2 uv, float strength) {\n"
                          "    float dx = dFdx(h + uv.x * 0.0) * strength;\n"
                          "    float dy = dFdy(h + uv.y * 0.0) * strength;\n"
                          "    return normalize(vec3(-dx, -dy, 1.0));\n"
                          "}\n\n");
    }
    if (helper_mask & WWAL_HLP_BLINN_PHONG) {
        ab_append_lit(
            sb, "vec4 wwal_blinn_phong(vec3 n, vec3 l, vec3 v, vec4 albedo, float shininess) {\n"
                "    vec3 nn = normalize(n);\n"
                "    vec3 nl = normalize(l);\n"
                "    vec3 nv = normalize(v);\n"
                "    vec3 h = normalize(nl + nv);\n"
                "    float diff = max(dot(nn, nl), 0.0);\n"
                "    float spec = pow(max(dot(nn, h), 0.0), max(shininess, 1.0));\n"
                "    vec3 col = albedo.rgb * (0.15 + 0.85 * diff) + vec3(spec * 0.5);\n"
                "    return vec4(col, albedo.a);\n"
                "}\n\n");
    }
    if (helper_mask & WWAL_HLP_FX_WATERRIPPLE) {
        ab_append_lit(
            sb,
            "vec4 wwal_effect_waterripple(sampler2D tex, vec2 uv_base, vec2 uv_in, float speed, "
            "float amp, float freq) {\n"
            "    vec2 p = (uv_base - vec2(0.5)) + uv_in * 0.05;\n"
            "    float dist = length(p);\n"
            "    vec2 dir = (dist > 1e-5) ? (p / dist) : vec2(0.0, 1.0);\n"
            "    float wave = sin(dist * max(freq, 1.0) - u_Time * speed);\n"
            "    vec2 disp = (dir * wave + uv_in * 0.08) * amp;\n"
            "    vec4 base = texture(tex, clamp(uv_base + disp, vec2(0.0), vec2(1.0)));\n"
            "    vec3 caustic = vec3(0.08, 0.52, 0.92) * (0.5 + 0.5 * wave) * clamp(abs(amp) * "
            "10.0, 0.12, 0.85);\n"
            "    return vec4(clamp(base.rgb + caustic, 0.0, 1.0), 1.0);\n"
            "}\n\n");
    }
    if (helper_mask & WWAL_HLP_FX_WATERWAVES) {
        ab_append_lit(
            sb,
            "vec4 wwal_effect_waterwaves(sampler2D tex, vec2 uv, float speed, float amp, float "
            "freq) {\n"
            "    float f = max(freq, 1.0);\n"
            "    vec2 disp = vec2(sin(uv.y * f + u_Time * speed), cos(uv.x * f * 0.85 - u_Time * "
            "speed * 0.9)) * amp;\n"
            "    vec4 base = texture(tex, clamp(uv + disp, vec2(0.0), vec2(1.0)));\n"
            "    float crest = 0.5 + 0.5 * sin((uv.x + uv.y) * f - u_Time * speed);\n"
            "    return vec4(clamp(base.rgb + vec3(0.04, 0.10, 0.16) * crest, 0.0, 1.0), 1.0);\n"
            "}\n\n");
    }
    if (helper_mask & WWAL_HLP_FX_CAS_SHARPEN) {
        ab_append_lit(
            sb,
            "vec4 wwal_effect_cas_sharpen(sampler2D tex, vec2 uv, float sharpness) {\n"
            "    vec2 texel = 1.0 / max(u_Resolution, vec2(1.0));\n"
            "    vec3 c = texture(tex, uv).rgb;\n"
            "    vec3 n = texture(tex, uv + vec2(0.0, -texel.y)).rgb;\n"
            "    vec3 s = texture(tex, uv + vec2(0.0,  texel.y)).rgb;\n"
            "    vec3 w = texture(tex, uv + vec2(-texel.x, 0.0)).rgb;\n"
            "    vec3 e = texture(tex, uv + vec2( texel.x, 0.0)).rgb;\n"
            "    vec3 mn = min(min(min(n, s), min(w, e)), c);\n"
            "    vec3 mx = max(max(max(n, s), max(w, e)), c);\n"
            "    vec3 amp = sqrt(clamp(min(mn, 2.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));\n"
            "    vec3 weight = -amp / mix(vec3(8.0), vec3(5.0), clamp(sharpness, 0.0, 1.0));\n"
            "    vec3 out_rgb = clamp((c + (n + s + w + e) * weight) / (1.0 + 4.0 * weight), 0.0, "
            "1.0);\n"
            "    return vec4(out_rgb, 1.0);\n"
            "}\n\n");
    }
    if (helper_mask & WWAL_HLP_FX_VIGNETTE) {
        ab_append_lit(
            sb,
            "vec4 wwal_effect_vignette(sampler2D tex, vec2 uv, float radius, float softness) {\n"
            "    vec4 c = texture(tex, uv);\n"
            "    float d = length(uv - vec2(0.5));\n"
            "    float r = (radius > 0.0) ? radius : 0.75;\n"
            "    float s = max(softness, 0.15);\n"
            "    float lo = max(r - s, 0.0);\n"
            "    float hi = max(r, lo + 1e-4);\n"
            "    float vig = 1.0 - smoothstep(lo, hi, d);\n"
            "    return vec4(c.rgb * vig, 1.0);\n"
            "}\n\n");
    }
    if (helper_mask & WWAL_HLP_FX_CRT_PIXELATE) {
        ab_append_lit(
            sb, "vec4 wwal_effect_crt_pixelate(sampler2D tex, vec2 uv, float density, float "
                "scan_strength) {\n"
                "    float cells = max(density, 64.0);\n"
                "    vec2 q_uv = (floor(uv * cells) + 0.5) / cells;\n"
                "    vec4 c = texture(tex, clamp(q_uv, vec2(0.0), vec2(1.0)));\n"
                "    float scan = 1.0 - clamp(scan_strength, 0.0, 0.75) * (0.5 + 0.5 * sin(uv.y * "
                "max(u_Resolution.y, 240.0) * 3.14159265));\n"
                "    return vec4(c.rgb * scan, 1.0);\n"
                "}\n\n");
    }
    if (helper_mask & WWAL_HLP_FX_GODRAYS) {
        ab_append_lit(
            sb, "vec4 wwal_effect_godrays(sampler2D tex, vec2 uv, float intensity, float decay) {\n"
                "    vec2 delta = (uv - vec2(0.5)) * 0.12;\n"
                "    vec2 cur = uv;\n"
                "    vec3 acc = vec3(0.0);\n"
                "    float w = 1.0;\n"
                "    float dec = clamp((decay > 0.0) ? decay : 0.85, 0.5, 0.98);\n"
                "    for (int i = 0; i < 4; ++i) {\n"
                "        cur = clamp(cur - delta, vec2(0.0), vec2(1.0));\n"
                "        acc += texture(tex, cur).rgb * w;\n"
                "        w *= dec;\n"
                "    }\n"
                "    vec3 base = texture(tex, uv).rgb;\n"
                "    return vec4(clamp(base + acc * 0.25 * max(intensity, 0.4), 0.0, 1.0), 1.0);\n"
                "}\n\n");
    }
}

static uint32_t pin_lane_count(uint32_t pin_type)
{
    switch (pin_type) {
    case WAYWAL_PIN_VEC2:
        return 2u;
    case WAYWAL_PIN_VEC3:
        return 3u;
    case WAYWAL_PIN_VEC4:
    case WAYWAL_PIN_AUDIO_SPECTRUM:
        return 4u;
    default:
        return 1u;
    }
}

static size_t coerce_glsl_expr_fast(uint32_t src_type, uint32_t dst_type, const char *raw_var,
                                    size_t raw_len, char *out_expr, size_t out_cap)
{
    uint32_t s_norm = (src_type == WAYWAL_PIN_AUDIO_SPECTRUM) ? WAYWAL_PIN_VEC4 : src_type;
    uint32_t d_norm = (dst_type == WAYWAL_PIN_AUDIO_SPECTRUM) ? WAYWAL_PIN_VEC4 : dst_type;

    stack_writer_t w = {.data = out_expr, .len = 0u, .cap = out_cap};
    if (s_norm == d_norm) {
        sw_append_n(&w, raw_var, raw_len);
        out_expr[w.len] = '\0';
        return w.len;
    }

    switch (d_norm) {
    case WAYWAL_PIN_FLOAT:
        if (s_norm == WAYWAL_PIN_INT) {
            sw_append_lit(&w, "float(");
            sw_append_n(&w, raw_var, raw_len);
            sw_append_lit(&w, ")");
        } else if (s_norm == WAYWAL_PIN_BOOL) {
            sw_append_lit(&w, "(");
            sw_append_n(&w, raw_var, raw_len);
            sw_append_lit(&w, " ? 1.0 : 0.0)");
        } else {
            sw_append_lit(&w, "(");
            sw_append_n(&w, raw_var, raw_len);
            sw_append_lit(&w, ").x");
        }
        break;
    case WAYWAL_PIN_INT:
        if (s_norm == WAYWAL_PIN_FLOAT) {
            sw_append_lit(&w, "int(");
            sw_append_n(&w, raw_var, raw_len);
            sw_append_lit(&w, ")");
        } else if (s_norm == WAYWAL_PIN_BOOL) {
            sw_append_lit(&w, "(");
            sw_append_n(&w, raw_var, raw_len);
            sw_append_lit(&w, " ? 1 : 0)");
        } else {
            sw_append_lit(&w, "int((");
            sw_append_n(&w, raw_var, raw_len);
            sw_append_lit(&w, ").x)");
        }
        break;
    case WAYWAL_PIN_BOOL:
        sw_append_lit(&w, "((");
        sw_append_n(&w, raw_var, raw_len);
        if (s_norm == WAYWAL_PIN_INT) {
            sw_append_lit(&w, ") != 0)");
        } else if (s_norm == WAYWAL_PIN_FLOAT) {
            sw_append_lit(&w, ") != 0.0)");
        } else {
            sw_append_lit(&w, ").x != 0.0)");
        }
        break;
    case WAYWAL_PIN_VEC2:
        if (s_norm == WAYWAL_PIN_FLOAT) {
            sw_append_lit(&w, "vec2(");
            sw_append_n(&w, raw_var, raw_len);
            sw_append_lit(&w, ")");
        } else if (s_norm == WAYWAL_PIN_INT) {
            sw_append_lit(&w, "vec2(float(");
            sw_append_n(&w, raw_var, raw_len);
            sw_append_lit(&w, "))");
        } else if (s_norm == WAYWAL_PIN_BOOL) {
            sw_append_lit(&w, "vec2(");
            sw_append_n(&w, raw_var, raw_len);
            sw_append_lit(&w, " ? 1.0 : 0.0)");
        } else {
            sw_append_lit(&w, "(");
            sw_append_n(&w, raw_var, raw_len);
            sw_append_lit(&w, ").xy");
        }
        break;
    case WAYWAL_PIN_VEC3:
        if (s_norm == WAYWAL_PIN_FLOAT) {
            sw_append_lit(&w, "vec3(");
            sw_append_n(&w, raw_var, raw_len);
            sw_append_lit(&w, ")");
        } else if (s_norm == WAYWAL_PIN_INT) {
            sw_append_lit(&w, "vec3(float(");
            sw_append_n(&w, raw_var, raw_len);
            sw_append_lit(&w, "))");
        } else if (s_norm == WAYWAL_PIN_BOOL) {
            sw_append_lit(&w, "vec3(");
            sw_append_n(&w, raw_var, raw_len);
            sw_append_lit(&w, " ? 1.0 : 0.0)");
        } else if (s_norm == WAYWAL_PIN_VEC2) {
            sw_append_lit(&w, "vec3(");
            sw_append_n(&w, raw_var, raw_len);
            sw_append_lit(&w, ", 0.0)");
        } else {
            sw_append_lit(&w, "(");
            sw_append_n(&w, raw_var, raw_len);
            sw_append_lit(&w, ").xyz");
        }
        break;
    case WAYWAL_PIN_VEC4:
        if (s_norm == WAYWAL_PIN_FLOAT) {
            sw_append_lit(&w, "vec4(vec3(");
            sw_append_n(&w, raw_var, raw_len);
            sw_append_lit(&w, "), 1.0)");
        } else if (s_norm == WAYWAL_PIN_INT) {
            sw_append_lit(&w, "vec4(vec3(float(");
            sw_append_n(&w, raw_var, raw_len);
            sw_append_lit(&w, ")), 1.0)");
        } else if (s_norm == WAYWAL_PIN_BOOL) {
            sw_append_lit(&w, "vec4(vec3(");
            sw_append_n(&w, raw_var, raw_len);
            sw_append_lit(&w, " ? 1.0 : 0.0), 1.0)");
        } else if (s_norm == WAYWAL_PIN_VEC2) {
            sw_append_lit(&w, "vec4(");
            sw_append_n(&w, raw_var, raw_len);
            sw_append_lit(&w, ", 0.0, 1.0)");
        } else {
            sw_append_lit(&w, "vec4(");
            sw_append_n(&w, raw_var, raw_len);
            sw_append_lit(&w, ", 1.0)");
        }
        break;
    default:
        sw_append_n(&w, raw_var, raw_len);
        break;
    }
    out_expr[w.len] = '\0';
    return w.len;
}

static size_t format_operand_typed_fast(const waywal_shader_graph_t *graph, const graph_adj_t *adj,
                                        const uint32_t *node_pass, const bool *emitted_in_pass,
                                        const uint16_t *canon_idx, uint32_t dst_idx,
                                        uint32_t pin_idx, uint32_t dst_type, char *out_expr,
                                        size_t out_cap)
{
    const waywal_graph_node_t *dst_node = &graph->nodes[dst_idx];
    if (pin_idx >= dst_node->input_pin_count || pin_idx >= WAYWAL_GRAPH_MAX_PINS_PER_NODE) {
        memcpy(out_expr, "0.0", 4u);
        return 3u;
    }

    const waywal_graph_pin_t *dst_pin = &dst_node->inputs[pin_idx];
    int16_t e_idx = adj->pin_in_edge[dst_idx][pin_idx];

    if (e_idx >= 0) {
        const waywal_graph_edge_t *incoming_edge = &graph->edges[e_idx];
        int16_t s_idx = adj->edge_src_idx[e_idx];
        if (s_idx >= 0) {
            const waywal_graph_node_t *src_node = &graph->nodes[s_idx];
            if (incoming_edge->src_pin_index < src_node->output_pin_count) {
                uint32_t src_type = src_node->outputs[incoming_edge->src_pin_index].pin_type;
                char raw_var[80];
                stack_writer_t rw = {.data = raw_var, .len = 0u, .cap = sizeof(raw_var)};

                if (node_pass[s_idx] != node_pass[dst_idx] || !emitted_in_pass[s_idx]) {
                    const char *buf_name =
                        resolve_pass_buffer_sampler((src_node->opcode == WAYWAL_NODE_IN_PASS_BUFFER)
                                                        ? src_node->prop_or_asset_key
                                                        : NULL,
                                                    node_pass[s_idx]);
                    sw_append_lit(&rw, "texture(");
                    sw_append_n(&rw, buf_name, strlen(buf_name));
                    sw_append_lit(&rw, ", v_TexCoord)");
                    src_type = WAYWAL_PIN_VEC4;
                } else {
                    uint16_t real_s = canon_idx[s_idx];
                    sw_append_lit(&rw, "n");
                    sw_append_u32(&rw, graph->nodes[real_s].node_id);
                    sw_append_lit(&rw, "_out");
                    sw_append_u32(&rw, incoming_edge->src_pin_index);
                }
                raw_var[rw.len] = '\0';

                uint32_t src_lanes = pin_lane_count(src_type);
                if (incoming_edge->swizzle_mask != 0u && src_lanes >= 2u) {
                    static const char k_swz_chars[4] = {'x', 'y', 'z', 'w'};
                    uint32_t swz_lanes = pin_lane_count(dst_type);
                    char swizzled_var[96];
                    stack_writer_t swz_w = {
                        .data = swizzled_var, .len = 0u, .cap = sizeof(swizzled_var)};
                    sw_append_lit(&swz_w, "(");
                    sw_append_n(&swz_w, raw_var, rw.len);
                    sw_append_lit(&swz_w, ").");
                    for (uint32_t i = 0; i < swz_lanes; ++i) {
                        uint32_t lane = (incoming_edge->swizzle_mask >> (i * 2u)) & 3u;
                        if (lane >= src_lanes)
                            lane = src_lanes - 1u;
                        sw_append_n(&swz_w, &k_swz_chars[lane], 1u);
                    }
                    swizzled_var[swz_w.len] = '\0';
                    uint32_t swz_type = (swz_lanes == 1u)   ? WAYWAL_PIN_FLOAT
                                        : (swz_lanes == 2u) ? WAYWAL_PIN_VEC2
                                        : (swz_lanes == 3u) ? WAYWAL_PIN_VEC3
                                                            : WAYWAL_PIN_VEC4;
                    return coerce_glsl_expr_fast(swz_type, dst_type, swizzled_var, swz_w.len,
                                                 out_expr, out_cap);
                }
                return coerce_glsl_expr_fast(src_type, dst_type, raw_var, rw.len, out_expr,
                                             out_cap);
            }
        }
    }

    char def_expr[128];
    stack_writer_t dw = {.data = def_expr, .len = 0u, .cap = sizeof(def_expr)};
    switch (dst_pin->pin_type) {
    case WAYWAL_PIN_FLOAT:
        sw_append_f32(&dw, finite_or_zero(dst_pin->default_val[0]));
        break;
    case WAYWAL_PIN_VEC2:
        if (dst_node->opcode == WAYWAL_NODE_IN_TEXTURE2D ||
            dst_node->opcode == WAYWAL_NODE_IN_PASS_BUFFER ||
            dst_node->opcode == WAYWAL_NODE_IN_DEPTH_MAP || strcasecmp(dst_pin->name, "uv") == 0) {
            sw_append_lit(&dw, "v_TexCoord");
        } else {
            sw_append_lit(&dw, "vec2(");
            sw_append_f32(&dw, finite_or_zero(dst_pin->default_val[0]));
            sw_append_lit(&dw, ", ");
            sw_append_f32(&dw, finite_or_zero(dst_pin->default_val[1]));
            sw_append_lit(&dw, ")");
        }
        break;
    case WAYWAL_PIN_VEC3:
        sw_append_lit(&dw, "vec3(");
        sw_append_f32(&dw, finite_or_zero(dst_pin->default_val[0]));
        sw_append_lit(&dw, ", ");
        sw_append_f32(&dw, finite_or_zero(dst_pin->default_val[1]));
        sw_append_lit(&dw, ", ");
        sw_append_f32(&dw, finite_or_zero(dst_pin->default_val[2]));
        sw_append_lit(&dw, ")");
        break;
    case WAYWAL_PIN_VEC4:
    case WAYWAL_PIN_AUDIO_SPECTRUM:
        sw_append_lit(&dw, "vec4(");
        sw_append_f32(&dw, finite_or_zero(dst_pin->default_val[0]));
        sw_append_lit(&dw, ", ");
        sw_append_f32(&dw, finite_or_zero(dst_pin->default_val[1]));
        sw_append_lit(&dw, ", ");
        sw_append_f32(&dw, finite_or_zero(dst_pin->default_val[2]));
        sw_append_lit(&dw, ", ");
        sw_append_f32(&dw, finite_or_zero(dst_pin->default_val[3]));
        sw_append_lit(&dw, ")");
        break;
    case WAYWAL_PIN_INT:
        sw_append_i32(&dw, safe_f32_to_i32(dst_pin->default_val[0]));
        break;
    case WAYWAL_PIN_BOOL:
        if (finite_or_zero(dst_pin->default_val[0]) != 0.0f) {
            sw_append_lit(&dw, "true");
        } else {
            sw_append_lit(&dw, "false");
        }
        break;
    case WAYWAL_PIN_SAMPLER2D:
        memcpy(out_expr, "u_BufferA", 10u);
        return 9u;
    default:
        memcpy(out_expr, "0.0", 4u);
        return 3u;
    }
    def_expr[dw.len] = '\0';
    return coerce_glsl_expr_fast(dst_pin->pin_type, dst_type, def_expr, dw.len, out_expr, out_cap);
}

static size_t format_operand_fast(const waywal_shader_graph_t *graph, const graph_adj_t *adj,
                                  const uint32_t *node_pass, const bool *emitted_in_pass,
                                  const uint16_t *canon_idx, uint32_t dst_idx, uint32_t pin_idx,
                                  char *out_expr, size_t out_cap)
{
    const waywal_graph_node_t *dst_node = &graph->nodes[dst_idx];
    if (pin_idx >= dst_node->input_pin_count || pin_idx >= WAYWAL_GRAPH_MAX_PINS_PER_NODE) {
        memcpy(out_expr, "0.0", 4u);
        return 3u;
    }
    return format_operand_typed_fast(graph, adj, node_pass, emitted_in_pass, canon_idx, dst_idx,
                                     pin_idx, dst_node->inputs[pin_idx].pin_type, out_expr,
                                     out_cap);
}

static void emit_muted_bypass_node(arena_str_t *sb, const waywal_shader_graph_t *graph,
                                   const graph_adj_t *adj, const uint32_t *node_pass,
                                   const bool *emitted_in_pass, const uint16_t *canon_idx,
                                   uint32_t idx)
{
    const waywal_graph_node_t *node = &graph->nodes[idx];
    uint32_t nid = node->node_id;
    uint32_t out_cnt = (node->output_pin_count <= WAYWAL_GRAPH_MAX_PINS_PER_NODE)
                           ? node->output_pin_count
                           : WAYWAL_GRAPH_MAX_PINS_PER_NODE;

    for (uint32_t out_p = 0; out_p < out_cnt; ++out_p) {
        uint32_t out_type = node->outputs[out_p].pin_type;
        const char *glsl_type = "vec4";
        const char *zero_lit = "vec4(0.0, 0.0, 0.0, 1.0)";
        switch (out_type) {
        case WAYWAL_PIN_FLOAT:
            glsl_type = "float";
            zero_lit = "0.0";
            break;
        case WAYWAL_PIN_VEC2:
            glsl_type = "vec2";
            zero_lit = "vec2(0.0)";
            break;
        case WAYWAL_PIN_VEC3:
            glsl_type = "vec3";
            zero_lit = "vec3(0.0)";
            break;
        case WAYWAL_PIN_INT:
            glsl_type = "int";
            zero_lit = "0";
            break;
        case WAYWAL_PIN_BOOL:
            glsl_type = "bool";
            zero_lit = "false";
            break;
        default:
            break;
        }
        ab_append_lit(sb, "    ");
        ab_append_cstr(sb, glsl_type);
        ab_append_lit(sb, " n");
        ab_append_u32(sb, nid);
        ab_append_lit(sb, "_out");
        ab_append_u32(sb, out_p);
        ab_append_lit(sb, " = ");
        if (node->input_pin_count > 0) {
            char expr[160];
            size_t elen =
                format_operand_typed_fast(graph, adj, node_pass, emitted_in_pass, canon_idx, idx, 0,
                                          out_type, expr, sizeof(expr));
            ab_append_n(sb, expr, elen);
        } else {
            ab_append_cstr(sb, zero_lit);
        }
        ab_append_lit(sb, ";\n");
    }
}

typedef struct {
    const char *s;
    size_t len;
} op_view_t;

static void emit_builtin_effect_node(arena_str_t *sb, const waywal_graph_node_t *node,
                                     const char *sampler, op_view_t in0, op_view_t in1,
                                     op_view_t in2, op_view_t in3)
{
    uint32_t nid = node->node_id;
    const char *key = node->prop_or_asset_key;

    /* Strip control characters, non-ASCII bytes, and trailing-comment-continuation backslashes so
     * untrusted keys cannot escape the single-line GLSL comment into executable code. */
    char safe_key[128];
    size_t sk_len = 0;
    for (size_t i = 0; key[i] != '\0' && sk_len + 1u < sizeof(safe_key); ++i) {
        unsigned char c = (unsigned char)key[i];
        if (c >= 0x20u && c < 0x7fu && c != '\\') {
            safe_key[sk_len++] = (char)c;
        }
    }

    ab_append_lit(sb, "    // Builtin Effect: ");
    if (sk_len > 0u) {
        ab_append_n(sb, safe_key, sk_len);
    } else {
        ab_append_lit(sb, "waterripple");
    }
    ab_append_lit(sb, "\n    vec4 n");
    ab_append_u32(sb, nid);
    ab_append_lit(sb, "_out0 = ");

    size_t smp_len = strlen(sampler);
    if (key[0] == '\0' || key_contains_ci(key, "waterripple") || key_contains_ci(key, "ripple")) {
        ab_append_lit(sb, "wwal_effect_waterripple(");
        ab_append_n(sb, sampler, smp_len);
        ab_append_lit(sb, ", v_TexCoord, (");
        ab_append_n(sb, in0.s, in0.len);
        ab_append_lit(sb, ").xy, ");
        ab_append_n(sb, in1.s, in1.len);
        ab_append_lit(sb, ", ");
        ab_append_n(sb, in2.s, in2.len);
        ab_append_lit(sb, ", ");
        ab_append_n(sb, in3.s, in3.len);
        ab_append_lit(sb, ");\n");
    } else if (key_contains_ci(key, "water") || key_contains_ci(key, "refract") ||
               key_contains_ci(key, "rain")) {
        ab_append_lit(sb, "wwal_effect_waterwaves(");
        ab_append_n(sb, sampler, smp_len);
        ab_append_lit(sb, ", (");
        ab_append_n(sb, in0.s, in0.len);
        ab_append_lit(sb, ").xy, ");
        ab_append_n(sb, in1.s, in1.len);
        ab_append_lit(sb, ", ");
        ab_append_n(sb, in2.s, in2.len);
        ab_append_lit(sb, ", ");
        ab_append_n(sb, in3.s, in3.len);
        ab_append_lit(sb, ");\n");
    } else if (key_contains_ci(key, "bloom") || key_contains_ci(key, "halo") ||
               key_contains_ci(key, "flare") || key_contains_ci(key, "shine")) {
        ab_append_lit(sb, "vec4(clamp(texture(");
        ab_append_n(sb, sampler, smp_len);
        ab_append_lit(sb, ", (");
        ab_append_n(sb, in0.s, in0.len);
        ab_append_lit(sb, ").xy).rgb + wwal_dual_kawase(");
        ab_append_n(sb, sampler, smp_len);
        ab_append_lit(sb, ", (");
        ab_append_n(sb, in0.s, in0.len);
        ab_append_lit(sb, ").xy, max(");
        ab_append_n(sb, in1.s, in1.len);
        ab_append_lit(sb, ", 1.5)).rgb * max(");
        ab_append_n(sb, in2.s, in2.len);
        ab_append_lit(sb, ", 0.5), 0.0, 1.0), 1.0);\n");
    } else if (key_contains_ci(key, "blur") || key_contains_ci(key, "depth_of_field")) {
        ab_append_lit(sb, "wwal_dual_kawase(");
        ab_append_n(sb, sampler, smp_len);
        ab_append_lit(sb, ", (");
        ab_append_n(sb, in0.s, in0.len);
        ab_append_lit(sb, ").xy, max(");
        ab_append_n(sb, in1.s, in1.len);
        ab_append_lit(sb, ", 1.5));\n");
    } else if (key_contains_ci(key, "chromatic") || key_contains_ci(key, "vhs")) {
        ab_append_lit(sb, "wwal_chromatic_aberration(");
        ab_append_n(sb, sampler, smp_len);
        ab_append_lit(sb, ", (");
        ab_append_n(sb, in0.s, in0.len);
        ab_append_lit(sb, ").xy, max(abs(");
        ab_append_n(sb, in1.s, in1.len);
        ab_append_lit(sb, ") * 0.02, 0.006));\n");
    } else if (key_contains_ci(key, "sharpen") || key_contains_ci(key, "edge") ||
               key_contains_ci(key, "outline")) {
        ab_append_lit(sb, "wwal_effect_cas_sharpen(");
        ab_append_n(sb, sampler, smp_len);
        ab_append_lit(sb, ", (");
        ab_append_n(sb, in0.s, in0.len);
        ab_append_lit(sb, ").xy, ");
        ab_append_n(sb, in1.s, in1.len);
        ab_append_lit(sb, ");\n");
    } else if (key_contains_ci(key, "vignette") || key_contains_ci(key, "iris")) {
        ab_append_lit(sb, "wwal_effect_vignette(");
        ab_append_n(sb, sampler, smp_len);
        ab_append_lit(sb, ", (");
        ab_append_n(sb, in0.s, in0.len);
        ab_append_lit(sb, ").xy, ");
        ab_append_n(sb, in1.s, in1.len);
        ab_append_lit(sb, ", ");
        ab_append_n(sb, in2.s, in2.len);
        ab_append_lit(sb, ");\n");
    } else if (key_contains_ci(key, "crt") || key_contains_ci(key, "pixelate") ||
               key_contains_ci(key, "halftone")) {
        ab_append_lit(sb, "wwal_effect_crt_pixelate(");
        ab_append_n(sb, sampler, smp_len);
        ab_append_lit(sb, ", (");
        ab_append_n(sb, in0.s, in0.len);
        ab_append_lit(sb, ").xy, ");
        ab_append_n(sb, in1.s, in1.len);
        ab_append_lit(sb, ", ");
        ab_append_n(sb, in2.s, in2.len);
        ab_append_lit(sb, ");\n");
    } else if (key_contains_ci(key, "twirl") || key_contains_ci(key, "fisheye") ||
               key_contains_ci(key, "pendulum") || key_contains_ci(key, "perspective")) {
        ab_append_lit(sb, "texture(");
        ab_append_n(sb, sampler, smp_len);
        ab_append_lit(sb, ", clamp(wwal_uv_twirl((");
        ab_append_n(sb, in0.s, in0.len);
        ab_append_lit(sb, ").xy, vec2(0.5), ");
        ab_append_n(sb, in1.s, in1.len);
        ab_append_lit(sb, ", max(");
        ab_append_n(sb, in2.s, in2.len);
        ab_append_lit(sb, ", 0.45)), vec2(0.0), vec2(1.0)));\n");
    } else if (key_contains_ci(key, "foliage") || key_contains_ci(key, "scroll") ||
               key_contains_ci(key, "shake") || key_contains_ci(key, "skew")) {
        ab_append_lit(sb, "texture(");
        ab_append_n(sb, sampler, smp_len);
        ab_append_lit(sb, ", clamp((");
        ab_append_n(sb, in0.s, in0.len);
        ab_append_lit(sb, ").xy + vec2(sin(u_Time * (");
        ab_append_n(sb, in1.s, in1.len);
        ab_append_lit(sb, ") + (");
        ab_append_n(sb, in0.s, in0.len);
        ab_append_lit(sb, ").y * (");
        ab_append_n(sb, in3.s, in3.len);
        ab_append_lit(sb, ")) * (");
        ab_append_n(sb, in2.s, in2.len);
        ab_append_lit(sb, "), 0.0), vec2(0.0), vec2(1.0)));\n");
    } else if (key_contains_ci(key, "pulse")) {
        ab_append_lit(sb, "texture(");
        ab_append_n(sb, sampler, smp_len);
        ab_append_lit(sb, ", clamp(vec2(0.5) + ((");
        ab_append_n(sb, in0.s, in0.len);
        ab_append_lit(sb, ").xy - vec2(0.5)) / (1.0 + u_AudioBands[0].x * max(");
        ab_append_n(sb, in1.s, in1.len);
        ab_append_lit(sb, ", 0.15)), vec2(0.0), vec2(1.0)));\n");
    } else if (key_contains_ci(key, "hsv") || key_contains_ci(key, "tint") ||
               key_contains_ci(key, "colorgrading")) {
        ab_append_lit(sb, "wwal_hue_shift(texture(");
        ab_append_n(sb, sampler, smp_len);
        ab_append_lit(sb, ", (");
        ab_append_n(sb, in0.s, in0.len);
        ab_append_lit(sb, ").xy), ");
        ab_append_n(sb, in1.s, in1.len);
        ab_append_lit(sb, ");\n");
    } else if (key_contains_ci(key, "contrast")) {
        ab_append_lit(sb, "wwal_contrast_saturation(texture(");
        ab_append_n(sb, sampler, smp_len);
        ab_append_lit(sb, ", (");
        ab_append_n(sb, in0.s, in0.len);
        ab_append_lit(sb, ").xy), max(");
        ab_append_n(sb, in1.s, in1.len);
        ab_append_lit(sb, ", 0.1), max(");
        ab_append_n(sb, in2.s, in2.len);
        ab_append_lit(sb, ", 0.0));\n");
    } else if (key_contains_ci(key, "godrays") || key_contains_ci(key, "lightshafts") ||
               key_contains_ci(key, "aurora") || key_contains_ci(key, "fire") ||
               key_contains_ci(key, "nitro") || key_contains_ci(key, "cloud")) {
        ab_append_lit(sb, "wwal_effect_godrays(");
        ab_append_n(sb, sampler, smp_len);
        ab_append_lit(sb, ", (");
        ab_append_n(sb, in0.s, in0.len);
        ab_append_lit(sb, ").xy, ");
        ab_append_n(sb, in1.s, in1.len);
        ab_append_lit(sb, ", ");
        ab_append_n(sb, in2.s, in2.len);
        ab_append_lit(sb, ");\n");
    } else {
        ab_append_lit(sb, "wwal_effect_waterwaves(");
        ab_append_n(sb, sampler, smp_len);
        ab_append_lit(sb, ", (");
        ab_append_n(sb, in0.s, in0.len);
        ab_append_lit(sb, ").xy, ");
        ab_append_n(sb, in1.s, in1.len);
        ab_append_lit(sb, ", ");
        ab_append_n(sb, in2.s, in2.len);
        ab_append_lit(sb, ", ");
        ab_append_n(sb, in3.s, in3.len);
        ab_append_lit(sb, ");\n");
    }
}

/* Pure ALU/procedural opcodes safe for intra-pass SSA Global Value Numbering. */
static inline bool is_pure_gvn_op(uint32_t opcode)
{
    if (opcode == WAYWAL_NODE_IN_UV || opcode == WAYWAL_NODE_IN_TIME ||
        opcode == WAYWAL_NODE_IN_RESOLUTION || opcode == WAYWAL_NODE_IN_POINTER ||
        opcode == WAYWAL_NODE_IN_AUDIO_FFT) {
        return true;
    }
    if (opcode >= WAYWAL_NODE_MATH_ADD && opcode <= WAYWAL_NODE_COLOR_CONTRAST_SAT) {
        return true;
    }
    return (opcode == WAYWAL_NODE_LIGHT_BLINN_PHONG);
}

typedef struct {
    uint32_t opcode;
    uint32_t max_ops;
    uint32_t kind[5];
    uint32_t w0[5];
    uint32_t w1[5];
    uint32_t w2[5];
    uint32_t w3[5];
} gvn_key_t;

typedef struct {
    uint64_t hash;
    uint16_t node_idx;
    bool occupied;
} gvn_slot_t;

static inline uint32_t f32_norm_bits(float f)
{
    float v = finite_or_zero(f);
    if (v == 0.0f) {
        v = 0.0f; /* Canonicalize -0.0f to +0.0f */
    }
    uint32_t u = 0;
    memcpy(&u, &v, sizeof(u));
    return u;
}

static inline uint64_t gvn_hash_key(const gvn_key_t *k)
{
    uint64_t h = 14695981039346656037ULL;
    const uint32_t *words = (const uint32_t *)k;
    size_t n = sizeof(*k) / sizeof(uint32_t);
    for (size_t i = 0; i < n; ++i) {
        h ^= (uint64_t)words[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void gvn_build_key(const waywal_shader_graph_t *graph, const graph_adj_t *adj,
                          const uint32_t *node_pass, const bool *emitted_in_pass,
                          const uint16_t *canon_idx, uint32_t idx, uint32_t pass, gvn_key_t *out)
{
    memset(out, 0, sizeof(*out));
    const waywal_graph_node_t *node = &graph->nodes[idx];
    uint32_t max_ops = (node->input_pin_count < 5u) ? node->input_pin_count : 5u;
    out->opcode = node->opcode;
    out->max_ops = max_ops;

    for (uint32_t p = 0; p < max_ops; ++p) {
        const waywal_graph_pin_t *pin = &node->inputs[p];
        int16_t e_idx = adj->pin_in_edge[idx][p];
        int16_t s_idx = (e_idx >= 0) ? adj->edge_src_idx[e_idx] : -1;
        if (e_idx >= 0 && s_idx >= 0 &&
            graph->edges[e_idx].src_pin_index < graph->nodes[s_idx].output_pin_count) {
            const waywal_graph_edge_t *edge = &graph->edges[e_idx];
            if (node_pass[s_idx] == pass && emitted_in_pass[s_idx]) {
                out->kind[p] = 1u;
                out->w0[p] = (uint32_t)canon_idx[s_idx];
                out->w1[p] = edge->src_pin_index;
                out->w2[p] = edge->swizzle_mask;
                out->w3[p] = pin->pin_type;
            } else {
                out->kind[p] = 2u;
                out->w0[p] = (uint32_t)s_idx;
                out->w1[p] = node_pass[s_idx];
                out->w2[p] = edge->swizzle_mask;
                out->w3[p] = pin->pin_type;
            }
        } else if (pin->pin_type == WAYWAL_PIN_VEC2 && strcasecmp(pin->name, "uv") == 0) {
            out->kind[p] = 3u;
            out->w0[p] = pin->pin_type;
        } else {
            out->kind[p] = 4u;
            out->w0[p] = f32_norm_bits(pin->default_val[0]);
            out->w1[p] = f32_norm_bits(pin->default_val[1]);
            out->w2[p] = f32_norm_bits(pin->default_val[2]);
            out->w3[p] = f32_norm_bits(pin->default_val[3]);
        }
    }

    if ((node->opcode == WAYWAL_NODE_MATH_ADD || node->opcode == WAYWAL_NODE_MATH_MUL ||
         node->opcode == WAYWAL_NODE_MATH_DOT) &&
        max_ops >= 2u) {
        uint32_t a[5] = {out->kind[0], out->w0[0], out->w1[0], out->w2[0], out->w3[0]};
        uint32_t b[5] = {out->kind[1], out->w0[1], out->w1[1], out->w2[1], out->w3[1]};
        bool swap = false;
        for (int c = 0; c < 5; ++c) {
            if (a[c] != b[c]) {
                swap = (b[c] < a[c]);
                break;
            }
        }
        if (swap) {
            out->kind[0] = b[0];
            out->w0[0] = b[1];
            out->w1[0] = b[2];
            out->w2[0] = b[3];
            out->w3[0] = b[4];
            out->kind[1] = a[0];
            out->w0[1] = a[1];
            out->w1[1] = a[2];
            out->w2[1] = a[3];
            out->w3[1] = a[4];
        }
    }
}

char *waywal_shader_graph_compile_glsl(const waywal_shader_graph_t *graph,
                                       const waywal_graph_schedule_t *schedule,
                                       size_t *out_glsl_len)
{
    if (!graph || !schedule) {
        if (out_glsl_len)
            *out_glsl_len = 0;
        return NULL;
    }

    uint32_t ncnt =
        (graph->node_count <= WAYWAL_GRAPH_MAX_NODES) ? graph->node_count : WAYWAL_GRAPH_MAX_NODES;
    uint32_t sorted_cnt = (schedule->sorted_count <= WAYWAL_GRAPH_MAX_NODES)
                              ? schedule->sorted_count
                              : WAYWAL_GRAPH_MAX_NODES;

    graph_adj_t adj;
    graph_adj_build(graph, &adj);

    bool reachable[WAYWAL_GRAPH_MAX_NODES];
    uint32_t node_pass[WAYWAL_GRAPH_MAX_NODES];
    compute_node_passes_readonly(graph, &adj, reachable, node_pass, NULL);

    wwal_bump_arena_t arena;
    wwal_bump_arena_init_compile(&arena);

    gvn_slot_t *gvn_table = (gvn_slot_t *)wwal_bump_alloc(&arena, 512u * sizeof(gvn_slot_t), 16u);
    gvn_key_t *gvn_keys =
        (gvn_key_t *)wwal_bump_alloc(&arena, WAYWAL_GRAPH_MAX_NODES * sizeof(gvn_key_t), 16u);

    arena_str_t sb;
    size_t est_cap = (size_t)ncnt * 144u + 4096u;
    if (est_cap < 16384u) {
        est_cap = 16384u;
    }
    if (!gvn_table || !gvn_keys || !ab_init(&sb, &arena, est_cap)) {
        wwal_bump_arena_free(&arena);
        if (out_glsl_len)
            *out_glsl_len = 0;
        return NULL;
    }

    uint32_t active_mask = schedule->active_pass_mask;
    if (active_mask == 0) {
        active_mask = (1u << 4);
    }

    uint16_t canon_idx[WAYWAL_GRAPH_MAX_NODES];
    for (uint32_t i = 0; i < ncnt; ++i) {
        canon_idx[i] = (uint16_t)i;
    }

    for (uint32_t pass = 0; pass < WAYWAL_GRAPH_MAX_PASSES; ++pass) {
        if (!(active_mask & (1u << pass))) {
            continue;
        }

        const char *pass_name = (pass == 0)   ? "BUFFER_A"
                                : (pass == 1) ? "BUFFER_B"
                                : (pass == 2) ? "BUFFER_C"
                                : (pass == 3) ? "BUFFER_D"
                                              : "IMAGE_PASS";

        ab_append_lit(
            &sb,
            "// =============================================================================\n"
            "#pragma Pass ");
        ab_append_cstr(&sb, pass_name);
        ab_append_lit(
            &sb,
            "\n// =============================================================================\n"
            "#version 310 es\n"
            "precision highp float;\n"
            "precision highp int;\n\n"
            "in vec2 v_TexCoord;\n"
            "layout(location = 0) out vec4 outColor;\n\n"
            "// Standard wwald daemon uniforms\n"
            "uniform float u_Time;\n"
            "uniform vec2  u_Resolution;\n"
            "uniform vec2  u_Pointer;\n"
            "uniform vec4  u_AudioBands[4];\n"
            "uniform sampler2D u_BufferA;\n"
            "uniform sampler2D u_BufferB;\n"
            "uniform sampler2D u_BufferC;\n"
            "uniform sampler2D u_BufferD;\n"
            "uniform sampler2D u_DepthMap;\n\n"
            "// Promoted GraphConstants uniforms (144 Hz zero-recompile scrubbing)\n");

        uint32_t helper_mask = 0u;
        for (uint32_t k = 0; k < sorted_cnt; ++k) {
            uint32_t idx = schedule->sorted_node_indices[k];
            if (idx >= ncnt || !reachable[idx] || node_pass[idx] != pass)
                continue;
            const waywal_graph_node_t *node = &graph->nodes[idx];
            helper_mask |= helper_mask_for_node(graph, &adj, node_pass, idx);
            if (!(node->flags & WAYWAL_NODE_FLAG_MUTED_BYPASS) &&
                (node->opcode == WAYWAL_NODE_IN_CONSTANT ||
                 node->opcode == WAYWAL_NODE_IN_USER_PROP)) {
                ab_append_lit(&sb, "uniform vec4 u_Node");
                ab_append_u32(&sb, node->node_id);
                ab_append_lit(&sb, "_Const;\n");
            }
        }
        ab_append_lit(&sb, "\n");

        emit_glsl_helpers(&sb, helper_mask);

        ab_append_lit(&sb, "void main() {\n");

        memset(gvn_table, 0, 512u * sizeof(gvn_slot_t));
        bool emitted_in_pass[WAYWAL_GRAPH_MAX_NODES] = {false};
        int32_t final_node_idx = -1;
        bool has_pass_sink = false;

        for (uint32_t k = 0; k < sorted_cnt; ++k) {
            uint32_t idx = schedule->sorted_node_indices[k];
            if (idx >= ncnt || !reachable[idx] || node_pass[idx] != pass) {
                continue;
            }
            const waywal_graph_node_t *node = &graph->nodes[idx];

            if ((node->flags & WAYWAL_NODE_FLAG_MUTED_BYPASS) && node->output_pin_count > 0) {
                emit_muted_bypass_node(&sb, graph, &adj, node_pass, emitted_in_pass, canon_idx,
                                       idx);
                emitted_in_pass[idx] = true;
                canon_idx[idx] = (uint16_t)idx;
                final_node_idx = (int32_t)idx;
                continue;
            }

            if (is_pure_gvn_op(node->opcode)) {
                gvn_key_t key;
                gvn_build_key(graph, &adj, node_pass, emitted_in_pass, canon_idx, idx, pass, &key);
                uint64_t h = gvn_hash_key(&key);
                uint32_t slot = (uint32_t)h & 511u;
                bool gvn_hit = false;
                while (gvn_table[slot].occupied) {
                    if (gvn_table[slot].hash == h &&
                        memcmp(&gvn_keys[gvn_table[slot].node_idx], &key, sizeof(key)) == 0) {
                        uint16_t match_idx = gvn_table[slot].node_idx;
                        canon_idx[idx] = match_idx;
                        emitted_in_pass[idx] = true;
                        if (node->output_pin_count > 0u) {
                            final_node_idx = (int32_t)match_idx;
                        }
                        gvn_hit = true;
                        break;
                    }
                    slot = (slot + 1u) & 511u;
                }
                if (gvn_hit) {
                    continue;
                }
                gvn_keys[idx] = key;
                gvn_table[slot].hash = h;
                gvn_table[slot].node_idx = (uint16_t)idx;
                gvn_table[slot].occupied = true;
            }

            char ops[5][160];
            op_view_t ov[5];
            uint32_t max_ops = (node->input_pin_count < 5u) ? node->input_pin_count : 5u;
            for (uint32_t p = 0; p < max_ops; ++p) {
                ov[p].len = format_operand_fast(graph, &adj, node_pass, emitted_in_pass, canon_idx,
                                                idx, p, ops[p], sizeof(ops[p]));
                ov[p].s = ops[p];
            }
            for (uint32_t p = max_ops; p < 5u; ++p) {
                ops[p][0] = '0';
                ops[p][1] = '.';
                ops[p][2] = '0';
                ops[p][3] = '\0';
                ov[p].s = ops[p];
                ov[p].len = 3u;
            }

            uint32_t nid = node->node_id;

            switch (node->opcode) {
            case WAYWAL_NODE_IN_UV:
                ab_append_lit(&sb, "    vec2 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = v_TexCoord;\n");
                break;
            case WAYWAL_NODE_IN_TIME:
                ab_append_lit(&sb, "    float n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = u_Time;\n");
                break;
            case WAYWAL_NODE_IN_RESOLUTION:
                ab_append_lit(&sb, "    vec2 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = u_Resolution;\n");
                break;
            case WAYWAL_NODE_IN_POINTER:
                ab_append_lit(&sb, "    vec2 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = u_Pointer;\n");
                break;
            case WAYWAL_NODE_IN_AUDIO_FFT:
                ab_append_lit(&sb, "    vec4 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = u_AudioBands[clamp(int(");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, "), 0, 3)];\n    float n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out1 = n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0.x;\n");
                break;
            case WAYWAL_NODE_IN_TEXTURE2D:
            case WAYWAL_NODE_IN_PASS_BUFFER: {
                const char *smp = resolve_pass_buffer_sampler(node->prop_or_asset_key, 0);
                ab_append_lit(&sb, "    vec4 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = texture(");
                ab_append_cstr(&sb, smp);
                ab_append_lit(&sb, ", ");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ");\n");
                break;
            }
            case WAYWAL_NODE_IN_DEPTH_MAP:
                ab_append_lit(&sb, "    float n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = texture(u_DepthMap, ");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ").r;\n");
                break;
            case WAYWAL_NODE_IN_CONSTANT:
            case WAYWAL_NODE_IN_USER_PROP:
                ab_append_lit(&sb, "    vec4 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = u_Node");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_Const;\n");
                break;
            case WAYWAL_NODE_MATH_ADD:
            case WAYWAL_NODE_MATH_SUB:
            case WAYWAL_NODE_MATH_MUL: {
                const char *op_sym = (node->opcode == WAYWAL_NODE_MATH_ADD)   ? " + "
                                     : (node->opcode == WAYWAL_NODE_MATH_SUB) ? " - "
                                                                              : " * ";
                ab_append_lit(&sb, "    vec4 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = (");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_n(&sb, op_sym, 3u);
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, ");\n");
                break;
            }
            case WAYWAL_NODE_MATH_DIV:
                ab_append_lit(&sb, "    vec4 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = (");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, " / mix(vec4(1e-6), ");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, ", notEqual(");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, ", vec4(0.0))));\n");
                break;
            case WAYWAL_NODE_MATH_FMA:
                ab_append_lit(&sb, "    vec4 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = (");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, " * ");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, " + ");
                ab_append_n(&sb, ov[2].s, ov[2].len);
                ab_append_lit(&sb, ");\n");
                break;
            case WAYWAL_NODE_MATH_MIX:
                ab_append_lit(&sb, "    vec4 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = mix(");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ", ");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, ", ");
                ab_append_n(&sb, ov[2].s, ov[2].len);
                ab_append_lit(&sb, ");\n");
                break;
            case WAYWAL_NODE_MATH_SMOOTHSTEP:
                /* GLSL ES 3.10 §8.3 leaves smoothstep(edge0, edge1, x) undefined when
                 * edge0 >= edge1; order the bounds and invert when reversed. */
                ab_append_lit(&sb, "    float n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_e0 = ");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ";\n    float n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_e1 = ");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, ";\n    float n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_lo = min(n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_e0, n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_e1);\n    float n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_hi = max(max(n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_e0, n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_e1), n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_lo + 1e-5);\n    float n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_s = smoothstep(n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_lo, n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_hi, ");
                ab_append_n(&sb, ov[2].s, ov[2].len);
                ab_append_lit(&sb, ");\n    float n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = (n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_e0 <= n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_e1) ? n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_s : (1.0 - n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_s);\n");
                break;
            case WAYWAL_NODE_MATH_CLAMP:
                ab_append_lit(&sb, "    vec4 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = clamp(");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ", ");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, ", ");
                ab_append_n(&sb, ov[2].s, ov[2].len);
                ab_append_lit(&sb, ");\n");
                break;
            case WAYWAL_NODE_MATH_SIN:
            case WAYWAL_NODE_MATH_COS:
            case WAYWAL_NODE_MATH_FRACT:
            case WAYWAL_NODE_MATH_FLOOR:
            case WAYWAL_NODE_MATH_ABS: {
                const char *fn_name = (node->opcode == WAYWAL_NODE_MATH_SIN)     ? "sin("
                                      : (node->opcode == WAYWAL_NODE_MATH_COS)   ? "cos("
                                      : (node->opcode == WAYWAL_NODE_MATH_FRACT) ? "fract("
                                      : (node->opcode == WAYWAL_NODE_MATH_FLOOR) ? "floor("
                                                                                 : "abs(";
                ab_append_lit(&sb, "    vec4 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = ");
                ab_append_cstr(&sb, fn_name);
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ");\n");
                break;
            }
            case WAYWAL_NODE_MATH_POW:
                ab_append_lit(&sb, "    vec4 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = wwal_safe_pow(");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ", vec4(");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, "));\n");
                break;
            case WAYWAL_NODE_MATH_DOT:
                ab_append_lit(&sb, "    float n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = dot(");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ", ");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, ");\n");
                break;
            case WAYWAL_NODE_MATH_CROSS:
                ab_append_lit(&sb, "    vec3 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = cross((");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ").xyz, (");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, ").xyz);\n");
                break;
            case WAYWAL_NODE_MATH_NORMALIZE:
                ab_append_lit(&sb, "    vec4 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = (length(");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ") > 1e-6 ? normalize(");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ") : ");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ");\n");
                break;
            case WAYWAL_NODE_MATH_LENGTH:
                ab_append_lit(&sb, "    float n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = length(");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ");\n");
                break;
            case WAYWAL_NODE_MATH_DISTANCE:
                ab_append_lit(&sb, "    float n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = distance((");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ").xy, (");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, ").xy);\n");
                break;
            case WAYWAL_NODE_VEC_SWIZZLE_COMBINE:
                ab_append_lit(&sb, "    vec2 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = vec2(");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ", ");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, ");\n    vec3 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out1 = vec3(");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ", ");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, ", ");
                ab_append_n(&sb, ov[2].s, ov[2].len);
                ab_append_lit(&sb, ");\n    vec4 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out2 = vec4(");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ", ");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, ", ");
                ab_append_n(&sb, ov[2].s, ov[2].len);
                ab_append_lit(&sb, ", ");
                ab_append_n(&sb, ov[3].s, ov[3].len);
                ab_append_lit(&sb, ");\n");
                break;
            case WAYWAL_NODE_NOISE_SIMPLEX2D:
                ab_append_lit(&sb, "    float n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = wwal_simplex2d((");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ").xy * (");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, "));\n");
                break;
            case WAYWAL_NODE_NOISE_CURL2D:
                ab_append_lit(&sb, "    vec2 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = wwal_curl2d((");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ").xy * (");
                ab_append_n(&sb, ov[2].s, ov[2].len);
                ab_append_lit(&sb, "), ");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, ");\n");
                break;
            case WAYWAL_NODE_NOISE_VORONOI2D:
                ab_append_lit(&sb, "    vec2 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_voro = wwal_voronoi2d((");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ").xy * (");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, "));\n    float n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_voro.x;\n    vec2  n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out1 = n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_voro;\n");
                break;
            case WAYWAL_NODE_NOISE_FBM2D:
                ab_append_lit(&sb, "    float n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = wwal_fbm2d((");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ").xy, int(");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, "), ");
                ab_append_n(&sb, ov[2].s, ov[2].len);
                ab_append_lit(&sb, ");\n");
                break;
            case WAYWAL_NODE_SDF_CIRCLE:
                ab_append_lit(&sb, "    float n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = length((");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ").xy) - (");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, ");\n");
                break;
            case WAYWAL_NODE_SDF_BOX:
                ab_append_lit(&sb, "    float n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = wwal_sdf_box((");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ").xy, (");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, ").xy);\n");
                break;
            case WAYWAL_NODE_SDF_RING:
                ab_append_lit(&sb, "    float n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = abs(length((");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ").xy) - (");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, ")) - (");
                ab_append_n(&sb, ov[2].s, ov[2].len);
                ab_append_lit(&sb, ");\n");
                break;
            case WAYWAL_NODE_UV_POLAR:
                ab_append_lit(&sb, "    vec2 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = wwal_uv_polar((");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ").xy, (");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, ").xy);\n");
                break;
            case WAYWAL_NODE_UV_TWIRL:
                ab_append_lit(&sb, "    vec2 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = wwal_uv_twirl((");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ").xy, (");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, ").xy, ");
                ab_append_n(&sb, ov[2].s, ov[2].len);
                ab_append_lit(&sb, ", ");
                ab_append_n(&sb, ov[3].s, ov[3].len);
                ab_append_lit(&sb, ");\n");
                break;
            case WAYWAL_NODE_UV_TILE:
                ab_append_lit(&sb, "    vec2 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = fract((");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ").xy * (");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, ").xy);\n");
                break;
            case WAYWAL_NODE_COLOR_OKLCH_TO_SRGB:
                ab_append_lit(&sb, "    vec4 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = vec4(wwal_oklch_to_srgb(");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, "), 1.0);\n");
                break;
            case WAYWAL_NODE_COLOR_SRGB_TO_OKLCH:
                ab_append_lit(&sb, "    vec4 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = vec4(wwal_srgb_to_oklch(");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, "), 1.0);\n");
                break;
            case WAYWAL_NODE_COLOR_HUE_SHIFT:
                ab_append_lit(&sb, "    vec4 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = wwal_hue_shift(");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ", ");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, ");\n");
                break;
            case WAYWAL_NODE_COLOR_CONTRAST_SAT:
                ab_append_lit(&sb, "    vec4 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = wwal_contrast_saturation(");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ", ");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, ", ");
                ab_append_n(&sb, ov[2].s, ov[2].len);
                ab_append_lit(&sb, ");\n");
                break;
            case WAYWAL_NODE_FILTER_DUAL_KAWASE: {
                bool has_smp = false;
                const char *smp = resolve_spatial_sampler(graph, &adj, node_pass, idx, 0, &has_smp);
                ab_append_lit(&sb, "    vec4 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = ");
                if (has_smp && key_contains_ci(node->prop_or_asset_key, "bloom")) {
                    float c1 = finite_or_zero(node->const_val[1]);
                    float glow = (c1 > 0.0f) ? c1 : 0.65f;
                    ab_append_lit(&sb, "texture(");
                    ab_append_cstr(&sb, smp);
                    ab_append_lit(&sb, ", (");
                    ab_append_n(&sb, ov[1].s, ov[1].len);
                    ab_append_lit(&sb, ").xy) + wwal_dual_kawase(");
                    ab_append_cstr(&sb, smp);
                    ab_append_lit(&sb, ", (");
                    ab_append_n(&sb, ov[1].s, ov[1].len);
                    ab_append_lit(&sb, ").xy, ");
                    ab_append_n(&sb, ov[2].s, ov[2].len);
                    ab_append_lit(&sb, ") * ");
                    ab_append_f32(&sb, glow);
                    ab_append_lit(&sb, ";\n");
                } else if (has_smp) {
                    ab_append_lit(&sb, "wwal_dual_kawase(");
                    ab_append_cstr(&sb, smp);
                    ab_append_lit(&sb, ", (");
                    ab_append_n(&sb, ov[1].s, ov[1].len);
                    ab_append_lit(&sb, ").xy, ");
                    ab_append_n(&sb, ov[2].s, ov[2].len);
                    ab_append_lit(&sb, ");\n");
                } else {
                    ab_append_n(&sb, ov[0].s, ov[0].len);
                    ab_append_lit(&sb, ";\n");
                }
                break;
            }
            case WAYWAL_NODE_FILTER_CHROMATIC_ABERR: {
                bool has_smp = false;
                const char *smp = resolve_spatial_sampler(graph, &adj, node_pass, idx, 0, &has_smp);
                ab_append_lit(&sb, "    vec4 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = ");
                if (has_smp) {
                    ab_append_lit(&sb, "wwal_chromatic_aberration(");
                    ab_append_cstr(&sb, smp);
                    ab_append_lit(&sb, ", (");
                    ab_append_n(&sb, ov[1].s, ov[1].len);
                    ab_append_lit(&sb, ").xy, ");
                    ab_append_n(&sb, ov[2].s, ov[2].len);
                    ab_append_lit(&sb, ");\n");
                } else {
                    ab_append_n(&sb, ov[0].s, ov[0].len);
                    ab_append_lit(&sb, ";\n");
                }
                break;
            }
            case WAYWAL_NODE_LIGHT_NORMAL_FROM_HEIGHT: {
                bool has_smp = false;
                const char *smp = resolve_spatial_sampler(graph, &adj, node_pass, idx, 0, &has_smp);
                ab_append_lit(&sb, "    vec3 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = ");
                if (adj.pin_in_edge[idx][0] >= 0 && has_smp) {
                    ab_append_lit(&sb, "wwal_normal_from_height_tex(");
                    ab_append_cstr(&sb, smp);
                } else {
                    ab_append_lit(&sb, "wwal_normal_from_height(");
                    ab_append_n(&sb, ov[0].s, ov[0].len);
                }
                ab_append_lit(&sb, ", (");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, ").xy, ");
                ab_append_n(&sb, ov[2].s, ov[2].len);
                ab_append_lit(&sb, ");\n");
                break;
            }
            case WAYWAL_NODE_LIGHT_BLINN_PHONG:
                ab_append_lit(&sb, "    vec4 n");
                ab_append_u32(&sb, nid);
                ab_append_lit(&sb, "_out0 = wwal_blinn_phong((");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ").xyz, (");
                ab_append_n(&sb, ov[1].s, ov[1].len);
                ab_append_lit(&sb, ").xyz, (");
                ab_append_n(&sb, ov[2].s, ov[2].len);
                ab_append_lit(&sb, ").xyz, ");
                ab_append_n(&sb, ov[3].s, ov[3].len);
                ab_append_lit(&sb, ", ");
                ab_append_n(&sb, ov[4].s, ov[4].len);
                ab_append_lit(&sb, ");\n");
                break;
            case WAYWAL_NODE_BUILTIN_EFFECT: {
                bool has_smp = false;
                const char *smp = resolve_spatial_sampler(graph, &adj, node_pass, idx, 0, &has_smp);
                (void)has_smp;
                emit_builtin_effect_node(&sb, node, smp, ov[0], ov[1], ov[2], ov[3]);
                break;
            }
            case WAYWAL_NODE_OUT_BUFFER_PASS:
                ab_append_lit(&sb, "    outColor = ");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ";\n");
                has_pass_sink = true;
                break;
            case WAYWAL_NODE_OUT_IMAGE_PASS:
                /* Invariant 2: IMAGE_PASS must force opaque alpha so Wayland surfaces never turn
                 * black/transparent. */
                ab_append_lit(&sb, "    outColor = vec4((");
                ab_append_n(&sb, ov[0].s, ov[0].len);
                ab_append_lit(&sb, ").rgb, 1.0);\n");
                has_pass_sink = true;
                break;
            default:
                break;
            }

            emitted_in_pass[idx] = true;
            canon_idx[idx] = (uint16_t)idx;
            if (node->output_pin_count > 0) {
                final_node_idx = (int32_t)idx;
            }
        }

        if (!has_pass_sink) {
            if (final_node_idx >= 0) {
                const waywal_graph_node_t *fn = &graph->nodes[final_node_idx];
                uint32_t fn_id = fn->node_id;
                uint32_t ftype = fn->outputs[0].pin_type;
                if (ftype == WAYWAL_PIN_FLOAT) {
                    ab_append_lit(&sb, "    outColor = vec4(vec3(n");
                    ab_append_u32(&sb, fn_id);
                    ab_append_lit(&sb, "_out0), 1.0);\n");
                } else if (ftype == WAYWAL_PIN_VEC2) {
                    ab_append_lit(&sb, "    outColor = vec4(n");
                    ab_append_u32(&sb, fn_id);
                    ab_append_lit(&sb, "_out0, 0.0, 1.0);\n");
                } else if (ftype == WAYWAL_PIN_VEC3) {
                    ab_append_lit(&sb, "    outColor = vec4(n");
                    ab_append_u32(&sb, fn_id);
                    ab_append_lit(&sb, "_out0, 1.0);\n");
                } else if (ftype == WAYWAL_PIN_INT || ftype == WAYWAL_PIN_BOOL) {
                    ab_append_lit(&sb, "    outColor = vec4(vec3(float(n");
                    ab_append_u32(&sb, fn_id);
                    ab_append_lit(&sb, "_out0)), 1.0);\n");
                } else if (pass == 4) {
                    ab_append_lit(&sb, "    outColor = vec4(n");
                    ab_append_u32(&sb, fn_id);
                    ab_append_lit(&sb, "_out0.rgb, 1.0);\n");
                } else {
                    ab_append_lit(&sb, "    outColor = n");
                    ab_append_u32(&sb, fn_id);
                    ab_append_lit(&sb, "_out0;\n");
                }
            } else {
                ab_append_cstr(&sb, (pass == 4) ? "    outColor = vec4(1.0, 1.0, 1.0, 1.0);\n"
                                                : "    outColor = vec4(0.0, 0.0, 0.0, 1.0);\n");
            }
        }

        ab_append_lit(&sb, "}\n\n");
    }

    if (sb.oom) {
        wwal_bump_arena_free(&arena);
        if (out_glsl_len)
            *out_glsl_len = 0;
        return NULL;
    }

    char *result = (char *)malloc(sb.len + 1u);
    if (!result) {
        wwal_bump_arena_free(&arena);
        if (out_glsl_len)
            *out_glsl_len = 0;
        return NULL;
    }
    memcpy(result, sb.data, sb.len);
    result[sb.len] = '\0';
    if (out_glsl_len) {
        *out_glsl_len = sb.len;
    }
    wwal_bump_arena_free(&arena);
    return result;
}

/* ========================================================================= */
/* .wwgraph JSON Serialization & Deserialization (Draft 2020-12 Schema)      */
/* ========================================================================= */

char *waywal_shader_graph_save_json(const waywal_shader_graph_t *graph)
{
    if (!graph)
        return NULL;

    str_buf_t sb;
    if (!str_buf_init(&sb, 8192))
        return NULL;

    uint32_t ncnt =
        (graph->node_count <= WAYWAL_GRAPH_MAX_NODES) ? graph->node_count : WAYWAL_GRAPH_MAX_NODES;
    uint32_t ecnt =
        (graph->edge_count <= WAYWAL_GRAPH_MAX_EDGES) ? graph->edge_count : WAYWAL_GRAPH_MAX_EDGES;

    str_buf_append(&sb, "{\n");
    str_buf_append_fmt(&sb, "  \"schema_version\": %u,\n", graph->schema_version);
    str_buf_append(&sb, "  \"graph_name\": ");
    str_buf_append_json_escaped(&sb, graph->graph_name);
    str_buf_append(&sb, ",\n  \"author\": ");
    str_buf_append_json_escaped(&sb, graph->author);
    str_buf_append_fmt(&sb, ",\n  \"flags\": %u,\n", graph->flags);

    str_buf_append(&sb, "  \"nodes\": [\n");
    for (uint32_t i = 0; i < ncnt; ++i) {
        const waywal_graph_node_t *n = &graph->nodes[i];
        str_buf_append(&sb, "    {\n");
        str_buf_append_fmt(&sb, "      \"node_id\": %u,\n", n->node_id);
        str_buf_append_fmt(&sb, "      \"opcode\": %u,\n", n->opcode);
        str_buf_append(&sb, "      \"title\": ");
        str_buf_append_json_escaped(&sb, n->title);
        str_buf_append(&sb, ",\n      \"prop_or_asset_key\": ");
        str_buf_append_json_escaped(&sb, n->prop_or_asset_key);
        if (n->custom_label[0] != '\0') {
            str_buf_append(&sb, ",\n      \"custom_label\": ");
            str_buf_append_json_escaped(&sb, n->custom_label);
        }
        str_buf_append_fmt(&sb, ",\n      \"canvas_pos\": [%.9g, %.9g],\n",
                           (double)finite_or_zero(n->canvas_pos[0]),
                           (double)finite_or_zero(n->canvas_pos[1]));
        str_buf_append_fmt(&sb, "      \"target_pass\": %u,\n", n->target_pass);
        str_buf_append_fmt(&sb, "      \"flags\": %u,\n", n->flags);
        str_buf_append_fmt(
            &sb, "      \"const_val\": [%.9g, %.9g, %.9g, %.9g]\n",
            (double)finite_or_zero(n->const_val[0]), (double)finite_or_zero(n->const_val[1]),
            (double)finite_or_zero(n->const_val[2]), (double)finite_or_zero(n->const_val[3]));
        str_buf_append_fmt(&sb, "    }%s\n", (i + 1 < ncnt) ? "," : "");
    }
    str_buf_append(&sb, "  ],\n");

    str_buf_append(&sb, "  \"edges\": [\n");
    for (uint32_t i = 0; i < ecnt; ++i) {
        const waywal_graph_edge_t *e = &graph->edges[i];
        str_buf_append(&sb, "    { ");
        str_buf_append_fmt(&sb, "\"edge_id\": %u, ", e->edge_id);
        str_buf_append_fmt(&sb, "\"src_node_id\": %u, ", e->src_node_id);
        str_buf_append_fmt(&sb, "\"src_pin_index\": %u, ", e->src_pin_index);
        str_buf_append_fmt(&sb, "\"dst_node_id\": %u, ", e->dst_node_id);
        str_buf_append_fmt(&sb, "\"dst_pin_index\": %u, ", e->dst_pin_index);
        str_buf_append_fmt(&sb, "\"swizzle_mask\": %u", e->swizzle_mask);
        if (e->flags != 0) {
            str_buf_append_fmt(&sb, ", \"flags\": %u", e->flags);
        }
        str_buf_append_fmt(&sb, " }%s\n", (i + 1 < ecnt) ? "," : "");
    }
    str_buf_append(&sb, "  ]\n");
    str_buf_append(&sb, "}\n");

    if (sb.oom) {
        str_buf_free(&sb);
        return NULL;
    }
    return sb.data;
}

char *waywal_shader_graph_to_json(const waywal_shader_graph_t *graph)
{
    return waywal_shader_graph_save_json(graph);
}

/* --- Lightweight Fast JSON Parser for .wwgraph Documents --- */

static void json_skip_ws(const char **p, const char *end)
{
    while (*p < end && isspace((unsigned char)**p)) {
        (*p)++;
    }
}

static bool json_expect_char(const char **p, const char *end, char c)
{
    json_skip_ws(p, end);
    if (*p < end && **p == c) {
        (*p)++;
        return true;
    }
    return false;
}

static int hex_digit_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

static void utf8_append_codepoint(uint32_t cp, char *out, size_t cap, size_t *io_len,
                                  bool *truncated)
{
    if (!out || cap <= 1u || (truncated && *truncated))
        return;
    char enc[4];
    size_t enc_len = 0;
    if (cp <= 0x7Fu) {
        enc[0] = (char)cp;
        enc_len = 1u;
    } else if (cp <= 0x7FFu) {
        enc[0] = (char)(0xC0u | (cp >> 6));
        enc[1] = (char)(0x80u | (cp & 0x3Fu));
        enc_len = 2u;
    } else if (cp <= 0xFFFFu) {
        enc[0] = (char)(0xE0u | (cp >> 12));
        enc[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        enc[2] = (char)(0x80u | (cp & 0x3Fu));
        enc_len = 3u;
    } else if (cp <= 0x10FFFFu) {
        enc[0] = (char)(0xF0u | (cp >> 18));
        enc[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        enc[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        enc[3] = (char)(0x80u | (cp & 0x3Fu));
        enc_len = 4u;
    } else {
        return;
    }
    if (*io_len + enc_len < cap) {
        memcpy(out + *io_len, enc, enc_len);
        *io_len += enc_len;
    } else if (truncated) {
        *truncated = true;
    }
}

static bool json_parse_string(const char **p, const char *end, char *out, size_t cap)
{
    json_skip_ws(p, end);
    if (*p >= end || **p != '"')
        return false;
    const char *cur = *p + 1;

    size_t out_len = 0;
    bool truncated = false;
    while (cur < end) {
        while ((size_t)(end - cur) >= 8u) {
            uint64_t chunk;
            memcpy(&chunk, cur, 8u);
            if (swar_has_json_special(chunk))
                break;
            if (out && !truncated && cap > 1u) {
                if (out_len + 1u < cap) {
                    size_t avail = cap - 1u - out_len;
                    size_t to_copy = (avail < 8u) ? avail : 8u;
                    memcpy(out + out_len, cur, to_copy);
                    out_len += to_copy;
                    if (avail < 8u) {
                        truncated = true;
                    }
                } else {
                    truncated = true;
                }
            }
            cur += 8u;
        }
        if (cur >= end)
            return false;

        unsigned char c = (unsigned char)*cur;
        if (c == '"') {
            *p = cur + 1;
            if (out && cap > 0u) {
                size_t final_len = (out_len < cap) ? out_len : (cap - 1u);
                if (truncated) {
                    final_len = utf8_clamp_boundary(out, final_len);
                }
                out[final_len] = '\0';
            }
            return true;
        }
        if (c < 0x20u) {
            return false;
        }
        if (c == '\\') {
            cur++;
            if (cur >= end)
                return false;
            char esc = *cur;
            char decoded = 0;
            bool single_byte = true;
            switch (esc) {
            case '"':
            case '\\':
            case '/':
                decoded = esc;
                break;
            case 'b':
                decoded = '\b';
                break;
            case 'f':
                decoded = '\f';
                break;
            case 'n':
                decoded = '\n';
                break;
            case 'r':
                decoded = '\r';
                break;
            case 't':
                decoded = '\t';
                break;
            case 'u': {
                single_byte = false;
                if ((size_t)(end - cur) < 5u)
                    return false;
                int h0 = hex_digit_val(cur[1]);
                int h1 = hex_digit_val(cur[2]);
                int h2 = hex_digit_val(cur[3]);
                int h3 = hex_digit_val(cur[4]);
                if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0)
                    return false;
                uint32_t cp = (uint32_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                cur += 5;
                if (cp >= 0xD800u && cp <= 0xDBFFu) {
                    if ((size_t)(end - cur) >= 6u && cur[0] == '\\' && cur[1] == 'u') {
                        int l0 = hex_digit_val(cur[2]);
                        int l1 = hex_digit_val(cur[3]);
                        int l2 = hex_digit_val(cur[4]);
                        int l3 = hex_digit_val(cur[5]);
                        if (l0 >= 0 && l1 >= 0 && l2 >= 0 && l3 >= 0) {
                            uint32_t lo = (uint32_t)((l0 << 12) | (l1 << 8) | (l2 << 4) | l3);
                            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                                cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
                                cur += 6;
                            } else {
                                cp = 0xFFFDu;
                            }
                        } else {
                            return false;
                        }
                    } else {
                        cp = 0xFFFDu;
                    }
                } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                    cp = 0xFFFDu;
                }
                utf8_append_codepoint(cp, out, cap, &out_len, &truncated);
                continue;
            }
            default:
                return false;
            }
            if (single_byte && out && !truncated && cap > 1u) {
                if (out_len + 1u < cap) {
                    out[out_len++] = decoded;
                } else {
                    truncated = true;
                }
            }
            cur++;
        } else {
            if (out && !truncated && cap > 1u) {
                if (out_len + 1u < cap) {
                    out[out_len++] = (char)c;
                } else {
                    truncated = true;
                }
            }
            cur++;
        }
    }
    return false;
}

static bool json_parse_number(const char **p, const char *end, double *out_val)
{
    json_skip_ws(p, end);
    if (*p >= end)
        return false;

    /* Copy numeric token into bounded NUL-terminated stack buffer so strtod
     * never reads past json_str + json_len on non-NUL-terminated slices. */
    char num_buf[64];
    size_t rem = (size_t)(end - *p);
    size_t max_n = (rem < sizeof(num_buf) - 1) ? rem : (sizeof(num_buf) - 1);
    size_t n = 0;
    while (n < max_n) {
        char c = (*p)[n];
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
            num_buf[n++] = c;
        } else {
            break;
        }
    }
    if (n == 0)
        return false;
    num_buf[n] = '\0';

    char *endptr = NULL;
    double val = strtod(num_buf, &endptr);
    if (endptr == num_buf || !isfinite(val))
        return false;
    *out_val = val;
    *p += (size_t)(endptr - num_buf);
    return true;
}

static bool json_parse_f32(const char **p, const char *end, float *out_val)
{
    double d = 0.0;
    if (!json_parse_number(p, end, &d))
        return false;
    if (d < -(double)FLT_MAX || d > (double)FLT_MAX)
        return false;
    *out_val = (float)d;
    return true;
}

static bool json_parse_uint(const char **p, const char *end, uint32_t *out_val)
{
    double d = 0.0;
    if (!json_parse_number(p, end, &d))
        return false;
    if (!isfinite(d) || d < 0.0 || d > (double)UINT32_MAX || floor(d) != d)
        return false;
    *out_val = (uint32_t)d;
    return true;
}

#define JSON_SKIP_MAX_DEPTH 128u

static bool json_skip_value(const char **p, const char *end)
{
    json_skip_ws(p, end);
    if (*p >= end)
        return false;

    if (**p == '"') {
        return json_parse_string(p, end, NULL, 0);
    }
    if (**p == '{' || **p == '[') {
        char close_stack[JSON_SKIP_MAX_DEPTH];
        size_t sp = 0u;
        close_stack[sp++] = (**p == '{') ? '}' : ']';
        (*p)++;

        while (*p < end && sp > 0u) {
            char c = **p;
            if (c == '"') {
                if (!json_parse_string(p, end, NULL, 0))
                    return false;
            } else if (c == '{' || c == '[') {
                if (sp >= JSON_SKIP_MAX_DEPTH)
                    return false;
                close_stack[sp++] = (c == '{') ? '}' : ']';
                (*p)++;
            } else if (c == '}' || c == ']') {
                if (c != close_stack[sp - 1u])
                    return false;
                sp--;
                (*p)++;
            } else {
                (*p)++;
            }
        }
        return (sp == 0u);
    }
    if ((size_t)(end - *p) >= 4u && memcmp(*p, "true", 4u) == 0) {
        *p += 4;
        return true;
    }
    if ((size_t)(end - *p) >= 5u && memcmp(*p, "false", 5u) == 0) {
        *p += 5;
        return true;
    }
    if ((size_t)(end - *p) >= 4u && memcmp(*p, "null", 4u) == 0) {
        *p += 4;
        return true;
    }
    double dummy_num = 0.0;
    return json_parse_number(p, end, &dummy_num);
}

bool waywal_shader_graph_load_json(const char *json_str, size_t json_len,
                                   waywal_shader_graph_t *out_graph, char err_buf[256])
{
    if (!json_str || !out_graph) {
        if (err_buf)
            snprintf(err_buf, 256, "Null pointer argument");
        return false;
    }
    if (json_len == 0) {
        json_len = strlen(json_str);
    }
    const char *p = json_str;
    const char *end = json_str + json_len;

    waywal_shader_graph_init(out_graph, NULL);

    if (!json_expect_char(&p, end, '{')) {
        if (err_buf)
            snprintf(err_buf, 256, "Expected root JSON object '{'");
        return false;
    }

    bool edge_had_explicit_id[WAYWAL_GRAPH_MAX_EDGES] = {false};
    bool root_closed = false;

    while (p < end) {
        json_skip_ws(&p, end);
        if (p >= end)
            break;
        if (*p == '}') {
            p++;
            root_closed = true;
            break;
        }

        char key[64];
        if (!json_parse_string(&p, end, key, sizeof(key))) {
            if (err_buf)
                snprintf(err_buf, 256, "Expected property key string");
            return false;
        }
        if (!json_expect_char(&p, end, ':')) {
            if (err_buf)
                snprintf(err_buf, 256, "Expected ':' after key '%s'", key);
            return false;
        }

        if (strcmp(key, "schema_version") == 0) {
            uint32_t sv = 0;
            if (!json_parse_uint(&p, end, &sv) || sv != WAYWAL_GRAPH_SCHEMA_VERSION) {
                if (err_buf)
                    snprintf(err_buf, 256, "Unsupported schema_version %u (expected 1)", sv);
                return false;
            }
            out_graph->schema_version = sv;
        } else if (strcmp(key, "graph_name") == 0) {
            if (!json_parse_string(&p, end, out_graph->graph_name, sizeof(out_graph->graph_name))) {
                if (err_buf)
                    snprintf(err_buf, 256, "Invalid graph_name string");
                return false;
            }
        } else if (strcmp(key, "author") == 0) {
            if (!json_parse_string(&p, end, out_graph->author, sizeof(out_graph->author))) {
                if (err_buf)
                    snprintf(err_buf, 256, "Invalid author string");
                return false;
            }
        } else if (strcmp(key, "flags") == 0) {
            if (!json_parse_uint(&p, end, &out_graph->flags)) {
                if (err_buf)
                    snprintf(err_buf, 256, "Invalid graph flags");
                return false;
            }
        } else if (strcmp(key, "nodes") == 0) {
            if (!json_expect_char(&p, end, '[')) {
                if (err_buf)
                    snprintf(err_buf, 256, "Expected '[' for nodes array");
                return false;
            }

            bool nodes_closed = false;
            while (p < end) {
                json_skip_ws(&p, end);
                if (p >= end)
                    break;
                if (*p == ']') {
                    p++;
                    nodes_closed = true;
                    break;
                }
                if (out_graph->node_count >= WAYWAL_GRAPH_MAX_NODES) {
                    if (err_buf)
                        snprintf(err_buf, 256, "Exceeded WAYWAL_GRAPH_MAX_NODES (256)");
                    return false;
                }

                if (!json_expect_char(&p, end, '{')) {
                    if (err_buf)
                        snprintf(err_buf, 256, "Expected node object '{'");
                    return false;
                }

                waywal_graph_node_t *n = &out_graph->nodes[out_graph->node_count];
                memset(n, 0, sizeof(*n));
                n->canvas_size[0] = 180.0f;
                n->canvas_size[1] = 120.0f;
                n->const_val[3] = 1.0f;
                bool has_target_pass = false;
                bool node_closed = false;

                while (p < end) {
                    json_skip_ws(&p, end);
                    if (p >= end)
                        break;
                    if (*p == '}') {
                        p++;
                        node_closed = true;
                        break;
                    }

                    char nkey[64];
                    if (!json_parse_string(&p, end, nkey, sizeof(nkey)) ||
                        !json_expect_char(&p, end, ':')) {
                        if (err_buf)
                            snprintf(err_buf, 256, "Malformed node key/value pair");
                        return false;
                    }

                    if (strcmp(nkey, "node_id") == 0 || strcmp(nkey, "id") == 0) {
                        if (!json_parse_uint(&p, end, &n->node_id)) {
                            if (err_buf)
                                snprintf(err_buf, 256, "Invalid node_id");
                            return false;
                        }
                    } else if (strcmp(nkey, "opcode") == 0) {
                        if (!json_parse_uint(&p, end, &n->opcode)) {
                            if (err_buf)
                                snprintf(err_buf, 256, "Invalid node opcode");
                            return false;
                        }
                    } else if (strcmp(nkey, "title") == 0) {
                        if (!json_parse_string(&p, end, n->title, sizeof(n->title))) {
                            if (err_buf)
                                snprintf(err_buf, 256, "Invalid node title");
                            return false;
                        }
                    } else if (strcmp(nkey, "prop_or_asset_key") == 0) {
                        if (!json_parse_string(&p, end, n->prop_or_asset_key,
                                               sizeof(n->prop_or_asset_key))) {
                            if (err_buf)
                                snprintf(err_buf, 256, "Invalid prop_or_asset_key");
                            return false;
                        }
                    } else if (strcmp(nkey, "custom_label") == 0) {
                        if (!json_parse_string(&p, end, n->custom_label, sizeof(n->custom_label))) {
                            if (err_buf)
                                snprintf(err_buf, 256, "Invalid custom_label");
                            return false;
                        }
                    } else if (strcmp(nkey, "canvas_pos") == 0) {
                        float x = 0.0f, y = 0.0f;
                        if (!json_expect_char(&p, end, '[') || !json_parse_f32(&p, end, &x) ||
                            !json_expect_char(&p, end, ',') || !json_parse_f32(&p, end, &y) ||
                            !json_expect_char(&p, end, ']')) {
                            if (err_buf)
                                snprintf(err_buf, 256, "Invalid canvas_pos array");
                            return false;
                        }
                        n->canvas_pos[0] = x;
                        n->canvas_pos[1] = y;
                    } else if (strcmp(nkey, "target_pass") == 0) {
                        if (!json_parse_uint(&p, end, &n->target_pass)) {
                            if (err_buf)
                                snprintf(err_buf, 256, "Invalid target_pass");
                            return false;
                        }
                        has_target_pass = true;
                    } else if (strcmp(nkey, "flags") == 0) {
                        if (!json_parse_uint(&p, end, &n->flags)) {
                            if (err_buf)
                                snprintf(err_buf, 256, "Invalid node flags");
                            return false;
                        }
                    } else if (strcmp(nkey, "const_val") == 0) {
                        if (!json_expect_char(&p, end, '[')) {
                            if (err_buf)
                                snprintf(err_buf, 256, "Expected '[' for const_val");
                            return false;
                        }
                        float c[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                        for (int ci = 0; ci < 4; ++ci) {
                            if (!json_parse_f32(&p, end, &c[ci])) {
                                if (err_buf)
                                    snprintf(err_buf, 256, "Invalid const_val element");
                                return false;
                            }
                            if (ci < 3 && !json_expect_char(&p, end, ',')) {
                                if (err_buf)
                                    snprintf(err_buf, 256, "Expected ',' in const_val");
                                return false;
                            }
                        }
                        if (!json_expect_char(&p, end, ']')) {
                            if (err_buf)
                                snprintf(err_buf, 256, "Expected ']' after const_val");
                            return false;
                        }
                        for (int ci = 0; ci < 4; ++ci)
                            n->const_val[ci] = c[ci];
                    } else {
                        if (!json_skip_value(&p, end)) {
                            if (err_buf)
                                snprintf(err_buf, 256, "Malformed value in node object");
                            return false;
                        }
                    }

                    if (!json_expect_char(&p, end, ',')) {
                        json_skip_ws(&p, end);
                        if (p >= end || *p != '}') {
                            if (err_buf)
                                snprintf(err_buf, 256, "Expected ',' or '}' in node object");
                            return false;
                        }
                    }
                }

                if (!node_closed) {
                    if (err_buf)
                        snprintf(err_buf, 256, "Unclosed node object");
                    return false;
                }

                if (!has_target_pass && n->opcode == WAYWAL_NODE_OUT_IMAGE_PASS) {
                    n->target_pass = 4u;
                }
                node_init_default_pins(n);
                out_graph->node_count++;

                if (!json_expect_char(&p, end, ',')) {
                    json_skip_ws(&p, end);
                    if (p >= end || *p != ']') {
                        if (err_buf)
                            snprintf(err_buf, 256, "Expected ',' or ']' in nodes array");
                        return false;
                    }
                }
            }

            if (!nodes_closed) {
                if (err_buf)
                    snprintf(err_buf, 256, "Unclosed nodes array");
                return false;
            }
        } else if (strcmp(key, "edges") == 0) {
            if (!json_expect_char(&p, end, '[')) {
                if (err_buf)
                    snprintf(err_buf, 256, "Expected '[' for edges array");
                return false;
            }

            bool edges_closed = false;
            while (p < end) {
                json_skip_ws(&p, end);
                if (p >= end)
                    break;
                if (*p == ']') {
                    p++;
                    edges_closed = true;
                    break;
                }
                if (out_graph->edge_count >= WAYWAL_GRAPH_MAX_EDGES) {
                    if (err_buf)
                        snprintf(err_buf, 256, "Exceeded WAYWAL_GRAPH_MAX_EDGES (512)");
                    return false;
                }

                if (!json_expect_char(&p, end, '{')) {
                    if (err_buf)
                        snprintf(err_buf, 256, "Expected edge object '{'");
                    return false;
                }

                uint32_t e_idx = out_graph->edge_count;
                waywal_graph_edge_t *e = &out_graph->edges[e_idx];
                memset(e, 0, sizeof(*e));
                bool edge_closed = false;

                while (p < end) {
                    json_skip_ws(&p, end);
                    if (p >= end)
                        break;
                    if (*p == '}') {
                        p++;
                        edge_closed = true;
                        break;
                    }

                    char ekey[64];
                    if (!json_parse_string(&p, end, ekey, sizeof(ekey)) ||
                        !json_expect_char(&p, end, ':')) {
                        if (err_buf)
                            snprintf(err_buf, 256, "Malformed edge key/value pair");
                        return false;
                    }

                    if (strcmp(ekey, "edge_id") == 0 || strcmp(ekey, "id") == 0) {
                        if (!json_parse_uint(&p, end, &e->edge_id)) {
                            if (err_buf)
                                snprintf(err_buf, 256, "Invalid edge_id");
                            return false;
                        }
                        edge_had_explicit_id[e_idx] = true;
                    } else if (strcmp(ekey, "src_node_id") == 0) {
                        if (!json_parse_uint(&p, end, &e->src_node_id)) {
                            if (err_buf)
                                snprintf(err_buf, 256, "Invalid src_node_id");
                            return false;
                        }
                    } else if (strcmp(ekey, "src_pin_index") == 0) {
                        if (!json_parse_uint(&p, end, &e->src_pin_index)) {
                            if (err_buf)
                                snprintf(err_buf, 256, "Invalid src_pin_index");
                            return false;
                        }
                    } else if (strcmp(ekey, "dst_node_id") == 0) {
                        if (!json_parse_uint(&p, end, &e->dst_node_id)) {
                            if (err_buf)
                                snprintf(err_buf, 256, "Invalid dst_node_id");
                            return false;
                        }
                    } else if (strcmp(ekey, "dst_pin_index") == 0) {
                        if (!json_parse_uint(&p, end, &e->dst_pin_index)) {
                            if (err_buf)
                                snprintf(err_buf, 256, "Invalid dst_pin_index");
                            return false;
                        }
                    } else if (strcmp(ekey, "swizzle_mask") == 0) {
                        if (!json_parse_uint(&p, end, &e->swizzle_mask)) {
                            if (err_buf)
                                snprintf(err_buf, 256, "Invalid swizzle_mask");
                            return false;
                        }
                    } else if (strcmp(ekey, "flags") == 0) {
                        if (!json_parse_uint(&p, end, &e->flags)) {
                            if (err_buf)
                                snprintf(err_buf, 256, "Invalid edge flags");
                            return false;
                        }
                    } else {
                        if (!json_skip_value(&p, end)) {
                            if (err_buf)
                                snprintf(err_buf, 256, "Malformed value in edge object");
                            return false;
                        }
                    }

                    if (!json_expect_char(&p, end, ',')) {
                        json_skip_ws(&p, end);
                        if (p >= end || *p != '}') {
                            if (err_buf)
                                snprintf(err_buf, 256, "Expected ',' or '}' in edge object");
                            return false;
                        }
                    }
                }

                if (!edge_closed) {
                    if (err_buf)
                        snprintf(err_buf, 256, "Unclosed edge object");
                    return false;
                }

                out_graph->edge_count++;

                if (!json_expect_char(&p, end, ',')) {
                    json_skip_ws(&p, end);
                    if (p >= end || *p != ']') {
                        if (err_buf)
                            snprintf(err_buf, 256, "Expected ',' or ']' in edges array");
                        return false;
                    }
                }
            }

            if (!edges_closed) {
                if (err_buf)
                    snprintf(err_buf, 256, "Unclosed edges array");
                return false;
            }
        } else {
            if (!json_skip_value(&p, end)) {
                if (err_buf)
                    snprintf(err_buf, 256, "Malformed value for key '%s'", key);
                return false;
            }
        }

        if (!json_expect_char(&p, end, ',')) {
            json_skip_ws(&p, end);
            if (p >= end || *p != '}') {
                if (err_buf)
                    snprintf(err_buf, 256, "Expected ',' or '}' in root object");
                return false;
            }
        }
    }

    if (!root_closed) {
        if (err_buf)
            snprintf(err_buf, 256, "Unclosed root JSON object");
        return false;
    }

    json_skip_ws(&p, end);
    if (p != end) {
        if (err_buf)
            snprintf(err_buf, 256, "Unexpected trailing content after root JSON object");
        return false;
    }

    /* Validate node invariants after parsing so unordered JSON keys ("edges" before "nodes") work
     * identically to canonical order. */
    for (uint32_t i = 0; i < out_graph->node_count; ++i) {
        const waywal_graph_node_t *n = &out_graph->nodes[i];
        if (n->node_id == 0 || n->node_id > (uint32_t)INT32_MAX) {
            if (err_buf)
                snprintf(err_buf, 256, "Invalid node_id %u", n->node_id);
            return false;
        }
        if (n->opcode > WAYWAL_NODE_OUT_IMAGE_PASS) {
            if (err_buf)
                snprintf(err_buf, 256, "Out-of-range opcode %u on node %u", n->opcode, n->node_id);
            return false;
        }
        if (n->target_pass >= WAYWAL_GRAPH_MAX_PASSES) {
            if (err_buf)
                snprintf(err_buf, 256, "Out-of-range target_pass %u on node %u", n->target_pass,
                         n->node_id);
            return false;
        }
        for (uint32_t j = 0; j < i; ++j) {
            if (out_graph->nodes[j].node_id == n->node_id) {
                if (err_buf)
                    snprintf(err_buf, 256, "Duplicate node_id %u", n->node_id);
                return false;
            }
        }
    }

    uint32_t max_eid = 0;
    for (uint32_t i = 0; i < out_graph->edge_count; ++i) {
        if (edge_had_explicit_id[i] && out_graph->edges[i].edge_id > max_eid)
            max_eid = out_graph->edges[i].edge_id;
    }

    for (uint32_t i = 0; i < out_graph->edge_count; ++i) {
        waywal_graph_edge_t *e = &out_graph->edges[i];
        if (!edge_had_explicit_id[i]) {
            if (max_eid >= (uint32_t)INT32_MAX) {
                if (err_buf)
                    snprintf(err_buf, 256, "Edge ID overflow");
                return false;
            }
            e->edge_id = ++max_eid;
        } else if (e->edge_id == 0 || e->edge_id > (uint32_t)INT32_MAX) {
            if (err_buf)
                snprintf(err_buf, 256, "Invalid edge_id %u", e->edge_id);
            return false;
        }

        const waywal_graph_node_t *sn =
            waywal_shader_graph_find_node_const(out_graph, e->src_node_id);
        const waywal_graph_node_t *dn =
            waywal_shader_graph_find_node_const(out_graph, e->dst_node_id);
        if (!sn || !dn) {
            if (err_buf)
                snprintf(err_buf, 256, "Edge %u references non-existent node (%u -> %u)",
                         e->edge_id, e->src_node_id, e->dst_node_id);
            return false;
        }
        if (e->src_pin_index >= sn->output_pin_count || e->dst_pin_index >= dn->input_pin_count) {
            if (err_buf)
                snprintf(err_buf, 256, "Edge %u pin index out of bounds", e->edge_id);
            return false;
        }
        if (!waywal_pin_types_compatible(sn->outputs[e->src_pin_index].pin_type,
                                         dn->inputs[e->dst_pin_index].pin_type)) {
            if (err_buf)
                snprintf(err_buf, 256, "Edge %u connects incompatible pin types", e->edge_id);
            return false;
        }
        for (uint32_t j = 0; j < i; ++j) {
            if (out_graph->edges[j].edge_id == e->edge_id) {
                if (err_buf)
                    snprintf(err_buf, 256, "Duplicate edge_id %u", e->edge_id);
                return false;
            }
            if (out_graph->edges[j].dst_node_id == e->dst_node_id &&
                out_graph->edges[j].dst_pin_index == e->dst_pin_index) {
                if (err_buf)
                    snprintf(err_buf, 256, "Multiple incoming edges on node %u pin %u",
                             e->dst_node_id, e->dst_pin_index);
                return false;
            }
        }
    }

    uint32_t cycle_node = 0;
    if (waywal_shader_graph_has_cycle(out_graph, &cycle_node)) {
        if (err_buf)
            snprintf(err_buf, 256, "Directed cycle detected involving node ID %u", cycle_node);
        return false;
    }

    sync_connected_pin_flags(out_graph);
    return true;
}

bool waywal_shader_graph_from_json(const char *json_str, size_t json_len,
                                   waywal_shader_graph_t *out_graph, char err_buf[256])
{
    return waywal_shader_graph_load_json(json_str, json_len, out_graph, err_buf);
}
