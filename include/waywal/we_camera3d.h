// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WE_CAMERA3D_H
#define WAYWAL_WE_CAMERA3D_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WE_CAMERA_MAX_KEYFRAMES 64u
#define WE_MAX_SCENE_LIGHTS     8u

typedef enum : uint32_t {
    WE_PROJ_ORTHOGRAPHIC_2D = 0,
    WE_PROJ_PERSPECTIVE_3D = 1,
    WE_PROJ_DEPTH_PARALLAX_25D = 2,
} we_projection_mode_t;

typedef enum : uint32_t {
    WE_CAM_PATH_LOOP = 0,
    WE_CAM_PATH_PINGPONG = 1,
    WE_CAM_PATH_ONCE = 2,
} we_camera_loop_mode_t;

typedef struct {
    float time_sec;            /* Keyframe timestamp in seconds */
    float position[3];         /* Camera eye position in 3D scene space */
    float orientation_quat[4]; /* Unit quaternion (x, y, z, w) for SQUAD slerp */
    float fov_deg;             /* Vertical field of view in degrees */
    float focal_distance;      /* Depth-of-field focus plane distance */
    float aperture_fstop;      /* CoC aperture f-stop (e.g. 2.8) */
    float tension;             /* Centripetal Catmull-Rom alpha in [0.0, 1.0], default 0.5 */
} we_camera_kf_t;
static_assert(sizeof(we_camera_kf_t) == 48, "we_camera_kf_t must be exactly 48 bytes");

/* 192-byte combined camera transform matrix block (view, proj, view_proj) */
typedef struct {
    float view_matrix[16];      /* Column-major 4x4 View matrix (64 B) */
    float proj_matrix[16];      /* Column-major 4x4 Projection matrix (64 B) */
    float view_proj_matrix[16]; /* Column-major 4x4 combined VP matrix (64 B) */
} we_camera3d_matrices_t;
static_assert(sizeof(we_camera3d_matrices_t) == 192,
              "we_camera3d_matrices_t matrix block must be exactly 192 bytes");

typedef struct {
    uint32_t projection_mode;   /* we_projection_mode_t */
    uint32_t path_enabled;      /* 0 = Static/Mouse Orbit, 1 = Catmull-Rom Path */
    uint32_t path_loop_mode;    /* we_camera_loop_mode_t */
    uint32_t keyframe_count;    /* 0..WE_CAMERA_MAX_KEYFRAMES */
    float eye_pos[3];           /* Current camera world position */
    float target_pos[3];        /* Look-at target position */
    float up_vec[3];            /* World up vector, default {0.0f, 1.0f, 0.0f} */
    float fov_y_deg;            /* Vertical FOV in degrees [10.0, 140.0] */
    float near_z;               /* Near clip plane (> 0.01) */
    float far_z;                /* Far clip plane */
    float parallax_amount[2];   /* Mouse orbit sway amplitude X/Y */
    float parallax_smoothing;   /* Exponential smoothing factor [0.01, 1.0] */
    float dof_enabled;          /* 0.0 = disabled, 1.0 = Circle-of-Confusion DOF active */
    float dof_focal_dist;       /* Focus plane Z distance */
    float dof_focal_region;     /* In-focus depth band width */
    float dof_bokeh_radius_px;  /* Maximum CoC blur disk radius in pixels [0.0, 16.0] */
    float reserved_pad;         /* Explicit 4-byte pad aligning matrices to offset 96 (16B) */
    float view_matrix[16];      /* Column-major 4x4 View matrix */
    float proj_matrix[16];      /* Column-major 4x4 Projection matrix */
    float view_proj_matrix[16]; /* Column-major 4x4 combined VP matrix */
    we_camera_kf_t keyframes[WE_CAMERA_MAX_KEYFRAMES];
} we_camera3d_t;
static_assert(sizeof(we_camera3d_t) == 3360,
              "we_camera3d_t must be exactly 3360 bytes with 16-byte aligned matrices");

typedef enum : uint32_t {
    WE_LIGHT_DISABLED = 0,
    WE_LIGHT_POINT = 1,
    WE_LIGHT_SPOT = 2,
    WE_LIGHT_DIRECTIONAL = 3,
    WE_LIGHT_TUBE = 4,
} we_light_type_t;

/* std140-aligned 64-byte light descriptor */
typedef struct {
    float position_or_dir[3]; /* World pos (Point/Spot/Tube) or normalized dir (Directional) */
    uint32_t light_type;      /* we_light_type_t */
    float color_rgb[3];       /* Linear RGB color */
    float intensity;          /* Luminous intensity multiplier [0.0, 20.0] */
    float attenuation_radius; /* Falloff radius R > 0 */
    float spot_inner_cos;     /* cos(inner_cone_angle_rad) */
    float spot_outer_cos;     /* cos(outer_cone_angle_rad) */
    float tube_length;        /* Segment length for WE_LIGHT_TUBE */
    float spot_dir[3];        /* Normalized spot/tube axis vector */
    float specular_power;     /* Blinn-Phong shininess exponent [1.0, 256.0] */
} we_scene_light_t;
static_assert(sizeof(we_scene_light_t) == 64,
              "we_scene_light_t must be exactly 64 bytes (std140 layout)");

/* std140-aligned 544-byte scene lighting uniform buffer object (UBO binding = 2) */
typedef struct {
    float ambient_color_rgb[3];  /* Hemisphere ambient RGB */
    uint32_t active_light_count; /* 0..WE_MAX_SCENE_LIGHTS */
    float sky_color_rgb[3];      /* Upper hemisphere tint RGB */
    float normal_map_strength;   /* Tangent-space normal map bump scale [0.0, 2.0] */
    we_scene_light_t lights[WE_MAX_SCENE_LIGHTS];
} we_lighting_uniforms_t;
static_assert(sizeof(we_lighting_uniforms_t) == 544,
              "we_lighting_uniforms_t must be exactly 544 bytes (std140 UBO)");

/* Initializes camera with standard 60-deg perspective or 2D ortho defaults */
void we_camera3d_init_default(we_camera3d_t *cam, float viewport_w, float viewport_h);

/* Evaluates centripetal Catmull-Rom (alpha = 0.5) position spline with 5-point Gauss-Legendre
 * arc-length parameterization + Shoemake SQUAD cubic quaternion orientation interpolation. */
void we_camera3d_eval_spline(const we_camera_kf_t *kfs, uint32_t count, uint32_t loop_mode,
                             double time_sec, float out_pos[3], float out_quat[4], float *out_fov,
                             float *out_focal_dist, float *out_aperture);

/* Evaluates Catmull-Rom spline + pointer parallax and updates view/proj/view_proj matrices */
void we_camera3d_evaluate(we_camera3d_t *cam, double time_sec, float pointer_norm_x,
                          float pointer_norm_y, float aspect_ratio);

/* Updates camera matrices, parallax, and CoC parameters (zero heap allocations per frame) */
void we_camera3d_update(we_camera3d_t *cam, double time_sec, float pointer_norm_x,
                        float pointer_norm_y, float aspect_ratio);

/* Computes Circle-of-Confusion radius in pixels for a layer at layer_z */
float we_camera3d_compute_coc_px(const we_camera3d_t *cam, float layer_z);

/* Computes cofactor (M_{3x3}^-1)^T normal matrix in 3x3 column-major order (9 floats) */
void we_camera3d_compute_normal_matrix(const float model[16], float out_normal_mat[9]);

/* Packs up to WE_MAX_SCENE_LIGHTS dynamic lights + ambient into 544-byte std140 UBO */
void we_lighting_pack_ubo(const we_scene_light_t *lights, uint32_t light_count,
                          const float ambient_rgb[3], const float sky_rgb[3],
                          float normal_map_strength, we_lighting_uniforms_t *out_ubo);

/* Matrix and Quaternion Math Utilities */
void mat4_perspective_rh_gl(float fovy_rad, float aspect, float near_z, float far_z, float out[16]);
void mat4_look_at(const float eye[3], const float target[3], const float up[3], float out[16]);
void mat4_ortho_gl(float left, float right, float bottom, float top, float near_z, float far_z,
                   float out[16]);
void mat4_mul_mat4(const float a[16], const float b[16], float out[16]);

void quat_identity(float q[4]);
void quat_normalize(float q[4]);
void quat_slerp(const float q0[4], const float q1[4], float t, float out[4]);
void quat_squad(const float q0[4], const float a[4], const float b[4], const float q1[4], float t,
                float out[4]);
void quat_to_mat4(const float q[4], const float pos[3], float out[16]);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WE_CAMERA3D_H */
