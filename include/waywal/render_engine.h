// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_RENDER_ENGINE_H
#define WAYWAL_RENDER_ENGINE_H

#include "waywal/bezier.h"
#include "waywal/dmabuf.h"
#include "waywal/types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WAYWAL_SYNC_SIMULTANEOUS = 0,
    WAYWAL_SYNC_STAGGERED = 1,
} waywal_sync_mode_t;

typedef struct {
    waywal_transition_type_t type;
    float progress;        /* Normalized 0.0f -> 1.0f */
    float angle_rad;       /* Wipe / Wave angle in radians */
    float wave_freq;       /* Wave frequency (default: 20.0f) */
    float wave_amp;        /* Wave amplitude (default: 0.05f) */
    float center_x;        /* Normalized center X: 0.0f -> 1.0f */
    float center_y;        /* Normalized center Y: 0.0f -> 1.0f */
    bezier_curve_t bezier; /* Bézier easing curve */
} waywal_transition_params_t;

typedef struct render_engine render_engine_t;

struct render_engine {
    union {
        bool has_gpu_compute;
        bool gpu_compute_active;
    };
    void *gpu_ctx; /* Internal EGL / GLES 3.1 compute context */
};

/* Initializes render engine with GPU compute capabilities */
bool render_engine_init(render_engine_t *re, dmabuf_context_t *dmabuf_ctx);

/* Destroys render engine and associated GPU compute resources */
void render_engine_destroy(render_engine_t *re);

/* Releases OpenGL texture and EGLImage associated with a dmabuf BO */
void render_engine_release_bo(render_engine_t *re, dmabuf_bo_t *bo);

/* Uploads and scales static RGBA8/BGRA8 pixels or solid clear color into a DMA-BUF BO via GPU */
bool render_engine_upload_static(render_engine_t *re, const uint8_t *src_rgba, uint32_t src_w,
                                 uint32_t src_h, dmabuf_bo_t *dst_bo, uint32_t scaling_mode,
                                 uint32_t clear_color);

void render_engine_set_span_bounds(render_engine_t *re, int32_t off_x, int32_t off_y,
                                   int32_t total_w, int32_t total_h);

/* Hot-compiles and loads an arbitrary GLSL compute shader for CUSTOM transition */
bool render_engine_load_custom_shader(render_engine_t *re, const char *shader_src);

/* Executes transition between old_bo and new_bo, outputting directly to target_bo */
bool render_engine_execute_transition(render_engine_t *re, dmabuf_bo_t *target_bo,
                                      dmabuf_bo_t *old_bo, dmabuf_bo_t *new_bo,
                                      const waywal_transition_params_t *params);

/* CPU software transition fallback operating on raw pixel memory */
bool render_engine_execute_cpu_transition(uint32_t *dst, const uint32_t *old_pixels,
                                          const uint32_t *new_pixels, uint32_t width,
                                          uint32_t height, uint32_t stride_bytes,
                                          const waywal_transition_params_t *params);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_RENDER_ENGINE_H */
