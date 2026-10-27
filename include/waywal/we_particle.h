// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WE_PARTICLE_H
#define WAYWAL_WE_PARTICLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/* Frees all CPU and GPU resources owned by a we_particle_sys_t */
void we_particle_destroy(we_particle_sys_t *sys);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WE_PARTICLE_H */
