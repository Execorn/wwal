#ifndef WAYWAL_RENDER_ENGINE_H
#define WAYWAL_RENDER_ENGINE_H

#include "waywal/bezier.h"
#include "waywal/dmabuf.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WAYWAL_TRANSITION_NONE = 0,
    WAYWAL_TRANSITION_SIMPLE = 1,
    WAYWAL_TRANSITION_FADE = 2,
    WAYWAL_TRANSITION_WIPE = 3,
    WAYWAL_TRANSITION_GROW = 4,
    WAYWAL_TRANSITION_OUTER = 5,
    WAYWAL_TRANSITION_WAVE = 6,
    WAYWAL_TRANSITION_NOISE = 7,
    WAYWAL_TRANSITION_CROSSZOOM = 8,
    WAYWAL_TRANSITION_SLIDE = 9,
    WAYWAL_TRANSITION_GLITCH = 10,
    WAYWAL_TRANSITION_BURN = 11,
    WAYWAL_TRANSITION_RIPPLE = 12,
    WAYWAL_TRANSITION_PIXELATE = 13,
    WAYWAL_TRANSITION_DOOM = 14,
    WAYWAL_TRANSITION_SWIRL = 15,
    WAYWAL_TRANSITION_CUBE = 16,
    WAYWAL_TRANSITION_LUMA = 17,
    WAYWAL_TRANSITION_LIGHT_LEAK = 18,
    WAYWAL_TRANSITION_PAGE_CURL = 19,
} waywal_transition_type_t;

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
    bool has_gpu_compute;
    void *gpu_ctx; /* Internal EGL / GLES 3.1 compute context */
};

/* Initializes render engine with GPU compute capabilities */
bool render_engine_init(render_engine_t *re, dmabuf_context_t *dmabuf_ctx);

/* Destroys render engine and associated GPU compute resources */
void render_engine_destroy(render_engine_t *re);

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
