// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WE_MESH_RIG_H
#define WAYWAL_WE_MESH_RIG_H

#include "waywal/we_physics.h"
#include "waywal/we_puppet.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WE_RIG_MAX_IK_CHAINS       32u
#define WE_RIG_MAX_AIM_CONSTRAINTS 32u
#define WE_RIG_COW_BLOCK_VERTS     256u
#define WE_RIG_HEAT_DEFAULT_REG    1e-3f
#define WE_RIG_HEAT_MAX_FILL_NNZ   (8u * 1024u * 1024u)

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

typedef enum : uint32_t {
    WE_BONE_DRIVE_FK = 0u,
    WE_BONE_DRIVE_IK = 1u,
    WE_BONE_DRIVE_AIM = 2u,
    WE_BONE_DRIVE_XPBD = 3u,
} we_bone_drive_mode_t;

typedef enum : uint32_t {
    WE_WEIGHT_OP_ADD = 0u,
    WE_WEIGHT_OP_SUBTRACT = 1u,
    WE_WEIGHT_OP_REPLACE = 2u,
    WE_WEIGHT_OP_SMOOTH = 3u,
    WE_WEIGHT_OP_NORMALIZE = 4u,
} we_weight_brush_op_t;

typedef enum : uint32_t {
    WE_HEATMAP_PALETTE_TURBO = 0u,
    WE_HEATMAP_PALETTE_VIRIDIS = 1u,
    WE_HEATMAP_PALETTE_INFERNO = 2u,
} we_heatmap_palette_t;

typedef enum : uint32_t {
    WE_ANIM_BLEND_OVERRIDE = 0u,
    WE_ANIM_BLEND_ADDITIVE = 1u,
} we_anim_blend_mode_t;

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

typedef struct {
    float conductivity_scale;       /* Heat conductivity scale c_heat, default 1.0f */
    float falloff_power;            /* Distance exponent clamped [0.5f, 8.0f], default 2.0f */
    float smooth_factor;            /* Post-solve 1-ring cotangent smoothing [0.0f, 1.0f] */
    uint32_t max_influences;        /* Max per-vertex influences [1..4], default 4u */
    float prune_epsilon;            /* Sub-threshold prune cutoff, default 1e-4f */
    bool use_visibility_raycast;    /* Occlusion-aware boundary raycast, default true */
    bool use_intrinsic_delaunay;    /* Bobenko-Springborn iDT flips for M-matrix guarantee */
    const uint64_t *locked_vert_bs; /* Optional bitset of size ceil(vertex_count / 64) */
} we_heat_skin_cfg_t;

typedef struct {
    uint32_t vertex_count;
    uint32_t bone_count;
    uint32_t island_count;
    uint32_t idt_edge_flips;
    uint32_t factor_nnz;
    bool used_ic0_fallback;
    double solve_time_ms;
} we_heat_solve_stats_t;

typedef struct {
    uint16_t root_bone;
    uint16_t mid_bone;
    uint16_t end_bone;
    int8_t bend_sign; /* +1 or -1 for 2D planar bend preference */
    uint8_t enabled;
    float target_world[3];
    float pole_world[3];
    float soft_zone_ratio;   /* [0.0f, 0.25f], default 0.05f */
    float max_stretch_ratio; /* [1.0f, 1.5f], 1.0f disables stretch */
    float root_angle_min_rad;
    float root_angle_max_rad;
    float mid_angle_min_rad;
    float mid_angle_max_rad;
    float blend_weight; /* [0.0f, 1.0f] */
} we_ik_chain_t;

typedef struct {
    uint16_t bone_index;
    uint8_t enabled;
    uint8_t _pad;
    float aim_axis_local[3];
    float target_world[3];
    float max_cone_rad;
    float blend_weight;
} we_aim_constraint_t;

typedef struct {
    we_ik_chain_t ik_chains[WE_RIG_MAX_IK_CHAINS];
    uint32_t ik_chain_count;
    we_aim_constraint_t aim_constraints[WE_RIG_MAX_AIM_CONSTRAINTS];
    uint32_t aim_count;
    we_bone_drive_mode_t bone_drive_modes[WE_MDL_MAX_BONES];
    we_anim_blend_mode_t layer_blend_modes[WE_MDL_MAX_LAYERS];
} we_rig_pose_ctx_t;

void we_heat_skin_cfg_default(we_heat_skin_cfg_t *out_cfg);

bool we_mesh_solve_heat_weights(we_puppet_t *puppet, const we_heat_skin_cfg_t *cfg,
                                we_heat_solve_stats_t *out_stats);

void we_mesh_renormalize_vertex_weights(we_mdl_vertex_t *vert, uint32_t bone_count,
                                        uint32_t active_bone, float target_weight,
                                        const uint64_t locked_bone_mask[2]);

uint32_t we_mesh_apply_weight_paint_dab(we_puppet_t *puppet, const float axy[2], const float bxy[2],
                                        float radius_a, float radius_b, float strength,
                                        we_weight_brush_op_t op, uint32_t active_bone,
                                        float target_value, const uint64_t locked_bone_mask[2],
                                        const uint64_t *locked_vert_bs,
                                        uint64_t *inout_dirty_block_mask);

bool we_ik_solve_two_bone(const we_puppet_t *puppet, const we_ik_chain_t *chain,
                          float (*inout_anim_local)[16]);

bool we_ik_solve_aim(const we_puppet_t *puppet, const we_aim_constraint_t *aim,
                     float (*inout_anim_local)[16]);

void we_rig_apply_constraints(const we_puppet_t *puppet, const we_rig_pose_ctx_t *rig,
                              float (*inout_anim_local)[16]);

void we_puppet_eval_skin_matrices_ex(const we_puppet_t *puppet, const we_puppet_layer_t *layers,
                                     uint32_t layer_count, const we_rig_pose_ctx_t *rig,
                                     double time_sec, float (*out_skin_mats)[16]);

bool we_mesh_rig_draw_heatmap_gl(const we_puppet_t *puppet, const float (*skin_mats)[16],
                                 uint8_t mode, uint8_t palette, uint16_t active_bone,
                                 float opacity);

void we_gpu_weight_brush_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WE_MESH_RIG_H */
