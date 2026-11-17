// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/render_engine.h"

#include "gpu_compute.h"
#include "simd/simd_blend.h"
#include "waywal/log.h"
#include "waywal/output_state.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#ifdef _OPENMP
#include <omp.h>
#endif

static bool bo_is_cpu_mappable(const dmabuf_bo_t *bo)
{
    if (!bo || !bo->gbm_bo || bo->width == 0u || bo->height == 0u || bo->is_10bit) {
        return false;
    }
    if (bo->drm_format == DRM_FORMAT_XRGB2101010 || bo->drm_format == DRM_FORMAT_ARGB2101010 ||
        bo->drm_format == DRM_FORMAT_XBGR2101010 || bo->drm_format == DRM_FORMAT_ABGR2101010) {
        return false;
    }
    /* Tiled or compressed vendor modifiers cannot be interpreted safely as row-major 32-bit rows.
     */
    return bo->modifier == DRM_FORMAT_MOD_LINEAR || bo->modifier == DRM_FORMAT_MOD_INVALID;
}

static uint32_t sample_bilinear_u32(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                                    uint32_t src_stride, double sx, double sy)
{
    if (sx < 0.0) {
        sx = 0.0;
    } else if (sx > (double)(src_w - 1u)) {
        sx = (double)(src_w - 1u);
    }
    if (sy < 0.0) {
        sy = 0.0;
    } else if (sy > (double)(src_h - 1u)) {
        sy = (double)(src_h - 1u);
    }

    uint32_t x0 = (uint32_t)sx;
    uint32_t y0 = (uint32_t)sy;
    uint32_t x1 = (x0 + 1u < src_w) ? (x0 + 1u) : x0;
    uint32_t y1 = (y0 + 1u < src_h) ? (y0 + 1u) : y0;

    uint32_t wx = (uint32_t)((sx - (double)x0) * 256.0 + 0.5);
    uint32_t wy = (uint32_t)((sy - (double)y0) * 256.0 + 0.5);
    uint32_t iwx = 256u - wx;
    uint32_t iwy = 256u - wy;

    const uint32_t *row0 = (const uint32_t *)(src + (size_t)y0 * src_stride);
    const uint32_t *row1 = (const uint32_t *)(src + (size_t)y1 * src_stride);
    uint32_t p00 = row0[x0];
    uint32_t p10 = row0[x1];
    uint32_t p01 = row1[x0];
    uint32_t p11 = row1[x1];

    uint32_t out = 0u;
    for (uint32_t shift = 0u; shift < 32u; shift += 8u) {
        uint32_t c00 = (p00 >> shift) & 0xFFu;
        uint32_t c10 = (p10 >> shift) & 0xFFu;
        uint32_t c01 = (p01 >> shift) & 0xFFu;
        uint32_t c11 = (p11 >> shift) & 0xFFu;
        uint32_t top = c00 * iwx + c10 * wx;
        uint32_t bot = c01 * iwx + c11 * wx;
        uint32_t val = (top * iwy + bot * wy + 32768u) >> 16u;
        if (val > 255u) {
            val = 255u;
        }
        out |= (val << shift);
    }
    return out;
}

static void copy_bo_cpu_scaled(uint8_t *dst_ptr, uint32_t dst_w, uint32_t dst_h,
                               uint32_t dst_stride, const uint8_t *src_ptr, uint32_t src_w,
                               uint32_t src_h, uint32_t src_stride, uint32_t scaling_mode,
                               uint32_t clear_color)
{
    size_t row_bytes = (size_t)dst_w * sizeof(uint32_t);
    if (src_w == dst_w && src_h == dst_h && scaling_mode != WAYWAL_SCALING_SPAN) {
        if (src_stride == row_bytes && dst_stride == row_bytes) {
            memcpy(dst_ptr, src_ptr, row_bytes * (size_t)dst_h);
        } else {
#ifdef _OPENMP
#pragma omp parallel for schedule(static) if (dst_h >= 64)
#endif
            for (uint32_t y = 0; y < dst_h; ++y) {
                memcpy(dst_ptr + (size_t)y * dst_stride, src_ptr + (size_t)y * src_stride,
                       row_bytes);
            }
        }
        return;
    }

    if (scaling_mode == WAYWAL_SCALING_CENTER) {
        int64_t off_x = ((int64_t)dst_w - (int64_t)src_w) / 2;
        int64_t off_y = ((int64_t)dst_h - (int64_t)src_h) / 2;
#ifdef _OPENMP
#pragma omp parallel for schedule(static) if (dst_h >= 64)
#endif
        for (uint32_t dy = 0; dy < dst_h; ++dy) {
            uint32_t *dst_row = (uint32_t *)(dst_ptr + (size_t)dy * dst_stride);
            int64_t sy = (int64_t)dy - off_y;
            if (sy < 0 || sy >= (int64_t)src_h) {
                for (uint32_t dx = 0; dx < dst_w; ++dx) {
                    dst_row[dx] = clear_color;
                }
                continue;
            }
            const uint32_t *src_row = (const uint32_t *)(src_ptr + (size_t)sy * src_stride);
            for (uint32_t dx = 0; dx < dst_w; ++dx) {
                int64_t sx = (int64_t)dx - off_x;
                dst_row[dx] = (sx >= 0 && sx < (int64_t)src_w) ? src_row[sx] : clear_color;
            }
        }
        return;
    }

    if (scaling_mode == WAYWAL_SCALING_TILE) {
#ifdef _OPENMP
#pragma omp parallel for schedule(static) if (dst_h >= 64)
#endif
        for (uint32_t dy = 0; dy < dst_h; ++dy) {
            uint32_t *dst_row = (uint32_t *)(dst_ptr + (size_t)dy * dst_stride);
            const uint32_t *src_row =
                (const uint32_t *)(src_ptr + (size_t)(dy % src_h) * src_stride);
            for (uint32_t dx = 0; dx < dst_w; ++dx) {
                dst_row[dx] = src_row[dx % src_w];
            }
        }
        return;
    }

    double scale_x = (double)dst_w / (double)src_w;
    double scale_y = (double)dst_h / (double)src_h;
    if (scaling_mode == WAYWAL_SCALING_FIT) {
        double s = scale_x < scale_y ? scale_x : scale_y;
        scale_x = s;
        scale_y = s;
    } else if (scaling_mode != WAYWAL_SCALING_STRETCH) {
        double s = scale_x > scale_y ? scale_x : scale_y;
        scale_x = s;
        scale_y = s;
    }

    double disp_w = (double)src_w * scale_x;
    double disp_h = (double)src_h * scale_y;
    double off_x = ((double)dst_w - disp_w) * 0.5;
    double off_y = ((double)dst_h - disp_h) * 0.5;
    bool clip_bounds = (scaling_mode == WAYWAL_SCALING_FIT);

#ifdef _OPENMP
#pragma omp parallel for schedule(static) if (dst_h >= 64)
#endif
    for (uint32_t dy = 0; dy < dst_h; ++dy) {
        uint32_t *dst_row = (uint32_t *)(dst_ptr + (size_t)dy * dst_stride);
        double vy = ((double)dy + 0.5 - off_y) / scale_y;
        if (clip_bounds && (vy < 0.0 || vy >= (double)src_h)) {
            for (uint32_t dx = 0; dx < dst_w; ++dx) {
                dst_row[dx] = clear_color;
            }
            continue;
        }
        double sy = vy - 0.5;
        for (uint32_t dx = 0; dx < dst_w; ++dx) {
            double ux = ((double)dx + 0.5 - off_x) / scale_x;
            if (clip_bounds && (ux < 0.0 || ux >= (double)src_w)) {
                dst_row[dx] = clear_color;
                continue;
            }
            dst_row[dx] = sample_bilinear_u32(src_ptr, src_w, src_h, src_stride, ux - 0.5, sy);
        }
    }
}

bool render_engine_init(render_engine_t *re, dmabuf_context_t *dmabuf_ctx)
{
    if (!re)
        return false;
    memset(re, 0, sizeof(*re));

#ifdef _OPENMP
    /* Apply OpenMP thread cap before any early GPU return so CPU fallback stays bounded on SMT
     * hosts. */
    const char *omp_env = getenv("OMP_NUM_THREADS");
    if (!omp_env || omp_env[0] == '\0') {
        int max_procs = omp_get_num_procs();
        int phys = (max_procs >= 8) ? (max_procs / 2) : max_procs;
        if (phys > 8) {
            phys = 8;
        }
        if (phys < 1) {
            phys = 1;
        }
        omp_set_num_threads(phys);
    }
#endif

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

bool render_engine_upload_static_async(render_engine_t *re, const uint8_t *src_rgba, uint32_t src_w,
                                       uint32_t src_h, dmabuf_bo_t *dst_bo, uint32_t scaling_mode,
                                       uint32_t clear_color, int *out_fence_fd)
{
    if (out_fence_fd) {
        *out_fence_fd = -1;
    }
    if (!re || !re->has_gpu_compute || !re->gpu_ctx || !dst_bo)
        return false;
    return gpu_compute_upload_static_async((gpu_compute_ctx_t *)re->gpu_ctx, src_rgba, src_w, src_h,
                                           dst_bo, (waywal_scaling_mode_t)scaling_mode, clear_color,
                                           out_fence_fd);
}

bool render_engine_has_native_fence(const render_engine_t *re)
{
    if (!re || !re->has_gpu_compute || !re->gpu_ctx) {
        return false;
    }
    return gpu_compute_has_native_fence((const gpu_compute_ctx_t *)re->gpu_ctx);
}

int render_engine_export_fence_fd(render_engine_t *re)
{
    if (!re || !re->has_gpu_compute || !re->gpu_ctx) {
        return -1;
    }
    return gpu_compute_export_fence_fd((gpu_compute_ctx_t *)re->gpu_ctx);
}

bool render_engine_wait_idle(render_engine_t *re)
{
    if (!re || !re->has_gpu_compute || !re->gpu_ctx) {
        return true;
    }
    return gpu_compute_wait_idle((gpu_compute_ctx_t *)re->gpu_ctx);
}

bool render_engine_copy_bo(render_engine_t *re, dmabuf_bo_t *dst_bo, dmabuf_bo_t *src_bo,
                           uint32_t scaling_mode, uint32_t clear_color, bool release_src_gl)
{
    return render_engine_copy_bo_async(re, dst_bo, src_bo, scaling_mode, clear_color,
                                       release_src_gl, NULL);
}

bool render_engine_copy_bo_async(render_engine_t *re, dmabuf_bo_t *dst_bo, dmabuf_bo_t *src_bo,
                                 uint32_t scaling_mode, uint32_t clear_color, bool release_src_gl,
                                 int *out_fence_fd)
{
    if (out_fence_fd) {
        *out_fence_fd = -1;
    }
    if (!re || !dst_bo || !src_bo || dst_bo == src_bo)
        return false;
    if (re->has_gpu_compute && re->gpu_ctx) {
        if (out_fence_fd) {
            return gpu_compute_scale_bo_async((gpu_compute_ctx_t *)re->gpu_ctx, src_bo, dst_bo,
                                              (waywal_scaling_mode_t)scaling_mode, clear_color,
                                              release_src_gl, out_fence_fd);
        }
        return gpu_compute_scale_bo((gpu_compute_ctx_t *)re->gpu_ctx, src_bo, dst_bo,
                                    (waywal_scaling_mode_t)scaling_mode, clear_color,
                                    release_src_gl);
    }
    if (bo_is_cpu_mappable(src_bo) && bo_is_cpu_mappable(dst_bo)) {
        uint32_t src_stride = 0, dst_stride = 0;
        void *map_src = NULL, *map_dst = NULL;
        void *src_ptr = gbm_bo_map(src_bo->gbm_bo, 0, 0, src_bo->width, src_bo->height,
                                   GBM_BO_TRANSFER_READ, &src_stride, &map_src);
        void *dst_ptr = gbm_bo_map(dst_bo->gbm_bo, 0, 0, dst_bo->width, dst_bo->height,
                                   GBM_BO_TRANSFER_WRITE, &dst_stride, &map_dst);
        size_t src_row_bytes = (size_t)src_bo->width * sizeof(uint32_t);
        size_t dst_row_bytes = (size_t)dst_bo->width * sizeof(uint32_t);
        if (src_ptr && src_ptr != MAP_FAILED && dst_ptr && dst_ptr != MAP_FAILED &&
            (size_t)src_stride >= src_row_bytes && (size_t)dst_stride >= dst_row_bytes) {
            copy_bo_cpu_scaled((uint8_t *)dst_ptr, dst_bo->width, dst_bo->height, dst_stride,
                               (const uint8_t *)src_ptr, src_bo->width, src_bo->height, src_stride,
                               scaling_mode, clear_color);
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

void render_engine_set_crop_gravity(render_engine_t *re, float gx, float gy)
{
    if (!re || !re->has_gpu_compute || !re->gpu_ctx)
        return;
    gpu_compute_set_crop_gravity((gpu_compute_ctx_t *)re->gpu_ctx, gx, gy);
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
    return render_engine_execute_transition_async(re, target_bo, old_bo, new_bo, params, NULL);
}

bool render_engine_execute_transition_async(render_engine_t *re, dmabuf_bo_t *target_bo,
                                            dmabuf_bo_t *old_bo, dmabuf_bo_t *new_bo,
                                            const waywal_transition_params_t *params,
                                            int *out_fence_fd)
{
    if (out_fence_fd) {
        *out_fence_fd = -1;
    }
    if (!target_bo || !old_bo || !new_bo || !params) {
        return false;
    }

    if (re && re->has_gpu_compute && re->gpu_ctx) {
        bool ok =
            out_fence_fd
                ? gpu_compute_dispatch_transition_async((gpu_compute_ctx_t *)re->gpu_ctx, target_bo,
                                                        old_bo, new_bo, params, out_fence_fd)
                : gpu_compute_dispatch_transition((gpu_compute_ctx_t *)re->gpu_ctx, target_bo,
                                                  old_bo, new_bo, params);
        if (ok) {
            return true;
        }
    }

    if (!bo_is_cpu_mappable(target_bo) || !bo_is_cpu_mappable(old_bo) ||
        !bo_is_cpu_mappable(new_bo) || old_bo->width != target_bo->width ||
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
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
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
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
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
