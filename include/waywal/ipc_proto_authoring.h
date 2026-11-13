// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_IPC_PROTO_AUTHORING_H
#define WAYWAL_IPC_PROTO_AUTHORING_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if __has_include("waywal/we_gpu_brush.h")
#include "waywal/we_gpu_brush.h"
#endif

#if __has_include("waywal/ipc_proto.h")
#include "waywal/ipc_proto.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef WAYWAL_WE_GPU_BRUSH_H
#define WAYWAL_WE_GPU_BRUSH_H

#define WE_BRUSH_TILE_SIZE         64u
#define WE_BRUSH_SCRATCH_MAX_DIM   1024u
#define WE_BRUSH_KAWASE_LEVELS     4u
#define WE_BRUSH_POOL_DEFAULT_CAPS 4096u
#define WE_BRUSH_MAX_STROKE_DABS   256u
#define WE_BRUSH_FLOW_NEUTRAL_U8   128u

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

typedef enum : uint32_t {
    WE_BRUSH_CHAN_MASK_R8 = 0u,
    WE_BRUSH_CHAN_FLOW_RG88 = 1u,
    WE_BRUSH_CHAN_COLOR_RGBA8 = 2u,
    WE_BRUSH_CHAN_BLUR = 3u,
    WE_BRUSH_CHAN_SMUDGE = 4u,
} we_brush_channel_t;

typedef enum : uint32_t {
    WE_BRUSH_OP_ADD_BUILDUP = 0u,
    WE_BRUSH_OP_MAX_CAP = 1u,
    WE_BRUSH_OP_SUBTRACT = 2u,
    WE_BRUSH_OP_ERASE = 3u,
    WE_BRUSH_OP_FLOW_PAINT = 4u,
    WE_BRUSH_OP_FLOW_ERASE = 5u,
    WE_BRUSH_OP_FLOW_COMB = 6u,
    WE_BRUSH_OP_BLUR_MASKED = 7u,
    WE_BRUSH_OP_SMUDGE_ADV = 8u,
} we_brush_op_mode_t;

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#pragma pack(push, 1)
typedef struct {
    uint32_t channel;
    uint32_t op_mode;
    float radius_px;
    float hardness;
    float falloff_power;
    float opacity;
    float target_value;
    float flow_strength;
    float blur_radius_px;
    float pressure_size_weight;
    float pressure_alpha_weight;
    float velocity_damp_weight;
    float color_rgba[4];
} we_brush_params_t;

typedef struct {
    float ax;
    float ay;
    float bx;
    float by;
    float radius_a;
    float radius_b;
    float opacity_eff;
    float flow_dir_x;
    float flow_dir_y;
    float drag_dx_px;
    float drag_dy_px;
    uint32_t flags;
} we_brush_dab_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} we_brush_dirty_rect_t;

typedef struct {
    uint16_t tx;
    uint16_t ty;
    uint32_t pool_slot;
} we_brush_tile_coord_t;

typedef struct {
    uint32_t last_dab_gpu_us;
    uint32_t active_pool_slots;
    uint32_t max_pool_slots;
    uint32_t active_stroke_tiles;
    uint32_t canvas_width;
    uint32_t canvas_height;
    uint32_t target_gl_tex;
    uint32_t target_tex_format;
} we_brush_stats_t;
#pragma pack(pop)
#endif /* WAYWAL_WE_GPU_BRUSH_H */

static_assert(sizeof(we_brush_params_t) == 64, "we_brush_params_t must be exactly 64 bytes");
static_assert(sizeof(we_brush_dab_t) == 48, "we_brush_dab_t must be exactly 48 bytes");
static_assert(sizeof(we_brush_dirty_rect_t) == 16,
              "we_brush_dirty_rect_t must be exactly 16 bytes");
static_assert(sizeof(we_brush_tile_coord_t) == 8, "we_brush_tile_coord_t must be exactly 8 bytes");
static_assert(sizeof(we_brush_stats_t) == 32, "we_brush_stats_t must be exactly 32 bytes");

/* Authoring Request Opcodes (0x0021 .. 0x002A) */
#ifndef WAYWAL_REQ_SCENE_MUTATE
#define WAYWAL_REQ_SCENE_MUTATE 0x0021u
#endif
#ifndef WAYWAL_REQ_HOT_RELOAD_CODE
#define WAYWAL_REQ_HOT_RELOAD_CODE 0x0022u
#endif
#ifndef WAYWAL_REQ_QUERY_SCENE_TREE
#define WAYWAL_REQ_QUERY_SCENE_TREE 0x0023u
#endif
#ifndef WAYWAL_REQ_SET_SHADER_COMBO
#define WAYWAL_REQ_SET_SHADER_COMBO 0x0024u
#endif
#ifndef WAYWAL_REQ_BRUSH_STROKE
#define WAYWAL_REQ_BRUSH_STROKE 0x0025u
#endif
#ifndef WAYWAL_REQ_PARTICLE_CONFIG
#define WAYWAL_REQ_PARTICLE_CONFIG 0x0026u
#endif
#ifndef WAYWAL_REQ_SET_AUDIO_BINDINGS
#define WAYWAL_REQ_SET_AUDIO_BINDINGS 0x0027u
#endif
#ifndef WAYWAL_REQ_EXPORT_SCENE_PKG
#define WAYWAL_REQ_EXPORT_SCENE_PKG 0x0028u
#endif
#ifndef WAYWAL_REQ_LINT_SCENESCRIPT
#define WAYWAL_REQ_LINT_SCENESCRIPT 0x0029u
#endif
#ifndef WAYWAL_REQ_READBACK_TEXTURE
#define WAYWAL_REQ_READBACK_TEXTURE 0x002Au
#endif

/* Authoring Response Opcodes (0x8010 .. 0x8019) */
#ifndef WAYWAL_RESP_SCENE_MUTATE
#define WAYWAL_RESP_SCENE_MUTATE 0x8010u
#endif
#ifndef WAYWAL_RESP_HOT_RELOAD_CODE
#define WAYWAL_RESP_HOT_RELOAD_CODE 0x8011u
#endif
#ifndef WAYWAL_RESP_QUERY_SCENE_TREE
#define WAYWAL_RESP_QUERY_SCENE_TREE 0x8012u
#endif
#ifndef WAYWAL_RESP_SET_SHADER_COMBO
#define WAYWAL_RESP_SET_SHADER_COMBO 0x8013u
#endif
#ifndef WAYWAL_RESP_BRUSH_STROKE
#define WAYWAL_RESP_BRUSH_STROKE 0x8014u
#endif
#ifndef WAYWAL_RESP_PARTICLE_CONFIG
#define WAYWAL_RESP_PARTICLE_CONFIG 0x8015u
#endif
#ifndef WAYWAL_RESP_SET_AUDIO_BINDINGS
#define WAYWAL_RESP_SET_AUDIO_BINDINGS 0x8016u
#endif
#ifndef WAYWAL_RESP_EXPORT_SCENE_PKG
#define WAYWAL_RESP_EXPORT_SCENE_PKG 0x8017u
#endif
#ifndef WAYWAL_RESP_LINT_SCENESCRIPT
#define WAYWAL_RESP_LINT_SCENESCRIPT 0x8018u
#endif
#ifndef WAYWAL_RESP_READBACK_TEXTURE
#define WAYWAL_RESP_READBACK_TEXTURE 0x8019u
#endif

#define WAYWAL_IPC_MAX_CODE_BYTES           1048576u
#define WAYWAL_IPC_MAX_SCENE_JSON           2097152u
#define WAYWAL_IPC_MAX_READBACK_BYTES       (64u * 1024u * 1024u)
#define WAYWAL_IPC_MAX_MUTATE_PAYLOAD_BYTES 65536u
#define WAYWAL_IPC_MAX_META_JSON_BYTES      65536u
#define WE_PARTICLE_RAMP_SAMPLES            65u
#define WE_PARTICLE_MAX_KNOTS               12u
#define WE_PARTICLE_PRESET_COUNT            8u
#define WE_MAX_AUDIO_BINDINGS               64u

/* Domain Enums & Packed Wire / Engine Contracts */

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

typedef enum : uint32_t {
    WE_MUTATE_ADD_OBJECT = 0u,
    WE_MUTATE_REMOVE_OBJECT = 1u,
    WE_MUTATE_REORDER_Z = 2u,
    WE_MUTATE_REPARENT = 3u,
    WE_MUTATE_SET_TRANSFORM = 4u,
    WE_MUTATE_SET_BLEND_VIS = 5u,
    WE_MUTATE_SET_TEXT_PROPS = 6u,
    WE_MUTATE_SET_SOUND_PROPS = 7u,
} we_scene_mutate_op_t;

typedef enum : uint32_t {
    WE_NODE_IMAGE = 0u,
    WE_NODE_PARTICLE = 1u,
    WE_NODE_SOUND = 2u,
    WE_NODE_TEXT = 3u,
    WE_NODE_LIGHT = 4u,
    WE_NODE_CAMERA3D = 5u,
} we_scene_node_kind_t;

typedef enum : uint32_t {
    WE_BRUSH_IPC_BIND_TARGET = 0u,
    WE_BRUSH_IPC_BEGIN_STROKE = 1u,
    WE_BRUSH_IPC_DAB_BATCH = 2u,
    WE_BRUSH_IPC_END_STROKE = 3u,
    WE_BRUSH_IPC_UNDO_STROKE = 4u,
    WE_BRUSH_IPC_REDO_STROKE = 5u,
} we_brush_ipc_action_t;

typedef enum : uint32_t {
    WE_PARTICLE_PRESET_RAIN = 0u,
    WE_PARTICLE_PRESET_SNOW = 1u,
    WE_PARTICLE_PRESET_EMBERS = 2u,
    WE_PARTICLE_PRESET_FIREFLIES = 3u,
    WE_PARTICLE_PRESET_CHERRY_BLOSSOMS = 4u,
    WE_PARTICLE_PRESET_STARFIELD = 5u,
    WE_PARTICLE_PRESET_RISING_SMOKE = 6u,
    WE_PARTICLE_PRESET_MAGIC_DUST = 7u,
} we_particle_preset_id_t;

typedef enum : uint32_t {
    WE_KNOT_HANDLE_AUTO_SMOOTH = 0u, /* Fritsch-Carlson monotone cubic Hermite */
    WE_KNOT_HANDLE_LINEAR = 1u,
    WE_KNOT_HANDLE_STEP = 2u,
    WE_KNOT_HANDLE_FREE_BEZIER = 3u,
} we_knot_handle_mode_t;

typedef enum : uint32_t {
    WE_PARTICLE_CFG_APPLY_PRESET = 0u,
    WE_PARTICLE_CFG_SET_SYSTEM_JSON = 1u,
    WE_PARTICLE_CFG_SET_RAMP_LUT = 2u,
    WE_PARTICLE_CFG_TRIGGER_BURST = 3u,
    WE_PARTICLE_CFG_PAUSE_TOGGLE = 4u,
} we_particle_cfg_op_t;

typedef enum : uint32_t {
    WE_AUDIO_SRC_BARK_RANGE = 0u,  /* Bands [band_start .. band_end] */
    WE_AUDIO_SRC_ENVELOPE = 1u,    /* Global 64-band RMS envelope */
    WE_AUDIO_SRC_BASS_ENERGY = 2u, /* Sub-bass/bass bin energy */
    WE_AUDIO_SRC_BEAT_PULSE = 3u,  /* Spectral-flux beat onset pulse */
} we_audio_src_kind_t;

typedef enum : uint32_t {
    WE_AUDIO_AGG_PEAK_WEIGHTED = 0u, /* 0.7 * peak + 0.3 * mean */
    WE_AUDIO_AGG_MEAN = 1u,
    WE_AUDIO_AGG_PEAK = 2u,
} we_audio_agg_mode_t;

typedef enum : uint32_t {
    WE_AUDIO_CURVE_LINEAR = 0u,
    WE_AUDIO_CURVE_QUADRATIC = 1u,
    WE_AUDIO_CURVE_SQRT = 2u,
    WE_AUDIO_CURVE_SMOOTHSTEP = 3u,
    WE_AUDIO_CURVE_TRIG_EDGE = 4u, /* Rising-edge one-shot burst trigger */
} we_audio_curve_t;

typedef enum : uint32_t {
    WE_AUDIO_BLEND_ADD = 0u,
    WE_AUDIO_BLEND_MULTIPLY = 1u,
    WE_AUDIO_BLEND_REPLACE = 2u,
} we_audio_blend_t;

typedef enum : uint32_t {
    WE_AUDIO_TGT_LAYER_ORIGIN_X = 0u,
    WE_AUDIO_TGT_LAYER_ORIGIN_Y = 1u,
    WE_AUDIO_TGT_LAYER_SCALE_XY = 2u,
    WE_AUDIO_TGT_LAYER_ROTATION_Z = 3u,
    WE_AUDIO_TGT_LAYER_ALPHA = 4u,
    WE_AUDIO_TGT_LAYER_BRIGHTNESS = 5u,
    WE_AUDIO_TGT_LAYER_COLOR_RGB = 6u,
    WE_AUDIO_TGT_PASS_UNIFORM_COMP = 7u,
    WE_AUDIO_TGT_LIGHT_INTENSITY = 8u,
    WE_AUDIO_TGT_LIGHT_RADIUS = 9u,
    WE_AUDIO_TGT_PARTICLE_RATE = 10u,
    WE_AUDIO_TGT_PARTICLE_SPEED = 11u,
    WE_AUDIO_TGT_PARTICLE_TURB_SPEED = 12u,
    WE_AUDIO_TGT_PARTICLE_VORTEX_SPD = 13u,
    WE_AUDIO_TGT_PARTICLE_BURST_TRIG = 14u,
} we_audio_target_kind_t;

typedef enum : uint32_t {
    WE_AUDIO_BIND_OP_REPLACE_ALL = 0u,
    WE_AUDIO_BIND_OP_UPSERT = 1u,
    WE_AUDIO_BIND_OP_REMOVE = 2u,
    WE_AUDIO_BIND_OP_CLEAR = 3u,
} we_audio_bind_op_t;

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#pragma pack(push, 1)

/* Particle Curve, OKLab Ramp & Preset Contracts */

typedef struct {
    float t;           /* Normalized lifetime [0.0, 1.0] */
    float y;           /* Target value */
    float in_tangent;  /* Incoming slope dy/dt */
    float out_tangent; /* Outgoing slope dy/dt */
    uint32_t mode;     /* we_knot_handle_mode_t */
    uint32_t _pad;
} we_particle_curve_knot_t;
static_assert(sizeof(we_particle_curve_knot_t) == 24,
              "we_particle_curve_knot_t must be exactly 24 bytes");

typedef struct {
    float t;      /* Gradient stop position [0.0, 1.0] */
    float rgb[3]; /* Linear/sRGB stop color (interpolated in OKLab during bake) */
} we_particle_color_stop_t;
static_assert(sizeof(we_particle_color_stop_t) == 16,
              "we_particle_color_stop_t must be exactly 16 bytes");

typedef struct {
    uint32_t operator_index; /* 0..WE_PARTICLE_MAX_OPERATORS-1 */
    uint32_t channel_kind;   /* 0=Alpha, 1=Size, 2=ColorRGB */
    uint32_t knot_count;     /* 2..WE_PARTICLE_MAX_KNOTS */
    uint32_t _pad;
    float scalar_lut[WE_PARTICLE_RAMP_SAMPLES]; /* Baked 65-sample curve (260 B) */
    float color_lut_r[WE_PARTICLE_RAMP_SAMPLES];
    float color_lut_g[WE_PARTICLE_RAMP_SAMPLES];
    float color_lut_b[WE_PARTICLE_RAMP_SAMPLES];
} we_particle_ramp_lut_t;
static_assert(sizeof(we_particle_ramp_lut_t) == 1056,
              "we_particle_ramp_lut_t must be exactly 1056 bytes");

typedef struct {
    uint32_t preset_id;     /* we_particle_preset_id_t */
    uint32_t max_count;     /* Reference max particle cap */
    uint32_t renderer_type; /* 0=SPRITE, 1=SPRITETRAIL, 2=ROPETRAIL */
    uint32_t blending;      /* we_blend_mode_t */
    float start_time_sec;   /* Pre-warm duration */
    float gravity[3];       /* +Y up world gravity (px/s^2) */
    char slug[32];          /* e.g. "rain", "cherry_blossoms" */
    char display_name[48];  /* Human-readable preset title */
} we_particle_preset_desc_t;
static_assert(sizeof(we_particle_preset_desc_t) == 112,
              "we_particle_preset_desc_t must be exactly 112 bytes");

/* No-Code Audio-Binding Contract (160 bytes) */

typedef struct {
    uint32_t binding_id;       /* Stable unique ID for undo/redo */
    int32_t target_object_id;  /* Scene object ID */
    uint32_t target_kind;      /* we_audio_target_kind_t */
    uint32_t target_sub_index; /* Emitter/operator/pass index */
    uint32_t target_component; /* Uniform component 0..3 */
    char uniform_name[64];     /* Uniform name for PASS_UNIFORM_COMP */

    uint32_t src_kind;   /* we_audio_src_kind_t */
    uint32_t agg_mode;   /* we_audio_agg_mode_t */
    uint16_t band_start; /* 0..63 */
    uint16_t band_end;   /* band_start..63 */
    float input_gain;    /* [0.1, 10.0] */

    float threshold_open; /* Gate threshold [0.0, 0.98] */
    float hysteresis;     /* Close hysteresis [0.0, 0.25] */
    float attack_sec;     /* [0.001, 1.0] */
    float release_sec;    /* [0.005, 2.0] */

    uint32_t curve_shape; /* we_audio_curve_t */
    uint32_t blend_mode;  /* we_audio_blend_t */
    float out_min;        /* Minimum output */
    float out_max;        /* Maximum output */
    float base_value;     /* Unmodulated baseline to prevent drift */

    float env_state;        /* Runtime IIR envelope state */
    float last_output;      /* Last evaluated output */
    float cooldown_rem_sec; /* Beat-trigger cooldown remaining */
    uint8_t gate_open;      /* 0 or 1 */
    uint8_t enabled;        /* 0 or 1 */
    uint8_t _pad[2];
    uint32_t reserved[2];
} we_audio_binding_t;
static_assert(sizeof(we_audio_binding_t) == 160, "we_audio_binding_t must be exactly 160 bytes");

/* Authoring IPC Wire Structs (0x0021..0x002A / 0x8010..0x8019) */

/* 0x0021: WAYWAL_REQ_SCENE_MUTATE (296 bytes + optional payload_len UTF-8 JSON tail) */
typedef struct {
    char target_output[64];
    uint32_t mutate_op; /* we_scene_mutate_op_t */
    uint32_t node_kind; /* we_scene_node_kind_t */
    int32_t object_id;
    int32_t parent_id;      /* -1 for root */
    int32_t target_z_index; /* 0..object_count-1 */
    uint32_t blend_mode;    /* we_blend_mode_t */
    uint32_t flags;         /* bit0:visible, bit1:locked, bit2:fullscreen, bit3:passthrough */
    float origin[3];
    float scale[3];
    float angles[3];
    float size[2];
    float parallax_depth[2];
    float color[4]; /* rgb + alpha */
    char name[128];
    uint32_t payload_len; /* <= 65536 */
    uint32_t _pad;
} waywal_ipc_req_scene_mutate_t;
static_assert(sizeof(waywal_ipc_req_scene_mutate_t) == 296,
              "waywal_ipc_req_scene_mutate_t must be 296 bytes");

/* 0x8010: WAYWAL_RESP_SCENE_MUTATE (272 bytes) */
typedef struct {
    uint32_t status_code; /* 0=OK, 1=NOT_FOUND, 2=CYCLE_REJECTED, 3=LIMIT_EXCEEDED */
    int32_t object_id;
    uint32_t scene_revision;
    uint32_t object_count;
    char error_msg[256];
} waywal_ipc_resp_scene_mutate_t;
static_assert(sizeof(waywal_ipc_resp_scene_mutate_t) == 272,
              "waywal_ipc_resp_scene_mutate_t must be 272 bytes");

/* 0x0022: WAYWAL_REQ_HOT_RELOAD_CODE (152 bytes + source_len UTF-8 bytes) */
typedef struct {
    char target_output[64];
    uint32_t code_kind;   /* 0=GLSL_FRAG, 1=GLSL_VERT, 2=SCENESCRIPT_JS */
    uint32_t flags;       /* bit0=VALIDATE_ONLY, bit1=HOT_SWAP, bit2=EXTRACT_META */
    int32_t object_id;    /* -1 for standalone shader wallpaper */
    uint32_t pass_index;  /* 0..15 or 0xFFFFFFFF for base_pass */
    uint32_t script_slot; /* 0..7 */
    uint32_t source_len;  /* <= WAYWAL_IPC_MAX_CODE_BYTES */
    char uniform_name[64];
} waywal_ipc_req_hot_reload_code_t;
static_assert(sizeof(waywal_ipc_req_hot_reload_code_t) == 152,
              "waywal_ipc_req_hot_reload_code_t must be 152 bytes");

/* 0x8011: WAYWAL_RESP_HOT_RELOAD_CODE (544 bytes + meta_json_len bytes) */
typedef struct {
    uint32_t status_code; /* 0=OK, 1=SYNTAX_ERR, 2=LINK_ERR, 3=WATCHDOG_ABORT, 4=NOT_FOUND */
    uint32_t error_line;
    uint32_t error_col;
    uint32_t uniform_count;
    uint32_t combo_count;
    uint32_t compile_time_us;
    uint32_t meta_json_len; /* <= 65536 */
    uint32_t _pad;
    char error_msg[512];
} waywal_ipc_resp_hot_reload_code_t;
static_assert(sizeof(waywal_ipc_resp_hot_reload_code_t) == 544,
              "waywal_ipc_resp_hot_reload_code_t must be 544 bytes");

/* 0x0023: WAYWAL_REQ_QUERY_SCENE_TREE (72 bytes) */
typedef struct {
    char target_output[64];
    uint32_t flags; /* bit0=uniforms, bit1=particles, bit2=scripts, bit3=audio */
    uint32_t _pad;
} waywal_ipc_req_query_scene_tree_t;
static_assert(sizeof(waywal_ipc_req_query_scene_tree_t) == 72,
              "waywal_ipc_req_query_scene_tree_t must be 72 bytes");

/* 0x8012: WAYWAL_RESP_QUERY_SCENE_TREE (16 bytes + json_len UTF-8 bytes) */
typedef struct {
    uint32_t status_code;
    uint32_t scene_revision;
    uint32_t object_count;
    uint32_t json_len; /* <= WAYWAL_IPC_MAX_SCENE_JSON */
} waywal_ipc_resp_query_scene_tree_t;
static_assert(sizeof(waywal_ipc_resp_query_scene_tree_t) == 16,
              "waywal_ipc_resp_query_scene_tree_t must be 16 bytes");

/* 0x0024: WAYWAL_REQ_SET_SHADER_COMBO (144 bytes) */
typedef struct {
    char target_output[64];
    int32_t object_id;
    uint32_t pass_index;
    char combo_name[64];
    int32_t combo_value;
    uint32_t flags;
} waywal_ipc_req_set_shader_combo_t;
static_assert(sizeof(waywal_ipc_req_set_shader_combo_t) == 144,
              "waywal_ipc_req_set_shader_combo_t must be 144 bytes");

/* 0x8013: WAYWAL_RESP_SET_SHADER_COMBO (264 bytes) */
typedef struct {
    uint32_t status_code;
    uint32_t recompile_time_us;
    char error_msg[256];
} waywal_ipc_resp_set_shader_combo_t;
static_assert(sizeof(waywal_ipc_resp_set_shader_combo_t) == 264,
              "waywal_ipc_resp_set_shader_combo_t must be 264 bytes");

/* 0x0025: WAYWAL_REQ_BRUSH_STROKE (160 bytes + dab_count * sizeof(we_brush_dab_t)) */
typedef struct {
    char target_output[64];
    uint32_t action; /* we_brush_ipc_action_t */
    int32_t object_id;
    uint32_t pass_index; /* 0..15 or 0xFFFFFFFF for base_pass */
    uint32_t tex_slot;   /* 0..7 */
    uint64_t stroke_id;
    uint32_t dab_count; /* 0..WE_BRUSH_MAX_STROKE_DABS */
    uint32_t _pad;
    we_brush_params_t params; /* 64 bytes */
} waywal_ipc_req_brush_stroke_t;
static_assert(sizeof(waywal_ipc_req_brush_stroke_t) == 160,
              "waywal_ipc_req_brush_stroke_t must be 160 bytes");

/* 0x8014: WAYWAL_RESP_BRUSH_STROKE (64 bytes) */
typedef struct {
    uint32_t status_code;
    uint64_t stroke_id;
    we_brush_dirty_rect_t dirty_rect; /* 16 bytes */
    we_brush_stats_t stats;           /* 32 bytes */
    uint32_t captured_tile_count;
} waywal_ipc_resp_brush_stroke_t;
static_assert(sizeof(waywal_ipc_resp_brush_stroke_t) == 64,
              "waywal_ipc_resp_brush_stroke_t must be 64 bytes");

/* 0x0026: WAYWAL_REQ_PARTICLE_CONFIG (96 bytes + payload_len bytes) */
typedef struct {
    char target_output[64];
    int32_t object_id;
    uint32_t config_op;   /* we_particle_cfg_op_t */
    uint32_t preset_id;   /* we_particle_preset_id_t (when APPLY_PRESET) */
    uint32_t flags;       /* bit0: paused, bit1: prewarm */
    uint32_t payload_len; /* sizeof(we_particle_ramp_lut_t) or UTF-8 JSON len */
    uint32_t _pad[3];
} waywal_ipc_req_particle_config_t;
static_assert(sizeof(waywal_ipc_req_particle_config_t) == 96,
              "waywal_ipc_req_particle_config_t must be 96 bytes");

/* 0x8015: WAYWAL_RESP_PARTICLE_CONFIG (24 bytes) */
typedef struct {
    uint32_t status_code;
    int32_t object_id;
    uint32_t alive_count;
    uint32_t max_count;
    uint32_t sim_time_us;
    uint32_t _pad;
} waywal_ipc_resp_particle_config_t;
static_assert(sizeof(waywal_ipc_resp_particle_config_t) == 24,
              "waywal_ipc_resp_particle_config_t must be 24 bytes");

/* 0x0027: WAYWAL_REQ_SET_AUDIO_BINDINGS (80 bytes + binding_count * 160 B) */
typedef struct {
    char target_output[64];
    uint32_t bind_op;       /* we_audio_bind_op_t */
    uint32_t binding_count; /* 0..WE_MAX_AUDIO_BINDINGS */
    uint32_t remove_id;     /* Used when bind_op == WE_AUDIO_BIND_OP_REMOVE */
    uint32_t _pad;
} waywal_ipc_req_set_audio_bindings_t;
static_assert(sizeof(waywal_ipc_req_set_audio_bindings_t) == 80,
              "waywal_ipc_req_set_audio_bindings_t must be 80 bytes");

/* 0x8016: WAYWAL_RESP_SET_AUDIO_BINDINGS (16 bytes) */
typedef struct {
    uint32_t status_code;
    uint32_t active_binding_count;
    uint32_t eval_time_ns;
    uint32_t _pad;
} waywal_ipc_resp_set_audio_bindings_t;
static_assert(sizeof(waywal_ipc_resp_set_audio_bindings_t) == 16,
              "waywal_ipc_resp_set_audio_bindings_t must be 16 bytes");

#ifndef WE_EXPORT_PKG_FLAG_PACK_SCENE_PKG
#define WE_EXPORT_PKG_FLAG_PACK_SCENE_PKG 0x1u
#endif
#ifndef WE_EXPORT_PKG_FLAG_DIRECT_PKG_FILE
#define WE_EXPORT_PKG_FLAG_DIRECT_PKG_FILE 0x2u
#endif

/* 0x0028: WAYWAL_REQ_EXPORT_SCENE_PKG (1096 bytes) */
typedef struct {
    char target_output[64];
    char dest_dir_or_pkg[1024];
    uint32_t flags; /* WE_EXPORT_PKG_FLAG_PACK_SCENE_PKG | WE_EXPORT_PKG_FLAG_DIRECT_PKG_FILE */
    uint32_t _pad;
} waywal_ipc_req_export_scene_pkg_t;
static_assert(sizeof(waywal_ipc_req_export_scene_pkg_t) == 1096,
              "waywal_ipc_req_export_scene_pkg_t must be 1096 bytes");

/* 0x8017: WAYWAL_RESP_EXPORT_SCENE_PKG (280 bytes) */
typedef struct {
    uint32_t status_code;
    uint32_t entry_count;
    uint64_t total_bytes;
    uint64_t export_time_us;
    char error_msg[256];
} waywal_ipc_resp_export_scene_pkg_t;
static_assert(sizeof(waywal_ipc_resp_export_scene_pkg_t) == 280,
              "waywal_ipc_resp_export_scene_pkg_t must be 280 bytes");

/* 0x0029: WAYWAL_REQ_LINT_SCENESCRIPT (16 bytes + source_len UTF-8 bytes) */
typedef struct {
    uint32_t source_len; /* <= WAYWAL_IPC_MAX_CODE_BYTES */
    uint32_t flags;      /* bit0: strict_es_subset */
    uint32_t _pad[2];
} waywal_ipc_req_lint_scenescript_t;
static_assert(sizeof(waywal_ipc_req_lint_scenescript_t) == 16,
              "waywal_ipc_req_lint_scenescript_t must be 16 bytes");

/* 0x8018: WAYWAL_RESP_LINT_SCENESCRIPT (272 bytes) */
typedef struct {
    uint32_t status_code; /* 0=OK, 1=SYNTAX_ERR, 2=DEPTH_EXCEEDED */
    uint32_t error_line;
    uint32_t error_col;
    uint32_t lint_time_us;
    char error_msg[256];
} waywal_ipc_resp_lint_scenescript_t;
static_assert(sizeof(waywal_ipc_resp_lint_scenescript_t) == 272,
              "waywal_ipc_resp_lint_scenescript_t must be 272 bytes");

/* 0x002A: WAYWAL_REQ_READBACK_TEXTURE (80 bytes) */
typedef struct {
    char target_output[64];
    int32_t object_id;
    uint32_t pass_index;
    uint32_t tex_slot;
    uint32_t format_mode; /* 0=CANONICAL_UNSWIZZLED, 1=RAW_TEXV0005 */
} waywal_ipc_req_readback_texture_t;
static_assert(sizeof(waywal_ipc_req_readback_texture_t) == 80,
              "waywal_ipc_req_readback_texture_t must be 80 bytes");

/* 0x8019: WAYWAL_RESP_READBACK_TEXTURE (32 bytes + data_len bytes, <=
 * WAYWAL_IPC_MAX_READBACK_BYTES) */
typedef struct {
    uint32_t status_code;
    uint32_t tex_format;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
    uint32_t data_len;
    uint64_t _pad;
} waywal_ipc_resp_readback_texture_t;
static_assert(sizeof(waywal_ipc_resp_readback_texture_t) == 32,
              "waywal_ipc_resp_readback_texture_t must be 32 bytes");

#pragma pack(pop)

/* Wire Validation & Serialization API */

bool waywal_ipc_validate_req_scene_mutate(const waywal_ipc_req_scene_mutate_t *req,
                                          size_t payload_size, const char **out_payload_json);

bool waywal_ipc_serialize_req_scene_mutate(const waywal_ipc_req_scene_mutate_t *params,
                                           const char *payload_json, waywal_ipc_hdr_t *out_hdr,
                                           waywal_ipc_req_scene_mutate_t *out_req, void *out_buf,
                                           size_t out_buf_cap, size_t *out_total_len);

bool waywal_ipc_validate_resp_scene_mutate(const waywal_ipc_resp_scene_mutate_t *resp,
                                           size_t payload_size);

bool waywal_ipc_serialize_resp_scene_mutate(uint32_t status_code, int32_t object_id,
                                            uint32_t scene_revision, uint32_t object_count,
                                            const char *error_msg, waywal_ipc_hdr_t *out_hdr,
                                            waywal_ipc_resp_scene_mutate_t *out_resp);

bool waywal_ipc_validate_req_hot_reload_code(const waywal_ipc_req_hot_reload_code_t *req,
                                             size_t payload_size, const char **out_source);

bool waywal_ipc_serialize_req_hot_reload_code(
    const char *target_output, uint32_t code_kind, uint32_t flags, int32_t object_id,
    uint32_t pass_index, uint32_t script_slot, const char *uniform_name, const char *source_utf8,
    size_t source_len, waywal_ipc_hdr_t *out_hdr, waywal_ipc_req_hot_reload_code_t *out_req,
    void *out_buf, size_t out_buf_cap, size_t *out_total_len);

bool waywal_ipc_validate_resp_hot_reload_code(const waywal_ipc_resp_hot_reload_code_t *resp,
                                              size_t payload_size, const char **out_meta_json);

bool waywal_ipc_serialize_resp_hot_reload_code(
    uint32_t status_code, uint32_t error_line, uint32_t error_col, uint32_t uniform_count,
    uint32_t combo_count, uint32_t compile_time_us, const char *error_msg, const char *meta_json,
    size_t meta_json_len, waywal_ipc_hdr_t *out_hdr, waywal_ipc_resp_hot_reload_code_t *out_resp,
    void *out_buf, size_t out_buf_cap, size_t *out_total_len);

bool waywal_ipc_validate_req_query_scene_tree(const waywal_ipc_req_query_scene_tree_t *req,
                                              size_t payload_size);

bool waywal_ipc_serialize_req_query_scene_tree(const char *target_output, uint32_t flags,
                                               waywal_ipc_hdr_t *out_hdr,
                                               waywal_ipc_req_query_scene_tree_t *out_req);

bool waywal_ipc_validate_resp_query_scene_tree(const waywal_ipc_resp_query_scene_tree_t *resp,
                                               size_t payload_size, const char **out_json);

bool waywal_ipc_serialize_resp_query_scene_tree(uint32_t status_code, uint32_t scene_revision,
                                                uint32_t object_count, const char *json,
                                                size_t json_len, waywal_ipc_hdr_t *out_hdr,
                                                waywal_ipc_resp_query_scene_tree_t *out_resp,
                                                void *out_buf, size_t out_buf_cap,
                                                size_t *out_total_len);

bool waywal_ipc_validate_req_set_shader_combo(const waywal_ipc_req_set_shader_combo_t *req,
                                              size_t payload_size);

bool waywal_ipc_serialize_req_set_shader_combo(const char *target_output, int32_t object_id,
                                               uint32_t pass_index, const char *combo_name,
                                               int32_t combo_value, uint32_t flags,
                                               waywal_ipc_hdr_t *out_hdr,
                                               waywal_ipc_req_set_shader_combo_t *out_req);

bool waywal_ipc_validate_resp_set_shader_combo(const waywal_ipc_resp_set_shader_combo_t *resp,
                                               size_t payload_size);

bool waywal_ipc_serialize_resp_set_shader_combo(uint32_t status_code, uint32_t recompile_time_us,
                                                const char *error_msg, waywal_ipc_hdr_t *out_hdr,
                                                waywal_ipc_resp_set_shader_combo_t *out_resp);

bool waywal_ipc_validate_req_brush_stroke(const waywal_ipc_req_brush_stroke_t *req,
                                          size_t payload_size, const we_brush_dab_t **out_dabs,
                                          uint32_t *out_dab_count);

bool waywal_ipc_serialize_req_brush_stroke(const char *target_output, uint32_t action,
                                           int32_t object_id, uint32_t pass_index,
                                           uint32_t tex_slot, uint64_t stroke_id,
                                           const we_brush_params_t *params,
                                           const we_brush_dab_t *dabs, uint32_t dab_count,
                                           waywal_ipc_hdr_t *out_hdr,
                                           waywal_ipc_req_brush_stroke_t *out_req, void *out_buf,
                                           size_t out_buf_cap, size_t *out_total_len);

bool waywal_ipc_validate_resp_brush_stroke(const waywal_ipc_resp_brush_stroke_t *resp,
                                           size_t payload_size);

bool waywal_ipc_serialize_resp_brush_stroke(uint32_t status_code, uint64_t stroke_id,
                                            const we_brush_dirty_rect_t *dirty_rect,
                                            const we_brush_stats_t *stats,
                                            uint32_t captured_tile_count, waywal_ipc_hdr_t *out_hdr,
                                            waywal_ipc_resp_brush_stroke_t *out_resp);

bool waywal_ipc_validate_req_particle_config(const waywal_ipc_req_particle_config_t *req,
                                             size_t payload_size, const void **out_payload,
                                             uint32_t *out_payload_len);

bool waywal_ipc_serialize_req_particle_config(const char *target_output, int32_t object_id,
                                              uint32_t config_op, uint32_t preset_id,
                                              uint32_t flags, const void *payload,
                                              uint32_t payload_len, waywal_ipc_hdr_t *out_hdr,
                                              waywal_ipc_req_particle_config_t *out_req,
                                              void *out_buf, size_t out_buf_cap,
                                              size_t *out_total_len);

bool waywal_ipc_validate_resp_particle_config(const waywal_ipc_resp_particle_config_t *resp,
                                              size_t payload_size);

bool waywal_ipc_serialize_resp_particle_config(uint32_t status_code, int32_t object_id,
                                               uint32_t alive_count, uint32_t max_count,
                                               uint32_t sim_time_us, waywal_ipc_hdr_t *out_hdr,
                                               waywal_ipc_resp_particle_config_t *out_resp);

bool waywal_ipc_validate_req_set_audio_bindings(const waywal_ipc_req_set_audio_bindings_t *req,
                                                size_t payload_size,
                                                const we_audio_binding_t **out_bindings,
                                                uint32_t *out_count);

bool waywal_ipc_serialize_req_set_audio_bindings(const char *target_output, uint32_t bind_op,
                                                 uint32_t remove_id,
                                                 const we_audio_binding_t *bindings,
                                                 uint32_t binding_count, waywal_ipc_hdr_t *out_hdr,
                                                 waywal_ipc_req_set_audio_bindings_t *out_req,
                                                 void *out_buf, size_t out_buf_cap,
                                                 size_t *out_total_len);

bool waywal_ipc_validate_resp_set_audio_bindings(const waywal_ipc_resp_set_audio_bindings_t *resp,
                                                 size_t payload_size);

bool waywal_ipc_serialize_resp_set_audio_bindings(uint32_t status_code,
                                                  uint32_t active_binding_count,
                                                  uint32_t eval_time_ns, waywal_ipc_hdr_t *out_hdr,
                                                  waywal_ipc_resp_set_audio_bindings_t *out_resp);

bool waywal_ipc_export_path_is_allowed(const char *path, char *out_resolved, size_t out_cap);

bool waywal_ipc_validate_req_export_scene_pkg(const waywal_ipc_req_export_scene_pkg_t *req,
                                              size_t payload_size);

bool waywal_ipc_serialize_req_export_scene_pkg(const char *target_output,
                                               const char *dest_dir_or_pkg, uint32_t flags,
                                               waywal_ipc_hdr_t *out_hdr,
                                               waywal_ipc_req_export_scene_pkg_t *out_req);

bool waywal_ipc_validate_resp_export_scene_pkg(const waywal_ipc_resp_export_scene_pkg_t *resp,
                                               size_t payload_size);

bool waywal_ipc_serialize_resp_export_scene_pkg(uint32_t status_code, uint32_t entry_count,
                                                uint64_t total_bytes, uint64_t export_time_us,
                                                const char *error_msg, waywal_ipc_hdr_t *out_hdr,
                                                waywal_ipc_resp_export_scene_pkg_t *out_resp);

bool waywal_ipc_validate_req_lint_scenescript(const waywal_ipc_req_lint_scenescript_t *req,
                                              size_t payload_size, const char **out_source);

bool waywal_ipc_serialize_req_lint_scenescript(const char *source_utf8, size_t source_len,
                                               uint32_t flags, waywal_ipc_hdr_t *out_hdr,
                                               waywal_ipc_req_lint_scenescript_t *out_req,
                                               void *out_buf, size_t out_buf_cap,
                                               size_t *out_total_len);

bool waywal_ipc_validate_resp_lint_scenescript(const waywal_ipc_resp_lint_scenescript_t *resp,
                                               size_t payload_size);

bool waywal_ipc_serialize_resp_lint_scenescript(uint32_t status_code, uint32_t error_line,
                                                uint32_t error_col, uint32_t lint_time_us,
                                                const char *error_msg, waywal_ipc_hdr_t *out_hdr,
                                                waywal_ipc_resp_lint_scenescript_t *out_resp);

bool waywal_ipc_validate_req_readback_texture(const waywal_ipc_req_readback_texture_t *req,
                                              size_t payload_size);

bool waywal_ipc_serialize_req_readback_texture(const char *target_output, int32_t object_id,
                                               uint32_t pass_index, uint32_t tex_slot,
                                               uint32_t format_mode, waywal_ipc_hdr_t *out_hdr,
                                               waywal_ipc_req_readback_texture_t *out_req);

bool waywal_ipc_validate_resp_readback_texture(const waywal_ipc_resp_readback_texture_t *resp,
                                               size_t payload_size, const void **out_data,
                                               uint32_t *out_data_len);

bool waywal_ipc_serialize_resp_readback_texture(uint32_t status_code, uint32_t tex_format,
                                                uint32_t width, uint32_t height, uint32_t flags,
                                                const void *data, uint32_t data_len,
                                                waywal_ipc_hdr_t *out_hdr,
                                                waywal_ipc_resp_readback_texture_t *out_resp,
                                                void *out_buf, size_t out_buf_cap,
                                                size_t *out_total_len);

struct daemon_state;

/* Top-level daemon dispatch handler for authoring opcodes (0x0021..0x002A). */
bool daemon_dispatch_authoring_ipc(struct daemon_state *state, int client_fd,
                                   const waywal_ipc_hdr_t *hdr, int attached_fd);

/* Non-blocking per-opcode dispatch handlers. */
bool daemon_dispatch_ipc_req_scene_mutate(struct daemon_state *state,
                                          const waywal_ipc_req_scene_mutate_t *req,
                                          const char *payload_json,
                                          waywal_ipc_resp_scene_mutate_t *resp);

bool daemon_dispatch_ipc_req_hot_reload_code(struct daemon_state *state,
                                             const waywal_ipc_req_hot_reload_code_t *req,
                                             const char *source_utf8,
                                             waywal_ipc_resp_hot_reload_code_t *resp,
                                             char **out_meta_json, size_t *out_meta_len);

bool daemon_dispatch_ipc_req_query_scene_tree(struct daemon_state *state,
                                              const waywal_ipc_req_query_scene_tree_t *req,
                                              waywal_ipc_resp_query_scene_tree_t *resp,
                                              char **out_json, size_t *out_json_len);

bool daemon_dispatch_ipc_req_set_shader_combo(struct daemon_state *state,
                                              const waywal_ipc_req_set_shader_combo_t *req,
                                              waywal_ipc_resp_set_shader_combo_t *resp);

bool daemon_dispatch_ipc_req_brush_stroke(struct daemon_state *state,
                                          const waywal_ipc_req_brush_stroke_t *req,
                                          const we_brush_dab_t *dabs,
                                          waywal_ipc_resp_brush_stroke_t *resp);

bool daemon_dispatch_ipc_req_particle_config(struct daemon_state *state,
                                             const waywal_ipc_req_particle_config_t *req,
                                             const void *payload,
                                             waywal_ipc_resp_particle_config_t *resp);

bool daemon_dispatch_ipc_req_set_audio_bindings(struct daemon_state *state,
                                                const waywal_ipc_req_set_audio_bindings_t *req,
                                                const we_audio_binding_t *bindings,
                                                waywal_ipc_resp_set_audio_bindings_t *resp);

bool daemon_dispatch_ipc_req_export_scene_pkg(struct daemon_state *state,
                                              const waywal_ipc_req_export_scene_pkg_t *req,
                                              waywal_ipc_resp_export_scene_pkg_t *resp);

bool daemon_dispatch_ipc_req_lint_scenescript(struct daemon_state *state,
                                              const waywal_ipc_req_lint_scenescript_t *req,
                                              const char *source_utf8,
                                              waywal_ipc_resp_lint_scenescript_t *resp);

bool daemon_dispatch_ipc_req_readback_texture(struct daemon_state *state,
                                              const waywal_ipc_req_readback_texture_t *req,
                                              waywal_ipc_resp_readback_texture_t *resp,
                                              uint8_t **out_data, uint32_t *out_data_len);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_IPC_PROTO_AUTHORING_H */
