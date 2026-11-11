// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WE_PHYSICS_H
#define WAYWAL_WE_PHYSICS_H

#include "waywal/we_puppet.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WE_PHYSICS_SUBSTEP_HZ   240.0f
#define WE_PHYSICS_SUBSTEP_DT   (1.0f / 240.0f)
#define WE_PHYSICS_FIXED_DT     (1.0f / 240.0f)
#define WE_PHYSICS_MAX_SUBSTEPS 8u
#define WE_PHYSICS_SOLVER_ITERS 2u

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
static_assert(sizeof(we_bone_sim_config_t) == 64, "we_bone_sim_config_t size mismatch");

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
static_assert(sizeof(we_bone_sim_state_t) == 64, "we_bone_sim_state_t size mismatch");

typedef struct {
    uint32_t enabled;         /* 0 = disabled, 1 = active */
    float base_direction_rad; /* Dominant wind angle in radians */
    float base_speed_px_s;    /* Mean laminar wind speed in pixels/sec */
    float gust_frequency_hz;  /* Temporal frequency of Simplex curl gusts */
    float gust_amplitude;     /* Peak gust modulation factor [0.0, 4.0] */
    float curl_spatial_scale; /* Spatial frequency 1/L of 2D/3D Simplex potential */
    float gravity_vec[2];     /* World gravity (px/s^2), default {0.0f, -980.0f} */
} we_wind_field_t;
static_assert(sizeof(we_wind_field_t) == 32, "we_wind_field_t size mismatch");

typedef struct {
    uint32_t enabled;       /* 0 = disabled, 1 = active */
    float radius_px;        /* Repulsion/attraction field radius R */
    float impulse_strength; /* Force gain (>0 repels, <0 attracts) */
    float falloff_power;    /* Radial falloff exponent p in (1 - d/R)^p */
} we_cursor_force_t;
static_assert(sizeof(we_cursor_force_t) == 16, "we_cursor_force_t size mismatch");

typedef struct we_puppet_physics_ctx {
    we_bone_sim_config_t configs[WE_MDL_MAX_BONES];
    we_bone_sim_state_t states[WE_MDL_MAX_BONES];
    we_wind_field_t wind;
    we_cursor_force_t cursor;
    float time_accumulator_sec;
    double total_sim_time_sec;
    uint32_t active_bone_count;
    bool initialized;

    /* Runtime dynamic cursor and audio inputs */
    float cursor_pos[2];
    float cursor_vel[2];
    float audio_bands[64];
    float prev_audio_bass;

    /* Cached bind-pose local rest vectors to tip */
    float rest_dir_local[WE_MDL_MAX_BONES][3];
} we_puppet_physics_ctx_t;

/* Populates default XPBD parameters from puppet bones and simulation_json */
void we_physics_init_from_puppet(we_puppet_physics_ctx_t *ctx, const we_puppet_t *puppet);

/* Applies material preset parameters (Silk, LongHair, HeavyCape, FoliageBranch, SoftRibbon,
 * MetallicChain) */
void we_physics_apply_preset(we_bone_sim_config_t *cfg, we_physics_preset_t preset);
void we_physics_load_preset(we_bone_sim_config_t *cfg, we_physics_preset_t preset);

/* Parses simulation_json from MDL chunk (stiffness, damping, gravity, mass, length, max_angle,
 * wind, audio) */
bool we_physics_parse_simulation_json(const char *json, we_bone_sim_config_t *out_cfg);

/* Evaluates divergence-free 3D Simplex curl-noise wind velocity at (x, y, z, t) */
void we_physics_eval_curl_wind_3d(const we_wind_field_t *wind, float x, float y, float z,
                                  double time_sec, float out_vel[3]);

/* Evaluates 2D Simplex curl-noise wind velocity at (x, y, t) */
void we_physics_sample_curl_wind(const we_wind_field_t *wind, float x, float y, double time_sec,
                                 float out_wind_vel[2]);

/* Dynamic cursor and audio input setters */
void we_physics_set_cursor(we_puppet_physics_ctx_t *ctx, float x, float y, float vx, float vy);
void we_physics_set_audio_bands(we_puppet_physics_ctx_t *ctx, const float bands[64]);

/* Evaluates a single bone substep or applies current deflection rotation */
void we_physics_substep_bone(we_puppet_physics_ctx_t *ctx, uint32_t k, const float pivot[3],
                             float W[16], float dt_s, double t);
void we_physics_apply_bone_rotation(const we_puppet_physics_ctx_t *ctx, uint32_t k,
                                    const float pivot[3], float W[16]);

/* Steps fixed 240 Hz XPBD solver and post-multiplies simulated bone rotations into
 * inout_skin_mats[k] = world_mats[k] * R_delta * offset_trans[k] (zero heap allocs). */
void we_physics_step_puppet(we_puppet_physics_ctx_t *ctx, const we_puppet_t *puppet,
                            float frame_dt_sec, float (*inout_skin_mats)[16]);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WE_PHYSICS_H */
