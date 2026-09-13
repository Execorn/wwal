#include "waywal/render_engine.h"
#include "gpu_compute.h"
#include "simd/simd_blend.h"
#include "waywal/log.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

bool render_engine_init(render_engine_t *re, dmabuf_context_t *dmabuf_ctx) {
    if (!re) return false;
    memset(re, 0, sizeof(*re));

    if (dmabuf_ctx && dmabuf_ctx->available) {
        re->gpu_ctx = gpu_compute_create(dmabuf_ctx);
        if (re->gpu_ctx) {
            re->has_gpu_compute = true;
            WAYWAL_LOG_INFO("Render Engine initialized with hardware GPU compute shader pipeline");
            return true;
        }
    }

    simd_blend_scanline_fn fn = simd_get_blend_fn();
    const char *simd_name = "Scalar";
#if defined(__x86_64__) || defined(_M_X64)
    if (fn == blend_scanline_avx512) simd_name = "AVX-512 VBMI2";
    else if (fn == blend_scanline_avx2) simd_name = "AVX2";
#elif defined(__aarch64__) || defined(_M_ARM64)
    if (fn == blend_scanline_neon) simd_name = "ARM NEON";
#endif

    WAYWAL_LOG_INFO("Render Engine initialized with CPU SIMD fallback (%s)", simd_name);
    return true;
}

void render_engine_destroy(render_engine_t *re) {
    if (!re) return;
    if (re->has_gpu_compute && re->gpu_ctx) {
        gpu_compute_destroy((gpu_compute_ctx_t *)re->gpu_ctx);
        re->gpu_ctx = NULL;
        re->has_gpu_compute = false;
    }
}

static inline float clampf(float v, float min_v, float max_v) {
    if (v < min_v) return min_v;
    if (v > max_v) return max_v;
    return v;
}

static inline float smoothstepf(float edge0, float edge1, float x) {
    float t = clampf((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static inline float hash21(float x, float y) {
    float px = fmodf(x * 123.34f + y * 456.21f, 1.0f);
    if (px < 0.0f) px += 1.0f;
    float dot = px * (px + 45.32f);
    float val = fmodf(dot * 1337.5f, 1.0f);
    return val < 0.0f ? val + 1.0f : val;
}

static inline uint32_t blend_pixel_scalar(uint32_t a, uint32_t b, uint16_t wb) {
    uint32_t wa = 256 - wb;
    uint32_t rb = (((a & 0x00FF00FF) * wa) + ((b & 0x00FF00FF) * wb)) >> 8;
    uint32_t g  = (((a & 0x0000FF00) * wa) + ((b & 0x0000FF00) * wb)) >> 8;
    return (rb & 0x00FF00FF) | (g & 0x0000FF00) | 0xFF000000;
}

bool render_engine_execute_cpu_transition(
    uint32_t *dst,
    const uint32_t *old_pixels,
    const uint32_t *new_pixels,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes,
    const waywal_transition_params_t *params
) {
    if (!dst || !old_pixels || !new_pixels || width == 0 || height == 0 || !params) {
        return false;
    }

    simd_blend_scanline_fn blend_fn = simd_get_blend_fn();
    uint32_t stride_pixels = stride_bytes / sizeof(uint32_t);
    float progress = clampf(params->progress, 0.0f, 1.0f);

    switch (params->type) {
        case WAYWAL_TRANSITION_FADE:
        case WAYWAL_TRANSITION_SIMPLE: {
            uint16_t weight_b = (uint16_t)(progress * 256.0f);
            if (weight_b > 256) weight_b = 256;

            for (uint32_t y = 0; y < height; ++y) {
                uint8_t *dst_row = (uint8_t *)(dst + y * stride_pixels);
                const uint8_t *src_a_row = (const uint8_t *)(old_pixels + y * width);
                const uint8_t *src_b_row = (const uint8_t *)(new_pixels + y * width);

                blend_fn(dst_row, src_a_row, src_b_row, weight_b, width * sizeof(uint32_t));
            }
            return true;
        }

        case WAYWAL_TRANSITION_WIPE: {
            float cos_a = cosf(params->angle_rad);
            float sin_a = sinf(params->angle_rad);
            float edge = 0.04f;

            for (uint32_t y = 0; y < height; ++y) {
                float ny = (float)y / (float)height - 0.5f;
                uint32_t *dst_row = dst + y * stride_pixels;
                const uint32_t *a_row = old_pixels + y * width;
                const uint32_t *b_row = new_pixels + y * width;

                for (uint32_t x = 0; x < width; ++x) {
                    float nx = (float)x / (float)width - 0.5f;
                    float proj = (nx * cos_a + ny * sin_a) / 1.41421356f + 0.5f;
                    float alpha = smoothstepf(proj - edge, proj + edge, progress);
                    if (alpha <= 0.0f) {
                        dst_row[x] = a_row[x];
                    } else if (alpha >= 1.0f) {
                        dst_row[x] = b_row[x];
                    } else {
                        uint16_t wb = (uint16_t)(alpha * 256.0f);
                        dst_row[x] = blend_pixel_scalar(a_row[x], b_row[x], wb);
                    }
                }
            }
            return true;
        }

        case WAYWAL_TRANSITION_GROW: {
            float cx = params->center_x > 0.0f ? params->center_x : 0.5f;
            float cy = params->center_y > 0.0f ? params->center_y : 0.5f;
            float aspect = (float)width / (float)height;
            float max_x = cx > (1.0f - cx) ? cx : (1.0f - cx);
            float max_y = cy > (1.0f - cy) ? cy : (1.0f - cy);
            float max_dist = sqrtf((max_x * aspect) * (max_x * aspect) + max_y * max_y) + 0.05f;
            float r = progress * max_dist;
            float edge = 0.03f * max_dist;

            for (uint32_t y = 0; y < height; ++y) {
                float dy = (float)y / (float)height - cy;
                uint32_t *dst_row = dst + y * stride_pixels;
                const uint32_t *a_row = old_pixels + y * width;
                const uint32_t *b_row = new_pixels + y * width;

                for (uint32_t x = 0; x < width; ++x) {
                    float dx = ((float)x / (float)width - cx) * aspect;
                    float dist = sqrtf(dx * dx + dy * dy);
                    float alpha = smoothstepf(dist - edge, dist + edge, r);
                    if (alpha <= 0.0f) {
                        dst_row[x] = a_row[x];
                    } else if (alpha >= 1.0f) {
                        dst_row[x] = b_row[x];
                    } else {
                        uint16_t wb = (uint16_t)(alpha * 256.0f);
                        dst_row[x] = blend_pixel_scalar(a_row[x], b_row[x], wb);
                    }
                }
            }
            return true;
        }

        case WAYWAL_TRANSITION_OUTER: {
            float cx = params->center_x > 0.0f ? params->center_x : 0.5f;
            float cy = params->center_y > 0.0f ? params->center_y : 0.5f;
            float aspect = (float)width / (float)height;
            float max_x = cx > (1.0f - cx) ? cx : (1.0f - cx);
            float max_y = cy > (1.0f - cy) ? cy : (1.0f - cy);
            float max_dist = sqrtf((max_x * aspect) * (max_x * aspect) + max_y * max_y) + 0.05f;
            float r = (1.0f - progress) * max_dist;
            float edge = 0.03f * max_dist;

            for (uint32_t y = 0; y < height; ++y) {
                float dy = (float)y / (float)height - cy;
                uint32_t *dst_row = dst + y * stride_pixels;
                const uint32_t *a_row = old_pixels + y * width;
                const uint32_t *b_row = new_pixels + y * width;

                for (uint32_t x = 0; x < width; ++x) {
                    float dx = ((float)x / (float)width - cx) * aspect;
                    float dist = sqrtf(dx * dx + dy * dy);
                    float alpha = smoothstepf(r - edge, r + edge, dist);
                    if (alpha <= 0.0f) {
                        dst_row[x] = a_row[x];
                    } else if (alpha >= 1.0f) {
                        dst_row[x] = b_row[x];
                    } else {
                        uint16_t wb = (uint16_t)(alpha * 256.0f);
                        dst_row[x] = blend_pixel_scalar(a_row[x], b_row[x], wb);
                    }
                }
            }
            return true;
        }

        case WAYWAL_TRANSITION_WAVE: {
            float cos_a = cosf(params->angle_rad);
            float sin_a = sinf(params->angle_rad);
            float freq = params->wave_freq > 0.0f ? params->wave_freq : 20.0f;
            float amp = params->wave_amp > 0.0f ? params->wave_amp : 0.05f;
            float edge = 0.04f;

            for (uint32_t y = 0; y < height; ++y) {
                float ny = (float)y / (float)height - 0.5f;
                uint32_t *dst_row = dst + y * stride_pixels;
                const uint32_t *a_row = old_pixels + y * width;
                const uint32_t *b_row = new_pixels + y * width;

                for (uint32_t x = 0; x < width; ++x) {
                    float nx = (float)x / (float)width - 0.5f;
                    float proj = (nx * cos_a + ny * sin_a) / 1.41421356f + 0.5f;
                    float cross = -nx * sin_a + ny * cos_a;
                    float wave = sinf(cross * freq) * amp;
                    float d = proj + wave;
                    float alpha = smoothstepf(d - edge, d + edge, progress);
                    if (alpha <= 0.0f) {
                        dst_row[x] = a_row[x];
                    } else if (alpha >= 1.0f) {
                        dst_row[x] = b_row[x];
                    } else {
                        uint16_t wb = (uint16_t)(alpha * 256.0f);
                        dst_row[x] = blend_pixel_scalar(a_row[x], b_row[x], wb);
                    }
                }
            }
            return true;
        }

        case WAYWAL_TRANSITION_NOISE: {
            float edge = 0.05f;
            for (uint32_t y = 0; y < height; ++y) {
                uint32_t *dst_row = dst + y * stride_pixels;
                const uint32_t *a_row = old_pixels + y * width;
                const uint32_t *b_row = new_pixels + y * width;

                for (uint32_t x = 0; x < width; ++x) {
                    float n = hash21((float)x * 0.05f, (float)y * 0.05f);
                    float alpha = smoothstepf(n - edge, n + edge, progress);
                    if (alpha <= 0.0f) {
                        dst_row[x] = a_row[x];
                    } else if (alpha >= 1.0f) {
                        dst_row[x] = b_row[x];
                    } else {
                        uint16_t wb = (uint16_t)(alpha * 256.0f);
                        dst_row[x] = blend_pixel_scalar(a_row[x], b_row[x], wb);
                    }
                }
            }
            return true;
        }

        default:
            break;
    }

    return false;
}

bool render_engine_execute_transition(
    render_engine_t *re,
    dmabuf_bo_t *target_bo,
    dmabuf_bo_t *old_bo,
    dmabuf_bo_t *new_bo,
    const waywal_transition_params_t *params
) {
    if (!target_bo || !old_bo || !new_bo || !params) {
        return false;
    }

    if (re && re->has_gpu_compute && re->gpu_ctx) {
        if (gpu_compute_dispatch_transition((gpu_compute_ctx_t *)re->gpu_ctx,
                                            target_bo, old_bo, new_bo, params)) {
            return true;
        }
    }

    /* Fallback path: CPU mapping and software SIMD kernel */
    uint32_t stride_target = 0, stride_old = 0, stride_new = 0;
    void *map_data_target = NULL, *map_data_old = NULL, *map_data_new = NULL;

    void *map_target = gbm_bo_map(target_bo->gbm_bo, 0, 0,
                                  target_bo->width, target_bo->height,
                                  GBM_BO_TRANSFER_WRITE, &stride_target, &map_data_target);
    void *map_old = gbm_bo_map(old_bo->gbm_bo, 0, 0,
                               old_bo->width, old_bo->height,
                               GBM_BO_TRANSFER_READ, &stride_old, &map_data_old);
    void *map_new = gbm_bo_map(new_bo->gbm_bo, 0, 0,
                               new_bo->width, new_bo->height,
                               GBM_BO_TRANSFER_READ, &stride_new, &map_data_new);

    bool ok = false;
    if (map_target && map_old && map_new) {
        ok = render_engine_execute_cpu_transition(
            (uint32_t *)map_target,
            (const uint32_t *)map_old,
            (const uint32_t *)map_new,
            target_bo->width, target_bo->height,
            stride_target, params
        );
    }

    if (map_target) gbm_bo_unmap(target_bo->gbm_bo, map_data_target);
    if (map_old) gbm_bo_unmap(old_bo->gbm_bo, map_data_old);
    if (map_new) gbm_bo_unmap(new_bo->gbm_bo, map_data_new);

    return ok;
}
