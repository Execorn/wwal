#ifndef WAYWAL_GPU_COMPUTE_H
#define WAYWAL_GPU_COMPUTE_H

#include "waywal/dmabuf.h"
#include "waywal/render_engine.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gpu_compute_ctx gpu_compute_ctx_t;

/* Initializes EGL on GBM and compiles GLES 3.1 compute shaders */
gpu_compute_ctx_t *gpu_compute_create(dmabuf_context_t *dmabuf_ctx);

/* Destroys GPU compute context and deletes all GL/EGL resources */
void gpu_compute_destroy(gpu_compute_ctx_t *ctx);

/* Dispatches compute shader writing directly into target_bo */
bool gpu_compute_dispatch_transition(gpu_compute_ctx_t *ctx, dmabuf_bo_t *target_bo,
                                     dmabuf_bo_t *old_bo, dmabuf_bo_t *new_bo,
                                     const waywal_transition_params_t *params);

/* Releases cached OpenGL texture and EGLImageKHR associated with a BO */
void gpu_compute_release_bo(gpu_compute_ctx_t *ctx, dmabuf_bo_t *bo);

/* Prepares GPU textures and FBO for hardware-accelerated transition on an SHM output */
bool gpu_compute_prepare_shm(gpu_compute_ctx_t *ctx, uint32_t *tex_old, uint32_t *tex_new,
                             uint32_t *tex_target, uint32_t *fbo, uint32_t *cur_w, uint32_t *cur_h,
                             uint32_t width, uint32_t height, const uint32_t *old_pixels,
                             const uint32_t *new_pixels);

/* Dispatches compute shader to target texture and reads back result into dst_pixels */
bool gpu_compute_dispatch_transition_shm(gpu_compute_ctx_t *ctx, uint32_t tex_old,
                                         uint32_t tex_new, uint32_t tex_target,
                                         uint32_t fbo, uint32_t width, uint32_t height,
                                         const waywal_transition_params_t *params,
                                         uint32_t *dst_pixels);

/* Releases GL textures and FBO allocated for SHM output */
void gpu_compute_release_shm_res(gpu_compute_ctx_t *ctx, uint32_t *tex_old, uint32_t *tex_new,
                                 uint32_t *tex_target, uint32_t *fbo);

/* Hot-compiles and loads an arbitrary custom GLSL compute shader */
bool gpu_compute_load_custom_shader(gpu_compute_ctx_t *ctx, const char *src);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_GPU_COMPUTE_H */
