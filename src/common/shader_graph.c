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
        strncpy(pin->name, name, sizeof(pin->name) - 1);
    }
    if (label) {
        strncpy(pin->label, label, sizeof(pin->label) - 1);
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
        strncpy(graph->graph_name, name, sizeof(graph->graph_name) - 1);
    } else {
        strncpy(graph->graph_name, "Untitled Shader Graph", sizeof(graph->graph_name) - 1);
    }
    strncpy(graph->author, "wwal", sizeof(graph->author) - 1);
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

static void sync_connected_pin_flags(waywal_shader_graph_t *graph)
{
    if (!graph)
        return;
    uint32_t ncnt =
        (graph->node_count <= WAYWAL_GRAPH_MAX_NODES) ? graph->node_count : WAYWAL_GRAPH_MAX_NODES;
    uint32_t ecnt =
        (graph->edge_count <= WAYWAL_GRAPH_MAX_EDGES) ? graph->edge_count : WAYWAL_GRAPH_MAX_EDGES;

    for (uint32_t i = 0; i < ncnt; ++i) {
        waywal_graph_node_t *n = &graph->nodes[i];
        for (uint32_t p = 0; p < n->input_pin_count && p < WAYWAL_GRAPH_MAX_PINS_PER_NODE; ++p)
            n->inputs[p].flags &= ~WAYWAL_PIN_FLAG_CONNECTED;
        for (uint32_t p = 0; p < n->output_pin_count && p < WAYWAL_GRAPH_MAX_PINS_PER_NODE; ++p)
            n->outputs[p].flags &= ~WAYWAL_PIN_FLAG_CONNECTED;
    }

    for (uint32_t i = 0; i < ecnt; ++i) {
        const waywal_graph_edge_t *e = &graph->edges[i];
        waywal_graph_node_t *sn = waywal_shader_graph_find_node(graph, e->src_node_id);
        waywal_graph_node_t *dn = waywal_shader_graph_find_node(graph, e->dst_node_id);
        if (sn && e->src_pin_index < sn->output_pin_count &&
            e->src_pin_index < WAYWAL_GRAPH_MAX_PINS_PER_NODE) {
            sn->outputs[e->src_pin_index].flags |= WAYWAL_PIN_FLAG_CONNECTED;
        }
        if (dn && e->dst_pin_index < dn->input_pin_count &&
            e->dst_pin_index < WAYWAL_GRAPH_MAX_PINS_PER_NODE) {
            dn->inputs[e->dst_pin_index].flags |= WAYWAL_PIN_FLAG_CONNECTED;
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
        strncpy(n->title, title, sizeof(n->title) - 1);
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
    if (!graph || graph->edge_count >= WAYWAL_GRAPH_MAX_EDGES) {
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

    /* Single-input constraint: remove existing incoming wire to (dst_node_id, dst_pin_idx) */
    waywal_shader_graph_disconnect_pins(graph, dst_node_id, dst_pin_idx);

    /* Allocate new edge ID */
    uint32_t max_eid = 0;
    for (uint32_t i = 0; i < graph->edge_count; ++i) {
        if (graph->edges[i].edge_id > max_eid) {
            max_eid = graph->edges[i].edge_id;
        }
    }
    if (max_eid >= (uint32_t)INT32_MAX) {
        return -1;
    }
    uint32_t new_eid = max_eid + 1;

    /* Tentatively add edge */
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

    /* Check if candidate edge creates an illegal directed cycle */
    uint32_t cycle_id = 0;
    if (waywal_shader_graph_has_cycle(graph, &cycle_id)) {
        /* Roll back edge */
        graph->edge_count--;
        return -1;
    }

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

#define GRAPH_DENSE_ID_CAP 1024

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

static bool tarjan_dfs(const waywal_shader_graph_t *graph, const graph_adj_t *adj, uint32_t u_idx,
                       int32_t *dfs_clock, tarjan_node_state_t *states, uint32_t *stack,
                       uint32_t *stack_top, uint32_t *out_cycle_node_id)
{
    states[u_idx].index = *dfs_clock;
    states[u_idx].lowlink = *dfs_clock;
    (*dfs_clock)++;

    stack[*stack_top] = u_idx;
    (*stack_top)++;
    states[u_idx].on_stack = true;

    uint32_t u_id = graph->nodes[u_idx].node_id;

    for (int16_t e = adj->head_out[u_idx]; e >= 0; e = adj->next_out[e]) {
        int16_t v_idx_s = adj->edge_dst_idx[e];
        if (v_idx_s < 0)
            continue;
        uint32_t v_idx = (uint32_t)v_idx_s;

        /* Self-loops are never valid temporal latches even on IN_PASS_BUFFER. */
        if (v_idx == u_idx) {
            if (out_cycle_node_id) {
                *out_cycle_node_id = u_id;
            }
            return true;
        }

        /* Temporal feedback latches sample frame (t - 1) and break combinational cycles. */
        if (graph->nodes[u_idx].opcode == WAYWAL_NODE_IN_PASS_BUFFER ||
            (graph->edges[e].flags & WAYWAL_EDGE_FLAG_FEEDBACK_LATCH)) {
            continue;
        }

        if (states[v_idx].index == -1) {
            if (tarjan_dfs(graph, adj, v_idx, dfs_clock, states, stack, stack_top,
                           out_cycle_node_id)) {
                return true;
            }
            if (states[v_idx].lowlink < states[u_idx].lowlink) {
                states[u_idx].lowlink = states[v_idx].lowlink;
            }
        } else if (states[v_idx].on_stack) {
            if (states[v_idx].index < states[u_idx].lowlink) {
                states[u_idx].lowlink = states[v_idx].index;
            }
        }
    }

    if (states[u_idx].lowlink == states[u_idx].index) {
        uint32_t scc_size = 0;
        uint32_t popped_idx;
        do {
            if (*stack_top == 0)
                break;
            (*stack_top)--;
            popped_idx = stack[*stack_top];
            states[popped_idx].on_stack = false;
            scc_size++;
        } while (popped_idx != u_idx);

        if (scc_size > 1) {
            if (out_cycle_node_id) {
                *out_cycle_node_id = u_id;
            }
            return true;
        }
    }

    return false;
}

static bool has_cycle_with_adj(const waywal_shader_graph_t *graph, const graph_adj_t *adj,
                               uint32_t *out_cycle_node_id)
{
    uint32_t ncnt =
        (graph->node_count <= WAYWAL_GRAPH_MAX_NODES) ? graph->node_count : WAYWAL_GRAPH_MAX_NODES;
    tarjan_node_state_t states[WAYWAL_GRAPH_MAX_NODES];
    uint32_t stack[WAYWAL_GRAPH_MAX_NODES];
    uint32_t stack_top = 0;
    int32_t dfs_clock = 0;

    for (uint32_t i = 0; i < ncnt; ++i) {
        states[i].index = -1;
        states[i].lowlink = -1;
        states[i].on_stack = false;
    }

    for (uint32_t i = 0; i < ncnt; ++i) {
        if (states[i].index == -1) {
            if (tarjan_dfs(graph, adj, i, &dfs_clock, states, stack, &stack_top,
                           out_cycle_node_id)) {
                return true;
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

static uint32_t compute_sethi_ullman_adj(const waywal_shader_graph_t *graph, const graph_adj_t *adj,
                                         uint32_t node_idx, const bool *reachable,
                                         uint32_t *weights, bool *visiting)
{
    if (!reachable[node_idx])
        return 0;
    if (weights[node_idx] > 0)
        return weights[node_idx];
    if (visiting[node_idx])
        return 1;

    const waywal_graph_node_t *node = &graph->nodes[node_idx];
    if (node->input_pin_count == 0) {
        weights[node_idx] = 1;
        return 1;
    }

    visiting[node_idx] = true;
    uint32_t child_weights[WAYWAL_GRAPH_MAX_PINS_PER_NODE];
    uint32_t child_count = 0;

    uint32_t pcnt = (node->input_pin_count <= WAYWAL_GRAPH_MAX_PINS_PER_NODE)
                        ? node->input_pin_count
                        : WAYWAL_GRAPH_MAX_PINS_PER_NODE;
    for (uint32_t p = 0; p < pcnt; ++p) {
        int16_t e_idx = adj->pin_in_edge[node_idx][p];
        if (e_idx < 0)
            continue;
        if (graph->edges[e_idx].flags & WAYWAL_EDGE_FLAG_FEEDBACK_LATCH) {
            child_weights[child_count++] = 1;
            continue;
        }
        int16_t s_idx = adj->edge_src_idx[e_idx];
        if (s_idx >= 0 && reachable[s_idx]) {
            if (graph->nodes[s_idx].opcode == WAYWAL_NODE_IN_PASS_BUFFER) {
                child_weights[child_count++] = 1;
            } else {
                child_weights[child_count++] = compute_sethi_ullman_adj(
                    graph, adj, (uint32_t)s_idx, reachable, weights, visiting);
            }
        }
    }

    visiting[node_idx] = false;

    if (child_count == 0) {
        weights[node_idx] = 1;
        return 1;
    }

    for (uint32_t i = 0; i < child_count; ++i) {
        for (uint32_t j = i + 1; j < child_count; ++j) {
            if (child_weights[j] > child_weights[i]) {
                uint32_t tmp = child_weights[i];
                child_weights[i] = child_weights[j];
                child_weights[j] = tmp;
            }
        }
    }

    uint32_t max_weight = 1;
    for (uint32_t i = 0; i < child_count; ++i) {
        uint32_t w = child_weights[i] + i;
        if (w > max_weight) {
            max_weight = w;
        }
    }
    weights[node_idx] = max_weight;
    return max_weight;
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
    bool any_sink = false;

    for (uint32_t i = 0; i < ncnt; ++i) {
        uint32_t op = graph_in->nodes[i].opcode;
        if (op != WAYWAL_NODE_OUT_IMAGE_PASS && op != WAYWAL_NODE_OUT_BUFFER_PASS) {
            continue;
        }
        any_sink = true;
        bool is_img = (op == WAYWAL_NODE_OUT_IMAGE_PASS);
        int8_t bpass = is_img ? -1 : (int8_t)resolve_buffer_pass_index(&graph_in->nodes[i]);

        bool visited_local[WAYWAL_GRAPH_MAX_NODES] = {false};
        uint32_t top = 0;
        bfs_stack[top++] = (uint16_t)i;
        visited_local[i] = true;

        while (top > 0) {
            uint16_t cur = bfs_stack[--top];
            reachable[cur] = true;
            if (is_img) {
                reaches_image_sink[cur] = true;
            } else if (explicit_buf_pass[cur] < 0) {
                explicit_buf_pass[cur] = bpass;
            }

            for (int16_t e = adj->head_in[cur]; e >= 0; e = adj->next_in[e]) {
                int16_t s = adj->edge_src_idx[e];
                if (s >= 0 && !visited_local[s]) {
                    visited_local[s] = true;
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

    uint32_t barrier_stage[WAYWAL_GRAPH_MAX_NODES] = {0};
    for (uint32_t iter = 0; iter < ncnt; ++iter) {
        bool changed = false;
        for (uint32_t e = 0; e < ecnt; ++e) {
            int16_t s = adj->edge_src_idx[e];
            int16_t d = adj->edge_dst_idx[e];
            if (s < 0 || d < 0 || !reachable[s] || !reachable[d])
                continue;

            if (graph_in->nodes[s].opcode == WAYWAL_NODE_IN_PASS_BUFFER ||
                (graph_in->edges[e].flags & WAYWAL_EDGE_FLAG_FEEDBACK_LATCH)) {
                continue;
            }

            bool split = edge_requires_barrier_split(graph_in, (uint32_t)s, (uint32_t)d,
                                                     &graph_in->edges[e]);
            uint32_t new_s = barrier_stage[s] + (split ? 1u : 0u);
            if (new_s > 4u)
                new_s = 4u;
            if (new_s > barrier_stage[d]) {
                barrier_stage[d] = new_s;
                changed = true;
            }
        }
        if (!changed)
            break;
    }

    /* Pull non-barrier leaf inputs up to their consumer stage so constants/UVs do not force
     * spurious intermediate pass splits. */
    for (uint32_t iter = 0; iter < ncnt; ++iter) {
        bool changed = false;
        for (uint32_t u = 0; u < ncnt; ++u) {
            if (!reachable[u] || !reaches_image_sink[u])
                continue;
            uint32_t op = graph_in->nodes[u].opcode;
            if (op == WAYWAL_NODE_OUT_IMAGE_PASS || op == WAYWAL_NODE_OUT_BUFFER_PASS ||
                op == WAYWAL_NODE_IN_PASS_BUFFER) {
                continue;
            }

            uint32_t min_dst_stage = UINT32_MAX;
            bool has_consumer = false;
            bool has_split_out = false;
            for (int16_t e = adj->head_out[u]; e >= 0; e = adj->next_out[e]) {
                if (graph_in->edges[e].flags & WAYWAL_EDGE_FLAG_FEEDBACK_LATCH)
                    continue;
                int16_t d = adj->edge_dst_idx[e];
                if (d < 0 || !reachable[d])
                    continue;
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
                changed = true;
            }
        }
        if (!changed)
            break;
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
    bool visiting[WAYWAL_GRAPH_MAX_NODES] = {false};
    for (uint32_t i = 0; i < ncnt; ++i) {
        if (reachable[i] && sethi_ullman[i] == 0) {
            compute_sethi_ullman_adj(graph_in, &adj, i, reachable, sethi_ullman, visiting);
        }
    }

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

static void emit_glsl_helpers(str_buf_t *sb, uint32_t helper_mask)
{
    if (helper_mask & (WWAL_HLP_CURL2D | WWAL_HLP_FBM2D))
        helper_mask |= WWAL_HLP_SIMPLEX2D;
    if (helper_mask & (WWAL_HLP_SIMPLEX2D | WWAL_HLP_VORONOI2D))
        helper_mask |= WWAL_HLP_HASH21;
    if (helper_mask & WWAL_HLP_HUE_SHIFT)
        helper_mask |= (WWAL_HLP_SRGB_TO_OKLCH | WWAL_HLP_OKLCH_TO_SRGB);

    if (helper_mask == 0u)
        return;

    if (helper_mask & WWAL_HLP_OKLCH_TO_SRGB) {
        str_buf_append(sb,
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
                       "    return clamp(vec3(r, g, bl), 0.0, 1.0);\n"
                       "}\n\n");
    }
    if (helper_mask & WWAL_HLP_SRGB_TO_OKLCH) {
        str_buf_append(
            sb, "vec3 wwal_srgb_to_oklch(vec4 srgb) {\n"
                "    vec3 c = srgb.rgb;\n"
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
                "    float h = atan(b, a);\n"
                "    return vec3(L, C, h);\n"
                "}\n\n");
    }
    if (helper_mask & WWAL_HLP_HASH21) {
        str_buf_append(sb, "float wwal_hash21(vec2 p) {\n"
                           "    vec3 p3 = fract(vec3(p.xyx) * 0.1031);\n"
                           "    p3 += dot(p3, p3.yzx + 33.33);\n"
                           "    return fract((p3.x + p3.y) * p3.z);\n"
                           "}\n\n");
    }
    if (helper_mask & WWAL_HLP_SIMPLEX2D) {
        str_buf_append(sb,
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
        str_buf_append(sb, "vec2 wwal_curl2d(vec2 p, float t) {\n"
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
        str_buf_append(sb,
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
        str_buf_append(sb, "float wwal_fbm2d(vec2 p, int octaves, float lacunarity) {\n"
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
        str_buf_append(sb, "float wwal_sdf_box(vec2 p, vec2 b) {\n"
                           "    vec2 d = abs(p) - b;\n"
                           "    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);\n"
                           "}\n\n");
    }
    if (helper_mask & WWAL_HLP_UV_POLAR) {
        str_buf_append(sb, "vec2 wwal_uv_polar(vec2 uv, vec2 center) {\n"
                           "    vec2 d = uv - center;\n"
                           "    float r = length(d);\n"
                           "    float a = atan(d.y, d.x) / 6.28318530718 + 0.5;\n"
                           "    return vec2(a, r);\n"
                           "}\n\n");
    }
    if (helper_mask & WWAL_HLP_UV_TWIRL) {
        str_buf_append(sb,
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
        str_buf_append(sb, "vec4 wwal_hue_shift(vec4 col, float shift) {\n"
                           "    vec3 oklch = wwal_srgb_to_oklch(col);\n"
                           "    oklch.z += shift;\n"
                           "    return vec4(wwal_oklch_to_srgb(vec4(oklch, 1.0)), col.a);\n"
                           "}\n\n");
    }
    if (helper_mask & WWAL_HLP_CONTRAST_SAT) {
        str_buf_append(sb, "vec4 wwal_contrast_saturation(vec4 col, float c, float s) {\n"
                           "    vec3 rgb = (col.rgb - 0.5) * c + 0.5;\n"
                           "    float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));\n"
                           "    rgb = mix(vec3(luma), rgb, s);\n"
                           "    return vec4(clamp(rgb, 0.0, 1.0), col.a);\n"
                           "}\n\n");
    }
    if (helper_mask & WWAL_HLP_DUAL_KAWASE) {
        str_buf_append(
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
        str_buf_append(sb,
                       "vec4 wwal_chromatic_aberration(sampler2D tex, vec2 uv, float intensity) {\n"
                       "    vec2 dir = (uv - vec2(0.5)) * intensity;\n"
                       "    float r = texture(tex, clamp(uv + dir, vec2(0.0), vec2(1.0))).r;\n"
                       "    vec4 center = texture(tex, uv);\n"
                       "    float b = texture(tex, clamp(uv - dir, vec2(0.0), vec2(1.0))).b;\n"
                       "    return vec4(r, center.g, b, center.a);\n"
                       "}\n\n");
    }
    if (helper_mask & WWAL_HLP_NORMAL_HEIGHT_TEX) {
        str_buf_append(
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
        str_buf_append(sb, "vec3 wwal_normal_from_height(float h, vec2 uv, float strength) {\n"
                           "    float dx = dFdx(h + uv.x * 0.0) * strength;\n"
                           "    float dy = dFdy(h + uv.y * 0.0) * strength;\n"
                           "    return normalize(vec3(-dx, -dy, 1.0));\n"
                           "}\n\n");
    }
    if (helper_mask & WWAL_HLP_BLINN_PHONG) {
        str_buf_append(
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
        str_buf_append(
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
        str_buf_append(
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
        str_buf_append(
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
        str_buf_append(
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
        str_buf_append(
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
        str_buf_append(
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

static void coerce_glsl_expr(uint32_t src_type, uint32_t dst_type, const char *raw_var,
                             char *out_expr, size_t out_cap)
{
    uint32_t s_norm = (src_type == WAYWAL_PIN_AUDIO_SPECTRUM) ? WAYWAL_PIN_VEC4 : src_type;
    uint32_t d_norm = (dst_type == WAYWAL_PIN_AUDIO_SPECTRUM) ? WAYWAL_PIN_VEC4 : dst_type;

    if (s_norm == d_norm) {
        snprintf(out_expr, out_cap, "%s", raw_var);
        return;
    }

    switch (d_norm) {
    case WAYWAL_PIN_FLOAT:
        if (s_norm == WAYWAL_PIN_INT)
            snprintf(out_expr, out_cap, "float(%s)", raw_var);
        else if (s_norm == WAYWAL_PIN_BOOL)
            snprintf(out_expr, out_cap, "(%s ? 1.0 : 0.0)", raw_var);
        else
            snprintf(out_expr, out_cap, "(%s).x", raw_var);
        return;

    case WAYWAL_PIN_INT:
        if (s_norm == WAYWAL_PIN_FLOAT)
            snprintf(out_expr, out_cap, "int(%s)", raw_var);
        else if (s_norm == WAYWAL_PIN_BOOL)
            snprintf(out_expr, out_cap, "(%s ? 1 : 0)", raw_var);
        else
            snprintf(out_expr, out_cap, "int((%s).x)", raw_var);
        return;

    case WAYWAL_PIN_BOOL:
        if (s_norm == WAYWAL_PIN_INT)
            snprintf(out_expr, out_cap, "((%s) != 0)", raw_var);
        else if (s_norm == WAYWAL_PIN_FLOAT)
            snprintf(out_expr, out_cap, "((%s) != 0.0)", raw_var);
        else
            snprintf(out_expr, out_cap, "((%s).x != 0.0)", raw_var);
        return;

    case WAYWAL_PIN_VEC2:
        if (s_norm == WAYWAL_PIN_FLOAT)
            snprintf(out_expr, out_cap, "vec2(%s)", raw_var);
        else if (s_norm == WAYWAL_PIN_INT)
            snprintf(out_expr, out_cap, "vec2(float(%s))", raw_var);
        else if (s_norm == WAYWAL_PIN_BOOL)
            snprintf(out_expr, out_cap, "vec2(%s ? 1.0 : 0.0)", raw_var);
        else
            snprintf(out_expr, out_cap, "(%s).xy", raw_var);
        return;

    case WAYWAL_PIN_VEC3:
        if (s_norm == WAYWAL_PIN_FLOAT)
            snprintf(out_expr, out_cap, "vec3(%s)", raw_var);
        else if (s_norm == WAYWAL_PIN_INT)
            snprintf(out_expr, out_cap, "vec3(float(%s))", raw_var);
        else if (s_norm == WAYWAL_PIN_BOOL)
            snprintf(out_expr, out_cap, "vec3(%s ? 1.0 : 0.0)", raw_var);
        else if (s_norm == WAYWAL_PIN_VEC2)
            snprintf(out_expr, out_cap, "vec3(%s, 0.0)", raw_var);
        else
            snprintf(out_expr, out_cap, "(%s).xyz", raw_var);
        return;

    case WAYWAL_PIN_VEC4:
        if (s_norm == WAYWAL_PIN_FLOAT)
            snprintf(out_expr, out_cap, "vec4(vec3(%s), 1.0)", raw_var);
        else if (s_norm == WAYWAL_PIN_INT)
            snprintf(out_expr, out_cap, "vec4(vec3(float(%s)), 1.0)", raw_var);
        else if (s_norm == WAYWAL_PIN_BOOL)
            snprintf(out_expr, out_cap, "vec4(vec3(%s ? 1.0 : 0.0), 1.0)", raw_var);
        else if (s_norm == WAYWAL_PIN_VEC2)
            snprintf(out_expr, out_cap, "vec4(%s, 0.0, 1.0)", raw_var);
        else
            snprintf(out_expr, out_cap, "vec4(%s, 1.0)", raw_var);
        return;

    default:
        snprintf(out_expr, out_cap, "%s", raw_var);
        return;
    }
}

static void format_operand_typed_adj(const waywal_shader_graph_t *graph, const graph_adj_t *adj,
                                     const uint32_t *node_pass, const bool *emitted_in_pass,
                                     uint32_t dst_idx, uint32_t pin_idx, uint32_t dst_type,
                                     char *out_expr, size_t out_cap)
{
    const waywal_graph_node_t *dst_node = &graph->nodes[dst_idx];
    if (pin_idx >= dst_node->input_pin_count || pin_idx >= WAYWAL_GRAPH_MAX_PINS_PER_NODE) {
        snprintf(out_expr, out_cap, "0.0");
        return;
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

                if (node_pass[s_idx] != node_pass[dst_idx] || !emitted_in_pass[s_idx]) {
                    const char *buf_name =
                        resolve_pass_buffer_sampler((src_node->opcode == WAYWAL_NODE_IN_PASS_BUFFER)
                                                        ? src_node->prop_or_asset_key
                                                        : NULL,
                                                    node_pass[s_idx]);
                    snprintf(raw_var, sizeof(raw_var), "texture(%s, v_TexCoord)", buf_name);
                    src_type = WAYWAL_PIN_VEC4;
                } else {
                    snprintf(raw_var, sizeof(raw_var), "n%u_out%u", incoming_edge->src_node_id,
                             incoming_edge->src_pin_index);
                }

                uint32_t src_lanes = pin_lane_count(src_type);
                if (incoming_edge->swizzle_mask != 0u && src_lanes >= 2u) {
                    static const char k_swz_chars[4] = {'x', 'y', 'z', 'w'};
                    uint32_t swz_lanes = pin_lane_count(dst_type);
                    char swz[5];
                    for (uint32_t i = 0; i < swz_lanes; ++i) {
                        uint32_t lane = (incoming_edge->swizzle_mask >> (i * 2u)) & 3u;
                        if (lane >= src_lanes)
                            lane = src_lanes - 1u;
                        swz[i] = k_swz_chars[lane];
                    }
                    swz[swz_lanes] = '\0';
                    char swizzled_var[96];
                    snprintf(swizzled_var, sizeof(swizzled_var), "(%s).%s", raw_var, swz);
                    uint32_t swz_type = (swz_lanes == 1u)   ? WAYWAL_PIN_FLOAT
                                        : (swz_lanes == 2u) ? WAYWAL_PIN_VEC2
                                        : (swz_lanes == 3u) ? WAYWAL_PIN_VEC3
                                                            : WAYWAL_PIN_VEC4;
                    coerce_glsl_expr(swz_type, dst_type, swizzled_var, out_expr, out_cap);
                } else {
                    coerce_glsl_expr(src_type, dst_type, raw_var, out_expr, out_cap);
                }
                return;
            }
        }
    }

    char def_expr[80];
    switch (dst_pin->pin_type) {
    case WAYWAL_PIN_FLOAT:
        snprintf(def_expr, sizeof(def_expr), "%.6f", finite_or_zero(dst_pin->default_val[0]));
        break;
    case WAYWAL_PIN_VEC2:
        if (dst_node->opcode == WAYWAL_NODE_IN_TEXTURE2D ||
            dst_node->opcode == WAYWAL_NODE_IN_PASS_BUFFER ||
            dst_node->opcode == WAYWAL_NODE_IN_DEPTH_MAP || strcasecmp(dst_pin->name, "uv") == 0) {
            snprintf(def_expr, sizeof(def_expr), "v_TexCoord");
        } else {
            snprintf(def_expr, sizeof(def_expr), "vec2(%.6f, %.6f)",
                     finite_or_zero(dst_pin->default_val[0]),
                     finite_or_zero(dst_pin->default_val[1]));
        }
        break;
    case WAYWAL_PIN_VEC3:
        snprintf(def_expr, sizeof(def_expr), "vec3(%.6f, %.6f, %.6f)",
                 finite_or_zero(dst_pin->default_val[0]), finite_or_zero(dst_pin->default_val[1]),
                 finite_or_zero(dst_pin->default_val[2]));
        break;
    case WAYWAL_PIN_VEC4:
    case WAYWAL_PIN_AUDIO_SPECTRUM:
        snprintf(def_expr, sizeof(def_expr), "vec4(%.6f, %.6f, %.6f, %.6f)",
                 finite_or_zero(dst_pin->default_val[0]), finite_or_zero(dst_pin->default_val[1]),
                 finite_or_zero(dst_pin->default_val[2]), finite_or_zero(dst_pin->default_val[3]));
        break;
    case WAYWAL_PIN_INT:
        snprintf(def_expr, sizeof(def_expr), "%d", (int)safe_f32_to_i32(dst_pin->default_val[0]));
        break;
    case WAYWAL_PIN_BOOL:
        snprintf(def_expr, sizeof(def_expr), "%s",
                 (finite_or_zero(dst_pin->default_val[0]) != 0.0f) ? "true" : "false");
        break;
    case WAYWAL_PIN_SAMPLER2D:
        snprintf(out_expr, out_cap, "u_BufferA");
        return;
    default:
        snprintf(out_expr, out_cap, "0.0");
        return;
    }
    coerce_glsl_expr(dst_pin->pin_type, dst_type, def_expr, out_expr, out_cap);
}

static void format_operand_adj(const waywal_shader_graph_t *graph, const graph_adj_t *adj,
                               const uint32_t *node_pass, const bool *emitted_in_pass,
                               uint32_t dst_idx, uint32_t pin_idx, char *out_expr, size_t out_cap)
{
    const waywal_graph_node_t *dst_node = &graph->nodes[dst_idx];
    if (pin_idx >= dst_node->input_pin_count || pin_idx >= WAYWAL_GRAPH_MAX_PINS_PER_NODE) {
        snprintf(out_expr, out_cap, "0.0");
        return;
    }
    format_operand_typed_adj(graph, adj, node_pass, emitted_in_pass, dst_idx, pin_idx,
                             dst_node->inputs[pin_idx].pin_type, out_expr, out_cap);
}

static void emit_muted_bypass_node(str_buf_t *sb, const waywal_shader_graph_t *graph,
                                   const graph_adj_t *adj, const uint32_t *node_pass,
                                   const bool *emitted_in_pass, uint32_t idx)
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
        if (node->input_pin_count > 0) {
            char expr[128];
            format_operand_typed_adj(graph, adj, node_pass, emitted_in_pass, idx, 0, out_type, expr,
                                     sizeof(expr));
            str_buf_append_fmt(sb, "    %s n%u_out%u = %s;\n", glsl_type, nid, out_p, expr);
        } else {
            str_buf_append_fmt(sb, "    %s n%u_out%u = %s;\n", glsl_type, nid, out_p, zero_lit);
        }
    }
}

static void emit_builtin_effect_node(str_buf_t *sb, const waywal_graph_node_t *node,
                                     const char *sampler, const char *in0, const char *in1,
                                     const char *in2, const char *in3)
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
    safe_key[sk_len] = '\0';

    str_buf_append_fmt(sb, "    // Builtin Effect: %s\n", (sk_len > 0) ? safe_key : "waterripple");

    if (key[0] == '\0' || key_contains_ci(key, "waterripple") || key_contains_ci(key, "ripple")) {
        str_buf_append_fmt(
            sb,
            "    vec4 n%u_out0 = wwal_effect_waterripple(%s, v_TexCoord, (%s).xy, %s, %s, %s);\n",
            nid, sampler, in0, in1, in2, in3);
    } else if (key_contains_ci(key, "water") || key_contains_ci(key, "refract") ||
               key_contains_ci(key, "rain")) {
        str_buf_append_fmt(sb,
                           "    vec4 n%u_out0 = wwal_effect_waterwaves(%s, (%s).xy, %s, %s, %s);\n",
                           nid, sampler, in0, in1, in2, in3);
    } else if (key_contains_ci(key, "bloom") || key_contains_ci(key, "halo") ||
               key_contains_ci(key, "flare") || key_contains_ci(key, "shine")) {
        str_buf_append_fmt(
            sb,
            "    vec4 n%u_out0 = vec4(clamp(texture(%s, (%s).xy).rgb + wwal_dual_kawase(%s, "
            "(%s).xy, max(%s, 1.5)).rgb * max(%s, 0.5), 0.0, 1.0), 1.0);\n",
            nid, sampler, in0, sampler, in0, in1, in2);
    } else if (key_contains_ci(key, "blur") || key_contains_ci(key, "depth_of_field")) {
        str_buf_append_fmt(sb, "    vec4 n%u_out0 = wwal_dual_kawase(%s, (%s).xy, max(%s, 1.5));\n",
                           nid, sampler, in0, in1);
    } else if (key_contains_ci(key, "chromatic") || key_contains_ci(key, "vhs")) {
        str_buf_append_fmt(sb,
                           "    vec4 n%u_out0 = wwal_chromatic_aberration(%s, (%s).xy, max(abs(%s) "
                           "* 0.02, 0.006));\n",
                           nid, sampler, in0, in1);
    } else if (key_contains_ci(key, "sharpen") || key_contains_ci(key, "edge") ||
               key_contains_ci(key, "outline")) {
        str_buf_append_fmt(sb, "    vec4 n%u_out0 = wwal_effect_cas_sharpen(%s, (%s).xy, %s);\n",
                           nid, sampler, in0, in1);
    } else if (key_contains_ci(key, "vignette") || key_contains_ci(key, "iris")) {
        str_buf_append_fmt(sb, "    vec4 n%u_out0 = wwal_effect_vignette(%s, (%s).xy, %s, %s);\n",
                           nid, sampler, in0, in1, in2);
    } else if (key_contains_ci(key, "crt") || key_contains_ci(key, "pixelate") ||
               key_contains_ci(key, "halftone")) {
        str_buf_append_fmt(sb,
                           "    vec4 n%u_out0 = wwal_effect_crt_pixelate(%s, (%s).xy, %s, %s);\n",
                           nid, sampler, in0, in1, in2);
    } else if (key_contains_ci(key, "twirl") || key_contains_ci(key, "fisheye") ||
               key_contains_ci(key, "pendulum") || key_contains_ci(key, "perspective")) {
        str_buf_append_fmt(sb,
                           "    vec4 n%u_out0 = texture(%s, clamp(wwal_uv_twirl((%s).xy, "
                           "vec2(0.5), %s, max(%s, 0.45)), vec2(0.0), vec2(1.0)));\n",
                           nid, sampler, in0, in1, in2);
    } else if (key_contains_ci(key, "foliage") || key_contains_ci(key, "scroll") ||
               key_contains_ci(key, "shake") || key_contains_ci(key, "skew")) {
        str_buf_append_fmt(sb,
                           "    vec4 n%u_out0 = texture(%s, clamp((%s).xy + vec2(sin(u_Time * (%s) "
                           "+ (%s).y * (%s)) * (%s), 0.0), vec2(0.0), vec2(1.0)));\n",
                           nid, sampler, in0, in1, in0, in3, in2);
    } else if (key_contains_ci(key, "pulse")) {
        str_buf_append_fmt(
            sb,
            "    vec4 n%u_out0 = texture(%s, clamp(vec2(0.5) + ((%s).xy - vec2(0.5)) / (1.0 + "
            "u_AudioBands[0].x * max(%s, 0.15)), vec2(0.0), vec2(1.0)));\n",
            nid, sampler, in0, in1);
    } else if (key_contains_ci(key, "hsv") || key_contains_ci(key, "tint") ||
               key_contains_ci(key, "colorgrading")) {
        str_buf_append_fmt(sb, "    vec4 n%u_out0 = wwal_hue_shift(texture(%s, (%s).xy), %s);\n",
                           nid, sampler, in0, in1);
    } else if (key_contains_ci(key, "contrast")) {
        str_buf_append_fmt(sb,
                           "    vec4 n%u_out0 = wwal_contrast_saturation(texture(%s, (%s).xy), "
                           "max(%s, 0.1), max(%s, 0.0));\n",
                           nid, sampler, in0, in1, in2);
    } else if (key_contains_ci(key, "godrays") || key_contains_ci(key, "lightshafts") ||
               key_contains_ci(key, "aurora") || key_contains_ci(key, "fire") ||
               key_contains_ci(key, "nitro") || key_contains_ci(key, "cloud")) {
        str_buf_append_fmt(sb, "    vec4 n%u_out0 = wwal_effect_godrays(%s, (%s).xy, %s, %s);\n",
                           nid, sampler, in0, in1, in2);
    } else {
        str_buf_append_fmt(sb,
                           "    vec4 n%u_out0 = wwal_effect_waterwaves(%s, (%s).xy, %s, %s, %s);\n",
                           nid, sampler, in0, in1, in2, in3);
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

    str_buf_t sb;
    if (!str_buf_init(&sb, 16384)) {
        if (out_glsl_len)
            *out_glsl_len = 0;
        return NULL;
    }

    uint32_t active_mask = schedule->active_pass_mask;
    if (active_mask == 0) {
        active_mask = (1u << 4);
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

        str_buf_append_fmt(
            &sb,
            "// =============================================================================\n"
            "#pragma Pass %s\n"
            "// =============================================================================\n"
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
            "// Promoted GraphConstants uniforms (144 Hz zero-recompile scrubbing)\n",
            pass_name);

        uint32_t helper_mask = 0u;
        for (uint32_t k = 0; k < sorted_cnt; ++k) {
            uint32_t idx = schedule->sorted_node_indices[k];
            if (idx >= ncnt || node_pass[idx] != pass)
                continue;
            const waywal_graph_node_t *node = &graph->nodes[idx];
            helper_mask |= helper_mask_for_node(graph, &adj, node_pass, idx);
            if (!(node->flags & WAYWAL_NODE_FLAG_MUTED_BYPASS) &&
                (node->opcode == WAYWAL_NODE_IN_CONSTANT ||
                 node->opcode == WAYWAL_NODE_IN_USER_PROP)) {
                str_buf_append_fmt(&sb, "uniform vec4 u_Node%u_Const;\n", node->node_id);
            }
        }
        str_buf_append(&sb, "\n");

        emit_glsl_helpers(&sb, helper_mask);

        str_buf_append(&sb, "void main() {\n");

        bool emitted_in_pass[WAYWAL_GRAPH_MAX_NODES] = {false};
        int32_t final_node_idx = -1;
        bool has_pass_sink = false;

        for (uint32_t k = 0; k < sorted_cnt; ++k) {
            uint32_t idx = schedule->sorted_node_indices[k];
            if (idx >= ncnt || node_pass[idx] != pass) {
                continue;
            }
            const waywal_graph_node_t *node = &graph->nodes[idx];

            if ((node->flags & WAYWAL_NODE_FLAG_MUTED_BYPASS) && node->output_pin_count > 0) {
                emit_muted_bypass_node(&sb, graph, &adj, node_pass, emitted_in_pass, idx);
                emitted_in_pass[idx] = true;
                final_node_idx = (int32_t)idx;
                continue;
            }

            char ops[5][128];
            uint32_t max_ops = (node->input_pin_count < 5) ? node->input_pin_count : 5;
            for (uint32_t p = 0; p < max_ops; ++p) {
                format_operand_adj(graph, &adj, node_pass, emitted_in_pass, idx, p, ops[p],
                                   sizeof(ops[p]));
            }
            for (uint32_t p = max_ops; p < 5; ++p) {
                ops[p][0] = '0';
                ops[p][1] = '.';
                ops[p][2] = '0';
                ops[p][3] = '\0';
            }

            const char *in0 = ops[0];
            const char *in1 = ops[1];
            const char *in2 = ops[2];
            const char *in3 = ops[3];
            const char *in4 = ops[4];
            uint32_t nid = node->node_id;

            switch (node->opcode) {
            case WAYWAL_NODE_IN_UV:
                str_buf_append_fmt(&sb, "    vec2 n%u_out0 = v_TexCoord;\n", nid);
                break;
            case WAYWAL_NODE_IN_TIME:
                str_buf_append_fmt(&sb, "    float n%u_out0 = u_Time;\n", nid);
                break;
            case WAYWAL_NODE_IN_RESOLUTION:
                str_buf_append_fmt(&sb, "    vec2 n%u_out0 = u_Resolution;\n", nid);
                break;
            case WAYWAL_NODE_IN_POINTER:
                str_buf_append_fmt(&sb, "    vec2 n%u_out0 = u_Pointer;\n", nid);
                break;
            case WAYWAL_NODE_IN_AUDIO_FFT:
                str_buf_append_fmt(&sb, "    vec4 n%u_out0 = u_AudioBands[clamp(int(%s), 0, 3)];\n",
                                   nid, in0);
                str_buf_append_fmt(&sb, "    float n%u_out1 = n%u_out0.x;\n", nid, nid);
                break;
            case WAYWAL_NODE_IN_TEXTURE2D: {
                const char *smp = resolve_pass_buffer_sampler(node->prop_or_asset_key, 0);
                str_buf_append_fmt(&sb, "    vec4 n%u_out0 = texture(%s, %s);\n", nid, smp, in0);
                break;
            }
            case WAYWAL_NODE_IN_PASS_BUFFER: {
                const char *smp = resolve_pass_buffer_sampler(node->prop_or_asset_key, 0);
                str_buf_append_fmt(&sb, "    vec4 n%u_out0 = texture(%s, %s);\n", nid, smp, in0);
                break;
            }
            case WAYWAL_NODE_IN_DEPTH_MAP:
                str_buf_append_fmt(&sb, "    float n%u_out0 = texture(u_DepthMap, %s).r;\n", nid,
                                   in0);
                break;
            case WAYWAL_NODE_IN_CONSTANT:
            case WAYWAL_NODE_IN_USER_PROP:
                str_buf_append_fmt(&sb, "    vec4 n%u_out0 = u_Node%u_Const;\n", nid, nid);
                break;
            case WAYWAL_NODE_MATH_ADD:
                str_buf_append_fmt(&sb, "    vec4 n%u_out0 = (%s + %s);\n", nid, in0, in1);
                break;
            case WAYWAL_NODE_MATH_SUB:
                str_buf_append_fmt(&sb, "    vec4 n%u_out0 = (%s - %s);\n", nid, in0, in1);
                break;
            case WAYWAL_NODE_MATH_MUL:
                str_buf_append_fmt(&sb, "    vec4 n%u_out0 = (%s * %s);\n", nid, in0, in1);
                break;
            case WAYWAL_NODE_MATH_DIV:
                str_buf_append_fmt(
                    &sb,
                    "    vec4 n%u_out0 = (%s / mix(vec4(1e-6), %s, notEqual(%s, vec4(0.0))));\n",
                    nid, in0, in1, in1);
                break;
            case WAYWAL_NODE_MATH_FMA:
                str_buf_append_fmt(&sb, "    vec4 n%u_out0 = (%s * %s + %s);\n", nid, in0, in1,
                                   in2);
                break;
            case WAYWAL_NODE_MATH_MIX:
                str_buf_append_fmt(&sb, "    vec4 n%u_out0 = mix(%s, %s, %s);\n", nid, in0, in1,
                                   in2);
                break;
            case WAYWAL_NODE_MATH_SMOOTHSTEP:
                /* GLSL ES 3.10 §8.3 leaves smoothstep(edge0, edge1, x) undefined when
                 * edge0 >= edge1; order the bounds and invert when reversed. */
                str_buf_append_fmt(
                    &sb,
                    "    float n%u_e0 = %s;\n"
                    "    float n%u_e1 = %s;\n"
                    "    float n%u_lo = min(n%u_e0, n%u_e1);\n"
                    "    float n%u_hi = max(max(n%u_e0, n%u_e1), n%u_lo + 1e-5);\n"
                    "    float n%u_s = smoothstep(n%u_lo, n%u_hi, %s);\n"
                    "    float n%u_out0 = (n%u_e0 <= n%u_e1) ? n%u_s : (1.0 - n%u_s);\n",
                    nid, in0, nid, in1, nid, nid, nid, nid, nid, nid, nid, nid, nid, nid, in2, nid,
                    nid, nid, nid, nid);
                break;
            case WAYWAL_NODE_MATH_CLAMP:
                str_buf_append_fmt(&sb, "    vec4 n%u_out0 = clamp(%s, %s, %s);\n", nid, in0, in1,
                                   in2);
                break;
            case WAYWAL_NODE_MATH_SIN:
                str_buf_append_fmt(&sb, "    vec4 n%u_out0 = sin(%s);\n", nid, in0);
                break;
            case WAYWAL_NODE_MATH_COS:
                str_buf_append_fmt(&sb, "    vec4 n%u_out0 = cos(%s);\n", nid, in0);
                break;
            case WAYWAL_NODE_MATH_POW:
                str_buf_append_fmt(&sb, "    vec4 n%u_out0 = pow(max(%s, vec4(0.0)), vec4(%s));\n",
                                   nid, in0, in1);
                break;
            case WAYWAL_NODE_MATH_DOT:
                str_buf_append_fmt(&sb, "    float n%u_out0 = dot(%s, %s);\n", nid, in0, in1);
                break;
            case WAYWAL_NODE_MATH_CROSS:
                str_buf_append_fmt(&sb, "    vec3 n%u_out0 = cross((%s).xyz, (%s).xyz);\n", nid,
                                   in0, in1);
                break;
            case WAYWAL_NODE_MATH_NORMALIZE:
                str_buf_append_fmt(
                    &sb, "    vec4 n%u_out0 = (length(%s) > 1e-6 ? normalize(%s) : %s);\n", nid,
                    in0, in0, in0);
                break;
            case WAYWAL_NODE_MATH_LENGTH:
                str_buf_append_fmt(&sb, "    float n%u_out0 = length(%s);\n", nid, in0);
                break;
            case WAYWAL_NODE_MATH_DISTANCE:
                str_buf_append_fmt(&sb, "    float n%u_out0 = distance((%s).xy, (%s).xy);\n", nid,
                                   in0, in1);
                break;
            case WAYWAL_NODE_MATH_FRACT:
                str_buf_append_fmt(&sb, "    vec4 n%u_out0 = fract(%s);\n", nid, in0);
                break;
            case WAYWAL_NODE_MATH_FLOOR:
                str_buf_append_fmt(&sb, "    vec4 n%u_out0 = floor(%s);\n", nid, in0);
                break;
            case WAYWAL_NODE_MATH_ABS:
                str_buf_append_fmt(&sb, "    vec4 n%u_out0 = abs(%s);\n", nid, in0);
                break;
            case WAYWAL_NODE_VEC_SWIZZLE_COMBINE:
                str_buf_append_fmt(&sb, "    vec2 n%u_out0 = vec2(%s, %s);\n", nid, in0, in1);
                str_buf_append_fmt(&sb, "    vec3 n%u_out1 = vec3(%s, %s, %s);\n", nid, in0, in1,
                                   in2);
                str_buf_append_fmt(&sb, "    vec4 n%u_out2 = vec4(%s, %s, %s, %s);\n", nid, in0,
                                   in1, in2, in3);
                break;
            case WAYWAL_NODE_NOISE_SIMPLEX2D:
                str_buf_append_fmt(&sb, "    float n%u_out0 = wwal_simplex2d((%s).xy * (%s));\n",
                                   nid, in0, in1);
                break;
            case WAYWAL_NODE_NOISE_CURL2D:
                str_buf_append_fmt(&sb, "    vec2 n%u_out0 = wwal_curl2d((%s).xy * (%s), %s);\n",
                                   nid, in0, in2, in1);
                break;
            case WAYWAL_NODE_NOISE_VORONOI2D:
                str_buf_append_fmt(&sb, "    vec2 n%u_voro = wwal_voronoi2d((%s).xy * (%s));\n",
                                   nid, in0, in1);
                str_buf_append_fmt(&sb, "    float n%u_out0 = n%u_voro.x;\n", nid, nid);
                str_buf_append_fmt(&sb, "    vec2  n%u_out1 = n%u_voro;\n", nid, nid);
                break;
            case WAYWAL_NODE_NOISE_FBM2D:
                str_buf_append_fmt(&sb, "    float n%u_out0 = wwal_fbm2d((%s).xy, int(%s), %s);\n",
                                   nid, in0, in1, in2);
                break;
            case WAYWAL_NODE_SDF_CIRCLE:
                str_buf_append_fmt(&sb, "    float n%u_out0 = length((%s).xy) - (%s);\n", nid, in0,
                                   in1);
                break;
            case WAYWAL_NODE_SDF_BOX:
                str_buf_append_fmt(&sb, "    float n%u_out0 = wwal_sdf_box((%s).xy, (%s).xy);\n",
                                   nid, in0, in1);
                break;
            case WAYWAL_NODE_SDF_RING:
                str_buf_append_fmt(&sb,
                                   "    float n%u_out0 = abs(length((%s).xy) - (%s)) - (%s);\n",
                                   nid, in0, in1, in2);
                break;
            case WAYWAL_NODE_UV_POLAR:
                str_buf_append_fmt(&sb, "    vec2 n%u_out0 = wwal_uv_polar((%s).xy, (%s).xy);\n",
                                   nid, in0, in1);
                break;
            case WAYWAL_NODE_UV_TWIRL:
                str_buf_append_fmt(&sb,
                                   "    vec2 n%u_out0 = wwal_uv_twirl((%s).xy, (%s).xy, %s, %s);\n",
                                   nid, in0, in1, in2, in3);
                break;
            case WAYWAL_NODE_UV_TILE:
                str_buf_append_fmt(&sb, "    vec2 n%u_out0 = fract((%s).xy * (%s).xy);\n", nid, in0,
                                   in1);
                break;
            case WAYWAL_NODE_COLOR_OKLCH_TO_SRGB:
                str_buf_append_fmt(&sb, "    vec4 n%u_out0 = vec4(wwal_oklch_to_srgb(%s), 1.0);\n",
                                   nid, in0);
                break;
            case WAYWAL_NODE_COLOR_SRGB_TO_OKLCH:
                str_buf_append_fmt(&sb, "    vec4 n%u_out0 = vec4(wwal_srgb_to_oklch(%s), 1.0);\n",
                                   nid, in0);
                break;
            case WAYWAL_NODE_COLOR_HUE_SHIFT:
                str_buf_append_fmt(&sb, "    vec4 n%u_out0 = wwal_hue_shift(%s, %s);\n", nid, in0,
                                   in1);
                break;
            case WAYWAL_NODE_COLOR_CONTRAST_SAT:
                str_buf_append_fmt(&sb,
                                   "    vec4 n%u_out0 = wwal_contrast_saturation(%s, %s, %s);\n",
                                   nid, in0, in1, in2);
                break;
            case WAYWAL_NODE_FILTER_DUAL_KAWASE: {
                bool has_smp = false;
                const char *smp = resolve_spatial_sampler(graph, &adj, node_pass, idx, 0, &has_smp);
                if (has_smp && key_contains_ci(node->prop_or_asset_key, "bloom")) {
                    float c1 = finite_or_zero(node->const_val[1]);
                    float glow = (c1 > 0.0f) ? c1 : 0.65f;
                    str_buf_append_fmt(&sb,
                                       "    vec4 n%u_out0 = texture(%s, (%s).xy) + "
                                       "wwal_dual_kawase(%s, (%s).xy, %s) * %.4f;\n",
                                       nid, smp, in1, smp, in1, in2, glow);
                } else if (has_smp) {
                    str_buf_append_fmt(&sb,
                                       "    vec4 n%u_out0 = wwal_dual_kawase(%s, (%s).xy, %s);\n",
                                       nid, smp, in1, in2);
                } else {
                    str_buf_append_fmt(&sb, "    vec4 n%u_out0 = %s;\n", nid, in0);
                }
                break;
            }
            case WAYWAL_NODE_FILTER_CHROMATIC_ABERR: {
                bool has_smp = false;
                const char *smp = resolve_spatial_sampler(graph, &adj, node_pass, idx, 0, &has_smp);
                if (has_smp) {
                    str_buf_append_fmt(
                        &sb, "    vec4 n%u_out0 = wwal_chromatic_aberration(%s, (%s).xy, %s);\n",
                        nid, smp, in1, in2);
                } else {
                    str_buf_append_fmt(&sb, "    vec4 n%u_out0 = %s;\n", nid, in0);
                }
                break;
            }
            case WAYWAL_NODE_LIGHT_NORMAL_FROM_HEIGHT: {
                bool has_smp = false;
                const char *smp = resolve_spatial_sampler(graph, &adj, node_pass, idx, 0, &has_smp);
                if (adj.pin_in_edge[idx][0] >= 0 && has_smp) {
                    str_buf_append_fmt(
                        &sb, "    vec3 n%u_out0 = wwal_normal_from_height_tex(%s, (%s).xy, %s);\n",
                        nid, smp, in1, in2);
                } else {
                    str_buf_append_fmt(
                        &sb, "    vec3 n%u_out0 = wwal_normal_from_height(%s, (%s).xy, %s);\n", nid,
                        in0, in1, in2);
                }
                break;
            }
            case WAYWAL_NODE_LIGHT_BLINN_PHONG:
                str_buf_append_fmt(
                    &sb,
                    "    vec4 n%u_out0 = wwal_blinn_phong((%s).xyz, (%s).xyz, (%s).xyz, %s, %s);\n",
                    nid, in0, in1, in2, in3, in4);
                break;
            case WAYWAL_NODE_BUILTIN_EFFECT: {
                bool has_smp = false;
                const char *smp = resolve_spatial_sampler(graph, &adj, node_pass, idx, 0, &has_smp);
                (void)has_smp;
                emit_builtin_effect_node(&sb, node, smp, in0, in1, in2, in3);
                break;
            }
            case WAYWAL_NODE_OUT_BUFFER_PASS:
                str_buf_append_fmt(&sb, "    outColor = %s;\n", in0);
                has_pass_sink = true;
                break;
            case WAYWAL_NODE_OUT_IMAGE_PASS:
                /* Invariant 2: IMAGE_PASS must force opaque alpha so Wayland surfaces never turn
                 * black/transparent. */
                str_buf_append_fmt(&sb, "    outColor = vec4((%s).rgb, 1.0);\n", in0);
                has_pass_sink = true;
                break;
            default:
                break;
            }

            emitted_in_pass[idx] = true;
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
                    str_buf_append_fmt(&sb, "    outColor = vec4(vec3(n%u_out0), 1.0);\n", fn_id);
                } else if (ftype == WAYWAL_PIN_VEC2) {
                    str_buf_append_fmt(&sb, "    outColor = vec4(n%u_out0, 0.0, 1.0);\n", fn_id);
                } else if (ftype == WAYWAL_PIN_VEC3) {
                    str_buf_append_fmt(&sb, "    outColor = vec4(n%u_out0, 1.0);\n", fn_id);
                } else if (ftype == WAYWAL_PIN_INT || ftype == WAYWAL_PIN_BOOL) {
                    str_buf_append_fmt(&sb, "    outColor = vec4(vec3(float(n%u_out0)), 1.0);\n",
                                       fn_id);
                } else if (pass == 4) {
                    str_buf_append_fmt(&sb, "    outColor = vec4(n%u_out0.rgb, 1.0);\n", fn_id);
                } else {
                    str_buf_append_fmt(&sb, "    outColor = n%u_out0;\n", fn_id);
                }
            } else {
                str_buf_append(&sb, (pass == 4) ? "    outColor = vec4(1.0, 1.0, 1.0, 1.0);\n"
                                                : "    outColor = vec4(0.0, 0.0, 0.0, 1.0);\n");
            }
        }

        str_buf_append(&sb, "}\n\n");
    }

    if (sb.oom) {
        str_buf_free(&sb);
        if (out_glsl_len)
            *out_glsl_len = 0;
        return NULL;
    }

    if (out_glsl_len) {
        *out_glsl_len = sb.len;
    }
    return sb.data;
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

static void utf8_append_codepoint(uint32_t cp, char *out, size_t cap, size_t *io_len)
{
    if (!out || cap <= 1u)
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
    }
}

static bool json_parse_string(const char **p, const char *end, char *out, size_t cap)
{
    json_skip_ws(p, end);
    if (*p >= end || **p != '"')
        return false;
    const char *cur = *p + 1;

    size_t out_len = 0;
    while (cur < end) {
        while ((size_t)(end - cur) >= 8u) {
            uint64_t chunk;
            memcpy(&chunk, cur, 8u);
            if (swar_has_json_special(chunk))
                break;
            if (out && cap > 1u && out_len + 1u < cap) {
                size_t avail = cap - 1u - out_len;
                size_t to_copy = (avail < 8u) ? avail : 8u;
                memcpy(out + out_len, cur, to_copy);
                out_len += to_copy;
            }
            cur += 8u;
        }
        if (cur >= end)
            return false;

        unsigned char c = (unsigned char)*cur;
        if (c == '"') {
            *p = cur + 1;
            if (out && cap > 0u) {
                out[(out_len < cap) ? out_len : (cap - 1u)] = '\0';
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
                utf8_append_codepoint(cp, out, cap, &out_len);
                continue;
            }
            default:
                return false;
            }
            if (single_byte && out && cap > 1u && out_len + 1u < cap) {
                out[out_len++] = decoded;
            }
            cur++;
        } else {
            if (out && cap > 1u && out_len + 1u < cap) {
                out[out_len++] = (char)c;
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

static bool json_skip_value(const char **p, const char *end)
{
    json_skip_ws(p, end);
    if (*p >= end)
        return false;

    if (**p == '"') {
        return json_parse_string(p, end, NULL, 0);
    }
    if (**p == '{' || **p == '[') {
        char open_c = **p;
        char close_c = (open_c == '{') ? '}' : ']';
        (*p)++;
        int depth = 1;
        while (*p < end && depth > 0) {
            if (**p == open_c) {
                depth++;
                (*p)++;
            } else if (**p == close_c) {
                depth--;
                (*p)++;
            } else if (**p == '"') {
                if (!json_parse_string(p, end, NULL, 0))
                    return false;
            } else if (**p == '{' || **p == '[') {
                if (!json_skip_value(p, end))
                    return false;
            } else {
                (*p)++;
            }
        }
        return (depth == 0);
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
