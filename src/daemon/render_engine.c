#include "waywal/render_engine.h"

#include "gpu_compute.h"
#include "simd/simd_blend.h"
#include "waywal/log.h"
#include "waywal/output_state.h"

#ifdef _OPENMP
#include <omp.h>
#endif
#include <math.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

bool render_engine_init(render_engine_t *re, dmabuf_context_t *dmabuf_ctx)
{
    if (!re)
        return false;
    memset(re, 0, sizeof(*re));

    /* Only initialize the EGL/GLES compute shader pipeline when the compositor's preferred
     * modifier is linear (matching ensure_dmabuf_ring); otherwise transitions use the
     * OpenMP/AVX2 CPU SIMD path and we avoid allocating ~67 MiB of unused GPU shader compiler heap. */
    if (dmabuf_ctx && dmabuf_ctx->available &&
        (dmabuf_ctx->map_supported || !dmabuf_ctx->dmabuf_proto) &&
        (dmabuf_ctx->preferred_modifier == DRM_FORMAT_MOD_LINEAR ||
         dmabuf_ctx->preferred_modifier == DRM_FORMAT_MOD_INVALID)) {
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
    if (fn == blend_scanline_avx512)
        simd_name = "AVX-512 VBMI2";
    else if (fn == blend_scanline_avx2)
        simd_name = "AVX2";
#elif defined(__aarch64__) || defined(_M_ARM64)
    if (fn == blend_scanline_neon)
        simd_name = "ARM NEON";
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

static inline float clampf(float v, float min_v, float max_v)
{
    if (v < min_v)
        return min_v;
    if (v > max_v)
        return max_v;
    return v;
}

static inline float smoothstepf(float edge0, float edge1, float x)
{
    float t = clampf((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static inline float hash21(float x, float y)
{
    float px = x * 123.34f;
    float py = y * 456.21f;
    px -= floorf(px);
    py -= floorf(py);
    float d = px * (px + 45.32f) + py * (py + 45.32f);
    px += d;
    py += d;
    float prod = px * py;
    return prod - floorf(prod);
}

static inline float lattice_hash21(int ix, int iy)
{
    float val = hash21((float)ix, (float)iy);
    uint32_t u = (uint32_t)(val * 4294967296.0f);
    float scaled = (float)u * (1.0f / 4294967296.0f);
    if (scaled > 0.999884f)
        scaled = 0.999884f;
    return scaled;
}

static inline uint32_t blend_pixel_scalar(uint32_t a, uint32_t b, uint16_t wb)
{
    if (wb > 256)
        wb = 256;
    uint32_t wa = 256 - wb;
    uint32_t rb = (((a & 0x00FF00FF) * wa) + ((b & 0x00FF00FF) * wb)) >> 8;
    uint32_t g = (((a & 0x0000FF00) * wa) + ((b & 0x0000FF00) * wb)) >> 8;
    return (rb & 0x00FF00FF) | (g & 0x0000FF00) | 0xFF000000;
}

static inline uint32_t blend_pixel_fast(uint32_t a, uint32_t b, uint32_t wa, uint32_t wb)
{
    uint32_t rb = (((a & 0x00FF00FF) * wa) + ((b & 0x00FF00FF) * wb)) >> 8;
    uint32_t g = (((a & 0x0000FF00) * wa) + ((b & 0x0000FF00) * wb)) >> 8;
    return (rb & 0x00FF00FF) | (g & 0x0000FF00) | 0xFF000000;
}

#if defined(__x86_64__) || defined(_M_X64)
__attribute__((target("avx2,fma"))) static void
swirl_span_avx2(uint32_t *restrict dst_row, const uint32_t *restrict old_pixels,
                const uint32_t *restrict new_pixels, const float *restrict swirl_sin,
                const float *restrict swirl_cos, int x_start, int x_end, float cx_pixel,
                float cy_pixel, float y_rel, float y_rel2, float dist_scale, int lut_max, int max_x,
                int max_y, int width, uint32_t wa, uint32_t wb)
{
    __m256 v_cx = _mm256_set1_ps(cx_pixel);
    __m256 v_cy = _mm256_set1_ps(cy_pixel);
    __m256 v_y_rel = _mm256_set1_ps(y_rel);
    __m256 v_y_rel2 = _mm256_set1_ps(y_rel2);
    __m256 v_dist_scale = _mm256_set1_ps(dist_scale);
    __m256i v_lut_max = _mm256_set1_epi32(lut_max);
    __m256i v_max_x = _mm256_set1_epi32(max_x);
    __m256i v_max_y = _mm256_set1_epi32(max_y);
    __m256i v_width = _mm256_set1_epi32(width);
    __m256i v_zero = _mm256_setzero_si256();
    __m256i v_wa = _mm256_set1_epi16((short)wa);
    __m256i v_wb = _mm256_set1_epi16((short)wb);

    for (int x = x_start; x + 8 <= x_end; x += 8) {
        __m256 vx = _mm256_set_ps((float)(x + 7), (float)(x + 6), (float)(x + 5), (float)(x + 4),
                                  (float)(x + 3), (float)(x + 2), (float)(x + 1), (float)x);
        __m256 vx_rel = _mm256_sub_ps(vx, v_cx);
        __m256 vd2 = _mm256_fmadd_ps(vx_rel, vx_rel, v_y_rel2);
        __m256 vdist = _mm256_sqrt_ps(vd2);
        __m256 vrf = _mm256_mul_ps(vdist, v_dist_scale);
        __m256i vidx = _mm256_min_epi32(_mm256_cvttps_epi32(vrf), v_lut_max);

        __m256 vsin = _mm256_i32gather_ps(swirl_sin, vidx, 4);
        __m256 vcos = _mm256_i32gather_ps(swirl_cos, vidx, 4);

        __m256 vpx =
            _mm256_add_ps(v_cx, _mm256_fmsub_ps(vx_rel, vcos, _mm256_mul_ps(v_y_rel, vsin)));
        __m256 vpy =
            _mm256_add_ps(v_cy, _mm256_fmadd_ps(vx_rel, vsin, _mm256_mul_ps(v_y_rel, vcos)));

        __m256i vsx =
            _mm256_max_epi32(v_zero, _mm256_min_epi32(_mm256_cvttps_epi32(vpx), v_max_x));
        __m256i vsy =
            _mm256_max_epi32(v_zero, _mm256_min_epi32(_mm256_cvttps_epi32(vpy), v_max_y));

        __m256i v_off = _mm256_add_epi32(_mm256_mullo_epi32(vsy, v_width), vsx);

        __m256i v_ca = _mm256_i32gather_epi32((const int *)old_pixels, v_off, 4);
        __m256i v_cb = _mm256_i32gather_epi32((const int *)new_pixels, v_off, 4);

        __m256i a_lo = _mm256_unpacklo_epi8(v_ca, v_zero);
        __m256i a_hi = _mm256_unpackhi_epi8(v_ca, v_zero);
        __m256i b_lo = _mm256_unpacklo_epi8(v_cb, v_zero);
        __m256i b_hi = _mm256_unpackhi_epi8(v_cb, v_zero);

        __m256i r_lo = _mm256_srli_epi16(
            _mm256_add_epi16(_mm256_mullo_epi16(a_lo, v_wa), _mm256_mullo_epi16(b_lo, v_wb)), 8);
        __m256i r_hi = _mm256_srli_epi16(
            _mm256_add_epi16(_mm256_mullo_epi16(a_hi, v_wa), _mm256_mullo_epi16(b_hi, v_wb)), 8);

        __m256i packed = _mm256_packus_epi16(r_lo, r_hi);
        _mm256_storeu_si256((__m256i *)&dst_row[x], packed);
    }
}
#endif

static inline uint32_t add_saturate_rgb(uint32_t c, int flash)
{
    int r = (int)((c >> 16) & 0xFF) + flash;
    int g = (int)((c >> 8) & 0xFF) + flash;
    int b = (int)(c & 0xFF) + flash;
    if (r > 255)
        r = 255;
    else if (r < 0)
        r = 0;
    if (g > 255)
        g = 255;
    else if (g < 0)
        g = 0;
    if (b > 255)
        b = 255;
    else if (b < 0)
        b = 0;
    return (c & 0xFF000000) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline float value_noise2(float x, float y)
{
    float ix = floorf(x);
    float iy = floorf(y);
    float fx = x - ix;
    float fy = y - iy;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    float a = hash21(ix, iy);
    float b = hash21(ix + 1.0f, iy);
    float c = hash21(ix, iy + 1.0f);
    float d = hash21(ix + 1.0f, iy + 1.0f);
    return (a * (1.0f - fx) + b * fx) * (1.0f - fy) + (c * (1.0f - fx) + d * fx) * fy;
}

static inline float fbm2(float x, float y)
{
    return 0.65f * value_noise2(x, y) + 0.35f * value_noise2(x * 2.1f + 1.7f, y * 2.1f + 3.2f);
}

bool render_engine_execute_cpu_transition(uint32_t *dst, const uint32_t *old_pixels,
                                          const uint32_t *new_pixels, uint32_t width,
                                          uint32_t height, uint32_t stride_bytes,
                                          const waywal_transition_params_t *params)
{
    if (!dst || !old_pixels || !new_pixels || width == 0 || height == 0 || !params) {
        return false;
    }

    simd_blend_scanline_fn blend_fn = simd_get_blend_fn();
    uint32_t stride_pixels = stride_bytes / sizeof(uint32_t);
    float progress = clampf(params->progress, 0.0f, 1.0f);

    if (progress <= 0.0f) {
        for (uint32_t y = 0; y < height; ++y) {
            memcpy(dst + (size_t)y * stride_pixels, old_pixels + (size_t)y * width,
                   (size_t)width * sizeof(uint32_t));
        }
        return true;
    }
    if (progress >= 1.0f) {
        for (uint32_t y = 0; y < height; ++y) {
            memcpy(dst + (size_t)y * stride_pixels, new_pixels + (size_t)y * width,
                   (size_t)width * sizeof(uint32_t));
        }
        return true;
    }

    switch (params->type) {
    case WAYWAL_TRANSITION_FADE:
    case WAYWAL_TRANSITION_SIMPLE: {
        uint16_t weight_b = (uint16_t)(progress * 256.0f);
        if (weight_b > 256)
            weight_b = 256;

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            uint8_t *dst_row = (uint8_t *)(dst + (size_t)y * stride_pixels);
            const uint8_t *src_a_row = (const uint8_t *)(old_pixels + (size_t)y * width);
            const uint8_t *src_b_row = (const uint8_t *)(new_pixels + (size_t)y * width);

            blend_fn(dst_row, src_a_row, src_b_row, weight_b, (size_t)width * sizeof(uint32_t));
        }
        return true;
    }

    case WAYWAL_TRANSITION_WIPE: {
        float cos_a = cosf(params->angle_rad);
        float sin_a = sinf(params->angle_rad);
        float edge = 0.04f;
        float p = progress * (1.0f + 2.0f * edge) - edge;
        float d_proj = cos_a / (1.41421356f * (float)width);

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float ny = (float)y / (float)height - 0.5f;
            float proj_0 = (ny * sin_a - 0.5f * cos_a) / 1.41421356f + 0.5f;
            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

            for (uint32_t x = 0; x < width; ++x) {
                float proj = proj_0 + (float)x * d_proj;
                float alpha = smoothstepf(proj - edge, proj + edge, p);
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
        float edge = 0.03f * max_dist;
        float r = progress * (max_dist + 2.0f * edge) - edge;
        float r_inner = r - edge;
        float r_outer = r + edge;
        float r_inner2 = r_inner > 0.0f ? r_inner * r_inner : 0.0f;
        float r_outer2 = r_outer > 0.0f ? r_outer * r_outer : 0.0f;

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float dy = (float)y / (float)height - cy;
            float dy2 = dy * dy;
            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

            if (dy2 >= r_outer2 && r_outer2 > 0.0f) {
                memcpy(dst_row, a_row, (size_t)width * sizeof(uint32_t));
                continue;
            }

            for (uint32_t x = 0; x < width; ++x) {
                float dx = ((float)x / (float)width - cx) * aspect;
                float dist2 = dx * dx + dy2;
                if (dist2 >= r_outer2) {
                    dst_row[x] = a_row[x];
                } else if (dist2 <= r_inner2 && r_inner > 0.0f) {
                    dst_row[x] = b_row[x];
                } else {
                    float dist = sqrtf(dist2);
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
        float edge = 0.03f * max_dist;
        float r = (1.0f - progress) * (max_dist + 2.0f * edge) - edge;
        float r_inner = r - edge;
        float r_outer = r + edge;
        float r_inner2 = r_inner > 0.0f ? r_inner * r_inner : 0.0f;
        float r_outer2 = r_outer > 0.0f ? r_outer * r_outer : 0.0f;

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float dy = (float)y / (float)height - cy;
            float dy2 = dy * dy;
            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

            if (dy2 >= r_outer2 && r_outer2 > 0.0f) {
                memcpy(dst_row, b_row, (size_t)width * sizeof(uint32_t));
                continue;
            }

            for (uint32_t x = 0; x < width; ++x) {
                float dx = ((float)x / (float)width - cx) * aspect;
                float dist2 = dx * dx + dy2;
                if (dist2 >= r_outer2) {
                    dst_row[x] = b_row[x];
                } else if (dist2 <= r_inner2 && r_inner > 0.0f) {
                    dst_row[x] = a_row[x];
                } else {
                    float dist = sqrtf(dist2);
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
        }
        return true;
    }

    case WAYWAL_TRANSITION_WAVE: {
        float cos_a = cosf(params->angle_rad);
        float sin_a = sinf(params->angle_rad);
        float freq = params->wave_freq > 0.0f ? params->wave_freq : 20.0f;
        float amp = params->wave_amp > 0.0f ? params->wave_amp : 0.05f;
        float edge = 0.04f;
        float margin = amp + edge;
        float p = progress * (1.0f + 2.0f * margin) - margin;

        /* Precompute 1024-entry sine LUT */
        float sine_lut[1024];
        for (int i = 0; i < 1024; ++i) {
            float theta = (float)i * (6.28318530718f / 1024.0f);
            sine_lut[i] = sinf(theta) * amp;
        }

        float d_proj = cos_a / (1.41421356f * (float)width);
        float d_cross = (-sin_a * freq) / (float)width;
        float d_phase = d_cross * (1024.0f / 6.28318530718f);

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float ny = (float)y / (float)height - 0.5f;
            float proj_0 = (ny * sin_a - 0.5f * cos_a) / 1.41421356f + 0.5f;
            float phase_0 = (ny * cos_a + 0.5f * sin_a) * freq * (1024.0f / 6.28318530718f);
            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

            for (uint32_t x = 0; x < width; ++x) {
                float proj = proj_0 + (float)x * d_proj;
                float ph = phase_0 + (float)x * d_phase;
                int idx = ((int)ph) & 1023;
                float wave = sine_lut[idx];
                float d = proj + wave;
                float alpha = smoothstepf(d - edge, d + edge, p);
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
        float p = progress * (1.0f + 2.0f * edge) - edge;
        float inv_w = 16.0f / (float)width;
        float inv_h = 16.0f / (float)height;

        /* Precompute 18x18 discrete lattice noise grid */
        float grid[18][18];
        for (int j = 0; j < 18; ++j) {
            for (int i = 0; i < 18; ++i) {
                grid[j][i] = lattice_hash21(i, j);
            }
        }

        typedef struct {
            int ix;
            float cx;
        } noise_col_t;

        noise_col_t col_lut_stack[4096];
        noise_col_t *col_lut = (width <= 4096)
                                   ? col_lut_stack
                                   : (noise_col_t *)malloc((size_t)width * sizeof(noise_col_t));
        if (!col_lut)
            return false;

        for (uint32_t x = 0; x < width; ++x) {
            float vx = ((float)x + 0.5f) * inv_w;
            int ix = (int)vx;
            if (ix < 0)
                ix = 0;
            if (ix > 16)
                ix = 16;
            float fx = vx - (float)ix;
            col_lut[x].ix = ix;
            col_lut[x].cx = fx * fx * (3.0f - 2.0f * fx);
        }

        float inv_2edge = 1.0f / (2.0f * edge);

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float vy = ((float)y + 0.5f) * inv_h;
            int iy = (int)vy;
            if (iy < 0)
                iy = 0;
            if (iy > 16)
                iy = 16;
            float fy = vy - (float)iy;
            float cy = fy * fy * (3.0f - 2.0f * fy);

            /* 1D scanline lerp factorization across columns */
            float v[18];
            for (int i = 0; i < 18; ++i) {
                v[i] = grid[iy][i] * (1.0f - cy) + grid[iy + 1][i] * cy;
            }

            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

            for (uint32_t x = 0; x < width; ++x) {
                int ix = col_lut[x].ix;
                float n = v[ix] + (v[ix + 1] - v[ix]) * col_lut[x].cx;

                float diff = p - (n - edge);
                if (diff <= 0.0f) {
                    dst_row[x] = a_row[x];
                } else if (diff >= 2.0f * edge) {
                    dst_row[x] = b_row[x];
                } else {
                    float t = diff * inv_2edge;
                    float alpha = t * t * (3.0f - 2.0f * t);
                    uint16_t wb = (uint16_t)(alpha * 256.0f);
                    dst_row[x] = blend_pixel_scalar(a_row[x], b_row[x], wb);
                }
            }
        }

        if (col_lut != col_lut_stack)
            free(col_lut);
        return true;
    }

    case WAYWAL_TRANSITION_CROSSZOOM: {
        float cx = (params->center_x > 0.0f || params->center_y > 0.0f) ? params->center_x : 0.5f;
        float cy = (params->center_y > 0.0f || params->center_y > 0.0f) ? params->center_y : 0.5f;
        float strength = params->wave_amp > 0.001f ? params->wave_amp : 0.4f;
        float peak = sinf(progress * 3.14159265f);
        int flash = (int)(peak * 60.0f);
        float zoom_a = 1.0f - progress * strength;
        float zoom_b = 1.0f + (1.0f - progress) * strength;
        uint16_t wb = (uint16_t)(progress * 256.0f);

        int lut_ax_stack[4096];
        int lut_bx_stack[4096];
        int *lut_ax = (width <= 4096) ? lut_ax_stack : (int *)malloc((size_t)width * sizeof(int));
        int *lut_bx = (width <= 4096) ? lut_bx_stack : (int *)malloc((size_t)width * sizeof(int));
        if (!lut_ax || !lut_bx) {
            if (lut_ax && lut_ax != lut_ax_stack)
                free(lut_ax);
            if (lut_bx && lut_bx != lut_bx_stack)
                free(lut_bx);
            return false;
        }

        for (uint32_t x = 0; x < width; ++x) {
            float nx = ((float)x / (float)width - cx);
            int ax = (int)((cx + nx * zoom_a) * (float)width);
            if (ax < 0)
                ax = 0;
            else if (ax >= (int)width)
                ax = width - 1;
            int bx = (int)((cx + nx * zoom_b) * (float)width);
            if (bx < 0)
                bx = 0;
            else if (bx >= (int)width)
                bx = width - 1;
            lut_ax[x] = ax;
            lut_bx[x] = bx;
        }

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float ny = ((float)y / (float)height - cy);
            int src_ay = (int)((cy + ny * zoom_a) * (float)height);
            if (src_ay < 0)
                src_ay = 0;
            else if (src_ay >= (int)height)
                src_ay = height - 1;

            int src_by = (int)((cy + ny * zoom_b) * (float)height);
            if (src_by < 0)
                src_by = 0;
            else if (src_by >= (int)height)
                src_by = height - 1;

            const uint32_t *row_a = old_pixels + (size_t)src_ay * width;
            const uint32_t *row_b = new_pixels + (size_t)src_by * width;
            uint32_t *dst_row = dst + (size_t)y * stride_pixels;

            for (uint32_t x = 0; x < width; ++x) {
                uint32_t ca = row_a[lut_ax[x]];
                uint32_t cb = row_b[lut_bx[x]];
                uint32_t blended = blend_pixel_scalar(ca, cb, wb);
                dst_row[x] = flash > 0 ? add_saturate_rgb(blended, flash) : blended;
            }
        }

        if (lut_ax != lut_ax_stack)
            free(lut_ax);
        if (lut_bx != lut_bx_stack)
            free(lut_bx);
        return true;
    }

    case WAYWAL_TRANSITION_SLIDE: {
        float cos_a = cosf(params->angle_rad);
        float sin_a = sinf(params->angle_rad);
        float edge = 0.04f;
        float p = progress * (1.0f + 2.0f * edge) - edge;
        float shift_ax = cos_a * progress * (float)width;
        float shift_bx = -cos_a * (1.0f - progress) * (float)width;
        float d_proj = cos_a / (1.41421356f * (float)width);

        int lut_ax_stack[4096];
        int lut_bx_stack[4096];
        int *lut_ax = (width <= 4096) ? lut_ax_stack : (int *)malloc((size_t)width * sizeof(int));
        int *lut_bx = (width <= 4096) ? lut_bx_stack : (int *)malloc((size_t)width * sizeof(int));
        if (!lut_ax || !lut_bx) {
            if (lut_ax && lut_ax != lut_ax_stack)
                free(lut_ax);
            if (lut_bx && lut_bx != lut_bx_stack)
                free(lut_bx);
            return false;
        }

        for (uint32_t x = 0; x < width; ++x) {
            int ax = (int)((float)x + shift_ax);
            if (ax < 0)
                ax = 0;
            else if (ax >= (int)width)
                ax = width - 1;

            int bx = (int)((float)x + shift_bx);
            if (bx < 0)
                bx = 0;
            else if (bx >= (int)width)
                bx = width - 1;

            lut_ax[x] = ax;
            lut_bx[x] = bx;
        }

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float ny = (float)y / (float)height - 0.5f;
            int ay = (int)((float)y + sin_a * progress * (float)height);
            if (ay < 0)
                ay = 0;
            else if (ay >= (int)height)
                ay = height - 1;

            int by = (int)((float)y - sin_a * (1.0f - progress) * (float)height);
            if (by < 0)
                by = 0;
            else if (by >= (int)height)
                by = height - 1;

            const uint32_t *row_a = old_pixels + (size_t)ay * width;
            const uint32_t *row_b = new_pixels + (size_t)by * width;
            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            float proj_0 = (ny * sin_a - 0.5f * cos_a) / 1.41421356f + 0.5f;

            for (uint32_t x = 0; x < width; ++x) {
                float proj = proj_0 + (float)x * d_proj;
                float split = smoothstepf(proj - edge, proj + edge, p);

                uint32_t ca = row_a[lut_ax[x]];
                uint32_t cb = row_b[lut_bx[x]];
                if (split <= 0.0f) {
                    dst_row[x] = ca;
                } else if (split >= 1.0f) {
                    dst_row[x] = cb;
                } else {
                    uint16_t wb = (uint16_t)(split * 256.0f);
                    dst_row[x] = blend_pixel_scalar(ca, cb, wb);
                }
            }
        }

        if (lut_ax != lut_ax_stack)
            free(lut_ax);
        if (lut_bx != lut_bx_stack)
            free(lut_bx);
        return true;
    }

    case WAYWAL_TRANSITION_GLITCH: {
        float bands = params->wave_freq > 0.0f ? params->wave_freq : 35.0f;
        float intensity = params->wave_amp > 0.0f ? params->wave_amp : 0.06f;
        float peak = sinf(progress * 3.14159265f);
        float step_t = floorf(progress * 24.0f);

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float slice_idx = floorf(((float)y / (float)height) * bands);
            float h = hash21(slice_idx, step_t);
            int jitter = 0;
            if (h > 0.60f) {
                jitter = (int)((h - 0.80f) * 2.5f * intensity * peak * (float)width);
            }
            int split = (int)(peak * intensity * 0.5f * (float)width);
            float block_y = floorf((float)y / 14.0f) + step_t;

            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

            /* 54-block coarsened evaluation (24-pixel blocks across width) */
            for (uint32_t bx_start = 0; bx_start < width; bx_start += 24) {
                uint32_t bx_end = bx_start + 24;
                if (bx_end > width)
                    bx_end = width;
                size_t blk_len = bx_end - bx_start;

                float block_h = hash21((float)(bx_start / 24), block_y);
                float gprog = progress + (block_h - 0.5f) * peak * 0.35f;
                float m = smoothstepf(0.46f, 0.54f, gprog);
                uint16_t wb = (uint16_t)(m * 256.0f);

                if (jitter == 0 && split == 0) {
                    blend_fn((uint8_t *)(dst_row + bx_start), (const uint8_t *)(a_row + bx_start),
                             (const uint8_t *)(b_row + bx_start), wb,
                             blk_len * sizeof(uint32_t));
                } else {
                    for (uint32_t x = bx_start; x < bx_end; ++x) {
                        int rx = (int)x + jitter + split;
                        int gx = (int)x + jitter;
                        int bx = (int)x + jitter - split;
                        if (rx < 0)
                            rx = 0;
                        else if (rx >= (int)width)
                            rx = width - 1;
                        if (gx < 0)
                            gx = 0;
                        else if (gx >= (int)width)
                            gx = width - 1;
                        if (bx < 0)
                            bx = 0;
                        else if (bx >= (int)width)
                            bx = width - 1;

                        uint32_t a_r = a_row[rx] & 0x00FF0000;
                        uint32_t a_g = a_row[gx] & 0x0000FF00;
                        uint32_t a_b = a_row[bx] & 0x000000FF;
                        uint32_t ca = 0xFF000000 | a_r | a_g | a_b;

                        uint32_t b_r = b_row[rx] & 0x00FF0000;
                        uint32_t b_g = b_row[gx] & 0x0000FF00;
                        uint32_t b_b = b_row[bx] & 0x000000FF;
                        uint32_t cb = 0xFF000000 | b_r | b_g | b_b;

                        dst_row[x] = blend_pixel_scalar(ca, cb, wb);
                    }
                }
            }
        }
        return true;
    }

    case WAYWAL_TRANSITION_BURN: {
        float scale = params->wave_freq > 0.0f ? params->wave_freq : 14.0f;
        float flame_width = params->wave_amp > 0.0f ? params->wave_amp : 0.08f;
        float p = progress * (1.0f + 2.0f * flame_width) - flame_width;

        /* Precompute 2D discrete lattice noise grids for Octave 1 and Octave 2:
         * Octave 1: nx * scale, ny * scale.
         * Octave 2: (nx * scale) * 2.1f + 1.7f, (ny * scale) * 2.1f + 3.2f. */
        int gw1 = (int)(scale + 3.0f);
        if (gw1 < 16)
            gw1 = 16;
        if (gw1 > 48)
            gw1 = 48;
        int gh1 = gw1;

        int gw2 = (int)(scale * 2.1f + 5.0f);
        if (gw2 < 32)
            gw2 = 32;
        if (gw2 > 96)
            gw2 = 96;
        int gh2 = (int)(scale * 2.1f + 6.0f);
        if (gh2 < 32)
            gh2 = 32;
        if (gh2 > 96)
            gh2 = 96;

        float grid1[48][48];
        for (int j = 0; j < gh1; ++j) {
            for (int i = 0; i < gw1; ++i) {
                grid1[j][i] = lattice_hash21(i, j);
            }
        }

        float grid2[96][96];
        for (int j = 0; j < gh2; ++j) {
            for (int i = 0; i < gw2; ++i) {
                grid2[j][i] = lattice_hash21(i, j);
            }
        }

        float inv_w = 1.0f / (float)width;
        float inv_h = 1.0f / (float)height;
        float step1_x = scale * inv_w;
        float step2_x = (scale * 2.1f) * inv_w;

        typedef struct {
            int16_t ix1;
            int16_t ix2;
            float cx1;
            float cx2;
        } burn_col_t;

        burn_col_t burn_lut_stack[4096];
        burn_col_t *burn_lut = (width <= 4096)
                                   ? burn_lut_stack
                                   : (burn_col_t *)malloc((size_t)width * sizeof(burn_col_t));
        if (!burn_lut)
            return false;

        for (uint32_t x = 0; x < width; ++x) {
            float x1 = (float)x * step1_x;
            int ix1 = (int)x1;
            if (ix1 < 0)
                ix1 = 0;
            if (ix1 > gw1 - 2)
                ix1 = gw1 - 2;
            float fx1 = x1 - (float)ix1;
            float cx1 = fx1 * fx1 * (3.0f - 2.0f * fx1);

            float x2 = (float)x * step2_x + 1.7f;
            int ix2 = (int)x2;
            if (ix2 < 0)
                ix2 = 0;
            if (ix2 > gw2 - 2)
                ix2 = gw2 - 2;
            float fx2 = x2 - (float)ix2;
            float cx2 = fx2 * fx2 * (3.0f - 2.0f * fx2);

            burn_lut[x].ix1 = (int16_t)ix1;
            burn_lut[x].ix2 = (int16_t)ix2;
            burn_lut[x].cx1 = cx1;
            burn_lut[x].cx2 = cx2;
        }

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float ny = (float)y * inv_h;

            /* Octave 1 row lerp */
            float y1 = ny * scale;
            int iy1 = (int)y1;
            if (iy1 < 0)
                iy1 = 0;
            if (iy1 > gh1 - 2)
                iy1 = gh1 - 2;
            float fy1 = y1 - (float)iy1;
            float cy1 = fy1 * fy1 * (3.0f - 2.0f * fy1);
            float v1[48];
            for (int i = 0; i < gw1; ++i) {
                v1[i] = grid1[iy1][i] * (1.0f - cy1) + grid1[iy1 + 1][i] * cy1;
            }

            /* Octave 2 row lerp */
            float y2 = (ny * scale) * 2.1f + 3.2f;
            int iy2 = (int)y2;
            if (iy2 < 0)
                iy2 = 0;
            if (iy2 > gh2 - 2)
                iy2 = gh2 - 2;
            float fy2 = y2 - (float)iy2;
            float cy2 = fy2 * fy2 * (3.0f - 2.0f * fy2);
            float v2[96];
            for (int i = 0; i < gw2; ++i) {
                v2[i] = grid2[iy2][i] * (1.0f - cy2) + grid2[iy2 + 1][i] * cy2;
            }

            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

            for (uint32_t x = 0; x < width; ++x) {
                int ix1 = burn_lut[x].ix1;
                float cx1 = burn_lut[x].cx1;
                float n1 = v1[ix1] + (v1[ix1 + 1] - v1[ix1]) * cx1;

                int ix2 = burn_lut[x].ix2;
                float cx2 = burn_lut[x].cx2;
                float n2 = v2[ix2] + (v2[ix2 + 1] - v2[ix2]) * cx2;

                float n = 0.65f * n1 + 0.35f * n2;
                if (n > 0.99988f)
                    n = 0.99988f;

                float d = n - p;
                if (d > flame_width) {
                    dst_row[x] = a_row[x];
                } else if (d < -flame_width) {
                    dst_row[x] = b_row[x];
                } else {
                    float burn_factor = smoothstepf(flame_width, -flame_width, d);
                    uint16_t wb = (uint16_t)(burn_factor * 256.0f);
                    uint32_t base = blend_pixel_scalar(a_row[x], b_row[x], wb);

                    float dist_norm = 1.0f - fabsf(d) / flame_width;
                    float heat = dist_norm * dist_norm;
                    int fr = (int)(heat * 240.0f);
                    int fg = (int)(heat * 120.0f);
                    int fb = (int)(heat * 20.0f);

                    int r = (int)((base >> 16) & 0xFF) + fr;
                    int g = (int)((base >> 8) & 0xFF) + fg;
                    int b = (int)(base & 0xFF) + fb;
                    if (r > 255)
                        r = 255;
                    if (g > 255)
                        g = 255;
                    if (b > 255)
                        b = 255;
                    dst_row[x] =
                        0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                }
            }
        }

        if (burn_lut != burn_lut_stack)
            free(burn_lut);
        return true;
    }

    case WAYWAL_TRANSITION_RIPPLE: {
        float cx = (params->center_x > 0.0f || params->center_y > 0.0f) ? params->center_x : 0.5f;
        float cy = (params->center_y > 0.0f || params->center_y > 0.0f) ? params->center_y : 0.5f;
        float aspect = (float)width / (float)height;
        float max_x = cx > (1.0f - cx) ? cx : (1.0f - cx);
        float max_y = cy > (1.0f - cy) ? cy : (1.0f - cy);
        float max_dist = sqrtf((max_x * aspect) * (max_x * aspect) + max_y * max_y) + 0.1f;
        float current_radius = progress * max_dist;
        float wave_width = 0.18f;
        float freq = params->wave_freq > 0.0f ? params->wave_freq : 32.0f;
        float amp = params->wave_amp > 0.0f ? params->wave_amp : 0.04f;
        float decay = (1.0f - progress) * (1.0f - progress);

        float r_inner = current_radius - wave_width;
        float r_outer = current_radius + wave_width;
        float r_inner2 = r_inner > 0.0f ? r_inner * r_inner : 0.0f;
        float r_outer2 = r_outer * r_outer;

        /* Precompute 512-entry wave LUT for diff in [-wave_width, wave_width] */
        float wave_lut[512];
        float inv_span = 511.0f / (2.0f * wave_width);
        for (int k = 0; k < 512; ++k) {
            float diff = -wave_width + ((float)k / 511.0f) * (2.0f * wave_width);
            float window = smoothstepf(wave_width, 0.0f, fabsf(diff));
            wave_lut[k] = sinf(diff * freq) * amp * decay * window;
        }

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float dy = (float)y / (float)height - cy;
            float dy2 = dy * dy;
            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

            if (dy2 >= r_outer2) {
                memcpy(dst_row, a_row, (size_t)width * sizeof(uint32_t));
                continue;
            }

            for (uint32_t x = 0; x < width; ++x) {
                float dx = ((float)x / (float)width - cx) * aspect;
                float dist2 = dx * dx + dy2;

                if (dist2 >= r_outer2) {
                    dst_row[x] = a_row[x];
                } else if (dist2 <= r_inner2 && r_inner > 0.0f) {
                    dst_row[x] = b_row[x];
                } else {
                    float dist = sqrtf(dist2);
                    float diff = dist - current_radius;

                    float wave = 0.0f;
                    if (fabsf(diff) < wave_width) {
                        int idx = (int)((diff + wave_width) * inv_span);
                        if (idx < 0)
                            idx = 0;
                        else if (idx > 511)
                            idx = 511;
                        wave = wave_lut[idx];
                    }

                    float ndx = (dist > 0.0001f) ? (dx / dist / aspect) : 0.0f;
                    float ndy = (dist > 0.0001f) ? (dy / dist) : 0.0f;

                    int sx = (int)(((float)x / (float)width + ndx * wave) * (float)width);
                    int sy = (int)(((float)y / (float)height + ndy * wave) * (float)height);
                    if (sx < 0)
                        sx = 0;
                    else if (sx >= (int)width)
                        sx = width - 1;
                    if (sy < 0)
                        sy = 0;
                    else if (sy >= (int)height)
                        sy = height - 1;

                    uint32_t ca = old_pixels[(size_t)sy * width + sx];
                    uint32_t cb = new_pixels[(size_t)sy * width + sx];

                    float split =
                        smoothstepf(current_radius - 0.02f, current_radius + 0.02f, dist);
                    uint16_t wa = (uint16_t)(split * 256.0f);
                    dst_row[x] = blend_pixel_scalar(cb, ca, wa);
                }
            }
        }
        return true;
    }

    case WAYWAL_TRANSITION_PIXELATE: {
        float d = sinf(progress * 3.1415926535f) * 64.0f + 1.0f;
        uint32_t block_sz = (d >= 1.0f) ? (uint32_t)d : 1u;
        uint16_t wb = (uint16_t)(progress * 256.0f);

#pragma omp parallel for schedule(dynamic, 8)
        for (uint32_t by = 0; by < height; by += block_sz) {
            uint32_t bh = (by + block_sz <= height) ? block_sz : (height - by);
            uint32_t sy = by + bh / 2;
            if (sy >= height)
                sy = height - 1;

            for (uint32_t bx = 0; bx < width; bx += block_sz) {
                uint32_t bw = (bx + block_sz <= width) ? block_sz : (width - bx);
                uint32_t sx = bx + bw / 2;
                if (sx >= width)
                    sx = width - 1;

                uint32_t ca = old_pixels[sy * width + sx];
                uint32_t cb = new_pixels[sy * width + sx];
                uint32_t blended = blend_pixel_scalar(ca, cb, wb);

                for (uint32_t y = by; y < by + bh; ++y) {
                    uint32_t *dst_row = dst + y * stride_pixels;
                    for (uint32_t x = bx; x < bx + bw; ++x) {
                        dst_row[x] = blended;
                    }
                }
            }
        }
        return true;
    }

    case WAYWAL_TRANSITION_DOOM: {
        uint32_t col_width = 16;
#pragma omp parallel for schedule(static, 8)
        for (uint32_t bx = 0; bx < width; bx += col_width) {
            uint32_t bw = (bx + col_width <= width) ? col_width : (width - bx);
            float col_idx = (float)(bx / col_width);
            float delay = fmodf(col_idx * 123.34f + 45.32f, 1.0f);
            if (delay < 0.0f)
                delay += 1.0f;
            delay = fmodf(delay * (delay + 33.33f) * 1337.5f, 1.0f);
            if (delay < 0.0f)
                delay += 1.0f;
            delay *= 0.35f;

            float shift = clampf((progress * 1.35f - delay) / 1.0f, 0.0f, 1.0f);
            int pixel_shift = (int)(shift * height);

            for (uint32_t y = 0; y < height; ++y) {
                uint32_t *dst_row = dst + y * stride_pixels;
                if ((int)y < pixel_shift) {
                    for (uint32_t x = bx; x < bx + bw; ++x) {
                        dst_row[x] = new_pixels[y * width + x];
                    }
                } else {
                    int sy = (int)y - pixel_shift;
                    if (sy < 0)
                        sy = 0;
                    if (sy >= (int)height)
                        sy = height - 1;
                    for (uint32_t x = bx; x < bx + bw; ++x) {
                        dst_row[x] = old_pixels[sy * width + x];
                    }
                }
            }
        }
        return true;
    }

    case WAYWAL_TRANSITION_SWIRL: {
        float cx = (params->center_x > 0.0f || params->center_y > 0.0f) ? params->center_x : 0.5f;
        float cy = (params->center_y > 0.0f || params->center_y > 0.0f) ? params->center_y : 0.5f;
        float angle_amt = sinf(progress * 3.1415926535f) * 8.0f;
        float max_r = 0.85f;
        uint16_t wb = (uint16_t)(progress * 256.0f);
        uint32_t wa = 256 - wb;

        if (fabsf(angle_amt) < 1e-4f) {
#pragma omp parallel for schedule(static, 8)
            for (uint32_t y = 0; y < height; ++y) {
                blend_fn((uint8_t *)(dst + (size_t)y * stride_pixels),
                         (const uint8_t *)(old_pixels + (size_t)y * width),
                         (const uint8_t *)(new_pixels + (size_t)y * width), wb,
                         (size_t)width * sizeof(uint32_t));
            }
            return true;
        }

        /* Direct pixel-space geometry */
        float max_dist = max_r * (float)height;
        float max_dist2 = max_dist * max_dist;
        float cx_pixel = cx * (float)width;
        float cy_pixel = cy * (float)height;

        /* 1024-entry sincos LUT (8 KB, perfectly resident in L1 cache) */
#define SWIRL_LUT_SIZE 1024
        float swirl_sin[SWIRL_LUT_SIZE + 1];
        float swirl_cos[SWIRL_LUT_SIZE + 1];
        float dist_scale = (float)SWIRL_LUT_SIZE / max_dist;
        float inv_dist_scale = max_dist / (float)SWIRL_LUT_SIZE;
        for (int i = 0; i < SWIRL_LUT_SIZE; ++i) {
            float dist = (float)i * inv_dist_scale;
            float factor = (1.0f - dist / max_dist);
            float theta = factor * factor * angle_amt;
            sincosf(theta, &swirl_sin[i], &swirl_cos[i]);
        }
        swirl_sin[SWIRL_LUT_SIZE] = 0.0f;
        swirl_cos[SWIRL_LUT_SIZE] = 1.0f;

        int max_x = (int)width - 1;
        int max_y = (int)height - 1;

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float y_rel = (float)y - cy_pixel;
            float y_rel2 = y_rel * y_rel;
            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

            if (y_rel2 >= max_dist2) {
                blend_fn((uint8_t *)dst_row, (const uint8_t *)a_row, (const uint8_t *)b_row, wb,
                         (size_t)width * sizeof(uint32_t));
                continue;
            }

            /* Circle chord in pixel coordinates: x_span = sqrt(max_dist^2 - y_rel^2) */
            float x_span = sqrtf(max_dist2 - y_rel2);
            int x_start = (int)(cx_pixel - x_span);
            int x_end = (int)(cx_pixel + x_span) + 1;
            if (x_start < 0)
                x_start = 0;
            if (x_start > (int)width)
                x_start = (int)width;
            if (x_end < 0)
                x_end = 0;
            if (x_end > (int)width)
                x_end = (int)width;
            if (x_start > x_end)
                x_start = x_end;

            /* Left span outside circle: SIMD blend */
            if (x_start > 0) {
                blend_fn((uint8_t *)dst_row, (const uint8_t *)a_row, (const uint8_t *)b_row, wb,
                         (size_t)x_start * sizeof(uint32_t));
            }

            /* Inside circle span: evaluate swirl deformation */
            int x = x_start;
#if defined(__x86_64__) || defined(_M_X64)
            if (__builtin_cpu_supports("avx2")) {
                int avx_count = (x_end - x_start) & ~7;
                if (avx_count > 0) {
                    swirl_span_avx2(dst_row, old_pixels, new_pixels, swirl_sin, swirl_cos,
                                    x_start, x_start + avx_count, cx_pixel, cy_pixel,
                                    y_rel, y_rel2, dist_scale, SWIRL_LUT_SIZE, max_x, max_y,
                                    (int)width, wa, wb);
                    x = x_start + avx_count;
                }
            }
#endif
            for (; x < x_end; ++x) {
                float x_rel = (float)x - cx_pixel;
                float d2 = x_rel * x_rel + y_rel2;
                if (d2 < max_dist2) {
                    float dist = sqrtf(d2);
                    float rf = dist * dist_scale;
                    int idx = (int)rf;
                    if (idx > SWIRL_LUT_SIZE)
                        idx = SWIRL_LUT_SIZE;
                    float sin_t = swirl_sin[idx];
                    float cos_t = swirl_cos[idx];

                    float px = cx_pixel + x_rel * cos_t - y_rel * sin_t;
                    float py = cy_pixel + x_rel * sin_t + y_rel * cos_t;

                    int sx = (int)px;
                    int sy = (int)py;
                    if (sx < 0)
                        sx = 0;
                    else if (sx > max_x)
                        sx = max_x;
                    if (sy < 0)
                        sy = 0;
                    else if (sy > max_y)
                        sy = max_y;

                    uint32_t ca = old_pixels[(size_t)sy * width + sx];
                    uint32_t cb = new_pixels[(size_t)sy * width + sx];
                    dst_row[x] = blend_pixel_fast(ca, cb, wa, wb);
                } else {
                    dst_row[x] = blend_pixel_fast(a_row[x], b_row[x], wa, wb);
                }
            }

            /* Right span outside circle: SIMD blend */
            if (x_end < (int)width) {
                blend_fn((uint8_t *)(dst_row + x_end), (const uint8_t *)(a_row + x_end),
                         (const uint8_t *)(b_row + x_end), wb,
                         (size_t)(width - x_end) * sizeof(uint32_t));
            }
        }
#undef SWIRL_LUT_SIZE
        return true;
    }

    case WAYWAL_TRANSITION_CUBE: {
        float theta = progress * 1.57079632679f;
        float zoom = 1.0f + 0.35f * sinf(progress * 3.1415926535f);
        float cos_t = cosf(theta);
        float sin_t = sinf(theta);
        float d_cam = 2.5f;

        float ro_rel_z = -d_cam - 1.0f;
        float ro_cube_x = -ro_rel_z * sin_t;
        float ro_cube_z = ro_rel_z * cos_t;

        float shade_front = 0.65f + 0.35f * cos_t;
        float shade_right = 0.65f + 0.35f * sin_t;
        uint16_t wb_front = (uint16_t)(shade_front * 256.0f);
        uint16_t wb_right = (uint16_t)(shade_right * 256.0f);

        typedef struct {
            int face; // 0 = none, 1 = front, 2 = right
            int sx;
            float ky;
        } cube_col_t;

        cube_col_t col_lut_stack[4096];
        cube_col_t *col_lut = (width <= 4096)
                                  ? col_lut_stack
                                  : (cube_col_t *)malloc((size_t)width * sizeof(cube_col_t));
        if (!col_lut)
            return false;

        for (uint32_t x = 0; x < width; ++x) {
            float px = ((float)x / (float)width - 0.5f) * 2.0f;
            float rdx = px * zoom;
            float rdz = d_cam;

            float rd_cube_x = rdx * cos_t - rdz * sin_t;
            float rd_cube_z = rdx * sin_t + rdz * cos_t;

            float t_f = -1.0f;
            float hx = 0.0f;
            if (fabsf(rd_cube_z) > 1e-5f) {
                float t = (-1.0f - ro_cube_z) / rd_cube_z;
                if (t > 0.0f) {
                    hx = ro_cube_x + t * rd_cube_x;
                    if (fabsf(hx) <= 1.0f) {
                        t_f = t;
                    }
                }
            }

            float t_r = -1.0f;
            float hz = 0.0f;
            if (fabsf(rd_cube_x) > 1e-5f) {
                float t = (1.0f - ro_cube_x) / rd_cube_x;
                if (t > 0.0f) {
                    hz = ro_cube_z + t * rd_cube_z;
                    if (fabsf(hz) <= 1.0f) {
                        t_r = t;
                    }
                }
            }

            if (t_f > 0.0f && (t_r < 0.0f || t_f < t_r)) {
                int sx = (int)((hx * 0.5f + 0.5f) * (float)width);
                if (sx < 0)
                    sx = 0;
                else if (sx >= (int)width)
                    sx = width - 1;
                col_lut[x].face = 1;
                col_lut[x].sx = sx;
                col_lut[x].ky = zoom * t_f;
            } else if (t_r > 0.0f) {
                int sx = (int)(((hz + 1.0f) * 0.5f) * (float)width);
                if (sx < 0)
                    sx = 0;
                else if (sx >= (int)width)
                    sx = width - 1;
                col_lut[x].face = 2;
                col_lut[x].sx = sx;
                col_lut[x].ky = zoom * t_r;
            } else {
                col_lut[x].face = 0;
                col_lut[x].sx = 0;
                col_lut[x].ky = 0.0f;
            }
        }

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float py = ((float)y / (float)height - 0.5f) * 2.0f;
            uint32_t *dst_row = dst + (size_t)y * stride_pixels;

            for (uint32_t x = 0; x < width; ++x) {
                const cube_col_t *col = &col_lut[x];
                if (col->face == 1) {
                    float hy = py * col->ky;
                    if (fabsf(hy) <= 1.0f) {
                        int sy = (int)((hy * 0.5f + 0.5f) * (float)height);
                        if (sy < 0)
                            sy = 0;
                        else if (sy >= (int)height)
                            sy = height - 1;
                        uint32_t c = old_pixels[(size_t)sy * width + col->sx];
                        dst_row[x] = blend_pixel_scalar(0xFF000000, c, wb_front);
                        continue;
                    }
                } else if (col->face == 2) {
                    float hy = py * col->ky;
                    if (fabsf(hy) <= 1.0f) {
                        int sy = (int)((hy * 0.5f + 0.5f) * (float)height);
                        if (sy < 0)
                            sy = 0;
                        else if (sy >= (int)height)
                            sy = height - 1;
                        uint32_t c = new_pixels[(size_t)sy * width + col->sx];
                        dst_row[x] = blend_pixel_scalar(0xFF000000, c, wb_right);
                        continue;
                    }
                }
                dst_row[x] = 0xFF08080C;
            }
        }

        if (col_lut != col_lut_stack)
            free(col_lut);
        return true;
    }

    case WAYWAL_TRANSITION_LUMA: {
        float threshold = progress * 1.2f - 0.1f;
        float softness = 0.08f;
        float aspect = (float)width / (float)height;

        int grid_w = (int)(aspect * 4.0f) + 3;
        if (grid_w > 48)
            grid_w = 48;
        int grid_h = 7;

        float grid[7][48];
        for (int j = 0; j < grid_h; ++j) {
            for (int i = 0; i < grid_w; ++i) {
                grid[j][i] = lattice_hash21(i, j);
            }
        }

        float inv_w_scale = (aspect * 4.0f) / (float)width;
        float inv_h_scale = 4.0f / (float)height;

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float gy = (float)y * inv_h_scale;
            int iy = (int)gy;
            if (iy < 0)
                iy = 0;
            if (iy > grid_h - 2)
                iy = grid_h - 2;
            float fy = gy - (float)iy;
            float cy = fy * fy * (3.0f - 2.0f * fy);

            float v[48];
            for (int i = 0; i < grid_w; ++i) {
                v[i] = grid[iy][i] * (1.0f - cy) + grid[iy + 1][i] * cy;
            }

            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

            for (uint32_t x = 0; x < width; ++x) {
                float gx = (float)x * inv_w_scale;
                int ix = (int)gx;
                if (ix < 0)
                    ix = 0;
                if (ix > grid_w - 2)
                    ix = grid_w - 2;
                float fx = gx - (float)ix;
                float cx = fx * fx * (3.0f - 2.0f * fx);
                float n = v[ix] + (v[ix + 1] - v[ix]) * cx;

                float alpha = smoothstepf(threshold - softness, threshold + softness, n);
                float mix_val = 1.0f - alpha;
                if (mix_val <= 0.0f) {
                    dst_row[x] = a_row[x];
                } else if (mix_val >= 1.0f) {
                    dst_row[x] = b_row[x];
                } else {
                    uint16_t wb = (uint16_t)(mix_val * 256.0f);
                    dst_row[x] = blend_pixel_scalar(a_row[x], b_row[x], wb);
                }
            }
        }
        return true;
    }

    case WAYWAL_TRANSITION_LIGHT_LEAK: {
        float mix_factor = smoothstepf(0.25f, 0.75f, progress);
        uint16_t wb = (uint16_t)(mix_factor * 256.0f);
        float sweep = progress * 1.6f - 0.3f;
        float env = sinf(progress * 3.1415926535f);
        env = env * env;
        float flash_mul = 180.0f * env;

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float vy = (float)y / (float)height;
            float row_offset = vy * 0.25f - sweep;
            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

            for (uint32_t x = 0; x < width; ++x) {
                float vx = (float)x / (float)width;
                float dist = fabsf(vx + row_offset);

                uint32_t ca = a_row[x];
                uint32_t cb = b_row[x];
                uint32_t blended = blend_pixel_scalar(ca, cb, wb);

                if (dist < 0.56f && flash_mul > 0.01f) {
                    float flare_beam = expf(-dist * dist * 32.0f);
                    int flash = (int)(flare_beam * flash_mul);
                    dst_row[x] = flash > 0 ? add_saturate_rgb(blended, flash) : blended;
                } else {
                    dst_row[x] = blended;
                }
            }
        }
        return true;
    }

    case WAYWAL_TRANSITION_PAGE_CURL: {
        float R = 0.12f;
        float line = 1.02f - progress * 1.25f;

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float vy = (float)y / (float)height;
            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

            for (uint32_t x = 0; x < width; ++x) {
                float vx = (float)x / (float)width;
                float s = (vx + vy) * 0.5f;

                if (s < line) {
                    dst_row[x] = a_row[x];
                } else if (s < line + R) {
                    float dist = s - line;
                    float curl_u = vx - dist;
                    float curl_v = vy - dist;
                    if (curl_u >= 0.0f && curl_u <= 1.0f && curl_v >= 0.0f && curl_v <= 1.0f) {
                        int sx = (int)(curl_u * (float)width);
                        int sy = (int)(curl_v * (float)height);
                        if (sx >= (int)width)
                            sx = width - 1;
                        if (sy >= (int)height)
                            sy = height - 1;
                        uint32_t col = old_pixels[(size_t)sy * width + sx];
                        float curl_light = 0.85f + 0.35f * sinf((dist / R) * 3.1415926535f);
                        dst_row[x] = blend_pixel_scalar(
                            0xFF000000, col, (uint16_t)clampf(curl_light * 230.0f, 0.0f, 256.0f));
                    } else {
                        float shadow = 0.5f + 0.5f * smoothstepf(0.0f, R, dist);
                        uint32_t col = b_row[x];
                        dst_row[x] =
                            blend_pixel_scalar(0xFF000000, col, (uint16_t)(shadow * 256.0f));
                    }
                } else {
                    dst_row[x] = b_row[x];
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
    if (map_target && map_old && map_new) {
        ok = render_engine_execute_cpu_transition((uint32_t *)map_target, (const uint32_t *)map_old,
                                                  (const uint32_t *)map_new, target_bo->width,
                                                  target_bo->height, stride_target, params);
    }

    if (map_target)
        gbm_bo_unmap(target_bo->gbm_bo, map_data_target);
    if (map_old)
        gbm_bo_unmap(old_bo->gbm_bo, map_data_old);
    if (map_new)
        gbm_bo_unmap(new_bo->gbm_bo, map_data_new);

    return ok;
}


