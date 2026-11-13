// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WE_PARTICLE_H
#define WAYWAL_WE_PARTICLE_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if __has_include("waywal/ipc_proto_authoring.h")
#include "waywal/ipc_proto_authoring.h"
#endif

#ifndef WAYWAL_IPC_PROTO_AUTHORING_PARTICLE_FALLBACK
#define WAYWAL_IPC_PROTO_AUTHORING_PARTICLE_FALLBACK
#ifndef WAYWAL_IPC_PROTO_AUTHORING_H
#define WE_PARTICLE_RAMP_SAMPLES 65u
#define WE_PARTICLE_MAX_KNOTS    12u
#define WE_PARTICLE_PRESET_COUNT 8u

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
    WE_KNOT_HANDLE_AUTO_SMOOTH = 0u,
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

#pragma pack(push, 1)

typedef struct {
    float t;
    float y;
    float in_tangent;
    float out_tangent;
    uint32_t mode;
    uint32_t _pad;
} we_particle_curve_knot_t;
static_assert(sizeof(we_particle_curve_knot_t) == 24,
              "we_particle_curve_knot_t must be exactly 24 bytes");

typedef struct {
    float t;
    float rgb[3];
} we_particle_color_stop_t;
static_assert(sizeof(we_particle_color_stop_t) == 16,
              "we_particle_color_stop_t must be exactly 16 bytes");

typedef struct {
    uint32_t operator_index;
    uint32_t channel_kind;
    uint32_t knot_count;
    uint32_t _pad;
    float scalar_lut[WE_PARTICLE_RAMP_SAMPLES];
    float color_lut_r[WE_PARTICLE_RAMP_SAMPLES];
    float color_lut_g[WE_PARTICLE_RAMP_SAMPLES];
    float color_lut_b[WE_PARTICLE_RAMP_SAMPLES];
} we_particle_ramp_lut_t;
static_assert(sizeof(we_particle_ramp_lut_t) == 1056,
              "we_particle_ramp_lut_t must be exactly 1056 bytes");

typedef struct {
    uint32_t preset_id;
    uint32_t max_count;
    uint32_t renderer_type;
    uint32_t blending;
    float start_time_sec;
    float gravity[3];
    char slug[32];
    char display_name[48];
} we_particle_preset_desc_t;
static_assert(sizeof(we_particle_preset_desc_t) == 112,
              "we_particle_preset_desc_t must be exactly 112 bytes");

#pragma pack(pop)
#endif /* WAYWAL_IPC_PROTO_AUTHORING_H */
#endif /* WAYWAL_IPC_PROTO_AUTHORING_PARTICLE_FALLBACK */

#ifdef __cplusplus
extern "C" {
#endif

#define WE_PARTICLE_MAX_CAP          16384u
#define WE_PARTICLE_DEFAULT_CAP      256u
#define WE_PARTICLE_SOA_ARRAYS       31u
#define WE_PARTICLE_MAX_EMITTERS     8u
#define WE_PARTICLE_MAX_INITIALIZERS 16u
#define WE_PARTICLE_MAX_OPERATORS    16u
#define WE_PARTICLE_MAX_CONTROL_PTS  8u

typedef enum {
    WE_EMITTER_BOXRANDOM = 0,
    WE_EMITTER_SPHERERANDOM = 1,
    WE_EMITTER_POINT = 2,
} we_emitter_type_t;

typedef struct {
    we_emitter_type_t type;
    float distance_min[3];
    float distance_max[3];
    float directions[3];
    float origin[3];
    float speed_min;
    float speed_max;
    float rate;
    uint32_t instantaneous;
    int32_t control_point;
    float emit_accum;
    bool burst_done;
} we_particle_emitter_t;

typedef enum {
    WE_INIT_LIFETIME_RANDOM = 0,
    WE_INIT_SIZE_RANDOM = 1,
    WE_INIT_ALPHA_RANDOM = 2,
    WE_INIT_COLOR_RANDOM = 3,
    WE_INIT_VELOCITY_RANDOM = 4,
    WE_INIT_ROTATION_RANDOM = 5,
    WE_INIT_ANGULAR_VELOCITY_RANDOM = 6,
    WE_INIT_TURBULENT_VELOCITY_RANDOM = 7,
} we_initializer_type_t;

typedef struct {
    we_initializer_type_t type;
    float min_val[3];
    float max_val[3];
    float exponent;
    float scale;
    float offset;
    float forward[3];
} we_particle_initializer_t;

typedef enum {
    WE_OP_MOVEMENT = 0,
    WE_OP_ANGULAR_MOVEMENT = 1,
    WE_OP_ALPHA_FADE = 2,
    WE_OP_SIZE_CHANGE = 3,
    WE_OP_COLOR_CHANGE = 4,
    WE_OP_OSCILLATE_ALPHA = 5,
    WE_OP_OSCILLATE_SIZE = 6,
    WE_OP_OSCILLATE_POSITION = 7,
    WE_OP_VORTEX = 8,
    WE_OP_TURBULENCE = 9,
    WE_OP_CONTROL_POINT_ATTRACT = 10,
} we_operator_type_t;

typedef struct {
    we_operator_type_t type;
    float gravity[3];
    float drag;
    float fade_in_time;
    float fade_out_time;
    float start_time;
    float end_time;
    float start_val[3];
    float end_val[3];
    float freq_min;
    float freq_max;
    float scale_min;
    float scale_max;
    float phase_min;
    float phase_max;
    float mask[3];
    int32_t control_point;
    float axis[3];
    float offset[3];
    float distance_inner;
    float distance_outer;
    float speed_inner;
    float speed_outer;
    float center_force;
    float turb_scale;
    float turb_speed;
    float turb_timescale;
    alignas(16) we_particle_ramp_lut_t ramp_lut;
    bool use_ramp_lut;
} we_particle_operator_t;

typedef enum {
    WE_RENDERER_SPRITE = 0,
    WE_RENDERER_SPRITETRAIL = 1,
    WE_RENDERER_ROPETRAIL = 2,
} we_renderer_type_t;

typedef struct {
    int32_t id;
    float offset[3];
    float pos[3];
    bool lock_to_pointer;
    bool world_space;
} we_particle_cp_t;

typedef struct {
    float pos[3];
    float uv[2];
    float color[4];
} we_particle_vertex_t;

typedef struct we_particle_sys {
    uint32_t max_count;
    uint32_t padded_cap;
    uint32_t alive_count;
    float start_time;
    double sim_time;
    uint64_t rng_state;

    char material_path[256];
    bool world_space;
    bool perspective;

    we_renderer_type_t renderer_type;
    float trail_length;
    float trail_min_length;
    float trail_max_length;
    uint32_t subdivision;

    we_particle_emitter_t emitters[WE_PARTICLE_MAX_EMITTERS];
    uint32_t emitter_count;

    we_particle_initializer_t initializers[WE_PARTICLE_MAX_INITIALIZERS];
    uint32_t initializer_count;

    we_particle_operator_t operators[WE_PARTICLE_MAX_OPERATORS];
    uint32_t operator_count;

    we_particle_cp_t control_points[WE_PARTICLE_MAX_CONTROL_PTS];
    uint32_t control_point_count;

    /* Single 32-byte-aligned SoA memory block */
    float *soa_block;

    float *pos_x;
    float *pos_y;
    float *pos_z;
    float *init_pos_x;
    float *init_pos_y;
    float *init_pos_z;
    float *vel_x;
    float *vel_y;
    float *vel_z;
    float *acc_x;
    float *acc_y;
    float *acc_z;
    float *r;
    float *g;
    float *b;
    float *a;
    float *init_r;
    float *init_g;
    float *init_b;
    float *init_a;
    float *size;
    float *init_size;
    float *rot_x;
    float *rot_y;
    float *rot_z;
    float *ang_vel_x;
    float *ang_vel_y;
    float *ang_vel_z;
    float *age;
    float *max_life;
    float *rng_phase;

    /* Batched quad vertex buffer (4 vertices per quad) */
    we_particle_vertex_t *vert_buf;
    uint32_t quad_cap;
    uint32_t quad_count;
    uint32_t vert_count;

    /* OpenGL ES 3.1 resources */
    uint32_t gl_vao;
    uint32_t gl_vbo;
    uint32_t gl_ebo;
    uint32_t gl_quad_cap;
    bool gl_uploaded;
    bool paused;
    uint32_t preset_blending;
} we_particle_sys_t;

/* Parses a Wallpaper Engine particle system JSON definition and allocates 32B-aligned SoA arrays */
we_particle_sys_t *we_particle_create_from_json(const char *json_str, size_t len);

/* Updates mouse-linked control points in world/object space */
void we_particle_set_pointer(we_particle_sys_t *sys, float world_x, float world_y, float world_z);

/* Steps emitters, AVX2 pre-operator reset, AVX2 operators, age increment, and dead compaction */
void we_particle_update(we_particle_sys_t *sys, float dt_sec);

/* Builds 4-vertex quad batches for sprite, spritetrail, or ropetrail renderers.
 * Returns number of vertices generated (4 * quad_count). */
uint32_t we_particle_build_vertices(we_particle_sys_t *sys, we_particle_vertex_t **out_verts);

/* Uploads/initializes GL VAO/VBO/EBO for batched particle rendering */
bool we_particle_upload_gl(we_particle_sys_t *sys);

/* Streams current particle vertex batch and renders with glDepthMask(GL_FALSE) */
void we_particle_draw_gl(we_particle_sys_t *sys, uint32_t gl_prog);

/* Reallocates 32B-aligned SoA arrays and quad vertex buffer when max_count increases */
bool we_particle_realloc_capacity(we_particle_sys_t *sys, uint32_t new_max_count);

/* Resets simulation state and re-runs start_time pre-warm after instance overrides are applied */
void we_particle_reset_and_prewarm(we_particle_sys_t *sys);

/* Bakes knots into a 65-sample LUT using Fritsch-Carlson monotone cubic Hermite (or
 * linear/step/bezier) */
bool we_particle_bake_scalar_lut(const we_particle_curve_knot_t *knots, uint32_t knot_count,
                                 float out_lut[WE_PARTICLE_RAMP_SAMPLES]);

/* Evaluates a 65-sample ramp LUT at normalized lifetime t in [0, 1] via linear interpolation */
float we_particle_eval_scalar_lut(const float lut[WE_PARTICLE_RAMP_SAMPLES], float t);

/* Bakes sRGB color stops into 65-sample RGB LUTs interpolated perceptually in OKLab */
bool we_particle_bake_oklab_lut(const we_particle_color_stop_t *stops, uint32_t stop_count,
                                float out_r[WE_PARTICLE_RAMP_SAMPLES],
                                float out_g[WE_PARTICLE_RAMP_SAMPLES],
                                float out_b[WE_PARTICLE_RAMP_SAMPLES]);

/* Applies a baked ramp LUT to the target operator (or appends one if operator_index == UINT32_MAX)
 */
bool we_particle_apply_ramp_lut(we_particle_sys_t *sys, const we_particle_ramp_lut_t *lut);

/* Returns canonical metadata for one of the 8 clean-room particle presets */
const we_particle_preset_desc_t *we_particle_get_preset_desc(we_particle_preset_id_t preset_id);

/* Instantiates one of the 8 clean-room particle presets and optionally runs start_time pre-warm */
we_particle_sys_t *we_particle_create_preset(we_particle_preset_id_t preset_id, bool prewarm);

/* Emits an immediate burst of particles from emitter 0 without altering continuous emission */
void we_particle_trigger_burst(we_particle_sys_t *sys, uint32_t count);

/* Frees all CPU and GPU resources owned by a we_particle_sys_t */
void we_particle_destroy(we_particle_sys_t *sys);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WE_PARTICLE_H */
