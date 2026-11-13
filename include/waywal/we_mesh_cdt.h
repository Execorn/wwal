// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WE_MESH_CDT_H
#define WAYWAL_WE_MESH_CDT_H

#include "waywal/we_puppet.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WE_CDT_MAX_IMAGE_DIM     8192u
#define WE_CDT_MAX_IMAGE_PIXELS  (64u * 1024u * 1024u)
#define WE_CDT_MAX_DILATION_PX   32.0f
#define WE_CDT_MIN_ANGLE_MIN_DEG 5.0f
#define WE_CDT_MIN_ANGLE_MAX_DEG 32.0f
#define WE_CDT_INTERNAL_VERT_CAP (WE_MDL_MAX_VERTICES + 3u)
#define WE_CDT_INTERNAL_TRI_CAP  (WE_MDL_MAX_TRIANGLES + 8u)

typedef struct {
    double x;
    double y;
} we_cdt_vec2_t;

typedef struct {
    we_cdt_vec2_t *points;
    uint32_t point_count;
    bool is_hole;
    double signed_area_px2;
} we_cdt_loop_t;

typedef struct {
    double px;
    double py;
    uint32_t flags; /* Bit 0: hard Steiner vertex constraint */
} we_cdt_pin_t;

typedef struct {
    float alpha_threshold;  /* Normalized [1/255 .. 1.0], default 0.5f */
    float dilation_px;      /* Separable alpha dilation [0.0 .. 32.0], default 2.0f */
    float simplify_eps_px;  /* Saalfeld TP-RDP max error [0.25 .. 64.0], default 1.5f */
    float interior_spacing; /* Bridson Poisson spacing [4.0 .. 512.0], default 24.0f */
    float min_angle_deg;    /* Chew/Ruppert target angle [5.0 .. 32.0], default 25.0f */
    float min_island_area;  /* Speckle discard area in px^2 [0.0 .. 4096.0], default 4.0f */
    uint32_t max_vertices;  /* Hard vertex budget <= WE_MDL_MAX_VERTICES, default 8192u */
} we_mesh_gen_cfg_t;

typedef struct {
    we_mdl_vertex_t *vertices; /* Compacted [0 .. vertex_count - 1], caller frees */
    uint32_t vertex_count;
    uint16_t *indices; /* Length = triangle_count * 3, CCW in pixel/clip space */
    uint32_t index_count;
    uint32_t triangle_count;
    float achieved_min_angle_deg;
    float achieved_mean_quality;
    bool hit_vertex_budget;
} we_mesh_gen_result_t;

void we_mesh_gen_cfg_default(we_mesh_gen_cfg_t *out_cfg);

bool we_mesh_extract_contours(const uint8_t *alpha, uint32_t width, uint32_t height,
                              uint32_t stride_bytes, const we_mesh_gen_cfg_t *cfg,
                              we_cdt_loop_t **out_loops, uint32_t *out_loop_count);

void we_mesh_loops_free(we_cdt_loop_t *loops, uint32_t loop_count);

bool we_mesh_gen_from_loops(const we_cdt_loop_t *loops, uint32_t loop_count, uint32_t tex_width,
                            uint32_t tex_height, const we_mesh_gen_cfg_t *cfg,
                            const we_cdt_pin_t *pins, uint32_t pin_count,
                            we_mesh_gen_result_t *out_result);

bool we_mesh_gen_from_alpha(const uint8_t *alpha, uint32_t width, uint32_t height,
                            uint32_t stride_bytes, const we_mesh_gen_cfg_t *cfg,
                            const we_cdt_pin_t *pins, uint32_t pin_count,
                            we_mesh_gen_result_t *out_result);

bool we_mesh_gen_from_rgba8(const uint8_t *rgba8, uint32_t width, uint32_t height,
                            uint32_t stride_bytes, const we_mesh_gen_cfg_t *cfg,
                            const we_cdt_pin_t *pins, uint32_t pin_count,
                            we_mesh_gen_result_t *out_result);

void we_mesh_gen_result_free(we_mesh_gen_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WE_MESH_CDT_H */
