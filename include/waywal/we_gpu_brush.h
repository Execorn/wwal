// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WE_GPU_BRUSH_H
#define WAYWAL_WE_GPU_BRUSH_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WE_BRUSH_TILE_SIZE         64u
#define WE_BRUSH_SCRATCH_MAX_DIM   1024u
#define WE_BRUSH_KAWASE_LEVELS     4u
#define WE_BRUSH_POOL_DEFAULT_CAPS 4096u
#define WE_BRUSH_MAX_STROKE_DABS   256u
#define WE_BRUSH_FLOW_NEUTRAL_U8   128u

typedef enum : uint32_t {
    WE_BRUSH_CHAN_MASK_R8 = 0u,     /* Single-channel opacity / wind / water mask (GL_R8) */
    WE_BRUSH_CHAN_FLOW_RG88 = 1u,   /* Tangent velocity flow map, rest = (128, 128) (GL_RG8) */
    WE_BRUSH_CHAN_COLOR_RGBA8 = 2u, /* Full-color albedo / paint layer (GL_RGBA8) */
    WE_BRUSH_CHAN_BLUR = 3u,        /* Separable Gaussian / Dual-Kawase spatial blur */
    WE_BRUSH_CHAN_SMUDGE = 4u,      /* Drag-advection smudge + optional blur softness */
} we_brush_channel_t;

typedef enum : uint32_t {
    WE_BRUSH_OP_ADD_BUILDUP = 0u, /* dst + (target - dst) * w */
    WE_BRUSH_OP_MAX_CAP = 1u,     /* max(dst, target * w) */
    WE_BRUSH_OP_SUBTRACT = 2u,    /* clamp(dst - w, 0, 1) */
    WE_BRUSH_OP_ERASE = 3u,       /* dst * (1 - w) */
    WE_BRUSH_OP_FLOW_PAINT = 4u,  /* mix(dst_vec, brush_vec, w) */
    WE_BRUSH_OP_FLOW_ERASE = 5u,  /* mix(dst_vec, vec2(0), w) */
    WE_BRUSH_OP_FLOW_COMB = 6u,   /* clamp_len(dst_vec + brush_vec * w, 1.0) */
    WE_BRUSH_OP_BLUR_MASKED = 7u, /* mix(orig, kawase_or_gauss, w) */
    WE_BRUSH_OP_SMUDGE_ADV = 8u,  /* 4-tap back-traced advection along -drag */
} we_brush_op_mode_t;

typedef struct {
    uint32_t channel;            /* we_brush_channel_t */
    uint32_t op_mode;            /* we_brush_op_mode_t */
    float radius_px;             /* Base brush radius [1.0, 512.0] */
    float hardness;              /* Core hardness [0.0, 0.999] */
    float falloff_power;         /* Edge shaping exponent [0.25, 4.0] */
    float opacity;               /* Dab strength [0.0, 1.0] */
    float target_value;          /* Target scalar mask value [0.0, 1.0] */
    float flow_strength;         /* Tangent velocity scale [0.0, 1.0] */
    float blur_radius_px;        /* Dual-Kawase / Gaussian kernel radius [1.0, 64.0] */
    float pressure_size_weight;  /* Stylus pressure -> radius coupling [0.0, 1.0] */
    float pressure_alpha_weight; /* Stylus pressure -> opacity coupling [0.0, 1.0] */
    float velocity_damp_weight;  /* Pointer speed damping [0.0, 1.0] */
    float color_rgba[4];         /* Paint color for WE_BRUSH_CHAN_COLOR_RGBA8 */
} we_brush_params_t;
static_assert(sizeof(we_brush_params_t) == 64, "we_brush_params_t must be exactly 64 bytes");

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
    uint32_t flags; /* bit0: invert_flow_y */
} we_brush_dab_t;
static_assert(sizeof(we_brush_dab_t) == 48, "we_brush_dab_t must be exactly 48 bytes");

typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} we_brush_dirty_rect_t;
static_assert(sizeof(we_brush_dirty_rect_t) == 16,
              "we_brush_dirty_rect_t must be exactly 16 bytes");

typedef struct {
    uint16_t tx;
    uint16_t ty;
    uint32_t pool_slot; /* Index inside GL_TEXTURE_2D_ARRAY CoW pool */
} we_brush_tile_coord_t;
static_assert(sizeof(we_brush_tile_coord_t) == 8, "we_brush_tile_coord_t must be exactly 8 bytes");

typedef struct {
    uint32_t last_dab_gpu_us;
    uint32_t active_pool_slots;
    uint32_t max_pool_slots;
    uint32_t active_stroke_tiles;
    uint32_t canvas_width;
    uint32_t canvas_height;
    uint32_t target_gl_tex;
    uint32_t target_tex_format; /* we_tex_format_t */
} we_brush_stats_t;
static_assert(sizeof(we_brush_stats_t) == 32, "we_brush_stats_t must be exactly 32 bytes");

typedef struct we_gpu_brush_ctx we_gpu_brush_ctx_t;
struct we_scene;
struct we_tex;

/* Allocates and compiles the GLES 3.1 brush shaders + scratch FBOs (S_ping, S_pong, K_down/up)
 * and 64x64 CoW GL_TEXTURE_2D_ARRAY undo pool on the current EGL/GLES 3.1 context. */
we_gpu_brush_ctx_t *we_gpu_brush_create(uint32_t pool_slot_capacity);
void we_gpu_brush_destroy(we_gpu_brush_ctx_t *ctx);

/* Ensures pass->textures[slot]->gl_tex is color-renderable (decompresses DXT/BC7 or promotes
 * non-renderable R8/RG8 targets to RGBA8 while preserving GL_TEXTURE_SWIZZLE_* and clamping
 * GL_TEXTURE_MAX_LEVEL = 0 during active strokes). */
bool we_gpu_brush_bind_target(we_gpu_brush_ctx_t *ctx, struct we_scene *scene, int32_t object_id,
                              uint32_t pass_index, uint32_t tex_slot);

/* Unbinds bound_scene/bound_tex if matching scene (or unconditionally when scene == NULL),
 * cancels any active in-flight stroke, and evicts undo strokes scoped to that scene. */
void we_gpu_brush_unbind_scene(we_gpu_brush_ctx_t *ctx, const struct we_scene *scene);

/* Stroke transaction lifecycle (64x64 CoW dirty-tile capture on first tile touch). */
bool we_gpu_brush_begin_stroke(we_gpu_brush_ctx_t *ctx, uint64_t stroke_id,
                               const we_brush_params_t *params);
bool we_gpu_brush_dispatch_dabs(we_gpu_brush_ctx_t *ctx, const we_brush_dab_t *dabs,
                                uint32_t dab_count, we_brush_dirty_rect_t *out_dirty_rect);
bool we_gpu_brush_end_stroke(we_gpu_brush_ctx_t *ctx, uint32_t *out_captured_tile_count);

/* Symmetric O(K) VRAM tile undo/redo for a committed stroke_id. */
bool we_gpu_brush_undo_stroke(we_gpu_brush_ctx_t *ctx, uint64_t stroke_id);
bool we_gpu_brush_redo_stroke(we_gpu_brush_ctx_t *ctx, uint64_t stroke_id);

/* Reads modified VRAM texture level 0 back into CPU memory for deterministic TEXV0005 export. */
bool we_gpu_brush_readback_texels(we_gpu_brush_ctx_t *ctx, uint32_t expected_format,
                                  uint8_t **out_pixels, size_t *out_byte_len);

/* Queries live brush pool and latency telemetry. */
void we_gpu_brush_get_stats(const we_gpu_brush_ctx_t *ctx, we_brush_stats_t *out_stats);

/* Exact UNORM8 RG88 flow vector helpers: rest = (128, 128). */
static inline void we_brush_encode_flow_u8(float vx, float vy, uint8_t *out_r, uint8_t *out_g)
{
    const float k0 = 128.0f / 255.0f;
    const float ks = 127.0f / 255.0f;
    float cx = k0 + vx * ks;
    float cy = k0 + vy * ks;
    if (cx < 0.0f)
        cx = 0.0f;
    if (cx > 1.0f)
        cx = 1.0f;
    if (cy < 0.0f)
        cy = 0.0f;
    if (cy > 1.0f)
        cy = 1.0f;
    *out_r = (uint8_t)(cx * 255.0f + 0.5f);
    *out_g = (uint8_t)(cy * 255.0f + 0.5f);
}

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WE_GPU_BRUSH_H */
