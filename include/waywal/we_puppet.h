// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WE_PUPPET_H
#define WAYWAL_WE_PUPPET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WE_MDL_MAX_VERTICES  65536u
#define WE_MDL_MAX_TRIANGLES 131072u
#define WE_MDL_MAX_BONES     128u
#define WE_MDL_MAX_ANIMS     64u
#define WE_MDL_MAX_LAYERS    16u

typedef struct {
    float pos[3];
    uint32_t blend_indices[4];
    float weight[4];
    float texcoord[2];
} we_mdl_vertex_t;

typedef struct {
    char name[128];
    int32_t unk;
    uint32_t parent; /* 0xFFFFFFFFu = root */
    float local_trans[16];
    float world_bind[16];
    float offset_trans[16]; /* Inverse bind matrix: mat4_inverse(world_bind) */
    float bind_pos[3];
    float bind_angle[3];
    float bind_scale[3];
    char simulation_json[256];
} we_mdl_bone_t;

typedef struct {
    float pos[3];
    float angle[3];
    float scale[3];
} we_mdl_keyframe_t;

typedef struct {
    int32_t bone_index;
    int32_t unk;
    uint32_t kf_count;
    we_mdl_keyframe_t *keyframes;
} we_mdl_bone_track_t;

typedef enum {
    WE_ANIM_LOOP = 0,
    WE_ANIM_MIRROR = 1,
    WE_ANIM_SINGLE = 2,
} we_mdl_anim_mode_t;

typedef struct {
    int32_t id;
    int32_t unk;
    char name[128];
    char mode_str[32];
    we_mdl_anim_mode_t mode;
    float fps;
    int32_t length;
    int32_t unk2;
    uint32_t track_count;
    we_mdl_bone_track_t *tracks;
} we_mdl_anim_t;

typedef struct {
    int32_t anim_id;
    char anim_name[128];
    float rate;
    float blend;
    bool visible;
} we_puppet_layer_t;

typedef struct {
    float pos[3];
    float uv[2];
} we_skinned_vertex_t;

typedef struct we_puppet {
    uint32_t mdlv_version;
    uint32_t mdls_version;
    uint32_t mdla_version;
    uint32_t mdat_version;

    int32_t mdl_flag;
    int32_t unk_1;
    int32_t unk_2;
    char mat_json_file[256];
    bool alt_mdl_format; /* true if 80-byte vertex stride (0x0180000F) */

    uint32_t vertex_count;
    we_mdl_vertex_t *vertices;

    uint32_t index_count;
    uint32_t triangle_count;
    uint16_t *indices;

    uint32_t bone_count;
    we_mdl_bone_t *bones;

    uint32_t anim_count;
    we_mdl_anim_t *anims;

    we_puppet_layer_t layers[WE_MDL_MAX_LAYERS];
    uint32_t layer_count;

    /* Scratch buffers for runtime two-pass skinning */
    float (*skin_mats)[16];
    we_skinned_vertex_t *skinned_verts;

    /* Bind-pose bounding box */
    float min_pos[3];
    float max_pos[3];

    /* OpenGL ES 3.1 resources */
    uint32_t gl_vao;
    uint32_t gl_vbo_dyn;  /* Dynamic pos[3] + uv[2] */
    uint32_t gl_vbo_skin; /* Static blend_indices[4] + weight[4] */
    uint32_t gl_ebo;
    bool gl_uploaded;
    bool vbo_has_bind_pose;
} we_puppet_t;

/* Dual-mode magic reader: supports 9-byte NUL-terminated ("MDLV0013\0".."MDLV0023\0",
 * "MDLS0001\0".."MDLS0002\0", "MDAT0001\0", "MDLA0001\0".."MDLA0003\0"), 8-byte unpadded,
 * and [u32 len=8]["MDL..."] length-prefixed section headers with lookahead disambiguation. */
bool we_mdl_read_magic(const uint8_t *buf, size_t size, size_t *inout_cursor, char out_magic[16]);

/* Column-major 4x4 matrix multiplication and analytical inversion */
void we_mat4_mul(const float a[16], const float b[16], float out[16]);
bool we_mat4_inverse(const float m[16], float out_inv[16]);

/* Parses a .mdl container (MDLV0013..23, optional MDAT, MDLS0001..02, MDLA0001..03) on CPU */
we_puppet_t *we_puppet_parse_cpu(const uint8_t *data, size_t size);

/* Evaluates multi-layer keyframe animations at time_sec using the Two-Pass Skinning Invariant:
 * Pass 1: world_mats[k] = (parent == 0xFFFFFFFFu) ? anim_local[k] : world_mats[parent] *
 * anim_local[k] Pass 2: out_skin_mats[k] = world_mats[k] * bones[k].offset_trans */
void we_puppet_eval_skin_matrices(const we_puppet_t *puppet, const we_puppet_layer_t *layers,
                                  uint32_t layer_count, double time_sec,
                                  float (*out_skin_mats)[16]);

/* Applies skinning matrices to bind-pose vertices on the CPU */
void we_puppet_skin_vertices_cpu(const we_puppet_t *puppet, const float (*skin_mats)[16],
                                 we_skinned_vertex_t *out_verts);

/* Uploads VAO/VBO/EBO for OpenGL ES 3.1 rendering */
bool we_puppet_upload_gl(we_puppet_t *puppet);

/* Evaluates animations at time_sec, updates VBO / g_Bones uniform, and draws the indexed mesh */
void we_puppet_draw_gl(we_puppet_t *puppet, uint32_t gl_prog, double time_sec);

/* Frees all CPU and GPU resources owned by a we_puppet_t */
void we_puppet_destroy(we_puppet_t *puppet);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WE_PUPPET_H */
