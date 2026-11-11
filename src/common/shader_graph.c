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
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================= */
/* Dynamic String Buffer Helper for Codegen & Serialization                  */
/* ========================================================================= */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} str_buf_t;

static void str_buf_init(str_buf_t *sb, size_t initial_cap)
{
    sb->cap = (initial_cap > 256) ? initial_cap : 256;
    sb->len = 0;
    sb->data = (char *)malloc(sb->cap);
    if (sb->data) {
        sb->data[0] = '\0';
    }
}

static inline void str_buf_free(str_buf_t *sb)
{
    if (sb->data) {
        free(sb->data);
        sb->data = NULL;
    }
    sb->len = 0;
    sb->cap = 0;
}

static bool str_buf_ensure(str_buf_t *sb, size_t extra)
{
    if (sb->len + extra + 1 > sb->cap) {
        size_t new_cap = sb->cap * 2;
        if (new_cap < sb->len + extra + 1) {
            new_cap = sb->len + extra + 1024;
        }
        char *new_data = (char *)realloc(sb->data, new_cap);
        if (!new_data) {
            return false;
        }
        sb->data = new_data;
        sb->cap = new_cap;
    }
    return true;
}

static void str_buf_append(str_buf_t *sb, const char *str)
{
    if (!str)
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
    if (!sb || !sb->data)
        return;
    size_t avail = (sb->cap > sb->len) ? (sb->cap - sb->len) : 0;

    va_list args;
    va_start(args, fmt);
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(sb->data + sb->len, avail, fmt, args_copy);
    va_end(args_copy);

    if (needed >= 0 && (size_t)needed < avail) {
        sb->len += (size_t)needed;
    } else if (needed > 0 && str_buf_ensure(sb, (size_t)needed)) {
        vsnprintf(sb->data + sb->len, (size_t)needed + 1, fmt, args);
        sb->len += (size_t)needed;
    }
    va_end(args);
}

static void str_buf_append_json_escaped(str_buf_t *sb, const char *str)
{
    if (!sb)
        return;
    const char *s = str ? str : "";
    size_t worst = strlen(s) * 6 + 3;
    if (!str_buf_ensure(sb, worst))
        return;

    char *dst = sb->data + sb->len;
    *dst++ = '"';
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; ++p) {
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

int32_t waywal_shader_graph_add_node(waywal_shader_graph_t *graph, uint32_t opcode,
                                     const char *title, float x, float y)
{
    if (!graph || graph->node_count >= WAYWAL_GRAPH_MAX_NODES) {
        return -1;
    }

    uint32_t max_id = 0;
    for (uint32_t i = 0; i < graph->node_count; ++i) {
        if (graph->nodes[i].node_id > max_id) {
            max_id = graph->nodes[i].node_id;
        }
    }
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

    uint32_t dst_nid = graph->edges[target_idx].dst_node_id;
    uint32_t dst_pid = graph->edges[target_idx].dst_pin_index;

    for (uint32_t i = (uint32_t)target_idx; i + 1 < graph->edge_count; ++i) {
        graph->edges[i] = graph->edges[i + 1];
    }
    graph->edge_count--;

    /* Update connected flag on dst pin */
    waywal_graph_node_t *dst_node = waywal_shader_graph_find_node(graph, dst_nid);
    if (dst_node && dst_pid < dst_node->input_pin_count) {
        dst_node->inputs[dst_pid].flags &= ~WAYWAL_PIN_FLAG_CONNECTED;
    }
    return true;
}

bool waywal_shader_graph_disconnect_pins(waywal_shader_graph_t *graph, uint32_t dst_node_id,
                                         uint32_t dst_pin_idx)
{
    if (!graph)
        return false;
    bool found = false;
    for (uint32_t i = 0; i < graph->edge_count;) {
        if (graph->edges[i].dst_node_id == dst_node_id &&
            graph->edges[i].dst_pin_index == dst_pin_idx) {
            waywal_shader_graph_disconnect(graph, graph->edges[i].edge_id);
            found = true;
        } else {
            i++;
        }
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

bool waywal_shader_graph_analyze(const waywal_shader_graph_t *graph_in,
                                 waywal_graph_schedule_t *out_schedule)
{
    if (!graph_in || !out_schedule)
        return false;
    memset(out_schedule, 0, sizeof(*out_schedule));

    uint32_t ncnt = (graph_in->node_count <= WAYWAL_GRAPH_MAX_NODES) ? graph_in->node_count
                                                                     : WAYWAL_GRAPH_MAX_NODES;
    uint32_t ecnt = (graph_in->edge_count <= WAYWAL_GRAPH_MAX_EDGES) ? graph_in->edge_count
                                                                     : WAYWAL_GRAPH_MAX_EDGES;

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

    /* 1. Iterative O(|V| + |E|) backward reachability from terminal sinks */
    bool reachable[WAYWAL_GRAPH_MAX_NODES] = {false};
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

            for (int16_t e = adj.head_in[cur]; e >= 0; e = adj.next_in[e]) {
                int16_t s = adj.edge_src_idx[e];
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

    /* 2. Spatial Convolution Barrier Stage Assignment */
    uint32_t barrier_stage[WAYWAL_GRAPH_MAX_NODES] = {0};
    for (uint32_t iter = 0; iter < ncnt; ++iter) {
        bool changed = false;
        for (uint32_t e = 0; e < ecnt; ++e) {
            int16_t s = adj.edge_src_idx[e];
            int16_t d = adj.edge_dst_idx[e];
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

    /* Pull non-barrier leaf inputs (e.g. constants/UVs feeding filter parameters) up to consumer
     * stage. */
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
            for (int16_t e = adj.head_out[u]; e >= 0; e = adj.next_out[e]) {
                if (graph_in->edges[e].flags & WAYWAL_EDGE_FLAG_FEEDBACK_LATCH)
                    continue;
                int16_t d = adj.edge_dst_idx[e];
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

    uint32_t target_pass[WAYWAL_GRAPH_MAX_NODES] = {0};
    for (uint32_t i = 0; i < ncnt; ++i) {
        if (!reachable[i])
            continue;
        uint32_t op = graph_in->nodes[i].opcode;
        if (op == WAYWAL_NODE_OUT_IMAGE_PASS) {
            target_pass[i] = 4;
        } else if (op == WAYWAL_NODE_OUT_BUFFER_PASS) {
            target_pass[i] = resolve_buffer_pass_index(&graph_in->nodes[i]);
        } else if (!reaches_image_sink[i] && explicit_buf_pass[i] >= 0) {
            target_pass[i] = (uint32_t)explicit_buf_pass[i];
        } else if (max_image_stage == 0) {
            target_pass[i] = 4;
        } else {
            target_pass[i] = (barrier_stage[i] >= max_image_stage)
                                 ? 4u
                                 : ((barrier_stage[i] <= 3u) ? barrier_stage[i] : 3u);
        }
        out_schedule->active_pass_mask |= (1u << target_pass[i]);
    }

    if (out_schedule->active_pass_mask == 0) {
        out_schedule->active_pass_mask = (1u << 4);
    }

    /* 3. Compute Sethi-Ullman register weights in O(|V| + |E|) */
    uint32_t sethi_ullman[WAYWAL_GRAPH_MAX_NODES] = {0};
    bool visiting[WAYWAL_GRAPH_MAX_NODES] = {false};
    for (uint32_t i = 0; i < ncnt; ++i) {
        if (reachable[i] && sethi_ullman[i] == 0) {
            compute_sethi_ullman_adj(graph_in, &adj, i, reachable, sethi_ullman, visiting);
        }
    }

    /* 4. Kahn's Topological Sort with Sethi-Ullman Ready Queue per pass */
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

    /* 5. Static ALU ops & Texture fetch estimation */
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

    waywal_shader_graph_t *graph_mut = (waywal_shader_graph_t *)graph_in;
    for (uint32_t i = 0; i < ncnt; ++i) {
        if (reachable[i]) {
            graph_mut->nodes[i].target_pass = target_pass[i];
            graph_mut->nodes[i].sethi_ullman_num = sethi_ullman[i];
        }
    }

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
                                           const graph_adj_t *adj, uint32_t node_idx,
                                           uint32_t pin_idx, bool *out_has_sampler)
{
    const waywal_graph_node_t *dst = &graph->nodes[node_idx];
    if (pin_idx < WAYWAL_GRAPH_MAX_PINS_PER_NODE) {
        int16_t e_idx = adj->pin_in_edge[node_idx][pin_idx];
        if (e_idx >= 0) {
            int16_t s_idx = adj->edge_src_idx[e_idx];
            if (s_idx >= 0) {
                const waywal_graph_node_t *src = &graph->nodes[s_idx];
                if (src->target_pass != dst->target_pass) {
                    if (out_has_sampler)
                        *out_has_sampler = true;
                    return resolve_pass_buffer_sampler(NULL, src->target_pass);
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
        *out_has_sampler = true;
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

static void emit_glsl_helpers(str_buf_t *sb)
{
    str_buf_append(sb,
                   "// --- Math, Spatial Filter & Built-In Effect Helper Functions ---\n"
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
                   "}\n\n"
                   "vec3 wwal_srgb_to_oklch(vec4 srgb) {\n"
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
                   "}\n\n"
                   "float wwal_hash21(vec2 p) {\n"
                   "    vec3 p3 = fract(vec3(p.xyx) * 0.1031);\n"
                   "    p3 += dot(p3, p3.yzx + 33.33);\n"
                   "    return fract((p3.x + p3.y) * p3.z);\n"
                   "}\n\n"
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
                   "}\n\n"
                   "vec2 wwal_curl2d(vec2 p, float t) {\n"
                   "    const float eps = 0.005;\n"
                   "    float n1 = wwal_simplex2d(p + vec2(0.0, eps) + vec2(t * 0.1));\n"
                   "    float n2 = wwal_simplex2d(p - vec2(0.0, eps) + vec2(t * 0.1));\n"
                   "    float n3 = wwal_simplex2d(p + vec2(eps, 0.0) + vec2(t * 0.1));\n"
                   "    float n4 = wwal_simplex2d(p - vec2(eps, 0.0) + vec2(t * 0.1));\n"
                   "    float dy = (n1 - n2) / (2.0 * eps);\n"
                   "    float dx = (n3 - n4) / (2.0 * eps);\n"
                   "    return vec2(-dy, dx);\n"
                   "}\n\n"
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
                       "}\n\n"
                       "float wwal_sdf_box(vec2 p, vec2 b) {\n"
                       "    vec2 d = abs(p) - b;\n"
                       "    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);\n"
                       "}\n\n"
                       "vec2 wwal_uv_polar(vec2 uv, vec2 center) {\n"
                       "    vec2 d = uv - center;\n"
                       "    float r = length(d);\n"
                       "    float a = atan(d.y, d.x) / 6.28318530718 + 0.5;\n"
                       "    return vec2(a, r);\n"
                       "}\n\n"
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
                       "}\n\n"
                       "vec4 wwal_hue_shift(vec4 col, float shift) {\n"
                       "    vec3 oklch = wwal_srgb_to_oklch(col);\n"
                       "    oklch.z += shift;\n"
                       "    return vec4(wwal_oklch_to_srgb(vec4(oklch, 1.0)), col.a);\n"
                       "}\n\n"
                       "vec4 wwal_contrast_saturation(vec4 col, float c, float s) {\n"
                       "    vec3 rgb = (col.rgb - 0.5) * c + 0.5;\n"
                       "    float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));\n"
                       "    rgb = mix(vec3(luma), rgb, s);\n"
                       "    return vec4(clamp(rgb, 0.0, 1.0), col.a);\n"
                       "}\n\n");
    str_buf_append(sb,
                   "vec4 wwal_dual_kawase(sampler2D tex, vec2 uv, float offset) {\n"
                   "    vec2 halfpixel = max(offset, 0.0) / max(u_Resolution, vec2(1.0));\n"
                   "    vec4 sum = texture(tex, uv) * 4.0;\n"
                   "    sum += texture(tex, clamp(uv - halfpixel.xy, vec2(0.0), vec2(1.0)));\n"
                   "    sum += texture(tex, clamp(uv + halfpixel.xy, vec2(0.0), vec2(1.0)));\n"
                   "    sum += texture(tex, clamp(uv + vec2(halfpixel.x, -halfpixel.y), vec2(0.0), "
                   "vec2(1.0)));\n"
                   "    sum += texture(tex, clamp(uv - vec2(halfpixel.x, -halfpixel.y), vec2(0.0), "
                   "vec2(1.0)));\n"
                   "    return sum * 0.125;\n"
                   "}\n\n"
                   "vec4 wwal_chromatic_aberration(sampler2D tex, vec2 uv, float intensity) {\n"
                   "    vec2 dir = (uv - vec2(0.5)) * intensity;\n"
                   "    float r = texture(tex, clamp(uv + dir, vec2(0.0), vec2(1.0))).r;\n"
                   "    vec4 center = texture(tex, uv);\n"
                   "    float b = texture(tex, clamp(uv - dir, vec2(0.0), vec2(1.0))).b;\n"
                   "    return vec4(r, center.g, b, center.a);\n"
                   "}\n\n"
                   "vec3 wwal_normal_from_height_tex(sampler2D tex, vec2 uv, float strength) {\n"
                   "    vec2 texel = 1.0 / max(u_Resolution, vec2(1.0));\n"
                   "    float hL = texture(tex, uv - vec2(texel.x, 0.0)).x;\n"
                   "    float hR = texture(tex, uv + vec2(texel.x, 0.0)).x;\n"
                   "    float hD = texture(tex, uv - vec2(0.0, texel.y)).x;\n"
                   "    float hU = texture(tex, uv + vec2(0.0, texel.y)).x;\n"
                   "    return normalize(vec3((hL - hR) * strength, (hD - hU) * strength, 1.0));\n"
                   "}\n\n"
                   "vec3 wwal_normal_from_height(float h, vec2 uv, float strength) {\n"
                   "    float dx = dFdx(h + uv.x * 0.0) * strength;\n"
                   "    float dy = dFdy(h + uv.y * 0.0) * strength;\n"
                   "    return normalize(vec3(-dx, -dy, 1.0));\n"
                   "}\n\n"
                   "vec4 wwal_blinn_phong(vec3 n, vec3 l, vec3 v, vec4 albedo, float shininess) {\n"
                   "    vec3 nn = normalize(n);\n"
                   "    vec3 nl = normalize(l);\n"
                   "    vec3 nv = normalize(v);\n"
                   "    vec3 h = normalize(nl + nv);\n"
                   "    float diff = max(dot(nn, nl), 0.0);\n"
                   "    float spec = pow(max(dot(nn, h), 0.0), max(shininess, 1.0));\n"
                   "    vec3 col = albedo.rgb * (0.15 + 0.85 * diff) + vec3(spec * 0.5);\n"
                   "    return vec4(col, albedo.a);\n"
                   "}\n\n");
    str_buf_append(
        sb, "vec4 wwal_effect_waterripple(sampler2D tex, vec2 uv_base, vec2 uv_in, float speed, "
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
            "}\n\n"
            "vec4 wwal_effect_waterwaves(sampler2D tex, vec2 uv, float speed, float amp, float "
            "freq) {\n"
            "    float f = max(freq, 1.0);\n"
            "    vec2 disp = vec2(sin(uv.y * f + u_Time * speed), cos(uv.x * f * 0.85 - u_Time * "
            "speed * 0.9)) * amp;\n"
            "    vec4 base = texture(tex, clamp(uv + disp, vec2(0.0), vec2(1.0)));\n"
            "    float crest = 0.5 + 0.5 * sin((uv.x + uv.y) * f - u_Time * speed);\n"
            "    return vec4(clamp(base.rgb + vec3(0.04, 0.10, 0.16) * crest, 0.0, 1.0), 1.0);\n"
            "}\n\n"
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
            "}\n\n"
            "vec4 wwal_effect_vignette(sampler2D tex, vec2 uv, float radius, float softness) {\n"
            "    vec4 c = texture(tex, uv);\n"
            "    float d = length(uv - vec2(0.5));\n"
            "    float r = (radius > 0.0) ? radius : 0.75;\n"
            "    float s = max(softness, 0.15);\n"
            "    float vig = smoothstep(r, max(r - s, 0.0), d);\n"
            "    return vec4(c.rgb * vig, 1.0);\n"
            "}\n\n"
            "vec4 wwal_effect_crt_pixelate(sampler2D tex, vec2 uv, float density, float "
            "scan_strength) {\n"
            "    float cells = max(density, 64.0);\n"
            "    vec2 q_uv = (floor(uv * cells) + 0.5) / cells;\n"
            "    vec4 c = texture(tex, clamp(q_uv, vec2(0.0), vec2(1.0)));\n"
            "    float scan = 1.0 - clamp(scan_strength, 0.0, 0.75) * (0.5 + 0.5 * sin(uv.y * "
            "max(u_Resolution.y, 240.0) * 3.14159265));\n"
            "    return vec4(c.rgb * scan, 1.0);\n"
            "}\n\n"
            "vec4 wwal_effect_godrays(sampler2D tex, vec2 uv, float intensity, float decay) {\n"
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

static void format_operand_adj(const waywal_shader_graph_t *graph, const graph_adj_t *adj,
                               const bool *emitted_in_pass, uint32_t dst_idx, uint32_t pin_idx,
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
                uint32_t dst_type = dst_pin->pin_type;
                char raw_var[64];

                if (src_node->target_pass != dst_node->target_pass || !emitted_in_pass[s_idx]) {
                    const char *buf_name =
                        resolve_pass_buffer_sampler((src_node->opcode == WAYWAL_NODE_IN_PASS_BUFFER)
                                                        ? src_node->prop_or_asset_key
                                                        : NULL,
                                                    src_node->target_pass);
                    snprintf(raw_var, sizeof(raw_var), "texture(%s, v_TexCoord)", buf_name);
                    src_type = WAYWAL_PIN_VEC4;
                } else {
                    snprintf(raw_var, sizeof(raw_var), "n%u_out%u", incoming_edge->src_node_id,
                             incoming_edge->src_pin_index);
                }

                if (src_type == dst_type) {
                    snprintf(out_expr, out_cap, "%s", raw_var);
                } else if (src_type == WAYWAL_PIN_FLOAT && dst_type == WAYWAL_PIN_VEC2) {
                    snprintf(out_expr, out_cap, "vec2(%s)", raw_var);
                } else if (src_type == WAYWAL_PIN_FLOAT && dst_type == WAYWAL_PIN_VEC3) {
                    snprintf(out_expr, out_cap, "vec3(%s)", raw_var);
                } else if (src_type == WAYWAL_PIN_FLOAT && dst_type == WAYWAL_PIN_VEC4) {
                    snprintf(out_expr, out_cap, "vec4(vec3(%s), 1.0)", raw_var);
                } else if (src_type == WAYWAL_PIN_VEC2 && dst_type == WAYWAL_PIN_VEC3) {
                    snprintf(out_expr, out_cap, "vec3(%s, 0.0)", raw_var);
                } else if (src_type == WAYWAL_PIN_VEC2 && dst_type == WAYWAL_PIN_VEC4) {
                    snprintf(out_expr, out_cap, "vec4(%s, 0.0, 1.0)", raw_var);
                } else if (src_type == WAYWAL_PIN_VEC3 && dst_type == WAYWAL_PIN_VEC4) {
                    snprintf(out_expr, out_cap, "vec4(%s, 1.0)", raw_var);
                } else if (src_type == WAYWAL_PIN_VEC4 && dst_type == WAYWAL_PIN_VEC3) {
                    snprintf(out_expr, out_cap, "(%s).xyz", raw_var);
                } else if (src_type == WAYWAL_PIN_VEC4 && dst_type == WAYWAL_PIN_VEC2) {
                    snprintf(out_expr, out_cap, "(%s).xy", raw_var);
                } else if (src_type == WAYWAL_PIN_VEC4 && dst_type == WAYWAL_PIN_FLOAT) {
                    snprintf(out_expr, out_cap, "(%s).x", raw_var);
                } else if (src_type == WAYWAL_PIN_VEC3 && dst_type == WAYWAL_PIN_VEC2) {
                    snprintf(out_expr, out_cap, "(%s).xy", raw_var);
                } else if (src_type == WAYWAL_PIN_VEC3 && dst_type == WAYWAL_PIN_FLOAT) {
                    snprintf(out_expr, out_cap, "(%s).x", raw_var);
                } else if (src_type == WAYWAL_PIN_VEC2 && dst_type == WAYWAL_PIN_FLOAT) {
                    snprintf(out_expr, out_cap, "(%s).x", raw_var);
                } else if (src_type == WAYWAL_PIN_AUDIO_SPECTRUM && dst_type == WAYWAL_PIN_FLOAT) {
                    snprintf(out_expr, out_cap, "(%s).x", raw_var);
                } else if (src_type == WAYWAL_PIN_INT && dst_type == WAYWAL_PIN_FLOAT) {
                    snprintf(out_expr, out_cap, "float(%s)", raw_var);
                } else if (src_type == WAYWAL_PIN_BOOL && dst_type == WAYWAL_PIN_FLOAT) {
                    snprintf(out_expr, out_cap, "(%s ? 1.0 : 0.0)", raw_var);
                } else {
                    snprintf(out_expr, out_cap, "%s", raw_var);
                }
                return;
            }
        }
    }

    switch (dst_pin->pin_type) {
    case WAYWAL_PIN_FLOAT:
        snprintf(out_expr, out_cap, "%.6f", dst_pin->default_val[0]);
        break;
    case WAYWAL_PIN_VEC2:
        if (dst_node->opcode == WAYWAL_NODE_IN_TEXTURE2D ||
            dst_node->opcode == WAYWAL_NODE_IN_PASS_BUFFER ||
            dst_node->opcode == WAYWAL_NODE_IN_DEPTH_MAP || strcasecmp(dst_pin->name, "uv") == 0) {
            snprintf(out_expr, out_cap, "v_TexCoord");
        } else {
            snprintf(out_expr, out_cap, "vec2(%.6f, %.6f)", dst_pin->default_val[0],
                     dst_pin->default_val[1]);
        }
        break;
    case WAYWAL_PIN_VEC3:
        snprintf(out_expr, out_cap, "vec3(%.6f, %.6f, %.6f)", dst_pin->default_val[0],
                 dst_pin->default_val[1], dst_pin->default_val[2]);
        break;
    case WAYWAL_PIN_VEC4:
    case WAYWAL_PIN_AUDIO_SPECTRUM:
        snprintf(out_expr, out_cap, "vec4(%.6f, %.6f, %.6f, %.6f)", dst_pin->default_val[0],
                 dst_pin->default_val[1], dst_pin->default_val[2], dst_pin->default_val[3]);
        break;
    case WAYWAL_PIN_INT:
        snprintf(out_expr, out_cap, "%d", (int)dst_pin->default_val[0]);
        break;
    case WAYWAL_PIN_BOOL:
        snprintf(out_expr, out_cap, "%s", (dst_pin->default_val[0] != 0.0f) ? "true" : "false");
        break;
    case WAYWAL_PIN_SAMPLER2D:
        snprintf(out_expr, out_cap, "u_BufferA");
        break;
    default:
        snprintf(out_expr, out_cap, "0.0");
        break;
    }
}

static void emit_builtin_effect_node(str_buf_t *sb, const waywal_graph_node_t *node,
                                     const char *sampler, const char *in0, const char *in1,
                                     const char *in2, const char *in3)
{
    uint32_t nid = node->node_id;
    const char *key = node->prop_or_asset_key;

    str_buf_append_fmt(sb, "    // Builtin Effect: %s\n", (key[0] != '\0') ? key : "waterripple");

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

    graph_adj_t adj;
    graph_adj_build(graph, &adj);

    str_buf_t sb;
    str_buf_init(&sb, 24576);

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

        for (uint32_t k = 0; k < schedule->sorted_count; ++k) {
            uint32_t idx = schedule->sorted_node_indices[k];
            const waywal_graph_node_t *node = &graph->nodes[idx];
            if (node->target_pass == pass) {
                if (node->opcode == WAYWAL_NODE_IN_CONSTANT ||
                    node->opcode == WAYWAL_NODE_IN_USER_PROP) {
                    str_buf_append_fmt(&sb, "uniform vec4 u_Node%u_Const;\n", node->node_id);
                } else if (node->opcode == WAYWAL_NODE_NOISE_CURL2D) {
                    str_buf_append_fmt(&sb, "uniform float u_Node%u_Scale;\n", node->node_id);
                }
            }
        }
        str_buf_append(&sb, "\n");

        emit_glsl_helpers(&sb);

        str_buf_append(&sb, "void main() {\n");

        bool emitted_in_pass[WAYWAL_GRAPH_MAX_NODES] = {false};
        int32_t final_node_idx = -1;
        bool has_pass_sink = false;

        for (uint32_t k = 0; k < schedule->sorted_count; ++k) {
            uint32_t idx = schedule->sorted_node_indices[k];
            const waywal_graph_node_t *node = &graph->nodes[idx];
            if (node->target_pass != pass) {
                continue;
            }

            char ops[5][128];
            uint32_t max_ops = (node->input_pin_count < 5) ? node->input_pin_count : 5;
            for (uint32_t p = 0; p < max_ops; ++p) {
                format_operand_adj(graph, &adj, emitted_in_pass, idx, p, ops[p], sizeof(ops[p]));
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
                str_buf_append_fmt(&sb, "    float n%u_out0 = smoothstep(%s, %s, %s);\n", nid, in0,
                                   in1, in2);
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
                const char *smp = resolve_spatial_sampler(graph, &adj, idx, 0, &has_smp);
                if (has_smp && key_contains_ci(node->prop_or_asset_key, "bloom")) {
                    float glow = (node->const_val[1] > 0.0f) ? node->const_val[1] : 0.65f;
                    str_buf_append_fmt(&sb,
                                       "    vec4 n%u_out0 = texture(%s, (%s).xy) + "
                                       "wwal_dual_kawase(%s, (%s).xy, %s) * %.4f;\n",
                                       nid, smp, in1, smp, in1, in2, glow);
                } else if (has_smp) {
                    str_buf_append_fmt(&sb,
                                       "    vec4 n%u_out0 = wwal_dual_kawase(%s, (%s).xy, %s);\n",
                                       nid, smp, in1, in2);
                } else {
                    str_buf_append_fmt(
                        &sb,
                        "    vec4 n%u_out0 = mix(%s, wwal_dual_kawase(%s, (%s).xy, %s), 0.5);\n",
                        nid, in0, smp, in1, in2);
                }
                break;
            }
            case WAYWAL_NODE_FILTER_CHROMATIC_ABERR: {
                bool has_smp = false;
                const char *smp = resolve_spatial_sampler(graph, &adj, idx, 0, &has_smp);
                if (has_smp) {
                    str_buf_append_fmt(
                        &sb, "    vec4 n%u_out0 = wwal_chromatic_aberration(%s, (%s).xy, %s);\n",
                        nid, smp, in1, in2);
                } else {
                    str_buf_append_fmt(&sb,
                                       "    vec4 n%u_out0 = mix(%s, wwal_chromatic_aberration(%s, "
                                       "(%s).xy, %s), 0.5);\n",
                                       nid, in0, smp, in1, in2);
                }
                break;
            }
            case WAYWAL_NODE_LIGHT_NORMAL_FROM_HEIGHT: {
                bool has_smp = false;
                const char *smp = resolve_spatial_sampler(graph, &adj, idx, 0, &has_smp);
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
                const char *smp = resolve_spatial_sampler(graph, &adj, idx, 0, &has_smp);
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
    str_buf_init(&sb, 8192);

    str_buf_append(&sb, "{\n");
    str_buf_append_fmt(&sb, "  \"schema_version\": %u,\n", graph->schema_version);
    str_buf_append(&sb, "  \"graph_name\": ");
    str_buf_append_json_escaped(&sb, graph->graph_name);
    str_buf_append(&sb, ",\n  \"author\": ");
    str_buf_append_json_escaped(&sb, graph->author);
    str_buf_append_fmt(&sb, ",\n  \"flags\": %u,\n", graph->flags);

    str_buf_append(&sb, "  \"nodes\": [\n");
    for (uint32_t i = 0; i < graph->node_count; ++i) {
        const waywal_graph_node_t *n = &graph->nodes[i];
        str_buf_append(&sb, "    {\n");
        str_buf_append_fmt(&sb, "      \"node_id\": %u,\n", n->node_id);
        str_buf_append_fmt(&sb, "      \"opcode\": %u,\n", n->opcode);
        str_buf_append(&sb, "      \"title\": ");
        str_buf_append_json_escaped(&sb, n->title);
        str_buf_append(&sb, ",\n      \"prop_or_asset_key\": ");
        str_buf_append_json_escaped(&sb, n->prop_or_asset_key);
        str_buf_append_fmt(&sb, ",\n      \"canvas_pos\": [%.2f, %.2f],\n", n->canvas_pos[0],
                           n->canvas_pos[1]);
        str_buf_append_fmt(&sb, "      \"target_pass\": %u,\n", n->target_pass);
        str_buf_append_fmt(&sb, "      \"flags\": %u,\n", n->flags);
        str_buf_append_fmt(&sb, "      \"const_val\": [%.4f, %.4f, %.4f, %.4f]\n", n->const_val[0],
                           n->const_val[1], n->const_val[2], n->const_val[3]);
        str_buf_append_fmt(&sb, "    }%s\n", (i + 1 < graph->node_count) ? "," : "");
    }
    str_buf_append(&sb, "  ],\n");

    str_buf_append(&sb, "  \"edges\": [\n");
    for (uint32_t i = 0; i < graph->edge_count; ++i) {
        const waywal_graph_edge_t *e = &graph->edges[i];
        str_buf_append(&sb, "    { ");
        str_buf_append_fmt(&sb, "\"edge_id\": %u, ", e->edge_id);
        str_buf_append_fmt(&sb, "\"src_node_id\": %u, ", e->src_node_id);
        str_buf_append_fmt(&sb, "\"src_pin_index\": %u, ", e->src_pin_index);
        str_buf_append_fmt(&sb, "\"dst_node_id\": %u, ", e->dst_node_id);
        str_buf_append_fmt(&sb, "\"dst_pin_index\": %u, ", e->dst_pin_index);
        str_buf_append_fmt(&sb, "\"swizzle_mask\": %u", e->swizzle_mask);
        str_buf_append_fmt(&sb, " }%s\n", (i + 1 < graph->edge_count) ? "," : "");
    }
    str_buf_append(&sb, "  ]\n");
    str_buf_append(&sb, "}\n");

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

static bool json_parse_string(const char **p, const char *end, char *out, size_t cap)
{
    json_skip_ws(p, end);
    if (*p >= end || **p != '"')
        return false;
    (*p)++;

    size_t out_len = 0;
    while (*p < end && **p != '"') {
        if (**p == '\\') {
            if (*p + 1 >= end)
                return false;
            (*p)++;
            char esc = **p;
            char decoded = esc;
            if (esc == '"' || esc == '\\' || esc == '/') {
                decoded = esc;
            } else if (esc == 'b') {
                decoded = '\b';
            } else if (esc == 'f') {
                decoded = '\f';
            } else if (esc == 'n') {
                decoded = '\n';
            } else if (esc == 'r') {
                decoded = '\r';
            } else if (esc == 't') {
                decoded = '\t';
            } else if (esc == 'u' && *p + 4 < end) {
                int h0 = hex_digit_val((*p)[1]);
                int h1 = hex_digit_val((*p)[2]);
                int h2 = hex_digit_val((*p)[3]);
                int h3 = hex_digit_val((*p)[4]);
                if (h0 >= 0 && h1 >= 0 && h2 >= 0 && h3 >= 0) {
                    unsigned int cp = (unsigned int)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                    if (cp < 0x80u) {
                        decoded = (char)cp;
                    } else {
                        decoded = '?';
                    }
                    *p += 4;
                }
            }
            if (out_len + 1 < cap) {
                out[out_len++] = decoded;
            }
        } else {
            if (out_len + 1 < cap) {
                out[out_len++] = **p;
            }
        }
        (*p)++;
    }
    if (*p >= end || **p != '"')
        return false;
    (*p)++;
    if (out_len < cap) {
        out[out_len] = '\0';
    } else if (cap > 0) {
        out[cap - 1] = '\0';
    }
    return true;
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
    *out_val = strtod(num_buf, &endptr);
    if (endptr == num_buf)
        return false;
    *p += (size_t)(endptr - num_buf);
    return true;
}

static bool json_parse_uint(const char **p, const char *end, uint32_t *out_val)
{
    double d = 0;
    if (!json_parse_number(p, end, &d))
        return false;
    if (d < 0)
        return false;
    *out_val = (uint32_t)d;
    return true;
}

static void json_skip_value(const char **p, const char *end)
{
    json_skip_ws(p, end);
    if (*p >= end)
        return;

    if (**p == '"') {
        char dummy[16];
        json_parse_string(p, end, dummy, sizeof(dummy));
    } else if (**p == '{') {
        (*p)++;
        int depth = 1;
        while (*p < end && depth > 0) {
            if (**p == '{')
                depth++;
            else if (**p == '}')
                depth--;
            else if (**p == '"') {
                char dummy[16];
                json_parse_string(p, end, dummy, sizeof(dummy));
                continue;
            }
            (*p)++;
        }
    } else if (**p == '[') {
        (*p)++;
        int depth = 1;
        while (*p < end && depth > 0) {
            if (**p == '[')
                depth++;
            else if (**p == ']')
                depth--;
            else if (**p == '"') {
                char dummy[16];
                json_parse_string(p, end, dummy, sizeof(dummy));
                continue;
            }
            (*p)++;
        }
    } else {
        while (*p < end && **p != ',' && **p != '}' && **p != ']' && !isspace((unsigned char)**p)) {
            (*p)++;
        }
    }
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

    while (p < end) {
        json_skip_ws(&p, end);
        if (p >= end)
            break;
        if (*p == '}') {
            p++;
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
            json_parse_string(&p, end, out_graph->graph_name, sizeof(out_graph->graph_name));
        } else if (strcmp(key, "author") == 0) {
            json_parse_string(&p, end, out_graph->author, sizeof(out_graph->author));
        } else if (strcmp(key, "flags") == 0) {
            json_parse_uint(&p, end, &out_graph->flags);
        } else if (strcmp(key, "nodes") == 0) {
            if (!json_expect_char(&p, end, '[')) {
                if (err_buf)
                    snprintf(err_buf, 256, "Expected '[' for nodes array");
                return false;
            }

            while (p < end) {
                json_skip_ws(&p, end);
                if (p >= end)
                    break;
                if (*p == ']') {
                    p++;
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

                while (p < end) {
                    json_skip_ws(&p, end);
                    if (p >= end)
                        break;
                    if (*p == '}') {
                        p++;
                        break;
                    }

                    char nkey[64];
                    if (!json_parse_string(&p, end, nkey, sizeof(nkey)))
                        break;
                    if (!json_expect_char(&p, end, ':'))
                        break;

                    if (strcmp(nkey, "node_id") == 0 || strcmp(nkey, "id") == 0) {
                        json_parse_uint(&p, end, &n->node_id);
                    } else if (strcmp(nkey, "opcode") == 0) {
                        json_parse_uint(&p, end, &n->opcode);
                    } else if (strcmp(nkey, "title") == 0) {
                        json_parse_string(&p, end, n->title, sizeof(n->title));
                    } else if (strcmp(nkey, "prop_or_asset_key") == 0) {
                        json_parse_string(&p, end, n->prop_or_asset_key,
                                          sizeof(n->prop_or_asset_key));
                    } else if (strcmp(nkey, "canvas_pos") == 0) {
                        if (json_expect_char(&p, end, '[')) {
                            double x = 0, y = 0;
                            json_parse_number(&p, end, &x);
                            json_expect_char(&p, end, ',');
                            json_parse_number(&p, end, &y);
                            json_expect_char(&p, end, ']');
                            n->canvas_pos[0] = (float)x;
                            n->canvas_pos[1] = (float)y;
                        }
                    } else if (strcmp(nkey, "target_pass") == 0) {
                        json_parse_uint(&p, end, &n->target_pass);
                    } else if (strcmp(nkey, "flags") == 0) {
                        json_parse_uint(&p, end, &n->flags);
                    } else if (strcmp(nkey, "const_val") == 0) {
                        if (json_expect_char(&p, end, '[')) {
                            double c[4] = {0, 0, 0, 1};
                            for (int ci = 0; ci < 4; ++ci) {
                                json_parse_number(&p, end, &c[ci]);
                                if (ci < 3)
                                    json_expect_char(&p, end, ',');
                            }
                            json_expect_char(&p, end, ']');
                            for (int ci = 0; ci < 4; ++ci)
                                n->const_val[ci] = (float)c[ci];
                        }
                    } else {
                        json_skip_value(&p, end);
                    }

                    json_expect_char(&p, end, ',');
                }

                node_init_default_pins(n);
                out_graph->node_count++;
                json_expect_char(&p, end, ',');
            }
        } else if (strcmp(key, "edges") == 0) {
            if (!json_expect_char(&p, end, '[')) {
                if (err_buf)
                    snprintf(err_buf, 256, "Expected '[' for edges array");
                return false;
            }

            while (p < end) {
                json_skip_ws(&p, end);
                if (p >= end)
                    break;
                if (*p == ']') {
                    p++;
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

                waywal_graph_edge_t *e = &out_graph->edges[out_graph->edge_count];
                memset(e, 0, sizeof(*e));

                while (p < end) {
                    json_skip_ws(&p, end);
                    if (p >= end)
                        break;
                    if (*p == '}') {
                        p++;
                        break;
                    }

                    char ekey[64];
                    if (!json_parse_string(&p, end, ekey, sizeof(ekey)))
                        break;
                    if (!json_expect_char(&p, end, ':'))
                        break;

                    if (strcmp(ekey, "edge_id") == 0 || strcmp(ekey, "id") == 0) {
                        json_parse_uint(&p, end, &e->edge_id);
                    } else if (strcmp(ekey, "src_node_id") == 0) {
                        json_parse_uint(&p, end, &e->src_node_id);
                    } else if (strcmp(ekey, "src_pin_index") == 0) {
                        json_parse_uint(&p, end, &e->src_pin_index);
                    } else if (strcmp(ekey, "dst_node_id") == 0) {
                        json_parse_uint(&p, end, &e->dst_node_id);
                    } else if (strcmp(ekey, "dst_pin_index") == 0) {
                        json_parse_uint(&p, end, &e->dst_pin_index);
                    } else if (strcmp(ekey, "swizzle_mask") == 0) {
                        json_parse_uint(&p, end, &e->swizzle_mask);
                    } else {
                        json_skip_value(&p, end);
                    }

                    json_expect_char(&p, end, ',');
                }

                /* Mark connected flag on pins */
                waywal_graph_node_t *dst_node =
                    waywal_shader_graph_find_node(out_graph, e->dst_node_id);
                if (dst_node && e->dst_pin_index < dst_node->input_pin_count) {
                    dst_node->inputs[e->dst_pin_index].flags |= WAYWAL_PIN_FLAG_CONNECTED;
                }
                waywal_graph_node_t *src_node =
                    waywal_shader_graph_find_node(out_graph, e->src_node_id);
                if (src_node && e->src_pin_index < src_node->output_pin_count) {
                    src_node->outputs[e->src_pin_index].flags |= WAYWAL_PIN_FLAG_CONNECTED;
                }

                out_graph->edge_count++;
                json_expect_char(&p, end, ',');
            }
        } else {
            json_skip_value(&p, end);
        }

        json_expect_char(&p, end, ',');
    }

    return true;
}

bool waywal_shader_graph_from_json(const char *json_str, size_t json_len,
                                   waywal_shader_graph_t *out_graph, char err_buf[256])
{
    return waywal_shader_graph_load_json(json_str, json_len, out_graph, err_buf);
}
