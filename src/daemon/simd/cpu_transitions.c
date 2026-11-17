// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/render_engine.h"

#include "simd_blend.h"
#include "waywal/log.h"
#include "waywal/output_state.h"

#ifdef _OPENMP
#include <omp.h>
#endif
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
#include <cpuid.h>
#include <immintrin.h>
#endif

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
    float span = edge1 - edge0;
    if (span == 0.0f) {
        return x >= edge1 ? 1.0f : 0.0f;
    }
    float t = clampf((x - edge0) / span, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static inline uint32_t iq_hash21_u32(int32_t ix, int32_t iy)
{
    uint32_t qx = (uint32_t)ix * 1597334677u;
    uint32_t qy = (uint32_t)iy * 3812015801u;
    return (qx ^ qy) * 1597334677u;
}

static inline float iq_hash21_f(int32_t ix, int32_t iy)
{
    return (float)iq_hash21_u32(ix, iy) * (1.0f / 4294967296.0f);
}

static inline void iq_hash2_vec(int32_t ix, int32_t iy, float *out_gx, float *out_gy)
{
    uint32_t qx = (uint32_t)ix * 1597334677u;
    uint32_t qy = (uint32_t)iy * 3812015801u;
    uint32_t xq = qx ^ qy;
    uint32_t hx = xq * 1597334677u;
    uint32_t hy = xq * 3812015801u;
    *out_gx = -1.0f + 2.0f * ((float)hx * (1.0f / 4294967296.0f));
    *out_gy = -1.0f + 2.0f * ((float)hy * (1.0f / 4294967296.0f));
}

static inline void resolve_center(const waywal_transition_params_t *params, float *out_cx,
                                  float *out_cy)
{
    float cx = params->center_x;
    float cy = params->center_y;
    /* Negative coordinates serve as the unset sentinel so top-left (0.0, 0.0) stays valid. */
    if (!isfinite(cx) || !isfinite(cy) || cx < 0.0f || cy < 0.0f) {
        *out_cx = 0.5f;
        *out_cy = 0.5f;
    } else {
        *out_cx = cx;
        *out_cy = cy;
    }
}

static inline uint32_t blend_pixel_scalar(uint32_t a, uint32_t b, uint16_t wb)
{
    if (wb > 256)
        wb = 256;
    uint32_t wa = 256 - wb;
    uint32_t rb = (((a & 0x00FF00FFu) * wa) + ((b & 0x00FF00FFu) * wb)) >> 8;
    uint32_t g = (((a & 0x0000FF00u) * wa) + ((b & 0x0000FF00u) * wb)) >> 8;
    return (rb & 0x00FF00FFu) | (g & 0x0000FF00u) | 0xFF000000u;
}

static inline uint32_t blend_pixel_fast(uint32_t a, uint32_t b, uint32_t wa, uint32_t wb)
{
    uint32_t rb = (((a & 0x00FF00FFu) * wa) + ((b & 0x00FF00FFu) * wb)) >> 8;
    uint32_t g = (((a & 0x0000FF00u) * wa) + ((b & 0x0000FF00u) * wb)) >> 8;
    return (rb & 0x00FF00FFu) | (g & 0x0000FF00u) | 0xFF000000u;
}

static inline uint32_t avg_pixels_rounded(uint32_t c0, uint32_t c1)
{
    uint32_t r = (((c0 >> 16) & 0xFFu) + ((c1 >> 16) & 0xFFu) + 1u) >> 1;
    uint32_t g = (((c0 >> 8) & 0xFFu) + ((c1 >> 8) & 0xFFu) + 1u) >> 1;
    uint32_t b = ((c0 & 0xFFu) + (c1 & 0xFFu) + 1u) >> 1;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static inline uint32_t add_saturate_rgb(uint32_t c, int flash)
{
    int r = (int)((c >> 16) & 0xFFu) + flash;
    int g = (int)((c >> 8) & 0xFFu) + flash;
    int b = (int)(c & 0xFFu) + flash;
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
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline void fire_color_rgb(float t, float *out_r, float *out_g, float *out_b)
{
    float s0 = clampf(t * 4.0f, 0.0f, 1.0f);
    float s1 = clampf((t - 0.25f) * 2.5f, 0.0f, 1.0f);
    float s2 = clampf((t - 0.65f) * 2.857142857f, 0.0f, 1.0f);

    float r = 0.08f + (0.85f - 0.08f) * s0;
    float g = 0.03f + (0.10f - 0.03f) * s0;
    float b = 0.02f;

    r += (1.00f - r) * s1;
    g += (0.55f - g) * s1;
    b += (0.08f - b) * s1;

    r += (1.00f - r) * s2;
    g += (0.95f - g) * s2;
    b += (0.70f - b) * s2;

    *out_r = r;
    *out_g = g;
    *out_b = b;
}

/* Compare in float before casting to int to avoid CVTTSS2SI overflow on off-screen centers. */
static inline int span_floor_clamp(float v, int width)
{
    if (v <= 0.0f)
        return 0;
    if (v >= (float)width)
        return width;
    return (int)v;
}

static inline int span_ceil_clamp(float v, int width)
{
    float v_plus = v + 1.0f;
    if (v_plus <= 0.0f)
        return 0;
    if (v_plus >= (float)width)
        return width;
    return (int)v_plus;
}

static inline void fill_u32_span(uint32_t *restrict dst, uint32_t val, int count)
{
    for (int i = 0; i < count; ++i) {
        dst[i] = val;
    }
}

static inline void copy_shifted_span(uint32_t *restrict dst_row, const uint32_t *restrict src_row,
                                     int x_start, int x_end, int ishift, int width)
{
    if (x_start >= x_end)
        return;

    int left_limit = -ishift;
    if (x_start < left_limit) {
        int end0 = left_limit < x_end ? left_limit : x_end;
        fill_u32_span(dst_row + x_start, src_row[0], end0 - x_start);
        x_start = end0;
        if (x_start >= x_end)
            return;
    }

    int mid_limit = width - ishift;
    if (x_start < mid_limit) {
        int end1 = mid_limit < x_end ? mid_limit : x_end;
        memcpy(dst_row + x_start, src_row + (x_start + ishift),
               (size_t)(end1 - x_start) * sizeof(uint32_t));
        x_start = end1;
        if (x_start >= x_end)
            return;
    }

    fill_u32_span(dst_row + x_start, src_row[width - 1], x_end - x_start);
}

static inline void compute_swirl_chord(int tx0, int tx1, float cx_pixel, float y_rel2,
                                       float max_dist2, int *restrict out_x0, int *restrict out_x1)
{
    float rem = max_dist2 - y_rel2;
    if (rem <= 0.0f) {
        *out_x0 = tx0;
        *out_x1 = tx0;
        return;
    }
    float half_chord = sqrtf(rem);
    int x0 = (int)ceilf(cx_pixel - half_chord - 0.5f);
    int x1 = (int)floorf(cx_pixel + half_chord - 0.5f) + 1;
    if (x0 < tx0)
        x0 = tx0;
    if (x1 > tx1)
        x1 = tx1;
    while (x0 < x1) {
        float dx = ((float)x0 + 0.5f) - cx_pixel;
        if (dx * dx + y_rel2 < max_dist2)
            break;
        x0++;
    }
    while (x1 > x0) {
        float dx = ((float)(x1 - 1) + 0.5f) - cx_pixel;
        if (dx * dx + y_rel2 < max_dist2)
            break;
        x1--;
    }
    *out_x0 = x0;
    *out_x1 = x1;
}

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
__attribute__((target("xsave"))) static inline uint64_t read_xcr0_xsave(void) { return _xgetbv(0); }

static bool probe_avx2_fma_support(void)
{
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (!__get_cpuid(0, &eax, &ebx, &ecx, &edx) || eax < 7u) {
        return false;
    }
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        return false;
    }
    /* Verify XSAVE, OSXSAVE, AVX, and FMA before executing _xgetbv or VEX instructions. */
    const unsigned int need_ecx1 = (1u << 26) | (1u << 27) | (1u << 28) | (1u << 12);
    if ((ecx & need_ecx1) != need_ecx1) {
        return false;
    }
    if ((read_xcr0_xsave() & 0x6ULL) != 0x6ULL) {
        return false;
    }
    if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        return false;
    }
    return (ebx & (1u << 5)) != 0u;
}

static inline bool cpu_supports_avx2_fma(void)
{
    static _Atomic int s_cached = -1;
    int v = atomic_load_explicit(&s_cached, memory_order_acquire);
    if (__builtin_expect(v >= 0, 1)) {
        return v != 0;
    }
    bool supported = probe_avx2_fma_support();
    atomic_store_explicit(&s_cached, supported ? 1 : 0, memory_order_release);
    return supported;
}

__attribute__((target("avx2,fma"))) static inline __m256i
blend8_argb_var_avx2(__m256i v_ca, __m256i v_cb, __m256i v_wb32)
{
    const __m256i v_mask_rb = _mm256_set1_epi32(0x00FF00FF);
    const __m256i v_mask_g = _mm256_set1_epi32(0x0000FF00);
    const __m256i v_alpha = _mm256_set1_epi32((int)0xFF000000U);
    const __m256i v_256 = _mm256_set1_epi16(256);

    __m256i v_wb16 = _mm256_or_si256(v_wb32, _mm256_slli_epi32(v_wb32, 16));
    __m256i v_wa16 = _mm256_sub_epi16(v_256, v_wb16);

    __m256i a_rb = _mm256_and_si256(v_ca, v_mask_rb);
    __m256i b_rb = _mm256_and_si256(v_cb, v_mask_rb);
    __m256i a_ag = _mm256_srli_epi16(v_ca, 8);
    __m256i b_ag = _mm256_srli_epi16(v_cb, 8);

    __m256i rb = _mm256_srli_epi16(
        _mm256_add_epi16(_mm256_mullo_epi16(a_rb, v_wa16), _mm256_mullo_epi16(b_rb, v_wb16)), 8);
    __m256i g = _mm256_and_si256(
        _mm256_add_epi16(_mm256_mullo_epi16(a_ag, v_wa16), _mm256_mullo_epi16(b_ag, v_wb16)),
        v_mask_g);

    return _mm256_or_si256(_mm256_or_si256(rb, g), v_alpha);
}

__attribute__((target("avx2"))) static inline __m256i shade8_argb_const_avx2(__m256i v_c,
                                                                             __m256i v_wb16)
{
    const __m256i v_mask_rb = _mm256_set1_epi32(0x00FF00FF);
    const __m256i v_mask_g = _mm256_set1_epi32(0x0000FF00);
    const __m256i v_alpha = _mm256_set1_epi32((int)0xFF000000U);

    __m256i c_rb = _mm256_and_si256(v_c, v_mask_rb);
    __m256i c_ag = _mm256_srli_epi16(v_c, 8);

    __m256i rb = _mm256_srli_epi16(_mm256_mullo_epi16(c_rb, v_wb16), 8);
    __m256i g = _mm256_and_si256(_mm256_mullo_epi16(c_ag, v_wb16), v_mask_g);

    return _mm256_or_si256(_mm256_or_si256(rb, g), v_alpha);
}

/* Cody-Waite range reduction + degree-7/8 minimax polynomials keep sin/cos < 1 ULP with zero
 * gathers. */
__attribute__((target("avx2,fma"))) static inline void
sincos_avx2(__m256 v_theta, __m256 *restrict out_sin, __m256 *restrict out_cos)
{
    const __m256 v_two_over_pi = _mm256_set1_ps(0.63661977236758134f);
    const __m256 v_dp1 = _mm256_set1_ps(1.5703125f);
    const __m256 v_dp2 = _mm256_set1_ps(0.000483751297f);
    const __m256 v_dp3 = _mm256_set1_ps(7.549790126e-8f);
    const __m256 v_one = _mm256_set1_ps(1.0f);
    const __m256 v_neg_half = _mm256_set1_ps(-0.5f);
    const __m256 v_s1 = _mm256_set1_ps(-0.16666666666666666f);
    const __m256 v_s2 = _mm256_set1_ps(0.008333333333333333f);
    const __m256 v_s3 = _mm256_set1_ps(-0.0001984126984126984f);
    const __m256 v_s4 = _mm256_set1_ps(2.7557319223985893e-6f);
    const __m256 v_c1 = _mm256_set1_ps(0.041666666666666664f);
    const __m256 v_c2 = _mm256_set1_ps(-0.001388888888888889f);
    const __m256 v_c3 = _mm256_set1_ps(2.48015873015873e-5f);
    const __m256i v_one_i = _mm256_set1_epi32(1);
    const __m256i v_two_i = _mm256_set1_epi32(2);

    __m256 v_j = _mm256_round_ps(_mm256_mul_ps(v_theta, v_two_over_pi),
                                 _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    __m256 v_r = _mm256_fnmadd_ps(v_j, v_dp1, v_theta);
    v_r = _mm256_fnmadd_ps(v_j, v_dp2, v_r);
    v_r = _mm256_fnmadd_ps(v_j, v_dp3, v_r);
    __m256 v_r2 = _mm256_mul_ps(v_r, v_r);
    __m256 v_r3 = _mm256_mul_ps(v_r2, v_r);
    __m256 v_r4 = _mm256_mul_ps(v_r2, v_r2);

    __m256 v_sp = _mm256_fmadd_ps(v_s4, v_r2, v_s3);
    v_sp = _mm256_fmadd_ps(v_sp, v_r2, v_s2);
    v_sp = _mm256_fmadd_ps(v_sp, v_r2, v_s1);
    v_sp = _mm256_fmadd_ps(v_sp, v_r3, v_r);

    __m256 v_cp = _mm256_fmadd_ps(v_c3, v_r2, v_c2);
    v_cp = _mm256_fmadd_ps(v_cp, v_r2, v_c1);
    v_cp = _mm256_fmadd_ps(v_cp, v_r4, _mm256_fmadd_ps(v_neg_half, v_r2, v_one));

    __m256i v_k = _mm256_cvtps_epi32(v_j);
    __m256 v_swap = _mm256_castsi256_ps(_mm256_slli_epi32(v_k, 31));
    __m256 v_s1_out = _mm256_blendv_ps(v_sp, v_cp, v_swap);
    __m256 v_c1_out = _mm256_blendv_ps(v_cp, v_sp, v_swap);

    __m256 v_sin_sign = _mm256_castsi256_ps(_mm256_slli_epi32(_mm256_and_si256(v_k, v_two_i), 30));
    __m256 v_cos_sign = _mm256_castsi256_ps(
        _mm256_slli_epi32(_mm256_and_si256(_mm256_add_epi32(v_k, v_one_i), v_two_i), 30));

    *out_sin = _mm256_xor_ps(v_s1_out, v_sin_sign);
    *out_cos = _mm256_xor_ps(v_c1_out, v_cos_sign);
}

/* Compact degree-5/4 Cody-Waite trig keeps swirl error < 0.04 px with minimal YMM register spills.
 */
__attribute__((target("avx2,fma"))) static inline void
sincos_swirl_avx2(__m256 v_theta, __m256 *restrict out_sin, __m256 *restrict out_cos)
{
    const __m256 v_two_over_pi = _mm256_set1_ps(0.63661977236758134f);
    const __m256 v_dp1 = _mm256_set1_ps(1.5703125f);
    const __m256 v_dp2 = _mm256_set1_ps(0.000483751297f);
    __m256 v_j = _mm256_round_ps(_mm256_mul_ps(v_theta, v_two_over_pi),
                                 _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    __m256 v_r = _mm256_fnmadd_ps(v_j, v_dp1, v_theta);
    v_r = _mm256_fnmadd_ps(v_j, v_dp2, v_r);
    __m256 v_r2 = _mm256_mul_ps(v_r, v_r);
    __m256 v_r3 = _mm256_mul_ps(v_r2, v_r);
    __m256 v_r4 = _mm256_mul_ps(v_r2, v_r2);

    __m256 v_sp =
        _mm256_fmadd_ps(_mm256_set1_ps(0.0083333333f), v_r2, _mm256_set1_ps(-0.16666667f));
    v_sp = _mm256_fmadd_ps(v_sp, v_r3, v_r);

    __m256 v_cp =
        _mm256_fmadd_ps(_mm256_set1_ps(0.041666667f), v_r4,
                        _mm256_fmadd_ps(_mm256_set1_ps(-0.5f), v_r2, _mm256_set1_ps(1.0f)));

    __m256i v_k = _mm256_cvtps_epi32(v_j);
    __m256 v_swap = _mm256_castsi256_ps(_mm256_slli_epi32(v_k, 31));
    __m256 v_s1 = _mm256_blendv_ps(v_sp, v_cp, v_swap);
    __m256 v_c1 = _mm256_blendv_ps(v_cp, v_sp, v_swap);

    const __m256i v_two_i = _mm256_set1_epi32(2);
    __m256 v_sin_sign = _mm256_castsi256_ps(_mm256_slli_epi32(_mm256_and_si256(v_k, v_two_i), 30));
    __m256 v_cos_sign = _mm256_castsi256_ps(_mm256_slli_epi32(
        _mm256_and_si256(_mm256_add_epi32(v_k, _mm256_set1_epi32(1)), v_two_i), 30));

    *out_sin = _mm256_xor_ps(v_s1, v_sin_sign);
    *out_cos = _mm256_xor_ps(v_c1, v_cos_sign);
}

/* Split each 64x64 tile into direct 8-px trig+prefetch and SWAR blend so Zen 3 avoids
 * microcoded vpgatherdd and serial vpinsrd stalls, bypassing outer chord spans with SIMD. */
__attribute__((target("avx2,fma"))) static void
swirl_tile_avx2(uint32_t *restrict dst, const uint32_t *restrict old_pixels,
                const uint32_t *restrict new_pixels, int tx0, int tx1, int ty0, int ty1, int width,
                int height, int stride_pixels, float cx_pixel, float cy_pixel, float inv_max_dist,
                float max_dist2, float angle_amt, uint32_t wa, uint32_t wb, bool interior_only,
                simd_blend_scanline_fn blend_fn)
{
    int offs[64 * 64] __attribute__((aligned(32)));
    int row_x0[64];
    int row_x1[64];
    const int tw = tx1 - tx0;
    const int th = ty1 - ty0;

    const __m256 v_cx = _mm256_set1_ps(cx_pixel);
    const __m256 v_cy = _mm256_set1_ps(cy_pixel);
    const __m256 v_inv_max_dist = _mm256_set1_ps(inv_max_dist);
    const __m256 v_angle_amt = _mm256_set1_ps(angle_amt);
    const __m256 v_zero = _mm256_setzero_ps();
    const __m256 v_one = _mm256_set1_ps(1.0f);
    const __m256 v_eps = _mm256_set1_ps(1e-12f);
    const __m256 v_half = _mm256_set1_ps(0.5f);
    const __m256 v_three_half = _mm256_set1_ps(1.5f);
    const __m256 v_eight = _mm256_set1_ps(8.0f);
    const __m256i v_zero_i = _mm256_setzero_si256();
    const __m256i v_max_x = _mm256_set1_epi32(width - 1);
    const __m256i v_max_y = _mm256_set1_epi32(height - 1);
    const __m256i v_width = _mm256_set1_epi32(width);

    const bool read_old = (wa != 0U);
    const bool read_new = (wb != 0U);

    for (int ry = 0; ry < th; ++ry) {
        int y = ty0 + ry;
        const float y_rel = ((float)y + 0.5f) - cy_pixel;
        const float y_rel2 = y_rel * y_rel;

        int xin0 = tx0;
        int xin1 = tx1;
        if (!interior_only) {
            compute_swirl_chord(tx0, tx1, cx_pixel, y_rel2, max_dist2, &xin0, &xin1);
        }
        row_x0[ry] = xin0;
        row_x1[ry] = xin1;

        if (xin0 >= xin1) {
            continue;
        }

        const __m256 v_y_rel = _mm256_set1_ps(y_rel);
        const __m256 v_y_rel2 = _mm256_set1_ps(y_rel2);
        const float x0_rel = ((float)xin0 + 0.5f) - cx_pixel;
        __m256 vx_rel = _mm256_set_ps(x0_rel + 7.0f, x0_rel + 6.0f, x0_rel + 5.0f, x0_rel + 4.0f,
                                      x0_rel + 3.0f, x0_rel + 2.0f, x0_rel + 1.0f, x0_rel);

        int *orow = &offs[ry * tw + (xin0 - tx0)];
        int span_w = xin1 - xin0;
        int lx = 0;
        for (; lx + 8 <= span_w; lx += 8) {
            __m256 vd2 = _mm256_fmadd_ps(vx_rel, vx_rel, v_y_rel2);
            __m256 vr = _mm256_rsqrt_ps(_mm256_max_ps(vd2, v_eps));
            vr = _mm256_mul_ps(vr, _mm256_fnmadd_ps(_mm256_mul_ps(vd2, vr),
                                                    _mm256_mul_ps(v_half, vr), v_three_half));
            __m256 v_dist = _mm256_mul_ps(vd2, vr);
            __m256 v_factor =
                _mm256_max_ps(v_zero, _mm256_fnmadd_ps(v_dist, v_inv_max_dist, v_one));
            __m256 v_theta = _mm256_mul_ps(_mm256_mul_ps(v_factor, v_factor), v_angle_amt);
            __m256 vsin, vcos;
            sincos_swirl_avx2(v_theta, &vsin, &vcos);

            __m256 vpx =
                _mm256_add_ps(v_cx, _mm256_fmsub_ps(vx_rel, vcos, _mm256_mul_ps(v_y_rel, vsin)));
            __m256 vpy =
                _mm256_add_ps(v_cy, _mm256_fmadd_ps(vx_rel, vsin, _mm256_mul_ps(v_y_rel, vcos)));

            __m256i vsx =
                _mm256_max_epi32(v_zero_i, _mm256_min_epi32(_mm256_cvttps_epi32(vpx), v_max_x));
            __m256i vsy =
                _mm256_max_epi32(v_zero_i, _mm256_min_epi32(_mm256_cvttps_epi32(vpy), v_max_y));
            __m256i voff = _mm256_add_epi32(_mm256_mullo_epi32(vsy, v_width), vsx);

            _mm256_storeu_si256((__m256i *)&orow[lx], voff);

            __m128i lo = _mm256_castsi256_si128(voff);
            __m128i hi = _mm256_extracti128_si256(voff, 1);
            int i0 = _mm_cvtsi128_si32(lo);
            int i2 = _mm_extract_epi32(lo, 2);
            int i4 = _mm_cvtsi128_si32(hi);
            int i6 = _mm_extract_epi32(hi, 2);
            if (read_old) {
                _mm_prefetch((const char *)&old_pixels[i0], _MM_HINT_T0);
                _mm_prefetch((const char *)&old_pixels[i2], _MM_HINT_T0);
                _mm_prefetch((const char *)&old_pixels[i4], _MM_HINT_T0);
                _mm_prefetch((const char *)&old_pixels[i6], _MM_HINT_T0);
            }
            if (read_new) {
                _mm_prefetch((const char *)&new_pixels[i0], _MM_HINT_T0);
                _mm_prefetch((const char *)&new_pixels[i2], _MM_HINT_T0);
                _mm_prefetch((const char *)&new_pixels[i4], _MM_HINT_T0);
                _mm_prefetch((const char *)&new_pixels[i6], _MM_HINT_T0);
            }

            vx_rel = _mm256_add_ps(vx_rel, v_eight);
        }

        for (; lx < span_w; ++lx) {
            int x = xin0 + lx;
            float x_rel = ((float)x + 0.5f) - cx_pixel;
            float d2 = x_rel * x_rel + y_rel2;
            float dist = sqrtf(d2);
            float factor = 1.0f - dist * inv_max_dist;
            if (factor < 0.0f)
                factor = 0.0f;
            float theta = factor * factor * angle_amt;
            float sin_t = sinf(theta);
            float cos_t = cosf(theta);
            float px = cx_pixel + x_rel * cos_t - y_rel * sin_t;
            float py = cy_pixel + x_rel * sin_t + y_rel * cos_t;
            int sx = (int)px;
            int sy = (int)py;
            if (sx < 0)
                sx = 0;
            else if (sx >= width)
                sx = width - 1;
            if (sy < 0)
                sy = 0;
            else if (sy >= height)
                sy = height - 1;
            orow[lx] = sy * width + sx;
        }
    }

    for (int ry = 0; ry < th; ++ry) {
        int y = ty0 + ry;
        uint32_t *dst_row = dst + (size_t)y * (size_t)stride_pixels;
        const uint32_t *a_row = old_pixels + (size_t)y * (size_t)width;
        const uint32_t *b_row = new_pixels + (size_t)y * (size_t)width;

        int xin0 = row_x0[ry];
        int xin1 = row_x1[ry];
        if (xin0 > tx0) {
            blend_fn((uint8_t *)(dst_row + tx0), (const uint8_t *)(a_row + tx0),
                     (const uint8_t *)(b_row + tx0), (uint16_t)wb,
                     (size_t)(xin0 - tx0) * sizeof(uint32_t));
        }

        if (xin0 < xin1) {
            const int *p = &offs[ry * tw + (xin0 - tx0)];
            int x = xin0;

            if (!read_new) {
                while (x < xin1 && (((uintptr_t)&dst_row[x] & 31u) != 0u)) {
                    dst_row[x++] = old_pixels[*p++] | 0xFF000000U;
                }
                for (; x + 8 <= xin1; x += 8, p += 8) {
                    uint32_t out[8] __attribute__((aligned(32)));
                    for (int k = 0; k < 8; ++k) {
                        out[k] = old_pixels[p[k]] | 0xFF000000U;
                    }
                    _mm256_stream_si256((__m256i *)&dst_row[x],
                                        _mm256_load_si256((const __m256i *)out));
                }
                for (; x < xin1; ++x, ++p) {
                    dst_row[x] = old_pixels[*p] | 0xFF000000U;
                }
            } else if (!read_old) {
                while (x < xin1 && (((uintptr_t)&dst_row[x] & 31u) != 0u)) {
                    dst_row[x++] = new_pixels[*p++] | 0xFF000000U;
                }
                for (; x + 8 <= xin1; x += 8, p += 8) {
                    uint32_t out[8] __attribute__((aligned(32)));
                    for (int k = 0; k < 8; ++k) {
                        out[k] = new_pixels[p[k]] | 0xFF000000U;
                    }
                    _mm256_stream_si256((__m256i *)&dst_row[x],
                                        _mm256_load_si256((const __m256i *)out));
                }
                for (; x < xin1; ++x, ++p) {
                    dst_row[x] = new_pixels[*p] | 0xFF000000U;
                }
            } else {
                while (x < xin1 && (((uintptr_t)&dst_row[x] & 31u) != 0u)) {
                    int off = *p++;
                    dst_row[x++] = blend_pixel_fast(old_pixels[off], new_pixels[off], wa, wb);
                }
                for (; x + 8 <= xin1; x += 8, p += 8) {
                    uint32_t out[8] __attribute__((aligned(32)));
                    for (int k = 0; k < 8; ++k) {
                        int off = p[k];
                        out[k] = blend_pixel_fast(old_pixels[off], new_pixels[off], wa, wb);
                    }
                    _mm256_stream_si256((__m256i *)&dst_row[x],
                                        _mm256_load_si256((const __m256i *)out));
                }
                for (; x < xin1; ++x, ++p) {
                    int off = *p;
                    dst_row[x] = blend_pixel_fast(old_pixels[off], new_pixels[off], wa, wb);
                }
            }
        }

        if (xin1 < tx1) {
            blend_fn((uint8_t *)(dst_row + xin1), (const uint8_t *)(a_row + xin1),
                     (const uint8_t *)(b_row + xin1), (uint16_t)wb,
                     (size_t)(tx1 - xin1) * sizeof(uint32_t));
        }
    }
}

__attribute__((target("avx2,fma"))) static int
wipe_seam_avx2(uint32_t *restrict dst_row, const uint32_t *restrict a_row,
               const uint32_t *restrict b_row, int x_start, int x_end, float proj_0, float d_proj,
               float p_plus_edge, float inv_2edge)
{
    __m256 v_dproj8 = _mm256_set1_ps(8.0f * d_proj);
    __m256 v_p_edge = _mm256_set1_ps(p_plus_edge);
    __m256 v_inv_2e = _mm256_set1_ps(inv_2edge);
    __m256 v_zero = _mm256_setzero_ps();
    __m256 v_one = _mm256_set1_ps(1.0f);
    __m256 v_two = _mm256_set1_ps(2.0f);
    __m256 v_three = _mm256_set1_ps(3.0f);
    __m256 v_256 = _mm256_set1_ps(256.0f);

    float p0 = proj_0 + (float)x_start * d_proj;
    __m256 vproj = _mm256_set_ps(p0 + 7.0f * d_proj, p0 + 6.0f * d_proj, p0 + 5.0f * d_proj,
                                 p0 + 4.0f * d_proj, p0 + 3.0f * d_proj, p0 + 2.0f * d_proj,
                                 p0 + 1.0f * d_proj, p0);

    int x = x_start;
    for (; x + 8 <= x_end; x += 8) {
        __m256 vt = _mm256_min_ps(
            v_one, _mm256_max_ps(v_zero, _mm256_mul_ps(_mm256_sub_ps(v_p_edge, vproj), v_inv_2e)));
        __m256 valpha = _mm256_mul_ps(_mm256_mul_ps(vt, vt), _mm256_fnmadd_ps(v_two, vt, v_three));
        __m256i v_wb = _mm256_cvttps_epi32(_mm256_mul_ps(valpha, v_256));

        __m256i v_ca = _mm256_loadu_si256((const __m256i *)&a_row[x]);
        __m256i v_cb = _mm256_loadu_si256((const __m256i *)&b_row[x]);
        _mm256_storeu_si256((__m256i *)&dst_row[x], blend8_argb_var_avx2(v_ca, v_cb, v_wb));

        vproj = _mm256_add_ps(vproj, v_dproj8);
    }
    return x;
}

__attribute__((target("avx2,fma"))) static int
radial_annulus_avx2(uint32_t *restrict dst_row, const uint32_t *restrict a_row,
                    const uint32_t *restrict b_row, int x_start, int x_end, float cx, float step_dx,
                    float dy2, float r_ref, float signed_inv_2edge)
{
    __m256 v_step8 = _mm256_set1_ps(8.0f * step_dx);
    __m256 v_dy2 = _mm256_set1_ps(dy2);
    __m256 v_r_ref = _mm256_set1_ps(r_ref);
    __m256 v_scale = _mm256_set1_ps(signed_inv_2edge);
    __m256 v_zero = _mm256_setzero_ps();
    __m256 v_one = _mm256_set1_ps(1.0f);
    __m256 v_two = _mm256_set1_ps(2.0f);
    __m256 v_three = _mm256_set1_ps(3.0f);
    __m256 v_256 = _mm256_set1_ps(256.0f);

    float dx0 = ((float)x_start + 0.5f) * step_dx - cx;
    __m256 vdx = _mm256_set_ps(dx0 + 7.0f * step_dx, dx0 + 6.0f * step_dx, dx0 + 5.0f * step_dx,
                               dx0 + 4.0f * step_dx, dx0 + 3.0f * step_dx, dx0 + 2.0f * step_dx,
                               dx0 + 1.0f * step_dx, dx0);

    int x = x_start;
    for (; x + 8 <= x_end; x += 8) {
        __m256 vdist2 = _mm256_fmadd_ps(vdx, vdx, v_dy2);
        __m256 vdist = _mm256_sqrt_ps(vdist2);
        __m256 vt = _mm256_min_ps(
            v_one, _mm256_max_ps(v_zero, _mm256_mul_ps(_mm256_sub_ps(vdist, v_r_ref), v_scale)));
        __m256 valpha = _mm256_mul_ps(_mm256_mul_ps(vt, vt), _mm256_fnmadd_ps(v_two, vt, v_three));
        __m256i v_wb = _mm256_cvttps_epi32(_mm256_mul_ps(valpha, v_256));

        __m256i v_ca = _mm256_loadu_si256((const __m256i *)&a_row[x]);
        __m256i v_cb = _mm256_loadu_si256((const __m256i *)&b_row[x]);
        _mm256_storeu_si256((__m256i *)&dst_row[x], blend8_argb_var_avx2(v_ca, v_cb, v_wb));

        vdx = _mm256_add_ps(vdx, v_step8);
    }
    return x;
}

__attribute__((target("avx2,fma"))) static int
ripple_annulus_avx2(uint32_t *restrict dst_row, const uint32_t *restrict old_pixels,
                    const uint32_t *restrict new_pixels, int x_start, int x_end, int y,
                    float cx_scaled, float step_dx, float dy, float dy2, float current_radius,
                    float wave_width, float freq, float amp, float decay, int width, int height)
{
    __m256 v_step_dx8 = _mm256_set1_ps(8.0f * step_dx);
    __m256 v_eight = _mm256_set1_ps(8.0f);
    __m256 v_dy = _mm256_set1_ps(dy);
    __m256 v_dy2 = _mm256_set1_ps(dy2);
    __m256 v_cur_r = _mm256_set1_ps(current_radius);
    __m256 v_inv_ww = _mm256_set1_ps(1.0f / wave_width);
    __m256 v_freq = _mm256_set1_ps(freq);
    __m256 v_amp_decay = _mm256_set1_ps(amp * decay);
    __m256 v_crest_scale = _mm256_set1_ps(decay * decay * 0.22f * 255.0f);
    __m256 v_inv_aspect = _mm256_set1_ps((float)height / (float)width);
    __m256 v_w = _mm256_set1_ps((float)width);
    __m256 v_h = _mm256_set1_ps((float)height);
    __m256 v_vy = _mm256_set1_ps(((float)y + 0.5f) / (float)height);
    __m256 v_eps_dist = _mm256_set1_ps(1e-8f);
    __m256 v_sign_mask = _mm256_set1_ps(-0.0f);
    __m256 v_zero = _mm256_setzero_ps();
    __m256 v_one = _mm256_set1_ps(1.0f);
    __m256 v_two = _mm256_set1_ps(2.0f);
    __m256 v_three = _mm256_set1_ps(3.0f);
    __m256 v_002 = _mm256_set1_ps(0.02f);
    __m256 v_25 = _mm256_set1_ps(25.0f);
    __m256 v_256 = _mm256_set1_ps(256.0f);
    __m256i v_zero_i = _mm256_setzero_si256();
    __m256i v_max_x = _mm256_set1_epi32(width - 1);
    __m256i v_max_y = _mm256_set1_epi32(height - 1);
    __m256i v_width_i = _mm256_set1_epi32(width);
    __m256i v_alpha = _mm256_set1_epi32((int)0xFF000000U);

    float dx0 = ((float)x_start + 0.5f) * step_dx - cx_scaled;
    __m256 vdx = _mm256_set_ps(dx0 + 7.0f * step_dx, dx0 + 6.0f * step_dx, dx0 + 5.0f * step_dx,
                               dx0 + 4.0f * step_dx, dx0 + 3.0f * step_dx, dx0 + 2.0f * step_dx,
                               dx0 + 1.0f * step_dx, dx0);
    float x0 = (float)x_start + 0.5f;
    __m256 vx = _mm256_set_ps(x0 + 7.0f, x0 + 6.0f, x0 + 5.0f, x0 + 4.0f, x0 + 3.0f, x0 + 2.0f,
                              x0 + 1.0f, x0);
    __m256 v_inv_w = _mm256_set1_ps(1.0f / (float)width);

    int x = x_start;
    for (; x + 8 <= x_end; x += 8) {
        __m256 vdist2 = _mm256_fmadd_ps(vdx, vdx, v_dy2);
        __m256 vdist = _mm256_sqrt_ps(vdist2);
        __m256 v_valid = _mm256_cmp_ps(vdist2, v_eps_dist, _CMP_GT_OQ);
        __m256 v_inv_dist =
            _mm256_and_ps(_mm256_div_ps(v_one, _mm256_max_ps(vdist, v_eps_dist)), v_valid);

        __m256 vdiff = _mm256_sub_ps(vdist, v_cur_r);
        __m256 vabs_diff = _mm256_andnot_ps(v_sign_mask, vdiff);
        __m256 v_wt = _mm256_min_ps(v_one, _mm256_mul_ps(vabs_diff, v_inv_ww));
        __m256 v_sm =
            _mm256_mul_ps(_mm256_mul_ps(v_wt, v_wt), _mm256_fnmadd_ps(v_two, v_wt, v_three));
        __m256 v_window = _mm256_sub_ps(v_one, v_sm);

        __m256 vsin, vcos;
        sincos_avx2(_mm256_mul_ps(vdiff, v_freq), &vsin, &vcos);
        (void)vcos;

        __m256 v_sin_win = _mm256_mul_ps(vsin, v_window);
        __m256 vwave = _mm256_mul_ps(v_sin_win, v_amp_decay);
        __m256 vcrest_f =
            _mm256_mul_ps(_mm256_max_ps(v_zero, _mm256_sub_ps(v_zero, v_sin_win)), v_crest_scale);

        __m256 v_ndx = _mm256_mul_ps(_mm256_mul_ps(vdx, v_inv_dist), v_inv_aspect);
        __m256 v_ndy = _mm256_mul_ps(v_dy, v_inv_dist);

        __m256 v_sx_f =
            _mm256_mul_ps(_mm256_fmadd_ps(v_ndx, vwave, _mm256_mul_ps(vx, v_inv_w)), v_w);
        __m256 v_sy_f = _mm256_mul_ps(_mm256_fmadd_ps(v_ndy, vwave, v_vy), v_h);

        __m256i vsx =
            _mm256_max_epi32(v_zero_i, _mm256_min_epi32(_mm256_cvttps_epi32(v_sx_f), v_max_x));
        __m256i vsy =
            _mm256_max_epi32(v_zero_i, _mm256_min_epi32(_mm256_cvttps_epi32(v_sy_f), v_max_y));
        __m256i v_off = _mm256_add_epi32(_mm256_mullo_epi32(vsy, v_width_i), vsx);

        __m256i v_ca = _mm256_i32gather_epi32((const int *)old_pixels, v_off, 4);
        __m256i v_cb = _mm256_i32gather_epi32((const int *)new_pixels, v_off, 4);

        __m256 vt = _mm256_min_ps(
            v_one, _mm256_max_ps(v_zero, _mm256_mul_ps(_mm256_add_ps(vdiff, v_002), v_25)));
        __m256 vsplit = _mm256_mul_ps(_mm256_mul_ps(vt, vt), _mm256_fnmadd_ps(v_two, vt, v_three));
        __m256i v_wa = _mm256_cvttps_epi32(_mm256_mul_ps(vsplit, v_256));

        __m256i v_blended = blend8_argb_var_avx2(v_cb, v_ca, v_wa);
        __m256i v_crest_i = _mm256_cvttps_epi32(vcrest_f);
        __m256i v_crest_px =
            _mm256_or_si256(_mm256_mullo_epi32(v_crest_i, _mm256_set1_epi32(0x00010101)), v_alpha);
        _mm256_storeu_si256((__m256i *)&dst_row[x], _mm256_adds_epu8(v_blended, v_crest_px));

        vdx = _mm256_add_ps(vdx, v_step_dx8);
        vx = _mm256_add_ps(vx, v_eight);
    }
    return x;
}

__attribute__((target("avx2,fma"))) static int
wave_band_avx2(uint32_t *restrict dst_row, const uint32_t *restrict a_row,
               const uint32_t *restrict b_row, const float *restrict sine_lut, int x_start,
               int x_end, float proj_0, float d_proj, float phase_0, float d_phase,
               float p_plus_edge, float inv_2edge)
{
    __m256 v_dproj8 = _mm256_set1_ps(8.0f * d_proj);
    __m256 v_dphase8 = _mm256_set1_ps(8.0f * d_phase);
    __m256 v_p_edge = _mm256_set1_ps(p_plus_edge);
    __m256 v_inv_2e = _mm256_set1_ps(inv_2edge);
    __m256 v_zero = _mm256_setzero_ps();
    __m256 v_one = _mm256_set1_ps(1.0f);
    __m256 v_two = _mm256_set1_ps(2.0f);
    __m256 v_three = _mm256_set1_ps(3.0f);
    __m256 v_256 = _mm256_set1_ps(256.0f);
    __m256i v_mask_1023 = _mm256_set1_epi32(1023);

    float pr0 = proj_0 + (float)x_start * d_proj;
    float ph0 = phase_0 + (float)x_start * d_phase;
    __m256 vproj = _mm256_set_ps(pr0 + 7.0f * d_proj, pr0 + 6.0f * d_proj, pr0 + 5.0f * d_proj,
                                 pr0 + 4.0f * d_proj, pr0 + 3.0f * d_proj, pr0 + 2.0f * d_proj,
                                 pr0 + 1.0f * d_proj, pr0);
    __m256 vphase = _mm256_set_ps(ph0 + 7.0f * d_phase, ph0 + 6.0f * d_phase, ph0 + 5.0f * d_phase,
                                  ph0 + 4.0f * d_phase, ph0 + 3.0f * d_phase, ph0 + 2.0f * d_phase,
                                  ph0 + 1.0f * d_phase, ph0);

    int x = x_start;
    for (; x + 8 <= x_end; x += 8) {
        __m256i vidx = _mm256_and_si256(_mm256_cvttps_epi32(_mm256_floor_ps(vphase)), v_mask_1023);
        __m256 vwave = _mm256_i32gather_ps(sine_lut, vidx, 4);
        __m256 vd = _mm256_add_ps(vproj, vwave);

        __m256 vt = _mm256_min_ps(
            v_one, _mm256_max_ps(v_zero, _mm256_mul_ps(_mm256_sub_ps(v_p_edge, vd), v_inv_2e)));
        __m256 valpha = _mm256_mul_ps(_mm256_mul_ps(vt, vt), _mm256_fnmadd_ps(v_two, vt, v_three));
        __m256i v_wb = _mm256_cvttps_epi32(_mm256_mul_ps(valpha, v_256));

        __m256i v_ca = _mm256_loadu_si256((const __m256i *)&a_row[x]);
        __m256i v_cb = _mm256_loadu_si256((const __m256i *)&b_row[x]);
        _mm256_storeu_si256((__m256i *)&dst_row[x], blend8_argb_var_avx2(v_ca, v_cb, v_wb));

        vproj = _mm256_add_ps(vproj, v_dproj8);
        vphase = _mm256_add_ps(vphase, v_dphase8);
    }
    return x;
}

__attribute__((target("avx2,fma"))) static int
cube_face_span_avx2(uint32_t *restrict dst_row, const uint32_t *restrict tex_pixels,
                    const int *restrict col_sx, const float *restrict col_ky, int x_start,
                    int x_end, float y_scale, float y_bias, int width, int max_y, uint16_t wb)
{
    __m256 v_yscale = _mm256_set1_ps(y_scale);
    __m256 v_ybias = _mm256_set1_ps(y_bias);
    __m256i v_zero = _mm256_setzero_si256();
    __m256i v_max_y = _mm256_set1_epi32(max_y);
    __m256i v_width = _mm256_set1_epi32(width);
    __m256i v_wb16 = _mm256_set1_epi16((short)wb);

    int x = x_start;
    for (; x + 8 <= x_end; x += 8) {
        __m256 v_ky = _mm256_loadu_ps(&col_ky[x]);
        __m256i v_sx = _mm256_loadu_si256((const __m256i *)&col_sx[x]);
        __m256 v_sy_f = _mm256_fmadd_ps(v_ky, v_yscale, v_ybias);
        __m256i v_sy =
            _mm256_max_epi32(v_zero, _mm256_min_epi32(_mm256_cvttps_epi32(v_sy_f), v_max_y));
        __m256i v_off = _mm256_add_epi32(_mm256_mullo_epi32(v_sy, v_width), v_sx);
        __m256i v_c = _mm256_i32gather_epi32((const int *)tex_pixels, v_off, 4);

        _mm256_storeu_si256((__m256i *)&dst_row[x], shade8_argb_const_avx2(v_c, v_wb16));
    }
    return x;
}

__attribute__((target("avx2,fma"))) static void
noise_row_avx2(uint32_t *restrict dst_row, const uint32_t *restrict a_row,
               const uint32_t *restrict b_row, const float *restrict v, const float *restrict dv,
               const int *restrict lut_ix, const float *restrict lut_cx, uint32_t width,
               float p_plus_edge, float two_edge, float inv_2edge)
{
    const __m256 v_p_edge = _mm256_set1_ps(p_plus_edge);
    const __m256 v_2edge = _mm256_set1_ps(two_edge);
    const __m256 v_inv_2edge = _mm256_set1_ps(inv_2edge);
    const __m256 v_zero_ps = _mm256_setzero_ps();
    const __m256 v_one_ps = _mm256_set1_ps(1.0f);
    const __m256 v_two_ps = _mm256_set1_ps(2.0f);
    const __m256 v_three_ps = _mm256_set1_ps(3.0f);
    const __m256 v_256_ps = _mm256_set1_ps(256.0f);

    uint32_t x = 0;
    for (; x + 8 <= width; x += 8) {
        __m256i v_ix = _mm256_loadu_si256((const __m256i *)&lut_ix[x]);
        __m256 v_cx = _mm256_loadu_ps(&lut_cx[x]);
        __m256 v_v0 = _mm256_i32gather_ps(v, v_ix, 4);
        __m256 v_dv = _mm256_i32gather_ps(dv, v_ix, 4);
        __m256 v_n = _mm256_fmadd_ps(v_dv, v_cx, v_v0);
        __m256 v_diff = _mm256_sub_ps(v_p_edge, v_n);

        if (_mm256_movemask_ps(_mm256_cmp_ps(v_diff, v_zero_ps, _CMP_GT_OQ)) == 0) {
            _mm256_storeu_si256((__m256i *)&dst_row[x],
                                _mm256_loadu_si256((const __m256i *)&a_row[x]));
            continue;
        }
        if (_mm256_movemask_ps(_mm256_cmp_ps(v_diff, v_2edge, _CMP_LT_OQ)) == 0) {
            _mm256_storeu_si256((__m256i *)&dst_row[x],
                                _mm256_loadu_si256((const __m256i *)&b_row[x]));
            continue;
        }

        __m256 v_t =
            _mm256_min_ps(_mm256_max_ps(_mm256_mul_ps(v_diff, v_inv_2edge), v_zero_ps), v_one_ps);
        __m256 v_alpha =
            _mm256_mul_ps(_mm256_mul_ps(v_t, v_t), _mm256_fnmadd_ps(v_two_ps, v_t, v_three_ps));
        __m256i v_wb_32 = _mm256_cvttps_epi32(_mm256_mul_ps(v_alpha, v_256_ps));

        __m256i v_ca = _mm256_loadu_si256((const __m256i *)&a_row[x]);
        __m256i v_cb = _mm256_loadu_si256((const __m256i *)&b_row[x]);
        _mm256_storeu_si256((__m256i *)&dst_row[x], blend8_argb_var_avx2(v_ca, v_cb, v_wb_32));
    }

    for (; x < width; ++x) {
        int ix = lut_ix[x];
        float n = v[ix] + dv[ix] * lut_cx[x];
        float diff = p_plus_edge - n;
        if (diff <= 0.0f) {
            dst_row[x] = a_row[x];
        } else if (diff >= two_edge) {
            dst_row[x] = b_row[x];
        } else {
            float t = diff * inv_2edge;
            float alpha = t * t * (3.0f - 2.0f * t);
            uint16_t wb = (uint16_t)(alpha * 256.0f);
            dst_row[x] = blend_pixel_scalar(a_row[x], b_row[x], wb);
        }
    }
}

__attribute__((target("avx2,fma"))) static inline __m256
iq_grad_dot_avx2(__m256i v_ix, __m256i v_iy, __m256 v_dx, __m256 v_dy)
{
    const __m256i v_c1 = _mm256_set1_epi32(1597334677);
    const __m256i v_c2 = _mm256_set1_epi32((int)3812015801u);
    const __m256i v_bias = _mm256_set1_epi32((int)0x80000000u);
    const __m256 v_inv_2p31 = _mm256_set1_ps(1.0f / 2147483648.0f);

    __m256i v_xq = _mm256_xor_si256(_mm256_mullo_epi32(v_ix, v_c1), _mm256_mullo_epi32(v_iy, v_c2));
    __m256i v_hx = _mm256_xor_si256(_mm256_mullo_epi32(v_xq, v_c1), v_bias);
    __m256i v_hy = _mm256_xor_si256(_mm256_mullo_epi32(v_xq, v_c2), v_bias);
    __m256 v_gx = _mm256_mul_ps(_mm256_cvtepi32_ps(v_hx), v_inv_2p31);
    __m256 v_gy = _mm256_mul_ps(_mm256_cvtepi32_ps(v_hy), v_inv_2p31);
    return _mm256_fmadd_ps(v_gx, v_dx, _mm256_mul_ps(v_gy, v_dy));
}

__attribute__((target("avx2,fma"))) static inline __m256 iq_noise2_avx2(__m256 v_px, __m256 v_py)
{
    const __m256 v_one = _mm256_set1_ps(1.0f);
    const __m256 v_two = _mm256_set1_ps(2.0f);
    const __m256 v_three = _mm256_set1_ps(3.0f);
    const __m256i v_one_i = _mm256_set1_epi32(1);

    __m256 v_ixf = _mm256_floor_ps(v_px);
    __m256 v_iyf = _mm256_floor_ps(v_py);
    __m256 v_fx = _mm256_sub_ps(v_px, v_ixf);
    __m256 v_fy = _mm256_sub_ps(v_py, v_iyf);
    __m256 v_fx1 = _mm256_sub_ps(v_fx, v_one);
    __m256 v_fy1 = _mm256_sub_ps(v_fy, v_one);

    __m256 v_ux = _mm256_mul_ps(_mm256_mul_ps(v_fx, v_fx), _mm256_fnmadd_ps(v_two, v_fx, v_three));
    __m256 v_uy = _mm256_mul_ps(_mm256_mul_ps(v_fy, v_fy), _mm256_fnmadd_ps(v_two, v_fy, v_three));

    __m256i v_ix0 = _mm256_cvtps_epi32(v_ixf);
    __m256i v_iy0 = _mm256_cvtps_epi32(v_iyf);
    __m256i v_ix1 = _mm256_add_epi32(v_ix0, v_one_i);
    __m256i v_iy1 = _mm256_add_epi32(v_iy0, v_one_i);

    __m256 v_d00 = iq_grad_dot_avx2(v_ix0, v_iy0, v_fx, v_fy);
    __m256 v_d10 = iq_grad_dot_avx2(v_ix1, v_iy0, v_fx1, v_fy);
    __m256 v_d01 = iq_grad_dot_avx2(v_ix0, v_iy1, v_fx, v_fy1);
    __m256 v_d11 = iq_grad_dot_avx2(v_ix1, v_iy1, v_fx1, v_fy1);

    __m256 v_x0 = _mm256_fmadd_ps(_mm256_sub_ps(v_d10, v_d00), v_ux, v_d00);
    __m256 v_x1 = _mm256_fmadd_ps(_mm256_sub_ps(v_d11, v_d01), v_ux, v_d01);
    return _mm256_fmadd_ps(_mm256_sub_ps(v_x1, v_x0), v_uy, v_x0);
}

__attribute__((target("avx2,fma"))) static void
luma_row_avx2(uint32_t *restrict dst_row, const uint32_t *restrict a_row,
              const uint32_t *restrict b_row, uint32_t width, float p1_x0, float dp1_x, float p1_y,
              float threshold, float edge0, float inv_span, float glow_env)
{
    const __m256 v_dp1_8 = _mm256_set1_ps(8.0f * dp1_x);
    const __m256 v_p1_y = _mm256_set1_ps(p1_y);
    const __m256 v_08 = _mm256_set1_ps(0.8f * 2.0f);
    const __m256 v_06 = _mm256_set1_ps(0.6f * 2.0f);
    const __m256 v_100 = _mm256_set1_ps(100.0f);
    const __m256 v_w0 = _mm256_set1_ps(0.70f);
    const __m256 v_w1 = _mm256_set1_ps(0.30f);
    const __m256 v_half = _mm256_set1_ps(0.5f);
    const __m256 v_thresh = _mm256_set1_ps(threshold);
    const __m256 v_edge0 = _mm256_set1_ps(edge0);
    const __m256 v_inv_span = _mm256_set1_ps(inv_span);
    const __m256 v_glow_r = _mm256_set1_ps(glow_env * 0.3f * 255.0f);
    const __m256 v_glow_g = _mm256_set1_ps(glow_env * 0.6f * 255.0f);
    const __m256 v_glow_b = _mm256_set1_ps(glow_env * 1.0f * 255.0f);
    const __m256 v_zero = _mm256_setzero_ps();
    const __m256 v_one = _mm256_set1_ps(1.0f);
    const __m256 v_two = _mm256_set1_ps(2.0f);
    const __m256 v_three = _mm256_set1_ps(3.0f);
    const __m256 v_256 = _mm256_set1_ps(256.0f);
    const __m256 v_sign_mask = _mm256_set1_ps(-0.0f);
    const __m256i v_alpha = _mm256_set1_epi32((int)0xFF000000U);

    __m256 v_p1x = _mm256_set_ps(p1_x0 + 7.0f * dp1_x, p1_x0 + 6.0f * dp1_x, p1_x0 + 5.0f * dp1_x,
                                 p1_x0 + 4.0f * dp1_x, p1_x0 + 3.0f * dp1_x, p1_x0 + 2.0f * dp1_x,
                                 p1_x0 + 1.0f * dp1_x, p1_x0);

    uint32_t x = 0;
    for (; x + 8 <= width; x += 8) {
        __m256 v_n1 = iq_noise2_avx2(v_p1x, v_p1_y);
        __m256 v_p2x = _mm256_fmadd_ps(v_p1x, v_08, _mm256_fnmadd_ps(v_p1_y, v_06, v_100));
        __m256 v_p2y = _mm256_fmadd_ps(v_p1x, v_06, _mm256_fmadd_ps(v_p1_y, v_08, v_100));
        __m256 v_n2 = iq_noise2_avx2(v_p2x, v_p2y);

        __m256 v_n =
            _mm256_fmadd_ps(_mm256_fmadd_ps(v_w0, v_n1, _mm256_mul_ps(v_w1, v_n2)), v_half, v_half);

        __m256 v_t = _mm256_min_ps(
            v_one, _mm256_max_ps(v_zero, _mm256_mul_ps(_mm256_sub_ps(v_n, v_edge0), v_inv_span)));
        __m256 v_alpha_s =
            _mm256_mul_ps(_mm256_mul_ps(v_t, v_t), _mm256_fnmadd_ps(v_two, v_t, v_three));
        __m256 v_mix = _mm256_sub_ps(v_one, v_alpha_s);
        __m256i v_wb_32 = _mm256_cvttps_epi32(_mm256_mul_ps(v_mix, v_256));

        __m256i v_ca = _mm256_loadu_si256((const __m256i *)&a_row[x]);
        __m256i v_cb = _mm256_loadu_si256((const __m256i *)&b_row[x]);
        __m256i v_base = blend8_argb_var_avx2(v_ca, v_cb, v_wb_32);

        __m256 v_abs_d = _mm256_andnot_ps(v_sign_mask, _mm256_sub_ps(v_n, v_thresh));
        __m256 v_edge = _mm256_max_ps(v_zero, _mm256_fnmadd_ps(v_abs_d, v_inv_span, v_one));
        __m256 v_e2 = _mm256_mul_ps(v_edge, v_edge);

        __m256i v_gr = _mm256_slli_epi32(_mm256_cvttps_epi32(_mm256_mul_ps(v_e2, v_glow_r)), 16);
        __m256i v_gg = _mm256_slli_epi32(_mm256_cvttps_epi32(_mm256_mul_ps(v_e2, v_glow_g)), 8);
        __m256i v_gb = _mm256_cvttps_epi32(_mm256_mul_ps(v_e2, v_glow_b));
        __m256i v_glow_px =
            _mm256_or_si256(_mm256_or_si256(v_gr, v_gg), _mm256_or_si256(v_gb, v_alpha));

        _mm256_storeu_si256((__m256i *)&dst_row[x], _mm256_adds_epu8(v_base, v_glow_px));
        v_p1x = _mm256_add_ps(v_p1x, v_dp1_8);
    }

    for (; x < width; ++x) {
        float p1x = p1_x0 + (float)x * dp1_x;
        float p2x = (0.8f * p1x - 0.6f * p1_y) * 2.0f + 100.0f;
        float p2y = (0.6f * p1x + 0.8f * p1_y) * 2.0f + 100.0f;

        int ix1 = (int)floorf(p1x), iy1 = (int)floorf(p1_y);
        float fx1 = p1x - (float)ix1, fy1 = p1_y - (float)iy1;
        float ux1 = fx1 * fx1 * (3.0f - 2.0f * fx1);
        float uy1 = fy1 * fy1 * (3.0f - 2.0f * fy1);
        float g00x, g00y, g10x, g10y, g01x, g01y, g11x, g11y;
        iq_hash2_vec(ix1, iy1, &g00x, &g00y);
        iq_hash2_vec(ix1 + 1, iy1, &g10x, &g10y);
        iq_hash2_vec(ix1, iy1 + 1, &g01x, &g01y);
        iq_hash2_vec(ix1 + 1, iy1 + 1, &g11x, &g11y);
        float d00 = g00x * fx1 + g00y * fy1;
        float d10 = g10x * (fx1 - 1.0f) + g10y * fy1;
        float d01 = g01x * fx1 + g01y * (fy1 - 1.0f);
        float d11 = g11x * (fx1 - 1.0f) + g11y * (fy1 - 1.0f);
        float n1 = (d00 + (d10 - d00) * ux1) +
                   ((d01 + (d11 - d01) * ux1) - (d00 + (d10 - d00) * ux1)) * uy1;

        int ix2 = (int)floorf(p2x), iy2 = (int)floorf(p2y);
        float fx2 = p2x - (float)ix2, fy2 = p2y - (float)iy2;
        float ux2 = fx2 * fx2 * (3.0f - 2.0f * fx2);
        float uy2 = fy2 * fy2 * (3.0f - 2.0f * fy2);
        iq_hash2_vec(ix2, iy2, &g00x, &g00y);
        iq_hash2_vec(ix2 + 1, iy2, &g10x, &g10y);
        iq_hash2_vec(ix2, iy2 + 1, &g01x, &g01y);
        iq_hash2_vec(ix2 + 1, iy2 + 1, &g11x, &g11y);
        d00 = g00x * fx2 + g00y * fy2;
        d10 = g10x * (fx2 - 1.0f) + g10y * fy2;
        d01 = g01x * fx2 + g01y * (fy2 - 1.0f);
        d11 = g11x * (fx2 - 1.0f) + g11y * (fy2 - 1.0f);
        float n2 = (d00 + (d10 - d00) * ux2) +
                   ((d01 + (d11 - d01) * ux2) - (d00 + (d10 - d00) * ux2)) * uy2;

        float n = (0.70f * n1 + 0.30f * n2) * 0.5f + 0.5f;
        float alpha = smoothstepf(edge0, edge0 + (1.0f / inv_span), n);
        float mix_val = 1.0f - alpha;
        uint16_t wb = (uint16_t)(mix_val * 256.0f);
        uint32_t base = blend_pixel_scalar(a_row[x], b_row[x], wb);
        float edge = 1.0f - fabsf(n - threshold) * inv_span;
        if (edge > 0.0f) {
            float glow = edge * edge * glow_env;
            int r = (int)((base >> 16) & 0xFFu) + (int)(glow * 0.3f * 255.0f);
            int g = (int)((base >> 8) & 0xFFu) + (int)(glow * 0.6f * 255.0f);
            int b = (int)(base & 0xFFu) + (int)(glow * 1.0f * 255.0f);
            if (r > 255)
                r = 255;
            if (g > 255)
                g = 255;
            if (b > 255)
                b = 255;
            base = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
        dst_row[x] = base;
    }
}

__attribute__((target("avx2,fma"))) static void
burn_row_avx2(uint32_t *restrict dst_row, const uint32_t *restrict a_row,
              const uint32_t *restrict b_row, const float *restrict v1, const float *restrict dv1,
              const float *restrict v2, const float *restrict dv2, const int *restrict lut_ix1,
              const float *restrict lut_cx1, const int *restrict lut_ix2,
              const float *restrict lut_cx2, uint32_t width, float p, float flame_width)
{
    const __m256 v_p = _mm256_set1_ps(p);
    const __m256 v_fw = _mm256_set1_ps(flame_width);
    const __m256 v_neg_fw = _mm256_set1_ps(-flame_width);
    const __m256 v_inv_2fw = _mm256_set1_ps(1.0f / (2.0f * flame_width));
    const __m256 v_inv_fw = _mm256_set1_ps(1.0f / flame_width);
    const __m256 v_w1 = _mm256_set1_ps(0.65f);
    const __m256 v_w2 = _mm256_set1_ps(0.35f);
    /* Clamp noise below 0.99989 so progress = 0.9999 (p - flame_width = 0.99989) converges to
     * new_pixels. */
    const __m256 v_n_clamp = _mm256_set1_ps(0.99988f);
    const __m256 v_zero_ps = _mm256_setzero_ps();
    const __m256 v_half_ps = _mm256_set1_ps(0.5f);
    const __m256 v_one_ps = _mm256_set1_ps(1.0f);
    const __m256 v_two_ps = _mm256_set1_ps(2.0f);
    const __m256 v_three_ps = _mm256_set1_ps(3.0f);
    const __m256 v_four_ps = _mm256_set1_ps(4.0f);
    const __m256 v_025_ps = _mm256_set1_ps(0.25f);
    const __m256 v_2p5_ps = _mm256_set1_ps(2.5f);
    const __m256 v_065_ps = _mm256_set1_ps(0.65f);
    const __m256 v_2p857_ps = _mm256_set1_ps(2.857142857f);
    const __m256 v_flame_scale = _mm256_set1_ps(1.8f * 255.0f);
    const __m256 v_256_ps = _mm256_set1_ps(256.0f);
    const __m256 v_sign_mask = _mm256_set1_ps(-0.0f);
    const __m256i v_alpha_mask = _mm256_set1_epi32((int)0xFF000000U);

    uint32_t x = 0;
    for (; x + 8 <= width; x += 8) {
        __m256i v_ix1 = _mm256_loadu_si256((const __m256i *)&lut_ix1[x]);
        __m256 v_cx1 = _mm256_loadu_ps(&lut_cx1[x]);
        __m256 v_n1 = _mm256_fmadd_ps(_mm256_i32gather_ps(dv1, v_ix1, 4), v_cx1,
                                      _mm256_i32gather_ps(v1, v_ix1, 4));

        __m256i v_ix2 = _mm256_loadu_si256((const __m256i *)&lut_ix2[x]);
        __m256 v_cx2 = _mm256_loadu_ps(&lut_cx2[x]);
        __m256 v_n2 = _mm256_fmadd_ps(_mm256_i32gather_ps(dv2, v_ix2, 4), v_cx2,
                                      _mm256_i32gather_ps(v2, v_ix2, 4));

        __m256 v_n =
            _mm256_min_ps(_mm256_fmadd_ps(v_w1, v_n1, _mm256_mul_ps(v_w2, v_n2)), v_n_clamp);
        __m256 v_d = _mm256_sub_ps(v_n, v_p);

        if (_mm256_movemask_ps(_mm256_cmp_ps(v_d, v_fw, _CMP_LT_OQ)) == 0) {
            _mm256_storeu_si256((__m256i *)&dst_row[x],
                                _mm256_loadu_si256((const __m256i *)&a_row[x]));
            continue;
        }
        if (_mm256_movemask_ps(_mm256_cmp_ps(v_d, v_neg_fw, _CMP_GT_OQ)) == 0) {
            _mm256_storeu_si256((__m256i *)&dst_row[x],
                                _mm256_loadu_si256((const __m256i *)&b_row[x]));
            continue;
        }

        __m256 v_t = _mm256_min_ps(
            _mm256_max_ps(_mm256_mul_ps(_mm256_sub_ps(v_fw, v_d), v_inv_2fw), v_zero_ps), v_one_ps);
        __m256 v_burn =
            _mm256_mul_ps(_mm256_mul_ps(v_t, v_t), _mm256_fnmadd_ps(v_two_ps, v_t, v_three_ps));
        __m256i v_wb_32 = _mm256_cvttps_epi32(_mm256_mul_ps(v_burn, v_256_ps));

        __m256i v_ca = _mm256_loadu_si256((const __m256i *)&a_row[x]);
        __m256i v_cb = _mm256_loadu_si256((const __m256i *)&b_row[x]);
        __m256i v_base = blend8_argb_var_avx2(v_ca, v_cb, v_wb_32);

        __m256 v_abs_d = _mm256_andnot_ps(v_sign_mask, v_d);
        __m256 v_dist_norm =
            _mm256_max_ps(v_zero_ps, _mm256_fnmadd_ps(v_abs_d, v_inv_fw, v_one_ps));
        __m256 v_heat = _mm256_mul_ps(v_dist_norm, _mm256_sqrt_ps(v_dist_norm));

        /* Darken the blended base near the ember front before adding the 4-stop blackbody flame. */
        __m256 v_char_f = _mm256_fnmadd_ps(v_heat, v_half_ps, v_one_ps);
        __m256i v_char_w = _mm256_cvttps_epi32(_mm256_mul_ps(v_char_f, v_256_ps));
        __m256i v_char_w16 = _mm256_or_si256(v_char_w, _mm256_slli_epi32(v_char_w, 16));
        __m256i v_charred = shade8_argb_const_avx2(v_base, v_char_w16);

        __m256 v_s0 = _mm256_min_ps(_mm256_mul_ps(v_heat, v_four_ps), v_one_ps);
        __m256 v_s1 = _mm256_min_ps(
            _mm256_max_ps(_mm256_mul_ps(_mm256_sub_ps(v_heat, v_025_ps), v_2p5_ps), v_zero_ps),
            v_one_ps);
        __m256 v_s2 = _mm256_min_ps(
            _mm256_max_ps(_mm256_mul_ps(_mm256_sub_ps(v_heat, v_065_ps), v_2p857_ps), v_zero_ps),
            v_one_ps);

        __m256 v_cr = _mm256_fmadd_ps(_mm256_set1_ps(0.77f), v_s0, _mm256_set1_ps(0.08f));
        __m256 v_cg = _mm256_fmadd_ps(_mm256_set1_ps(0.07f), v_s0, _mm256_set1_ps(0.03f));
        __m256 v_cblu = _mm256_set1_ps(0.02f);

        v_cr = _mm256_fmadd_ps(_mm256_sub_ps(v_one_ps, v_cr), v_s1, v_cr);
        v_cg = _mm256_fmadd_ps(_mm256_sub_ps(_mm256_set1_ps(0.55f), v_cg), v_s1, v_cg);
        v_cblu = _mm256_fmadd_ps(_mm256_sub_ps(_mm256_set1_ps(0.08f), v_cblu), v_s1, v_cblu);

        v_cr = _mm256_fmadd_ps(_mm256_sub_ps(v_one_ps, v_cr), v_s2, v_cr);
        v_cg = _mm256_fmadd_ps(_mm256_sub_ps(_mm256_set1_ps(0.95f), v_cg), v_s2, v_cg);
        v_cblu = _mm256_fmadd_ps(_mm256_sub_ps(_mm256_set1_ps(0.70f), v_cblu), v_s2, v_cblu);

        __m256 v_gain = _mm256_mul_ps(v_heat, v_flame_scale);
        __m256i v_255 = _mm256_set1_epi32(255);
        __m256i v_fr = _mm256_slli_epi32(
            _mm256_min_epi32(_mm256_cvttps_epi32(_mm256_mul_ps(v_cr, v_gain)), v_255), 16);
        __m256i v_fg = _mm256_slli_epi32(
            _mm256_min_epi32(_mm256_cvttps_epi32(_mm256_mul_ps(v_cg, v_gain)), v_255), 8);
        __m256i v_fb = _mm256_min_epi32(_mm256_cvttps_epi32(_mm256_mul_ps(v_cblu, v_gain)), v_255);
        __m256i v_flame =
            _mm256_or_si256(_mm256_or_si256(v_fr, v_fg), _mm256_or_si256(v_fb, v_alpha_mask));

        _mm256_storeu_si256((__m256i *)&dst_row[x], _mm256_adds_epu8(v_charred, v_flame));
    }

    for (; x < width; ++x) {
        int ix1 = lut_ix1[x];
        float n1 = v1[ix1] + dv1[ix1] * lut_cx1[x];
        int ix2 = lut_ix2[x];
        float n2 = v2[ix2] + dv2[ix2] * lut_cx2[x];
        float n = 0.65f * n1 + 0.35f * n2;
        if (n > 0.99988f)
            n = 0.99988f;

        float d = n - p;
        if (d >= flame_width) {
            dst_row[x] = a_row[x];
        } else if (d <= -flame_width) {
            dst_row[x] = b_row[x];
        } else {
            float burn_factor = 1.0f - smoothstepf(-flame_width, flame_width, d);
            uint16_t wb = (uint16_t)(burn_factor * 256.0f);
            uint32_t base = blend_pixel_scalar(a_row[x], b_row[x], wb);

            float dist_norm = 1.0f - fabsf(d) / flame_width;
            float heat = dist_norm * sqrtf(dist_norm);
            float char_scale = 1.0f - heat * 0.5f;
            float fr_f = 0.0f, fg_f = 0.0f, fb_f = 0.0f;
            fire_color_rgb(heat, &fr_f, &fg_f, &fb_f);
            float gain = heat * 1.8f * 255.0f;

            int r = (int)((float)((base >> 16) & 0xFFu) * char_scale + fr_f * gain);
            int g = (int)((float)((base >> 8) & 0xFFu) * char_scale + fg_f * gain);
            int b = (int)((float)(base & 0xFFu) * char_scale + fb_f * gain);
            if (r > 255)
                r = 255;
            if (g > 255)
                g = 255;
            if (b > 255)
                b = 255;
            dst_row[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

__attribute__((target("avx2,fma"))) static void crosszoom_row_avx2(
    uint32_t *restrict dst_row, const uint32_t *restrict row_a0, const uint32_t *restrict row_a1,
    const uint32_t *restrict row_b0, const uint32_t *restrict row_b1, const int *restrict lut_ax0,
    const int *restrict lut_ax1, const int *restrict lut_bx0, const int *restrict lut_bx1,
    uint32_t width, uint16_t wb, int flash, bool has_blur)
{
    const __m256i v_zero = _mm256_setzero_si256();
    const __m256i v_wb = _mm256_set1_epi16((short)wb);
    const __m256i v_wa = _mm256_set1_epi16((short)(256 - wb));
    const uint32_t flash_u = (flash > 0) ? (uint32_t)flash : 0U;
    const __m256i v_flash = _mm256_set1_epi32((int)(0xFF000000U | (flash_u * 0x00010101U)));

    uint32_t x = 0;
    for (; x + 8 <= width; x += 8) {
        __m256i v_ax0 = _mm256_loadu_si256((const __m256i *)&lut_ax0[x]);
        __m256i v_bx0 = _mm256_loadu_si256((const __m256i *)&lut_bx0[x]);
        __m256i v_ca = _mm256_i32gather_epi32((const int *)row_a0, v_ax0, 4);
        __m256i v_cb = _mm256_i32gather_epi32((const int *)row_b0, v_bx0, 4);
        if (has_blur) {
            __m256i v_ax1 = _mm256_loadu_si256((const __m256i *)&lut_ax1[x]);
            __m256i v_bx1 = _mm256_loadu_si256((const __m256i *)&lut_bx1[x]);
            v_ca = _mm256_avg_epu8(v_ca, _mm256_i32gather_epi32((const int *)row_a1, v_ax1, 4));
            v_cb = _mm256_avg_epu8(v_cb, _mm256_i32gather_epi32((const int *)row_b1, v_bx1, 4));
        }

        __m256i a_lo = _mm256_unpacklo_epi8(v_ca, v_zero);
        __m256i a_hi = _mm256_unpackhi_epi8(v_ca, v_zero);
        __m256i b_lo = _mm256_unpacklo_epi8(v_cb, v_zero);
        __m256i b_hi = _mm256_unpackhi_epi8(v_cb, v_zero);

        __m256i r_lo = _mm256_srli_epi16(
            _mm256_add_epi16(_mm256_mullo_epi16(a_lo, v_wa), _mm256_mullo_epi16(b_lo, v_wb)), 8);
        __m256i r_hi = _mm256_srli_epi16(
            _mm256_add_epi16(_mm256_mullo_epi16(a_hi, v_wa), _mm256_mullo_epi16(b_hi, v_wb)), 8);

        __m256i packed = _mm256_adds_epu8(_mm256_packus_epi16(r_lo, r_hi), v_flash);
        _mm256_storeu_si256((__m256i *)&dst_row[x], packed);
    }

    for (; x < width; ++x) {
        uint32_t ca = has_blur ? avg_pixels_rounded(row_a0[lut_ax0[x]], row_a1[lut_ax1[x]])
                               : row_a0[lut_ax0[x]];
        uint32_t cb = has_blur ? avg_pixels_rounded(row_b0[lut_bx0[x]], row_b1[lut_bx1[x]])
                               : row_b0[lut_bx0[x]];
        uint32_t blended = blend_pixel_scalar(ca, cb, wb);
        dst_row[x] = flash > 0 ? add_saturate_rgb(blended, flash) : blended;
    }
}

__attribute__((target("avx2,fma"))) static void
glitch_block_avx2(uint32_t *restrict dst_row, const uint32_t *restrict a_row,
                  const uint32_t *restrict b_row, uint32_t bx_start, uint32_t bx_end,
                  uint32_t width, int dr, int dg, int db, uint16_t wb)
{
    const __m256i v_mask_g = _mm256_set1_epi32(0x0000FF00);
    const __m256i v_mask_a = _mm256_set1_epi32((int)0xFF000000U);
    const __m256i v_zero = _mm256_setzero_si256();
    const __m256i v_wb = _mm256_set1_epi16((short)wb);
    const __m256i v_wa = _mm256_set1_epi16((short)(256 - wb));

    uint32_t x = bx_start;
    int min_d = dr < dg ? (dr < db ? dr : db) : (dg < db ? dg : db);
    int max_d = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);

    if ((int)bx_start + min_d >= 0 && (int)bx_end + max_d <= (int)width) {
        if (wb == 0) {
            for (; x + 8 <= bx_end; x += 8) {
                __m256i vr = _mm256_loadu_si256((const __m256i *)&a_row[(int)x + dr]);
                __m256i vg = _mm256_loadu_si256((const __m256i *)&a_row[(int)x + dg]);
                __m256i vb = _mm256_loadu_si256((const __m256i *)&a_row[(int)x + db]);
                __m256i v_gb = _mm256_blendv_epi8(vb, vg, v_mask_g);
                __m256i v_ra = _mm256_or_si256(vr, v_mask_a);
                _mm256_storeu_si256((__m256i *)&dst_row[x], _mm256_blend_epi16(v_gb, v_ra, 0xAA));
            }
        } else if (wb >= 256) {
            for (; x + 8 <= bx_end; x += 8) {
                __m256i vr = _mm256_loadu_si256((const __m256i *)&b_row[(int)x + dr]);
                __m256i vg = _mm256_loadu_si256((const __m256i *)&b_row[(int)x + dg]);
                __m256i vb = _mm256_loadu_si256((const __m256i *)&b_row[(int)x + db]);
                __m256i v_gb = _mm256_blendv_epi8(vb, vg, v_mask_g);
                __m256i v_ra = _mm256_or_si256(vr, v_mask_a);
                _mm256_storeu_si256((__m256i *)&dst_row[x], _mm256_blend_epi16(v_gb, v_ra, 0xAA));
            }
        } else {
            for (; x + 8 <= bx_end; x += 8) {
                __m256i ar = _mm256_loadu_si256((const __m256i *)&a_row[(int)x + dr]);
                __m256i ag = _mm256_loadu_si256((const __m256i *)&a_row[(int)x + dg]);
                __m256i ab = _mm256_loadu_si256((const __m256i *)&a_row[(int)x + db]);
                __m256i ca = _mm256_blend_epi16(_mm256_blendv_epi8(ab, ag, v_mask_g),
                                                _mm256_or_si256(ar, v_mask_a), 0xAA);

                __m256i br = _mm256_loadu_si256((const __m256i *)&b_row[(int)x + dr]);
                __m256i bg = _mm256_loadu_si256((const __m256i *)&b_row[(int)x + dg]);
                __m256i bb = _mm256_loadu_si256((const __m256i *)&b_row[(int)x + db]);
                __m256i cb = _mm256_blend_epi16(_mm256_blendv_epi8(bb, bg, v_mask_g),
                                                _mm256_or_si256(br, v_mask_a), 0xAA);

                __m256i a_lo = _mm256_unpacklo_epi8(ca, v_zero);
                __m256i a_hi = _mm256_unpackhi_epi8(ca, v_zero);
                __m256i b_lo = _mm256_unpacklo_epi8(cb, v_zero);
                __m256i b_hi = _mm256_unpackhi_epi8(cb, v_zero);

                __m256i r_lo = _mm256_srli_epi16(_mm256_add_epi16(_mm256_mullo_epi16(a_lo, v_wa),
                                                                  _mm256_mullo_epi16(b_lo, v_wb)),
                                                 8);
                __m256i r_hi = _mm256_srli_epi16(_mm256_add_epi16(_mm256_mullo_epi16(a_hi, v_wa),
                                                                  _mm256_mullo_epi16(b_hi, v_wb)),
                                                 8);
                _mm256_storeu_si256((__m256i *)&dst_row[x],
                                    _mm256_or_si256(_mm256_packus_epi16(r_lo, r_hi), v_mask_a));
            }
        }
    }

    for (; x < bx_end; ++x) {
        int rx = (int)x + dr;
        int gx = (int)x + dg;
        int bx = (int)x + db;
        if (rx < 0)
            rx = 0;
        else if (rx >= (int)width)
            rx = (int)width - 1;
        if (gx < 0)
            gx = 0;
        else if (gx >= (int)width)
            gx = (int)width - 1;
        if (bx < 0)
            bx = 0;
        else if (bx >= (int)width)
            bx = (int)width - 1;

        uint32_t ca = 0xFF000000U | (a_row[rx] & 0x00FF0000U) | (a_row[gx] & 0x0000FF00U) |
                      (a_row[bx] & 0x000000FFU);
        uint32_t cb = 0xFF000000U | (b_row[rx] & 0x00FF0000U) | (b_row[gx] & 0x0000FF00U) |
                      (b_row[bx] & 0x000000FFU);
        dst_row[x] = blend_pixel_scalar(ca, cb, wb);
    }
}

__attribute__((target("avx2,fma"))) static void
light_leak_row_avx2(uint32_t *restrict dst_row, const uint32_t *restrict a_row,
                    const uint32_t *restrict b_row, uint32_t width, float inv_w, float row_offset,
                    float orb1_cx, float orb1_dy2, float orb2_cx, float orb2_dy2, float env255,
                    uint16_t wb)
{
    const __m256 v_inv_w = _mm256_set1_ps(inv_w);
    const __m256 v_row_offset = _mm256_set1_ps(row_offset);
    const __m256 v_orb1_cx = _mm256_set1_ps(orb1_cx);
    const __m256 v_orb1_dy2 = _mm256_set1_ps(orb1_dy2);
    const __m256 v_orb2_cx = _mm256_set1_ps(orb2_cx);
    const __m256 v_orb2_dy2 = _mm256_set1_ps(orb2_dy2);
    const __m256 v_beam_k = _mm256_set1_ps(10.666667f);
    const __m256 v_four = _mm256_set1_ps(4.0f);
    const __m256 v_seven = _mm256_set1_ps(7.0f);
    const __m256 v_one = _mm256_set1_ps(1.0f);
    const __m256 v_zero_f = _mm256_setzero_ps();
    const __m256 v_eight = _mm256_set1_ps(8.0f);

    const __m256 v_beam_r = _mm256_set1_ps(1.1f * 1.8f * env255);
    const __m256 v_beam_g = _mm256_set1_ps(0.65f * 1.8f * env255);
    const __m256 v_beam_b = _mm256_set1_ps(0.25f * 1.8f * env255);
    const __m256 v_o1_r = _mm256_set1_ps(1.0f * 1.2f * env255);
    const __m256 v_o1_g = _mm256_set1_ps(0.85f * 1.2f * env255);
    const __m256 v_o1_b = _mm256_set1_ps(0.4f * 1.2f * env255);
    const __m256 v_o2_r = _mm256_set1_ps(0.9f * 1.0f * env255);
    const __m256 v_o2_g = _mm256_set1_ps(0.3f * 1.0f * env255);
    const __m256 v_o2_b = _mm256_set1_ps(0.5f * 1.0f * env255);

    const __m256i v_255 = _mm256_set1_epi32(255);
    const __m256i v_alpha = _mm256_set1_epi32((int)0xFF000000U);
    const __m256i v_wb_32 = _mm256_set1_epi32((int)wb);

    __m256 vx = _mm256_set_ps(7.5f, 6.5f, 5.5f, 4.5f, 3.5f, 2.5f, 1.5f, 0.5f);
    uint32_t x = 0;
    for (; x + 8 <= width; x += 8) {
        __m256 v_uvx = _mm256_mul_ps(vx, v_inv_w);
        __m256 v_db = _mm256_add_ps(v_uvx, v_row_offset);
        __m256 v_tb =
            _mm256_max_ps(v_zero_f, _mm256_fnmadd_ps(_mm256_mul_ps(v_db, v_db), v_beam_k, v_one));
        __m256 v_fb = _mm256_mul_ps(_mm256_mul_ps(v_tb, v_tb), v_tb);

        __m256 v_dx1 = _mm256_sub_ps(v_uvx, v_orb1_cx);
        __m256 v_d12 = _mm256_fmadd_ps(v_dx1, v_dx1, v_orb1_dy2);
        __m256 v_t1 = _mm256_max_ps(v_zero_f, _mm256_fnmadd_ps(v_d12, v_four, v_one));
        __m256 v_o1 = _mm256_mul_ps(_mm256_mul_ps(v_t1, v_t1), v_t1);

        __m256 v_dx2 = _mm256_sub_ps(v_uvx, v_orb2_cx);
        __m256 v_d22 = _mm256_fmadd_ps(v_dx2, v_dx2, v_orb2_dy2);
        __m256 v_t2 = _mm256_max_ps(v_zero_f, _mm256_fnmadd_ps(v_d22, v_seven, v_one));
        __m256 v_o2 = _mm256_mul_ps(_mm256_mul_ps(v_t2, v_t2), v_t2);

        __m256 v_fr_f = _mm256_fmadd_ps(v_fb, v_beam_r,
                                        _mm256_fmadd_ps(v_o1, v_o1_r, _mm256_mul_ps(v_o2, v_o2_r)));
        __m256 v_fg_f = _mm256_fmadd_ps(v_fb, v_beam_g,
                                        _mm256_fmadd_ps(v_o1, v_o1_g, _mm256_mul_ps(v_o2, v_o2_g)));
        __m256 v_fb_f = _mm256_fmadd_ps(v_fb, v_beam_b,
                                        _mm256_fmadd_ps(v_o1, v_o1_b, _mm256_mul_ps(v_o2, v_o2_b)));

        __m256i v_fr = _mm256_slli_epi32(_mm256_min_epi32(_mm256_cvttps_epi32(v_fr_f), v_255), 16);
        __m256i v_fg = _mm256_slli_epi32(_mm256_min_epi32(_mm256_cvttps_epi32(v_fg_f), v_255), 8);
        __m256i v_fbi = _mm256_min_epi32(_mm256_cvttps_epi32(v_fb_f), v_255);
        __m256i v_flare =
            _mm256_or_si256(_mm256_or_si256(v_fr, v_fg), _mm256_or_si256(v_fbi, v_alpha));

        __m256i v_ca = _mm256_loadu_si256((const __m256i *)&a_row[x]);
        __m256i v_cb = _mm256_loadu_si256((const __m256i *)&b_row[x]);
        __m256i v_base = blend8_argb_var_avx2(v_ca, v_cb, v_wb_32);

        _mm256_storeu_si256((__m256i *)&dst_row[x], _mm256_adds_epu8(v_base, v_flare));
        vx = _mm256_add_ps(vx, v_eight);
    }

    for (; x < width; ++x) {
        float uvx = ((float)x + 0.5f) * inv_w;
        float db = uvx + row_offset;
        float tb = 1.0f - (db * db) * 10.666667f;
        float fb = (tb > 0.0f) ? (tb * tb * tb) : 0.0f;

        float dx1 = uvx - orb1_cx;
        float t1 = 1.0f - (dx1 * dx1 + orb1_dy2) * 4.0f;
        float o1 = (t1 > 0.0f) ? (t1 * t1 * t1) : 0.0f;

        float dx2 = uvx - orb2_cx;
        float t2 = 1.0f - (dx2 * dx2 + orb2_dy2) * 7.0f;
        float o2 = (t2 > 0.0f) ? (t2 * t2 * t2) : 0.0f;

        int fr = (int)((1.1f * 1.8f * fb + 1.0f * 1.2f * o1 + 0.9f * o2) * env255);
        int fg = (int)((0.65f * 1.8f * fb + 0.85f * 1.2f * o1 + 0.3f * o2) * env255);
        int fbl = (int)((0.25f * 1.8f * fb + 0.4f * 1.2f * o1 + 0.5f * o2) * env255);

        uint32_t blended = blend_pixel_scalar(a_row[x], b_row[x], wb);
        int r = (int)((blended >> 16) & 0xFFu) + fr;
        int g = (int)((blended >> 8) & 0xFFu) + fg;
        int b = (int)(blended & 0xFFu) + fbl;
        if (r > 255)
            r = 255;
        if (g > 255)
            g = 255;
        if (b > 255)
            b = 255;
        dst_row[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}
#endif

static float s_unit_sine_lut[1024];
static pthread_once_t s_sine_lut_once = PTHREAD_ONCE_INIT;

static void init_unit_sine_lut(void)
{
    for (int i = 0; i < 1024; ++i) {
        s_unit_sine_lut[i] = sinf((float)i * (6.28318530718f / 1024.0f));
    }
}

static inline const float *get_unit_sine_lut(void)
{
    pthread_once(&s_sine_lut_once, init_unit_sine_lut);
    return s_unit_sine_lut;
}

static inline void simd_stream_fence(void)
{
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
    _mm_sfence();
#endif
}

bool render_engine_execute_cpu_transition(uint32_t *dst, const uint32_t *old_pixels,
                                          const uint32_t *new_pixels, uint32_t width,
                                          uint32_t height, uint32_t stride_bytes,
                                          const waywal_transition_params_t *params)
{
    if (!dst || !old_pixels || !new_pixels || width == 0 || height == 0 || !params) {
        return false;
    }
    if (stride_bytes == 0) {
        stride_bytes = width * (uint32_t)sizeof(uint32_t);
    } else if (stride_bytes < width * sizeof(uint32_t)) {
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

    waywal_transition_type_t eff_type = params->type;
    if (eff_type == WAYWAL_TRANSITION_RANDOM) {
        /* Lock one deterministic transition per frame pair across progress ticks without rand(). */
        uint64_t h = (uint64_t)(uintptr_t)old_pixels ^
                     ((uint64_t)(uintptr_t)new_pixels * 0x9E3779B97F4A7C15ULL);
        h ^= ((uint64_t)width << 32) | (uint64_t)height;
        h ^= ((uint64_t)old_pixels[0] << 32) | (uint64_t)new_pixels[0];
        h ^= h >> 30;
        h *= 0xBF58476D1CE4E5B9ULL;
        h ^= h >> 27;
        eff_type = (waywal_transition_type_t)(2 + (int)(h % 18ULL));
    }

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
    bool has_avx2 = cpu_supports_avx2_fma();
#endif

    switch (eff_type) {
    case WAYWAL_TRANSITION_FADE:
    case WAYWAL_TRANSITION_SIMPLE:
    case WAYWAL_TRANSITION_CUSTOM: {
        uint16_t weight_b = (uint16_t)(progress * 256.0f + 0.5f);
        if (weight_b > 256)
            weight_b = 256;

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            uint8_t *dst_row = (uint8_t *)(dst + (size_t)y * stride_pixels);
            const uint8_t *src_a_row = (const uint8_t *)(old_pixels + (size_t)y * width);
            const uint8_t *src_b_row = (const uint8_t *)(new_pixels + (size_t)y * width);

            blend_fn(dst_row, src_a_row, src_b_row, weight_b, (size_t)width * sizeof(uint32_t));
        }
        simd_stream_fence();
        return true;
    }

    case WAYWAL_TRANSITION_WIPE: {
        float cos_a = cosf(params->angle_rad);
        float sin_a = sinf(params->angle_rad);
        float inv_span = 1.0f / fmaxf(fabsf(cos_a) + fabsf(sin_a), 1e-5f);
        float edge = 0.04f;
        float p = progress * (1.0f + 2.0f * edge) - edge;
        float d_proj = (cos_a * inv_span) / (float)width;
        float nx0 = 0.5f / (float)width - 0.5f;
        float p_plus_edge = p + edge;
        float p_minus_edge = p - edge;
        float inv_2edge = 1.0f / (2.0f * edge);
        bool flat_x = (fabsf(d_proj) * (float)width < 1e-5f);
        float inv_d = flat_x ? 0.0f : (1.0f / d_proj);

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float ny = ((float)y + 0.5f) / (float)height - 0.5f;
            float proj_0 = (nx0 * cos_a + ny * sin_a) * inv_span + 0.5f;
            float proj_end = proj_0 + (float)(width - 1U) * d_proj;
            float proj_min = proj_0 < proj_end ? proj_0 : proj_end;
            float proj_max = proj_0 > proj_end ? proj_0 : proj_end;

            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

            if (proj_min >= p_plus_edge) {
                memcpy(dst_row, a_row, (size_t)width * sizeof(uint32_t));
                continue;
            }
            if (proj_max <= p_minus_edge) {
                memcpy(dst_row, b_row, (size_t)width * sizeof(uint32_t));
                continue;
            }

            int x_seam_L = 0;
            int x_seam_R = (int)width;
            if (!flat_x) {
                float x1 = (p_minus_edge - proj_0) * inv_d;
                float x2 = (p_plus_edge - proj_0) * inv_d;
                float x_min = x1 < x2 ? x1 : x2;
                float x_max = x1 > x2 ? x1 : x2;
                x_seam_L = span_floor_clamp(x_min, (int)width);
                x_seam_R = span_ceil_clamp(x_max, (int)width);
                if (x_seam_L > x_seam_R)
                    x_seam_L = x_seam_R;
            }

            const uint32_t *left_src = (d_proj >= 0.0f) ? b_row : a_row;
            const uint32_t *right_src = (d_proj >= 0.0f) ? a_row : b_row;

            if (x_seam_L > 0) {
                memcpy(dst_row, left_src, (size_t)x_seam_L * sizeof(uint32_t));
            }

            int x = x_seam_L;
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
            if (has_avx2 && x_seam_R - x_seam_L >= 8) {
                x = wipe_seam_avx2(dst_row, a_row, b_row, x_seam_L, x_seam_R, proj_0, d_proj,
                                   p_plus_edge, inv_2edge);
            }
#endif
            for (; x < x_seam_R; ++x) {
                float proj = proj_0 + (float)x * d_proj;
                float t = clampf((p_plus_edge - proj) * inv_2edge, 0.0f, 1.0f);
                float alpha = t * t * (3.0f - 2.0f * t);
                if (alpha <= 0.0f) {
                    dst_row[x] = a_row[x];
                } else if (alpha >= 1.0f) {
                    dst_row[x] = b_row[x];
                } else {
                    uint16_t wb = (uint16_t)(alpha * 256.0f);
                    dst_row[x] = blend_pixel_scalar(a_row[x], b_row[x], wb);
                }
            }

            if (x_seam_R < (int)width) {
                memcpy(dst_row + x_seam_R, right_src + x_seam_R,
                       (size_t)((int)width - x_seam_R) * sizeof(uint32_t));
            }
        }
        simd_stream_fence();
        return true;
    }

    case WAYWAL_TRANSITION_GROW:
    case WAYWAL_TRANSITION_OUTER: {
        bool is_grow = (eff_type == WAYWAL_TRANSITION_GROW);
        float cx = 0.5f, cy = 0.5f;
        resolve_center(params, &cx, &cy);

        float aspect = (float)width / (float)height;
        float max_x = cx > (1.0f - cx) ? cx : (1.0f - cx);
        float max_y = cy > (1.0f - cy) ? cy : (1.0f - cy);
        float max_dist = sqrtf((max_x * aspect) * (max_x * aspect) + max_y * max_y) + 0.05f;
        float edge = 0.03f * max_dist;
        float eff_p = is_grow ? progress : (1.0f - progress);
        float r = eff_p * (max_dist + 2.0f * edge) - edge;
        float r_inner = r - edge;
        float r_outer = r + edge;
        float r_inner2 = r_inner > 0.0f ? r_inner * r_inner : 0.0f;
        float r_outer2 = r_outer > 0.0f ? r_outer * r_outer : 0.0f;

        float xc_px = cx * (float)width;
        float scale_x = (float)height;
        float step_dx = aspect / (float)width;
        float cx_scaled = cx * aspect;
        float r_ref = is_grow ? r_outer : r_inner;
        float signed_inv_2edge = (is_grow ? -1.0f : 1.0f) / (2.0f * edge);

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float dy = ((float)y + 0.5f) / (float)height - cy;
            float dy2 = dy * dy;
            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;
            const uint32_t *ext_row = is_grow ? a_row : b_row;
            const uint32_t *int_row = is_grow ? b_row : a_row;

            if (r_outer <= 0.0f || dy2 >= r_outer2) {
                memcpy(dst_row, ext_row, (size_t)width * sizeof(uint32_t));
                continue;
            }

            float dx_out = scale_x * sqrtf(r_outer2 - dy2);
            int x_out_L = span_floor_clamp(xc_px - dx_out - 0.5f, (int)width);
            int x_out_R = span_ceil_clamp(xc_px + dx_out - 0.5f, (int)width);
            if (x_out_L > x_out_R)
                x_out_L = x_out_R;

            int x_in_L = x_out_L;
            int x_in_R = x_out_L;
            if (r_inner > 0.0f && dy2 < r_inner2) {
                float dx_in = scale_x * sqrtf(r_inner2 - dy2);
                x_in_L = span_ceil_clamp(xc_px - dx_in - 0.5f, (int)width);
                x_in_R = span_floor_clamp(xc_px + dx_in - 0.5f, (int)width);
                if (x_in_L < x_out_L)
                    x_in_L = x_out_L;
                if (x_in_R > x_out_R)
                    x_in_R = x_out_R;
                if (x_in_L > x_in_R)
                    x_in_L = x_in_R;
            }

            if (x_out_L > 0) {
                memcpy(dst_row, ext_row, (size_t)x_out_L * sizeof(uint32_t));
            }

            int x = x_out_L;
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
            if (has_avx2 && x_in_L - x >= 8) {
                x = radial_annulus_avx2(dst_row, a_row, b_row, x, x_in_L, cx_scaled, step_dx, dy2,
                                        r_ref, signed_inv_2edge);
            }
#endif
            for (; x < x_in_L; ++x) {
                float dx = ((float)x + 0.5f) * step_dx - cx_scaled;
                float dist = sqrtf(dx * dx + dy2);
                float t = clampf((dist - r_ref) * signed_inv_2edge, 0.0f, 1.0f);
                float alpha = t * t * (3.0f - 2.0f * t);
                if (alpha <= 0.0f) {
                    dst_row[x] = a_row[x];
                } else if (alpha >= 1.0f) {
                    dst_row[x] = b_row[x];
                } else {
                    uint16_t wb = (uint16_t)(alpha * 256.0f);
                    dst_row[x] = blend_pixel_scalar(a_row[x], b_row[x], wb);
                }
            }

            if (x_in_R > x_in_L) {
                memcpy(dst_row + x_in_L, int_row + x_in_L,
                       (size_t)(x_in_R - x_in_L) * sizeof(uint32_t));
            }

            x = x_in_R;
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
            if (has_avx2 && x_out_R - x >= 8) {
                x = radial_annulus_avx2(dst_row, a_row, b_row, x, x_out_R, cx_scaled, step_dx, dy2,
                                        r_ref, signed_inv_2edge);
            }
#endif
            for (; x < x_out_R; ++x) {
                float dx = ((float)x + 0.5f) * step_dx - cx_scaled;
                float dist = sqrtf(dx * dx + dy2);
                float t = clampf((dist - r_ref) * signed_inv_2edge, 0.0f, 1.0f);
                float alpha = t * t * (3.0f - 2.0f * t);
                if (alpha <= 0.0f) {
                    dst_row[x] = a_row[x];
                } else if (alpha >= 1.0f) {
                    dst_row[x] = b_row[x];
                } else {
                    uint16_t wb = (uint16_t)(alpha * 256.0f);
                    dst_row[x] = blend_pixel_scalar(a_row[x], b_row[x], wb);
                }
            }

            if (x_out_R < (int)width) {
                memcpy(dst_row + x_out_R, ext_row + x_out_R,
                       (size_t)((int)width - x_out_R) * sizeof(uint32_t));
            }
        }
        simd_stream_fence();
        return true;
    }

    case WAYWAL_TRANSITION_WAVE: {
        float cos_a = cosf(params->angle_rad);
        float sin_a = sinf(params->angle_rad);
        float inv_span = 1.0f / fmaxf(fabsf(cos_a) + fabsf(sin_a), 1e-5f);
        float freq = params->wave_freq > 0.0f ? params->wave_freq : 20.0f;
        float amp = params->wave_amp > 0.0f ? params->wave_amp : 0.05f;
        float edge = 0.04f;
        float margin = amp + edge;
        float p = progress * (1.0f + 2.0f * margin) - margin;
        float p_plus_margin = p + margin;
        float p_minus_margin = p - margin;
        float p_plus_edge = p + edge;
        float inv_2edge = 1.0f / (2.0f * edge);

        const float *unit_sin = get_unit_sine_lut();
        float sine_lut[1024];
        for (int i = 0; i < 1024; ++i) {
            sine_lut[i] = unit_sin[i] * amp;
        }

        float d_proj = (cos_a * inv_span) / (float)width;
        float nx0 = 0.5f / (float)width - 0.5f;
        float d_cross = (-sin_a * freq) / (float)width;
        float d_phase = d_cross * (1024.0f / 6.28318530718f);
        bool flat_x = (fabsf(d_proj) * (float)width < 1e-5f);
        float inv_d = flat_x ? 0.0f : (1.0f / d_proj);

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float ny = ((float)y + 0.5f) / (float)height - 0.5f;
            float proj_0 = (nx0 * cos_a + ny * sin_a) * inv_span + 0.5f;
            float proj_end = proj_0 + (float)(width - 1U) * d_proj;
            float proj_min = proj_0 < proj_end ? proj_0 : proj_end;
            float proj_max = proj_0 > proj_end ? proj_0 : proj_end;

            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

            if (proj_min >= p_plus_margin) {
                memcpy(dst_row, a_row, (size_t)width * sizeof(uint32_t));
                continue;
            }
            if (proj_max <= p_minus_margin) {
                memcpy(dst_row, b_row, (size_t)width * sizeof(uint32_t));
                continue;
            }

            int x_band_L = 0;
            int x_band_R = (int)width;
            if (!flat_x) {
                float x1 = (p_minus_margin - proj_0) * inv_d;
                float x2 = (p_plus_margin - proj_0) * inv_d;
                float x_min = x1 < x2 ? x1 : x2;
                float x_max = x1 > x2 ? x1 : x2;
                x_band_L = span_floor_clamp(x_min, (int)width);
                x_band_R = span_ceil_clamp(x_max, (int)width);
                if (x_band_L > x_band_R)
                    x_band_L = x_band_R;
            }

            const uint32_t *left_src = (d_proj >= 0.0f) ? b_row : a_row;
            const uint32_t *right_src = (d_proj >= 0.0f) ? a_row : b_row;

            if (x_band_L > 0) {
                memcpy(dst_row, left_src, (size_t)x_band_L * sizeof(uint32_t));
            }

            float phase_0 = (-nx0 * sin_a + ny * cos_a) * freq * (1024.0f / 6.28318530718f);
            int x = x_band_L;
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
            if (has_avx2 && x_band_R - x >= 8) {
                x = wave_band_avx2(dst_row, a_row, b_row, sine_lut, x, x_band_R, proj_0, d_proj,
                                   phase_0, d_phase, p_plus_edge, inv_2edge);
            }
#endif
            for (; x < x_band_R; ++x) {
                float proj = proj_0 + (float)x * d_proj;
                float ph = phase_0 + (float)x * d_phase;
                int idx = ((int)floorf(ph)) & 1023;
                float d = proj + sine_lut[idx];
                float t = clampf((p_plus_edge - d) * inv_2edge, 0.0f, 1.0f);
                float alpha = t * t * (3.0f - 2.0f * t);
                if (alpha <= 0.0f) {
                    dst_row[x] = a_row[x];
                } else if (alpha >= 1.0f) {
                    dst_row[x] = b_row[x];
                } else {
                    uint16_t wb = (uint16_t)(alpha * 256.0f);
                    dst_row[x] = blend_pixel_scalar(a_row[x], b_row[x], wb);
                }
            }

            if (x_band_R < (int)width) {
                memcpy(dst_row + x_band_R, right_src + x_band_R,
                       (size_t)((int)width - x_band_R) * sizeof(uint32_t));
            }
        }
        simd_stream_fence();
        return true;
    }

    case WAYWAL_TRANSITION_NOISE: {
        float edge = 0.05f;
        float p = progress * (1.0f + 2.0f * edge) - edge;
        float inv_w = 16.0f / (float)width;
        float inv_h = 16.0f / (float)height;

        float grid[18][18];
        for (int j = 0; j < 18; ++j) {
            for (int i = 0; i < 18; ++i) {
                float hval = iq_hash21_f(i, j);
                grid[j][i] = (hval > 0.999884f) ? 0.999884f : hval;
            }
        }

        int lut_ix_stack[4096];
        float lut_cx_stack[4096];
        int *lut_ix = (width <= 4096) ? lut_ix_stack : (int *)malloc((size_t)width * sizeof(int));
        float *lut_cx =
            (width <= 4096) ? lut_cx_stack : (float *)malloc((size_t)width * sizeof(float));
        if (!lut_ix || !lut_cx) {
            if (lut_ix && lut_ix != lut_ix_stack)
                free(lut_ix);
            if (lut_cx && lut_cx != lut_cx_stack)
                free(lut_cx);
            return false;
        }

        for (uint32_t x = 0; x < width; ++x) {
            float vx = ((float)x + 0.5f) * inv_w;
            int ix = (int)vx;
            if (ix < 0)
                ix = 0;
            if (ix > 16)
                ix = 16;
            float fx = vx - (float)ix;
            lut_ix[x] = ix;
            lut_cx[x] = fx * fx * (3.0f - 2.0f * fx);
        }

        float p_plus_edge = p + edge;
        float two_edge = 2.0f * edge;
        float inv_2edge = 1.0f / two_edge;

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

            float v[18];
            float dv[18];
            for (int i = 0; i < 18; ++i) {
                v[i] = grid[iy][i] * (1.0f - cy) + grid[iy + 1][i] * cy;
            }
            for (int i = 0; i < 17; ++i) {
                dv[i] = v[i + 1] - v[i];
            }
            dv[17] = 0.0f;

            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
            if (has_avx2) {
                noise_row_avx2(dst_row, a_row, b_row, v, dv, lut_ix, lut_cx, width, p_plus_edge,
                               two_edge, inv_2edge);
                continue;
            }
#endif
            for (uint32_t x = 0; x < width; ++x) {
                int ix = lut_ix[x];
                float n = v[ix] + dv[ix] * lut_cx[x];
                float diff = p_plus_edge - n;
                if (diff <= 0.0f) {
                    dst_row[x] = a_row[x];
                } else if (diff >= two_edge) {
                    dst_row[x] = b_row[x];
                } else {
                    float t = diff * inv_2edge;
                    float alpha = t * t * (3.0f - 2.0f * t);
                    uint16_t wb = (uint16_t)(alpha * 256.0f);
                    dst_row[x] = blend_pixel_scalar(a_row[x], b_row[x], wb);
                }
            }
        }

        if (lut_ix != lut_ix_stack)
            free(lut_ix);
        if (lut_cx != lut_cx_stack)
            free(lut_cx);
        simd_stream_fence();
        return true;
    }

    case WAYWAL_TRANSITION_CROSSZOOM: {
        float cx = 0.5f, cy = 0.5f;
        resolve_center(params, &cx, &cy);
        float strength = params->wave_amp > 0.001f ? params->wave_amp : 0.4f;
        float peak = sinf(progress * 3.14159265f);
        float blur = peak * strength * 0.6f;
        bool has_blur = (blur >= 0.001f);
        int flash = (int)lroundf(peak * 0.25f * 255.0f);
        float zoom_a = 1.0f - progress * strength;
        float zoom_b = 1.0f + (1.0f - progress) * strength;
        uint16_t wb = (uint16_t)(progress * 256.0f + 0.5f);
        if (wb > 256)
            wb = 256;

        int lut_ax0_stack[4096], lut_ax1_stack[4096];
        int lut_bx0_stack[4096], lut_bx1_stack[4096];
        int *lut_ax0 = (width <= 4096) ? lut_ax0_stack : (int *)malloc((size_t)width * sizeof(int));
        int *lut_ax1 = (width <= 4096) ? lut_ax1_stack : (int *)malloc((size_t)width * sizeof(int));
        int *lut_bx0 = (width <= 4096) ? lut_bx0_stack : (int *)malloc((size_t)width * sizeof(int));
        int *lut_bx1 = (width <= 4096) ? lut_bx1_stack : (int *)malloc((size_t)width * sizeof(int));
        if (!lut_ax0 || !lut_ax1 || !lut_bx0 || !lut_bx1) {
            if (lut_ax0 && lut_ax0 != lut_ax0_stack)
                free(lut_ax0);
            if (lut_ax1 && lut_ax1 != lut_ax1_stack)
                free(lut_ax1);
            if (lut_bx0 && lut_bx0 != lut_bx0_stack)
                free(lut_bx0);
            if (lut_bx1 && lut_bx1 != lut_bx1_stack)
                free(lut_bx1);
            return false;
        }

        for (uint32_t x = 0; x < width; ++x) {
            float nx = (((float)x + 0.5f) / (float)width - cx);
            float base_ax = clampf(cx + nx * zoom_a, 0.0f, 1.0f);
            float base_bx = clampf(cx + nx * zoom_b, 0.0f, 1.0f);
            float off_x = nx * blur * 0.25f;

            int ax0 = (int)(clampf(base_ax - off_x, 0.0f, 1.0f) * (float)width);
            int ax1 = (int)(clampf(base_ax + off_x, 0.0f, 1.0f) * (float)width);
            int bx0 = (int)(clampf(base_bx - off_x, 0.0f, 1.0f) * (float)width);
            int bx1 = (int)(clampf(base_bx + off_x, 0.0f, 1.0f) * (float)width);
            if (ax0 >= (int)width)
                ax0 = (int)width - 1;
            if (ax1 >= (int)width)
                ax1 = (int)width - 1;
            if (bx0 >= (int)width)
                bx0 = (int)width - 1;
            if (bx1 >= (int)width)
                bx1 = (int)width - 1;
            lut_ax0[x] = ax0;
            lut_ax1[x] = ax1;
            lut_bx0[x] = bx0;
            lut_bx1[x] = bx1;
        }

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float ny = (((float)y + 0.5f) / (float)height - cy);
            float base_ay = clampf(cy + ny * zoom_a, 0.0f, 1.0f);
            float base_by = clampf(cy + ny * zoom_b, 0.0f, 1.0f);
            float off_y = ny * blur * 0.25f;

            int ay0 = (int)(clampf(base_ay - off_y, 0.0f, 1.0f) * (float)height);
            int ay1 = (int)(clampf(base_ay + off_y, 0.0f, 1.0f) * (float)height);
            int by0 = (int)(clampf(base_by - off_y, 0.0f, 1.0f) * (float)height);
            int by1 = (int)(clampf(base_by + off_y, 0.0f, 1.0f) * (float)height);
            if (ay0 >= (int)height)
                ay0 = (int)height - 1;
            if (ay1 >= (int)height)
                ay1 = (int)height - 1;
            if (by0 >= (int)height)
                by0 = (int)height - 1;
            if (by1 >= (int)height)
                by1 = (int)height - 1;

            const uint32_t *row_a0 = old_pixels + (size_t)ay0 * width;
            const uint32_t *row_a1 = old_pixels + (size_t)ay1 * width;
            const uint32_t *row_b0 = new_pixels + (size_t)by0 * width;
            const uint32_t *row_b1 = new_pixels + (size_t)by1 * width;
            uint32_t *dst_row = dst + (size_t)y * stride_pixels;

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
            if (has_avx2) {
                crosszoom_row_avx2(dst_row, row_a0, row_a1, row_b0, row_b1, lut_ax0, lut_ax1,
                                   lut_bx0, lut_bx1, width, wb, flash, has_blur);
                continue;
            }
#endif
            for (uint32_t x = 0; x < width; ++x) {
                uint32_t ca = has_blur ? avg_pixels_rounded(row_a0[lut_ax0[x]], row_a1[lut_ax1[x]])
                                       : row_a0[lut_ax0[x]];
                uint32_t cb = has_blur ? avg_pixels_rounded(row_b0[lut_bx0[x]], row_b1[lut_bx1[x]])
                                       : row_b0[lut_bx0[x]];
                uint32_t blended = blend_pixel_scalar(ca, cb, wb);
                dst_row[x] = flash > 0 ? add_saturate_rgb(blended, flash) : blended;
            }
        }

        if (lut_ax0 != lut_ax0_stack)
            free(lut_ax0);
        if (lut_ax1 != lut_ax1_stack)
            free(lut_ax1);
        if (lut_bx0 != lut_bx0_stack)
            free(lut_bx0);
        if (lut_bx1 != lut_bx1_stack)
            free(lut_bx1);
        simd_stream_fence();
        return true;
    }

    case WAYWAL_TRANSITION_SLIDE: {
        float cos_a = cosf(params->angle_rad);
        float sin_a = sinf(params->angle_rad);
        float inv_span = 1.0f / fmaxf(fabsf(cos_a) + fabsf(sin_a), 1e-5f);
        float blur_strength = (params->wave_amp > 0.001f) ? params->wave_amp : 0.08f;
        float blur_mag = sinf(progress * 3.14159265f) * blur_strength;
        bool has_blur = (blur_mag >= 0.001f);
        float blur_off_x = cos_a * (blur_mag * 0.25f);
        float blur_off_y = sin_a * (blur_mag * 0.25f);

        float edge = 0.02f + blur_mag * 0.5f;
        float p = progress * (1.0f + 2.0f * edge) - edge;
        float p_plus_edge = p + edge;
        float p_minus_edge = p - edge;
        float inv_2edge = 1.0f / (2.0f * edge);

        /* Match slide.comp: uv_a = uv - dir * progress; uv_b = uv + dir * (1 - progress). */
        float shift_ax = -cos_a * progress * (float)width;
        float shift_bx = cos_a * (1.0f - progress) * (float)width;
        int ishift_ax = (int)lroundf(shift_ax);
        int ishift_bx = (int)lroundf(shift_bx);
        float d_proj = (cos_a * inv_span) / (float)width;
        float nx0 = 0.5f / (float)width - 0.5f;
        bool flat_x = (fabsf(d_proj) * (float)width < 1e-5f);
        float inv_d = flat_x ? 0.0f : (1.0f / d_proj);

        int lut_ax0_stack[4096], lut_ax1_stack[4096];
        int lut_bx0_stack[4096], lut_bx1_stack[4096];
        int *lut_ax0 = lut_ax0_stack, *lut_ax1 = lut_ax1_stack;
        int *lut_bx0 = lut_bx0_stack, *lut_bx1 = lut_bx1_stack;
        if (has_blur && width > 4096) {
            lut_ax0 = (int *)malloc((size_t)width * sizeof(int));
            lut_ax1 = (int *)malloc((size_t)width * sizeof(int));
            lut_bx0 = (int *)malloc((size_t)width * sizeof(int));
            lut_bx1 = (int *)malloc((size_t)width * sizeof(int));
            if (!lut_ax0 || !lut_ax1 || !lut_bx0 || !lut_bx1) {
                free(lut_ax0);
                free(lut_ax1);
                free(lut_bx0);
                free(lut_bx1);
                return false;
            }
        }
        if (has_blur) {
            float inv_w = 1.0f / (float)width;
            for (uint32_t x = 0; x < width; ++x) {
                float ux = ((float)x + 0.5f) * inv_w;
                float base_ax = clampf(ux - cos_a * progress, 0.0f, 1.0f);
                float base_bx = clampf(ux + cos_a * (1.0f - progress), 0.0f, 1.0f);
                int ax0 = (int)(clampf(base_ax - blur_off_x, 0.0f, 1.0f) * (float)width);
                int ax1 = (int)(clampf(base_ax + blur_off_x, 0.0f, 1.0f) * (float)width);
                int bx0 = (int)(clampf(base_bx - blur_off_x, 0.0f, 1.0f) * (float)width);
                int bx1 = (int)(clampf(base_bx + blur_off_x, 0.0f, 1.0f) * (float)width);
                if (ax0 >= (int)width)
                    ax0 = (int)width - 1;
                if (ax1 >= (int)width)
                    ax1 = (int)width - 1;
                if (bx0 >= (int)width)
                    bx0 = (int)width - 1;
                if (bx1 >= (int)width)
                    bx1 = (int)width - 1;
                lut_ax0[x] = ax0;
                lut_ax1[x] = ax1;
                lut_bx0[x] = bx0;
                lut_bx1[x] = bx1;
            }
        }

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float vy = ((float)y + 0.5f) / (float)height;
            float ny = vy - 0.5f;
            float base_ay = clampf(vy - sin_a * progress, 0.0f, 1.0f);
            float base_by = clampf(vy + sin_a * (1.0f - progress), 0.0f, 1.0f);
            int ay0 =
                (int)(clampf(base_ay - (has_blur ? blur_off_y : 0.0f), 0.0f, 1.0f) * (float)height);
            int ay1 =
                (int)(clampf(base_ay + (has_blur ? blur_off_y : 0.0f), 0.0f, 1.0f) * (float)height);
            int by0 =
                (int)(clampf(base_by - (has_blur ? blur_off_y : 0.0f), 0.0f, 1.0f) * (float)height);
            int by1 =
                (int)(clampf(base_by + (has_blur ? blur_off_y : 0.0f), 0.0f, 1.0f) * (float)height);
            if (ay0 >= (int)height)
                ay0 = (int)height - 1;
            if (ay1 >= (int)height)
                ay1 = (int)height - 1;
            if (by0 >= (int)height)
                by0 = (int)height - 1;
            if (by1 >= (int)height)
                by1 = (int)height - 1;

            const uint32_t *row_a0 = old_pixels + (size_t)ay0 * width;
            const uint32_t *row_a1 = old_pixels + (size_t)ay1 * width;
            const uint32_t *row_b0 = new_pixels + (size_t)by0 * width;
            const uint32_t *row_b1 = new_pixels + (size_t)by1 * width;
            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            float proj_0 = (nx0 * cos_a + ny * sin_a) * inv_span + 0.5f;
            float proj_end = proj_0 + (float)(width - 1U) * d_proj;
            float proj_min = proj_0 < proj_end ? proj_0 : proj_end;
            float proj_max = proj_0 > proj_end ? proj_0 : proj_end;

            if (!has_blur) {
                if (proj_min >= p_plus_edge) {
                    copy_shifted_span(dst_row, row_a0, 0, (int)width, ishift_ax, (int)width);
                    continue;
                }
                if (proj_max <= p_minus_edge) {
                    copy_shifted_span(dst_row, row_b0, 0, (int)width, ishift_bx, (int)width);
                    continue;
                }
            } else {
                if (proj_min >= p_plus_edge) {
                    for (uint32_t x = 0; x < width; ++x) {
                        dst_row[x] = avg_pixels_rounded(row_a0[lut_ax0[x]], row_a1[lut_ax1[x]]);
                    }
                    continue;
                }
                if (proj_max <= p_minus_edge) {
                    for (uint32_t x = 0; x < width; ++x) {
                        dst_row[x] = avg_pixels_rounded(row_b0[lut_bx0[x]], row_b1[lut_bx1[x]]);
                    }
                    continue;
                }
            }

            int x_seam_L = 0;
            int x_seam_R = (int)width;
            if (!flat_x) {
                float x1 = (p_minus_edge - proj_0) * inv_d;
                float x2 = (p_plus_edge - proj_0) * inv_d;
                float x_min = x1 < x2 ? x1 : x2;
                float x_max = x1 > x2 ? x1 : x2;
                x_seam_L = span_floor_clamp(x_min, (int)width);
                x_seam_R = span_ceil_clamp(x_max, (int)width);
                if (x_seam_L > x_seam_R)
                    x_seam_L = x_seam_R;
            }

            if (x_seam_L > 0) {
                if (!has_blur) {
                    if (d_proj >= 0.0f)
                        copy_shifted_span(dst_row, row_b0, 0, x_seam_L, ishift_bx, (int)width);
                    else
                        copy_shifted_span(dst_row, row_a0, 0, x_seam_L, ishift_ax, (int)width);
                } else if (d_proj >= 0.0f) {
                    for (int x = 0; x < x_seam_L; ++x)
                        dst_row[x] = avg_pixels_rounded(row_b0[lut_bx0[x]], row_b1[lut_bx1[x]]);
                } else {
                    for (int x = 0; x < x_seam_L; ++x)
                        dst_row[x] = avg_pixels_rounded(row_a0[lut_ax0[x]], row_a1[lut_ax1[x]]);
                }
            }

            for (int x = x_seam_L; x < x_seam_R; ++x) {
                float proj = proj_0 + (float)x * d_proj;
                float t = clampf((p_plus_edge - proj) * inv_2edge, 0.0f, 1.0f);
                float split = t * t * (3.0f - 2.0f * t);

                uint32_t ca, cb;
                if (has_blur) {
                    ca = avg_pixels_rounded(row_a0[lut_ax0[x]], row_a1[lut_ax1[x]]);
                    cb = avg_pixels_rounded(row_b0[lut_bx0[x]], row_b1[lut_bx1[x]]);
                } else {
                    int ax = (int)((float)x + shift_ax);
                    if (ax < 0)
                        ax = 0;
                    else if (ax >= (int)width)
                        ax = (int)width - 1;

                    int bx = (int)((float)x + shift_bx);
                    if (bx < 0)
                        bx = 0;
                    else if (bx >= (int)width)
                        bx = (int)width - 1;
                    ca = row_a0[ax];
                    cb = row_b0[bx];
                }

                if (split <= 0.0f) {
                    dst_row[x] = ca;
                } else if (split >= 1.0f) {
                    dst_row[x] = cb;
                } else {
                    uint16_t wb = (uint16_t)(split * 256.0f);
                    dst_row[x] = blend_pixel_scalar(ca, cb, wb);
                }
            }

            if (x_seam_R < (int)width) {
                if (!has_blur) {
                    if (d_proj >= 0.0f)
                        copy_shifted_span(dst_row, row_a0, x_seam_R, (int)width, ishift_ax,
                                          (int)width);
                    else
                        copy_shifted_span(dst_row, row_b0, x_seam_R, (int)width, ishift_bx,
                                          (int)width);
                } else if (d_proj >= 0.0f) {
                    for (int x = x_seam_R; x < (int)width; ++x)
                        dst_row[x] = avg_pixels_rounded(row_a0[lut_ax0[x]], row_a1[lut_ax1[x]]);
                } else {
                    for (int x = x_seam_R; x < (int)width; ++x)
                        dst_row[x] = avg_pixels_rounded(row_b0[lut_bx0[x]], row_b1[lut_bx1[x]]);
                }
            }
        }
        if (has_blur && width > 4096) {
            free(lut_ax0);
            free(lut_ax1);
            free(lut_bx0);
            free(lut_bx1);
        }
        simd_stream_fence();
        return true;
    }

    case WAYWAL_TRANSITION_GLITCH: {
        float bands = params->wave_freq > 0.0f ? params->wave_freq : 35.0f;
        float intensity = params->wave_amp > 0.0f ? params->wave_amp : 0.06f;
        float peak = sinf(progress * 3.14159265f);
        float step_t = floorf(progress * 24.0f);
        float split_uv = peak * intensity * 0.6f;
        bool has_split = (split_uv >= 0.0005f);
        int split_px = has_split ? (int)lroundf(split_uv * (float)width) : 0;

        /* Match glitch.comp: normalized 24x14 digital block grid shifted by step_t. */
        uint16_t wb_grid[14][24];
        int step_i = (int)step_t;
        for (int by = 0; by < 14; ++by) {
            for (int bx = 0; bx < 24; ++bx) {
                float block_h = iq_hash21_f(bx + step_i, by + step_i);
                float gprog = progress + (block_h - 0.5f) * peak * 0.35f;
                float m = smoothstepf(0.46f, 0.54f, gprog);
                wb_grid[by][bx] = (uint16_t)(m * 256.0f + 0.5f);
            }
        }

        uint32_t col_x0[24], col_x1[24];
        for (uint32_t bx = 0; bx < 24U; ++bx) {
            uint32_t x0 = (uint32_t)ceilf(((float)bx / 24.0f) * (float)width - 0.5f);
            uint32_t x1 = (uint32_t)ceilf(((float)(bx + 1U) / 24.0f) * (float)width - 0.5f);
            if (x0 > width)
                x0 = width;
            if (x1 > width)
                x1 = width;
            if (bx == 0U)
                x0 = 0U;
            if (bx == 23U)
                x1 = width;
            col_x0[bx] = x0;
            col_x1[bx] = x1;
        }

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float vy = ((float)y + 0.5f) / (float)height;
            int slice_idx = (int)floorf(vy * bands);
            float h = iq_hash21_f(slice_idx, step_i);
            int jitter = 0;
            if (h > 0.60f) {
                jitter = (int)lroundf((h - 0.80f) * 2.5f * intensity * peak * (float)width);
            }
            int dr = jitter + split_px;
            int dg = jitter;
            int db = jitter - split_px;

            int by = (int)floorf(vy * 14.0f);
            if (by < 0)
                by = 0;
            else if (by > 13)
                by = 13;

            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

            for (uint32_t bx_idx = 0; bx_idx < 24U; ++bx_idx) {
                uint32_t bx_start = col_x0[bx_idx];
                uint32_t bx_end = col_x1[bx_idx];
                if (bx_start >= bx_end)
                    continue;
                size_t blk_len = bx_end - bx_start;
                uint16_t wb = wb_grid[by][bx_idx];

                if (jitter == 0 && split_px == 0) {
                    blend_fn((uint8_t *)(dst_row + bx_start), (const uint8_t *)(a_row + bx_start),
                             (const uint8_t *)(b_row + bx_start), wb, blk_len * sizeof(uint32_t));
                } else {
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
                    if (has_avx2) {
                        glitch_block_avx2(dst_row, a_row, b_row, bx_start, bx_end, width, dr, dg,
                                          db, wb);
                        continue;
                    }
#endif
                    for (uint32_t x = bx_start; x < bx_end; ++x) {
                        int rx = (int)x + dr;
                        int gx = (int)x + dg;
                        int bx = (int)x + db;
                        if (rx < 0)
                            rx = 0;
                        else if (rx >= (int)width)
                            rx = (int)width - 1;
                        if (gx < 0)
                            gx = 0;
                        else if (gx >= (int)width)
                            gx = (int)width - 1;
                        if (bx < 0)
                            bx = 0;
                        else if (bx >= (int)width)
                            bx = (int)width - 1;

                        uint32_t ca = 0xFF000000U | (a_row[rx] & 0x00FF0000U) |
                                      (a_row[gx] & 0x0000FF00U) | (a_row[bx] & 0x000000FFU);
                        uint32_t cb = 0xFF000000U | (b_row[rx] & 0x00FF0000U) |
                                      (b_row[gx] & 0x0000FF00U) | (b_row[bx] & 0x000000FFU);
                        dst_row[x] = blend_pixel_scalar(ca, cb, wb);
                    }
                }
            }
        }

        simd_stream_fence();
        return true;
    }

    case WAYWAL_TRANSITION_BURN: {
        float scale = params->wave_freq > 0.0f ? params->wave_freq : 14.0f;
        float flame_width = params->wave_amp > 1e-5f ? params->wave_amp : 0.08f;
        float p = progress * (1.0f + 2.0f * flame_width) - flame_width;

        int gw1 = (int)ceilf(scale) + 2;
        int gh1 = gw1;
        int gw2 = (int)ceilf(scale * 2.1f + 1.7f) + 2;
        int gh2 = (int)ceilf(scale * 2.1f + 3.2f) + 2;
        bool use_grid_cache = (gw1 <= 64 && gh1 <= 64 && gw2 <= 144 && gh2 <= 144);

        float *grid1 = NULL;
        float *grid2 = NULL;
        float grid1_stack[64 * 64];
        float grid2_stack[144 * 144];
        if (use_grid_cache) {
            grid1 = grid1_stack;
            grid2 = grid2_stack;
            for (int j = 0; j < gh1; ++j) {
                for (int i = 0; i < gw1; ++i) {
                    float hval = iq_hash21_f(i, j);
                    grid1[(size_t)j * (size_t)gw1 + (size_t)i] =
                        (hval > 0.999884f) ? 0.999884f : hval;
                }
            }
            for (int j = 0; j < gh2; ++j) {
                for (int i = 0; i < gw2; ++i) {
                    float hval = iq_hash21_f(i, j);
                    grid2[(size_t)j * (size_t)gw2 + (size_t)i] =
                        (hval > 0.999884f) ? 0.999884f : hval;
                }
            }
        }

        float inv_w = 1.0f / (float)width;
        float inv_h = 1.0f / (float)height;
        float step1_x = scale * inv_w;
        float step2_x = (scale * 2.1f) * inv_w;

        int lut_ix1_stack[4096], lut_ix2_stack[4096];
        float lut_cx1_stack[4096], lut_cx2_stack[4096];
        int *lut_ix1 = (width <= 4096) ? lut_ix1_stack : (int *)malloc((size_t)width * sizeof(int));
        int *lut_ix2 = (width <= 4096) ? lut_ix2_stack : (int *)malloc((size_t)width * sizeof(int));
        float *lut_cx1 =
            (width <= 4096) ? lut_cx1_stack : (float *)malloc((size_t)width * sizeof(float));
        float *lut_cx2 =
            (width <= 4096) ? lut_cx2_stack : (float *)malloc((size_t)width * sizeof(float));
        if (!lut_ix1 || !lut_ix2 || !lut_cx1 || !lut_cx2) {
            if (lut_ix1 && lut_ix1 != lut_ix1_stack)
                free(lut_ix1);
            if (lut_ix2 && lut_ix2 != lut_ix2_stack)
                free(lut_ix2);
            if (lut_cx1 && lut_cx1 != lut_cx1_stack)
                free(lut_cx1);
            if (lut_cx2 && lut_cx2 != lut_cx2_stack)
                free(lut_cx2);
            return false;
        }

        for (uint32_t x = 0; x < width; ++x) {
            float x1 = ((float)x + 0.5f) * step1_x;
            int ix1 = (int)floorf(x1);
            if (ix1 < 0)
                ix1 = 0;
            if (ix1 > gw1 - 2)
                ix1 = gw1 - 2;
            float fx1 = x1 - (float)ix1;
            lut_ix1[x] = ix1;
            lut_cx1[x] = fx1 * fx1 * (3.0f - 2.0f * fx1);

            float x2 = ((float)x + 0.5f) * step2_x + 1.7f;
            int ix2 = (int)floorf(x2);
            if (ix2 < 0)
                ix2 = 0;
            if (ix2 > gw2 - 2)
                ix2 = gw2 - 2;
            float fx2 = x2 - (float)ix2;
            lut_ix2[x] = ix2;
            lut_cx2[x] = fx2 * fx2 * (3.0f - 2.0f * fx2);
        }

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float ny = ((float)y + 0.5f) * inv_h;
            float y1 = ny * scale;
            int iy1 = (int)floorf(y1);
            if (iy1 < 0)
                iy1 = 0;
            if (iy1 > gh1 - 2)
                iy1 = gh1 - 2;
            float fy1 = y1 - (float)iy1;
            float cy1 = fy1 * fy1 * (3.0f - 2.0f * fy1);

            float y2 = (ny * scale) * 2.1f + 3.2f;
            int iy2 = (int)floorf(y2);
            if (iy2 < 0)
                iy2 = 0;
            if (iy2 > gh2 - 2)
                iy2 = gh2 - 2;
            float fy2 = y2 - (float)iy2;
            float cy2 = fy2 * fy2 * (3.0f - 2.0f * fy2);

            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

            if (use_grid_cache) {
                float v1[64], dv1[64];
                float v2[144], dv2[144];
                const float *g1_r0 = grid1 + (size_t)iy1 * (size_t)gw1;
                const float *g1_r1 = grid1 + (size_t)(iy1 + 1) * (size_t)gw1;
                for (int i = 0; i < gw1; ++i) {
                    v1[i] = g1_r0[i] * (1.0f - cy1) + g1_r1[i] * cy1;
                }
                for (int i = 0; i < gw1 - 1; ++i) {
                    dv1[i] = v1[i + 1] - v1[i];
                }
                dv1[gw1 - 1] = 0.0f;

                const float *g2_r0 = grid2 + (size_t)iy2 * (size_t)gw2;
                const float *g2_r1 = grid2 + (size_t)(iy2 + 1) * (size_t)gw2;
                for (int i = 0; i < gw2; ++i) {
                    v2[i] = g2_r0[i] * (1.0f - cy2) + g2_r1[i] * cy2;
                }
                for (int i = 0; i < gw2 - 1; ++i) {
                    dv2[i] = v2[i + 1] - v2[i];
                }
                dv2[gw2 - 1] = 0.0f;

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
                if (has_avx2) {
                    burn_row_avx2(dst_row, a_row, b_row, v1, dv1, v2, dv2, lut_ix1, lut_cx1,
                                  lut_ix2, lut_cx2, width, p, flame_width);
                    continue;
                }
#endif
                for (uint32_t x = 0; x < width; ++x) {
                    int ix1 = lut_ix1[x];
                    float n1 = v1[ix1] + dv1[ix1] * lut_cx1[x];
                    int ix2 = lut_ix2[x];
                    float n2 = v2[ix2] + dv2[ix2] * lut_cx2[x];
                    float n = 0.65f * n1 + 0.35f * n2;
                    if (n > 0.99988f)
                        n = 0.99988f;

                    float d = n - p;
                    if (d >= flame_width) {
                        dst_row[x] = a_row[x];
                    } else if (d <= -flame_width) {
                        dst_row[x] = b_row[x];
                    } else {
                        float burn_factor = 1.0f - smoothstepf(-flame_width, flame_width, d);
                        uint16_t wb = (uint16_t)(burn_factor * 256.0f);
                        uint32_t base = blend_pixel_scalar(a_row[x], b_row[x], wb);

                        float dist_norm = 1.0f - fabsf(d) / flame_width;
                        float heat = dist_norm * sqrtf(dist_norm);
                        float char_scale = 1.0f - heat * 0.5f;
                        float fr_f = 0.0f, fg_f = 0.0f, fb_f = 0.0f;
                        fire_color_rgb(heat, &fr_f, &fg_f, &fb_f);
                        float gain = heat * 1.8f * 255.0f;

                        int r = (int)((float)((base >> 16) & 0xFFu) * char_scale + fr_f * gain);
                        int g = (int)((float)((base >> 8) & 0xFFu) * char_scale + fg_f * gain);
                        int b = (int)((float)(base & 0xFFu) * char_scale + fb_f * gain);
                        if (r > 255)
                            r = 255;
                        if (g > 255)
                            g = 255;
                        if (b > 255)
                            b = 255;
                        dst_row[x] =
                            0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                    }
                }
            } else {
                for (uint32_t x = 0; x < width; ++x) {
                    int ix1 = lut_ix1[x];
                    float a1 =
                        iq_hash21_f(ix1, iy1) * (1.0f - cy1) + iq_hash21_f(ix1, iy1 + 1) * cy1;
                    float b1 = iq_hash21_f(ix1 + 1, iy1) * (1.0f - cy1) +
                               iq_hash21_f(ix1 + 1, iy1 + 1) * cy1;
                    float n1 = a1 + (b1 - a1) * lut_cx1[x];

                    int ix2 = lut_ix2[x];
                    float a2 =
                        iq_hash21_f(ix2, iy2) * (1.0f - cy2) + iq_hash21_f(ix2, iy2 + 1) * cy2;
                    float b2 = iq_hash21_f(ix2 + 1, iy2) * (1.0f - cy2) +
                               iq_hash21_f(ix2 + 1, iy2 + 1) * cy2;
                    float n2 = a2 + (b2 - a2) * lut_cx2[x];

                    float n = 0.65f * n1 + 0.35f * n2;
                    if (n > 0.99988f)
                        n = 0.99988f;

                    float d = n - p;
                    if (d >= flame_width) {
                        dst_row[x] = a_row[x];
                    } else if (d <= -flame_width) {
                        dst_row[x] = b_row[x];
                    } else {
                        float burn_factor = 1.0f - smoothstepf(-flame_width, flame_width, d);
                        uint16_t wb = (uint16_t)(burn_factor * 256.0f);
                        uint32_t base = blend_pixel_scalar(a_row[x], b_row[x], wb);

                        float dist_norm = 1.0f - fabsf(d) / flame_width;
                        float heat = dist_norm * sqrtf(dist_norm);
                        float char_scale = 1.0f - heat * 0.5f;
                        float fr_f = 0.0f, fg_f = 0.0f, fb_f = 0.0f;
                        fire_color_rgb(heat, &fr_f, &fg_f, &fb_f);
                        float gain = heat * 1.8f * 255.0f;

                        int r = (int)((float)((base >> 16) & 0xFFu) * char_scale + fr_f * gain);
                        int g = (int)((float)((base >> 8) & 0xFFu) * char_scale + fg_f * gain);
                        int b = (int)((float)(base & 0xFFu) * char_scale + fb_f * gain);
                        if (r > 255)
                            r = 255;
                        if (g > 255)
                            g = 255;
                        if (b > 255)
                            b = 255;
                        dst_row[x] =
                            0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                    }
                }
            }
        }

        if (lut_ix1 != lut_ix1_stack)
            free(lut_ix1);
        if (lut_ix2 != lut_ix2_stack)
            free(lut_ix2);
        if (lut_cx1 != lut_cx1_stack)
            free(lut_cx1);
        if (lut_cx2 != lut_cx2_stack)
            free(lut_cx2);
        simd_stream_fence();
        return true;
    }

    case WAYWAL_TRANSITION_RIPPLE: {
        float cx = 0.5f, cy = 0.5f;
        resolve_center(params, &cx, &cy);
        float aspect = (float)width / (float)height;
        float max_x = cx > (1.0f - cx) ? cx : (1.0f - cx);
        float max_y = cy > (1.0f - cy) ? cy : (1.0f - cy);
        float max_dist = sqrtf((max_x * aspect) * (max_x * aspect) + max_y * max_y);
        float wave_width = 0.18f;
        float current_radius = -0.03f + progress * (max_dist + wave_width + 0.03f);
        float freq = params->wave_freq > 0.0f ? params->wave_freq : 32.0f;
        float amp = params->wave_amp > 1e-5f ? params->wave_amp : 0.04f;
        float env = smoothstepf(0.0f, 0.08f, progress) * (1.0f - progress) * (1.0f - progress);
        float decay = env;

        float r_inner = current_radius - wave_width;
        float r_outer = current_radius + wave_width;
        float r_inner2 = r_inner > 0.0f ? r_inner * r_inner : 0.0f;
        float r_outer2 = r_outer > 0.0f ? r_outer * r_outer : 0.0f;

        float xc_px = cx * (float)width;
        float scale_x = (float)height;
        float step_dx = aspect / (float)width;
        float cx_scaled = cx * aspect;
        float inv_w = 1.0f / (float)width;
        float inv_h = 1.0f / (float)height;

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float vy = ((float)y + 0.5f) * inv_h;
            float dy = vy - cy;
            float dy2 = dy * dy;
            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

            if (r_outer <= 0.0f || dy2 >= r_outer2) {
                memcpy(dst_row, a_row, (size_t)width * sizeof(uint32_t));
                continue;
            }

            float dx_out = scale_x * sqrtf(r_outer2 - dy2);
            int x_out_L = span_floor_clamp(xc_px - dx_out - 0.5f, (int)width);
            int x_out_R = span_ceil_clamp(xc_px + dx_out - 0.5f, (int)width);
            if (x_out_L > x_out_R)
                x_out_L = x_out_R;

            int x_in_L = x_out_L;
            int x_in_R = x_out_L;
            if (r_inner > 0.0f && dy2 < r_inner2) {
                float dx_in = scale_x * sqrtf(r_inner2 - dy2);
                x_in_L = span_ceil_clamp(xc_px - dx_in - 0.5f, (int)width);
                x_in_R = span_floor_clamp(xc_px + dx_in - 0.5f, (int)width);
                if (x_in_L < x_out_L)
                    x_in_L = x_out_L;
                if (x_in_R > x_out_R)
                    x_in_R = x_out_R;
                if (x_in_L > x_in_R)
                    x_in_L = x_in_R;
            }

            if (x_out_L > 0) {
                memcpy(dst_row, a_row, (size_t)x_out_L * sizeof(uint32_t));
            }

            int spans[2][2] = {{x_out_L, x_in_L}, {x_in_R, x_out_R}};

            if (x_in_R > x_in_L) {
                memcpy(dst_row + x_in_L, b_row + x_in_L,
                       (size_t)(x_in_R - x_in_L) * sizeof(uint32_t));
            }

            for (int s = 0; s < 2; ++s) {
                int x = spans[s][0];
                int x_end = spans[s][1];
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
                if (has_avx2 && x_end - x >= 8) {
                    x = ripple_annulus_avx2(dst_row, old_pixels, new_pixels, x, x_end, (int)y,
                                            cx_scaled, step_dx, dy, dy2, current_radius, wave_width,
                                            freq, amp, decay, (int)width, (int)height);
                }
#endif
                for (; x < x_end; ++x) {
                    float dx = ((float)x + 0.5f) * step_dx - cx_scaled;
                    float dist2 = dx * dx + dy2;
                    if (dist2 >= r_outer2) {
                        dst_row[x] = a_row[x];
                    } else if (r_inner > 0.0f && dist2 <= r_inner2) {
                        dst_row[x] = b_row[x];
                    } else {
                        float dist = sqrtf(dist2);
                        float diff = dist - current_radius;
                        float window = 1.0f - smoothstepf(0.0f, wave_width, fabsf(diff));
                        float sin_w = sinf(diff * freq) * window;
                        float wave = sin_w * amp * decay;
                        float crest = fmaxf(0.0f, -sin_w) * decay * decay * 0.22f;

                        float inv_dist = (dist > 0.0001f) ? (1.0f / dist) : 0.0f;
                        float ndx = dx * inv_dist / aspect;
                        float ndy = dy * inv_dist;

                        float ref_u = clampf(((float)x + 0.5f) * inv_w + ndx * wave, 0.0f, 1.0f);
                        float ref_v = clampf(vy + ndy * wave, 0.0f, 1.0f);
                        int sx = (int)(ref_u * (float)width);
                        int sy = (int)(ref_v * (float)height);
                        if (sx >= (int)width)
                            sx = (int)width - 1;
                        if (sy >= (int)height)
                            sy = (int)height - 1;

                        uint32_t ca = old_pixels[(size_t)sy * width + sx];
                        uint32_t cb = new_pixels[(size_t)sy * width + sx];

                        float split =
                            smoothstepf(current_radius - 0.02f, current_radius + 0.02f, dist);
                        uint16_t wa = (uint16_t)(split * 256.0f);
                        uint32_t res = blend_pixel_scalar(cb, ca, wa);
                        int c_add = (int)(crest * 255.0f);
                        dst_row[x] = (c_add > 0) ? add_saturate_rgb(res, c_add) : res;
                    }
                }
            }

            if (x_out_R < (int)width) {
                memcpy(dst_row + x_out_R, a_row + x_out_R,
                       (size_t)((int)width - x_out_R) * sizeof(uint32_t));
            }
        }
        simd_stream_fence();
        return true;
    }

    case WAYWAL_TRANSITION_PIXELATE: {
        float d = sinf(progress * 3.1415926535f) * 64.0f + 1.0f;
        float inv_d = 1.0f / d;
        uint16_t wb = (uint16_t)(progress * 256.0f + 0.5f);
        if (wb > 256U)
            wb = 256U;

        if (d < 1.5f) {
#pragma omp parallel for schedule(static, 8)
            for (uint32_t y = 0; y < height; ++y) {
                blend_fn((uint8_t *)(dst + (size_t)y * stride_pixels),
                         (const uint8_t *)(old_pixels + (size_t)y * width),
                         (const uint8_t *)(new_pixels + (size_t)y * width), wb,
                         (size_t)width * sizeof(uint32_t));
            }
            simd_stream_fence();
            return true;
        }

        uint32_t max_tx = width;
        uint32_t *tx_starts = (uint32_t *)malloc((size_t)(max_tx + 1U) * sizeof(uint32_t));
        uint32_t *tx_sxs = (uint32_t *)malloc((size_t)max_tx * sizeof(uint32_t));
        uint32_t *ty_starts = (uint32_t *)malloc((size_t)(height + 1U) * sizeof(uint32_t));
        uint32_t *ty_sys = (uint32_t *)malloc((size_t)height * sizeof(uint32_t));
        if (!tx_starts || !tx_sxs || !ty_starts || !ty_sys) {
            free(tx_starts);
            free(tx_sxs);
            free(ty_starts);
            free(ty_sys);
            return false;
        }

        uint32_t num_tx = 0;
        uint32_t x_cur = 0;
        while (x_cur < width) {
            float tx_f = floorf((float)x_cur * inv_d);
            uint32_t x_next = x_cur + 1U;
            while (x_next < width && floorf((float)x_next * inv_d) == tx_f) {
                x_next++;
            }
            int sx = (int)floorf(clampf(tx_f * d + d * 0.5f, 0.5f, (float)width - 0.5f));
            if (sx < 0)
                sx = 0;
            else if (sx >= (int)width)
                sx = (int)width - 1;
            tx_starts[num_tx] = x_cur;
            tx_sxs[num_tx] = (uint32_t)sx;
            num_tx++;
            x_cur = x_next;
        }
        tx_starts[num_tx] = width;

        uint32_t num_ty = 0;
        uint32_t y_cur = 0;
        while (y_cur < height) {
            float ty_f = floorf((float)y_cur * inv_d);
            uint32_t y_next = y_cur + 1U;
            while (y_next < height && floorf((float)y_next * inv_d) == ty_f) {
                y_next++;
            }
            int sy = (int)floorf(clampf(ty_f * d + d * 0.5f, 0.5f, (float)height - 0.5f));
            if (sy < 0)
                sy = 0;
            else if (sy >= (int)height)
                sy = (int)height - 1;
            ty_starts[num_ty] = y_cur;
            ty_sys[num_ty] = (uint32_t)sy;
            num_ty++;
            y_cur = y_next;
        }
        ty_starts[num_ty] = height;

#pragma omp parallel for schedule(dynamic, 2)
        for (uint32_t ty = 0; ty < num_ty; ++ty) {
            uint32_t by = ty_starts[ty];
            uint32_t by_end = ty_starts[ty + 1U];
            uint32_t sy = ty_sys[ty];

            const uint32_t *a_row = old_pixels + (size_t)sy * width;
            const uint32_t *b_row = new_pixels + (size_t)sy * width;
            uint32_t *first_row = dst + (size_t)by * stride_pixels;

            for (uint32_t tx = 0; tx < num_tx; ++tx) {
                uint32_t sx = tx_sxs[tx];
                uint32_t blended = blend_pixel_scalar(a_row[sx], b_row[sx], wb);
                uint32_t x0 = tx_starts[tx];
                uint32_t x1 = tx_starts[tx + 1U];
                for (uint32_t x = x0; x < x1; ++x) {
                    first_row[x] = blended;
                }
            }

            for (uint32_t y = by + 1U; y < by_end; ++y) {
                memcpy(dst + (size_t)y * stride_pixels, first_row,
                       (size_t)width * sizeof(uint32_t));
            }
        }

        free(tx_starts);
        free(tx_sxs);
        free(ty_starts);
        free(ty_sys);
        return true;
    }

    case WAYWAL_TRANSITION_DOOM: {
        const uint32_t col_width = 4;
        uint32_t num_cols = (width + col_width - 1) / col_width;
        int shift_stack[2048];
        int *pixel_shift_lut =
            (num_cols <= 2048) ? shift_stack : (int *)malloc((size_t)num_cols * sizeof(int));
        if (!pixel_shift_lut)
            return false;

        for (uint32_t c = 0; c < num_cols; ++c) {
            float p_hash = (float)c * 0.1031f;
            p_hash -= floorf(p_hash);
            p_hash *= p_hash + 33.33f;
            p_hash *= p_hash + p_hash;
            float delay = (p_hash - floorf(p_hash)) * 0.35f;

            float shift = clampf(progress * 1.35f - delay, 0.0f, 1.0f);
            pixel_shift_lut[c] = (int)lroundf(shift * (float)height);
        }

        /* Tile columns in 64-px strips so vertical row gathers stay resident in L2 cache. */
        const uint32_t cols_per_strip = 16U;
        uint32_t num_strips = (num_cols + cols_per_strip - 1U) / cols_per_strip;

#pragma omp parallel for schedule(static)
        for (uint32_t s = 0; s < num_strips; ++s) {
            uint32_t c_start = s * cols_per_strip;
            uint32_t c_end = c_start + cols_per_strip;
            if (c_end > num_cols)
                c_end = num_cols;

            for (uint32_t y = 0; y < height; ++y) {
                uint32_t *dst_row = dst + (size_t)y * stride_pixels;
                const uint32_t *new_row = new_pixels + (size_t)y * width;

                for (uint32_t c = c_start; c < c_end; ++c) {
                    uint32_t bx = c * col_width;
                    uint32_t bw = (bx + col_width <= width) ? col_width : (width - bx);
                    int pixel_shift = pixel_shift_lut[c];
                    const uint32_t *src_p;
                    if ((int)y < pixel_shift) {
                        src_p = new_row + bx;
                    } else {
                        int sy = (int)y - pixel_shift;
                        if (sy < 0)
                            sy = 0;
                        else if (sy >= (int)height)
                            sy = (int)height - 1;
                        src_p = old_pixels + (size_t)sy * width + bx;
                    }
                    uint32_t *dst_p = dst_row + bx;
                    if (bw == 4U) {
                        dst_p[0] = src_p[0];
                        dst_p[1] = src_p[1];
                        dst_p[2] = src_p[2];
                        dst_p[3] = src_p[3];
                    } else {
                        for (uint32_t k = 0; k < bw; ++k) {
                            dst_p[k] = src_p[k];
                        }
                    }
                }
            }
        }

        if (pixel_shift_lut != shift_stack)
            free(pixel_shift_lut);
        return true;
    }

    case WAYWAL_TRANSITION_SWIRL: {
        float cx = 0.5f, cy = 0.5f;
        resolve_center(params, &cx, &cy);
        float angle_amt = sinf(progress * 3.1415926535f) * 8.0f;
        float max_r = 0.85f;
        uint16_t wb = (uint16_t)(progress * 256.0f + 0.5f);
        if (wb > 256U)
            wb = 256U;
        uint32_t wa = 256U - wb;

        if (fabsf(angle_amt) < 1e-4f) {
#pragma omp parallel for schedule(static, 8)
            for (uint32_t y = 0; y < height; ++y) {
                blend_fn((uint8_t *)(dst + (size_t)y * stride_pixels),
                         (const uint8_t *)(old_pixels + (size_t)y * width),
                         (const uint8_t *)(new_pixels + (size_t)y * width), wb,
                         (size_t)width * sizeof(uint32_t));
            }
            simd_stream_fence();
            return true;
        }

        float max_dist = max_r * (float)height;
        float inv_max_dist = 1.0f / max_dist;
        float max_dist2 = max_dist * max_dist;
        float cx_pixel = cx * (float)width;
        float cy_pixel = cy * (float)height;

        /* Tile into 64x64 L2-resident blocks so source rotations hit local cache instead of
         * thrashing DRAM. */
        const uint32_t tile_sz = 64U;
        uint32_t num_tx = (width + tile_sz - 1U) / tile_sz;
        uint32_t num_ty = (height + tile_sz - 1U) / tile_sz;
        uint32_t total_tiles = num_tx * num_ty;

#ifdef _OPENMP
        int swirl_threads = omp_get_max_threads();
        if (!getenv("OMP_NUM_THREADS")) {
            int np = omp_get_num_procs();
            swirl_threads = (np >= 6 && np <= 12) ? 6 : np;
        } else if (swirl_threads > 6 && omp_get_num_procs() <= 12) {
            /* Keep one thread per physical core to avoid 12T SMT L1/DRAM contention. */
            swirl_threads = 6;
        }
        if (swirl_threads < 1) {
            swirl_threads = 1;
        }
#if defined(_OPENMP) && (_OPENMP >= 201307)
#pragma omp parallel for schedule(dynamic, 4) num_threads(swirl_threads) proc_bind(spread)
#else
#pragma omp parallel for schedule(dynamic, 4) num_threads(swirl_threads)
#endif
#endif
        for (uint32_t tidx = 0; tidx < total_tiles; ++tidx) {
            uint32_t tx0 = (tidx % num_tx) * tile_sz;
            uint32_t ty0 = (tidx / num_tx) * tile_sz;
            uint32_t tx1 = tx0 + tile_sz;
            uint32_t ty1 = ty0 + tile_sz;
            if (tx1 > width)
                tx1 = width;
            if (ty1 > height)
                ty1 = height;

            float x_lo = (float)tx0 + 0.5f;
            float x_hi = (float)tx1 - 0.5f;
            float y_lo = (float)ty0 + 0.5f;
            float y_hi = (float)ty1 - 0.5f;

            float near_dx = clampf(cx_pixel, x_lo, x_hi) - cx_pixel;
            float near_dy = clampf(cy_pixel, y_lo, y_hi) - cy_pixel;
            float d2_min = near_dx * near_dx + near_dy * near_dy;

            if (d2_min >= max_dist2) {
                size_t row_bytes = (size_t)(tx1 - tx0) * sizeof(uint32_t);
                for (uint32_t y = ty0; y < ty1; ++y) {
                    blend_fn((uint8_t *)(dst + (size_t)y * stride_pixels + tx0),
                             (const uint8_t *)(old_pixels + (size_t)y * width + tx0),
                             (const uint8_t *)(new_pixels + (size_t)y * width + tx0), wb,
                             row_bytes);
                }
                continue;
            }

            float far_dx = fmaxf(fabsf(x_lo - cx_pixel), fabsf(x_hi - cx_pixel));
            float far_dy = fmaxf(fabsf(y_lo - cy_pixel), fabsf(y_hi - cy_pixel));
            bool interior_only = (far_dx * far_dx + far_dy * far_dy) < max_dist2;

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
            if (has_avx2) {
                swirl_tile_avx2(dst, old_pixels, new_pixels, (int)tx0, (int)tx1, (int)ty0, (int)ty1,
                                (int)width, (int)height, (int)stride_pixels, cx_pixel, cy_pixel,
                                inv_max_dist, max_dist2, angle_amt, wa, wb, interior_only,
                                blend_fn);
                continue;
            }
#endif
            for (uint32_t y = ty0; y < ty1; ++y) {
                float y_rel = ((float)y + 0.5f) - cy_pixel;
                float y_rel2 = y_rel * y_rel;
                uint32_t *dst_row = dst + (size_t)y * stride_pixels;
                const uint32_t *a_row = old_pixels + (size_t)y * width;
                const uint32_t *b_row = new_pixels + (size_t)y * width;

                int xin0 = (int)tx0;
                int xin1 = (int)tx1;
                if (!interior_only) {
                    compute_swirl_chord((int)tx0, (int)tx1, cx_pixel, y_rel2, max_dist2, &xin0,
                                        &xin1);
                    if (xin0 > (int)tx0) {
                        blend_fn((uint8_t *)(dst_row + tx0), (const uint8_t *)(a_row + tx0),
                                 (const uint8_t *)(b_row + tx0), wb,
                                 (size_t)(xin0 - (int)tx0) * sizeof(uint32_t));
                    }
                }

                for (int x = xin0; x < xin1; ++x) {
                    float x_rel = ((float)x + 0.5f) - cx_pixel;
                    float d2 = x_rel * x_rel + y_rel2;
                    float dist = sqrtf(d2);
                    float factor = 1.0f - dist * inv_max_dist;
                    if (factor < 0.0f)
                        factor = 0.0f;
                    float theta = factor * factor * angle_amt;
                    float sin_t = sinf(theta);
                    float cos_t = cosf(theta);

                    float px = cx_pixel + x_rel * cos_t - y_rel * sin_t;
                    float py = cy_pixel + x_rel * sin_t + y_rel * cos_t;

                    int sx = (int)px;
                    int sy = (int)py;
                    if (sx < 0)
                        sx = 0;
                    else if (sx >= (int)width)
                        sx = (int)width - 1;
                    if (sy < 0)
                        sy = 0;
                    else if (sy >= (int)height)
                        sy = (int)height - 1;

                    uint32_t ca = old_pixels[(size_t)sy * width + (size_t)sx];
                    uint32_t cb = new_pixels[(size_t)sy * width + (size_t)sx];
                    dst_row[x] = blend_pixel_fast(ca, cb, wa, wb);
                }

                if (!interior_only && xin1 < (int)tx1) {
                    blend_fn((uint8_t *)(dst_row + xin1), (const uint8_t *)(a_row + xin1),
                             (const uint8_t *)(b_row + xin1), wb,
                             (size_t)((int)tx1 - xin1) * sizeof(uint32_t));
                }
            }
        }
        simd_stream_fence();
        return true;
    }

    case WAYWAL_TRANSITION_CUBE: {
        /* Keep camera focal length, Z-pullback, and culling synchronized with cube.comp. */
        float theta = progress * 1.57079632679f;
        float cos_t = cosf(theta);
        float sin_t = sinf(theta);
        const float d_cam = 2.5f;
        const float z_pull = 0.75f;
        float dist = (d_cam + 1.0f) + z_pull * sinf(progress * 3.14159265359f);

        float ro_cube_x = dist * sin_t;
        float ro_cube_z = -dist * cos_t;

        float shade_front = 0.65f + 0.35f * cos_t;
        float shade_right = 0.65f + 0.35f * sin_t;
        uint16_t wb_front = (uint16_t)(shade_front * 256.0f + 0.5f);
        uint16_t wb_right = (uint16_t)(shade_right * 256.0f + 0.5f);
        if (wb_front > 256U)
            wb_front = 256U;
        if (wb_right > 256U)
            wb_right = 256U;

        float inv_w = 1.0f / (float)width;
        float inv_h = 1.0f / (float)height;

        int col_sx_stack[4096];
        float col_ky_stack[4096];
        int *col_sx = (width <= 4096) ? col_sx_stack : (int *)malloc((size_t)width * sizeof(int));
        float *col_ky =
            (width <= 4096) ? col_ky_stack : (float *)malloc((size_t)width * sizeof(float));
        if (!col_sx || !col_ky) {
            if (col_sx && col_sx != col_sx_stack)
                free(col_sx);
            if (col_ky && col_ky != col_ky_stack)
                free(col_ky);
            return false;
        }

        int front_x0 = (int)width, front_x1 = 0;
        int right_x0 = (int)width, right_x1 = 0;

        for (uint32_t x = 0; x < width; ++x) {
            float rdx = (((float)x + 0.5f) * inv_w - 0.5f) * 2.0f;
            float rdz = d_cam;

            float rd_cube_x = rdx * cos_t - rdz * sin_t;
            float rd_cube_z = rdx * sin_t + rdz * cos_t;

            float t_f = -1.0f;
            float hx = 0.0f;
            if (ro_cube_z < -1.0f && rd_cube_z > 1e-6f) {
                float t = (-1.0f - ro_cube_z) / rd_cube_z;
                if (t > 0.0f) {
                    hx = ro_cube_x + t * rd_cube_x;
                    if (hx >= -1.0f && hx <= 1.0f + 1e-5f) {
                        t_f = t;
                    }
                }
            }

            float t_r = -1.0f;
            float hz = 0.0f;
            if (ro_cube_x > 1.0f && rd_cube_x < -1e-6f) {
                float t = (1.0f - ro_cube_x) / rd_cube_x;
                if (t > 0.0f) {
                    hz = ro_cube_z + t * rd_cube_z;
                    if (hz >= -1.0f - 1e-5f && hz <= 1.0f) {
                        t_r = t;
                    }
                }
            }

            if (t_f > 0.0f && (t_r < 0.0f || t_f <= t_r)) {
                float u_f = clampf(hx * 0.5f + 0.5f, 0.0f, 1.0f);
                int sx = (int)(u_f * (float)width);
                if (sx < 0)
                    sx = 0;
                else if (sx >= (int)width)
                    sx = (int)width - 1;
                col_sx[x] = sx;
                col_ky[x] = t_f;
                if ((int)x < front_x0)
                    front_x0 = (int)x;
                front_x1 = (int)x + 1;
            } else if (t_r > 0.0f) {
                float u_r = clampf(hz * 0.5f + 0.5f, 0.0f, 1.0f);
                int sx = (int)(u_r * (float)width);
                if (sx < 0)
                    sx = 0;
                else if (sx >= (int)width)
                    sx = (int)width - 1;
                col_sx[x] = sx;
                col_ky[x] = t_r;
                if ((int)x < right_x0)
                    right_x0 = (int)x;
                right_x1 = (int)x + 1;
            } else {
                col_sx[x] = 0;
                col_ky[x] = 1e30f;
            }
        }

        int max_y = (int)height - 1;
        float y_bias = 0.5f * (float)height;

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float py = (((float)y + 0.5f) * inv_h - 0.5f) * 2.0f;
            float abs_py = fabsf(py);
            float y_scale = py * 0.5f * (float)height;
            uint32_t *dst_row = dst + (size_t)y * stride_pixels;

            /* col_ky decreases monotonically across the front face and increases monotonically
             * across the right face, allowing binary search for the vertical frustum clip. */
            int xf0 = front_x1;
            int xf1 = front_x1;
            if (front_x0 < front_x1 && abs_py * col_ky[front_x1 - 1] <= 1.0f) {
                if (abs_py * col_ky[front_x0] <= 1.0f) {
                    xf0 = front_x0;
                } else {
                    int lo = front_x0, hi = front_x1 - 1;
                    while (lo < hi) {
                        int mid = (lo + hi) >> 1;
                        if (abs_py * col_ky[mid] <= 1.0f)
                            hi = mid;
                        else
                            lo = mid + 1;
                    }
                    xf0 = lo;
                }
            }

            int xr0 = right_x0;
            int xr1 = right_x0;
            if (right_x0 < right_x1 && abs_py * col_ky[right_x0] <= 1.0f) {
                if (abs_py * col_ky[right_x1 - 1] <= 1.0f) {
                    xr1 = right_x1;
                } else {
                    int lo = right_x0, hi = right_x1;
                    while (lo < hi) {
                        int mid = (lo + hi) >> 1;
                        if (abs_py * col_ky[mid] <= 1.0f)
                            lo = mid + 1;
                        else
                            hi = mid;
                    }
                    xr1 = lo;
                }
            }

            int cursor = 0;
            if (xf0 < xf1) {
                if (xf0 > cursor)
                    fill_u32_span(dst_row + cursor, 0xFF08080DU, xf0 - cursor);
                int x = xf0;
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
                if (has_avx2 && xf1 - x >= 8) {
                    x = cube_face_span_avx2(dst_row, old_pixels, col_sx, col_ky, x, xf1, y_scale,
                                            y_bias, (int)width, max_y, wb_front);
                }
#endif
                for (; x < xf1; ++x) {
                    int sy = (int)(col_ky[x] * y_scale + y_bias);
                    if (sy < 0)
                        sy = 0;
                    else if (sy > max_y)
                        sy = max_y;
                    uint32_t c = old_pixels[(size_t)sy * width + col_sx[x]];
                    dst_row[x] = blend_pixel_scalar(0xFF000000, c, wb_front);
                }
                cursor = xf1;
            }

            if (xr0 < xr1) {
                if (xr0 > cursor)
                    fill_u32_span(dst_row + cursor, 0xFF08080DU, xr0 - cursor);
                int x = xr0;
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
                if (has_avx2 && xr1 - x >= 8) {
                    x = cube_face_span_avx2(dst_row, new_pixels, col_sx, col_ky, x, xr1, y_scale,
                                            y_bias, (int)width, max_y, wb_right);
                }
#endif
                for (; x < xr1; ++x) {
                    int sy = (int)(col_ky[x] * y_scale + y_bias);
                    if (sy < 0)
                        sy = 0;
                    else if (sy > max_y)
                        sy = max_y;
                    uint32_t c = new_pixels[(size_t)sy * width + col_sx[x]];
                    dst_row[x] = blend_pixel_scalar(0xFF000000, c, wb_right);
                }
                cursor = xr1;
            }

            if (cursor < (int)width) {
                fill_u32_span(dst_row + cursor, 0xFF08080DU, (int)width - cursor);
            }
        }

        if (col_sx != col_sx_stack)
            free(col_sx);
        if (col_ky != col_ky_stack)
            free(col_ky);
        simd_stream_fence();
        return true;
    }

    case WAYWAL_TRANSITION_LUMA: {
        float threshold = progress * 1.2f - 0.1f;
        float softness = 0.08f;
        float edge0 = threshold - softness;
        float inv_span = 1.0f / (2.0f * softness);
        float aspect = (float)width / (float)height;
        float dp1_x = (aspect * 4.0f) / (float)width;
        float p1_x0 = 0.5f * dp1_x;
        float dp1_y = 4.0f / (float)height;
        float glow_env = sinf(progress * 3.1415926535f) * 0.25f;

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float p1_y = ((float)y + 0.5f) * dp1_y;
            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
            if (has_avx2) {
                luma_row_avx2(dst_row, a_row, b_row, width, p1_x0, dp1_x, p1_y, threshold, edge0,
                              inv_span, glow_env);
                continue;
            }
#endif
            for (uint32_t x = 0; x < width; ++x) {
                float p1x = p1_x0 + (float)x * dp1_x;
                float p2x = (0.8f * p1x - 0.6f * p1_y) * 2.0f + 100.0f;
                float p2y = (0.6f * p1x + 0.8f * p1_y) * 2.0f + 100.0f;

                int ix1 = (int)floorf(p1x), iy1 = (int)floorf(p1_y);
                float fx1 = p1x - (float)ix1, fy1 = p1_y - (float)iy1;
                float ux1 = fx1 * fx1 * (3.0f - 2.0f * fx1);
                float uy1 = fy1 * fy1 * (3.0f - 2.0f * fy1);
                float g00x, g00y, g10x, g10y, g01x, g01y, g11x, g11y;
                iq_hash2_vec(ix1, iy1, &g00x, &g00y);
                iq_hash2_vec(ix1 + 1, iy1, &g10x, &g10y);
                iq_hash2_vec(ix1, iy1 + 1, &g01x, &g01y);
                iq_hash2_vec(ix1 + 1, iy1 + 1, &g11x, &g11y);
                float d00 = g00x * fx1 + g00y * fy1;
                float d10 = g10x * (fx1 - 1.0f) + g10y * fy1;
                float d01 = g01x * fx1 + g01y * (fy1 - 1.0f);
                float d11 = g11x * (fx1 - 1.0f) + g11y * (fy1 - 1.0f);
                float x0_v = d00 + (d10 - d00) * ux1;
                float x1_v = d01 + (d11 - d01) * ux1;
                float n1 = x0_v + (x1_v - x0_v) * uy1;

                int ix2 = (int)floorf(p2x), iy2 = (int)floorf(p2y);
                float fx2 = p2x - (float)ix2, fy2 = p2y - (float)iy2;
                float ux2 = fx2 * fx2 * (3.0f - 2.0f * fx2);
                float uy2 = fy2 * fy2 * (3.0f - 2.0f * fy2);
                iq_hash2_vec(ix2, iy2, &g00x, &g00y);
                iq_hash2_vec(ix2 + 1, iy2, &g10x, &g10y);
                iq_hash2_vec(ix2, iy2 + 1, &g01x, &g01y);
                iq_hash2_vec(ix2 + 1, iy2 + 1, &g11x, &g11y);
                d00 = g00x * fx2 + g00y * fy2;
                d10 = g10x * (fx2 - 1.0f) + g10y * fy2;
                d01 = g01x * fx2 + g01y * (fy2 - 1.0f);
                d11 = g11x * (fx2 - 1.0f) + g11y * (fy2 - 1.0f);
                x0_v = d00 + (d10 - d00) * ux2;
                x1_v = d01 + (d11 - d01) * ux2;
                float n2 = x0_v + (x1_v - x0_v) * uy2;

                float n = (0.70f * n1 + 0.30f * n2) * 0.5f + 0.5f;
                float alpha = smoothstepf(edge0, threshold + softness, n);
                float mix_val = 1.0f - alpha;
                uint32_t base;
                if (mix_val <= 0.0f) {
                    base = a_row[x];
                } else if (mix_val >= 1.0f) {
                    base = b_row[x];
                } else {
                    uint16_t wb = (uint16_t)(mix_val * 256.0f);
                    base = blend_pixel_scalar(a_row[x], b_row[x], wb);
                }
                float edge = 1.0f - fabsf(n - threshold) * inv_span;
                if (edge > 0.0f) {
                    float glow = edge * edge * glow_env;
                    int r = (int)((base >> 16) & 0xFFu) + (int)(glow * 0.3f * 255.0f);
                    int g = (int)((base >> 8) & 0xFFu) + (int)(glow * 0.6f * 255.0f);
                    int b = (int)(base & 0xFFu) + (int)(glow * 1.0f * 255.0f);
                    if (r > 255)
                        r = 255;
                    if (g > 255)
                        g = 255;
                    if (b > 255)
                        b = 255;
                    base = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                }
                dst_row[x] = base;
            }
        }
        simd_stream_fence();
        return true;
    }

    case WAYWAL_TRANSITION_LIGHT_LEAK: {
        float mix_factor = smoothstepf(0.25f, 0.75f, progress);
        uint16_t wb = (uint16_t)(mix_factor * 256.0f + 0.5f);
        if (wb > 256U)
            wb = 256U;
        float sweep = progress * 1.6f - 0.3f;
        float env = sinf(progress * 3.1415926535f);
        env = env * env;
        float env255 = env * 255.0f;

        if (env255 < 0.25f) {
#pragma omp parallel for schedule(static, 8)
            for (uint32_t y = 0; y < height; ++y) {
                blend_fn((uint8_t *)(dst + (size_t)y * stride_pixels),
                         (const uint8_t *)(old_pixels + (size_t)y * width),
                         (const uint8_t *)(new_pixels + (size_t)y * width), wb,
                         (size_t)width * sizeof(uint32_t));
            }
            simd_stream_fence();
            return true;
        }

        float inv_w = 1.0f / (float)width;
        float inv_h = 1.0f / (float)height;
        float orb1_cx = sweep + 0.1f;
        float orb2_cx = sweep - 0.2f;

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float vy = ((float)y + 0.5f) * inv_h;
            float row_offset = vy * 0.25f - sweep;
            float dy1 = vy - 0.4f;
            float dy2 = vy - 0.7f;
            float orb1_dy2 = dy1 * dy1;
            float orb2_dy2 = dy2 * dy2;
            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
            if (has_avx2) {
                light_leak_row_avx2(dst_row, a_row, b_row, width, inv_w, row_offset, orb1_cx,
                                    orb1_dy2, orb2_cx, orb2_dy2, env255, wb);
                continue;
            }
#endif
            for (uint32_t x = 0; x < width; ++x) {
                float uvx = ((float)x + 0.5f) * inv_w;
                float db = uvx + row_offset;
                float tb = 1.0f - (db * db) * 10.666667f;
                float fb = (tb > 0.0f) ? (tb * tb * tb) : 0.0f;

                float dx1 = uvx - orb1_cx;
                float t1 = 1.0f - (dx1 * dx1 + orb1_dy2) * 4.0f;
                float o1 = (t1 > 0.0f) ? (t1 * t1 * t1) : 0.0f;

                float dx2 = uvx - orb2_cx;
                float t2 = 1.0f - (dx2 * dx2 + orb2_dy2) * 7.0f;
                float o2 = (t2 > 0.0f) ? (t2 * t2 * t2) : 0.0f;

                int fr = (int)((1.1f * 1.8f * fb + 1.0f * 1.2f * o1 + 0.9f * o2) * env255);
                int fg = (int)((0.65f * 1.8f * fb + 0.85f * 1.2f * o1 + 0.3f * o2) * env255);
                int fbl = (int)((0.25f * 1.8f * fb + 0.4f * 1.2f * o1 + 0.5f * o2) * env255);

                uint32_t blended = blend_pixel_scalar(a_row[x], b_row[x], wb);
                int r = (int)((blended >> 16) & 0xFFu) + fr;
                int g = (int)((blended >> 8) & 0xFFu) + fg;
                int b = (int)(blended & 0xFFu) + fbl;
                if (r > 255)
                    r = 255;
                if (g > 255)
                    g = 255;
                if (b > 255)
                    b = 255;
                dst_row[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
        simd_stream_fence();
        return true;
    }

    case WAYWAL_TRANSITION_PAGE_CURL: {
        const float R = 0.12f;
        const float inv_R = 1.0f / R;
        const float shadow_w = R * 1.8f;
        float line = 1.02f - progress * 1.25f;
        float inv_w = 1.0f / (float)width;
        float inv_h = 1.0f / (float)height;
        /* Preserve angle_rad == 0.0f for horizontal peel; only -999.0f/non-finite defaults to 45
         * deg. */
        float angle = (!isfinite(params->angle_rad) || fabsf(params->angle_rad - (-999.0f)) < 1e-4f)
                          ? 0.78539816f
                          : params->angle_rad;
        float dir_x = cosf(angle);
        float dir_y = -sinf(angle);
        float span = fmaxf(fabsf(dir_x) + fabsf(dir_y), 1e-5f);
        float inv_span = 1.0f / span;
        float d_s = (dir_x * inv_span) * inv_w;
        float nx0 = 0.5f * inv_w - 0.5f;
        bool flat_x = (fabsf(d_s) * (float)width < 1e-5f);
        float inv_ds = flat_x ? 0.0f : (1.0f / d_s);

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float vy = ((float)y + 0.5f) * inv_h;
            float s0 = (nx0 * dir_x + (vy - 0.5f) * dir_y) * inv_span + 0.5f;
            float s_end = s0 + (float)(width - 1U) * d_s;
            float s_min = s0 < s_end ? s0 : s_end;
            float s_max = s0 > s_end ? s0 : s_end;

            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

            if (s_max < line) {
                memcpy(dst_row, a_row, (size_t)width * sizeof(uint32_t));
                continue;
            }
            if (s_min >= line + shadow_w) {
                memcpy(dst_row, b_row, (size_t)width * sizeof(uint32_t));
                continue;
            }

            int x_band_L = 0;
            int x_band_R = (int)width;
            if (!flat_x) {
                float x1 = (line - s0) * inv_ds;
                float x2 = (line + shadow_w - s0) * inv_ds;
                float x_min = x1 < x2 ? x1 : x2;
                float x_max = x1 > x2 ? x1 : x2;
                x_band_L = span_floor_clamp(x_min, (int)width);
                x_band_R = span_ceil_clamp(x_max, (int)width);
                if (x_band_L > x_band_R)
                    x_band_L = x_band_R;
            }

            const uint32_t *left_src = (d_s >= 0.0f) ? a_row : b_row;
            const uint32_t *right_src = (d_s >= 0.0f) ? b_row : a_row;
            if (x_band_L > 0) {
                memcpy(dst_row, left_src, (size_t)x_band_L * sizeof(uint32_t));
            }

            for (int x = x_band_L; x < x_band_R; ++x) {
                float vx = ((float)x + 0.5f) * inv_w;
                float s = s0 + (float)x * d_s;
                if (s < line) {
                    dst_row[x] = a_row[x];
                } else if (s < line + R) {
                    float dist = s - line;
                    float curl_off = 2.0f * span * dist;
                    float curl_u = vx - curl_off * dir_x;
                    float curl_v = vy - curl_off * dir_y;
                    if (curl_u >= 0.0f && curl_u <= 1.0f && curl_v >= 0.0f && curl_v <= 1.0f) {
                        int sx = (int)(curl_u * (float)width);
                        int sy = (int)(curl_v * (float)height);
                        if (sx >= (int)width)
                            sx = (int)width - 1;
                        if (sy >= (int)height)
                            sy = (int)height - 1;

                        float angle_norm = dist * inv_R;
                        float curl_light = 1.0f + 0.18f * sinf(angle_norm * 3.14159265f);
                        float light_mix =
                            1.0f + (curl_light - 1.0f) * smoothstepf(0.0f, R * 0.25f, dist);
                        uint32_t c = old_pixels[(size_t)sy * width + (size_t)sx];
                        int r = (int)((float)((c >> 16) & 0xFFu) * light_mix + 0.5f);
                        int g = (int)((float)((c >> 8) & 0xFFu) * light_mix + 0.5f);
                        int b = (int)((float)(c & 0xFFu) * light_mix + 0.5f);
                        if (r > 255)
                            r = 255;
                        if (g > 255)
                            g = 255;
                        if (b > 255)
                            b = 255;
                        dst_row[x] =
                            0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                    } else {
                        float shadow = 0.5f + 0.5f * smoothstepf(0.0f, shadow_w, dist);
                        uint16_t wb_sh = (uint16_t)(shadow * 256.0f + 0.5f);
                        dst_row[x] = blend_pixel_scalar(0xFF000000u, b_row[x], wb_sh);
                    }
                } else if (s < line + shadow_w) {
                    float dist = s - line;
                    float shadow = 0.5f + 0.5f * smoothstepf(0.0f, shadow_w, dist);
                    uint16_t wb_sh = (uint16_t)(shadow * 256.0f + 0.5f);
                    dst_row[x] = blend_pixel_scalar(0xFF000000u, b_row[x], wb_sh);
                } else {
                    dst_row[x] = b_row[x];
                }
            }

            if (x_band_R < (int)width) {
                memcpy(dst_row + x_band_R, right_src + x_band_R,
                       (size_t)((int)width - x_band_R) * sizeof(uint32_t));
            }
        }
        simd_stream_fence();
        return true;
    }

    default:
        break;
    }

    return false;
}
