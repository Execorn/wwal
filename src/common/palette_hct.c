// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/palette_hct.h"

#include "waywal/log.h"
#include "waywal/path.h"
#include "waywal/toml_parser.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#if defined(__x86_64__) && !defined(WWAL_DISABLE_SIMD)
#include <immintrin.h>
#endif
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define WU_BINS       32
#define WU_GRID       33
#define WU_MAX_BOXES  64
#define KMEANS_K      16
#define KMEANS_ITERS  6
#define SUBSAMPLE_DIM 128

#define PI_F         3.14159265358979323846f
#define TWO_PI_F     6.28318530717958647692f
#define DEG_TO_RAD_F (PI_F / 180.0f)
#define RAD_TO_DEG_F (180.0f / PI_F)

typedef struct {
    int l0, l1;
    int a0, a1;
    int b0, b1;
    float vol_variance;
} wu_box_t;

typedef struct {
    float w, l, a, b, m2, pad[3];
} wu_bin_t;

typedef struct {
    wu_bin_t bins[WU_GRID][WU_GRID][WU_GRID] __attribute__((aligned(32)));
    uint16_t active_bins[WU_BINS * WU_BINS * WU_BINS];
    uint32_t num_active;
    int min_l, max_l;
    int min_a, max_a;
    int min_b, max_b;
} wu_hist_t;

typedef struct {
    float l;
    float a;
    float b;
    float weight;
} weighted_point_t;

static float g_srgb_to_linear[256];
static uint8_t g_linear_to_srgb_lut[4097];
static atomic_bool g_srgb_lut_ready = false;

static inline int clamp_i(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }

static void ensure_srgb_lut(void)
{
    if (atomic_load_explicit(&g_srgb_lut_ready, memory_order_acquire)) {
        return;
    }
    for (int i = 0; i < 256; ++i) {
        float s = (float)i * (1.0f / 255.0f);
        if (s <= 0.04045f) {
            g_srgb_to_linear[i] = s * (1.0f / 12.92f);
        } else {
            g_srgb_to_linear[i] = powf((s + 0.055f) * (1.0f / 1.055f), 2.4f);
        }
    }
    for (int i = 0; i <= 4096; ++i) {
        float u = (float)i * (1.0f / 4096.0f);
        float c = u * u;
        float s = (c <= 0.0031308f) ? (12.92f * c) : (1.055f * powf(c, 1.0f / 2.4f) - 0.055f);
        int v = (int)(s * 255.0f + 0.5f);
        g_linear_to_srgb_lut[i] = (uint8_t)clamp_i(v, 0, 255);
    }
    atomic_store_explicit(&g_srgb_lut_ready, true, memory_order_release);
}

__attribute__((constructor)) static void init_palette_lut_ctor(void) { ensure_srgb_lut(); }

static inline float clamp_f(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

static inline float wrap_radians(float h)
{
    h = fmodf(h, TWO_PI_F);
    if (h < 0.0f) {
        h += TWO_PI_F;
    }
    return h;
}

static inline float hue_diff_deg(float h1_deg, float h2_deg)
{
    float d = fabsf(h1_deg - h2_deg);
    if (d > 180.0f) {
        d = 360.0f - d;
    }
    return d;
}

static inline float fast_cbrtf_scalar(float x)
{
    if (x <= 1e-24f) {
        return 0.0f;
    }
    union {
        float f;
        uint32_t u;
    } conv = {.f = x};
    conv.u = 0x54a21d2aU - (conv.u / 3U);
    float r = conv.f;
    const float four_thirds = 4.0f / 3.0f;
    const float neg_third_x = (-1.0f / 3.0f) * x;
    r = r * (four_thirds + neg_third_x * (r * r * r));
    r = r * (four_thirds + neg_third_x * (r * r * r));
    return x * (r * r);
}

static inline uint8_t linear_to_srgb_u8(float c)
{
    if (c <= 0.0f) {
        return 0;
    }
    if (c >= 1.0f) {
        return 255;
    }
    ensure_srgb_lut();
    int idx = (int)(sqrtf(c) * 4096.0f + 0.5f);
    return g_linear_to_srgb_lut[clamp_i(idx, 0, 4096)];
}

static inline waywal_oklab_t linear_rgb_to_oklab(float r, float g, float b)
{
    float l = 0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b;
    float m = 0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b;
    float s = 0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b;

    float l_ = fast_cbrtf_scalar(l);
    float m_ = fast_cbrtf_scalar(m);
    float s_ = fast_cbrtf_scalar(s);

    waywal_oklab_t out;
    out.l = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_;
    out.a = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
    out.b = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;
    return out;
}

static inline void oklab_to_linear_rgb(float l, float a, float b, float *out_r, float *out_g,
                                       float *out_b)
{
    float l_ = l + 0.3963377774f * a + 0.2158037573f * b;
    float m_ = l - 0.1055613458f * a - 0.0638541728f * b;
    float s_ = l - 0.0894841775f * a - 1.2914855480f * b;

    float l3 = l_ * l_ * l_;
    float m3 = m_ * m_ * m_;
    float s3 = s_ * s_ * s_;

    *out_r = +4.0767416621f * l3 - 3.3077115913f * m3 + 0.2309699292f * s3;
    *out_g = -1.2684380046f * l3 + 2.6097574011f * m3 - 0.3413193965f * s3;
    *out_b = -0.0041960863f * l3 - 0.7034186147f * m3 + 1.7076147010f * s3;
}

static inline bool linear_rgb_in_gamut(float r, float g, float b)
{
    const float eps = 1e-4f;
    return (r >= -eps && r <= 1.0f + eps && g >= -eps && g <= 1.0f + eps && b >= -eps &&
            b <= 1.0f + eps);
}

#if defined(__x86_64__) && !defined(WWAL_DISABLE_SIMD)
__attribute__((target("avx2,fma"))) static inline __m256 avx2_cbrt_ps(__m256 x)
{
    const __m256 eps = _mm256_set1_ps(1e-24f);
    __m256 valid_mask = _mm256_cmp_ps(x, eps, _CMP_GT_OQ);
    __m256 xc = _mm256_max_ps(x, eps);

    __m256i xi = _mm256_castps_si256(xc);
    __m256 xf = _mm256_cvtepi32_ps(xi);
    __m256i third = _mm256_cvttps_epi32(_mm256_mul_ps(xf, _mm256_set1_ps(0.3333333333f)));
    __m256i ri = _mm256_sub_epi32(_mm256_set1_epi32(0x54a21d2a), third);
    __m256 r = _mm256_castsi256_ps(ri);

    const __m256 four_thirds = _mm256_set1_ps(4.0f / 3.0f);
    __m256 neg_third_x = _mm256_mul_ps(_mm256_set1_ps(-1.0f / 3.0f), xc);

    __m256 r2 = _mm256_mul_ps(r, r);
    __m256 r3 = _mm256_mul_ps(r2, r);
    r = _mm256_mul_ps(r, _mm256_fmadd_ps(neg_third_x, r3, four_thirds));

    r2 = _mm256_mul_ps(r, r);
    r3 = _mm256_mul_ps(r2, r);
    r = _mm256_mul_ps(r, _mm256_fmadd_ps(neg_third_x, r3, four_thirds));

    r2 = _mm256_mul_ps(r, r);
    __m256 y = _mm256_mul_ps(xc, r2);
    return _mm256_and_ps(y, valid_mask);
}
#endif

static inline void wu_hist_reset(wu_hist_t *hist)
{
    memset(hist->bins, 0, sizeof(hist->bins));
    hist->num_active = 0;
    hist->min_l = WU_BINS;
    hist->max_l = 1;
    hist->min_a = WU_BINS;
    hist->max_a = 1;
    hist->min_b = WU_BINS;
    hist->max_b = 1;
}

static inline void wu_accum_pixel(wu_hist_t *hist, float l, float a, float b)
{
    int bl = clamp_i((int)(l * (float)WU_BINS) + 1, 1, WU_BINS);
    int ba = clamp_i((int)(((a + 0.4f) * (1.0f / 0.8f)) * (float)WU_BINS) + 1, 1, WU_BINS);
    int bb = clamp_i((int)(((b + 0.4f) * (1.0f / 0.8f)) * (float)WU_BINS) + 1, 1, WU_BINS);

    wu_bin_t *bin = &hist->bins[bl][ba][bb];
    if (bin->w == 0.0f) {
        if (hist->num_active < (uint32_t)(WU_BINS * WU_BINS * WU_BINS)) {
            hist->active_bins[hist->num_active++] =
                (uint16_t)(((bl - 1) << 10) | ((ba - 1) << 5) | (bb - 1));
        }
        if (bl < hist->min_l)
            hist->min_l = bl;
        if (bl > hist->max_l)
            hist->max_l = bl;
        if (ba < hist->min_a)
            hist->min_a = ba;
        if (ba > hist->max_a)
            hist->max_a = ba;
        if (bb < hist->min_b)
            hist->min_b = bb;
        if (bb > hist->max_b)
            hist->max_b = bb;
    }
    bin->w += 1.0f;
    bin->l += l;
    bin->a += a;
    bin->b += b;
    bin->m2 += (l * l + a * a + b * b);
}

#if defined(__x86_64__) && !defined(WWAL_DISABLE_SIMD)
__attribute__((target("avx2,fma"))) static size_t
wu_populate_histogram_avx2(wu_hist_t *hist, const uint8_t *pixels, uint32_t width, uint32_t height,
                           uint32_t stride_bytes, bool is_bgra)
{
    wu_hist_reset(hist);

    uint32_t step_x = width > SUBSAMPLE_DIM ? (width / SUBSAMPLE_DIM) : 1U;
    uint32_t step_y = height > SUBSAMPLE_DIM ? (height / SUBSAMPLE_DIM) : 1U;
    if (step_x == 0U)
        step_x = 1U;
    if (step_y == 0U)
        step_y = 1U;

    size_t valid_samples = 0;
    const int r_off = is_bgra ? 2 : 0;
    const int b_off = is_bgra ? 0 : 2;

    float r_lin[8] __attribute__((aligned(32)));
    float g_lin[8] __attribute__((aligned(32)));
    float b_lin[8] __attribute__((aligned(32)));
    float out_l[8] __attribute__((aligned(32)));
    float out_a[8] __attribute__((aligned(32)));
    float out_b[8] __attribute__((aligned(32)));
    int batch_count = 0;

    for (uint32_t y = 0; y < height; y += step_y) {
        const uint8_t *row = pixels + (size_t)y * stride_bytes;
        for (uint32_t x = 0; x < width; x += step_x) {
            const uint8_t *px = row + (size_t)x * 4U;
            if (px[3] == 0U) {
                continue;
            }
            valid_samples++;

            r_lin[batch_count] = g_srgb_to_linear[px[r_off]];
            g_lin[batch_count] = g_srgb_to_linear[px[1]];
            b_lin[batch_count] = g_srgb_to_linear[px[b_off]];
            batch_count++;

            if (batch_count == 8) {
                __m256 vr = _mm256_load_ps(r_lin);
                __m256 vg = _mm256_load_ps(g_lin);
                __m256 vb = _mm256_load_ps(b_lin);

                __m256 vl = _mm256_mul_ps(_mm256_set1_ps(0.4122214708f), vr);
                vl = _mm256_fmadd_ps(_mm256_set1_ps(0.5363325363f), vg, vl);
                vl = _mm256_fmadd_ps(_mm256_set1_ps(0.0514459929f), vb, vl);

                __m256 vm = _mm256_mul_ps(_mm256_set1_ps(0.2119034982f), vr);
                vm = _mm256_fmadd_ps(_mm256_set1_ps(0.6806995451f), vg, vm);
                vm = _mm256_fmadd_ps(_mm256_set1_ps(0.1073969566f), vb, vm);

                __m256 vs = _mm256_mul_ps(_mm256_set1_ps(0.0883024619f), vr);
                vs = _mm256_fmadd_ps(_mm256_set1_ps(0.2817188376f), vg, vs);
                vs = _mm256_fmadd_ps(_mm256_set1_ps(0.6299787005f), vb, vs);

                __m256 vl_c = avx2_cbrt_ps(vl);
                __m256 vm_c = avx2_cbrt_ps(vm);
                __m256 vs_c = avx2_cbrt_ps(vs);

                __m256 ok_l = _mm256_mul_ps(_mm256_set1_ps(0.2104542553f), vl_c);
                ok_l = _mm256_fmadd_ps(_mm256_set1_ps(0.7936177850f), vm_c, ok_l);
                ok_l = _mm256_fmadd_ps(_mm256_set1_ps(-0.0040720468f), vs_c, ok_l);

                __m256 ok_a = _mm256_mul_ps(_mm256_set1_ps(1.9779984951f), vl_c);
                ok_a = _mm256_fmadd_ps(_mm256_set1_ps(-2.4285922050f), vm_c, ok_a);
                ok_a = _mm256_fmadd_ps(_mm256_set1_ps(0.4505937099f), vs_c, ok_a);

                __m256 ok_b = _mm256_mul_ps(_mm256_set1_ps(0.0259040371f), vl_c);
                ok_b = _mm256_fmadd_ps(_mm256_set1_ps(0.7827717662f), vm_c, ok_b);
                ok_b = _mm256_fmadd_ps(_mm256_set1_ps(-0.8086757660f), vs_c, ok_b);

                _mm256_store_ps(out_l, ok_l);
                _mm256_store_ps(out_a, ok_a);
                _mm256_store_ps(out_b, ok_b);

                for (int i = 0; i < 8; ++i) {
                    wu_accum_pixel(hist, out_l[i], out_a[i], out_b[i]);
                }
                batch_count = 0;
            }
        }
    }

    for (int i = 0; i < batch_count; ++i) {
        waywal_oklab_t ok = linear_rgb_to_oklab(r_lin[i], g_lin[i], b_lin[i]);
        wu_accum_pixel(hist, ok.l, ok.a, ok.b);
    }

    return valid_samples;
}

__attribute__((target("avx2,fma"))) static void wu_compute_moments_avx2(wu_hist_t *hist)
{
    int l0 = hist->min_l <= hist->max_l ? hist->min_l : 1;
    int l1 = hist->min_l <= hist->max_l ? hist->max_l : WU_BINS;
    int a0 = hist->min_a <= hist->max_a ? hist->min_a : 1;
    int a1 = hist->min_a <= hist->max_a ? hist->max_a : WU_BINS;
    int b0 = hist->min_b <= hist->max_b ? hist->min_b : 1;
    int b1 = hist->min_b <= hist->max_b ? hist->max_b : WU_BINS;

    for (int l = l0; l <= l1; ++l) {
        __m256 area[WU_GRID];
        for (int b = b0; b <= b1; ++b) {
            area[b] = _mm256_setzero_ps();
        }

        for (int a = a0; a <= a1; ++a) {
            __m256 line = _mm256_setzero_ps();
            for (int b = b0; b <= b1; ++b) {
                __m256 cur = _mm256_load_ps(&hist->bins[l][a][b].w);
                line = _mm256_add_ps(line, cur);
                area[b] = _mm256_add_ps(area[b], line);
                __m256 prev_l = _mm256_load_ps(&hist->bins[l - 1][a][b].w);
                _mm256_store_ps(&hist->bins[l][a][b].w, _mm256_add_ps(prev_l, area[b]));
            }
        }
    }
}
#endif

static size_t wu_populate_histogram(wu_hist_t *hist, const uint8_t *pixels, uint32_t width,
                                    uint32_t height, uint32_t stride_bytes, bool is_bgra)
{
#if defined(__x86_64__) && !defined(WWAL_DISABLE_SIMD)
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) {
        return wu_populate_histogram_avx2(hist, pixels, width, height, stride_bytes, is_bgra);
    }
#endif
    wu_hist_reset(hist);

    uint32_t step_x = width > SUBSAMPLE_DIM ? (width / SUBSAMPLE_DIM) : 1U;
    uint32_t step_y = height > SUBSAMPLE_DIM ? (height / SUBSAMPLE_DIM) : 1U;
    if (step_x == 0U)
        step_x = 1U;
    if (step_y == 0U)
        step_y = 1U;

    size_t valid_samples = 0;
    const int r_off = is_bgra ? 2 : 0;
    const int b_off = is_bgra ? 0 : 2;

    for (uint32_t y = 0; y < height; y += step_y) {
        const uint8_t *row = pixels + (size_t)y * stride_bytes;
        for (uint32_t x = 0; x < width; x += step_x) {
            const uint8_t *px = row + (size_t)x * 4U;
            if (px[3] == 0U) {
                continue;
            }
            valid_samples++;

            float r = g_srgb_to_linear[px[r_off]];
            float g = g_srgb_to_linear[px[1]];
            float b = g_srgb_to_linear[px[b_off]];
            waywal_oklab_t ok = linear_rgb_to_oklab(r, g, b);
            wu_accum_pixel(hist, ok.l, ok.a, ok.b);
        }
    }

    return valid_samples;
}

static void wu_compute_moments(wu_hist_t *hist)
{
    if (hist->num_active <= WU_MAX_BOXES) {
        return;
    }
#if defined(__x86_64__) && !defined(WWAL_DISABLE_SIMD)
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) {
        wu_compute_moments_avx2(hist);
        return;
    }
#endif
    int l0 = hist->min_l <= hist->max_l ? hist->min_l : 1;
    int l1 = hist->min_l <= hist->max_l ? hist->max_l : WU_BINS;
    int a0 = hist->min_a <= hist->max_a ? hist->min_a : 1;
    int a1 = hist->min_a <= hist->max_a ? hist->max_a : WU_BINS;
    int b0 = hist->min_b <= hist->max_b ? hist->min_b : 1;
    int b1 = hist->min_b <= hist->max_b ? hist->max_b : WU_BINS;

    for (int l = l0; l <= l1; ++l) {
        wu_bin_t area[WU_GRID];
        memset(area, 0, sizeof(area));

        for (int a = a0; a <= a1; ++a) {
            float line_w = 0.0f;
            float line_l = 0.0f;
            float line_a = 0.0f;
            float line_b = 0.0f;
            float line_2 = 0.0f;

            for (int b = b0; b <= b1; ++b) {
                wu_bin_t *cur = &hist->bins[l][a][b];
                const wu_bin_t *prev = &hist->bins[l - 1][a][b];
                line_w += cur->w;
                line_l += cur->l;
                line_a += cur->a;
                line_b += cur->b;
                line_2 += cur->m2;

                area[b].w += line_w;
                area[b].l += line_l;
                area[b].a += line_a;
                area[b].b += line_b;
                area[b].m2 += line_2;

                cur->w = prev->w + area[b].w;
                cur->l = prev->l + area[b].l;
                cur->a = prev->a + area[b].a;
                cur->b = prev->b + area[b].b;
                cur->m2 = prev->m2 + area[b].m2;
            }
        }
    }
}

static inline wu_bin_t wu_vol_bin(const wu_box_t *box, const wu_hist_t *hist)
{
    wu_bin_t out;
#if defined(__AVX2__) && !defined(WWAL_DISABLE_SIMD)
    __m256 v = _mm256_load_ps(&hist->bins[box->l1][box->a1][box->b1].w);
    v = _mm256_sub_ps(v, _mm256_load_ps(&hist->bins[box->l1][box->a1][box->b0].w));
    v = _mm256_sub_ps(v, _mm256_load_ps(&hist->bins[box->l1][box->a0][box->b1].w));
    v = _mm256_add_ps(v, _mm256_load_ps(&hist->bins[box->l1][box->a0][box->b0].w));
    v = _mm256_sub_ps(v, _mm256_load_ps(&hist->bins[box->l0][box->a1][box->b1].w));
    v = _mm256_add_ps(v, _mm256_load_ps(&hist->bins[box->l0][box->a1][box->b0].w));
    v = _mm256_add_ps(v, _mm256_load_ps(&hist->bins[box->l0][box->a0][box->b1].w));
    v = _mm256_sub_ps(v, _mm256_load_ps(&hist->bins[box->l0][box->a0][box->b0].w));
    _mm256_storeu_ps(&out.w, v);
#else
    const wu_bin_t *c111 = &hist->bins[box->l1][box->a1][box->b1];
    const wu_bin_t *c110 = &hist->bins[box->l1][box->a1][box->b0];
    const wu_bin_t *c101 = &hist->bins[box->l1][box->a0][box->b1];
    const wu_bin_t *c100 = &hist->bins[box->l1][box->a0][box->b0];
    const wu_bin_t *c011 = &hist->bins[box->l0][box->a1][box->b1];
    const wu_bin_t *c010 = &hist->bins[box->l0][box->a1][box->b0];
    const wu_bin_t *c001 = &hist->bins[box->l0][box->a0][box->b1];
    const wu_bin_t *c000 = &hist->bins[box->l0][box->a0][box->b0];
    out.w = c111->w - c110->w - c101->w + c100->w - c011->w + c010->w + c001->w - c000->w;
    out.l = c111->l - c110->l - c101->l + c100->l - c011->l + c010->l + c001->l - c000->l;
    out.a = c111->a - c110->a - c101->a + c100->a - c011->a + c010->a + c001->a - c000->a;
    out.b = c111->b - c110->b - c101->b + c100->b - c011->b + c010->b + c001->b - c000->b;
    out.m2 = c111->m2 - c110->m2 - c101->m2 + c100->m2 - c011->m2 + c010->m2 + c001->m2 - c000->m2;
#endif
    return out;
}

static inline wu_bin_t wu_bottom_bin(const wu_box_t *box, int dir, const wu_hist_t *hist)
{
    wu_bin_t out;
    const wu_bin_t *p0, *p1, *p2, *p3;
    if (dir == 0) {
        p0 = &hist->bins[box->l0][box->a1][box->b1];
        p1 = &hist->bins[box->l0][box->a1][box->b0];
        p2 = &hist->bins[box->l0][box->a0][box->b1];
        p3 = &hist->bins[box->l0][box->a0][box->b0];
    } else if (dir == 1) {
        p0 = &hist->bins[box->l1][box->a0][box->b1];
        p1 = &hist->bins[box->l1][box->a0][box->b0];
        p2 = &hist->bins[box->l0][box->a0][box->b1];
        p3 = &hist->bins[box->l0][box->a0][box->b0];
    } else {
        p0 = &hist->bins[box->l1][box->a1][box->b0];
        p1 = &hist->bins[box->l1][box->a0][box->b0];
        p2 = &hist->bins[box->l0][box->a1][box->b0];
        p3 = &hist->bins[box->l0][box->a0][box->b0];
    }
#if defined(__AVX2__) && !defined(WWAL_DISABLE_SIMD)
    __m256 v = _mm256_sub_ps(_mm256_load_ps(&p1->w), _mm256_load_ps(&p0->w));
    v = _mm256_add_ps(v, _mm256_load_ps(&p2->w));
    v = _mm256_sub_ps(v, _mm256_load_ps(&p3->w));
    _mm256_storeu_ps(&out.w, v);
#else
    out.w = -p0->w + p1->w + p2->w - p3->w;
    out.l = -p0->l + p1->l + p2->l - p3->l;
    out.a = -p0->a + p1->a + p2->a - p3->a;
    out.b = -p0->b + p1->b + p2->b - p3->b;
    out.m2 = -p0->m2 + p1->m2 + p2->m2 - p3->m2;
#endif
    return out;
}

static inline wu_bin_t wu_top_bin(const wu_box_t *box, int dir, int pos, const wu_hist_t *hist)
{
    wu_bin_t out;
    const wu_bin_t *p0, *p1, *p2, *p3;
    if (dir == 0) {
        p0 = &hist->bins[pos][box->a1][box->b1];
        p1 = &hist->bins[pos][box->a1][box->b0];
        p2 = &hist->bins[pos][box->a0][box->b1];
        p3 = &hist->bins[pos][box->a0][box->b0];
    } else if (dir == 1) {
        p0 = &hist->bins[box->l1][pos][box->b1];
        p1 = &hist->bins[box->l1][pos][box->b0];
        p2 = &hist->bins[box->l0][pos][box->b1];
        p3 = &hist->bins[box->l0][pos][box->b0];
    } else {
        p0 = &hist->bins[box->l1][box->a1][pos];
        p1 = &hist->bins[box->l1][box->a0][pos];
        p2 = &hist->bins[box->l0][box->a1][pos];
        p3 = &hist->bins[box->l0][box->a0][pos];
    }
#if defined(__AVX2__) && !defined(WWAL_DISABLE_SIMD)
    __m256 v = _mm256_sub_ps(_mm256_load_ps(&p0->w), _mm256_load_ps(&p1->w));
    v = _mm256_sub_ps(v, _mm256_load_ps(&p2->w));
    v = _mm256_add_ps(v, _mm256_load_ps(&p3->w));
    _mm256_storeu_ps(&out.w, v);
#else
    out.w = p0->w - p1->w - p2->w + p3->w;
    out.l = p0->l - p1->l - p2->l + p3->l;
    out.a = p0->a - p1->a - p2->a + p3->a;
    out.b = p0->b - p1->b - p2->b + p3->b;
    out.m2 = p0->m2 - p1->m2 - p2->m2 + p3->m2;
#endif
    return out;
}

static float wu_box_variance(const wu_hist_t *hist, const wu_box_t *box)
{
    wu_bin_t vol = wu_vol_bin(box, hist);
    if (vol.w <= 0.0f) {
        return 0.0f;
    }
    float v = vol.m2 - (vol.l * vol.l + vol.a * vol.a + vol.b * vol.b) / vol.w;
    return v > 0.0f ? v : 0.0f;
}

static float wu_maximize(const wu_hist_t *hist, const wu_box_t *box, int dir, int first, int last,
                         int *out_cut, float whole_w, float whole_l, float whole_a, float whole_b)
{
    wu_bin_t base = wu_bottom_bin(box, dir, hist);

    float max_score = -1.0f;
    *out_cut = -1;

    for (int i = first; i < last; ++i) {
        wu_bin_t top = wu_top_bin(box, dir, i, hist);
        float half_w = base.w + top.w;
        if (half_w <= 0.0f) {
            continue;
        }
        float rem_w = whole_w - half_w;
        if (rem_w <= 0.0f) {
            continue;
        }

        float half_l = base.l + top.l;
        float half_a = base.a + top.a;
        float half_b = base.b + top.b;

        float rem_l = whole_l - half_l;
        float rem_a = whole_a - half_a;
        float rem_b = whole_b - half_b;

        float score = (half_l * half_l + half_a * half_a + half_b * half_b) / half_w +
                      (rem_l * rem_l + rem_a * rem_a + rem_b * rem_b) / rem_w;
        if (score > max_score) {
            max_score = score;
            *out_cut = i;
        }
    }
    return max_score;
}

static bool wu_cut(const wu_hist_t *hist, wu_box_t *set1, wu_box_t *set2)
{
    wu_bin_t whole = wu_vol_bin(set1, hist);
    if (whole.w <= 0.0f) {
        return false;
    }

    int cut_l = -1, cut_a = -1, cut_b = -1;
    float max_l = wu_maximize(hist, set1, 0, set1->l0 + 1, set1->l1, &cut_l, whole.w, whole.l,
                              whole.a, whole.b);
    float max_a = wu_maximize(hist, set1, 1, set1->a0 + 1, set1->a1, &cut_a, whole.w, whole.l,
                              whole.a, whole.b);
    float max_b = wu_maximize(hist, set1, 2, set1->b0 + 1, set1->b1, &cut_b, whole.w, whole.l,
                              whole.a, whole.b);

    int dir = 0;
    float best = max_l;
    if (max_a >= best) {
        best = max_a;
        dir = 1;
    }
    if (max_b >= best) {
        best = max_b;
        dir = 2;
    }

    if (best < 0.0f) {
        set1->vol_variance = 0.0f;
        return false;
    }

    *set2 = *set1;
    if (dir == 0) {
        set1->l1 = cut_l;
        set2->l0 = cut_l;
    } else if (dir == 1) {
        set1->a1 = cut_a;
        set2->a0 = cut_a;
    } else {
        set1->b1 = cut_b;
        set2->b0 = cut_b;
    }

    set1->vol_variance = wu_box_variance(hist, set1);
    set2->vol_variance = wu_box_variance(hist, set2);
    return true;
}

static size_t wu_partition(const wu_hist_t *hist, weighted_point_t *out_points, size_t max_boxes)
{
    if (hist->num_active <= max_boxes) {
        size_t count = 0;
        for (uint32_t i = 0; i < hist->num_active && count < max_boxes; ++i) {
            uint16_t packed = hist->active_bins[i];
            int bl = ((packed >> 10) & 31) + 1;
            int ba = ((packed >> 5) & 31) + 1;
            int bb = (packed & 31) + 1;
            const wu_bin_t *bin = &hist->bins[bl][ba][bb];
            if (bin->w <= 0.0f) {
                continue;
            }
            float inv_w = 1.0f / bin->w;
            out_points[count].l = bin->l * inv_w;
            out_points[count].a = bin->a * inv_w;
            out_points[count].b = bin->b * inv_w;
            out_points[count].weight = bin->w;
            count++;
        }
        return count;
    }

    wu_box_t boxes[WU_MAX_BOXES];
    memset(boxes, 0, sizeof(boxes));

    boxes[0].l0 = (hist->min_l <= hist->max_l) ? (hist->min_l - 1) : 0;
    boxes[0].l1 = (hist->min_l <= hist->max_l) ? hist->max_l : WU_BINS;
    boxes[0].a0 = (hist->min_a <= hist->max_a) ? (hist->min_a - 1) : 0;
    boxes[0].a1 = (hist->min_a <= hist->max_a) ? hist->max_a : WU_BINS;
    boxes[0].b0 = (hist->min_b <= hist->max_b) ? (hist->min_b - 1) : 0;
    boxes[0].b1 = (hist->min_b <= hist->max_b) ? hist->max_b : WU_BINS;
    boxes[0].vol_variance = wu_box_variance(hist, &boxes[0]);

    size_t num_boxes = 1;
    while (num_boxes < max_boxes) {
        size_t best_idx = 0;
        float max_v = -1.0f;
        for (size_t i = 0; i < num_boxes; ++i) {
            if (boxes[i].vol_variance > max_v) {
                max_v = boxes[i].vol_variance;
                best_idx = i;
            }
        }
        if (max_v <= 1e-9f) {
            break;
        }
        if (wu_cut(hist, &boxes[best_idx], &boxes[num_boxes])) {
            num_boxes++;
        } else {
            boxes[best_idx].vol_variance = 0.0f;
        }
    }

    size_t count = 0;
    for (size_t i = 0; i < num_boxes; ++i) {
        wu_bin_t vol = wu_vol_bin(&boxes[i], hist);
        if (vol.w <= 0.0f) {
            continue;
        }
        float inv_w = 1.0f / vol.w;
        out_points[count].l = vol.l * inv_w;
        out_points[count].a = vol.a * inv_w;
        out_points[count].b = vol.b * inv_w;
        out_points[count].weight = vol.w;
        count++;
    }
    return count;
}

static inline float oklab_dist_sq(float l1, float a1, float b1, float l2, float a2, float b2)
{
    float dl = l1 - l2;
    float da = a1 - a2;
    float db = b1 - b2;
    return dl * dl + da * da + db * db;
}

static size_t weighted_kmeans_pp(const weighted_point_t *pts, size_t n_pts,
                                 weighted_point_t *centroids, size_t max_k)
{
    if (n_pts == 0) {
        return 0;
    }
    size_t k_target = n_pts < max_k ? n_pts : max_k;

    size_t first_idx = 0;
    float best_first_score = -1.0f;
    for (size_t i = 0; i < n_pts; ++i) {
        float chroma = sqrtf(pts[i].a * pts[i].a + pts[i].b * pts[i].b);
        float s = pts[i].weight * (1.0f + 4.0f * chroma);
        if (s > best_first_score) {
            best_first_score = s;
            first_idx = i;
        }
    }

    centroids[0] = pts[first_idx];
    float min_d2[WU_MAX_BOXES];
    for (size_t i = 0; i < n_pts; ++i) {
        min_d2[i] = oklab_dist_sq(pts[i].l, pts[i].a, pts[i].b, centroids[0].l, centroids[0].a,
                                  centroids[0].b);
    }

    for (size_t k = 1; k < k_target; ++k) {
        float total_wd2 = 0.0f;
        size_t max_wd2_idx = 0;
        float max_wd2 = -1.0f;

        for (size_t i = 0; i < n_pts; ++i) {
            float wd2 = pts[i].weight * min_d2[i];
            total_wd2 += wd2;
            if (wd2 > max_wd2) {
                max_wd2 = wd2;
                max_wd2_idx = i;
            }
        }

        if (total_wd2 <= 1e-12f) {
            k_target = k;
            break;
        }

        /* Golden-ratio quasi-random quantile keeps D^2 seeding deterministic across runs. */
        float q = fmodf(((float)k + 0.5f) * 0.61803398875f, 1.0f);
        float threshold = q * total_wd2;
        float accum = 0.0f;
        size_t chosen = max_wd2_idx;

        for (size_t i = 0; i < n_pts; ++i) {
            float wd2 = pts[i].weight * min_d2[i];
            accum += wd2;
            if (accum >= threshold && min_d2[i] > 1e-10f) {
                chosen = i;
                break;
            }
        }

        centroids[k] = pts[chosen];
        for (size_t i = 0; i < n_pts; ++i) {
            float d2 = oklab_dist_sq(pts[i].l, pts[i].a, pts[i].b, centroids[k].l, centroids[k].a,
                                     centroids[k].b);
            if (d2 < min_d2[i]) {
                min_d2[i] = d2;
            }
        }
    }

    for (int iter = 0; iter < KMEANS_ITERS; ++iter) {
        float sum_l[KMEANS_K] = {0.0f};
        float sum_a[KMEANS_K] = {0.0f};
        float sum_b[KMEANS_K] = {0.0f};
        float sum_w[KMEANS_K] = {0.0f};

        for (size_t i = 0; i < n_pts; ++i) {
            size_t best_c = 0;
            float best_d2 = oklab_dist_sq(pts[i].l, pts[i].a, pts[i].b, centroids[0].l,
                                          centroids[0].a, centroids[0].b);
            for (size_t c = 1; c < k_target; ++c) {
                float d2 = oklab_dist_sq(pts[i].l, pts[i].a, pts[i].b, centroids[c].l,
                                         centroids[c].a, centroids[c].b);
                if (d2 < best_d2) {
                    best_d2 = d2;
                    best_c = c;
                }
            }
            float w = pts[i].weight;
            sum_l[best_c] += w * pts[i].l;
            sum_a[best_c] += w * pts[i].a;
            sum_b[best_c] += w * pts[i].b;
            sum_w[best_c] += w;
        }

        for (size_t c = 0; c < k_target; ++c) {
            if (sum_w[c] > 0.0f) {
                float inv = 1.0f / sum_w[c];
                centroids[c].l = sum_l[c] * inv;
                centroids[c].a = sum_a[c] * inv;
                centroids[c].b = sum_b[c] * inv;
                centroids[c].weight = sum_w[c];
            } else {
                centroids[c].weight = 0.0f;
            }
        }
    }

    size_t valid_k = 0;
    for (size_t c = 0; c < k_target; ++c) {
        if (centroids[c].weight > 0.0f) {
            if (valid_k != c) {
                centroids[valid_k] = centroids[c];
            }
            valid_k++;
        }
    }

    return valid_k;
}

/* Algebraic CAM16 correlates under standard sRGB D65 viewing conditions (Li et al. 2017). */
static waywal_cam16_t linear_rgb_to_cam16(float r_lin, float g_lin, float b_lin, float *out_y)
{
    r_lin = clamp_f(r_lin, 0.0f, 1.0f);
    g_lin = clamp_f(g_lin, 0.0f, 1.0f);
    b_lin = clamp_f(b_lin, 0.0f, 1.0f);

    float x = (0.41233895f * r_lin + 0.35762064f * g_lin + 0.18051042f * b_lin) * 100.0f;
    float y = (0.2126f * r_lin + 0.7152f * g_lin + 0.0722f * b_lin) * 100.0f;
    float z = (0.01932141f * r_lin + 0.11916382f * g_lin + 0.95034478f * b_lin) * 100.0f;
    if (out_y) {
        *out_y = y;
    }

    float r_c = 0.401288f * x + 0.650173f * y - 0.051461f * z;
    float g_c = -0.250268f * x + 1.204414f * y + 0.045854f * z;
    float b_c = -0.002079f * x + 0.048952f * y + 0.953127f * z;

    const float d_r = 1.0211777f;
    const float d_g = 0.9863077f;
    const float d_b = 0.9339605f;
    const float fl = 0.38848145f;

    float r_d = r_c * d_r;
    float g_d = g_c * d_g;
    float b_d = b_c * d_b;

    float p_r = powf(fl * fabsf(r_d) * 0.01f, 0.42f);
    float p_g = powf(fl * fabsf(g_d) * 0.01f, 0.42f);
    float p_b = powf(fl * fabsf(b_d) * 0.01f, 0.42f);

    float r_a = copysignf(400.0f * p_r / (p_r + 27.13f), r_d);
    float g_a = copysignf(400.0f * p_g / (p_g + 27.13f), g_d);
    float b_a = copysignf(400.0f * p_b / (p_b + 27.13f), b_d);

    float cam_a = r_a - (12.0f * g_a - b_a) / 11.0f;
    float cam_b = (r_a + g_a - 2.0f * b_a) / 9.0f;
    float u = (20.0f * r_a + 20.0f * g_a + 21.0f * b_a) / 20.0f;
    float p2 = (40.0f * r_a + 20.0f * g_a + b_a) / 20.0f;

    const float a_w = 29.981f;
    const float n_bb = 1.016919f;
    float a_achrom = p2 * n_bb;
    float j = 100.0f * powf(clamp_f(a_achrom / a_w, 0.0f, 1.5f), 0.69f * 1.48f);

    float h_rad = atan2f(cam_b, cam_a);
    if (h_rad < 0.0f) {
        h_rad += TWO_PI_F;
    }
    float h_deg = h_rad * RAD_TO_DEG_F;

    float e_t = 0.25f * (cosf(h_rad + 2.0f) + 3.8f);
    float t = (50000.0f / 13.0f) * 1.0f * n_bb * e_t * sqrtf(cam_a * cam_a + cam_b * cam_b) /
              (u + 0.305f);
    float c = powf(t, 0.9f) * sqrtf(j * 0.01f) * 0.789f;

    waywal_cam16_t out = {.j = j, .c = c, .h_deg = h_deg};
    return out;
}

static waywal_cam16_t oklab_to_cam16(float l, float a, float b)
{
    float r_lin, g_lin, b_lin;
    oklab_to_linear_rgb(l, a, b, &r_lin, &g_lin, &b_lin);
    return linear_rgb_to_cam16(r_lin, g_lin, b_lin, NULL);
}

static void cam16_jch_to_linear_rgb_precomp(float j, float c, float cos_h, float sin_h,
                                            float p1_star, float *out_r, float *out_g, float *out_b,
                                            float *out_y)
{
    if (j <= 1e-6f) {
        *out_r = 0.0f;
        *out_g = 0.0f;
        *out_b = 0.0f;
        if (out_y) {
            *out_y = 0.0f;
        }
        return;
    }

    const float a_w = 29.981f;
    const float n_bb = 1.016919f;
    const float d_r = 1.0211777f;
    const float d_g = 0.9863077f;
    const float d_b = 0.9339605f;
    const float fl = 0.38848145f;

    float j_norm = j * 0.01f;
    float a_achrom = a_w * powf(j_norm, 1.0f / (0.69f * 1.48f));
    float p2 = a_achrom * (1.0f / n_bb);

    float alpha = (c <= 1e-6f) ? 0.0f : (c / sqrtf(j_norm));
    float t = (alpha <= 1e-6f) ? 0.0f : powf(alpha * (1.0f / 0.789f), 1.0f / 0.9f);

    float cam_a = 0.0f;
    float cam_b = 0.0f;

    if (t > 1e-6f) {
        float denom = 23.0f * p1_star + t * (11.0f * cos_h + 108.0f * sin_h);
        if (fabsf(denom) > 1e-6f) {
            float gamma = (23.0f * (p2 + 0.305f) * t) / denom;
            cam_a = gamma * cos_h;
            cam_b = gamma * sin_h;
        }
    }

    float r_a = (460.0f * p2 + 451.0f * cam_a + 288.0f * cam_b) * (1.0f / 1403.0f);
    float g_a = (460.0f * p2 - 891.0f * cam_a - 261.0f * cam_b) * (1.0f / 1403.0f);
    float b_a = (460.0f * p2 - 220.0f * cam_a - 6300.0f * cam_b) * (1.0f / 1403.0f);

    float abs_ra = clamp_f(fabsf(r_a), 0.0f, 399.99f);
    float abs_ga = clamp_f(fabsf(g_a), 0.0f, 399.99f);
    float abs_ba = clamp_f(fabsf(b_a), 0.0f, 399.99f);

    float p_r = (27.13f * abs_ra) / (400.0f - abs_ra);
    float p_g = (27.13f * abs_ga) / (400.0f - abs_ga);
    float p_b = (27.13f * abs_ba) / (400.0f - abs_ba);

    const float inv_042 = 1.0f / 0.42f;
    const float scale_fl = 100.0f / fl;
    float r_d = copysignf(scale_fl * powf(p_r, inv_042), r_a);
    float g_d = copysignf(scale_fl * powf(p_g, inv_042), g_a);
    float b_d = copysignf(scale_fl * powf(p_b, inv_042), b_a);

    float r_c = r_d * (1.0f / d_r);
    float g_c = g_d * (1.0f / d_g);
    float b_c = b_d * (1.0f / d_b);

    float x = 1.86206786f * r_c - 1.01125463f * g_c + 0.14918677f * b_c;
    float y = 0.38752654f * r_c + 0.62144744f * g_c - 0.00897398f * b_c;
    float z = -0.01584150f * r_c - 0.03412294f * g_c + 1.04996444f * b_c;

    if (out_y) {
        *out_y = y;
    }

    *out_r = (3.24100326f * x - 1.53739899f * y - 0.49861587f * z) * 0.01f;
    *out_g = (-0.96922426f * x + 1.87592999f * y + 0.04155422f * z) * 0.01f;
    *out_b = (0.05563942f * x - 0.20401120f * y + 1.05714897f * z) * 0.01f;
}

static void solve_cam16_rgb_at_y(float cos_h, float sin_h, float p1_star, float c_cam16,
                                 float y_target, float j_init, float *out_r, float *out_g,
                                 float *out_b)
{
    float j_lo = 0.0f;
    float j_hi = 100.0f;
    float j_cur = clamp_f(j_init, 0.5f, 99.5f);
    float r = 0.0f, g = 0.0f, b = 0.0f;

    for (int i = 0; i < 5; ++i) {
        float y_eval = 0.0f;
        cam16_jch_to_linear_rgb_precomp(j_cur, c_cam16, cos_h, sin_h, p1_star, &r, &g, &b, &y_eval);
        if (y_eval < y_target) {
            j_lo = j_cur;
        } else {
            j_hi = j_cur;
        }
        if (fabsf(y_eval - y_target) < 0.02f) {
            break;
        }
        if (y_eval > 1e-4f) {
            float j_est = j_cur * sqrtf(y_target / y_eval);
            if (j_est > j_lo + 0.05f && j_est < j_hi - 0.05f) {
                j_cur = j_est;
                continue;
            }
        }
        j_cur = 0.5f * (j_lo + j_hi);
    }
    *out_r = r;
    *out_g = g;
    *out_b = b;
}

void waywal_rgba_to_hct(color_rgba_t rgba, float *out_h_deg, float *out_c_cam16, float *out_t_lstar)
{
    ensure_srgb_lut();
    float r_lin = g_srgb_to_linear[rgba.r];
    float g_lin = g_srgb_to_linear[rgba.g];
    float b_lin = g_srgb_to_linear[rgba.b];

    float y = 0.0f;
    waywal_cam16_t cam = linear_rgb_to_cam16(r_lin, g_lin, b_lin, &y);
    float y_norm = y * 0.01f;
    float lstar = (y_norm > 0.0088564516f) ? (116.0f * fast_cbrtf_scalar(y_norm) - 16.0f)
                                           : (903.296296f * y_norm);
    lstar = clamp_f(lstar, 0.0f, 100.0f);

    if (out_h_deg) {
        *out_h_deg = cam.h_deg;
    }
    if (out_c_cam16) {
        *out_c_cam16 = cam.c;
    }
    if (out_t_lstar) {
        *out_t_lstar = lstar;
    }
}

color_rgba_t waywal_hct_to_rgba(float h_deg, float c_cam16, float t_lstar)
{
    if (t_lstar <= 0.0f) {
        return (color_rgba_t){.r = 0, .g = 0, .b = 0, .a = 255};
    }
    if (t_lstar >= 100.0f) {
        return (color_rgba_t){.r = 255, .g = 255, .b = 255, .a = 255};
    }

    float f = (t_lstar + 16.0f) * (1.0f / 116.0f);
    float y_target = (t_lstar > 8.0f) ? (100.0f * f * f * f) : (t_lstar * (100.0f / 903.296296f));
    if (c_cam16 < 1.0f) {
        uint8_t v = linear_to_srgb_u8(y_target * 0.01f);
        return (color_rgba_t){.r = v, .g = v, .b = v, .a = 255};
    }
    float h_rad = wrap_radians(h_deg * DEG_TO_RAD_F);
    float cos_h = cosf(h_rad);
    float sin_h = sinf(h_rad);
    float e_t = 0.25f * (cosf(h_rad + 2.0f) + 3.8f);
    float p1_star = (50000.0f / 13.0f) * 1.016919f * e_t;
    float target_c = c_cam16;

    float r = 0.0f, g = 0.0f, b = 0.0f;
    solve_cam16_rgb_at_y(cos_h, sin_h, p1_star, target_c, y_target, t_lstar, &r, &g, &b);

    if (!linear_rgb_in_gamut(r, g, b)) {
        float c_lo = 0.0f;
        float c_hi = target_c;
        float best_r = y_target * 0.01f;
        float best_g = best_r;
        float best_b = best_r;

        for (int iter = 0; iter < 7; ++iter) {
            float c_mid = 0.5f * (c_lo + c_hi);
            float mr, mg, mb;
            solve_cam16_rgb_at_y(cos_h, sin_h, p1_star, c_mid, y_target, t_lstar, &mr, &mg, &mb);
            if (linear_rgb_in_gamut(mr, mg, mb)) {
                c_lo = c_mid;
                best_r = mr;
                best_g = mg;
                best_b = mb;
            } else {
                c_hi = c_mid;
            }
        }
        r = best_r;
        g = best_g;
        b = best_b;
    }

    color_rgba_t out;
    out.r = linear_to_srgb_u8(r);
    out.g = linear_to_srgb_u8(g);
    out.b = linear_to_srgb_u8(b);
    out.a = 255;
    return out;
}

color_rgba_t waywal_palette_generate_tone(float l_target, float chroma, float hue_rad)
{
    /* Convert OKLab-scaled chroma (0..0.4) to CAM16 chroma (0..120), or pass CAM16 chroma directly
     */
    float c_cam16 = (chroma <= 0.5f) ? (chroma * 320.0f) : chroma;
    float h_deg = wrap_radians(hue_rad) * RAD_TO_DEG_F;
    return waywal_hct_to_rgba(h_deg, c_cam16, l_target);
}

waywal_scheme_variant_t waywal_scheme_variant_from_str(const char *str)
{
    if (!str || str[0] == '\0') {
        return WAYWAL_SCHEME_TONAL_SPOT;
    }
    if (strcasecmp(str, "tonal-spot") == 0 || strcasecmp(str, "tonal_spot") == 0 ||
        strcasecmp(str, "tonalspot") == 0) {
        return WAYWAL_SCHEME_TONAL_SPOT;
    }
    if (strcasecmp(str, "vibrant") == 0) {
        return WAYWAL_SCHEME_VIBRANT;
    }
    if (strcasecmp(str, "expressive") == 0) {
        return WAYWAL_SCHEME_EXPRESSIVE;
    }
    if (strcasecmp(str, "content") == 0) {
        return WAYWAL_SCHEME_CONTENT;
    }
    if (strcasecmp(str, "fidelity") == 0) {
        return WAYWAL_SCHEME_FIDELITY;
    }
    if (strcasecmp(str, "neutral") == 0) {
        return WAYWAL_SCHEME_NEUTRAL;
    }
    if (strcasecmp(str, "monochrome") == 0) {
        return WAYWAL_SCHEME_MONOCHROME;
    }
    if (strcasecmp(str, "rainbow") == 0) {
        return WAYWAL_SCHEME_RAINBOW;
    }
    if (strcasecmp(str, "fruit-salad") == 0 || strcasecmp(str, "fruit_salad") == 0 ||
        strcasecmp(str, "fruitsalad") == 0) {
        return WAYWAL_SCHEME_FRUIT_SALAD;
    }
    return WAYWAL_SCHEME_TONAL_SPOT;
}

static const char *scheme_variant_to_caelestia_str(waywal_scheme_variant_t v)
{
    switch (v) {
    case WAYWAL_SCHEME_VIBRANT:
        return "vibrant";
    case WAYWAL_SCHEME_EXPRESSIVE:
        return "expressive";
    case WAYWAL_SCHEME_CONTENT:
        return "content";
    case WAYWAL_SCHEME_FIDELITY:
        return "fidelity";
    case WAYWAL_SCHEME_NEUTRAL:
        return "neutral";
    case WAYWAL_SCHEME_MONOCHROME:
        return "monochrome";
    case WAYWAL_SCHEME_RAINBOW:
        return "rainbow";
    case WAYWAL_SCHEME_FRUIT_SALAD:
        return "fruitsalad";
    case WAYWAL_SCHEME_TONAL_SPOT:
    default:
        return "tonalspot";
    }
}

static void apply_scheme_variant_seeds(waywal_scheme_variant_t variant, float src_c_ok,
                                       waywal_oklch_t *p_seed, waywal_oklch_t *s_seed,
                                       waywal_oklch_t *t_seed, waywal_oklch_t *n_seed,
                                       waywal_oklch_t *nv_seed, bool *is_mono)
{
    float p_h = p_seed->h_rad;
    switch (variant) {
    case WAYWAL_SCHEME_TONAL_SPOT:
        break;
    case WAYWAL_SCHEME_VIBRANT:
        *is_mono = false;
        p_seed->c = 0.25f;
        s_seed->c = 0.08f;
        s_seed->h_rad = wrap_radians(p_h + 18.0f * DEG_TO_RAD_F);
        t_seed->c = 0.11f;
        t_seed->h_rad = wrap_radians(p_h + 35.0f * DEG_TO_RAD_F);
        n_seed->c = 0.032f;
        nv_seed->c = 0.042f;
        break;
    case WAYWAL_SCHEME_EXPRESSIVE:
        *is_mono = false;
        p_seed->h_rad = wrap_radians(p_h + 240.0f * DEG_TO_RAD_F);
        p_seed->c = 0.13f;
        s_seed->h_rad = wrap_radians(p_h + 45.0f * DEG_TO_RAD_F);
        s_seed->c = 0.08f;
        t_seed->h_rad = wrap_radians(p_h + 120.0f * DEG_TO_RAD_F);
        t_seed->c = 0.11f;
        n_seed->h_rad = wrap_radians(p_h + 15.0f * DEG_TO_RAD_F);
        n_seed->c = 0.025f;
        nv_seed->h_rad = wrap_radians(p_h + 15.0f * DEG_TO_RAD_F);
        nv_seed->c = 0.04f;
        break;
    case WAYWAL_SCHEME_CONTENT:
        p_seed->c = clamp_f(src_c_ok, 0.04f, 0.26f);
        s_seed->h_rad = p_h;
        s_seed->c = clamp_f(src_c_ok * 0.33f, 0.02f, 0.10f);
        t_seed->h_rad = p_h;
        t_seed->c = clamp_f(src_c_ok * 0.66f, 0.03f, 0.16f);
        n_seed->c = clamp_f(src_c_ok * 0.125f, 0.008f, 0.03f);
        nv_seed->c = clamp_f(src_c_ok * 0.25f, 0.014f, 0.05f);
        break;
    case WAYWAL_SCHEME_FIDELITY:
        p_seed->c = clamp_f(src_c_ok, 0.05f, 0.28f);
        s_seed->h_rad = p_h;
        s_seed->c = clamp_f(fmaxf(src_c_ok - 0.10f, src_c_ok * 0.5f), 0.03f, 0.14f);
        t_seed->h_rad = wrap_radians(p_h + 180.0f * DEG_TO_RAD_F);
        t_seed->c = clamp_f(src_c_ok * 0.8f, 0.05f, 0.18f);
        n_seed->c = clamp_f(src_c_ok * 0.125f, 0.01f, 0.035f);
        nv_seed->c = clamp_f(src_c_ok * 0.25f, 0.018f, 0.055f);
        break;
    case WAYWAL_SCHEME_NEUTRAL:
        p_seed->c = 0.038f;
        s_seed->h_rad = p_h;
        s_seed->c = 0.025f;
        t_seed->h_rad = p_h;
        t_seed->c = 0.05f;
        n_seed->c = 0.006f;
        nv_seed->c = 0.006f;
        break;
    case WAYWAL_SCHEME_MONOCHROME:
        *is_mono = true;
        p_seed->c = 0.0f;
        s_seed->c = 0.0f;
        t_seed->c = 0.0f;
        n_seed->c = 0.0f;
        nv_seed->c = 0.0f;
        break;
    case WAYWAL_SCHEME_RAINBOW:
        *is_mono = false;
        p_seed->c = 0.15f;
        s_seed->h_rad = p_h;
        s_seed->c = 0.05f;
        t_seed->h_rad = wrap_radians(p_h + 60.0f * DEG_TO_RAD_F);
        t_seed->c = 0.08f;
        n_seed->c = 0.0f;
        nv_seed->c = 0.0f;
        break;
    case WAYWAL_SCHEME_FRUIT_SALAD:
        *is_mono = false;
        p_seed->h_rad = wrap_radians(p_h - 50.0f * DEG_TO_RAD_F);
        p_seed->c = 0.15f;
        s_seed->h_rad = wrap_radians(p_h - 50.0f * DEG_TO_RAD_F);
        s_seed->c = 0.11f;
        t_seed->h_rad = p_h;
        t_seed->c = 0.11f;
        n_seed->h_rad = p_h;
        n_seed->c = 0.03f;
        nv_seed->h_rad = p_h;
        nv_seed->c = 0.05f;
        break;
    }
}

static color_rgba_t harmonize_semantic_color(float base_hue_deg, float base_chroma, float tone,
                                             float primary_hue_rad, float primary_chroma)
{
    float primary_deg = wrap_radians(primary_hue_rad) * RAD_TO_DEG_F;
    float diff = primary_deg - base_hue_deg;
    while (diff > 180.0f)
        diff -= 360.0f;
    while (diff < -180.0f)
        diff += 360.0f;

    float max_rot = 22.0f;
    float shift = clamp_f(diff * 0.35f, -max_rot, max_rot);
    float h_rad = wrap_radians((base_hue_deg + shift) * DEG_TO_RAD_F);
    float c = clamp_f(0.75f * base_chroma + 0.25f * primary_chroma, 0.04f, 0.22f);
    return waywal_palette_generate_tone(tone, c, h_rad);
}

static void synthesize_m3_scheme(waywal_palette_t *pal, waywal_theme_mode_t mode,
                                 waywal_scheme_variant_t variant, float contrast, bool is_mono,
                                 waywal_oklch_t p_seed, waywal_oklch_t s_seed,
                                 waywal_oklch_t t_seed, waywal_oklch_t n_seed,
                                 waywal_oklch_t nv_seed)
{
    memset(pal, 0, sizeof(*pal));
    pal->mode = mode;
    pal->variant = variant;
    pal->contrast = clamp_f(contrast, -1.0f, 1.0f);
    pal->is_monochrome = is_mono;
    pal->primary_seed = p_seed;
    pal->secondary_seed = s_seed;
    pal->tertiary_seed = t_seed;
    pal->neutral_seed = n_seed;
    pal->neutral_variant_seed = nv_seed;

    float c_shift = pal->contrast * 12.0f;

    pal->primary_paletteKeyColor = waywal_palette_generate_tone(40.0f, p_seed.c, p_seed.h_rad);
    pal->secondary_paletteKeyColor = waywal_palette_generate_tone(40.0f, s_seed.c, s_seed.h_rad);
    pal->tertiary_paletteKeyColor = waywal_palette_generate_tone(40.0f, t_seed.c, t_seed.h_rad);
    pal->neutral_paletteKeyColor = waywal_palette_generate_tone(40.0f, n_seed.c, n_seed.h_rad);
    pal->neutral_variant_paletteKeyColor =
        waywal_palette_generate_tone(40.0f, nv_seed.c, nv_seed.h_rad);

    const float err_h = 25.0f * DEG_TO_RAD_F;
    const float err_c = 0.18f;

    pal->shadow = (color_rgba_t){.r = 0, .g = 0, .b = 0, .a = 255};
    pal->scrim = (color_rgba_t){.r = 0, .g = 0, .b = 0, .a = 255};

    pal->primaryFixed = waywal_palette_generate_tone(90.0f, p_seed.c, p_seed.h_rad);
    pal->primaryFixedDim = waywal_palette_generate_tone(80.0f, p_seed.c, p_seed.h_rad);
    pal->onPrimaryFixed = waywal_palette_generate_tone(10.0f, p_seed.c, p_seed.h_rad);
    pal->onPrimaryFixedVariant = waywal_palette_generate_tone(30.0f, p_seed.c, p_seed.h_rad);

    pal->secondaryFixed = waywal_palette_generate_tone(90.0f, s_seed.c, s_seed.h_rad);
    pal->secondaryFixedDim = waywal_palette_generate_tone(80.0f, s_seed.c, s_seed.h_rad);
    pal->onSecondaryFixed = waywal_palette_generate_tone(10.0f, s_seed.c, s_seed.h_rad);
    pal->onSecondaryFixedVariant = waywal_palette_generate_tone(30.0f, s_seed.c, s_seed.h_rad);

    pal->tertiaryFixed = waywal_palette_generate_tone(90.0f, t_seed.c, t_seed.h_rad);
    pal->tertiaryFixedDim = waywal_palette_generate_tone(80.0f, t_seed.c, t_seed.h_rad);
    pal->onTertiaryFixed = waywal_palette_generate_tone(10.0f, t_seed.c, t_seed.h_rad);
    pal->onTertiaryFixedVariant = waywal_palette_generate_tone(30.0f, t_seed.c, t_seed.h_rad);

    if (mode == WAYWAL_THEME_DARK) {
        pal->background = waywal_palette_generate_tone(clamp_f(6.0f - c_shift * 0.4f, 0.0f, 18.0f),
                                                       n_seed.c, n_seed.h_rad);
        pal->onBackground = waywal_palette_generate_tone(
            clamp_f(90.0f + c_shift * 0.8f, 75.0f, 100.0f), n_seed.c, n_seed.h_rad);
        pal->surface = pal->background;
        pal->surfaceDim = pal->background;
        pal->surfaceBright = waywal_palette_generate_tone(24.0f, n_seed.c, n_seed.h_rad);
        pal->surfaceContainerLowest = waywal_palette_generate_tone(4.0f, n_seed.c, n_seed.h_rad);
        pal->surfaceContainerLow = waywal_palette_generate_tone(10.0f, n_seed.c, n_seed.h_rad);
        pal->surfaceContainer = waywal_palette_generate_tone(12.0f, n_seed.c, n_seed.h_rad);
        pal->surfaceContainerHigh = waywal_palette_generate_tone(17.0f, n_seed.c, n_seed.h_rad);
        pal->surfaceContainerHighest = waywal_palette_generate_tone(22.0f, n_seed.c, n_seed.h_rad);
        pal->onSurface = pal->onBackground;
        pal->surfaceVariant = waywal_palette_generate_tone(30.0f, nv_seed.c, nv_seed.h_rad);
        pal->onSurfaceVariant = waywal_palette_generate_tone(80.0f, nv_seed.c, nv_seed.h_rad);
        pal->inverseSurface = pal->onBackground;
        pal->inverseOnSurface = waywal_palette_generate_tone(20.0f, n_seed.c, n_seed.h_rad);
        pal->outline = waywal_palette_generate_tone(60.0f, nv_seed.c, nv_seed.h_rad);
        pal->outlineVariant = pal->surfaceVariant;

        pal->primary = waywal_palette_generate_tone(clamp_f(80.0f + c_shift, 65.0f, 95.0f),
                                                    p_seed.c, p_seed.h_rad);
        pal->surfaceTint = pal->primary;
        pal->onPrimary = waywal_palette_generate_tone(clamp_f(20.0f - c_shift, 5.0f, 35.0f),
                                                      p_seed.c, p_seed.h_rad);
        pal->primaryContainer = waywal_palette_generate_tone(30.0f, p_seed.c, p_seed.h_rad);
        pal->onPrimaryContainer = pal->primaryFixed;
        pal->inversePrimary = pal->primary_paletteKeyColor;

        pal->secondary = pal->secondaryFixedDim;
        pal->onSecondary = waywal_palette_generate_tone(20.0f, s_seed.c, s_seed.h_rad);
        pal->secondaryContainer = waywal_palette_generate_tone(30.0f, s_seed.c, s_seed.h_rad);
        pal->onSecondaryContainer = pal->secondaryFixed;

        pal->tertiary = pal->tertiaryFixedDim;
        pal->onTertiary = waywal_palette_generate_tone(20.0f, t_seed.c, t_seed.h_rad);
        pal->tertiaryContainer = waywal_palette_generate_tone(30.0f, t_seed.c, t_seed.h_rad);
        pal->onTertiaryContainer = pal->tertiaryFixed;

        pal->error = waywal_palette_generate_tone(80.0f, err_c, err_h);
        pal->onError = waywal_palette_generate_tone(20.0f, err_c, err_h);
        pal->errorContainer = waywal_palette_generate_tone(30.0f, err_c, err_h);
        pal->onErrorContainer = waywal_palette_generate_tone(90.0f, err_c, err_h);

        pal->text = pal->onSurface;
        pal->subtext1 = pal->onSurfaceVariant;
        pal->subtext0 = pal->outline;
        pal->overlay2 = waywal_palette_generate_tone(52.0f, nv_seed.c, nv_seed.h_rad);
        pal->overlay1 = waywal_palette_generate_tone(45.0f, nv_seed.c, nv_seed.h_rad);
        pal->overlay0 = waywal_palette_generate_tone(38.0f, nv_seed.c, nv_seed.h_rad);
        pal->surface2 = waywal_palette_generate_tone(30.0f, nv_seed.c, nv_seed.h_rad);
        pal->surface1 = waywal_palette_generate_tone(22.0f, n_seed.c, n_seed.h_rad);
        pal->surface0 = waywal_palette_generate_tone(15.0f, n_seed.c, n_seed.h_rad);
        pal->base = pal->surface;
        pal->mantle = waywal_palette_generate_tone(5.0f, n_seed.c, n_seed.h_rad);
        pal->crust = pal->surfaceContainerLowest;
    } else {
        pal->background = waywal_palette_generate_tone(98.0f, n_seed.c, n_seed.h_rad);
        pal->onBackground = waywal_palette_generate_tone(
            clamp_f(10.0f - c_shift * 0.8f, 0.0f, 25.0f), n_seed.c, n_seed.h_rad);
        pal->surface = pal->background;
        pal->surfaceDim = waywal_palette_generate_tone(87.0f, n_seed.c, n_seed.h_rad);
        pal->surfaceBright = pal->background;
        pal->surfaceContainerLowest = waywal_palette_generate_tone(100.0f, n_seed.c, n_seed.h_rad);
        pal->surfaceContainerLow = waywal_palette_generate_tone(96.0f, n_seed.c, n_seed.h_rad);
        pal->surfaceContainer = waywal_palette_generate_tone(94.0f, n_seed.c, n_seed.h_rad);
        pal->surfaceContainerHigh = waywal_palette_generate_tone(92.0f, n_seed.c, n_seed.h_rad);
        pal->surfaceContainerHighest = waywal_palette_generate_tone(90.0f, n_seed.c, n_seed.h_rad);
        pal->onSurface = pal->onBackground;
        pal->surfaceVariant = waywal_palette_generate_tone(90.0f, nv_seed.c, nv_seed.h_rad);
        pal->onSurfaceVariant = waywal_palette_generate_tone(30.0f, nv_seed.c, nv_seed.h_rad);
        pal->inverseSurface = waywal_palette_generate_tone(20.0f, n_seed.c, n_seed.h_rad);
        pal->inverseOnSurface = waywal_palette_generate_tone(95.0f, n_seed.c, n_seed.h_rad);
        pal->outline = waywal_palette_generate_tone(50.0f, nv_seed.c, nv_seed.h_rad);
        pal->outlineVariant = waywal_palette_generate_tone(80.0f, nv_seed.c, nv_seed.h_rad);

        pal->primary = waywal_palette_generate_tone(clamp_f(40.0f - c_shift, 15.0f, 55.0f),
                                                    p_seed.c, p_seed.h_rad);
        pal->surfaceTint = pal->primary;
        pal->onPrimary = waywal_palette_generate_tone(100.0f, p_seed.c, p_seed.h_rad);
        pal->primaryContainer = pal->primaryFixed;
        pal->onPrimaryContainer = pal->onPrimaryFixed;
        pal->inversePrimary = pal->primaryFixedDim;

        pal->secondary = pal->secondary_paletteKeyColor;
        pal->onSecondary = waywal_palette_generate_tone(100.0f, s_seed.c, s_seed.h_rad);
        pal->secondaryContainer = pal->secondaryFixed;
        pal->onSecondaryContainer = pal->onSecondaryFixed;

        pal->tertiary = pal->tertiary_paletteKeyColor;
        pal->onTertiary = waywal_palette_generate_tone(100.0f, t_seed.c, t_seed.h_rad);
        pal->tertiaryContainer = pal->tertiaryFixed;
        pal->onTertiaryContainer = pal->onTertiaryFixed;

        pal->error = waywal_palette_generate_tone(40.0f, err_c, err_h);
        pal->onError = waywal_palette_generate_tone(100.0f, err_c, err_h);
        pal->errorContainer = waywal_palette_generate_tone(90.0f, err_c, err_h);
        pal->onErrorContainer = waywal_palette_generate_tone(10.0f, err_c, err_h);

        pal->text = pal->onSurface;
        pal->subtext1 = pal->onSurfaceVariant;
        pal->subtext0 = pal->outline;
        pal->overlay2 = waywal_palette_generate_tone(48.0f, nv_seed.c, nv_seed.h_rad);
        pal->overlay1 = waywal_palette_generate_tone(56.0f, nv_seed.c, nv_seed.h_rad);
        pal->overlay0 = waywal_palette_generate_tone(64.0f, nv_seed.c, nv_seed.h_rad);
        pal->surface2 = waywal_palette_generate_tone(72.0f, nv_seed.c, nv_seed.h_rad);
        pal->surface1 = waywal_palette_generate_tone(80.0f, n_seed.c, n_seed.h_rad);
        pal->surface0 = waywal_palette_generate_tone(88.0f, n_seed.c, n_seed.h_rad);
        pal->base = pal->surface;
        pal->mantle = pal->surfaceContainerLow;
        pal->crust = pal->surfaceContainer;
    }

    float ansi_hues[6] = {25.0f, 145.0f, 85.0f, 255.0f, 320.0f, 200.0f};
    float base_tone = (mode == WAYWAL_THEME_DARK) ? 74.0f : 42.0f;
    float bright_tone = (mode == WAYWAL_THEME_DARK) ? 84.0f : 32.0f;

    pal->term[0] = waywal_palette_generate_tone((mode == WAYWAL_THEME_DARK) ? 22.0f : 18.0f,
                                                n_seed.c, n_seed.h_rad);
    pal->term[7] = waywal_palette_generate_tone((mode == WAYWAL_THEME_DARK) ? 86.0f : 82.0f,
                                                nv_seed.c, nv_seed.h_rad);
    pal->term[8] = waywal_palette_generate_tone((mode == WAYWAL_THEME_DARK) ? 64.0f : 45.0f,
                                                nv_seed.c, nv_seed.h_rad);
    pal->term[15] = (color_rgba_t){.r = 255, .g = 255, .b = 255, .a = 255};

    for (int i = 0; i < 6; ++i) {
        pal->term[1 + i] =
            harmonize_semantic_color(ansi_hues[i], 0.14f, base_tone, p_seed.h_rad, p_seed.c);
        pal->term[9 + i] =
            harmonize_semantic_color(ansi_hues[i], 0.16f, bright_tone, p_seed.h_rad, p_seed.c);
    }

    float cat_tone = (mode == WAYWAL_THEME_DARK) ? 82.0f : 44.0f;
    pal->rosewater =
        harmonize_semantic_color(15.0f, 0.06f, cat_tone + 8.0f, p_seed.h_rad, p_seed.c);
    pal->flamingo = harmonize_semantic_color(12.0f, 0.09f, cat_tone + 5.0f, p_seed.h_rad, p_seed.c);
    pal->pink = harmonize_semantic_color(335.0f, 0.12f, cat_tone + 3.0f, p_seed.h_rad, p_seed.c);
    pal->mauve = harmonize_semantic_color(295.0f, 0.15f, cat_tone, p_seed.h_rad, p_seed.c);
    pal->red = harmonize_semantic_color(20.0f, 0.16f, cat_tone - 2.0f, p_seed.h_rad, p_seed.c);
    pal->maroon = harmonize_semantic_color(15.0f, 0.13f, cat_tone, p_seed.h_rad, p_seed.c);
    pal->peach = harmonize_semantic_color(48.0f, 0.15f, cat_tone + 2.0f, p_seed.h_rad, p_seed.c);
    pal->yellow = harmonize_semantic_color(85.0f, 0.13f, cat_tone + 6.0f, p_seed.h_rad, p_seed.c);
    pal->green = harmonize_semantic_color(142.0f, 0.14f, cat_tone + 2.0f, p_seed.h_rad, p_seed.c);
    pal->teal = harmonize_semantic_color(182.0f, 0.12f, cat_tone + 2.0f, p_seed.h_rad, p_seed.c);
    pal->sky = harmonize_semantic_color(210.0f, 0.13f, cat_tone + 3.0f, p_seed.h_rad, p_seed.c);
    pal->sapphire = harmonize_semantic_color(225.0f, 0.14f, cat_tone, p_seed.h_rad, p_seed.c);
    pal->blue = harmonize_semantic_color(255.0f, 0.15f, cat_tone, p_seed.h_rad, p_seed.c);
    pal->lavender =
        harmonize_semantic_color(275.0f, 0.11f, cat_tone + 3.0f, p_seed.h_rad, p_seed.c);

    pal->klink = harmonize_semantic_color(250.0f, 0.14f, 62.0f, p_seed.h_rad, p_seed.c);
    pal->klinkSelection = pal->klink;
    pal->kvisited = harmonize_semantic_color(285.0f, 0.14f, 60.0f, p_seed.h_rad, p_seed.c);
    pal->kvisitedSelection = pal->kvisited;
    pal->knegative = harmonize_semantic_color(22.0f, 0.16f, 64.0f, p_seed.h_rad, p_seed.c);
    pal->knegativeSelection = pal->knegative;
    pal->kneutral = harmonize_semantic_color(75.0f, 0.14f, 68.0f, p_seed.h_rad, p_seed.c);
    pal->kneutralSelection = pal->kneutral;
    pal->kpositive = harmonize_semantic_color(148.0f, 0.14f, 66.0f, p_seed.h_rad, p_seed.c);
    pal->kpositiveSelection = pal->kpositive;

    float succ_h = wrap_radians(146.0f * DEG_TO_RAD_F);
    float succ_c = 0.08f;
    if (mode == WAYWAL_THEME_DARK) {
        pal->success = waywal_palette_generate_tone(80.0f, succ_c, succ_h);
        pal->onSuccess = waywal_palette_generate_tone(20.0f, succ_c, succ_h);
        pal->successContainer = waywal_palette_generate_tone(30.0f, succ_c, succ_h);
        pal->onSuccessContainer = waywal_palette_generate_tone(90.0f, succ_c, succ_h);
    } else {
        pal->success = waywal_palette_generate_tone(40.0f, succ_c, succ_h);
        pal->onSuccess = waywal_palette_generate_tone(100.0f, succ_c, succ_h);
        pal->successContainer = waywal_palette_generate_tone(90.0f, succ_c, succ_h);
        pal->onSuccessContainer = waywal_palette_generate_tone(10.0f, succ_c, succ_h);
    }

    pal->is_light_mode = (mode == WAYWAL_THEME_LIGHT);
    pal->source_color = ((uint32_t)pal->primary_paletteKeyColor.r << 16) |
                        ((uint32_t)pal->primary_paletteKeyColor.g << 8) |
                        (uint32_t)pal->primary_paletteKeyColor.b;
}

static uint32_t oklch_to_rgb24(float l, float c_ok, float h_rad)
{
    float a = c_ok * cosf(h_rad);
    float b = c_ok * sinf(h_rad);
    float r_lin = 0.0f, g_lin = 0.0f, b_lin = 0.0f;
    oklab_to_linear_rgb(l, a, b, &r_lin, &g_lin, &b_lin);
    uint8_t r = linear_to_srgb_u8(r_lin);
    uint8_t g = linear_to_srgb_u8(g_lin);
    uint8_t bl = linear_to_srgb_u8(b_lin);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

static waywal_oklch_t rgba_to_oklch_seed(color_rgba_t col, float fallback_h_rad)
{
    ensure_srgb_lut();
    waywal_oklab_t ok =
        linear_rgb_to_oklab(g_srgb_to_linear[col.r], g_srgb_to_linear[col.g], g_srgb_to_linear[col.b]);
    float c = sqrtf(ok.a * ok.a + ok.b * ok.b);
    float h = (c > 1e-4f) ? wrap_radians(atan2f(ok.b, ok.a)) : fallback_h_rad;
    return (waywal_oklch_t){.l = clamp_f(ok.l, 0.2f, 0.85f), .c = c, .h_rad = h};
}

void waywal_palette_derive_opposite(const waywal_palette_t *src, waywal_palette_t *out_opp)
{
    if (!src || !out_opp) {
        return;
    }

    waywal_palette_t src_copy = *src;
    waywal_theme_mode_t opp_mode =
        (src_copy.mode == WAYWAL_THEME_DARK) ? WAYWAL_THEME_LIGHT : WAYWAL_THEME_DARK;

    waywal_oklch_t p_seed = src_copy.primary_seed;
    waywal_oklch_t s_seed = src_copy.secondary_seed;
    waywal_oklch_t t_seed = src_copy.tertiary_seed;
    waywal_oklch_t n_seed = src_copy.neutral_seed;
    waywal_oklch_t nv_seed = src_copy.neutral_variant_seed;
    bool is_mono = src_copy.is_monochrome;

    if (p_seed.l == 0.0f && p_seed.c == 0.0f && n_seed.l == 0.0f) {
        p_seed = rgba_to_oklch_seed(src_copy.primary, 250.0f * DEG_TO_RAD_F);
        float src_c_ok = p_seed.c;
        if (p_seed.c < 0.04f)
            p_seed.c = 0.12f;
        s_seed = rgba_to_oklch_seed(src_copy.secondary,
                                    wrap_radians(p_seed.h_rad + 15.0f * DEG_TO_RAD_F));
        if (s_seed.c < 0.03f)
            s_seed.c = 0.05f;
        t_seed =
            rgba_to_oklch_seed(src_copy.tertiary, wrap_radians(p_seed.h_rad + 60.0f * DEG_TO_RAD_F));
        if (t_seed.c < 0.04f)
            t_seed.c = 0.08f;
        n_seed = (waywal_oklch_t){.l = 0.5f, .c = 0.018f, .h_rad = p_seed.h_rad};
        nv_seed = (waywal_oklch_t){.l = 0.5f, .c = 0.036f, .h_rad = p_seed.h_rad};
        apply_scheme_variant_seeds(src_copy.variant, src_c_ok, &p_seed, &s_seed, &t_seed, &n_seed,
                                   &nv_seed, &is_mono);
    }

    synthesize_m3_scheme(out_opp, opp_mode, src_copy.variant, src_copy.contrast, is_mono, p_seed,
                         s_seed, t_seed, n_seed, nv_seed);

    if (src_copy.seed_count > 0U || src_copy.source_color != 0U) {
        out_opp->source_color = src_copy.source_color;
    }
    memcpy(out_opp->seeds, src_copy.seeds, sizeof(out_opp->seeds));
    out_opp->seed_count = src_copy.seed_count;
    out_opp->source_color_index = src_copy.source_color_index;
    out_opp->is_light_mode = (opp_mode == WAYWAL_THEME_LIGHT);
    memcpy(out_opp->wallpaper_path, src_copy.wallpaper_path, sizeof(out_opp->wallpaper_path));
}

static bool extract_dual_impl(const uint8_t *pixels, uint32_t width, uint32_t height,
                              uint32_t stride_bytes, bool is_bgra, waywal_scheme_variant_t variant,
                              float contrast, uint8_t source_color_index, bool has_seed_override,
                              uint32_t seed_override_rgb, waywal_palette_t *out_dark,
                              waywal_palette_t *out_light)
{
    if ((!out_dark && !out_light) || (!has_seed_override && (!pixels || width == 0U || height == 0U))) {
        return false;
    }
    if (pixels && width > 0U && stride_bytes < width * 4U) {
        stride_bytes = width * 4U;
    }

    ensure_srgb_lut();

    size_t n_clusters = 0;
    weighted_point_t clusters[KMEANS_K];
    if (pixels && width > 0U && height > 0U) {
        wu_hist_t *hist = (wu_hist_t *)aligned_alloc(32, sizeof(wu_hist_t));
        if (!hist) {
            return false;
        }
        size_t valid_samples =
            wu_populate_histogram(hist, pixels, width, height, stride_bytes, is_bgra);
        if (valid_samples > 0) {
            wu_compute_moments(hist);
            weighted_point_t wu_boxes[WU_MAX_BOXES];
            size_t n_boxes = wu_partition(hist, wu_boxes, WU_MAX_BOXES);
            free(hist);
            n_clusters = weighted_kmeans_pp(wu_boxes, n_boxes, clusters, KMEANS_K);
        } else {
            free(hist);
        }
    }

    float total_weight = 0.0f;
    for (size_t i = 0; i < n_clusters; ++i) {
        total_weight += clusters[i].weight;
    }
    if (total_weight <= 0.0f) {
        total_weight = 1.0f;
    }

    typedef struct {
        float l;
        float c_ok;
        float h_rad;
        float h_deg;
        float cam_c;
        float cam_j;
        float weight;
        float score;
    } candidate_t;

    candidate_t cands[KMEANS_K];
    float max_c_ok = 0.0f;

    for (size_t i = 0; i < n_clusters; ++i) {
        float l = clusters[i].l;
        float a = clusters[i].a;
        float b = clusters[i].b;
        float c_ok = sqrtf(a * a + b * b);
        float h_rad = wrap_radians(atan2f(b, a));
        float h_deg = h_rad * RAD_TO_DEG_F;
        waywal_cam16_t cam = oklab_to_cam16(l, a, b);

        if (c_ok > max_c_ok) {
            max_c_ok = c_ok;
        }

        float prop = clusters[i].weight / total_weight;
        float pop_factor = powf(prop, 0.35f);

        float chroma_factor = (c_ok * 5.5f) + (cam.c * 0.015f);
        if (c_ok < 0.025f) {
            chroma_factor *= 0.15f;
        }

        float light_penalty = 1.0f;
        if (l < 0.15f) {
            light_penalty = l / 0.15f * 0.25f;
        } else if (l > 0.92f) {
            light_penalty = (1.0f - l) / 0.08f * 0.3f;
        }
        light_penalty = clamp_f(light_penalty, 0.02f, 1.0f);

        /* Dampen desaturated skin-tone / muddy brown clusters so vibrant accents win. */
        float skin_penalty = 1.0f;
        if (h_deg >= 25.0f && h_deg <= 58.0f && c_ok > 0.02f && c_ok < 0.09f && l > 0.35f &&
            l < 0.80f) {
            skin_penalty = 0.55f;
        }

        cands[i].l = l;
        cands[i].c_ok = c_ok;
        cands[i].h_rad = h_rad;
        cands[i].h_deg = h_deg;
        cands[i].cam_c = cam.c;
        cands[i].cam_j = cam.j;
        cands[i].weight = clusters[i].weight;
        cands[i].score = pop_factor * (0.2f + chroma_factor) * light_penalty * skin_penalty;
    }

    for (size_t i = 0; i < n_clusters; ++i) {
        for (size_t j = i + 1; j < n_clusters; ++j) {
            if (cands[j].score > cands[i].score) {
                candidate_t tmp = cands[i];
                cands[i] = cands[j];
                cands[j] = tmp;
            }
        }
    }

    uint32_t extracted_seeds[4] = {0};
    uint8_t extracted_seed_count = (uint8_t)(n_clusters < 4U ? n_clusters : 4U);
    for (uint8_t k = 0; k < extracted_seed_count; ++k) {
        extracted_seeds[k] = oklch_to_rgb24(cands[k].l, cands[k].c_ok, cands[k].h_rad);
    }

    uint8_t clamped_idx = (source_color_index <= 3U) ? source_color_index : 0U;
    uint8_t effective_idx = 0U;
    uint32_t chosen_seed_rgb = (extracted_seed_count > 0) ? extracted_seeds[0] : 0U;

    if (!has_seed_override) {
        effective_idx =
            (n_clusters > 0U)
                ? (uint8_t)((clamped_idx < n_clusters) ? clamped_idx : (n_clusters - 1U))
                : 0U;
        if (effective_idx > 0U && n_clusters > 0U) {
            size_t sel = (size_t)effective_idx;
            candidate_t tmp = cands[0];
            cands[0] = cands[sel];
            cands[sel] = tmp;
            if (sel < 4U) {
                chosen_seed_rgb = extracted_seeds[sel];
            }
        }
    }

    bool is_mono = (max_c_ok < 0.018f);
    waywal_oklch_t p_seed, s_seed, t_seed, n_seed, nv_seed;
    float src_c_ok = (n_clusters > 0) ? cands[0].c_ok : 0.0f;

    if (has_seed_override) {
        uint8_t sr = (uint8_t)((seed_override_rgb >> 16) & 0xFFU);
        uint8_t sg = (uint8_t)((seed_override_rgb >> 8) & 0xFFU);
        uint8_t sb = (uint8_t)(seed_override_rgb & 0xFFU);
        waywal_oklab_t ok =
            linear_rgb_to_oklab(g_srgb_to_linear[sr], g_srgb_to_linear[sg], g_srgb_to_linear[sb]);
        float c_ok = sqrtf(ok.a * ok.a + ok.b * ok.b);
        float h_rad = (c_ok > 1e-4f) ? wrap_radians(atan2f(ok.b, ok.a)) : (250.0f * DEG_TO_RAD_F);
        src_c_ok = c_ok;
        is_mono = (c_ok < 0.018f);
        if (is_mono) {
            p_seed = (waywal_oklch_t){.l = 0.65f, .c = 0.02f, .h_rad = h_rad};
            s_seed = (waywal_oklch_t){
                .l = 0.65f, .c = 0.015f, .h_rad = wrap_radians(h_rad + 60.0f * DEG_TO_RAD_F)};
            t_seed = (waywal_oklch_t){
                .l = 0.65f, .c = 0.02f, .h_rad = wrap_radians(h_rad + 120.0f * DEG_TO_RAD_F)};
            n_seed = (waywal_oklch_t){.l = 0.65f, .c = 0.008f, .h_rad = h_rad};
            nv_seed = (waywal_oklch_t){.l = 0.65f, .c = 0.014f, .h_rad = h_rad};
        } else {
            float p_c = clamp_f(c_ok, 0.09f, 0.20f);
            p_seed = (waywal_oklch_t){.l = clamp_f(ok.l, 0.35f, 0.75f), .c = p_c, .h_rad = h_rad};
            s_seed = (waywal_oklch_t){
                .l = p_seed.l,
                .c = clamp_f(p_c * 0.38f, 0.035f, 0.065f),
                .h_rad = wrap_radians(h_rad + 15.0f * DEG_TO_RAD_F),
            };
            t_seed = (waywal_oklch_t){
                .l = p_seed.l,
                .c = clamp_f(p_c * 0.65f, 0.06f, 0.11f),
                .h_rad = wrap_radians(h_rad + 60.0f * DEG_TO_RAD_F),
            };
            n_seed = (waywal_oklch_t){.l = 0.5f, .c = 0.018f, .h_rad = h_rad};
            nv_seed = (waywal_oklch_t){.l = 0.5f, .c = 0.036f, .h_rad = h_rad};
        }
        chosen_seed_rgb = seed_override_rgb & 0xFFFFFFU;
        extracted_seeds[0] = chosen_seed_rgb;
        if (extracted_seed_count == 0U) {
            extracted_seed_count = 1U;
        }
        effective_idx = 0U;
    } else if (is_mono || n_clusters == 0) {
        float h =
            (n_clusters > 0 && cands[0].c_ok > 1e-4f) ? cands[0].h_rad : (250.0f * DEG_TO_RAD_F);
        p_seed = (waywal_oklch_t){.l = 0.65f, .c = 0.02f, .h_rad = h};
        s_seed = (waywal_oklch_t){
            .l = 0.65f, .c = 0.015f, .h_rad = wrap_radians(h + 60.0f * DEG_TO_RAD_F)};
        t_seed = (waywal_oklch_t){
            .l = 0.65f, .c = 0.02f, .h_rad = wrap_radians(h + 120.0f * DEG_TO_RAD_F)};
        n_seed = (waywal_oklch_t){.l = 0.65f, .c = 0.008f, .h_rad = h};
        nv_seed = (waywal_oklch_t){.l = 0.65f, .c = 0.014f, .h_rad = h};
    } else {
        float p_c = clamp_f(cands[0].c_ok, 0.09f, 0.20f);
        float p_h = cands[0].h_rad;
        p_seed = (waywal_oklch_t){.l = clamp_f(cands[0].l, 0.35f, 0.75f), .c = p_c, .h_rad = p_h};

        int sec_idx = -1;
        for (size_t i = 1; i < n_clusters; ++i) {
            float dh = hue_diff_deg(cands[0].h_deg, cands[i].h_deg);
            if (dh >= 25.0f && dh <= 140.0f && cands[i].c_ok >= 0.035f &&
                cands[i].score >= 0.25f * cands[0].score) {
                sec_idx = (int)i;
                break;
            }
        }

        if (sec_idx >= 0) {
            s_seed = (waywal_oklch_t){
                .l = clamp_f(cands[sec_idx].l, 0.35f, 0.75f),
                .c = clamp_f(cands[sec_idx].c_ok * 0.55f, 0.04f, 0.08f),
                .h_rad = cands[sec_idx].h_rad,
            };
        } else {
            s_seed = (waywal_oklch_t){
                .l = p_seed.l,
                .c = clamp_f(p_c * 0.38f, 0.035f, 0.065f),
                .h_rad = wrap_radians(p_h + 15.0f * DEG_TO_RAD_F),
            };
        }

        int tert_idx = -1;
        float s_deg = s_seed.h_rad * RAD_TO_DEG_F;
        for (size_t i = 1; i < n_clusters; ++i) {
            if ((int)i == sec_idx) {
                continue;
            }
            float dh1 = hue_diff_deg(cands[0].h_deg, cands[i].h_deg);
            float dh2 = hue_diff_deg(s_deg, cands[i].h_deg);
            if (dh1 >= 45.0f && dh2 >= 30.0f && cands[i].c_ok >= 0.04f &&
                cands[i].score >= 0.18f * cands[0].score) {
                tert_idx = (int)i;
                break;
            }
        }

        if (tert_idx >= 0) {
            t_seed = (waywal_oklch_t){
                .l = clamp_f(cands[tert_idx].l, 0.35f, 0.75f),
                .c = clamp_f(cands[tert_idx].c_ok * 0.75f, 0.06f, 0.13f),
                .h_rad = cands[tert_idx].h_rad,
            };
        } else {
            t_seed = (waywal_oklch_t){
                .l = p_seed.l,
                .c = clamp_f(p_c * 0.65f, 0.06f, 0.11f),
                .h_rad = wrap_radians(p_h + 60.0f * DEG_TO_RAD_F),
            };
        }

        n_seed = (waywal_oklch_t){.l = 0.5f, .c = 0.018f, .h_rad = p_h};
        nv_seed = (waywal_oklch_t){.l = 0.5f, .c = 0.036f, .h_rad = p_h};
    }

    apply_scheme_variant_seeds(variant, src_c_ok, &p_seed, &s_seed, &t_seed, &n_seed, &nv_seed,
                               &is_mono);

    if (out_dark) {
        synthesize_m3_scheme(out_dark, WAYWAL_THEME_DARK, variant, contrast, is_mono, p_seed,
                             s_seed, t_seed, n_seed, nv_seed);
        if (extracted_seed_count > 0U || has_seed_override) {
            out_dark->source_color = chosen_seed_rgb;
        }
        memcpy(out_dark->seeds, extracted_seeds, sizeof(extracted_seeds));
        out_dark->seed_count = extracted_seed_count;
        out_dark->source_color_index = effective_idx;
        out_dark->is_light_mode = false;
    }
    if (out_light) {
        synthesize_m3_scheme(out_light, WAYWAL_THEME_LIGHT, variant, contrast, is_mono, p_seed,
                             s_seed, t_seed, n_seed, nv_seed);
        if (extracted_seed_count > 0U || has_seed_override) {
            out_light->source_color = chosen_seed_rgb;
        }
        memcpy(out_light->seeds, extracted_seeds, sizeof(extracted_seeds));
        out_light->seed_count = extracted_seed_count;
        out_light->source_color_index = effective_idx;
        out_light->is_light_mode = true;
    }
    return true;
}

bool waywal_palette_extract_dual_ex(const uint8_t *pixels, uint32_t width, uint32_t height,
                                    uint32_t stride_bytes, bool is_bgra,
                                    waywal_scheme_variant_t variant, float contrast,
                                    uint8_t source_color_index, bool has_seed_override,
                                    uint32_t seed_override_rgb, waywal_palette_t *out_dark,
                                    waywal_palette_t *out_light)
{
    return extract_dual_impl(pixels, width, height, stride_bytes, is_bgra, variant, contrast,
                             source_color_index, has_seed_override, seed_override_rgb, out_dark,
                             out_light);
}

bool waywal_palette_extract_seed_ex(const uint8_t *pixels, uint32_t width, uint32_t height,
                                    uint32_t stride_bytes, bool is_bgra, waywal_theme_mode_t mode,
                                    waywal_scheme_variant_t variant, float contrast,
                                    uint8_t source_color_index, bool has_seed_override,
                                    uint32_t seed_override_rgb, waywal_palette_t *out_palette)
{
    if (!out_palette) {
        return false;
    }
    if (mode == WAYWAL_THEME_LIGHT) {
        return extract_dual_impl(pixels, width, height, stride_bytes, is_bgra, variant, contrast,
                                 source_color_index, has_seed_override, seed_override_rgb, NULL,
                                 out_palette);
    }
    return extract_dual_impl(pixels, width, height, stride_bytes, is_bgra, variant, contrast,
                             source_color_index, has_seed_override, seed_override_rgb, out_palette,
                             NULL);
}

bool waywal_palette_extract_dual(const uint8_t *rgba_pixels, uint32_t width, uint32_t height,
                                 uint32_t stride_bytes, waywal_palette_t *out_dark,
                                 waywal_palette_t *out_light)
{
    return extract_dual_impl(rgba_pixels, width, height, stride_bytes, false,
                             WAYWAL_SCHEME_TONAL_SPOT, 0.0f, 0U, false, 0U, out_dark, out_light);
}

bool waywal_palette_extract_dual_bgra(const uint8_t *bgra_pixels, uint32_t width, uint32_t height,
                                      uint32_t stride_bytes, waywal_palette_t *out_dark,
                                      waywal_palette_t *out_light)
{
    return extract_dual_impl(bgra_pixels, width, height, stride_bytes, true,
                             WAYWAL_SCHEME_TONAL_SPOT, 0.0f, 0U, false, 0U, out_dark, out_light);
}

bool waywal_palette_extract_ex(const uint8_t *pixels, uint32_t width, uint32_t height,
                               uint32_t stride_bytes, bool is_bgra, waywal_theme_mode_t mode,
                               waywal_scheme_variant_t variant, float contrast,
                               waywal_palette_t *out_palette)
{
    return waywal_palette_extract_seed_ex(pixels, width, height, stride_bytes, is_bgra, mode,
                                          variant, contrast, 0U, false, 0U, out_palette);
}

bool waywal_palette_extract(const uint8_t *rgba_pixels, uint32_t width, uint32_t height,
                            uint32_t stride_bytes, waywal_theme_mode_t mode,
                            waywal_palette_t *out_palette)
{
    return waywal_palette_extract_ex(rgba_pixels, width, height, stride_bytes, false, mode,
                                     WAYWAL_SCHEME_TONAL_SPOT, 0.0f, out_palette);
}

bool waywal_palette_extract_bgra(const uint8_t *bgra_pixels, uint32_t width, uint32_t height,
                                 uint32_t stride_bytes, waywal_theme_mode_t mode,
                                 waywal_palette_t *out_palette)
{
    return waywal_palette_extract_ex(bgra_pixels, width, height, stride_bytes, true, mode,
                                     WAYWAL_SCHEME_TONAL_SPOT, 0.0f, out_palette);
}

typedef struct {
    const char *name;
    color_rgba_t color;
} named_color_slot_t;

static size_t collect_color_slots(const waywal_palette_t *pal, named_color_slot_t *slots)
{
    size_t n = 0;
#define ADD_SLOT(field)                                                                            \
    do {                                                                                           \
        slots[n].name = #field;                                                                    \
        slots[n].color = pal->field;                                                               \
        n++;                                                                                       \
    } while (0)

    ADD_SLOT(primary_paletteKeyColor);
    ADD_SLOT(secondary_paletteKeyColor);
    ADD_SLOT(tertiary_paletteKeyColor);
    ADD_SLOT(neutral_paletteKeyColor);
    ADD_SLOT(neutral_variant_paletteKeyColor);
    ADD_SLOT(background);
    ADD_SLOT(onBackground);
    ADD_SLOT(surface);
    ADD_SLOT(surfaceDim);
    ADD_SLOT(surfaceBright);
    ADD_SLOT(surfaceContainerLowest);
    ADD_SLOT(surfaceContainerLow);
    ADD_SLOT(surfaceContainer);
    ADD_SLOT(surfaceContainerHigh);
    ADD_SLOT(surfaceContainerHighest);
    ADD_SLOT(onSurface);
    ADD_SLOT(surfaceVariant);
    ADD_SLOT(onSurfaceVariant);
    ADD_SLOT(inverseSurface);
    ADD_SLOT(inverseOnSurface);
    ADD_SLOT(outline);
    ADD_SLOT(outlineVariant);
    ADD_SLOT(shadow);
    ADD_SLOT(scrim);
    ADD_SLOT(surfaceTint);
    ADD_SLOT(primary);
    ADD_SLOT(onPrimary);
    ADD_SLOT(primaryContainer);
    ADD_SLOT(onPrimaryContainer);
    ADD_SLOT(inversePrimary);
    ADD_SLOT(secondary);
    ADD_SLOT(onSecondary);
    ADD_SLOT(secondaryContainer);
    ADD_SLOT(onSecondaryContainer);
    ADD_SLOT(tertiary);
    ADD_SLOT(onTertiary);
    ADD_SLOT(tertiaryContainer);
    ADD_SLOT(onTertiaryContainer);
    ADD_SLOT(error);
    ADD_SLOT(onError);
    ADD_SLOT(errorContainer);
    ADD_SLOT(onErrorContainer);
    ADD_SLOT(primaryFixed);
    ADD_SLOT(primaryFixedDim);
    ADD_SLOT(onPrimaryFixed);
    ADD_SLOT(onPrimaryFixedVariant);
    ADD_SLOT(secondaryFixed);
    ADD_SLOT(secondaryFixedDim);
    ADD_SLOT(onSecondaryFixed);
    ADD_SLOT(onSecondaryFixedVariant);
    ADD_SLOT(tertiaryFixed);
    ADD_SLOT(tertiaryFixedDim);
    ADD_SLOT(onTertiaryFixed);
    ADD_SLOT(onTertiaryFixedVariant);
    ADD_SLOT(term0);
    ADD_SLOT(term1);
    ADD_SLOT(term2);
    ADD_SLOT(term3);
    ADD_SLOT(term4);
    ADD_SLOT(term5);
    ADD_SLOT(term6);
    ADD_SLOT(term7);
    ADD_SLOT(term8);
    ADD_SLOT(term9);
    ADD_SLOT(term10);
    ADD_SLOT(term11);
    ADD_SLOT(term12);
    ADD_SLOT(term13);
    ADD_SLOT(term14);
    ADD_SLOT(term15);
    ADD_SLOT(rosewater);
    ADD_SLOT(flamingo);
    ADD_SLOT(pink);
    ADD_SLOT(mauve);
    ADD_SLOT(red);
    ADD_SLOT(maroon);
    ADD_SLOT(peach);
    ADD_SLOT(yellow);
    ADD_SLOT(green);
    ADD_SLOT(teal);
    ADD_SLOT(sky);
    ADD_SLOT(sapphire);
    ADD_SLOT(blue);
    ADD_SLOT(lavender);
    ADD_SLOT(klink);
    ADD_SLOT(klinkSelection);
    ADD_SLOT(kvisited);
    ADD_SLOT(kvisitedSelection);
    ADD_SLOT(knegative);
    ADD_SLOT(knegativeSelection);
    ADD_SLOT(kneutral);
    ADD_SLOT(kneutralSelection);
    ADD_SLOT(kpositive);
    ADD_SLOT(kpositiveSelection);
    ADD_SLOT(text);
    ADD_SLOT(subtext1);
    ADD_SLOT(subtext0);
    ADD_SLOT(overlay2);
    ADD_SLOT(overlay1);
    ADD_SLOT(overlay0);
    ADD_SLOT(surface2);
    ADD_SLOT(surface1);
    ADD_SLOT(surface0);
    ADD_SLOT(base);
    ADD_SLOT(mantle);
    ADD_SLOT(crust);
    ADD_SLOT(success);
    ADD_SLOT(onSuccess);
    ADD_SLOT(successContainer);
    ADD_SLOT(onSuccessContainer);
#undef ADD_SLOT
    return n;
}

size_t waywal_palette_format_sequences(const waywal_palette_t *pal, char *out_buf, size_t buf_cap)
{
    if (!pal || !out_buf || buf_cap == 0) {
        return 0;
    }

    size_t pos = 0;
#define APPEND_OSC(code_str, col)                                                                  \
    do {                                                                                           \
        int w = snprintf(out_buf + pos, (pos < buf_cap) ? (buf_cap - pos) : 0,                     \
                         "\033]" code_str ";rgb:%02x/%02x/%02x\033\\", (col).r, (col).g, (col).b); \
        if (w < 0)                                                                                 \
            return 0;                                                                              \
        pos += (size_t)w;                                                                          \
    } while (0)

    APPEND_OSC("10", pal->onSurface);
    APPEND_OSC("11", pal->surface);
    APPEND_OSC("12", pal->secondary);
    APPEND_OSC("17", pal->secondary);

    for (int i = 0; i < 16; ++i) {
        int w = snprintf(out_buf + pos, (pos < buf_cap) ? (buf_cap - pos) : 0,
                         "\033]4;%d;rgb:%02x/%02x/%02x\033\\", i, pal->term[i].r, pal->term[i].g,
                         pal->term[i].b);
        if (w < 0)
            return 0;
        pos += (size_t)w;
    }

    APPEND_OSC("4;16", pal->primary);
    APPEND_OSC("4;17", pal->secondary);
    APPEND_OSC("4;18", pal->tertiary);
#undef APPEND_OSC

    if (pos >= buf_cap) {
        out_buf[buf_cap - 1] = '\0';
        return buf_cap - 1;
    }
    return pos;
}

static bool mkdir_parents(const char *file_path)
{
    path_buf_t dir;
    path_buf_from_str(&dir, file_path);
    if (!path_buf_parent(&dir)) {
        return true;
    }

    char tmp[WAYWAL_PATH_MAX];
    size_t len = strlen(dir.data);
    if (len == 0 || len >= sizeof(tmp)) {
        return false;
    }
    memcpy(tmp, dir.data, len + 1);

    for (size_t i = 1; i < len; ++i) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return false;
            }
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

static bool resolve_target_or_parent_realpath(const char *path, char *out_resolved, size_t cap)
{
    if (!path || path[0] == '\0' || !out_resolved || cap == 0) {
        return false;
    }

    char cur[WAYWAL_PATH_MAX];
    int n = snprintf(cur, sizeof(cur), "%s", path);
    if (n < 0 || (size_t)n >= sizeof(cur)) {
        return false;
    }
    if (cur[0] == '/') {
        waywal_normalize_path(cur);
    }

    char rp[WAYWAL_PATH_MAX];
    for (int depth = 0; depth < 8; ++depth) {
        if (realpath(cur, rp) != NULL) {
            int rn = snprintf(out_resolved, cap, "%s", rp);
            return (rn >= 0 && (size_t)rn < cap);
        }
        struct stat st;
        if (lstat(cur, &st) != 0 || !S_ISLNK(st.st_mode)) {
            break;
        }
        char link_buf[WAYWAL_PATH_MAX];
        ssize_t llen = readlink(cur, link_buf, sizeof(link_buf) - 1);
        if (llen <= 0) {
            break;
        }
        link_buf[llen] = '\0';
        char next_cur[WAYWAL_PATH_MAX];
        if (link_buf[0] == '/') {
            n = snprintf(next_cur, sizeof(next_cur), "%s", link_buf);
        } else {
            char *slash = strrchr(cur, '/');
            if (slash && slash != cur) {
                *slash = '\0';
                n = snprintf(next_cur, sizeof(next_cur), "%s/%s", cur, link_buf);
            } else if (slash == cur) {
                n = snprintf(next_cur, sizeof(next_cur), "/%s", link_buf);
            } else {
                n = snprintf(next_cur, sizeof(next_cur), "%s", link_buf);
            }
        }
        if (n < 0 || (size_t)n >= sizeof(next_cur)) {
            return false;
        }
        if (next_cur[0] == '/') {
            waywal_normalize_path(next_cur);
        }
        memcpy(cur, next_cur, (size_t)n + 1);
    }

    if (cur[0] == '/') {
        char probe[WAYWAL_PATH_MAX];
        snprintf(probe, sizeof(probe), "%s", cur);
        while (probe[0] == '/' && strcmp(probe, "/") != 0) {
            char *slash = strrchr(probe, '/');
            if (!slash || slash == probe) {
                break;
            }
            size_t prefix_len = (size_t)(slash - probe);
            probe[prefix_len] = '\0';
            if (realpath(probe, rp) != NULL) {
                char joined[WAYWAL_PATH_MAX];
                int jn = snprintf(joined, sizeof(joined), "%s%s", rp, cur + prefix_len);
                if (jn < 0 || (size_t)jn >= sizeof(joined)) {
                    return false;
                }
                waywal_normalize_path(joined);
                int on = snprintf(out_resolved, cap, "%s", joined);
                return (on >= 0 && (size_t)on < cap);
            }
        }
    }

    int on = snprintf(out_resolved, cap, "%s", cur);
    return (on >= 0 && (size_t)on < cap);
}

static bool atomic_write_file(const char *target_path, const char *data, size_t len)
{
    if (!target_path || !data) {
        return false;
    }

    char resolved_target[WAYWAL_PATH_MAX];
    const char *write_path = target_path;
    struct stat st;
    if (lstat(target_path, &st) == 0 && S_ISLNK(st.st_mode)) {
        if (resolve_target_or_parent_realpath(target_path, resolved_target,
                                              sizeof(resolved_target))) {
            write_path = resolved_target;
        }
    }

    if (!mkdir_parents(write_path)) {
        return false;
    }

    char tmp_path[WAYWAL_PATH_MAX];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", write_path, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        return false;
    }

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }

    const char *p = data;
    size_t rem = len;
    bool ok = true;
    while (rem > 0) {
        ssize_t w = write(fd, p, rem);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        p += (size_t)w;
        rem -= (size_t)w;
    }
    close(fd);

    if (!ok) {
        unlink(tmp_path);
        return false;
    }
    if (rename(tmp_path, write_path) != 0) {
        unlink(tmp_path);
        return false;
    }
    return true;
}

static size_t format_caelestia_json(const waywal_palette_t *pal, const named_color_slot_t *slots,
                                    size_t n_slots, char *buf, size_t cap)
{
    const char *mode_str = (pal->mode == WAYWAL_THEME_LIGHT) ? "light" : "dark";
    const char *variant_str = scheme_variant_to_caelestia_str(pal->variant);
    int n = snprintf(buf, cap,
                     "{\"name\": \"dynamic\", \"flavour\": \"default\", \"mode\": \"%s\", "
                     "\"variant\": \"%s\", \"colours\": {",
                     mode_str, variant_str);
    if (n < 0 || (size_t)n >= cap)
        return 0;
    size_t pos = (size_t)n;

    for (size_t i = 0; i < n_slots; ++i) {
        const char *sep = (i + 1 < n_slots) ? ", " : "";
        n = snprintf(buf + pos, cap - pos, "\"%s\": \"%02x%02x%02x\"%s", slots[i].name,
                     slots[i].color.r, slots[i].color.g, slots[i].color.b, sep);
        if (n < 0 || pos + (size_t)n >= cap)
            return 0;
        pos += (size_t)n;
    }

    n = snprintf(buf + pos, cap - pos, "}}");
    if (n < 0 || pos + (size_t)n >= cap)
        return 0;
    pos += (size_t)n;
    return pos;
}

static size_t format_wwal_colors_json(const waywal_palette_t *pal, const named_color_slot_t *slots,
                                      size_t n_slots, char *buf, size_t cap)
{
    const char *mode_str = (pal->mode == WAYWAL_THEME_LIGHT) ? "light" : "dark";
    int n = snprintf(buf, cap, "{\n  \"mode\": \"%s\",\n  \"colors\": {\n", mode_str);
    if (n < 0 || (size_t)n >= cap)
        return 0;
    size_t pos = (size_t)n;

    for (size_t i = 0; i < n_slots; ++i) {
        const char *comma = (i + 1 < n_slots) ? "," : "";
        n = snprintf(buf + pos, cap - pos, "    \"%s\": \"#%02x%02x%02x\"%s\n", slots[i].name,
                     slots[i].color.r, slots[i].color.g, slots[i].color.b, comma);
        if (n < 0 || pos + (size_t)n >= cap)
            return 0;
        pos += (size_t)n;
    }

    n = snprintf(buf + pos, cap - pos, "  }\n}\n");
    if (n < 0 || pos + (size_t)n >= cap)
        return 0;
    pos += (size_t)n;
    return pos;
}

static size_t format_wwal_css(const named_color_slot_t *slots, size_t n_slots, char *buf,
                              size_t cap)
{
    int n = snprintf(buf, cap, ":root {\n");
    if (n < 0 || (size_t)n >= cap)
        return 0;
    size_t pos = (size_t)n;

    for (size_t i = 0; i < n_slots; ++i) {
        n = snprintf(buf + pos, cap - pos, "  --%s: #%02x%02x%02x;\n", slots[i].name,
                     slots[i].color.r, slots[i].color.g, slots[i].color.b);
        if (n < 0 || pos + (size_t)n >= cap)
            return 0;
        pos += (size_t)n;
    }

    n = snprintf(buf + pos, cap - pos, "}\n");
    if (n < 0 || pos + (size_t)n >= cap)
        return 0;
    pos += (size_t)n;
    return pos;
}

static size_t format_wwal_hyprland(const named_color_slot_t *slots, size_t n_slots, char *buf,
                                   size_t cap)
{
    size_t pos = 0;
    for (size_t i = 0; i < n_slots; ++i) {
        int n = snprintf(buf + pos, cap - pos, "$%s = rgba(%02x%02x%02xff)\n", slots[i].name,
                         slots[i].color.r, slots[i].color.g, slots[i].color.b);
        if (n < 0 || pos + (size_t)n >= cap)
            return 0;
        pos += (size_t)n;
    }
    return pos;
}

static void normalize_role_key(const char *in, char *out, size_t cap)
{
    size_t w = 0;
    for (const char *p = in; *p != '\0' && w + 1 < cap; ++p) {
        if (*p == '_' || *p == '-') {
            continue;
        }
        out[w++] = (char)tolower((unsigned char)*p);
    }
    out[w] = '\0';
}

static bool parse_hex_literal_color(const char *s, color_rgba_t *out_col)
{
    if (!s)
        return false;
    while (*s && isspace((unsigned char)*s))
        s++;
    if (s[0] == '#')
        s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    else
        return false;

    size_t len = 0;
    while (isxdigit((unsigned char)s[len]))
        len++;
    if (len != 3 && len != 6 && len != 8)
        return false;
    const char *after = s + len;
    while (*after && isspace((unsigned char)*after))
        after++;
    if (*after != '\0')
        return false;

    char hex_buf[12];
    memcpy(hex_buf, s, len);
    hex_buf[len] = '\0';
    unsigned long v = strtoul(hex_buf, NULL, 16);
    if (len == 3) {
        uint8_t r = (uint8_t)((v >> 8) & 0xFU);
        uint8_t g = (uint8_t)((v >> 4) & 0xFU);
        uint8_t b = (uint8_t)(v & 0xFU);
        out_col->r = (uint8_t)((r << 4) | r);
        out_col->g = (uint8_t)((g << 4) | g);
        out_col->b = (uint8_t)((b << 4) | b);
        out_col->a = 255U;
    } else if (len == 6) {
        out_col->r = (uint8_t)((v >> 16) & 0xFFU);
        out_col->g = (uint8_t)((v >> 8) & 0xFFU);
        out_col->b = (uint8_t)(v & 0xFFU);
        out_col->a = 255U;
    } else {
        out_col->r = (uint8_t)((v >> 24) & 0xFFU);
        out_col->g = (uint8_t)((v >> 16) & 0xFFU);
        out_col->b = (uint8_t)((v >> 8) & 0xFFU);
        out_col->a = (uint8_t)(v & 0xFFU);
    }
    return true;
}

static bool resolve_palette_role_color(const waywal_palette_t *pal, const char *role,
                                       color_rgba_t *out_col)
{
    if (!pal || !role || !out_col) {
        return false;
    }

    if (parse_hex_literal_color(role, out_col)) {
        return true;
    }

    char norm[64];
    normalize_role_key(role, norm, sizeof(norm));

    if (strcmp(norm, "sourcecolor") == 0) {
        if (pal->seed_count > 0U || pal->source_color != 0U) {
            out_col->r = (uint8_t)((pal->source_color >> 16) & 0xFFU);
            out_col->g = (uint8_t)((pal->source_color >> 8) & 0xFFU);
            out_col->b = (uint8_t)(pal->source_color & 0xFFU);
            out_col->a = 255U;
        } else {
            *out_col = pal->primary_paletteKeyColor;
        }
        return true;
    }
    if (strcmp(norm, "foreground") == 0) {
        *out_col = pal->onSurface;
        return true;
    }
    if (strcmp(norm, "cursor") == 0) {
        *out_col = pal->secondary;
        return true;
    }
    if (strncmp(norm, "color", 5) == 0 && isdigit((unsigned char)norm[5])) {
        char *end = NULL;
        unsigned long idx = strtoul(norm + 5, &end, 10);
        if (end && *end == '\0' && idx < 16UL) {
            *out_col = pal->term[idx];
            return true;
        }
    }

    named_color_slot_t slots[128];
    size_t n_slots = collect_color_slots(pal, slots);
    for (size_t i = 0; i < n_slots; ++i) {
        if (strcmp(slots[i].name, role) == 0) {
            *out_col = slots[i].color;
            return true;
        }
        char slot_norm[64];
        normalize_role_key(slots[i].name, slot_norm, sizeof(slot_norm));
        if (strcmp(slot_norm, norm) == 0) {
            *out_col = slots[i].color;
            return true;
        }
    }
    return false;
}

static bool is_color_format_prop(const char *prop)
{
    if (!prop || prop[0] == '\0')
        return false;
    return (strcmp(prop, "hex") == 0 || strcmp(prop, "strip") == 0 ||
            strcmp(prop, "hex_stripped") == 0 || strcmp(prop, "rgb") == 0 ||
            strcmp(prop, "rgb_css") == 0 || strcmp(prop, "rgba") == 0 ||
            strcmp(prop, "xrgba") == 0 || strcmp(prop, "octal") == 0 ||
            strcmp(prop, "hsl") == 0 || strcmp(prop, "oklch") == 0 ||
            strcmp(prop, "red") == 0 || strcmp(prop, "r") == 0 ||
            strcmp(prop, "green") == 0 || strcmp(prop, "g") == 0 ||
            strcmp(prop, "blue") == 0 || strcmp(prop, "b") == 0 ||
            strcmp(prop, "alpha") == 0 || strcmp(prop, "a") == 0 ||
            strcmp(prop, "hue") == 0 || strcmp(prop, "h") == 0 ||
            strcmp(prop, "saturation") == 0 || strcmp(prop, "s") == 0 ||
            strcmp(prop, "lightness") == 0 || strcmp(prop, "l") == 0);
}

static void rgba_to_hsl_deg(color_rgba_t c, float *out_h, float *out_s, float *out_l)
{
    float r = (float)c.r / 255.0f;
    float g = (float)c.g / 255.0f;
    float b = (float)c.b / 255.0f;
    float mx = fmaxf(r, fmaxf(g, b));
    float mn = fminf(r, fminf(g, b));
    float l = 0.5f * (mx + mn);
    float d = mx - mn;
    float h = 0.0f;
    float s = 0.0f;
    if (d > 1e-5f) {
        s = (l > 0.5f) ? (d / (2.0f - mx - mn)) : (d / (mx + mn));
        if (mx == r) {
            h = (g - b) / d + (g < b ? 6.0f : 0.0f);
        } else if (mx == g) {
            h = (b - r) / d + 2.0f;
        } else {
            h = (r - g) / d + 4.0f;
        }
        h *= 60.0f;
    }
    *out_h = clamp_f(h, 0.0f, 360.0f);
    *out_s = clamp_f(s * 100.0f, 0.0f, 100.0f);
    *out_l = clamp_f(l * 100.0f, 0.0f, 100.0f);
}

static color_rgba_t hsl_deg_to_rgba(float h_deg, float s_pct, float l_pct, uint8_t alpha)
{
    h_deg = fmodf(h_deg, 360.0f);
    if (h_deg < 0.0f) {
        h_deg += 360.0f;
    }
    float s = clamp_f(s_pct * 0.01f, 0.0f, 1.0f);
    float l = clamp_f(l_pct * 0.01f, 0.0f, 1.0f);
    float c = (1.0f - fabsf(2.0f * l - 1.0f)) * s;
    float hp = h_deg / 60.0f;
    float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    float m = l - 0.5f * c;
    float r1 = 0.0f, g1 = 0.0f, b1 = 0.0f;
    if (hp < 1.0f) {
        r1 = c;
        g1 = x;
    } else if (hp < 2.0f) {
        r1 = x;
        g1 = c;
    } else if (hp < 3.0f) {
        g1 = c;
        b1 = x;
    } else if (hp < 4.0f) {
        g1 = x;
        b1 = c;
    } else if (hp < 5.0f) {
        r1 = x;
        b1 = c;
    } else {
        r1 = c;
        b1 = x;
    }
    return (color_rgba_t){
        .r = (uint8_t)clamp_i((int)((r1 + m) * 255.0f + 0.5f), 0, 255),
        .g = (uint8_t)clamp_i((int)((g1 + m) * 255.0f + 0.5f), 0, 255),
        .b = (uint8_t)clamp_i((int)((b1 + m) * 255.0f + 0.5f), 0, 255),
        .a = alpha,
    };
}

static int format_color_property(color_rgba_t c, float alpha_f, const char *fmt, char *out_buf,
                                 size_t out_cap)
{
    if (!fmt || fmt[0] == '\0' || strcmp(fmt, "hex") == 0) {
        return snprintf(out_buf, out_cap, "#%02x%02x%02x", c.r, c.g, c.b);
    }
    if (strcmp(fmt, "strip") == 0 || strcmp(fmt, "hex_stripped") == 0) {
        return snprintf(out_buf, out_cap, "%02x%02x%02x", c.r, c.g, c.b);
    }
    if (strcmp(fmt, "rgb") == 0) {
        return snprintf(out_buf, out_cap, "%u, %u, %u", c.r, c.g, c.b);
    }
    if (strcmp(fmt, "rgb_css") == 0) {
        return snprintf(out_buf, out_cap, "rgb(%u, %u, %u)", c.r, c.g, c.b);
    }
    if (strcmp(fmt, "rgba") == 0) {
        return snprintf(out_buf, out_cap, "rgba(%u, %u, %u, %.2f)", c.r, c.g, c.b,
                        clamp_f(alpha_f, 0.0f, 1.0f));
    }
    if (strcmp(fmt, "xrgba") == 0) {
        uint8_t a8 = (uint8_t)clamp_i((int)(clamp_f(alpha_f, 0.0f, 1.0f) * 255.0f + 0.5f), 0, 255);
        return snprintf(out_buf, out_cap, "%02x/%02x/%02x/%02x", c.r, c.g, c.b, a8);
    }
    if (strcmp(fmt, "octal") == 0) {
        return snprintf(out_buf, out_cap, "%03o, %03o, %03o", c.r, c.g, c.b);
    }
    if (strcmp(fmt, "red") == 0 || strcmp(fmt, "r") == 0) {
        return snprintf(out_buf, out_cap, "%u", c.r);
    }
    if (strcmp(fmt, "green") == 0 || strcmp(fmt, "g") == 0) {
        return snprintf(out_buf, out_cap, "%u", c.g);
    }
    if (strcmp(fmt, "blue") == 0 || strcmp(fmt, "b") == 0) {
        return snprintf(out_buf, out_cap, "%u", c.b);
    }
    if (strcmp(fmt, "alpha") == 0 || strcmp(fmt, "a") == 0) {
        return snprintf(out_buf, out_cap, "%.2f", clamp_f(alpha_f, 0.0f, 1.0f));
    }
    if (strcmp(fmt, "hsl") == 0 || strcmp(fmt, "hue") == 0 || strcmp(fmt, "h") == 0 ||
        strcmp(fmt, "saturation") == 0 || strcmp(fmt, "s") == 0 ||
        strcmp(fmt, "lightness") == 0 || strcmp(fmt, "l") == 0) {
        float h = 0.0f, s = 0.0f, l = 0.0f;
        rgba_to_hsl_deg(c, &h, &s, &l);
        if (strcmp(fmt, "hsl") == 0) {
            return snprintf(out_buf, out_cap, "hsl(%.0f, %.0f%%, %.0f%%)", h, s, l);
        }
        if (strcmp(fmt, "hue") == 0 || strcmp(fmt, "h") == 0) {
            return snprintf(out_buf, out_cap, "%.0f", h);
        }
        if (strcmp(fmt, "saturation") == 0 || strcmp(fmt, "s") == 0) {
            return snprintf(out_buf, out_cap, "%.0f", s);
        }
        return snprintf(out_buf, out_cap, "%.0f", l);
    }
    if (strcmp(fmt, "oklch") == 0) {
        ensure_srgb_lut();
        waywal_oklab_t ok =
            linear_rgb_to_oklab(g_srgb_to_linear[c.r], g_srgb_to_linear[c.g], g_srgb_to_linear[c.b]);
        float ch = sqrtf(ok.a * ok.a + ok.b * ok.b);
        float hd = (ch > 1e-4f) ? (wrap_radians(atan2f(ok.b, ok.a)) * RAD_TO_DEG_F) : 0.0f;
        return snprintf(out_buf, out_cap, "oklch(%.3f %.3f %.1f)", ok.l, ch, hd);
    }
    return -1;
}

static bool resolve_base_color_expr(const waywal_palette_t *default_pal,
                                    const waywal_palette_t *dark_pal,
                                    const waywal_palette_t *light_pal, const char *base_expr,
                                    color_rgba_t *out_col, char *out_fmt, size_t fmt_cap)
{
    char buf[128];
    while (*base_expr && isspace((unsigned char)*base_expr))
        base_expr++;
    size_t blen = strlen(base_expr);
    while (blen > 0 && isspace((unsigned char)base_expr[blen - 1]))
        blen--;
    if (blen == 0 || blen >= sizeof(buf))
        return false;
    memcpy(buf, base_expr, blen);
    buf[blen] = '\0';

    snprintf(out_fmt, fmt_cap, "hex");

    if (parse_hex_literal_color(buf, out_col)) {
        return true;
    }

    char *segs[6] = {0};
    size_t n_segs = 0;
    char *p = buf;
    while (p && n_segs < 6) {
        segs[n_segs++] = p;
        char *dot = strchr(p, '.');
        if (!dot)
            break;
        if (n_segs == 6)
            return false;
        *dot = '\0';
        p = dot + 1;
    }

    if (n_segs >= 3 && strcmp(segs[0], "palettes") == 0) {
        if (n_segs > 4) {
            return false;
        }
        const char *fam = segs[1];
        const char *tone_str = segs[2];
        if (tone_str[0] == '\0') {
            return false;
        }
        for (const char *tp = tone_str; *tp != '\0'; ++tp) {
            if (!isdigit((unsigned char)*tp)) {
                return false;
            }
        }
        char *end = NULL;
        float tone = strtof(tone_str, &end);
        if (!end || end == tone_str || *end != '\0' || !isfinite(tone) || tone < 0.0f ||
            tone > 100.0f) {
            return false;
        }
        if (n_segs == 4) {
            if (!is_color_format_prop(segs[3]))
                return false;
            snprintf(out_fmt, fmt_cap, "%s", segs[3]);
        }
        char fam_norm[32];
        normalize_role_key(fam, fam_norm, sizeof(fam_norm));
        waywal_oklch_t seed = default_pal->primary_seed;
        if (seed.l == 0.0f && seed.c == 0.0f) {
            seed = rgba_to_oklch_seed(default_pal->primary, 250.0f * DEG_TO_RAD_F);
        }
        if (strcmp(fam_norm, "primary") == 0) {
            /* already primary_seed */
        } else if (strcmp(fam_norm, "secondary") == 0) {
            seed = default_pal->secondary_seed;
            if (seed.l == 0.0f && seed.c == 0.0f)
                seed = rgba_to_oklch_seed(default_pal->secondary, 265.0f * DEG_TO_RAD_F);
        } else if (strcmp(fam_norm, "tertiary") == 0) {
            seed = default_pal->tertiary_seed;
            if (seed.l == 0.0f && seed.c == 0.0f)
                seed = rgba_to_oklch_seed(default_pal->tertiary, 310.0f * DEG_TO_RAD_F);
        } else if (strcmp(fam_norm, "neutral") == 0) {
            seed = default_pal->neutral_seed;
            if (seed.l == 0.0f && seed.c == 0.0f)
                seed = (waywal_oklch_t){.l = 0.5f, .c = 0.018f, .h_rad = 4.5f};
        } else if (strcmp(fam_norm, "neutralvariant") == 0) {
            seed = default_pal->neutral_variant_seed;
            if (seed.l == 0.0f && seed.c == 0.0f)
                seed = (waywal_oklch_t){.l = 0.5f, .c = 0.036f, .h_rad = 4.5f};
        } else if (strcmp(fam_norm, "error") == 0) {
            seed = (waywal_oklch_t){.l = 0.5f, .c = 0.18f, .h_rad = 25.0f * DEG_TO_RAD_F};
        } else {
            return false;
        }
        *out_col = waywal_palette_generate_tone(tone, seed.c, seed.h_rad);
        return true;
    }

    size_t idx = 0;
    if (n_segs >= 2 && strcmp(segs[0], "colors") == 0) {
        idx = 1;
    }
    if (idx >= n_segs) {
        return false;
    }

    const char *role = segs[idx++];
    const waywal_palette_t *target_pal = default_pal;

    if (idx < n_segs) {
        if (strcmp(segs[idx], "default") == 0) {
            target_pal = default_pal;
            idx++;
        } else if (strcmp(segs[idx], "dark") == 0) {
            target_pal = dark_pal ? dark_pal : default_pal;
            idx++;
        } else if (strcmp(segs[idx], "light") == 0) {
            target_pal = light_pal ? light_pal : default_pal;
            idx++;
        }
    }

    if (idx < n_segs) {
        if (!is_color_format_prop(segs[idx])) {
            return false;
        }
        snprintf(out_fmt, fmt_cap, "%s", segs[idx]);
        idx++;
    }
    if (idx != n_segs) {
        return false;
    }

    return resolve_palette_role_color(target_pal, role, out_col);
}

static bool apply_pipe_filter(const waywal_palette_t *default_pal,
                              const waywal_palette_t *dark_pal, const waywal_palette_t *light_pal,
                              const char *filter_str, color_rgba_t *col, float *alpha_f,
                              char *fmt_buf, size_t fmt_cap)
{
    char buf[128];
    while (*filter_str && isspace((unsigned char)*filter_str))
        filter_str++;
    size_t flen = strlen(filter_str);
    while (flen > 0 && isspace((unsigned char)filter_str[flen - 1]))
        flen--;
    if (flen == 0 || flen >= sizeof(buf))
        return false;
    memcpy(buf, filter_str, flen);
    buf[flen] = '\0';

    char *lparen = strchr(buf, '(');
    char *args = NULL;
    if (lparen) {
        *lparen = '\0';
        args = lparen + 1;
        char *rparen = strrchr(args, ')');
        if (!rparen || strchr(args, '(') != NULL || strchr(args, ')') != rparen) {
            return false;
        }
        char *after_r = rparen + 1;
        while (*after_r && isspace((unsigned char)*after_r))
            after_r++;
        if (*after_r != '\0') {
            return false;
        }
        *rparen = '\0';
    } else {
        /* Support space-separated argument like `lighten 0.2` */
        char *sp = buf;
        while (*sp && !isspace((unsigned char)*sp))
            sp++;
        if (*sp) {
            *sp = '\0';
            args = sp + 1;
        }
    }

    char *fname = buf;
    while (*fname && isspace((unsigned char)*fname))
        fname++;
    size_t fnlen = strlen(fname);
    while (fnlen > 0 && isspace((unsigned char)fname[fnlen - 1]))
        fname[--fnlen] = '\0';

    if (args) {
        while (*args && isspace((unsigned char)*args))
            args++;
        size_t alen = strlen(args);
        while (alen > 0 && isspace((unsigned char)args[alen - 1]))
            args[--alen] = '\0';
    }

    if (!args || args[0] == '\0') {
        if (lparen) {
            return false;
        }
        if (strcmp(fname, "invert") == 0) {
            col->r = (uint8_t)(255U - col->r);
            col->g = (uint8_t)(255U - col->g);
            col->b = (uint8_t)(255U - col->b);
            return true;
        }
        if (strcmp(fname, "complementary") == 0) {
            float h = 0.0f, s = 0.0f, l = 0.0f;
            rgba_to_hsl_deg(*col, &h, &s, &l);
            *col = hsl_deg_to_rgba(h + 180.0f, s, l, col->a);
            return true;
        }
        if (is_color_format_prop(fname)) {
            snprintf(fmt_buf, fmt_cap, "%s", fname);
            return true;
        }
        return false;
    }

    if (strcmp(fname, "blend") == 0) {
        char *comma = strchr(args, ',');
        if (!comma)
            return false;
        *comma = '\0';
        const char *target_str = args;
        const char *factor_str = comma + 1;
        color_rgba_t target_col = {0};
        char dummy_fmt[32];
        if (!resolve_base_color_expr(default_pal, dark_pal, light_pal, target_str, &target_col,
                                     dummy_fmt, sizeof(dummy_fmt))) {
            return false;
        }
        char *end = NULL;
        float factor = strtof(factor_str, &end);
        if (!end || end == factor_str || !isfinite(factor))
            return false;
        while (*end && isspace((unsigned char)*end))
            end++;
        if (*end != '\0')
            return false;
        if (fabsf(factor) > 1.0f)
            factor *= 0.01f;
        factor = clamp_f(factor, 0.0f, 1.0f);

        ensure_srgb_lut();
        waywal_oklab_t ok1 = linear_rgb_to_oklab(
            g_srgb_to_linear[col->r], g_srgb_to_linear[col->g], g_srgb_to_linear[col->b]);
        waywal_oklab_t ok2 = linear_rgb_to_oklab(g_srgb_to_linear[target_col.r],
                                                 g_srgb_to_linear[target_col.g],
                                                 g_srgb_to_linear[target_col.b]);
        float l = ok1.l + (ok2.l - ok1.l) * factor;
        float a = ok1.a + (ok2.a - ok1.a) * factor;
        float b = ok1.b + (ok2.b - ok1.b) * factor;
        float rl = 0.0f, gl = 0.0f, bl = 0.0f;
        oklab_to_linear_rgb(l, a, b, &rl, &gl, &bl);
        col->r = linear_to_srgb_u8(rl);
        col->g = linear_to_srgb_u8(gl);
        col->b = linear_to_srgb_u8(bl);
        return true;
    }

    char *end = NULL;
    float val = strtof(args, &end);
    if (!end || end == args || !isfinite(val)) {
        return false;
    }
    while (*end && isspace((unsigned char)*end))
        end++;
    if (*end != '\0') {
        return false;
    }

    if (strcmp(fname, "lighten") == 0) {
        float delta = (fabsf(val) <= 1.0f) ? (val * 100.0f) : val;
        float h = 0.0f, s = 0.0f, l = 0.0f;
        rgba_to_hsl_deg(*col, &h, &s, &l);
        l = clamp_f(l + delta, 0.0f, 100.0f);
        *col = hsl_deg_to_rgba(h, s, l, col->a);
        return true;
    }
    if (strcmp(fname, "darken") == 0) {
        float delta = (fabsf(val) <= 1.0f) ? (val * 100.0f) : val;
        float h = 0.0f, s = 0.0f, l = 0.0f;
        rgba_to_hsl_deg(*col, &h, &s, &l);
        l = clamp_f(l - delta, 0.0f, 100.0f);
        *col = hsl_deg_to_rgba(h, s, l, col->a);
        return true;
    }
    if (strcmp(fname, "set_lightness") == 0 || strcmp(fname, "lightness") == 0) {
        float target_l = (fabsf(val) <= 1.0f) ? (val * 100.0f) : val;
        float h = 0.0f, s = 0.0f, l = 0.0f;
        rgba_to_hsl_deg(*col, &h, &s, &l);
        l = clamp_f(target_l, 0.0f, 100.0f);
        *col = hsl_deg_to_rgba(h, s, l, col->a);
        return true;
    }
    if (strcmp(fname, "saturate") == 0) {
        float delta = (fabsf(val) <= 1.0f) ? (val * 100.0f) : val;
        float h = 0.0f, s = 0.0f, l = 0.0f;
        rgba_to_hsl_deg(*col, &h, &s, &l);
        s = clamp_f(s + delta, 0.0f, 100.0f);
        *col = hsl_deg_to_rgba(h, s, l, col->a);
        return true;
    }
    if (strcmp(fname, "desaturate") == 0) {
        float delta = (fabsf(val) <= 1.0f) ? (val * 100.0f) : val;
        float h = 0.0f, s = 0.0f, l = 0.0f;
        rgba_to_hsl_deg(*col, &h, &s, &l);
        s = clamp_f(s - delta, 0.0f, 100.0f);
        *col = hsl_deg_to_rgba(h, s, l, col->a);
        return true;
    }
    if (strcmp(fname, "alpha") == 0 || strcmp(fname, "set_alpha") == 0) {
        float a = (fabsf(val) <= 1.0f) ? val : (val * 0.01f);
        a = clamp_f(a, 0.0f, 1.0f);
        *alpha_f = a;
        col->a = (uint8_t)clamp_i((int)(a * 255.0f + 0.5f), 0, 255);
        if (strcmp(fmt_buf, "rgb") == 0 || strcmp(fmt_buf, "rgb_css") == 0) {
            snprintf(fmt_buf, fmt_cap, "rgba");
        }
        return true;
    }
    return false;
}

static int evaluate_template_expr(const waywal_palette_t *default_pal,
                                  const waywal_palette_t *dark_pal,
                                  const waywal_palette_t *light_pal, const char *raw_expr,
                                  char *out_buf, size_t out_cap)
{
    char expr[256];
    while (*raw_expr && isspace((unsigned char)*raw_expr))
        raw_expr++;
    size_t elen = strlen(raw_expr);
    while (elen > 0 && isspace((unsigned char)raw_expr[elen - 1]))
        elen--;
    if (elen == 0 || elen >= sizeof(expr))
        return -1;
    memcpy(expr, raw_expr, elen);
    expr[elen] = '\0';

    if (strcmp(expr, "mode") == 0) {
        return snprintf(out_buf, out_cap, "%s",
                        (default_pal->mode == WAYWAL_THEME_LIGHT) ? "light" : "dark");
    }
    if (strcmp(expr, "image") == 0 || strcmp(expr, "wallpaper") == 0) {
        return snprintf(out_buf, out_cap, "%s", default_pal->wallpaper_path);
    }

    char *work = expr;
    while (work[0] == '(') {
        int depth = 0;
        char *match_r = NULL;
        for (char *p = work; *p != '\0'; ++p) {
            if (*p == '(') {
                depth++;
            } else if (*p == ')') {
                depth--;
                if (depth == 0) {
                    match_r = p;
                    break;
                }
            }
        }
        if (!match_r) {
            return -1;
        }
        char *after = match_r + 1;
        while (*after && isspace((unsigned char)*after))
            after++;
        if (*after == '\0' || *after == '|') {
            *work = ' ';
            *match_r = ' ';
        } else if (*after == '.') {
            char *prop_start = after + 1;
            char *prop_end = prop_start;
            while (*prop_end && (isalnum((unsigned char)*prop_end) || *prop_end == '_')) {
                prop_end++;
            }
            size_t plen = (size_t)(prop_end - prop_start);
            if (plen == 0 || plen >= 32) {
                return -1;
            }
            char prop_name[32];
            memcpy(prop_name, prop_start, plen);
            prop_name[plen] = '\0';
            if (!is_color_format_prop(prop_name)) {
                return -1;
            }
            char *post = prop_end;
            while (*post && isspace((unsigned char)*post))
                post++;
            if (*post != '\0' && *post != '|') {
                return -1;
            }
            *work = ' ';
            *match_r = '|';
            *after = ' ';
        } else {
            return -1;
        }
        while (*work && isspace((unsigned char)*work))
            work++;
    }

    char *pipes[16] = {0};
    size_t n_pipes = 0;
    char *cur = work;
    while (cur && n_pipes < 16) {
        pipes[n_pipes++] = cur;
        char *bar = strchr(cur, '|');
        if (!bar)
            break;
        if (n_pipes == 16)
            return -1;
        *bar = '\0';
        cur = bar + 1;
    }

    color_rgba_t col = {0};
    float alpha_f = 1.0f;
    char fmt[32] = "hex";

    if (!resolve_base_color_expr(default_pal, dark_pal, light_pal, pipes[0], &col, fmt,
                                 sizeof(fmt))) {
        return -1;
    }
    alpha_f = (float)col.a / 255.0f;

    for (size_t i = 1; i < n_pipes; ++i) {
        if (!apply_pipe_filter(default_pal, dark_pal, light_pal, pipes[i], &col, &alpha_f, fmt,
                               sizeof(fmt))) {
            return -1;
        }
    }

    return format_color_property(col, alpha_f, fmt, out_buf, out_cap);
}

static bool is_pywal_single_brace_token(const char *tok)
{
    if (!tok || tok[0] == '\0')
        return false;
    if (strcmp(tok, "wallpaper") == 0)
        return true;

    char buf[64];
    size_t len = strlen(tok);
    if (len >= sizeof(buf))
        return false;
    memcpy(buf, tok, len + 1);

    char *dot = strchr(buf, '.');
    if (dot) {
        *dot = '\0';
        const char *prop = dot + 1;
        if (!is_color_format_prop(prop))
            return false;
    }

    if (strcmp(buf, "background") == 0 || strcmp(buf, "foreground") == 0 ||
        strcmp(buf, "cursor") == 0) {
        return true;
    }
    if (strncmp(buf, "color", 5) == 0 && isdigit((unsigned char)buf[5])) {
        char *end = NULL;
        unsigned long idx = strtoul(buf + 5, &end, 10);
        if (end && *end == '\0' && idx < 16UL) {
            return true;
        }
    }
    return false;
}

static bool render_single_template_dual(const waywal_palette_t *default_pal,
                                        const waywal_palette_t *dark_pal,
                                        const waywal_palette_t *light_pal, const char *src,
                                        size_t src_len, const char *out_path)
{
    if (!default_pal || !src || !out_path) {
        return false;
    }

    waywal_palette_t derived_opp;
    if (!dark_pal || !light_pal) {
        waywal_palette_derive_opposite(default_pal, &derived_opp);
        if (!dark_pal) {
            dark_pal = (default_pal->mode == WAYWAL_THEME_DARK) ? default_pal : &derived_opp;
        }
        if (!light_pal) {
            light_pal = (default_pal->mode == WAYWAL_THEME_LIGHT) ? default_pal : &derived_opp;
        }
    }

    size_t cap = src_len * 2 + 4096;
    char *out = (char *)malloc(cap);
    if (!out) {
        return false;
    }

    size_t pos = 0;
    size_t i = 0;
    while (i < src_len) {
        /* 0. Escaped braces: \{{ -> literal {{ and \{ -> literal { */
        if (i + 2 < src_len && src[i] == '\\' && src[i + 1] == '{' && src[i + 2] == '{') {
            if (pos + 3 >= cap) {
                size_t new_cap = cap * 2;
                char *nb = (char *)realloc(out, new_cap);
                if (!nb) {
                    free(out);
                    return false;
                }
                out = nb;
                cap = new_cap;
            }
            out[pos++] = '{';
            out[pos++] = '{';
            i += 3;
            continue;
        }
        if (i + 1 < src_len && src[i] == '\\' && src[i + 1] == '{') {
            if (pos + 2 >= cap) {
                size_t new_cap = cap * 2;
                char *nb = (char *)realloc(out, new_cap);
                if (!nb) {
                    free(out);
                    return false;
                }
                out = nb;
                cap = new_cap;
            }
            out[pos++] = '{';
            i += 2;
            continue;
        }

        /* 1. Double-brace {{ ... }} expressions */
        if (i + 3 < src_len && src[i] == '{' && src[i + 1] == '{') {
            size_t end = i + 2;
            while (end + 1 < src_len && end - (i + 2) < 240 &&
                   !(src[end] == '}' && src[end + 1] == '}')) {
                if (src[end] == '\n')
                    break;
                end++;
            }
            if (end + 1 < src_len && src[end] == '}' && src[end + 1] == '}') {
                char expr[256];
                size_t expr_len = end - (i + 2);
                memcpy(expr, src + i + 2, expr_len);
                expr[expr_len] = '\0';

                char replacement[600];
                int rep_len = evaluate_template_expr(default_pal, dark_pal, light_pal, expr,
                                                     replacement, sizeof(replacement));
                if (rep_len >= 0 && (size_t)rep_len < sizeof(replacement)) {
                    if (pos + (size_t)rep_len + 1 >= cap) {
                        size_t new_cap = (cap + (size_t)rep_len + 4096) * 2;
                        char *nb = (char *)realloc(out, new_cap);
                        if (!nb) {
                            free(out);
                            return false;
                        }
                        out = nb;
                        cap = new_cap;
                    }
                    memcpy(out + pos, replacement, (size_t)rep_len);
                    pos += (size_t)rep_len;
                    i = end + 2;
                    continue;
                }
            }
        }

        /* 2. Single-brace pywal {color0.strip}, {background}, {wallpaper} placeholders */
        if (src[i] == '{' &&
            (i == 0 || (src[i - 1] != '{' && src[i - 1] != '$' && src[i - 1] != '\\')) &&
            (i + 1 < src_len && src[i + 1] != '{')) {
            size_t end = i + 1;
            while (end < src_len && end - (i + 1) < 48 && src[end] != '}' && src[end] != '\n' &&
                   src[end] != '{') {
                end++;
            }
            if (end < src_len && src[end] == '}') {
                char tok[64];
                size_t tlen = end - (i + 1);
                memcpy(tok, src + i + 1, tlen);
                tok[tlen] = '\0';

                if (is_pywal_single_brace_token(tok)) {
                    char replacement[600];
                    int rep_len = evaluate_template_expr(default_pal, dark_pal, light_pal, tok,
                                                         replacement, sizeof(replacement));
                    if (rep_len >= 0 && (size_t)rep_len < sizeof(replacement)) {
                        if (pos + (size_t)rep_len + 1 >= cap) {
                            size_t new_cap = (cap + (size_t)rep_len + 4096) * 2;
                            char *nb = (char *)realloc(out, new_cap);
                            if (!nb) {
                                free(out);
                                return false;
                            }
                            out = nb;
                            cap = new_cap;
                        }
                        memcpy(out + pos, replacement, (size_t)rep_len);
                        pos += (size_t)rep_len;
                        i = end + 1;
                        continue;
                    }
                }
            }
        }

        if (pos + 2 >= cap) {
            size_t new_cap = cap * 2;
            char *nb = (char *)realloc(out, new_cap);
            if (!nb) {
                free(out);
                return false;
            }
            out = nb;
            cap = new_cap;
        }
        out[pos++] = src[i++];
    }

    bool ok = atomic_write_file(out_path, out, pos);
    if (!ok) {
        WAYWAL_LOG_WARN("Failed to write rendered template to '%s': %s", out_path, strerror(errno));
    }
    free(out);
    return ok;
}

static bool is_single_unsafe_template_target_str(const char *cand)
{
    if (!cand || cand[0] == '\0') {
        return true;
    }
    static const char *const sys_prefixes[] = {
        "/etc", "/usr", "/bin", "/sbin", "/lib", "/lib64", "/boot",
        "/proc", "/sys", "/dev", "/var",  "/run", "/root",
    };
    if (strcmp(cand, "/") == 0) {
        return true;
    }
    for (size_t i = 0; i < sizeof(sys_prefixes) / sizeof(sys_prefixes[0]); ++i) {
        size_t plen = strlen(sys_prefixes[i]);
        if (strncmp(cand, sys_prefixes[i], plen) == 0 &&
            (cand[plen] == '\0' || cand[plen] == '/')) {
            return true;
        }
    }

    char home_norm[WAYWAL_PATH_MAX];
    if (waywal_expand_path("~", home_norm, sizeof(home_norm)) && home_norm[0] != '\0') {
        char real_home[WAYWAL_PATH_MAX];
        const char *homes[2] = {
            home_norm,
            realpath(home_norm, real_home) ? real_home : NULL,
        };
        static const char *const home_sensitive[] = {
            ".ssh",  ".gnupg",       ".pki",    ".aws",
            ".kube", ".docker",      ".netrc",  ".password-store",
            ".local/share/keyrings",
        };
        for (size_t h = 0; h < 2; ++h) {
            if (!homes[h] || homes[h][0] == '\0') {
                continue;
            }
            size_t hlen = strlen(homes[h]);
            if (strcmp(cand, homes[h]) == 0) {
                return true;
            }
            /* Reject top-level $HOME dotfiles (e.g., ~/.bashrc, ~/.profile, ~/.zshrc) */
            if (strncmp(cand, homes[h], hlen) == 0 && cand[hlen] == '/' &&
                cand[hlen + 1] == '.' && strchr(cand + hlen + 1, '/') == NULL) {
                return true;
            }
            for (size_t i = 0; i < sizeof(home_sensitive) / sizeof(home_sensitive[0]); ++i) {
                char sub[WAYWAL_PATH_MAX];
                int n = snprintf(sub, sizeof(sub), "%s/%s", homes[h], home_sensitive[i]);
                if (n > 0 && (size_t)n < sizeof(sub)) {
                    if (strncmp(cand, sub, (size_t)n) == 0 &&
                        (cand[n] == '\0' || cand[n] == '/')) {
                        return true;
                    }
                }
            }
        }
    }

    const char *xdg_data = getenv("XDG_DATA_HOME");
    if (xdg_data && xdg_data[0] != '\0') {
        char xdg_keyrings[WAYWAL_PATH_MAX];
        int n = snprintf(xdg_keyrings, sizeof(xdg_keyrings), "%s/keyrings", xdg_data);
        if (n > 0 && (size_t)n < sizeof(xdg_keyrings)) {
            waywal_normalize_path(xdg_keyrings);
            size_t klen = strlen(xdg_keyrings);
            if (strncmp(cand, xdg_keyrings, klen) == 0 &&
                (cand[klen] == '\0' || cand[klen] == '/')) {
                return true;
            }
        }
    }
    return false;
}

static bool is_unsafe_template_target(const char *norm_path)
{
    if (!norm_path || norm_path[0] == '\0') {
        return true;
    }
    if (is_single_unsafe_template_target_str(norm_path)) {
        return true;
    }
    char resolved_path[WAYWAL_PATH_MAX];
    if (resolve_target_or_parent_realpath(norm_path, resolved_path, sizeof(resolved_path))) {
        if (is_single_unsafe_template_target_str(resolved_path)) {
            return true;
        }
    }
    return false;
}

static bool resolve_template_path(const char *raw_path, char *out_buf, size_t out_cap)
{
    if (!raw_path || raw_path[0] == '\0' || !out_buf || out_cap == 0) {
        return false;
    }
    if (raw_path[0] == '~' || strchr(raw_path, '$') != NULL) {
        return waywal_expand_path(raw_path, out_buf, out_cap);
    }
    size_t len = strlen(raw_path);
    if (len >= out_cap) {
        out_buf[0] = '\0';
        return false;
    }
    memcpy(out_buf, raw_path, len + 1);
    if (out_buf[0] == '/') {
        waywal_normalize_path(out_buf);
    }
    return true;
}

bool waywal_palette_render_template_file(const waywal_palette_t *default_pal,
                                         const waywal_palette_t *dark_pal,
                                         const waywal_palette_t *light_pal, const char *input_path,
                                         const char *output_path)
{
    if (!default_pal || !input_path || !output_path || input_path[0] == '\0' ||
        output_path[0] == '\0') {
        return false;
    }

    char exp_in[WAYWAL_PATH_MAX];
    char exp_out[WAYWAL_PATH_MAX];
    if (!resolve_template_path(input_path, exp_in, sizeof(exp_in)) ||
        !resolve_template_path(output_path, exp_out, sizeof(exp_out))) {
        WAYWAL_LOG_WARN("Template path resolution failed for '%s' -> '%s'", input_path,
                        output_path);
        return false;
    }

    if (is_unsafe_template_target(exp_out)) {
        WAYWAL_LOG_WARN("Refusing to write template to sensitive target '%s'", exp_out);
        return false;
    }

    char real_in[WAYWAL_PATH_MAX];
    char real_out[WAYWAL_PATH_MAX];
    bool has_real_in = resolve_target_or_parent_realpath(exp_in, real_in, sizeof(real_in));
    bool has_real_out = resolve_target_or_parent_realpath(exp_out, real_out, sizeof(real_out));

    if (strcmp(exp_in, exp_out) == 0 ||
        (has_real_in && has_real_out && strcmp(real_in, real_out) == 0)) {
        WAYWAL_LOG_WARN("Refusing to clobber template input '%s' with identical output path",
                        exp_in);
        return false;
    }

    struct stat st;
    if (stat(exp_in, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        st.st_size > (4 * 1024 * 1024)) {
        WAYWAL_LOG_WARN("Template input '%s' is missing or invalid", exp_in);
        return false;
    }

    struct stat st_out;
    if (stat(exp_out, &st_out) == 0 && st.st_dev == st_out.st_dev && st.st_ino == st_out.st_ino) {
        WAYWAL_LOG_WARN("Refusing to clobber template input '%s' via hardlink/symlink '%s'", exp_in,
                        exp_out);
        return false;
    }

    int fd = open(exp_in, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        WAYWAL_LOG_WARN("Failed to open template input '%s': %s", exp_in, strerror(errno));
        return false;
    }

    size_t fsize = (size_t)st.st_size;
    char *buf = (char *)malloc(fsize + 1);
    if (!buf) {
        close(fd);
        return false;
    }

    size_t nread_total = 0;
    while (nread_total < fsize) {
        ssize_t r = read(fd, buf + nread_total, fsize - nread_total);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (r == 0)
            break;
        nread_total += (size_t)r;
    }
    close(fd);
    buf[nread_total] = '\0';

    bool ok =
        render_single_template_dual(default_pal, dark_pal, light_pal, buf, nread_total, exp_out);
    free(buf);
    return ok;
}

size_t waywal_palette_apply_configured_templates(const waywal_palette_t *default_pal,
                                                 const waywal_palette_t *dark_pal,
                                                 const waywal_palette_t *light_pal,
                                                 const struct waywal_config *cfg)
{
    if (!default_pal || !cfg) {
        return 0;
    }

    if (cfg->theme.pywal_compat) {
        (void)waywal_palette_export_pywal(default_pal, NULL);
    }

    if (cfg->template_count == 0) {
        return 0;
    }

    waywal_palette_t derived_opp;
    if (!dark_pal || !light_pal) {
        waywal_palette_derive_opposite(default_pal, &derived_opp);
        if (!dark_pal) {
            dark_pal = (default_pal->mode == WAYWAL_THEME_DARK) ? default_pal : &derived_opp;
        }
        if (!light_pal) {
            light_pal = (default_pal->mode == WAYWAL_THEME_LIGHT) ? default_pal : &derived_opp;
        }
    }

    size_t rendered = 0;
    for (size_t i = 0; i < cfg->template_count; ++i) {
        const waywal_template_rule_t *rule = &cfg->templates[i];
        if (!rule->enabled || rule->input_path[0] == '\0' || rule->output_path[0] == '\0') {
            continue;
        }
        if (waywal_palette_render_template_file(default_pal, dark_pal, light_pal, rule->input_path,
                                                rule->output_path)) {
            rendered++;
            if (rule->post_hook[0] != '\0') {
                waywal_palette_run_hook(rule->post_hook);
            }
        }
    }
    return rendered;
}

bool waywal_palette_render_templates(const waywal_palette_t *pal, const char *templates_dir,
                                     const char *out_dir)
{
    if (!pal) {
        return false;
    }

    path_buf_t tpl_path;
    if (templates_dir && templates_dir[0] != '\0') {
        path_buf_from_str(&tpl_path, templates_dir);
    } else {
        const char *xdg_cfg = getenv("XDG_CONFIG_HOME");
        const char *home = getenv("HOME");
        if (xdg_cfg && xdg_cfg[0] != '\0') {
            path_buf_from_str(&tpl_path, xdg_cfg);
            path_buf_push(&tpl_path, "wwal/templates");
        } else if (home && home[0] != '\0') {
            path_buf_from_str(&tpl_path, home);
            path_buf_push(&tpl_path, ".config/wwal/templates");
        } else {
            return true;
        }
    }

    path_buf_t target_dir;
    if (out_dir && out_dir[0] != '\0') {
        path_buf_from_str(&target_dir, out_dir);
    } else {
        const char *env_cache = getenv("WWAL_CACHE_DIR");
        const char *xdg_cache = getenv("XDG_CACHE_HOME");
        const char *home = getenv("HOME");
        if (env_cache && env_cache[0] != '\0') {
            path_buf_from_str(&target_dir, env_cache);
        } else if (xdg_cache && xdg_cache[0] != '\0') {
            path_buf_from_str(&target_dir, xdg_cache);
            path_buf_push(&target_dir, "wwal");
        } else if (home && home[0] != '\0') {
            path_buf_from_str(&target_dir, home);
            path_buf_push(&target_dir, ".cache/wwal");
        } else {
            return false;
        }
    }

    DIR *dir = opendir(tpl_path.data);
    if (!dir) {
        return true;
    }

    waywal_palette_t opp_pal;
    waywal_palette_derive_opposite(pal, &opp_pal);
    const waywal_palette_t *dark_pal = (pal->mode == WAYWAL_THEME_DARK) ? pal : &opp_pal;
    const waywal_palette_t *light_pal = (pal->mode == WAYWAL_THEME_LIGHT) ? pal : &opp_pal;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }

        path_buf_t src_file = tpl_path;
        if (!path_buf_push(&src_file, ent->d_name)) {
            continue;
        }

        path_buf_t dst_file = target_dir;
        if (path_buf_push(&dst_file, ent->d_name)) {
            waywal_palette_render_template_file(pal, dark_pal, light_pal, src_file.data,
                                                dst_file.data);
        }
    }

    closedir(dir);
    return true;
}

bool waywal_palette_run_hook(const char *cmd)
{
    if (!cmd || cmd[0] == '\0') {
        return false;
    }

    static pid_t bg_pids[32];
    for (size_t i = 0; i < 32; ++i) {
        if (bg_pids[i] > 0) {
            if (waitpid(bg_pids[i], NULL, WNOHANG) != 0) {
                bg_pids[i] = 0;
            }
        }
    }

    posix_spawnattr_t attr;
    if (posix_spawnattr_init(&attr) != 0) {
        return false;
    }
    sigset_t empty_mask;
    sigemptyset(&empty_mask);
    posix_spawnattr_setsigmask(&attr, &empty_mask);

    sigset_t def_mask;
    sigemptyset(&def_mask);
    sigaddset(&def_mask, SIGINT);
    sigaddset(&def_mask, SIGTERM);
    sigaddset(&def_mask, SIGQUIT);
    sigaddset(&def_mask, SIGHUP);
    sigaddset(&def_mask, SIGCHLD);
    sigaddset(&def_mask, SIGALRM);
    sigaddset(&def_mask, SIGPIPE);
    posix_spawnattr_setsigdefault(&attr, &def_mask);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        posix_spawnattr_destroy(&attr);
        return false;
    }
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);

    pid_t pid = 0;
    char *const argv[] = {(char *)"sh", (char *)"-c", (char *)cmd, NULL};
    int rc = posix_spawn(&pid, "/bin/sh", &actions, &attr, argv, environ);

    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);

    if (rc != 0) {
        WAYWAL_LOG_WARN("Failed to spawn hook '%s': %s", cmd, strerror(rc));
        return false;
    }

    bool reaped = false;
    for (int attempt = 0; attempt < 250; ++attempt) {
        pid_t w = waitpid(pid, NULL, WNOHANG);
        if (w == pid || (w < 0 && errno != EINTR)) {
            reaped = true;
            break;
        }
        usleep(2000);
    }

    if (!reaped && pid > 0) {
        for (size_t i = 0; i < 32; ++i) {
            if (bg_pids[i] <= 0) {
                bg_pids[i] = pid;
                break;
            }
        }
    }

    return true;
}

static void escape_json_str(const char *src, char *dst, size_t dst_cap)
{
    if (!dst || dst_cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t w = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; ++p) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            if (w + 2 >= dst_cap)
                break;
            dst[w++] = '\\';
            dst[w++] = (char)c;
        } else if (c == '\n') {
            if (w + 2 >= dst_cap)
                break;
            dst[w++] = '\\';
            dst[w++] = 'n';
        } else if (c == '\r') {
            if (w + 2 >= dst_cap)
                break;
            dst[w++] = '\\';
            dst[w++] = 'r';
        } else if (c == '\t') {
            if (w + 2 >= dst_cap)
                break;
            dst[w++] = '\\';
            dst[w++] = 't';
        } else if (c == '\b') {
            if (w + 2 >= dst_cap)
                break;
            dst[w++] = '\\';
            dst[w++] = 'b';
        } else if (c == '\f') {
            if (w + 2 >= dst_cap)
                break;
            dst[w++] = '\\';
            dst[w++] = 'f';
        } else if (c < 0x20U) {
            if (w + 6 >= dst_cap)
                break;
            snprintf(dst + w, dst_cap - w, "\\u%04x", (unsigned int)c);
            w += 6;
        } else {
            if (w + 1 >= dst_cap)
                break;
            dst[w++] = (char)c;
        }
    }
    dst[w] = '\0';
}

static void escape_sh_single_quote(const char *src, char *dst, size_t dst_cap)
{
    if (!dst || dst_cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t w = 0;
    for (const char *p = src; *p != '\0'; ++p) {
        if (*p == '\'') {
            if (w + 4 >= dst_cap)
                break;
            dst[w++] = '\'';
            dst[w++] = '\\';
            dst[w++] = '\'';
            dst[w++] = '\'';
        } else {
            if (w + 1 >= dst_cap)
                break;
            dst[w++] = *p;
        }
    }
    dst[w] = '\0';
}

bool waywal_palette_export_pywal(const waywal_palette_t *pal, const char *override_base_dir)
{
    if (!pal) {
        return false;
    }

    path_buf_t wal_dir;
    if (override_base_dir && override_base_dir[0] != '\0') {
        path_buf_from_str(&wal_dir, override_base_dir);
        path_buf_push(&wal_dir, "wal");
    } else {
        const char *xdg_cache = getenv("XDG_CACHE_HOME");
        const char *home = getenv("HOME");
        if (xdg_cache && xdg_cache[0] != '\0') {
            path_buf_from_str(&wal_dir, xdg_cache);
            path_buf_push(&wal_dir, "wal");
        } else if (home && home[0] != '\0') {
            path_buf_from_str(&wal_dir, home);
            path_buf_push(&wal_dir, ".cache/wal");
        } else {
            return false;
        }
    }

    char wp_json[2048];
    char wp_sh[2048];
    escape_json_str(pal->wallpaper_path, wp_json, sizeof(wp_json));
    escape_sh_single_quote(pal->wallpaper_path, wp_sh, sizeof(wp_sh));

    char pywal_json[8192];
    int jpos = snprintf(pywal_json, sizeof(pywal_json),
                        "{\n"
                        "  \"wallpaper\": \"%s\",\n"
                        "  \"alpha\": \"100\",\n"
                        "  \"special\": {\n"
                        "    \"background\": \"#%02x%02x%02x\",\n"
                        "    \"foreground\": \"#%02x%02x%02x\",\n"
                        "    \"cursor\": \"#%02x%02x%02x\"\n"
                        "  },\n"
                        "  \"colors\": {\n",
                        wp_json, pal->background.r, pal->background.g,
                        pal->background.b, pal->onSurface.r, pal->onSurface.g, pal->onSurface.b,
                        pal->secondary.r, pal->secondary.g, pal->secondary.b);
    if (jpos < 0 || (size_t)jpos >= sizeof(pywal_json)) {
        return false;
    }
    for (int i = 0; i < 16; ++i) {
        const char *comma = (i < 15) ? "," : "";
        int n = snprintf(pywal_json + jpos, sizeof(pywal_json) - (size_t)jpos,
                         "    \"color%d\": \"#%02x%02x%02x\"%s\n", i, pal->term[i].r,
                         pal->term[i].g, pal->term[i].b, comma);
        if (n < 0 || (size_t)jpos + (size_t)n >= sizeof(pywal_json))
            return false;
        jpos += n;
    }
    int n_end = snprintf(pywal_json + jpos, sizeof(pywal_json) - (size_t)jpos, "  }\n}\n");
    if (n_end < 0 || (size_t)jpos + (size_t)n_end >= sizeof(pywal_json))
        return false;
    jpos += n_end;

    char pywal_colors[512];
    int cpos = 0;
    for (int i = 0; i < 16; ++i) {
        int n = snprintf(pywal_colors + cpos, sizeof(pywal_colors) - (size_t)cpos, "#%02x%02x%02x\n",
                         pal->term[i].r, pal->term[i].g, pal->term[i].b);
        if (n < 0 || (size_t)cpos + (size_t)n >= sizeof(pywal_colors))
            return false;
        cpos += n;
    }

    char pywal_sh[8192];
    int spos = snprintf(pywal_sh, sizeof(pywal_sh),
                        "wallpaper='%s'\n"
                        "background='#%02x%02x%02x'\n"
                        "foreground='#%02x%02x%02x'\n"
                        "cursor='#%02x%02x%02x'\n",
                        wp_sh, pal->background.r, pal->background.g,
                        pal->background.b, pal->onSurface.r, pal->onSurface.g, pal->onSurface.b,
                        pal->secondary.r, pal->secondary.g, pal->secondary.b);
    if (spos < 0 || (size_t)spos >= sizeof(pywal_sh))
        return false;
    for (int i = 0; i < 16; ++i) {
        int n = snprintf(pywal_sh + spos, sizeof(pywal_sh) - (size_t)spos,
                         "color%d='#%02x%02x%02x'\n", i, pal->term[i].r, pal->term[i].g,
                         pal->term[i].b);
        if (n < 0 || (size_t)spos + (size_t)n >= sizeof(pywal_sh))
            return false;
        spos += n;
    }

    char pywal_css[8192];
    int csspos = snprintf(pywal_css, sizeof(pywal_css),
                          ":root {\n"
                          "  --wallpaper: url(\"%s\");\n"
                          "  --background: #%02x%02x%02x;\n"
                          "  --foreground: #%02x%02x%02x;\n"
                          "  --cursor: #%02x%02x%02x;\n",
                          wp_json, pal->background.r, pal->background.g,
                          pal->background.b, pal->onSurface.r, pal->onSurface.g, pal->onSurface.b,
                          pal->secondary.r, pal->secondary.g, pal->secondary.b);
    if (csspos < 0 || (size_t)csspos >= sizeof(pywal_css))
        return false;
    for (int i = 0; i < 16; ++i) {
        int n = snprintf(pywal_css + csspos, sizeof(pywal_css) - (size_t)csspos,
                         "  --color%d: #%02x%02x%02x;\n", i, pal->term[i].r, pal->term[i].g,
                         pal->term[i].b);
        if (n < 0 || (size_t)csspos + (size_t)n >= sizeof(pywal_css))
            return false;
        csspos += n;
    }
    int n_css_end = snprintf(pywal_css + csspos, sizeof(pywal_css) - (size_t)csspos, "}\n");
    if (n_css_end < 0 || (size_t)csspos + (size_t)n_css_end >= sizeof(pywal_css))
        return false;
    csspos += n_css_end;

    char seq_buf[2048];
    size_t seq_len = waywal_palette_format_sequences(pal, seq_buf, sizeof(seq_buf));

    path_buf_t fpath;

    fpath = wal_dir;
    if (!path_buf_push(&fpath, "colors.json") ||
        !atomic_write_file(fpath.data, pywal_json, (size_t)jpos)) {
        return false;
    }

    fpath = wal_dir;
    if (!path_buf_push(&fpath, "colors") ||
        !atomic_write_file(fpath.data, pywal_colors, (size_t)cpos)) {
        return false;
    }

    if (seq_len > 0) {
        fpath = wal_dir;
        if (!path_buf_push(&fpath, "sequences") ||
            !atomic_write_file(fpath.data, seq_buf, seq_len)) {
            return false;
        }
    }

    fpath = wal_dir;
    if (!path_buf_push(&fpath, "colors.sh") ||
        !atomic_write_file(fpath.data, pywal_sh, (size_t)spos)) {
        return false;
    }

    fpath = wal_dir;
    if (!path_buf_push(&fpath, "colors.css") ||
        !atomic_write_file(fpath.data, pywal_css, (size_t)csspos)) {
        return false;
    }

    return true;
}

bool waywal_palette_export_filtered(const waywal_palette_t *pal, const char *override_base_dir,
                                    bool caelestia_sync, bool export_css, bool export_hyprland)
{
    if (!pal) {
        return false;
    }

    named_color_slot_t slots[128];
    size_t n_slots = collect_color_slots(pal, slots);

    char scheme_json[16384];
    char seq_txt[2048];
    char colors_json[16384];
    char colors_css[16384];
    char colors_hypr[16384];

    size_t scheme_len =
        format_caelestia_json(pal, slots, n_slots, scheme_json, sizeof(scheme_json));
    size_t seq_len = waywal_palette_format_sequences(pal, seq_txt, sizeof(seq_txt));
    size_t cjson_len =
        format_wwal_colors_json(pal, slots, n_slots, colors_json, sizeof(colors_json));
    size_t ccss_len = format_wwal_css(slots, n_slots, colors_css, sizeof(colors_css));
    size_t chypr_len = format_wwal_hyprland(slots, n_slots, colors_hypr, sizeof(colors_hypr));

    if (!scheme_len || !seq_len || !cjson_len || !ccss_len || !chypr_len) {
        return false;
    }

    const char *env_state = getenv("WWAL_STATE_DIR");
    const char *env_cache = getenv("WWAL_CACHE_DIR");

    path_buf_t state_dir;
    path_buf_t cache_dir;

    if (override_base_dir && override_base_dir[0] != '\0') {
        path_buf_from_str(&state_dir, override_base_dir);
        path_buf_push(&state_dir, "caelestia");
        path_buf_from_str(&cache_dir, override_base_dir);
        path_buf_push(&cache_dir, "wwal");

        path_buf_t flat;
        if (caelestia_sync) {
            path_buf_from_str(&flat, override_base_dir);
            path_buf_push(&flat, "scheme.json");
            atomic_write_file(flat.data, scheme_json, scheme_len);

            path_buf_from_str(&flat, override_base_dir);
            path_buf_push(&flat, "sequences.txt");
            atomic_write_file(flat.data, seq_txt, seq_len);
        }

        path_buf_from_str(&flat, override_base_dir);
        path_buf_push(&flat, "colors.json");
        atomic_write_file(flat.data, colors_json, cjson_len);

        if (export_css) {
            path_buf_from_str(&flat, override_base_dir);
            path_buf_push(&flat, "colors.css");
            atomic_write_file(flat.data, colors_css, ccss_len);
        }

        if (export_hyprland) {
            path_buf_from_str(&flat, override_base_dir);
            path_buf_push(&flat, "colors-hyprland.conf");
            atomic_write_file(flat.data, colors_hypr, chypr_len);
        }
    } else {
        const char *home = getenv("HOME");
        if (env_state && env_state[0] != '\0') {
            path_buf_from_str(&state_dir, env_state);
        } else {
            const char *xdg_state = getenv("XDG_STATE_HOME");
            if (xdg_state && xdg_state[0] != '\0') {
                path_buf_from_str(&state_dir, xdg_state);
                path_buf_push(&state_dir, "caelestia");
            } else if (home && home[0] != '\0') {
                path_buf_from_str(&state_dir, home);
                path_buf_push(&state_dir, ".local/state/caelestia");
            } else {
                return false;
            }
        }

        if (env_cache && env_cache[0] != '\0') {
            path_buf_from_str(&cache_dir, env_cache);
        } else {
            const char *xdg_cache = getenv("XDG_CACHE_HOME");
            if (xdg_cache && xdg_cache[0] != '\0') {
                path_buf_from_str(&cache_dir, xdg_cache);
                path_buf_push(&cache_dir, "wwal");
            } else if (home && home[0] != '\0') {
                path_buf_from_str(&cache_dir, home);
                path_buf_push(&cache_dir, ".cache/wwal");
            } else {
                return false;
            }
        }
    }

    path_buf_t target;

    if (caelestia_sync) {
        target = state_dir;
        if (!path_buf_push(&target, "scheme.json") ||
            !atomic_write_file(target.data, scheme_json, scheme_len)) {
            return false;
        }

        target = state_dir;
        if (!path_buf_push(&target, "sequences.txt") ||
            !atomic_write_file(target.data, seq_txt, seq_len)) {
            return false;
        }
    }

    target = cache_dir;
    if (!path_buf_push(&target, "colors.json") ||
        !atomic_write_file(target.data, colors_json, cjson_len)) {
        return false;
    }

    if (export_css) {
        target = cache_dir;
        if (!path_buf_push(&target, "colors.css") ||
            !atomic_write_file(target.data, colors_css, ccss_len)) {
            return false;
        }
    }

    if (export_hyprland) {
        target = cache_dir;
        if (!path_buf_push(&target, "colors-hyprland.conf") ||
            !atomic_write_file(target.data, colors_hypr, chypr_len)) {
            return false;
        }
    }

    if (override_base_dir && override_base_dir[0] != '\0') {
        path_buf_t tpl_override;
        path_buf_from_str(&tpl_override, override_base_dir);
        if (path_buf_push(&tpl_override, "templates")) {
            waywal_palette_render_templates(pal, tpl_override.data, cache_dir.data);
            waywal_palette_render_templates(pal, tpl_override.data, override_base_dir);
        }
    } else {
        waywal_palette_render_templates(pal, NULL, cache_dir.data);
    }

    return true;
}

bool waywal_palette_export_all(const waywal_palette_t *pal, const char *override_base_dir)
{
    if (!waywal_palette_export_filtered(pal, override_base_dir, true, true, true)) {
        return false;
    }
    (void)waywal_palette_export_pywal(pal, override_base_dir);
    return true;
}

static bool is_numeric_name(const char *s)
{
    if (!s || *s == '\0') {
        return false;
    }
    for (const char *p = s; *p != '\0'; ++p) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

bool waywal_palette_broadcast_tty(const waywal_palette_t *pal)
{
    if (!pal) {
        return false;
    }

    char seq_buf[2048];
    size_t seq_len = waywal_palette_format_sequences(pal, seq_buf, sizeof(seq_buf));
    if (seq_len == 0) {
        return false;
    }

    DIR *dir = opendir("/dev/pts");
    if (!dir) {
        return false;
    }

    uid_t my_uid = getuid();
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!is_numeric_name(ent->d_name)) {
            continue;
        }

        char pty_path[128];
        int n = snprintf(pty_path, sizeof(pty_path), "/dev/pts/%s", ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(pty_path)) {
            continue;
        }

        struct stat st;
        if (stat(pty_path, &st) != 0 || st.st_uid != my_uid) {
            continue;
        }

        int fd = open(pty_path, O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        ssize_t ignored = write(fd, seq_buf, seq_len);
        (void)ignored;
        close(fd);
    }

    closedir(dir);
    return true;
}
