// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/render_engine.h"

#include "gpu_compute.h"
#include "simd/simd_blend.h"
#include "waywal/log.h"
#include "waywal/output_state.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#ifdef _OPENMP
#include <omp.h>
#endif

bool render_engine_init(render_engine_t *re, dmabuf_context_t *dmabuf_ctx)
{
    if (!re)
        return false;
    memset(re, 0, sizeof(*re));

    if (dmabuf_ctx && dmabuf_ctx->drm_fd >= 0 && dmabuf_ctx->gbm_dev != NULL) {
        re->gpu_ctx = gpu_compute_create(dmabuf_ctx);
        if (re->gpu_ctx) {
            re->has_gpu_compute = true;
            WAYWAL_LOG_INFO("Render Engine initialized with hardware GPU compute shader pipeline");
            return true;
        }
    }

    simd_blend_scanline_fn fn = simd_get_blend_fn();
    const char *simd_name = "Scalar";
#if !defined(WWAL_DISABLE_SIMD)
#if defined(__x86_64__) || defined(_M_X64)
    if (fn == blend_scanline_avx512)
        simd_name = "AVX-512 BW";
    else if (fn == blend_scanline_avx2)
        simd_name = "AVX2";
#elif defined(__aarch64__) || defined(_M_ARM64)
    if (fn == blend_scanline_neon)
        simd_name = "ARM NEON";
#endif
#else
    (void)fn;
#endif

    WAYWAL_LOG_INFO("Render Engine initialized with CPU SIMD fallback (%s)", simd_name);

#ifdef _OPENMP
    if (!getenv("OMP_NUM_THREADS")) {
        int max_procs = omp_get_num_procs();
        omp_set_num_threads(max_procs > 4 ? 4 : max_procs);
    }
#endif
    return true;
}

void render_engine_destroy(render_engine_t *re)
{
    if (!re)
        return;
    if (re->has_gpu_compute && re->gpu_ctx) {
        gpu_compute_destroy((gpu_compute_ctx_t *)re->gpu_ctx);
        re->gpu_ctx = NULL;
        re->has_gpu_compute = false;
    }
}

bool render_engine_load_custom_shader(render_engine_t *re, const char *shader_src)
{
    if (!re || !re->has_gpu_compute || !re->gpu_ctx || !shader_src)
        return false;
    return gpu_compute_load_custom_shader((gpu_compute_ctx_t *)re->gpu_ctx, shader_src);
}

void render_engine_release_bo(render_engine_t *re, dmabuf_bo_t *bo)
{
    if (!re || !re->has_gpu_compute || !re->gpu_ctx || !bo)
        return;
    gpu_compute_release_bo((gpu_compute_ctx_t *)re->gpu_ctx, bo);
}

bool render_engine_upload_static(render_engine_t *re, const uint8_t *src_rgba, uint32_t src_w,
                                 uint32_t src_h, dmabuf_bo_t *dst_bo, uint32_t scaling_mode,
                                 uint32_t clear_color)
{
    if (!re || !re->has_gpu_compute || !re->gpu_ctx || !dst_bo)
        return false;
    return gpu_compute_upload_static((gpu_compute_ctx_t *)re->gpu_ctx, src_rgba, src_w, src_h,
                                     dst_bo, (waywal_scaling_mode_t)scaling_mode, clear_color);
}

bool render_engine_copy_bo(render_engine_t *re, dmabuf_bo_t *dst_bo, dmabuf_bo_t *src_bo,
                           uint32_t scaling_mode, uint32_t clear_color, bool release_src_gl)
{
    if (!re || !dst_bo || !src_bo || dst_bo == src_bo)
        return false;
    if (re->has_gpu_compute && re->gpu_ctx) {
        return gpu_compute_scale_bo((gpu_compute_ctx_t *)re->gpu_ctx, src_bo, dst_bo,
                                    (waywal_scaling_mode_t)scaling_mode, clear_color,
                                    release_src_gl);
    }
    if (src_bo->gbm_bo && dst_bo->gbm_bo && src_bo->width == dst_bo->width &&
        src_bo->height == dst_bo->height) {
        uint32_t src_stride = 0, dst_stride = 0;
        void *map_src = NULL, *map_dst = NULL;
        void *src_ptr = gbm_bo_map(src_bo->gbm_bo, 0, 0, src_bo->width, src_bo->height,
                                   GBM_BO_TRANSFER_READ, &src_stride, &map_src);
        void *dst_ptr = gbm_bo_map(dst_bo->gbm_bo, 0, 0, dst_bo->width, dst_bo->height,
                                   GBM_BO_TRANSFER_WRITE, &dst_stride, &map_dst);
        size_t row_bytes = (size_t)dst_bo->width * 4u;
        if (src_ptr && src_ptr != MAP_FAILED && dst_ptr && dst_ptr != MAP_FAILED &&
            (size_t)src_stride >= row_bytes && (size_t)dst_stride >= row_bytes) {
            for (uint32_t y = 0; y < dst_bo->height; ++y) {
                memcpy((uint8_t *)dst_ptr + (size_t)y * dst_stride,
                       (const uint8_t *)src_ptr + (size_t)y * src_stride, row_bytes);
            }
            gbm_bo_unmap(dst_bo->gbm_bo, map_dst);
            gbm_bo_unmap(src_bo->gbm_bo, map_src);
            return true;
        }
        if (dst_ptr && dst_ptr != MAP_FAILED)
            gbm_bo_unmap(dst_bo->gbm_bo, map_dst);
        if (src_ptr && src_ptr != MAP_FAILED)
            gbm_bo_unmap(src_bo->gbm_bo, map_src);
    }
    return false;
}

void render_engine_make_current(render_engine_t *re)
{
    if (!re || !re->has_gpu_compute || !re->gpu_ctx)
        return;
    gpu_compute_make_current((gpu_compute_ctx_t *)re->gpu_ctx);
}

void render_engine_set_span_bounds(render_engine_t *re, int32_t off_x, int32_t off_y,
                                   int32_t total_w, int32_t total_h)
{
    if (!re || !re->has_gpu_compute || !re->gpu_ctx)
        return;
    gpu_compute_set_span_bounds((gpu_compute_ctx_t *)re->gpu_ctx, off_x, off_y, total_w, total_h);
}

void render_engine_bump_upload_gen(render_engine_t *re)
{
    if (!re || !re->has_gpu_compute || !re->gpu_ctx)
        return;
    gpu_compute_bump_upload_gen((gpu_compute_ctx_t *)re->gpu_ctx);
}

void render_engine_release_upload_cache(render_engine_t *re)
{
    if (!re || !re->has_gpu_compute || !re->gpu_ctx)
        return;
    gpu_compute_release_upload_cache((gpu_compute_ctx_t *)re->gpu_ctx);
}

bool render_engine_execute_transition(render_engine_t *re, dmabuf_bo_t *target_bo,
                                      dmabuf_bo_t *old_bo, dmabuf_bo_t *new_bo,
                                      const waywal_transition_params_t *params)
{
    if (!target_bo || !old_bo || !new_bo || !params) {
        return false;
    }

    if (re && re->has_gpu_compute && re->gpu_ctx) {
        if (gpu_compute_dispatch_transition((gpu_compute_ctx_t *)re->gpu_ctx, target_bo, old_bo,
                                            new_bo, params)) {
            return true;
        }
    }

    if (!target_bo->gbm_bo || !old_bo->gbm_bo || !new_bo->gbm_bo || target_bo->width == 0 ||
        target_bo->height == 0 || old_bo->width != target_bo->width ||
        old_bo->height != target_bo->height || new_bo->width != target_bo->width ||
        new_bo->height != target_bo->height) {
        return false;
    }

    /* Fallback path: CPU mapping and software SIMD kernel */
    uint32_t stride_target = 0, stride_old = 0, stride_new = 0;
    void *map_data_target = NULL, *map_data_old = NULL, *map_data_new = NULL;

    void *map_target = gbm_bo_map(target_bo->gbm_bo, 0, 0, target_bo->width, target_bo->height,
                                  GBM_BO_TRANSFER_WRITE, &stride_target, &map_data_target);
    void *map_old = gbm_bo_map(old_bo->gbm_bo, 0, 0, old_bo->width, old_bo->height,
                               GBM_BO_TRANSFER_READ, &stride_old, &map_data_old);
    void *map_new = gbm_bo_map(new_bo->gbm_bo, 0, 0, new_bo->width, new_bo->height,
                               GBM_BO_TRANSFER_READ, &stride_new, &map_data_new);

    bool ok = false;
    size_t row_bytes = (size_t)target_bo->width * sizeof(uint32_t);
    if (map_target && map_target != MAP_FAILED && map_old && map_old != MAP_FAILED && map_new &&
        map_new != MAP_FAILED && stride_target >= row_bytes && stride_old >= row_bytes &&
        stride_new >= row_bytes) {
        const uint32_t *src_old = (const uint32_t *)map_old;
        const uint32_t *src_new = (const uint32_t *)map_new;
        uint32_t *packed_old = NULL;
        uint32_t *packed_new = NULL;
        bool inputs_ready = true;

        if (stride_old != row_bytes) {
            packed_old = (uint32_t *)malloc(row_bytes * (size_t)target_bo->height);
            if (packed_old) {
                for (uint32_t y = 0; y < target_bo->height; ++y) {
                    memcpy((uint8_t *)packed_old + (size_t)y * row_bytes,
                           (const uint8_t *)map_old + (size_t)y * stride_old, row_bytes);
                }
                src_old = packed_old;
            } else {
                inputs_ready = false;
            }
        }
        if (inputs_ready && stride_new != row_bytes) {
            packed_new = (uint32_t *)malloc(row_bytes * (size_t)target_bo->height);
            if (packed_new) {
                for (uint32_t y = 0; y < target_bo->height; ++y) {
                    memcpy((uint8_t *)packed_new + (size_t)y * row_bytes,
                           (const uint8_t *)map_new + (size_t)y * stride_new, row_bytes);
                }
                src_new = packed_new;
            } else {
                inputs_ready = false;
            }
        }

        if (inputs_ready) {
            ok = render_engine_execute_cpu_transition((uint32_t *)map_target, src_old, src_new,
                                                      target_bo->width, target_bo->height,
                                                      stride_target, params);
        }

        free(packed_old);
        free(packed_new);
    }

    if (map_target && map_target != MAP_FAILED)
        gbm_bo_unmap(target_bo->gbm_bo, map_data_target);
    if (map_old && map_old != MAP_FAILED)
        gbm_bo_unmap(old_bo->gbm_bo, map_data_old);
    if (map_new && map_new != MAP_FAILED)
        gbm_bo_unmap(new_bo->gbm_bo, map_data_new);

    return ok;
}
