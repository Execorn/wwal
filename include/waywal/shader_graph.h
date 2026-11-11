// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_SHADER_GRAPH_H
#define WAYWAL_SHADER_GRAPH_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAYWAL_GRAPH_SCHEMA_VERSION    1u
#define WAYWAL_GRAPH_MAX_NODES         256u
#define WAYWAL_GRAPH_MAX_PINS_PER_NODE 8u
#define WAYWAL_GRAPH_MAX_EDGES         512u
#define WAYWAL_GRAPH_MAX_PASSES        5u /* 0..3 = BUFFER_A..D, 4 = IMAGE_PASS */

/* Graph Flags */
#define WAYWAL_GRAPH_FLAG_AUTO_COMPILE  (1u << 0)
#define WAYWAL_GRAPH_FLAG_TEN_BIT_HDR   (1u << 1)
#define WAYWAL_GRAPH_FLAG_FORCE_COMPUTE (1u << 2)

/* Node Flags */
#define WAYWAL_NODE_FLAG_COLLAPSED        (1u << 0)
#define WAYWAL_NODE_FLAG_PREVIEW_EXPANDED (1u << 1)
#define WAYWAL_NODE_FLAG_MUTED_BYPASS     (1u << 2)

/* Edge Flags */
#define WAYWAL_EDGE_FLAG_SELECTED       (1u << 0)
#define WAYWAL_EDGE_FLAG_FEEDBACK_LATCH (1u << 1)

/* Pin Flags */
#define WAYWAL_PIN_FLAG_CONNECTED     (1u << 0)
#define WAYWAL_PIN_FLAG_HIDDEN        (1u << 1)
#define WAYWAL_PIN_FLAG_NON_COERCIBLE (1u << 2)

typedef enum : uint32_t {
    WAYWAL_PIN_FLOAT = 0,          /* Scalar float (Coral #FF6B6B, Circle) */
    WAYWAL_PIN_VEC2 = 1,           /* 2D vector / UV (Mint #4ECDC4, Capsule) */
    WAYWAL_PIN_VEC3 = 2,           /* 3D vector / Normal / Position (Amber #FFD166, Triangle) */
    WAYWAL_PIN_VEC4 = 3,           /* 4D vector / RGBA Color (Lavender #C084FC, Diamond) */
    WAYWAL_PIN_INT = 4,            /* Signed 32-bit int (Sky Blue #38BDF8, RoundedSquare) */
    WAYWAL_PIN_BOOL = 5,           /* Boolean toggle (Lime #A3E635, Hexagon) */
    WAYWAL_PIN_SAMPLER2D = 6,      /* Texture / FBO Pass sampler (Orange #FB923C, RoundedSquare) */
    WAYWAL_PIN_AUDIO_SPECTRUM = 7, /* PipeWire Bark FFT array (Magenta #F472B6, WaveformRing) */
} waywal_pin_type_t;

typedef enum : uint32_t {
    /* Input Sources (0..9) */
    WAYWAL_NODE_IN_UV = 0,
    WAYWAL_NODE_IN_TIME = 1,
    WAYWAL_NODE_IN_RESOLUTION = 2,
    WAYWAL_NODE_IN_POINTER = 3,
    WAYWAL_NODE_IN_AUDIO_FFT = 4,
    WAYWAL_NODE_IN_TEXTURE2D = 5,
    WAYWAL_NODE_IN_PASS_BUFFER = 6, /* Reads BUFFER_A..D from previous frame (Feedback Latch) */
    WAYWAL_NODE_IN_DEPTH_MAP = 7,
    WAYWAL_NODE_IN_USER_PROP = 8,
    WAYWAL_NODE_IN_CONSTANT = 9,

    /* Math & Vector Operators (10..29) */
    WAYWAL_NODE_MATH_ADD = 10,
    WAYWAL_NODE_MATH_SUB = 11,
    WAYWAL_NODE_MATH_MUL = 12,
    WAYWAL_NODE_MATH_DIV = 13,
    WAYWAL_NODE_MATH_FMA = 14,
    WAYWAL_NODE_MATH_MIX = 15,
    WAYWAL_NODE_MATH_SMOOTHSTEP = 16,
    WAYWAL_NODE_MATH_CLAMP = 17,
    WAYWAL_NODE_MATH_SIN = 18,
    WAYWAL_NODE_MATH_COS = 19,
    WAYWAL_NODE_MATH_POW = 20,
    WAYWAL_NODE_MATH_DOT = 21,
    WAYWAL_NODE_MATH_CROSS = 22,
    WAYWAL_NODE_MATH_NORMALIZE = 23,
    WAYWAL_NODE_MATH_LENGTH = 24,
    WAYWAL_NODE_MATH_DISTANCE = 25,
    WAYWAL_NODE_MATH_FRACT = 26,
    WAYWAL_NODE_MATH_FLOOR = 27,
    WAYWAL_NODE_MATH_ABS = 28,
    WAYWAL_NODE_VEC_SWIZZLE_COMBINE = 29,

    /* Procedural Noise, UV & SDF Primitives (30..39) */
    WAYWAL_NODE_NOISE_SIMPLEX2D = 30,
    WAYWAL_NODE_NOISE_CURL2D = 31,
    WAYWAL_NODE_NOISE_VORONOI2D = 32,
    WAYWAL_NODE_NOISE_FBM2D = 33,
    WAYWAL_NODE_SDF_CIRCLE = 34,
    WAYWAL_NODE_SDF_BOX = 35,
    WAYWAL_NODE_SDF_RING = 36,
    WAYWAL_NODE_UV_POLAR = 37,
    WAYWAL_NODE_UV_TWIRL = 38,
    WAYWAL_NODE_UV_TILE = 39,

    /* Color Science, Lighting & Built-In Effect Invocations (40..48) */
    WAYWAL_NODE_COLOR_OKLCH_TO_SRGB = 40,
    WAYWAL_NODE_COLOR_SRGB_TO_OKLCH = 41,
    WAYWAL_NODE_COLOR_HUE_SHIFT = 42,
    WAYWAL_NODE_COLOR_CONTRAST_SAT = 43,
    WAYWAL_NODE_FILTER_DUAL_KAWASE = 44,
    WAYWAL_NODE_FILTER_CHROMATIC_ABERR = 45,
    WAYWAL_NODE_LIGHT_NORMAL_FROM_HEIGHT = 46,
    WAYWAL_NODE_LIGHT_BLINN_PHONG = 47,
    WAYWAL_NODE_BUILTIN_EFFECT = 48, /* Wraps any of the 50 we_builtin_effect_id_t effects */

    /* Output Pass Sinks (49..50) */
    WAYWAL_NODE_OUT_BUFFER_PASS = 49, /* Writes GL_RGBA16F BUFFER_A..D */
    WAYWAL_NODE_OUT_IMAGE_PASS = 50,  /* Final screen sink; forces vec4(rgb, 1.0) */
} waywal_node_op_t;

/* Node opcode aliases for clarity & cross-spec consistency */
#define WAYWAL_NODE_BUFFER_PASS  WAYWAL_NODE_IN_PASS_BUFFER
#define WAYWAL_NODE_BLUR_KAWASE  WAYWAL_NODE_FILTER_DUAL_KAWASE
#define WAYWAL_NODE_WATER_RIPPLE WAYWAL_NODE_BUILTIN_EFFECT

typedef struct {
    char name[32];        /* Socket identifier, e.g. "uv", "a", "result" */
    char label[32];       /* Human-readable label for UI display */
    uint32_t pin_type;    /* waywal_pin_type_t */
    uint32_t is_output;   /* 0 = input pin, 1 = output pin */
    float default_val[4]; /* Default constant literal when input pin has no incoming wire (16 bytes)
                           */
    uint32_t flags;       /* Pin flags (e.g. WAYWAL_PIN_FLAG_CONNECTED) */
    uint32_t reserved;    /* Explicit padding to 96 bytes */
} waywal_graph_pin_t;
static_assert(sizeof(waywal_graph_pin_t) == 96, "waywal_graph_pin_t size mismatch");

typedef struct {
    uint32_t node_id;            /* Unique non-zero node identifier */
    uint32_t opcode;             /* waywal_node_op_t */
    char title[64];              /* Display header in NodeGraphEditorWidget */
    char prop_or_asset_key[128]; /* Uniform name, texture path, or builtin effect slug */
    char custom_label[64];       /* User comment, note, or expression snippet */
    float canvas_pos[2];         /* X, Y position in studio scene coordinates */
    float canvas_size[2];        /* Width, Height on canvas */
    uint32_t target_pass;        /* 0..3 = BUFFER_A..D, 4 = IMAGE_PASS */
    uint32_t flags;              /* 0x1=Collapsed, 0x2=PreviewExpanded, 0x4=MutedBypass */
    uint32_t input_pin_count;    /* 0..WAYWAL_GRAPH_MAX_PINS_PER_NODE */
    uint32_t output_pin_count;   /* 0..WAYWAL_GRAPH_MAX_PINS_PER_NODE */
    float const_val[4];          /* Node constant vector / color payload */
    uint32_t sethi_ullman_num;   /* Register pressure weight for topological scheduling */
    uint32_t reserved;           /* Padding to 320 bytes */
    waywal_graph_pin_t inputs[WAYWAL_GRAPH_MAX_PINS_PER_NODE];  /* 768 bytes */
    waywal_graph_pin_t outputs[WAYWAL_GRAPH_MAX_PINS_PER_NODE]; /* 768 bytes */
} waywal_graph_node_t;
static_assert(sizeof(waywal_graph_node_t) == 1856, "waywal_graph_node_t size mismatch");

typedef struct {
    uint32_t edge_id;       /* Unique wire ID */
    uint32_t src_node_id;   /* Producer node_id */
    uint32_t src_pin_index; /* Producer output pin index (0..7) */
    uint32_t dst_node_id;   /* Consumer node_id */
    uint32_t dst_pin_index; /* Consumer input pin index (0..7) */
    uint32_t swizzle_mask;  /* 0 = Identity/Auto-Widen, or packed 4x2-bit channel swizzle */
    uint32_t flags;         /* Edge flags (WAYWAL_EDGE_FLAG_*) */
    uint32_t reserved;      /* Explicit 32-byte alignment padding */
} waywal_graph_edge_t;
static_assert(sizeof(waywal_graph_edge_t) == 32, "waywal_graph_edge_t size mismatch");

/* Total size: 144 + 256 * 1856 + 512 * 32 = 491,664 bytes (~480.1 KB).
 * Heap allocate via waywal_shader_graph_create() / waywal_shader_graph_alloc(). */
typedef struct {
    uint32_t schema_version; /* WAYWAL_GRAPH_SCHEMA_VERSION */
    uint32_t node_count;     /* 0..WAYWAL_GRAPH_MAX_NODES */
    uint32_t edge_count;     /* 0..WAYWAL_GRAPH_MAX_EDGES */
    uint32_t flags;          /* 0x1=AutoCompile, 0x2=TenBitHdr, 0x4=ForceCompute */
    char graph_name[64];
    char author[64];
    waywal_graph_node_t nodes[WAYWAL_GRAPH_MAX_NODES];
    waywal_graph_edge_t edges[WAYWAL_GRAPH_MAX_EDGES];
} waywal_shader_graph_t;
static_assert(sizeof(waywal_shader_graph_t) == 491664, "waywal_shader_graph_t size mismatch");

typedef struct {
    uint32_t sorted_node_indices[WAYWAL_GRAPH_MAX_NODES];
    uint32_t sorted_count;
    uint32_t active_pass_mask;      /* Bit 0..3 = BUFFER_A..D, Bit 4 = IMAGE_PASS */
    uint32_t has_cycle_error;       /* 1 if Tarjan SCC found an unlatched cycle */
    uint32_t cycle_node_id;         /* Offending node_id in cycle */
    uint32_t estimated_alu_ops;     /* Static ALU instruction count across reachable nodes */
    uint32_t estimated_tex_fetches; /* Static texture sample count */
    char error_msg[256];
} waywal_graph_schedule_t;
static_assert(sizeof(waywal_graph_schedule_t) == 1304,
              "waywal_graph_schedule_t must be exactly 1304 bytes");

/* Heap lifecycle APIs */
waywal_shader_graph_t *waywal_shader_graph_create(void);
void waywal_shader_graph_destroy(waywal_shader_graph_t *graph);
waywal_shader_graph_t *waywal_shader_graph_alloc(void);
void waywal_shader_graph_free(waywal_shader_graph_t *graph);
void waywal_shader_graph_init(waywal_shader_graph_t *graph, const char *name);

/* Graph mutation APIs */
int32_t waywal_shader_graph_add_node(waywal_shader_graph_t *graph, uint32_t opcode,
                                     const char *title, float x, float y);
bool waywal_shader_graph_remove_node(waywal_shader_graph_t *graph, uint32_t node_id);
int32_t waywal_shader_graph_connect(waywal_shader_graph_t *graph, uint32_t src_node_id,
                                    uint32_t src_pin_idx, uint32_t dst_node_id,
                                    uint32_t dst_pin_idx);
bool waywal_shader_graph_disconnect(waywal_shader_graph_t *graph, uint32_t edge_id);
bool waywal_shader_graph_disconnect_pins(waywal_shader_graph_t *graph, uint32_t dst_node_id,
                                         uint32_t dst_pin_idx);
waywal_graph_node_t *waywal_shader_graph_find_node(waywal_shader_graph_t *graph, uint32_t node_id);
const waywal_graph_node_t *waywal_shader_graph_find_node_const(const waywal_shader_graph_t *graph,
                                                               uint32_t node_id);

/* Pin validation & coercion lattice */
bool waywal_pin_types_compatible(uint32_t src_type, uint32_t dst_type);

/* Tarjan's O(|V| + |E|) SCC cycle detector */
bool waywal_shader_graph_has_cycle(const waywal_shader_graph_t *graph, uint32_t *out_cycle_node_id);

/* Spatial convolution barrier detection & multi-pass Kahn/Sethi-Ullman scheduling */
bool waywal_shader_graph_analyze(const waywal_shader_graph_t *graph,
                                 waywal_graph_schedule_t *out_schedule);

/* Single Static Assignment (SSA) GLSL ES 3.10 code generator */
char *waywal_shader_graph_compile_glsl(const waywal_shader_graph_t *graph,
                                       const waywal_graph_schedule_t *schedule,
                                       size_t *out_glsl_len);

/* .wwgraph JSON serialization & deserialization (Draft 2020-12 schema) */
char *waywal_shader_graph_save_json(const waywal_shader_graph_t *graph);
bool waywal_shader_graph_load_json(const char *json_str, size_t json_len,
                                   waywal_shader_graph_t *out_graph, char err_buf[256]);
char *waywal_shader_graph_to_json(const waywal_shader_graph_t *graph);
bool waywal_shader_graph_from_json(const char *json_str, size_t json_len,
                                   waywal_shader_graph_t *out_graph, char err_buf[256]);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_SHADER_GRAPH_H */
