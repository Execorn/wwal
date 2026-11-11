// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_IPC_PROTO_PHASE3_H
#define WAYWAL_IPC_PROTO_PHASE3_H

#include "waywal/ipc_proto.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if __has_include("waywal/shader_graph.h")
#include "waywal/shader_graph.h"
#endif

#if __has_include("waywal/we_physics.h")
#include "waywal/we_physics.h"
#else
#ifndef WAYWAL_WE_PHYSICS_H
#define WAYWAL_WE_PHYSICS_H

typedef enum : uint32_t {
    WE_BONE_PHYS_ENABLED = (1u << 0),
    WE_BONE_PHYS_ANGULAR_LIMIT = (1u << 1),
    WE_BONE_PHYS_CURSOR_COLLIDE = (1u << 2),
    WE_BONE_PHYS_PINNED_ROOT = (1u << 3),
} we_bone_phys_flags_t;

typedef enum : uint32_t {
    WE_PHYS_PRESET_CUSTOM = 0,
    WE_PHYS_PRESET_SILK = 1,
    WE_PHYS_PRESET_LONG_HAIR = 2,
    WE_PHYS_PRESET_HEAVY_CAPE = 3,
    WE_PHYS_PRESET_FOLIAGE_BRANCH = 4,
    WE_PHYS_PRESET_SOFT_RIBBON = 5,
    WE_PHYS_PRESET_METALLIC_CHAIN = 6,
} we_physics_preset_t;

typedef struct {
    uint32_t bone_index;       /* Target bone index in we_puppet_t (0..127) */
    uint32_t flags;            /* Bitmask of we_bone_phys_flags_t */
    float compliance_stretch;  /* XPBD distance compliance alpha_d = 1/k_d (m/N) */
    float compliance_bend;     /* XPBD angular bending compliance alpha_b = 1/k_b (rad/Nm) */
    float damping_ratio;       /* Rayleigh / XPBD velocity damping zeta in [0.0, 2.0] */
    float mass_kg;             /* Effective particle tip mass in [0.01, 50.0] kg */
    float gravity_scale;       /* Multiplier on scene gravity vector */
    float wind_drag_coeff;     /* Aerodynamic coupling coefficient in [0.0, 5.0] */
    float angle_limit_min_rad; /* Minimum local swing angle delta in [-pi, 0] */
    float angle_limit_max_rad; /* Maximum local swing angle delta in [0, pi] */
    float collision_radius_px; /* Capsule radius for cursor SDF repulsion in pixels */
    float rest_length_px;      /* Rest distance between bone origin and tip in pixels */
    float audio_drag_coeff;    /* Coupling coefficient for PipeWire audio impulses */
    float rayleigh_damping;    /* Kelvin-Voigt high-frequency damping factor */
    float reserved[2];         /* 64-byte alignment padding */
} we_bone_sim_config_t;
static_assert(sizeof(we_bone_sim_config_t) == 64, "we_bone_sim_config_t must be exactly 64 bytes");

typedef struct {
    float pos_curr[3];       /* Current world-space bone tip position x_i^n */
    float pos_prev[3];       /* Previous substep world-space bone tip position x_i^{n-1} */
    float vel[3];            /* Current world-space velocity v_i^n */
    float lambda_stretch;    /* Accumulated XPBD Lagrange multiplier for stretch */
    float lambda_bend;       /* Accumulated XPBD Lagrange multiplier for bending */
    float current_angle_rad; /* Solved local Z-axis angular deflection (radians) */
    float target_pos[3];     /* Animated target rest tip position x_i^target */
    float reserved;          /* 64-byte alignment padding */
} we_bone_sim_state_t;
static_assert(sizeof(we_bone_sim_state_t) == 64, "we_bone_sim_state_t must be exactly 64 bytes");

typedef struct {
    uint32_t enabled;         /* 0 = disabled, 1 = active */
    float base_direction_rad; /* Dominant wind angle in radians */
    float base_speed_px_s;    /* Mean laminar wind speed in pixels/sec */
    float gust_frequency_hz;  /* Temporal frequency of Simplex curl gusts */
    float gust_amplitude;     /* Peak gust modulation factor [0.0, 4.0] */
    float curl_spatial_scale; /* Spatial frequency 1/L of 2D/3D Simplex potential */
    float gravity_vec[2];     /* World gravity (px/s^2), default {0.0f, -980.0f} */
} we_wind_field_t;
static_assert(sizeof(we_wind_field_t) == 32, "we_wind_field_t must be exactly 32 bytes");

typedef struct {
    uint32_t enabled;       /* 0 = disabled, 1 = active */
    float radius_px;        /* Repulsion/attraction field radius R */
    float impulse_strength; /* Force gain (>0 repels, <0 attracts) */
    float falloff_power;    /* Radial falloff exponent p in (1 - d/R)^p */
} we_cursor_force_t;
static_assert(sizeof(we_cursor_force_t) == 16, "we_cursor_force_t must be exactly 16 bytes");

#endif /* WAYWAL_WE_PHYSICS_H */
#endif

#if __has_include("waywal/we_camera3d.h")
#include "waywal/we_camera3d.h"
#else
#ifndef WAYWAL_WE_CAMERA3D_H
#define WAYWAL_WE_CAMERA3D_H

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

typedef enum : uint32_t {
    WE_LIGHT_DISABLED = 0,
    WE_LIGHT_POINT = 1,
    WE_LIGHT_SPOT = 2,
    WE_LIGHT_DIRECTIONAL = 3,
    WE_LIGHT_TUBE = 4,
} we_light_type_t;

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

typedef struct {
    float ambient_color_rgb[3];  /* Hemisphere ambient RGB */
    uint32_t active_light_count; /* 0..WE_MAX_SCENE_LIGHTS */
    float sky_color_rgb[3];      /* Upper hemisphere tint RGB */
    float normal_map_strength;   /* Tangent-space normal map bump scale [0.0, 2.0] */
    we_scene_light_t lights[WE_MAX_SCENE_LIGHTS];
} we_lighting_uniforms_t;
static_assert(sizeof(we_lighting_uniforms_t) == 544,
              "we_lighting_uniforms_t must be exactly 544 bytes (std140 UBO)");

#endif /* WAYWAL_WE_CAMERA3D_H */
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Shader Graph, 3D Camera, Physics & Lighting Request Opcodes (0x001D .. 0x0020) */
#ifndef WAYWAL_REQ_COMPILE_SHADER_GRAPH
#define WAYWAL_REQ_COMPILE_SHADER_GRAPH 0x001Du
#endif
#ifndef WAYWAL_REQ_SET_CAMERA_3D
#define WAYWAL_REQ_SET_CAMERA_3D 0x001Eu
#endif
#ifndef WAYWAL_REQ_SET_PHYSICS_PARAMS
#define WAYWAL_REQ_SET_PHYSICS_PARAMS 0x001Fu
#endif
#ifndef WAYWAL_REQ_SET_SCENE_LIGHTS
#define WAYWAL_REQ_SET_SCENE_LIGHTS 0x0020u
#endif

/* Shader Graph, 3D Camera, Physics & Lighting Response Opcodes (0x800C .. 0x800F) */
#ifndef WAYWAL_RESP_COMPILE_SHADER_GRAPH
#define WAYWAL_RESP_COMPILE_SHADER_GRAPH 0x800Cu
#endif
#ifndef WAYWAL_RESP_SET_CAMERA_3D
#define WAYWAL_RESP_SET_CAMERA_3D 0x800Du
#endif
#ifndef WAYWAL_RESP_SET_PHYSICS_PARAMS
#define WAYWAL_RESP_SET_PHYSICS_PARAMS 0x800Eu
#endif
#ifndef WAYWAL_RESP_SET_SCENE_LIGHTS
#define WAYWAL_RESP_SET_SCENE_LIGHTS 0x800Fu
#endif

/* Shader Graph & Scene Mutation Status Codes */
#ifndef WAYWAL_STATUS_OK
#define WAYWAL_STATUS_OK 0u
#endif
#ifndef WAYWAL_STATUS_ERR_SYNTAX
#define WAYWAL_STATUS_ERR_SYNTAX 1u
#endif
#ifndef WAYWAL_STATUS_ERR_CYCLE
#define WAYWAL_STATUS_ERR_CYCLE 2u
#endif

/* Flags for WAYWAL_REQ_COMPILE_SHADER_GRAPH */
#define WAYWAL_COMPILE_FLAG_VALIDATE_ONLY 0x0001u
#define WAYWAL_COMPILE_FLAG_HOT_SWAP      0x0002u
#define WAYWAL_COMPILE_FLAG_DUMP_GLSL     0x0004u

/* Flags for WAYWAL_REQ_SET_CAMERA_3D */
#define WAYWAL_CAMERA_FLAG_ENABLE_DOF       0x0001u
#define WAYWAL_CAMERA_FLAG_ENABLE_KEYFRAMES 0x0002u
#define WAYWAL_CAMERA_FLAG_RESET            0x0004u

#pragma pack(push, 1)

/* WAYWAL_REQ_COMPILE_SHADER_GRAPH (0x001D) */
typedef struct {
    char target_output[64];     /* Output name or "*" for all outputs */
    char graph_json_path[1024]; /* Path to .wwgraph file, or empty if inline JSON follows */
    uint32_t inline_json_len;   /* Byte length of inline .wwgraph JSON following this struct */
    uint32_t flags;             /* 0x1 = Validate only, 0x2 = Hot-swap active, 0x4 = Dump GLSL */
} waywal_ipc_req_compile_shader_graph_t;
static_assert(sizeof(waywal_ipc_req_compile_shader_graph_t) == 1096,
              "waywal_ipc_req_compile_shader_graph_t must be exactly 1096 bytes");
typedef waywal_ipc_req_compile_shader_graph_t waywal_ipc_compile_shader_graph_req_t;
static_assert(sizeof(waywal_ipc_compile_shader_graph_req_t) == 1096, "size mismatch");

/* WAYWAL_RESP_COMPILE_SHADER_GRAPH (0x800C) */
typedef struct {
    uint32_t status_code;         /* 0 = OK, non-zero = error */
    uint32_t active_pass_mask;    /* Bitmask of compiled passes */
    uint32_t compiled_node_count; /* Reachable live nodes emitted */
    uint32_t estimated_alu_ops;   /* Static ALU operation count */
    float compile_time_ms;        /* Total DAG->GLSL->GLES link time in milliseconds */
    uint32_t offending_node_id;   /* Node ID if cycle or type error occurred */
    char error_msg[256];          /* Compiler/linker diagnostic string */
} waywal_ipc_resp_compile_shader_graph_t;
static_assert(sizeof(waywal_ipc_resp_compile_shader_graph_t) == 280,
              "waywal_ipc_resp_compile_shader_graph_t must be exactly 280 bytes");
typedef waywal_ipc_resp_compile_shader_graph_t waywal_ipc_compile_shader_graph_resp_t;
static_assert(sizeof(waywal_ipc_compile_shader_graph_resp_t) == 280, "size mismatch");

/* WAYWAL_REQ_SET_CAMERA_3D (0x001E) */
typedef struct {
    char target_output[64];   /* Output name or "*" */
    uint32_t projection_mode; /* we_projection_mode_t */
    uint32_t flags;           /* 0x1 = Enable DOF, 0x2 = Enable Keyframe Path, 0x4 = Reset */
    float eye_pos[3];
    float target_pos[3];
    float up_vec[3];
    float fov_y_deg;
    float near_z;
    float far_z;
    float parallax_amount[2];
    float parallax_smoothing;
    float dof_focal_dist;
    float dof_focal_region;
    float dof_bokeh_radius_px;
} waywal_ipc_req_set_camera_3d_t;
static_assert(sizeof(waywal_ipc_req_set_camera_3d_t) == 144,
              "waywal_ipc_req_set_camera_3d_t must be exactly 144 bytes");
typedef waywal_ipc_req_set_camera_3d_t waywal_ipc_set_camera_3d_req_t;
static_assert(sizeof(waywal_ipc_set_camera_3d_req_t) == 144, "size mismatch");

/* WAYWAL_RESP_SET_CAMERA_3D (0x800D) */
typedef struct {
    uint32_t status_code;       /* 0 = OK */
    float active_view_proj[16]; /* Resulting 4x4 View-Projection matrix */
} waywal_ipc_resp_set_camera_3d_t;
static_assert(sizeof(waywal_ipc_resp_set_camera_3d_t) == 68,
              "waywal_ipc_resp_set_camera_3d_t must be exactly 68 bytes");
typedef waywal_ipc_resp_set_camera_3d_t waywal_ipc_set_camera_3d_resp_t;
static_assert(sizeof(waywal_ipc_set_camera_3d_resp_t) == 68, "size mismatch");

/* WAYWAL_REQ_SET_PHYSICS_PARAMS (0x001F) */
typedef struct {
    char target_output[64]; /* Output name or "*" */
    int32_t object_id;      /* Target puppet scene object ID, or -1 for primary puppet */
    int32_t bone_index;     /* Target bone index (0..127), or -1 for all simulated bones */
    we_bone_sim_config_t bone_cfg;
    we_wind_field_t wind;
    we_cursor_force_t cursor;
} waywal_ipc_req_set_physics_params_t;
static_assert(sizeof(waywal_ipc_req_set_physics_params_t) == 184,
              "waywal_ipc_req_set_physics_params_t must be exactly 184 bytes");
typedef waywal_ipc_req_set_physics_params_t waywal_ipc_set_physics_params_req_t;
static_assert(sizeof(waywal_ipc_set_physics_params_req_t) == 184, "size mismatch");

/* WAYWAL_RESP_SET_PHYSICS_PARAMS (0x800E) */
typedef struct {
    uint32_t status_code;      /* 0 = OK */
    uint32_t active_sim_bones; /* Number of simulated bones active in puppet */
    float last_step_time_us;   /* Last 240 Hz XPBD step duration in microseconds */
} waywal_ipc_resp_set_physics_params_t;
static_assert(sizeof(waywal_ipc_resp_set_physics_params_t) == 12,
              "waywal_ipc_resp_set_physics_params_t must be exactly 12 bytes");
typedef waywal_ipc_resp_set_physics_params_t waywal_ipc_set_physics_params_resp_t;
static_assert(sizeof(waywal_ipc_set_physics_params_resp_t) == 12, "size mismatch");

/* WAYWAL_REQ_SET_SCENE_LIGHTS (0x0020) */
typedef struct {
    char target_output[64]; /* Output name or "*" */
    we_lighting_uniforms_t lighting;
} waywal_ipc_req_set_scene_lights_t;
static_assert(sizeof(waywal_ipc_req_set_scene_lights_t) == 608,
              "waywal_ipc_req_set_scene_lights_t must be exactly 608 bytes");
typedef waywal_ipc_req_set_scene_lights_t waywal_ipc_set_scene_lights_req_t;
static_assert(sizeof(waywal_ipc_set_scene_lights_req_t) == 608, "size mismatch");

/* WAYWAL_RESP_SET_SCENE_LIGHTS (0x800F) */
typedef struct {
    uint32_t status_code;   /* 0 = OK */
    uint32_t active_lights; /* 0..WE_MAX_SCENE_LIGHTS */
} waywal_ipc_resp_set_scene_lights_t;
static_assert(sizeof(waywal_ipc_resp_set_scene_lights_t) == 8,
              "waywal_ipc_resp_set_scene_lights_t must be exactly 8 bytes");
typedef waywal_ipc_resp_set_scene_lights_t waywal_ipc_set_scene_lights_resp_t;
static_assert(sizeof(waywal_ipc_set_scene_lights_resp_t) == 8, "size mismatch");

#pragma pack(pop)

/* Serialization & Validation Helpers */
bool waywal_ipc_validate_req_compile_shader_graph(const waywal_ipc_req_compile_shader_graph_t *req,
                                                  size_t payload_size,
                                                  const char **out_inline_json);

bool waywal_ipc_serialize_req_compile_shader_graph(
    const char *target_output, const char *graph_json_path, const char *inline_json, uint32_t flags,
    waywal_ipc_hdr_t *out_hdr, waywal_ipc_req_compile_shader_graph_t *out_req, void *out_buf,
    size_t out_buf_cap, size_t *out_total_len);

bool waywal_ipc_validate_resp_compile_shader_graph(
    const waywal_ipc_resp_compile_shader_graph_t *resp, size_t payload_size);

bool waywal_ipc_serialize_resp_compile_shader_graph(
    uint32_t status_code, uint32_t active_pass_mask, uint32_t compiled_node_count,
    uint32_t estimated_alu_ops, float compile_time_ms, uint32_t offending_node_id,
    const char *error_msg, waywal_ipc_hdr_t *out_hdr,
    waywal_ipc_resp_compile_shader_graph_t *out_resp);

bool waywal_ipc_validate_req_set_camera_3d(const waywal_ipc_req_set_camera_3d_t *req,
                                           size_t payload_size);

bool waywal_ipc_serialize_req_set_camera_3d(
    const char *target_output, uint32_t projection_mode, uint32_t flags, const float eye_pos[3],
    const float target_pos[3], const float up_vec[3], float fov_y_deg, float near_z, float far_z,
    const float parallax_amount[2], float parallax_smoothing, float dof_focal_dist,
    float dof_focal_region, float dof_bokeh_radius_px, waywal_ipc_hdr_t *out_hdr,
    waywal_ipc_req_set_camera_3d_t *out_req);

bool waywal_ipc_validate_resp_set_camera_3d(const waywal_ipc_resp_set_camera_3d_t *resp,
                                            size_t payload_size);

bool waywal_ipc_serialize_resp_set_camera_3d(uint32_t status_code, const float active_view_proj[16],
                                             waywal_ipc_hdr_t *out_hdr,
                                             waywal_ipc_resp_set_camera_3d_t *out_resp);

bool waywal_ipc_validate_req_set_physics_params(const waywal_ipc_req_set_physics_params_t *req,
                                                size_t payload_size);

bool waywal_ipc_serialize_req_set_physics_params(const char *target_output, int32_t object_id,
                                                 int32_t bone_index,
                                                 const we_bone_sim_config_t *bone_cfg,
                                                 const we_wind_field_t *wind,
                                                 const we_cursor_force_t *cursor,
                                                 waywal_ipc_hdr_t *out_hdr,
                                                 waywal_ipc_req_set_physics_params_t *out_req);

bool waywal_ipc_validate_resp_set_physics_params(const waywal_ipc_resp_set_physics_params_t *resp,
                                                 size_t payload_size);

bool waywal_ipc_serialize_resp_set_physics_params(uint32_t status_code, uint32_t active_sim_bones,
                                                  float last_step_time_us,
                                                  waywal_ipc_hdr_t *out_hdr,
                                                  waywal_ipc_resp_set_physics_params_t *out_resp);

bool waywal_ipc_validate_req_set_scene_lights(const waywal_ipc_req_set_scene_lights_t *req,
                                              size_t payload_size);

bool waywal_ipc_serialize_req_set_scene_lights(const char *target_output,
                                               const we_lighting_uniforms_t *lighting,
                                               waywal_ipc_hdr_t *out_hdr,
                                               waywal_ipc_req_set_scene_lights_t *out_req);

bool waywal_ipc_validate_resp_set_scene_lights(const waywal_ipc_resp_set_scene_lights_t *resp,
                                               size_t payload_size);

bool waywal_ipc_serialize_resp_set_scene_lights(uint32_t status_code, uint32_t active_lights,
                                                waywal_ipc_hdr_t *out_hdr,
                                                waywal_ipc_resp_set_scene_lights_t *out_resp);

/* Forward declaration for daemon state */
struct daemon_state;

/* Top-level daemon dispatch handler for shader graph, 3D camera, physics & lighting opcodes
 * (0x001D..0x0020) */
bool daemon_dispatch_phase3_ipc(struct daemon_state *state, int client_fd,
                                const waywal_ipc_hdr_t *hdr, int attached_fd);

/* Individual non-blocking dispatch handlers */
bool daemon_dispatch_ipc_req_compile_shader_graph(struct daemon_state *state,
                                                  const waywal_ipc_req_compile_shader_graph_t *req,
                                                  const char *inline_json,
                                                  waywal_ipc_resp_compile_shader_graph_t *resp);

bool daemon_dispatch_ipc_req_set_camera_3d(struct daemon_state *state,
                                           const waywal_ipc_req_set_camera_3d_t *req,
                                           waywal_ipc_resp_set_camera_3d_t *resp);

bool daemon_dispatch_ipc_req_set_physics_params(struct daemon_state *state,
                                                const waywal_ipc_req_set_physics_params_t *req,
                                                waywal_ipc_resp_set_physics_params_t *resp);

bool daemon_dispatch_ipc_req_set_scene_lights(struct daemon_state *state,
                                              const waywal_ipc_req_set_scene_lights_t *req,
                                              waywal_ipc_resp_set_scene_lights_t *resp);

/* Invariant 1 validation: accepts legacy headers >= 72 bytes */
bool daemon_validate_set_image_header(const waywal_ipc_hdr_t *hdr);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_IPC_PROTO_PHASE3_H */
