// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_IPC_PROTO_RIGGING_H
#define WAYWAL_IPC_PROTO_RIGGING_H

#include "waywal/ipc_proto.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sequential Rigging & Animation Request Opcodes (0x002B .. 0x0033) */
#define WAYWAL_REQ_PUPPET_SET_MESH         0x002Bu
#define WAYWAL_IPC_OP_PUPPET_SET_MESH      WAYWAL_REQ_PUPPET_SET_MESH
#define WAYWAL_REQ_PUPPET_SET_SKELETON     0x002Cu
#define WAYWAL_IPC_OP_PUPPET_SET_SKELETON  WAYWAL_REQ_PUPPET_SET_SKELETON
#define WAYWAL_REQ_PUPPET_SOLVE_WEIGHTS    0x002Du
#define WAYWAL_IPC_OP_PUPPET_SOLVE_WEIGHTS WAYWAL_REQ_PUPPET_SOLVE_WEIGHTS
#define WAYWAL_REQ_PUPPET_PAINT_WEIGHTS    0x002Eu
#define WAYWAL_IPC_OP_PUPPET_PAINT_WEIGHTS WAYWAL_REQ_PUPPET_PAINT_WEIGHTS
#define WAYWAL_REQ_PUPPET_SET_HEATMAP_MODE 0x002Fu
#define WAYWAL_IPC_OP_PUPPET_SET_HEATMAP   WAYWAL_REQ_PUPPET_SET_HEATMAP_MODE
#define WAYWAL_REQ_PUPPET_PREVIEW_IK       0x0030u
#define WAYWAL_IPC_OP_PUPPET_PREVIEW_IK    WAYWAL_REQ_PUPPET_PREVIEW_IK
#define WAYWAL_REQ_PUPPET_SET_ANIM_TRACKS  0x0031u
#define WAYWAL_IPC_OP_PUPPET_SET_ANIM      WAYWAL_REQ_PUPPET_SET_ANIM_TRACKS
#define WAYWAL_REQ_PUPPET_SCRUB_TIME       0x0032u
#define WAYWAL_IPC_OP_PUPPET_SCRUB_TIME    WAYWAL_REQ_PUPPET_SCRUB_TIME
#define WAYWAL_REQ_EXPORT_PUPPET_MDL       0x0033u
#define WAYWAL_IPC_OP_EXPORT_PUPPET_MDL    WAYWAL_REQ_EXPORT_PUPPET_MDL

/* Sequential Rigging & Animation Response Opcodes (0x801A .. 0x801D) */
#define WAYWAL_RESP_PUPPET_MESH_ACK   0x801Au
#define WAYWAL_RESP_PUPPET_WEIGHTS    0x801Bu
#define WAYWAL_RESP_PUPPET_IK_POSE    0x801Cu
#define WAYWAL_RESP_EXPORT_PUPPET_MDL 0x801Du

#define WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES (8u * 1024u * 1024u)
#define WAYWAL_IPC_RIGGING_MAX_VERTICES      65536u
#define WAYWAL_IPC_RIGGING_MAX_TRIANGLES     131072u
#define WAYWAL_IPC_RIGGING_MAX_BONES         128u
#define WAYWAL_IPC_RIGGING_MAX_ANIMS         64u
#define WAYWAL_IPC_RIGGING_MAX_KEYFRAMES     16384u

#pragma pack(push, 1)
typedef struct {
    float pos[3];
    uint32_t blend_indices[4];
    float weight[4];
    float texcoord[2];
} waywal_ipc_mdl_vertex_t;

typedef struct {
    uint32_t object_id;
    uint32_t vertex_count;   /* [3 .. 65536] */
    uint32_t triangle_count; /* [1 .. 131072] */
    uint32_t flags;          /* bit 0: normalize_weights, bit 1: preserve_skeleton */
} waywal_ipc_puppet_set_mesh_t;
/* Followed by waywal_ipc_mdl_vertex_t[vertex_count] and uint16_t[triangle_count * 3] */

typedef struct {
    int32_t status;
    uint32_t object_id;
    uint32_t vertex_count;
    uint32_t triangle_count;
} waywal_ipc_puppet_mesh_ack_t;

typedef struct {
    char name[128];
    int32_t unk;
    uint32_t parent; /* 0xFFFFFFFFu = root, else < bone_index */
    float local_trans[16];
    char simulation_json[256];
} waywal_ipc_mdl_bone_t;

typedef struct {
    uint32_t object_id;
    uint32_t bone_count; /* [0 .. 128] */
    uint32_t flags;      /* bit 0: reinit_physics_state */
    uint32_t reserved;
} waywal_ipc_puppet_set_skeleton_t;
/* Followed by waywal_ipc_mdl_bone_t[bone_count] */

typedef struct {
    uint32_t object_id;
    float falloff_power;             /* [0.5f .. 8.0f] */
    float smoothness;                /* [0.0f .. 1.0f] */
    uint32_t max_influences;         /* [1 .. 4] */
    uint32_t flags;                  /* bit 0: apply_to_live_puppet, bit 1: respect_locked_vertices,
                                        bit 2: use_visibility_raycast, bit 3: use_intrinsic_delaunay */
    uint32_t locked_vert_word_count; /* 0 if bit 1 clear, else ceil(vertex_count / 64) <= 1024 */
} waywal_ipc_puppet_solve_weights_t;
/* Followed by uint64_t locked_vert_bs[locked_vert_word_count] when flags bit 1 is set */

typedef struct {
    uint32_t vertex_index;
    uint32_t blend_indices[4];
    float weight[4];
} waywal_ipc_vertex_weight_t;

typedef struct {
    int32_t status;
    uint32_t object_id;
    uint32_t entry_count;
    uint32_t reserved;
} waywal_ipc_puppet_weights_resp_t;
/* Followed by waywal_ipc_vertex_weight_t[entry_count] */

typedef struct {
    uint32_t object_id;
    uint32_t entry_count; /* [1 .. 65536] */
    uint32_t flags;       /* bit 0: normalize_row */
    uint32_t reserved;
} waywal_ipc_puppet_paint_weights_t;
/* Followed by waywal_ipc_vertex_weight_t[entry_count] */

typedef struct {
    uint32_t object_id;
    uint16_t enabled;         /* 0 = off, 1 = active heatmap overlay */
    uint16_t palette;         /* 0 = Turbo, 1 = Viridis, 2 = Inferno (we_heatmap_palette_t) */
    uint32_t active_bone_idx; /* < bone_count or 0xFFFFFFFFu */
    float opacity;            /* [0.0f .. 1.0f] */
} waywal_ipc_puppet_heatmap_mode_t;

typedef struct {
    uint32_t bone_index;
    float local_pos[3];
    float local_angle[3];
    float local_scale[3];
} waywal_ipc_bone_trs_t;

typedef struct {
    uint32_t object_id;
    uint16_t root_bone; /* 0xFFFFu if pure FK override */
    uint16_t mid_bone;  /* 0xFFFFu if 1-bone aim */
    uint16_t end_bone;  /* 0xFFFFu if pure FK override */
    uint16_t pole_flip; /* 0 or 1 */
    float target_world[3];
    uint32_t override_count; /* [0 .. 128] */
    uint32_t mode_flags;     /* bit 0: enable_ik, bit 1: clear_preview, bit 2: commit_pose */
} waywal_ipc_puppet_preview_ik_t;
/* Followed by waywal_ipc_bone_trs_t[override_count] */

typedef struct {
    int32_t status;
    uint32_t object_id;
    uint32_t bone_count;
    uint32_t reserved;
} waywal_ipc_puppet_ik_pose_resp_t;
/* Followed by waywal_ipc_bone_trs_t[bone_count] */

typedef struct {
    char name[128];
    char mode_str[32]; /* "loop", "mirror", or "single" */
    int32_t id;
    float fps;
    int32_t length;
    uint32_t track_count;
    float layer_blend;
    float layer_rate;
    uint32_t layer_visible;
    uint32_t layer_blend_mode; /* 0 = WE_ANIM_BLEND_OVERRIDE, 1 = WE_ANIM_BLEND_ADDITIVE */
} waywal_ipc_anim_clip_desc_t;

typedef struct {
    int32_t bone_index;
    uint32_t keyframe_count;
} waywal_ipc_anim_track_desc_t;

typedef struct {
    float pos[3];
    float angle[3];
    float scale[3];
} waywal_ipc_mdl_keyframe_t;

typedef struct {
    uint32_t object_id;
    uint32_t clip_count; /* [0 .. 64] */
    uint32_t flags;      /* bit 0: replace_all_clips, bit 1: sync_puppet_layers */
    uint32_t reserved;
} waywal_ipc_puppet_set_anim_tracks_t;

typedef struct {
    uint32_t object_id;
    float time_sec;
    uint32_t pause_playback; /* 0 = play, 1 = freeze at time_sec */
    uint32_t flags;          /* bit 0: reset_xpbd_physics, bit 1: step_physics_dt */
} waywal_ipc_puppet_scrub_t;

typedef struct {
    uint32_t object_id;
    uint32_t flags; /* bit 0: return_inline_blob, bit 1: write_to_export_path */
    char out_path[512];
} waywal_ipc_export_puppet_mdl_t;

typedef struct {
    int32_t status;
    uint32_t object_id;
    uint32_t mdl_byte_size;
    uint32_t reserved;
} waywal_ipc_export_puppet_mdl_resp_t;
#pragma pack(pop)

static_assert(sizeof(waywal_ipc_mdl_vertex_t) == 52,
              "waywal_ipc_mdl_vertex_t must be exactly 52 bytes");
static_assert(sizeof(waywal_ipc_puppet_set_mesh_t) == 16,
              "waywal_ipc_puppet_set_mesh_t must be exactly 16 bytes");
static_assert(sizeof(waywal_ipc_puppet_mesh_ack_t) == 16,
              "waywal_ipc_puppet_mesh_ack_t must be exactly 16 bytes");
static_assert(sizeof(waywal_ipc_mdl_bone_t) == 456,
              "waywal_ipc_mdl_bone_t must be exactly 456 bytes");
static_assert(sizeof(waywal_ipc_puppet_set_skeleton_t) == 16,
              "waywal_ipc_puppet_set_skeleton_t must be exactly 16 bytes");
static_assert(sizeof(waywal_ipc_puppet_solve_weights_t) == 24,
              "waywal_ipc_puppet_solve_weights_t must be exactly 24 bytes");
static_assert(sizeof(waywal_ipc_vertex_weight_t) == 36,
              "waywal_ipc_vertex_weight_t must be exactly 36 bytes");
static_assert(sizeof(waywal_ipc_puppet_weights_resp_t) == 16,
              "waywal_ipc_puppet_weights_resp_t must be exactly 16 bytes");
static_assert(sizeof(waywal_ipc_puppet_paint_weights_t) == 16,
              "waywal_ipc_puppet_paint_weights_t must be exactly 16 bytes");
static_assert(sizeof(waywal_ipc_puppet_heatmap_mode_t) == 16,
              "waywal_ipc_puppet_heatmap_mode_t must be exactly 16 bytes");
static_assert(sizeof(waywal_ipc_bone_trs_t) == 40,
              "waywal_ipc_bone_trs_t must be exactly 40 bytes");
static_assert(sizeof(waywal_ipc_puppet_preview_ik_t) == 32,
              "waywal_ipc_puppet_preview_ik_t must be exactly 32 bytes");
static_assert(sizeof(waywal_ipc_puppet_ik_pose_resp_t) == 16,
              "waywal_ipc_puppet_ik_pose_resp_t must be exactly 16 bytes");
static_assert(sizeof(waywal_ipc_anim_clip_desc_t) == 192,
              "waywal_ipc_anim_clip_desc_t must be exactly 192 bytes");
static_assert(sizeof(waywal_ipc_anim_track_desc_t) == 8,
              "waywal_ipc_anim_track_desc_t must be exactly 8 bytes");
static_assert(sizeof(waywal_ipc_mdl_keyframe_t) == 36,
              "waywal_ipc_mdl_keyframe_t must be exactly 36 bytes");
static_assert(sizeof(waywal_ipc_puppet_set_anim_tracks_t) == 16,
              "waywal_ipc_puppet_set_anim_tracks_t must be exactly 16 bytes");
static_assert(sizeof(waywal_ipc_puppet_scrub_t) == 16,
              "waywal_ipc_puppet_scrub_t must be exactly 16 bytes");
static_assert(sizeof(waywal_ipc_export_puppet_mdl_t) == 520,
              "waywal_ipc_export_puppet_mdl_t must be exactly 520 bytes");
static_assert(sizeof(waywal_ipc_export_puppet_mdl_resp_t) == 16,
              "waywal_ipc_export_puppet_mdl_resp_t must be exactly 16 bytes");

static inline bool waywal_ipc_rigging_calc_mesh_bytes(uint32_t vertex_count,
                                                      uint32_t triangle_count,
                                                      uint64_t *out_total_bytes)
{
    if (!out_total_bytes || vertex_count < 3u || vertex_count > WAYWAL_IPC_RIGGING_MAX_VERTICES ||
        triangle_count < 1u || triangle_count > WAYWAL_IPC_RIGGING_MAX_TRIANGLES) {
        return false;
    }
    uint64_t total = (uint64_t)sizeof(waywal_ipc_puppet_set_mesh_t) +
                     (uint64_t)vertex_count * (uint64_t)sizeof(waywal_ipc_mdl_vertex_t) +
                     (uint64_t)triangle_count * 6ull;
    if (total > (uint64_t)WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES) {
        return false;
    }
    *out_total_bytes = total;
    return true;
}

static inline bool waywal_ipc_rigging_expects_generic_ok(uint16_t opcode)
{
    switch (opcode) {
    case WAYWAL_REQ_PUPPET_SET_SKELETON:
    case WAYWAL_REQ_PUPPET_PAINT_WEIGHTS:
    case WAYWAL_REQ_PUPPET_SET_HEATMAP_MODE:
    case WAYWAL_REQ_PUPPET_SET_ANIM_TRACKS:
    case WAYWAL_REQ_PUPPET_SCRUB_TIME:
        return true;
    default:
        return false;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_IPC_PROTO_RIGGING_H */
