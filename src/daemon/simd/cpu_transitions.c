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
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
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

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
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
    __m256 v_eps = _mm256_set1_ps(1e-8f);
    __m256 v_eight = _mm256_set1_ps(8.0f);
    __m256i v_lut_max = _mm256_set1_epi32(lut_max);
    __m256i v_max_x = _mm256_set1_epi32(max_x);
    __m256i v_max_y = _mm256_set1_epi32(max_y);
    __m256i v_width = _mm256_set1_epi32(width);
    __m256i v_zero = _mm256_setzero_si256();
    __m256i v_wa = _mm256_set1_epi16((short)wa);
    __m256i v_wb = _mm256_set1_epi16((short)wb);
    const __m256i v_mask_rb = _mm256_set1_epi32(0x00FF00FF);
    const __m256i v_mask_g = _mm256_set1_epi32(0x0000FF00);
    const __m256i v_alpha = _mm256_set1_epi32((int)0xFF000000U);

    float x0 = (float)x_start - cx_pixel;
    __m256 vx_rel = _mm256_set_ps(x0 + 7.0f, x0 + 6.0f, x0 + 5.0f, x0 + 4.0f, x0 + 3.0f, x0 + 2.0f,
                                  x0 + 1.0f, x0);

    for (int x = x_start; x + 8 <= x_end; x += 8) {
        __m256 vd2 = _mm256_fmadd_ps(vx_rel, vx_rel, v_y_rel2);
        __m256 v_inv_d = _mm256_rsqrt_ps(_mm256_max_ps(vd2, v_eps));
        __m256 vdist = _mm256_mul_ps(vd2, v_inv_d);
        __m256i v_idx = _mm256_cvttps_epi32(_mm256_mul_ps(vdist, v_dist_scale));
        v_idx = _mm256_max_epi32(v_zero, _mm256_min_epi32(v_idx, v_lut_max));

        __m256 vsin = _mm256_i32gather_ps(swirl_sin, v_idx, 4);
        __m256 vcos = _mm256_i32gather_ps(swirl_cos, v_idx, 4);

        __m256 vpx =
            _mm256_add_ps(v_cx, _mm256_fmsub_ps(vx_rel, vcos, _mm256_mul_ps(v_y_rel, vsin)));
        __m256 vpy =
            _mm256_add_ps(v_cy, _mm256_fmadd_ps(vx_rel, vsin, _mm256_mul_ps(v_y_rel, vcos)));

        __m256i vsx = _mm256_max_epi32(v_zero, _mm256_min_epi32(_mm256_cvttps_epi32(vpx), v_max_x));
        __m256i vsy = _mm256_max_epi32(v_zero, _mm256_min_epi32(_mm256_cvttps_epi32(vpy), v_max_y));

        __m256i v_off = _mm256_add_epi32(_mm256_mullo_epi32(vsy, v_width), vsx);

        __m256i v_ca = _mm256_i32gather_epi32((const int *)old_pixels, v_off, 4);
        __m256i v_cb = _mm256_i32gather_epi32((const int *)new_pixels, v_off, 4);

        __m256i a_rb = _mm256_and_si256(v_ca, v_mask_rb);
        __m256i b_rb = _mm256_and_si256(v_cb, v_mask_rb);
        __m256i a_ag = _mm256_srli_epi16(v_ca, 8);
        __m256i b_ag = _mm256_srli_epi16(v_cb, 8);

        __m256i rb = _mm256_srli_epi16(
            _mm256_add_epi16(_mm256_mullo_epi16(a_rb, v_wa), _mm256_mullo_epi16(b_rb, v_wb)), 8);
        __m256i g = _mm256_and_si256(
            _mm256_add_epi16(_mm256_mullo_epi16(a_ag, v_wa), _mm256_mullo_epi16(b_ag, v_wb)),
            v_mask_g);

        _mm256_storeu_si256((__m256i *)&dst_row[x],
                            _mm256_or_si256(_mm256_or_si256(rb, g), v_alpha));
        vx_rel = _mm256_add_ps(vx_rel, v_eight);
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

    float dx0 = (float)x_start * step_dx - cx;
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
                    const uint32_t *restrict new_pixels, const float *restrict wave_lut,
                    int x_start, int x_end, int y, float cx_scaled, float step_dx, float dy,
                    float dy2, float current_radius, float wave_width, float inv_span, int width,
                    int height)
{
    __m256 v_step_dx8 = _mm256_set1_ps(8.0f * step_dx);
    __m256 v_eight = _mm256_set1_ps(8.0f);
    __m256 v_dy = _mm256_set1_ps(dy);
    __m256 v_dy2 = _mm256_set1_ps(dy2);
    __m256 v_cur_r = _mm256_set1_ps(current_radius);
    __m256 v_wave_w = _mm256_set1_ps(wave_width);
    __m256 v_inv_span = _mm256_set1_ps(inv_span);
    __m256 v_inv_aspect = _mm256_set1_ps((float)height / (float)width);
    __m256 v_w = _mm256_set1_ps((float)width);
    __m256 v_h = _mm256_set1_ps((float)height);
    __m256 v_vy = _mm256_set1_ps((float)y / (float)height);
    __m256 v_eps = _mm256_set1_ps(1e-8f);
    __m256 v_half = _mm256_set1_ps(0.5f);
    __m256 v_three_half = _mm256_set1_ps(1.5f);
    __m256 v_zero = _mm256_setzero_ps();
    __m256 v_one = _mm256_set1_ps(1.0f);
    __m256 v_two = _mm256_set1_ps(2.0f);
    __m256 v_three = _mm256_set1_ps(3.0f);
    __m256 v_002 = _mm256_set1_ps(0.02f);
    __m256 v_25 = _mm256_set1_ps(25.0f);
    __m256 v_256 = _mm256_set1_ps(256.0f);
    __m256i v_zero_i = _mm256_setzero_si256();
    __m256i v_511 = _mm256_set1_epi32(511);
    __m256i v_max_x = _mm256_set1_epi32(width - 1);
    __m256i v_max_y = _mm256_set1_epi32(height - 1);
    __m256i v_width_i = _mm256_set1_epi32(width);

    float dx0 = (float)x_start * step_dx - cx_scaled;
    __m256 vdx = _mm256_set_ps(dx0 + 7.0f * step_dx, dx0 + 6.0f * step_dx, dx0 + 5.0f * step_dx,
                               dx0 + 4.0f * step_dx, dx0 + 3.0f * step_dx, dx0 + 2.0f * step_dx,
                               dx0 + 1.0f * step_dx, dx0);
    float x0 = (float)x_start;
    __m256 vx = _mm256_set_ps(x0 + 7.0f, x0 + 6.0f, x0 + 5.0f, x0 + 4.0f, x0 + 3.0f, x0 + 2.0f,
                              x0 + 1.0f, x0);
    __m256 v_inv_w = _mm256_set1_ps(1.0f / (float)width);

    int x = x_start;
    for (; x + 8 <= x_end; x += 8) {
        __m256 vdist2 = _mm256_fmadd_ps(vdx, vdx, v_dy2);
        __m256 v_valid = _mm256_cmp_ps(vdist2, v_eps, _CMP_GT_OQ);
        __m256 vd2_safe = _mm256_max_ps(vdist2, v_eps);

        /* One Newton-Raphson step on rsqrt(d^2) yields both 1/dist and dist = d^2 * (1/dist). */
        __m256 y0 = _mm256_rsqrt_ps(vd2_safe);
        __m256 y1 = _mm256_mul_ps(y0, _mm256_fnmadd_ps(_mm256_mul_ps(v_half, vd2_safe),
                                                       _mm256_mul_ps(y0, y0), v_three_half));
        __m256 v_inv_dist = _mm256_and_ps(y1, v_valid);
        __m256 vdist = _mm256_and_ps(_mm256_mul_ps(vdist2, y1), v_valid);

        __m256 vdiff = _mm256_sub_ps(vdist, v_cur_r);
        __m256 v_idx_f = _mm256_mul_ps(_mm256_add_ps(vdiff, v_wave_w), v_inv_span);
        __m256i vidx =
            _mm256_max_epi32(v_zero_i, _mm256_min_epi32(_mm256_cvttps_epi32(v_idx_f), v_511));
        __m256 vwave = _mm256_i32gather_ps(wave_lut, vidx, 4);

        __m256 v_in_wave =
            _mm256_and_ps(_mm256_cmp_ps(vdiff, _mm256_sub_ps(v_zero, v_wave_w), _CMP_GT_OQ),
                          _mm256_cmp_ps(vdiff, v_wave_w, _CMP_LT_OQ));
        vwave = _mm256_and_ps(vwave, v_in_wave);

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

        _mm256_storeu_si256((__m256i *)&dst_row[x], blend8_argb_var_avx2(v_cb, v_ca, v_wa));

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
        __m256i vidx = _mm256_and_si256(_mm256_cvttps_epi32(vphase), v_mask_1023);
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

__attribute__((target("avx2,fma"))) static void
luma_row_avx2(uint32_t *restrict dst_row, const uint32_t *restrict a_row,
              const uint32_t *restrict b_row, const float *restrict v, const float *restrict dv,
              const int *restrict lut_ix, const float *restrict lut_cx, uint32_t width, float edge0,
              float inv_span)
{
    const __m256 v_edge0 = _mm256_set1_ps(edge0);
    const __m256 v_inv_span = _mm256_set1_ps(inv_span);
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

        __m256 v_t_raw = _mm256_mul_ps(_mm256_sub_ps(v_n, v_edge0), v_inv_span);
        if (_mm256_movemask_ps(_mm256_cmp_ps(v_t_raw, v_one_ps, _CMP_LT_OQ)) == 0) {
            _mm256_storeu_si256((__m256i *)&dst_row[x],
                                _mm256_loadu_si256((const __m256i *)&a_row[x]));
            continue;
        }
        if (_mm256_movemask_ps(_mm256_cmp_ps(v_t_raw, v_zero_ps, _CMP_GT_OQ)) == 0) {
            _mm256_storeu_si256((__m256i *)&dst_row[x],
                                _mm256_loadu_si256((const __m256i *)&b_row[x]));
            continue;
        }

        __m256 v_t = _mm256_min_ps(_mm256_max_ps(v_t_raw, v_zero_ps), v_one_ps);
        __m256 v_alpha =
            _mm256_mul_ps(_mm256_mul_ps(v_t, v_t), _mm256_fnmadd_ps(v_two_ps, v_t, v_three_ps));
        __m256 v_mix = _mm256_sub_ps(v_one_ps, v_alpha);
        __m256i v_wb_32 = _mm256_cvttps_epi32(_mm256_mul_ps(v_mix, v_256_ps));

        __m256i v_ca = _mm256_loadu_si256((const __m256i *)&a_row[x]);
        __m256i v_cb = _mm256_loadu_si256((const __m256i *)&b_row[x]);
        _mm256_storeu_si256((__m256i *)&dst_row[x], blend8_argb_var_avx2(v_ca, v_cb, v_wb_32));
    }

    for (; x < width; ++x) {
        int ix = lut_ix[x];
        float n = v[ix] + dv[ix] * lut_cx[x];
        float t = clampf((n - edge0) * inv_span, 0.0f, 1.0f);
        float alpha = t * t * (3.0f - 2.0f * t);
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
    const __m256 v_one_ps = _mm256_set1_ps(1.0f);
    const __m256 v_two_ps = _mm256_set1_ps(2.0f);
    const __m256 v_three_ps = _mm256_set1_ps(3.0f);
    const __m256 v_256_ps = _mm256_set1_ps(256.0f);
    const __m256 v_240_ps = _mm256_set1_ps(240.0f);
    const __m256 v_120_ps = _mm256_set1_ps(120.0f);
    const __m256 v_20_ps = _mm256_set1_ps(20.0f);
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

        if (_mm256_movemask_ps(_mm256_cmp_ps(v_d, v_fw, _CMP_LE_OQ)) == 0) {
            _mm256_storeu_si256((__m256i *)&dst_row[x],
                                _mm256_loadu_si256((const __m256i *)&a_row[x]));
            continue;
        }
        if (_mm256_movemask_ps(_mm256_cmp_ps(v_d, v_neg_fw, _CMP_GE_OQ)) == 0) {
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
        __m256 v_heat = _mm256_mul_ps(v_dist_norm, v_dist_norm);

        __m256i v_fr = _mm256_slli_epi32(_mm256_cvttps_epi32(_mm256_mul_ps(v_heat, v_240_ps)), 16);
        __m256i v_fg = _mm256_slli_epi32(_mm256_cvttps_epi32(_mm256_mul_ps(v_heat, v_120_ps)), 8);
        __m256i v_fb = _mm256_cvttps_epi32(_mm256_mul_ps(v_heat, v_20_ps));
        __m256i v_flame =
            _mm256_or_si256(_mm256_or_si256(v_fr, v_fg), _mm256_or_si256(v_fb, v_alpha_mask));

        _mm256_storeu_si256((__m256i *)&dst_row[x], _mm256_adds_epu8(v_base, v_flame));
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
            dst_row[x] = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

__attribute__((target("avx2,fma"))) static void
crosszoom_row_avx2(uint32_t *restrict dst_row, const uint32_t *restrict row_a,
                   const uint32_t *restrict row_b, const int *restrict lut_ax,
                   const int *restrict lut_bx, uint32_t width, uint16_t wb, int flash)
{
    const __m256i v_zero = _mm256_setzero_si256();
    const __m256i v_wb = _mm256_set1_epi16((short)wb);
    const __m256i v_wa = _mm256_set1_epi16((short)(256 - wb));
    const uint32_t flash_u = (flash > 0) ? (uint32_t)flash : 0U;
    const __m256i v_flash = _mm256_set1_epi32((int)(0xFF000000U | (flash_u * 0x00010101U)));

    uint32_t x = 0;
    for (; x + 8 <= width; x += 8) {
        __m256i v_ax = _mm256_loadu_si256((const __m256i *)&lut_ax[x]);
        __m256i v_bx = _mm256_loadu_si256((const __m256i *)&lut_bx[x]);
        __m256i v_ca = _mm256_i32gather_epi32((const int *)row_a, v_ax, 4);
        __m256i v_cb = _mm256_i32gather_epi32((const int *)row_b, v_bx, 4);

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
        uint32_t ca = row_a[lut_ax[x]];
        uint32_t cb = row_b[lut_bx[x]];
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
light_leak_span_avx2(uint32_t *restrict dst_row, const uint32_t *restrict a_row,
                     const uint32_t *restrict b_row, const uint32_t *restrict flare_lut,
                     int x_start, int x_end, float inv_w, float row_offset, float lut_scale,
                     int lut_max, uint16_t wb)
{
    const __m256 v_inv_w = _mm256_set1_ps(inv_w);
    const __m256 v_row_offset = _mm256_set1_ps(row_offset);
    const __m256 v_lut_scale = _mm256_set1_ps(lut_scale);
    const __m256 v_sign_mask = _mm256_set1_ps(-0.0f);
    const __m256i v_lut_max = _mm256_set1_epi32(lut_max);
    const __m256i v_zero = _mm256_setzero_si256();
    const __m256i v_wb = _mm256_set1_epi16((short)wb);
    const __m256i v_wa = _mm256_set1_epi16((short)(256 - wb));

    int x = x_start;
    for (; x + 8 <= x_end; x += 8) {
        __m256 vx = _mm256_set_ps((float)(x + 7), (float)(x + 6), (float)(x + 5), (float)(x + 4),
                                  (float)(x + 3), (float)(x + 2), (float)(x + 1), (float)x);
        __m256 v_dist = _mm256_andnot_ps(v_sign_mask, _mm256_fmadd_ps(vx, v_inv_w, v_row_offset));
        __m256i v_idx =
            _mm256_min_epi32(_mm256_cvttps_epi32(_mm256_mul_ps(v_dist, v_lut_scale)), v_lut_max);
        __m256i v_flash = _mm256_i32gather_epi32((const int *)flare_lut, v_idx, 4);

        __m256i v_ca = _mm256_loadu_si256((const __m256i *)&a_row[x]);
        __m256i v_cb = _mm256_loadu_si256((const __m256i *)&b_row[x]);

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
}
#endif

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
        eff_type = (waywal_transition_type_t)(1 + (rand() % 19));
    }

    switch (eff_type) {
    case WAYWAL_TRANSITION_FADE:
    case WAYWAL_TRANSITION_SIMPLE:
    case WAYWAL_TRANSITION_CUSTOM: {
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
        simd_stream_fence();
        return true;
    }

    case WAYWAL_TRANSITION_WIPE: {
        float cos_a = cosf(params->angle_rad);
        float sin_a = sinf(params->angle_rad);
        float edge = 0.04f;
        float p = progress * (1.0f + 2.0f * edge) - edge;
        float d_proj = cos_a / (1.41421356f * (float)width);
        float p_plus_edge = p + edge;
        float p_minus_edge = p - edge;
        float inv_2edge = 1.0f / (2.0f * edge);
        bool flat_x = (fabsf(d_proj) * (float)width < 1e-5f);
        float inv_d = flat_x ? 0.0f : (1.0f / d_proj);
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
        bool has_avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float ny = (float)y / (float)height - 0.5f;
            float proj_0 = (ny * sin_a - 0.5f * cos_a) / 1.41421356f + 0.5f;
            float proj_end = proj_0 + (float)width * d_proj;
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
        return true;
    }

    case WAYWAL_TRANSITION_GROW:
    case WAYWAL_TRANSITION_OUTER: {
        bool is_grow = (eff_type == WAYWAL_TRANSITION_GROW);
        float cx = params->center_x;
        float cy = params->center_y;
        if (cx <= 0.0f && cy <= 0.0f) {
            cx = 0.5f;
            cy = 0.5f;
        }
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
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
        bool has_avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float dy = (float)y / (float)height - cy;
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
            int x_out_L = span_floor_clamp(xc_px - dx_out, (int)width);
            int x_out_R = span_ceil_clamp(xc_px + dx_out, (int)width);
            if (x_out_L > x_out_R)
                x_out_L = x_out_R;

            int x_in_L = x_out_L;
            int x_in_R = x_out_L;
            if (r_inner > 0.0f && dy2 < r_inner2) {
                float dx_in = scale_x * sqrtf(r_inner2 - dy2);
                x_in_L = span_ceil_clamp(xc_px - dx_in, (int)width);
                x_in_R = span_floor_clamp(xc_px + dx_in, (int)width);
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
                float dx = (float)x * step_dx - cx_scaled;
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
                float dx = (float)x * step_dx - cx_scaled;
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
        float p_plus_margin = p + margin;
        float p_minus_margin = p - margin;
        float p_plus_edge = p + edge;
        float inv_2edge = 1.0f / (2.0f * edge);

        const float *unit_sin = get_unit_sine_lut();
        float sine_lut[1024];
        for (int i = 0; i < 1024; ++i) {
            sine_lut[i] = unit_sin[i] * amp;
        }

        float d_proj = cos_a / (1.41421356f * (float)width);
        float d_cross = (-sin_a * freq) / (float)width;
        float d_phase = d_cross * (1024.0f / 6.28318530718f);
        bool flat_x = (fabsf(d_proj) * (float)width < 1e-5f);
        float inv_d = flat_x ? 0.0f : (1.0f / d_proj);
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
        bool has_avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float ny = (float)y / (float)height - 0.5f;
            float proj_0 = (ny * sin_a - 0.5f * cos_a) / 1.41421356f + 0.5f;
            float proj_end = proj_0 + (float)width * d_proj;
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

            float phase_0 = (ny * cos_a + 0.5f * sin_a) * freq * (1024.0f / 6.28318530718f);
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
                int idx = ((int)ph) & 1023;
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
                grid[j][i] = lattice_hash21(i, j);
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
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
        bool has_avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif

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
        return true;
    }

    case WAYWAL_TRANSITION_CROSSZOOM: {
        bool has_center = (params->center_x > 0.0f || params->center_y > 0.0f);
        float cx = has_center ? params->center_x : 0.5f;
        float cy = has_center ? params->center_y : 0.5f;
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
                ax = (int)width - 1;
            int bx = (int)((cx + nx * zoom_b) * (float)width);
            if (bx < 0)
                bx = 0;
            else if (bx >= (int)width)
                bx = (int)width - 1;
            lut_ax[x] = ax;
            lut_bx[x] = bx;
        }

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
        bool has_avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float ny = ((float)y / (float)height - cy);
            int src_ay = (int)((cy + ny * zoom_a) * (float)height);
            if (src_ay < 0)
                src_ay = 0;
            else if (src_ay >= (int)height)
                src_ay = (int)height - 1;

            int src_by = (int)((cy + ny * zoom_b) * (float)height);
            if (src_by < 0)
                src_by = 0;
            else if (src_by >= (int)height)
                src_by = (int)height - 1;

            const uint32_t *row_a = old_pixels + (size_t)src_ay * width;
            const uint32_t *row_b = new_pixels + (size_t)src_by * width;
            uint32_t *dst_row = dst + (size_t)y * stride_pixels;

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
            if (has_avx2) {
                crosszoom_row_avx2(dst_row, row_a, row_b, lut_ax, lut_bx, width, wb, flash);
                continue;
            }
#endif
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
        float p_plus_edge = p + edge;
        float p_minus_edge = p - edge;
        float inv_2edge = 1.0f / (2.0f * edge);
        float shift_ax = cos_a * progress * (float)width;
        float shift_bx = -cos_a * (1.0f - progress) * (float)width;
        int ishift_ax = (int)floorf(shift_ax);
        int ishift_bx = (int)floorf(shift_bx);
        float d_proj = cos_a / (1.41421356f * (float)width);
        bool flat_x = (fabsf(d_proj) * (float)width < 1e-5f);
        float inv_d = flat_x ? 0.0f : (1.0f / d_proj);

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float ny = (float)y / (float)height - 0.5f;
            int ay = (int)((float)y + sin_a * progress * (float)height);
            if (ay < 0)
                ay = 0;
            else if (ay >= (int)height)
                ay = (int)height - 1;

            int by = (int)((float)y - sin_a * (1.0f - progress) * (float)height);
            if (by < 0)
                by = 0;
            else if (by >= (int)height)
                by = (int)height - 1;

            const uint32_t *row_a = old_pixels + (size_t)ay * width;
            const uint32_t *row_b = new_pixels + (size_t)by * width;
            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            float proj_0 = (ny * sin_a - 0.5f * cos_a) / 1.41421356f + 0.5f;
            float proj_end = proj_0 + (float)width * d_proj;
            float proj_min = proj_0 < proj_end ? proj_0 : proj_end;
            float proj_max = proj_0 > proj_end ? proj_0 : proj_end;

            if (proj_min >= p_plus_edge) {
                copy_shifted_span(dst_row, row_a, 0, (int)width, ishift_ax, (int)width);
                continue;
            }
            if (proj_max <= p_minus_edge) {
                copy_shifted_span(dst_row, row_b, 0, (int)width, ishift_bx, (int)width);
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

            if (x_seam_L > 0) {
                if (d_proj >= 0.0f)
                    copy_shifted_span(dst_row, row_b, 0, x_seam_L, ishift_bx, (int)width);
                else
                    copy_shifted_span(dst_row, row_a, 0, x_seam_L, ishift_ax, (int)width);
            }

            for (int x = x_seam_L; x < x_seam_R; ++x) {
                float proj = proj_0 + (float)x * d_proj;
                float t = clampf((p_plus_edge - proj) * inv_2edge, 0.0f, 1.0f);
                float split = t * t * (3.0f - 2.0f * t);

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

                uint32_t ca = row_a[ax];
                uint32_t cb = row_b[bx];
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
                if (d_proj >= 0.0f)
                    copy_shifted_span(dst_row, row_a, x_seam_R, (int)width, ishift_ax, (int)width);
                else
                    copy_shifted_span(dst_row, row_b, x_seam_R, (int)width, ishift_bx, (int)width);
            }
        }
        return true;
    }

    case WAYWAL_TRANSITION_GLITCH: {
        float bands = params->wave_freq > 0.0f ? params->wave_freq : 35.0f;
        float intensity = params->wave_amp > 0.0f ? params->wave_amp : 0.06f;
        float peak = sinf(progress * 3.14159265f);
        float step_t = floorf(progress * 24.0f);
        int split = (int)(peak * intensity * 0.5f * (float)width);

        uint32_t num_bx = (width + 23U) / 24U;
        uint32_t num_by = (height + 13U) / 14U;
        uint16_t wb_stack[4096];
        size_t total_blocks = (size_t)num_bx * num_by;
        uint16_t *wb_lut =
            (total_blocks <= 4096) ? wb_stack : (uint16_t *)malloc(total_blocks * sizeof(uint16_t));
        if (!wb_lut)
            return false;

        for (uint32_t by = 0; by < num_by; ++by) {
            float block_y = (float)by + step_t;
            uint16_t *wb_row = wb_lut + (size_t)by * num_bx;
            for (uint32_t bx = 0; bx < num_bx; ++bx) {
                float block_h = hash21((float)bx, block_y);
                float gprog = progress + (block_h - 0.5f) * peak * 0.35f;
                float m = smoothstepf(0.46f, 0.54f, gprog);
                wb_row[bx] = (uint16_t)(m * 256.0f);
            }
        }

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
        bool has_avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float slice_idx = floorf(((float)y / (float)height) * bands);
            float h = hash21(slice_idx, step_t);
            int jitter = 0;
            if (h > 0.60f) {
                jitter = (int)((h - 0.80f) * 2.5f * intensity * peak * (float)width);
            }
            int dr = jitter + split;
            int dg = jitter;
            int db = jitter - split;

            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;
            const uint16_t *wb_row = wb_lut + (size_t)(y / 14U) * num_bx;

            for (uint32_t bx_idx = 0; bx_idx < num_bx; ++bx_idx) {
                uint32_t bx_start = bx_idx * 24U;
                uint32_t bx_end = bx_start + 24U;
                if (bx_end > width)
                    bx_end = width;
                size_t blk_len = bx_end - bx_start;
                uint16_t wb = wb_row[bx_idx];

                if (jitter == 0 && split == 0) {
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

        if (wb_lut != wb_stack)
            free(wb_lut);
        simd_stream_fence();
        return true;
    }

    case WAYWAL_TRANSITION_BURN: {
        float scale = params->wave_freq > 0.0f ? params->wave_freq : 14.0f;
        float flame_width = params->wave_amp > 0.0f ? params->wave_amp : 0.08f;
        float p = progress * (1.0f + 2.0f * flame_width) - flame_width;

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

        int lut_ix1_stack[4096];
        int lut_ix2_stack[4096];
        float lut_cx1_stack[4096];
        float lut_cx2_stack[4096];
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
            float x1 = (float)x * step1_x;
            int ix1 = (int)x1;
            if (ix1 < 0)
                ix1 = 0;
            if (ix1 > gw1 - 2)
                ix1 = gw1 - 2;
            float fx1 = x1 - (float)ix1;
            lut_ix1[x] = ix1;
            lut_cx1[x] = fx1 * fx1 * (3.0f - 2.0f * fx1);

            float x2 = (float)x * step2_x + 1.7f;
            int ix2 = (int)x2;
            if (ix2 < 0)
                ix2 = 0;
            if (ix2 > gw2 - 2)
                ix2 = gw2 - 2;
            float fx2 = x2 - (float)ix2;
            lut_ix2[x] = ix2;
            lut_cx2[x] = fx2 * fx2 * (3.0f - 2.0f * fx2);
        }

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
        bool has_avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float ny = (float)y * inv_h;

            float y1 = ny * scale;
            int iy1 = (int)y1;
            if (iy1 < 0)
                iy1 = 0;
            if (iy1 > gh1 - 2)
                iy1 = gh1 - 2;
            float fy1 = y1 - (float)iy1;
            float cy1 = fy1 * fy1 * (3.0f - 2.0f * fy1);
            float v1[48];
            float dv1[48];
            for (int i = 0; i < gw1; ++i) {
                v1[i] = grid1[iy1][i] * (1.0f - cy1) + grid1[iy1 + 1][i] * cy1;
            }
            for (int i = 0; i < gw1 - 1; ++i) {
                dv1[i] = v1[i + 1] - v1[i];
            }
            dv1[gw1 - 1] = 0.0f;

            float y2 = (ny * scale) * 2.1f + 3.2f;
            int iy2 = (int)y2;
            if (iy2 < 0)
                iy2 = 0;
            if (iy2 > gh2 - 2)
                iy2 = gh2 - 2;
            float fy2 = y2 - (float)iy2;
            float cy2 = fy2 * fy2 * (3.0f - 2.0f * fy2);
            float v2[96];
            float dv2[96];
            for (int i = 0; i < gw2; ++i) {
                v2[i] = grid2[iy2][i] * (1.0f - cy2) + grid2[iy2 + 1][i] * cy2;
            }
            for (int i = 0; i < gw2 - 1; ++i) {
                dv2[i] = v2[i + 1] - v2[i];
            }
            dv2[gw2 - 1] = 0.0f;

            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
            if (has_avx2) {
                burn_row_avx2(dst_row, a_row, b_row, v1, dv1, v2, dv2, lut_ix1, lut_cx1, lut_ix2,
                              lut_cx2, width, p, flame_width);
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

        if (lut_ix1 != lut_ix1_stack)
            free(lut_ix1);
        if (lut_ix2 != lut_ix2_stack)
            free(lut_ix2);
        if (lut_cx1 != lut_cx1_stack)
            free(lut_cx1);
        if (lut_cx2 != lut_cx2_stack)
            free(lut_cx2);
        return true;
    }

    case WAYWAL_TRANSITION_RIPPLE: {
        bool has_center = (params->center_x > 0.0f || params->center_y > 0.0f);
        float cx = has_center ? params->center_x : 0.5f;
        float cy = has_center ? params->center_y : 0.5f;
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

        const float *unit_sin = get_unit_sine_lut();
        float wave_lut[512];
        float inv_span = 511.0f / (2.0f * wave_width);
        const float rad_to_lut = 1024.0f / 6.28318530718f;
        for (int k = 0; k < 512; ++k) {
            float diff = -wave_width + ((float)k / 511.0f) * (2.0f * wave_width);
            float window = smoothstepf(wave_width, 0.0f, fabsf(diff));
            int sidx = ((int)floorf(diff * freq * rad_to_lut)) & 1023;
            wave_lut[k] = unit_sin[sidx] * amp * decay * window;
        }

        float xc_px = cx * (float)width;
        float scale_x = (float)height;
        float step_dx = aspect / (float)width;
        float cx_scaled = cx * aspect;
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
        bool has_avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif

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

            float dx_out = scale_x * sqrtf(r_outer2 - dy2);
            int x_out_L = span_floor_clamp(xc_px - dx_out, (int)width);
            int x_out_R = span_ceil_clamp(xc_px + dx_out, (int)width);
            if (x_out_L > x_out_R)
                x_out_L = x_out_R;

            int x_in_L = x_out_L;
            int x_in_R = x_out_L;
            if (r_inner > 0.0f && dy2 < r_inner2) {
                float dx_in = scale_x * sqrtf(r_inner2 - dy2);
                x_in_L = span_ceil_clamp(xc_px - dx_in, (int)width);
                x_in_R = span_floor_clamp(xc_px + dx_in, (int)width);
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
                    x = ripple_annulus_avx2(dst_row, old_pixels, new_pixels, wave_lut, x, x_end,
                                            (int)y, cx_scaled, step_dx, dy, dy2, current_radius,
                                            wave_width, inv_span, (int)width, (int)height);
                }
#endif
                for (; x < x_end; ++x) {
                    float dx = (float)x * step_dx - cx_scaled;
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

                        float inv_dist = (dist > 0.0001f) ? (1.0f / dist) : 0.0f;
                        float ndx = dx * inv_dist / aspect;
                        float ndy = dy * inv_dist;

                        int sx = (int)(((float)x / (float)width + ndx * wave) * (float)width);
                        int sy = (int)(((float)y / (float)height + ndy * wave) * (float)height);
                        if (sx < 0)
                            sx = 0;
                        else if (sx >= (int)width)
                            sx = (int)width - 1;
                        if (sy < 0)
                            sy = 0;
                        else if (sy >= (int)height)
                            sy = (int)height - 1;

                        uint32_t ca = old_pixels[(size_t)sy * width + sx];
                        uint32_t cb = new_pixels[(size_t)sy * width + sx];

                        float split =
                            smoothstepf(current_radius - 0.02f, current_radius + 0.02f, dist);
                        uint16_t wa = (uint16_t)(split * 256.0f);
                        dst_row[x] = blend_pixel_scalar(cb, ca, wa);
                    }
                }
            }

            if (x_out_R < (int)width) {
                memcpy(dst_row + x_out_R, a_row + x_out_R,
                       (size_t)((int)width - x_out_R) * sizeof(uint32_t));
            }
        }
        return true;
    }

    case WAYWAL_TRANSITION_PIXELATE: {
        float d = sinf(progress * 3.1415926535f) * 64.0f + 1.0f;
        uint32_t block_sz = (d >= 1.0f) ? (uint32_t)d : 1u;
        uint16_t wb = (uint16_t)(progress * 256.0f);

        if (block_sz == 1u) {
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

#pragma omp parallel for schedule(dynamic, 4)
        for (uint32_t by = 0; by < height; by += block_sz) {
            uint32_t bh = (by + block_sz <= height) ? block_sz : (height - by);
            uint32_t sy = by + bh / 2;
            if (sy >= height)
                sy = height - 1;

            const uint32_t *a_row = old_pixels + (size_t)sy * width;
            const uint32_t *b_row = new_pixels + (size_t)sy * width;
            uint32_t *first_row = dst + (size_t)by * stride_pixels;

            for (uint32_t bx = 0; bx < width; bx += block_sz) {
                uint32_t bw = (bx + block_sz <= width) ? block_sz : (width - bx);
                uint32_t sx = bx + bw / 2;
                if (sx >= width)
                    sx = width - 1;

                uint32_t blended = blend_pixel_scalar(a_row[sx], b_row[sx], wb);
                for (uint32_t x = bx; x < bx + bw; ++x) {
                    first_row[x] = blended;
                }
            }

            for (uint32_t y = by + 1; y < by + bh; ++y) {
                memcpy(dst + (size_t)y * stride_pixels, first_row,
                       (size_t)width * sizeof(uint32_t));
            }
        }
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

            float shift = clampf((progress * 1.35f - delay) / 1.0f, 0.0f, 1.0f);
            pixel_shift_lut[c] = (int)(shift * (float)height);
        }

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *new_row = new_pixels + (size_t)y * width;

            for (uint32_t c = 0; c < num_cols; ++c) {
                uint32_t bx = c * col_width;
                uint32_t bw = (bx + col_width <= width) ? col_width : (width - bx);
                int pixel_shift = pixel_shift_lut[c];

                if ((int)y < pixel_shift) {
                    memcpy(dst_row + bx, new_row + bx, (size_t)bw * sizeof(uint32_t));
                } else {
                    int sy = (int)y - pixel_shift;
                    if (sy < 0)
                        sy = 0;
                    if (sy >= (int)height)
                        sy = (int)height - 1;
                    memcpy(dst_row + bx, old_pixels + (size_t)sy * width + bx,
                           (size_t)bw * sizeof(uint32_t));
                }
            }
        }

        if (pixel_shift_lut != shift_stack)
            free(pixel_shift_lut);
        return true;
    }

    case WAYWAL_TRANSITION_SWIRL: {
        bool has_center = (params->center_x > 0.0f || params->center_y > 0.0f);
        float cx = has_center ? params->center_x : 0.5f;
        float cy = has_center ? params->center_y : 0.5f;
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
            simd_stream_fence();
            return true;
        }

        float max_dist = max_r * (float)height;
        float max_dist2 = max_dist * max_dist;
        float cx_pixel = cx * (float)width;
        float cy_pixel = cy * (float)height;

#define SWIRL_LUT_SIZE 1024
        const float *unit_sin = get_unit_sine_lut();
        const float rad_to_lut = 1024.0f / 6.28318530718f;
        float swirl_sin[SWIRL_LUT_SIZE + 1];
        float swirl_cos[SWIRL_LUT_SIZE + 1];
        float dist_scale = (float)SWIRL_LUT_SIZE / max_dist;
        float inv_dist_scale = max_dist / (float)SWIRL_LUT_SIZE;
        for (int i = 0; i < SWIRL_LUT_SIZE; ++i) {
            float dist = (float)i * inv_dist_scale;
            float factor = (1.0f - dist / max_dist);
            float theta = factor * factor * angle_amt;
            int sidx = ((int)floorf(theta * rad_to_lut)) & 1023;
            swirl_sin[i] = unit_sin[sidx];
            swirl_cos[i] = unit_sin[(sidx + 256) & 1023];
        }
        swirl_sin[SWIRL_LUT_SIZE] = 0.0f;
        swirl_cos[SWIRL_LUT_SIZE] = 1.0f;

        int max_x = (int)width - 1;
        int max_y = (int)height - 1;
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
        bool has_avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif

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

            float x_span = sqrtf(max_dist2 - y_rel2);
            int x_start = span_floor_clamp(cx_pixel - x_span, (int)width);
            int x_end = span_ceil_clamp(cx_pixel + x_span, (int)width);
            if (x_start > x_end)
                x_start = x_end;

            if (x_start > 0) {
                blend_fn((uint8_t *)dst_row, (const uint8_t *)a_row, (const uint8_t *)b_row, wb,
                         (size_t)x_start * sizeof(uint32_t));
            }

            int x = x_start;
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
            if (has_avx2) {
                int avx_count = (x_end - x_start) & ~7;
                if (avx_count > 0) {
                    swirl_span_avx2(dst_row, old_pixels, new_pixels, swirl_sin, swirl_cos, x_start,
                                    x_start + avx_count, cx_pixel, cy_pixel, y_rel, y_rel2,
                                    dist_scale, SWIRL_LUT_SIZE, max_x, max_y, (int)width, wa, wb);
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

            if (x_end < (int)width) {
                blend_fn((uint8_t *)(dst_row + x_end), (const uint8_t *)(a_row + x_end),
                         (const uint8_t *)(b_row + x_end), wb,
                         (size_t)((int)width - x_end) * sizeof(uint32_t));
            }
        }
#undef SWIRL_LUT_SIZE
        simd_stream_fence();
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
                    sx = (int)width - 1;
                col_sx[x] = sx;
                col_ky[x] = zoom * t_f;
                if ((int)x < front_x0)
                    front_x0 = (int)x;
                front_x1 = (int)x + 1;
            } else if (t_r > 0.0f) {
                int sx = (int)(((hz + 1.0f) * 0.5f) * (float)width);
                if (sx < 0)
                    sx = 0;
                else if (sx >= (int)width)
                    sx = (int)width - 1;
                col_sx[x] = sx;
                col_ky[x] = zoom * t_r;
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
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
        bool has_avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float py = ((float)y / (float)height - 0.5f) * 2.0f;
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
                    fill_u32_span(dst_row + cursor, 0xFF08080CU, xf0 - cursor);
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
                    fill_u32_span(dst_row + cursor, 0xFF08080CU, xr0 - cursor);
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
                fill_u32_span(dst_row + cursor, 0xFF08080CU, (int)width - cursor);
            }
        }

        if (col_sx != col_sx_stack)
            free(col_sx);
        if (col_ky != col_ky_stack)
            free(col_ky);
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
            float gx = (float)x * inv_w_scale;
            int ix = (int)gx;
            if (ix < 0)
                ix = 0;
            if (ix > grid_w - 2)
                ix = grid_w - 2;
            float fx = gx - (float)ix;
            lut_ix[x] = ix;
            lut_cx[x] = fx * fx * (3.0f - 2.0f * fx);
        }

        float inv_span = 1.0f / (2.0f * softness);
        float glow_env = (progress > 0.05f && progress < 0.95f)
                             ? (sinf(progress * 3.1415926535f) * 0.25f)
                             : 0.0f;
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
        float edge0 = threshold - softness;
        bool has_avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif

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
            float dv[48];
            for (int i = 0; i < grid_w; ++i) {
                v[i] = grid[iy][i] * (1.0f - cy) + grid[iy + 1][i] * cy;
            }
            for (int i = 0; i < grid_w - 1; ++i) {
                dv[i] = v[i + 1] - v[i];
            }
            dv[grid_w - 1] = 0.0f;

            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
            if (has_avx2 && glow_env <= 0.0f) {
                luma_row_avx2(dst_row, a_row, b_row, v, dv, lut_ix, lut_cx, width, edge0, inv_span);
                continue;
            }
#endif
            for (uint32_t x = 0; x < width; ++x) {
                int ix = lut_ix[x];
                float n = v[ix] + dv[ix] * lut_cx[x];
                float alpha = smoothstepf(threshold - softness, threshold + softness, n);
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
                if (glow_env > 0.0f) {
                    float edge = 1.0f - fabsf(n - threshold) * inv_span;
                    if (edge > 0.0f) {
                        float glow = edge * edge * glow_env;
                        int gr = (int)(glow * 0.3f * 255.0f);
                        int gg = (int)(glow * 0.6f * 255.0f);
                        int gb = (int)(glow * 1.0f * 255.0f);
                        int r = (int)((base >> 16) & 0xFF) + gr;
                        int g = (int)((base >> 8) & 0xFF) + gg;
                        int b = (int)(base & 0xFF) + gb;
                        if (r > 255)
                            r = 255;
                        if (g > 255)
                            g = 255;
                        if (b > 255)
                            b = 255;
                        base = 0xFF000000U | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                    }
                }
                dst_row[x] = base;
            }
        }

        if (lut_ix != lut_ix_stack)
            free(lut_ix);
        if (lut_cx != lut_cx_stack)
            free(lut_cx);
        return true;
    }

    case WAYWAL_TRANSITION_LIGHT_LEAK: {
        float mix_factor = smoothstepf(0.25f, 0.75f, progress);
        uint16_t wb = (uint16_t)(mix_factor * 256.0f);
        float sweep = progress * 1.6f - 0.3f;
        float env = sinf(progress * 3.1415926535f);
        env = env * env;
        float flash_mul = 180.0f * env;

        if (flash_mul < 1.0f) {
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

#define LIGHT_LEAK_LUT_SIZE 1024
        uint32_t flare_lut[LIGHT_LEAK_LUT_SIZE + 1];
        const float max_flare_dist = 0.56f;
        const float lut_scale = (float)LIGHT_LEAK_LUT_SIZE / max_flare_dist;
        const float inv_lut_scale = max_flare_dist / (float)LIGHT_LEAK_LUT_SIZE;
        for (int i = 0; i <= LIGHT_LEAK_LUT_SIZE; ++i) {
            float d = (float)i * inv_lut_scale;
            float base = expf(-d * d * 32.0f) * flash_mul;
            int fr = (int)(base * 1.1f);
            int fg = (int)(base * 0.65f);
            int fb = (int)(base * 0.25f);
            if (fr > 255)
                fr = 255;
            else if (fr < 0)
                fr = 0;
            if (fg > 255)
                fg = 255;
            else if (fg < 0)
                fg = 0;
            if (fb > 255)
                fb = 255;
            else if (fb < 0)
                fb = 0;
            flare_lut[i] = 0xFF000000U | ((uint32_t)fr << 16) | ((uint32_t)fg << 8) | (uint32_t)fb;
        }

        float d_cutoff = sqrtf(logf(flash_mul * 1.1f) / 32.0f) + 0.002f;
        if (d_cutoff > max_flare_dist)
            d_cutoff = max_flare_dist;

        float inv_w = 1.0f / (float)width;
        float inv_h = 1.0f / (float)height;
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
        bool has_avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float vy = (float)y * inv_h;
            float row_offset = vy * 0.25f - sweep;
            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

            int x_start = span_floor_clamp((-row_offset - d_cutoff) * (float)width, (int)width);
            int x_end = span_ceil_clamp((-row_offset + d_cutoff) * (float)width, (int)width);
            if (x_start > x_end)
                x_start = x_end;

            if (x_start > 0) {
                blend_fn((uint8_t *)dst_row, (const uint8_t *)a_row, (const uint8_t *)b_row, wb,
                         (size_t)x_start * sizeof(uint32_t));
            }

            int x = x_start;
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
            if (has_avx2) {
                int avx_count = (x_end - x_start) & ~7;
                if (avx_count > 0) {
                    light_leak_span_avx2(dst_row, a_row, b_row, flare_lut, x_start,
                                         x_start + avx_count, inv_w, row_offset, lut_scale,
                                         LIGHT_LEAK_LUT_SIZE, wb);
                    x = x_start + avx_count;
                }
            }
#endif
            for (; x < x_end; ++x) {
                float vx = (float)x * inv_w;
                float dist = fabsf(vx + row_offset);
                uint32_t blended = blend_pixel_scalar(a_row[x], b_row[x], wb);
                if (dist < max_flare_dist) {
                    int idx = (int)(dist * lut_scale);
                    if (idx > LIGHT_LEAK_LUT_SIZE)
                        idx = LIGHT_LEAK_LUT_SIZE;
                    uint32_t flare = flare_lut[idx];
                    if ((flare & 0x00FFFFFFU) != 0) {
                        uint32_t r = ((blended >> 16) & 0xFFU) + ((flare >> 16) & 0xFFU);
                        uint32_t g = ((blended >> 8) & 0xFFU) + ((flare >> 8) & 0xFFU);
                        uint32_t b = (blended & 0xFFU) + (flare & 0xFFU);
                        if (r > 255U)
                            r = 255U;
                        if (g > 255U)
                            g = 255U;
                        if (b > 255U)
                            b = 255U;
                        dst_row[x] = 0xFF000000U | (r << 16) | (g << 8) | b;
                    } else {
                        dst_row[x] = blended;
                    }
                } else {
                    dst_row[x] = blended;
                }
            }

            if (x_end < (int)width) {
                blend_fn((uint8_t *)(dst_row + x_end), (const uint8_t *)(a_row + x_end),
                         (const uint8_t *)(b_row + x_end), wb,
                         (size_t)((int)width - x_end) * sizeof(uint32_t));
            }
        }
#undef LIGHT_LEAK_LUT_SIZE
        simd_stream_fence();
        return true;
    }

    case WAYWAL_TRANSITION_PAGE_CURL: {
        float R = 0.12f;
        float r2 = R * R;
        float line = 1.02f - progress * 1.25f;
        float inv_w = 1.0f / (float)width;
        float inv_h = 1.0f / (float)height;
        float inv_R_lut = 255.0f / R;
        float angle = (fabsf(params->angle_rad) > 1e-5f) ? params->angle_rad : 0.78539816f;
        float dir_x = cosf(angle);
        float dir_y = -sinf(angle);
        float drop_shadow_width = 0.08f;

        uint16_t curl_wb_lut[256];
        uint16_t shadow_wb_lut[256];
        float curl_offset_lut[256];
        for (int i = 0; i < 256; ++i) {
            float t = (float)i / 255.0f;
            float dist = t * R;
            float rad2 = r2 - dist * dist;
            curl_offset_lut[i] = (rad2 > 0.0f) ? (R - sqrtf(rad2)) : R;
            float curl_light = 0.85f + 0.35f * sinf(t * 3.1415926535f);
            curl_wb_lut[i] = (uint16_t)clampf(curl_light * 230.0f, 0.0f, 256.0f);
            float sm = t * t * (3.0f - 2.0f * t);
            shadow_wb_lut[i] = (uint16_t)((0.5f + 0.5f * sm) * 256.0f);
        }

#pragma omp parallel for schedule(static, 8)
        for (uint32_t y = 0; y < height; ++y) {
            float vy = (float)y * inv_h;
            uint32_t *dst_row = dst + (size_t)y * stride_pixels;
            const uint32_t *a_row = old_pixels + (size_t)y * width;
            const uint32_t *b_row = new_pixels + (size_t)y * width;

            for (int x = 0; x < (int)width; ++x) {
                float vx = (float)x * inv_w;
                float s = ((vx - 0.5f) * dir_x + (vy - 0.5f) * dir_y) * 0.70710678f + 0.5f;
                if (s < line) {
                    dst_row[x] = a_row[x];
                } else if (s < line + R) {
                    float dist = s - line;
                    int lut_idx = (int)(dist * inv_R_lut);
                    if (lut_idx < 0)
                        lut_idx = 0;
                    else if (lut_idx > 255)
                        lut_idx = 255;

                    float curl_off = curl_offset_lut[lut_idx] * 1.41421356f;
                    float curl_u = vx - curl_off * dir_x;
                    float curl_v = vy - curl_off * dir_y;
                    if (curl_u >= 0.0f && curl_u <= 1.0f && curl_v >= 0.0f && curl_v <= 1.0f) {
                        int sx = (int)(curl_u * (float)width);
                        int sy = (int)(curl_v * (float)height);
                        if (sx >= (int)width)
                            sx = (int)width - 1;
                        if (sy >= (int)height)
                            sy = (int)height - 1;
                        uint32_t col = old_pixels[(size_t)sy * width + sx];
                        dst_row[x] = blend_pixel_scalar(0xFF000000, col, curl_wb_lut[lut_idx]);
                    } else {
                        dst_row[x] =
                            blend_pixel_scalar(0xFF000000, b_row[x], shadow_wb_lut[lut_idx]);
                    }
                } else if (s < line + R + drop_shadow_width) {
                    float t_sh = clampf((s - (line + R)) / drop_shadow_width, 0.0f, 1.0f);
                    float sm = t_sh * t_sh * (3.0f - 2.0f * t_sh);
                    uint16_t wb_sh = (uint16_t)((0.65f + 0.35f * sm) * 256.0f);
                    dst_row[x] = blend_pixel_scalar(0xFF000000, b_row[x], wb_sh);
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
