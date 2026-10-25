#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/palette_hct.h"
#include "waywal/path.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <immintrin.h>
#include <math.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char **environ;

#define WU_BINS 32
#define WU_GRID 33
#define WU_MAX_BOXES 64
#define KMEANS_K 16
#define KMEANS_ITERS 6
#define SUBSAMPLE_DIM 128

#define PI_F 3.14159265358979323846f
#define TWO_PI_F 6.28318530717958647692f
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
} wu_hist_t;

typedef struct {
    float l;
    float a;
    float b;
    float weight;
} weighted_point_t;

static _Thread_local wu_hist_t tls_wu_hist;
static float g_srgb_to_linear[256];
static uint8_t g_linear_to_srgb_lut[4097];
static atomic_bool g_srgb_lut_ready = false;

static inline int clamp_i(int x, int lo, int hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static void ensure_srgb_lut(void) {
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

__attribute__((constructor)) static void init_palette_lut_ctor(void) {
    ensure_srgb_lut();
}

static inline float clamp_f(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline float wrap_radians(float h) {
    h = fmodf(h, TWO_PI_F);
    if (h < 0.0f) {
        h += TWO_PI_F;
    }
    return h;
}

static inline float hue_diff_deg(float h1_deg, float h2_deg) {
    float d = fabsf(h1_deg - h2_deg);
    if (d > 180.0f) {
        d = 360.0f - d;
    }
    return d;
}

static inline float fast_cbrtf_scalar(float x) {
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

static inline uint8_t linear_to_srgb_u8(float c) {
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

static inline waywal_oklab_t linear_rgb_to_oklab(float r, float g, float b) {
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
                                       float *out_b) {
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

static inline bool linear_rgb_in_gamut(float r, float g, float b) {
    const float eps = 1e-4f;
    return (r >= -eps && r <= 1.0f + eps && g >= -eps && g <= 1.0f + eps && b >= -eps &&
            b <= 1.0f + eps);
}

#ifdef __AVX2__
static inline __m256 avx2_cbrt_ps(__m256 x) {
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

static inline void wu_accum_pixel(wu_hist_t *hist, float l, float a, float b) {
    int bl = clamp_i((int)(l * (float)WU_BINS) + 1, 1, WU_BINS);
    int ba = clamp_i((int)(((a + 0.4f) * (1.0f / 0.8f)) * (float)WU_BINS) + 1, 1, WU_BINS);
    int bb = clamp_i((int)(((b + 0.4f) * (1.0f / 0.8f)) * (float)WU_BINS) + 1, 1, WU_BINS);

    wu_bin_t *bin = &hist->bins[bl][ba][bb];
    bin->w += 1.0f;
    bin->l += l;
    bin->a += a;
    bin->b += b;
    bin->m2 += (l * l + a * a + b * b);
}

static size_t wu_populate_histogram(wu_hist_t *hist, const uint8_t *pixels, uint32_t width,
                                    uint32_t height, uint32_t stride_bytes, bool is_bgra) {
    memset(hist, 0, sizeof(*hist));

    uint32_t step_x = width > SUBSAMPLE_DIM ? (width / SUBSAMPLE_DIM) : 1U;
    uint32_t step_y = height > SUBSAMPLE_DIM ? (height / SUBSAMPLE_DIM) : 1U;
    if (step_x == 0U) step_x = 1U;
    if (step_y == 0U) step_y = 1U;

    size_t valid_samples = 0;
    const int r_off = is_bgra ? 2 : 0;
    const int b_off = is_bgra ? 0 : 2;

#ifdef __AVX2__
    int32_t r_idx[8] __attribute__((aligned(32)));
    int32_t g_idx[8] __attribute__((aligned(32)));
    int32_t b_idx[8] __attribute__((aligned(32)));
    float out_l[8] __attribute__((aligned(32)));
    float out_a[8] __attribute__((aligned(32)));
    float out_b[8] __attribute__((aligned(32)));
    int batch_count = 0;
#endif

    for (uint32_t y = 0; y < height; y += step_y) {
        const uint8_t *row = pixels + (size_t)y * stride_bytes;
        for (uint32_t x = 0; x < width; x += step_x) {
            const uint8_t *px = row + (size_t)x * 4U;
            if (px[3] == 0U) {
                continue;
            }
            valid_samples++;

#ifdef __AVX2__
            r_idx[batch_count] = px[r_off];
            g_idx[batch_count] = px[1];
            b_idx[batch_count] = px[b_off];
            batch_count++;

            if (batch_count == 8) {
                __m256i vr_i = _mm256_load_si256((const __m256i *)r_idx);
                __m256i vg_i = _mm256_load_si256((const __m256i *)g_idx);
                __m256i vb_i = _mm256_load_si256((const __m256i *)b_idx);

                __m256 vr = _mm256_i32gather_ps(g_srgb_to_linear, vr_i, 4);
                __m256 vg = _mm256_i32gather_ps(g_srgb_to_linear, vg_i, 4);
                __m256 vb = _mm256_i32gather_ps(g_srgb_to_linear, vb_i, 4);

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
#else
            float r = g_srgb_to_linear[px[r_off]];
            float g = g_srgb_to_linear[px[1]];
            float b = g_srgb_to_linear[px[b_off]];
            waywal_oklab_t ok = linear_rgb_to_oklab(r, g, b);
            wu_accum_pixel(hist, ok.l, ok.a, ok.b);
#endif
        }
    }

#ifdef __AVX2__
    for (int i = 0; i < batch_count; ++i) {
        float r = g_srgb_to_linear[r_idx[i]];
        float g = g_srgb_to_linear[g_idx[i]];
        float b = g_srgb_to_linear[b_idx[i]];
        waywal_oklab_t ok = linear_rgb_to_oklab(r, g, b);
        wu_accum_pixel(hist, ok.l, ok.a, ok.b);
    }
#endif

    return valid_samples;
}

static void wu_compute_moments(wu_hist_t *hist) {
#ifdef __AVX2__
    for (int l = 1; l <= WU_BINS; ++l) {
        __m256 area[WU_GRID];
        for (int b = 0; b < WU_GRID; ++b) {
            area[b] = _mm256_setzero_ps();
        }

        for (int a = 1; a <= WU_BINS; ++a) {
            __m256 line = _mm256_setzero_ps();
            for (int b = 1; b <= WU_BINS; ++b) {
                __m256 cur = _mm256_load_ps(&hist->bins[l][a][b].w);
                line = _mm256_add_ps(line, cur);
                area[b] = _mm256_add_ps(area[b], line);
                __m256 prev_l = _mm256_load_ps(&hist->bins[l - 1][a][b].w);
                _mm256_store_ps(&hist->bins[l][a][b].w, _mm256_add_ps(prev_l, area[b]));
            }
        }
    }
#else
    for (int l = 1; l <= WU_BINS; ++l) {
        wu_bin_t area[WU_GRID];
        memset(area, 0, sizeof(area));

        for (int a = 1; a <= WU_BINS; ++a) {
            float line_w = 0.0f;
            float line_l = 0.0f;
            float line_a = 0.0f;
            float line_b = 0.0f;
            float line_2 = 0.0f;

            for (int b = 1; b <= WU_BINS; ++b) {
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
#endif
}

static inline wu_bin_t wu_vol_bin(const wu_box_t *box, const wu_hist_t *hist) {
    wu_bin_t out;
#ifdef __AVX2__
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

static inline wu_bin_t wu_bottom_bin(const wu_box_t *box, int dir, const wu_hist_t *hist) {
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
#ifdef __AVX2__
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

static inline wu_bin_t wu_top_bin(const wu_box_t *box, int dir, int pos, const wu_hist_t *hist) {
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
#ifdef __AVX2__
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

static float wu_box_variance(const wu_hist_t *hist, const wu_box_t *box) {
    wu_bin_t vol = wu_vol_bin(box, hist);
    if (vol.w <= 0.0f) {
        return 0.0f;
    }
    float v = vol.m2 - (vol.l * vol.l + vol.a * vol.a + vol.b * vol.b) / vol.w;
    return v > 0.0f ? v : 0.0f;
}

static float wu_maximize(const wu_hist_t *hist, const wu_box_t *box, int dir, int first, int last,
                         int *out_cut, float whole_w, float whole_l, float whole_a,
                         float whole_b) {
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

static bool wu_cut(const wu_hist_t *hist, wu_box_t *set1, wu_box_t *set2) {
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

static size_t wu_partition(const wu_hist_t *hist, weighted_point_t *out_points, size_t max_boxes) {
    wu_box_t boxes[WU_MAX_BOXES];
    memset(boxes, 0, sizeof(boxes));

    boxes[0].l0 = 0;
    boxes[0].l1 = WU_BINS;
    boxes[0].a0 = 0;
    boxes[0].a1 = WU_BINS;
    boxes[0].b0 = 0;
    boxes[0].b1 = WU_BINS;
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

static inline float oklab_dist_sq(float l1, float a1, float b1, float l2, float a2, float b2) {
    float dl = l1 - l2;
    float da = a1 - a2;
    float db = b1 - b2;
    return dl * dl + da * da + db * db;
}

static size_t weighted_kmeans_pp(const weighted_point_t *pts, size_t n_pts,
                                 weighted_point_t *centroids, size_t max_k) {
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

    return k_target;
}

/* Algebraic CAM16 correlates under standard sRGB D65 viewing conditions (Li et al. 2017). */
static waywal_cam16_t oklab_to_cam16(float l, float a, float b) {
    float r_lin, g_lin, b_lin;
    oklab_to_linear_rgb(l, a, b, &r_lin, &g_lin, &b_lin);
    r_lin = clamp_f(r_lin, 0.0f, 1.0f);
    g_lin = clamp_f(g_lin, 0.0f, 1.0f);
    b_lin = clamp_f(b_lin, 0.0f, 1.0f);

    float x = (0.41233895f * r_lin + 0.35762064f * g_lin + 0.18051042f * b_lin) * 100.0f;
    float y = (0.2126f * r_lin + 0.7152f * g_lin + 0.0722f * b_lin) * 100.0f;
    float z = (0.01932141f * r_lin + 0.11916382f * g_lin + 0.95034478f * b_lin) * 100.0f;

    float r_c = 0.401288f * x + 0.650173f * y - 0.051461f * z;
    float g_c = -0.250268f * x + 1.204414f * y + 0.045854f * z;
    float b_c = -0.002079f * x + 0.048952f * y + 0.953127f * z;

    const float d_r = 1.0211777f;
    const float d_g = 0.9863077f;
    const float d_b = 0.9339605f;
    const float fl = 0.38848145f;

    float r_d = fabsf(r_c * d_r);
    float g_d = fabsf(g_c * d_g);
    float b_d = fabsf(b_c * d_b);

    float p_r = powf(fl * r_d * 0.01f, 0.42f);
    float p_g = powf(fl * g_d * 0.01f, 0.42f);
    float p_b = powf(fl * b_d * 0.01f, 0.42f);

    float r_a = 400.0f * p_r / (p_r + 27.13f);
    float g_a = 400.0f * p_g / (p_g + 27.13f);
    float b_a = 400.0f * p_b / (p_b + 27.13f);

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

static float tone_to_oklab_l(float l_target) {
    if (l_target <= 0.0f) {
        return 0.0f;
    }
    if (l_target <= 1.0f) {
        return l_target;
    }
    if (l_target >= 100.0f) {
        return 1.0f;
    }
    /* Convert Material 3 CIE L* tone (0..100) to relative luminance Y, then to OKLab L = Y^(1/3). */
    float f = (l_target + 16.0f) * (1.0f / 116.0f);
    float y = (l_target > 8.0f) ? (f * f * f) : (l_target * (1.0f / 903.296296f));
    return fast_cbrtf_scalar(y);
}

static inline void oklab_ray_to_linear_rgb(float l_ok, float c, float dl, float dm, float ds,
                                           float *out_r, float *out_g, float *out_b) {
    float l_ = l_ok + c * dl;
    float m_ = l_ok + c * dm;
    float s_ = l_ok + c * ds;

    float l = l_ * l_ * l_;
    float m = m_ * m_ * m_;
    float s = s_ * s_ * s_;

    *out_r = +4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    *out_g = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    *out_b = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
}

color_rgba_t waywal_palette_generate_tone(float l_target, float chroma, float hue_rad) {
    if (l_target <= 0.0f) {
        return (color_rgba_t){.r = 0, .g = 0, .b = 0, .a = 255};
    }
    if (l_target >= 100.0f) {
        return (color_rgba_t){.r = 255, .g = 255, .b = 255, .a = 255};
    }

    float l_ok = tone_to_oklab_l(l_target);
    if (l_ok <= 0.0f) {
        return (color_rgba_t){.r = 0, .g = 0, .b = 0, .a = 255};
    }
    if (l_ok >= 1.0f) {
        return (color_rgba_t){.r = 255, .g = 255, .b = 255, .a = 255};
    }

    float target_c = chroma > 0.0f ? chroma : 0.0f;
    float cos_h = cosf(hue_rad);
    float sin_h = sinf(hue_rad);

    /* Pre-project unit hue vector (cos_h, sin_h) onto LMS cube-root axes once per tone. */
    float dl = +0.3963377774f * cos_h + 0.2158037573f * sin_h;
    float dm = -0.1055613458f * cos_h - 0.0638541728f * sin_h;
    float ds = -0.0894841775f * cos_h - 1.2914855480f * sin_h;

    float r, g, b;
    oklab_ray_to_linear_rgb(l_ok, target_c, dl, dm, ds, &r, &g, &b);

    if (!linear_rgb_in_gamut(r, g, b)) {
        float lo = 0.0f;
        float hi = target_c;
        float best_r = l_ok * l_ok * l_ok;
        float best_g = best_r;
        float best_b = best_r;
        for (int i = 0; i < 6; ++i) {
            float mid = 0.5f * (lo + hi);
            float mr, mg, mb;
            oklab_ray_to_linear_rgb(l_ok, mid, dl, dm, ds, &mr, &mg, &mb);
            if (linear_rgb_in_gamut(mr, mg, mb)) {
                lo = mid;
                best_r = mr;
                best_g = mg;
                best_b = mb;
            } else {
                hi = mid;
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

static color_rgba_t harmonize_semantic_color(float base_hue_deg, float base_chroma, float tone,
                                             float primary_hue_rad, float primary_chroma) {
    float primary_deg = wrap_radians(primary_hue_rad) * RAD_TO_DEG_F;
    float diff = primary_deg - base_hue_deg;
    while (diff > 180.0f) diff -= 360.0f;
    while (diff < -180.0f) diff += 360.0f;

    float max_rot = 22.0f;
    float shift = clamp_f(diff * 0.35f, -max_rot, max_rot);
    float h_rad = wrap_radians((base_hue_deg + shift) * DEG_TO_RAD_F);
    float c = clamp_f(0.75f * base_chroma + 0.25f * primary_chroma, 0.04f, 0.22f);
    return waywal_palette_generate_tone(tone, c, h_rad);
}

static void synthesize_m3_scheme(waywal_palette_t *pal, waywal_theme_mode_t mode, bool is_mono,
                                 waywal_oklch_t p_seed, waywal_oklch_t s_seed,
                                 waywal_oklch_t t_seed, waywal_oklch_t n_seed,
                                 waywal_oklch_t nv_seed) {
    memset(pal, 0, sizeof(*pal));
    pal->mode = mode;
    pal->is_monochrome = is_mono;
    pal->primary_seed = p_seed;
    pal->secondary_seed = s_seed;
    pal->tertiary_seed = t_seed;
    pal->neutral_seed = n_seed;
    pal->neutral_variant_seed = nv_seed;

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
        pal->background = waywal_palette_generate_tone(6.0f, n_seed.c, n_seed.h_rad);
        pal->onBackground = waywal_palette_generate_tone(90.0f, n_seed.c, n_seed.h_rad);
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

        pal->primary = pal->primaryFixedDim;
        pal->surfaceTint = pal->primary;
        pal->onPrimary = waywal_palette_generate_tone(20.0f, p_seed.c, p_seed.h_rad);
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
        pal->onBackground = waywal_palette_generate_tone(10.0f, n_seed.c, n_seed.h_rad);
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

        pal->primary = pal->primary_paletteKeyColor;
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
        pal->term[1 + i] = harmonize_semantic_color(ansi_hues[i], 0.14f, base_tone, p_seed.h_rad,
                                                    p_seed.c);
        pal->term[9 + i] = harmonize_semantic_color(ansi_hues[i], 0.16f, bright_tone, p_seed.h_rad,
                                                    p_seed.c);
    }

    float cat_tone = (mode == WAYWAL_THEME_DARK) ? 82.0f : 44.0f;
    pal->rosewater = harmonize_semantic_color(15.0f, 0.06f, cat_tone + 8.0f, p_seed.h_rad, p_seed.c);
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
    pal->lavender = harmonize_semantic_color(275.0f, 0.11f, cat_tone + 3.0f, p_seed.h_rad, p_seed.c);

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
}

static bool extract_dual_impl(const uint8_t *pixels, uint32_t width, uint32_t height,
                              uint32_t stride_bytes, bool is_bgra, waywal_palette_t *out_dark,
                              waywal_palette_t *out_light) {
    if (!pixels || width == 0U || height == 0U || (!out_dark && !out_light)) {
        return false;
    }
    if (stride_bytes < width * 4U) {
        stride_bytes = width * 4U;
    }

    ensure_srgb_lut();

    size_t valid_samples = wu_populate_histogram(&tls_wu_hist, pixels, width, height,
                                                 stride_bytes, is_bgra);
    if (valid_samples == 0) {
        waywal_oklch_t def_p = {.l = 0.6f, .c = 0.02f, .h_rad = 4.5f};
        waywal_oklch_t def_s = {.l = 0.6f, .c = 0.015f, .h_rad = 4.5f};
        waywal_oklch_t def_t = {.l = 0.6f, .c = 0.02f, .h_rad = 5.5f};
        waywal_oklch_t def_n = {.l = 0.6f, .c = 0.01f, .h_rad = 4.5f};
        waywal_oklch_t def_nv = {.l = 0.6f, .c = 0.02f, .h_rad = 4.5f};
        if (out_dark) {
            synthesize_m3_scheme(out_dark, WAYWAL_THEME_DARK, true, def_p, def_s, def_t, def_n,
                                 def_nv);
        }
        if (out_light) {
            synthesize_m3_scheme(out_light, WAYWAL_THEME_LIGHT, true, def_p, def_s, def_t, def_n,
                                 def_nv);
        }
        return true;
    }

    wu_compute_moments(&tls_wu_hist);

    weighted_point_t wu_boxes[WU_MAX_BOXES];
    size_t n_boxes = wu_partition(&tls_wu_hist, wu_boxes, WU_MAX_BOXES);

    weighted_point_t clusters[KMEANS_K];
    size_t n_clusters = weighted_kmeans_pp(wu_boxes, n_boxes, clusters, KMEANS_K);

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

    bool is_mono = (max_c_ok < 0.018f);
    waywal_oklch_t p_seed, s_seed, t_seed, n_seed, nv_seed;

    if (is_mono || n_clusters == 0) {
        float h = (n_clusters > 0 && cands[0].c_ok > 1e-4f) ? cands[0].h_rad : (250.0f * DEG_TO_RAD_F);
        p_seed = (waywal_oklch_t){.l = 0.65f, .c = 0.02f, .h_rad = h};
        s_seed = (waywal_oklch_t){.l = 0.65f, .c = 0.015f, .h_rad = wrap_radians(h + 60.0f * DEG_TO_RAD_F)};
        t_seed = (waywal_oklch_t){.l = 0.65f, .c = 0.02f, .h_rad = wrap_radians(h + 120.0f * DEG_TO_RAD_F)};
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

    if (out_dark) {
        synthesize_m3_scheme(out_dark, WAYWAL_THEME_DARK, is_mono, p_seed, s_seed, t_seed, n_seed,
                             nv_seed);
    }
    if (out_light) {
        synthesize_m3_scheme(out_light, WAYWAL_THEME_LIGHT, is_mono, p_seed, s_seed, t_seed,
                             n_seed, nv_seed);
    }
    return true;
}

bool waywal_palette_extract_dual(const uint8_t *rgba_pixels, uint32_t width, uint32_t height,
                                 uint32_t stride_bytes, waywal_palette_t *out_dark,
                                 waywal_palette_t *out_light) {
    return extract_dual_impl(rgba_pixels, width, height, stride_bytes, false, out_dark, out_light);
}

bool waywal_palette_extract_dual_bgra(const uint8_t *bgra_pixels, uint32_t width, uint32_t height,
                                      uint32_t stride_bytes, waywal_palette_t *out_dark,
                                      waywal_palette_t *out_light) {
    return extract_dual_impl(bgra_pixels, width, height, stride_bytes, true, out_dark, out_light);
}

bool waywal_palette_extract(const uint8_t *rgba_pixels, uint32_t width, uint32_t height,
                            uint32_t stride_bytes, waywal_theme_mode_t mode,
                            waywal_palette_t *out_palette) {
    if (!out_palette) {
        return false;
    }
    if (mode == WAYWAL_THEME_LIGHT) {
        return extract_dual_impl(rgba_pixels, width, height, stride_bytes, false, NULL,
                                 out_palette);
    }
    return extract_dual_impl(rgba_pixels, width, height, stride_bytes, false, out_palette, NULL);
}

bool waywal_palette_extract_bgra(const uint8_t *bgra_pixels, uint32_t width, uint32_t height,
                                 uint32_t stride_bytes, waywal_theme_mode_t mode,
                                 waywal_palette_t *out_palette) {
    if (!out_palette) {
        return false;
    }
    if (mode == WAYWAL_THEME_LIGHT) {
        return extract_dual_impl(bgra_pixels, width, height, stride_bytes, true, NULL, out_palette);
    }
    return extract_dual_impl(bgra_pixels, width, height, stride_bytes, true, out_palette, NULL);
}

typedef struct {
    const char *name;
    color_rgba_t color;
} named_color_slot_t;

static size_t collect_color_slots(const waywal_palette_t *pal, named_color_slot_t *slots) {
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

size_t waywal_palette_format_sequences(const waywal_palette_t *pal, char *out_buf, size_t buf_cap) {
    if (!pal || !out_buf || buf_cap == 0) {
        return 0;
    }

    size_t pos = 0;
#define APPEND_OSC(code_str, col)                                                                  \
    do {                                                                                           \
        int w = snprintf(out_buf + pos, (pos < buf_cap) ? (buf_cap - pos) : 0,                     \
                         "\033]" code_str ";rgb:%02x/%02x/%02x\033\\", (col).r, (col).g, (col).b); \
        if (w < 0) return 0;                                                                       \
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
        if (w < 0) return 0;
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

static bool mkdir_parents(const char *file_path) {
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

static bool atomic_write_file(const char *target_path, const char *data, size_t len) {
    if (!target_path || !data) {
        return false;
    }
    if (!mkdir_parents(target_path)) {
        return false;
    }

    char tmp_path[WAYWAL_PATH_MAX];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", target_path, (int)getpid());
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
            if (errno == EINTR) continue;
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
    if (rename(tmp_path, target_path) != 0) {
        unlink(tmp_path);
        return false;
    }
    return true;
}

static size_t format_caelestia_json(const waywal_palette_t *pal, const named_color_slot_t *slots,
                                    size_t n_slots, char *buf, size_t cap) {
    const char *mode_str = (pal->mode == WAYWAL_THEME_LIGHT) ? "light" : "dark";
    int n = snprintf(buf, cap,
                     "{\"name\": \"dynamic\", \"flavour\": \"default\", \"mode\": \"%s\", "
                     "\"variant\": \"tonalspot\", \"colours\": {",
                     mode_str);
    if (n < 0 || (size_t)n >= cap) return 0;
    size_t pos = (size_t)n;

    for (size_t i = 0; i < n_slots; ++i) {
        const char *sep = (i + 1 < n_slots) ? ", " : "";
        n = snprintf(buf + pos, cap - pos, "\"%s\": \"%02x%02x%02x\"%s", slots[i].name,
                     slots[i].color.r, slots[i].color.g, slots[i].color.b, sep);
        if (n < 0 || pos + (size_t)n >= cap) return 0;
        pos += (size_t)n;
    }

    n = snprintf(buf + pos, cap - pos, "}}");
    if (n < 0 || pos + (size_t)n >= cap) return 0;
    pos += (size_t)n;
    return pos;
}

static size_t format_wwal_colors_json(const waywal_palette_t *pal, const named_color_slot_t *slots,
                                      size_t n_slots, char *buf, size_t cap) {
    const char *mode_str = (pal->mode == WAYWAL_THEME_LIGHT) ? "light" : "dark";
    int n = snprintf(buf, cap, "{\n  \"mode\": \"%s\",\n  \"colors\": {\n", mode_str);
    if (n < 0 || (size_t)n >= cap) return 0;
    size_t pos = (size_t)n;

    for (size_t i = 0; i < n_slots; ++i) {
        const char *comma = (i + 1 < n_slots) ? "," : "";
        n = snprintf(buf + pos, cap - pos, "    \"%s\": \"#%02x%02x%02x\"%s\n", slots[i].name,
                     slots[i].color.r, slots[i].color.g, slots[i].color.b, comma);
        if (n < 0 || pos + (size_t)n >= cap) return 0;
        pos += (size_t)n;
    }

    n = snprintf(buf + pos, cap - pos, "  }\n}\n");
    if (n < 0 || pos + (size_t)n >= cap) return 0;
    pos += (size_t)n;
    return pos;
}

static size_t format_wwal_css(const named_color_slot_t *slots, size_t n_slots, char *buf,
                              size_t cap) {
    int n = snprintf(buf, cap, ":root {\n");
    if (n < 0 || (size_t)n >= cap) return 0;
    size_t pos = (size_t)n;

    for (size_t i = 0; i < n_slots; ++i) {
        n = snprintf(buf + pos, cap - pos, "  --%s: #%02x%02x%02x;\n", slots[i].name,
                     slots[i].color.r, slots[i].color.g, slots[i].color.b);
        if (n < 0 || pos + (size_t)n >= cap) return 0;
        pos += (size_t)n;
    }

    n = snprintf(buf + pos, cap - pos, "}\n");
    if (n < 0 || pos + (size_t)n >= cap) return 0;
    pos += (size_t)n;
    return pos;
}

static size_t format_wwal_hyprland(const named_color_slot_t *slots, size_t n_slots, char *buf,
                                   size_t cap) {
    size_t pos = 0;
    for (size_t i = 0; i < n_slots; ++i) {
        int n = snprintf(buf + pos, cap - pos, "$%s = rgba(%02x%02x%02xff)\n", slots[i].name,
                         slots[i].color.r, slots[i].color.g, slots[i].color.b);
        if (n < 0 || pos + (size_t)n >= cap) return 0;
        pos += (size_t)n;
    }
    return pos;
}

static bool render_single_template(const waywal_palette_t *pal, const named_color_slot_t *slots,
                                   size_t n_slots, const char *src, size_t src_len,
                                   const char *out_path) {
    size_t cap = src_len * 2 + 4096;
    char *out = (char *)malloc(cap);
    if (!out) {
        return false;
    }

    size_t pos = 0;
    size_t i = 0;
    while (i < src_len) {
        if (i + 3 < src_len && src[i] == '{' && src[i + 1] == '{') {
            size_t end = i + 2;
            while (end + 1 < src_len && end - (i + 2) < 64 &&
                   !(src[end] == '}' && src[end + 1] == '}')) {
                end++;
            }
            if (end + 1 < src_len && src[end] == '}' && src[end + 1] == '}') {
                char expr[68];
                size_t expr_len = end - (i + 2);
                memcpy(expr, src + i + 2, expr_len);
                expr[expr_len] = '\0';

                /* Trim whitespace inside {{ ... }} */
                char *start_p = expr;
                while (*start_p && isspace((unsigned char)*start_p)) start_p++;
                char *end_p = start_p + strlen(start_p);
                while (end_p > start_p && isspace((unsigned char)end_p[-1])) {
                    *(--end_p) = '\0';
                }

                char replacement[64];
                int rep_len = -1;

                if (strcmp(start_p, "mode") == 0) {
                    rep_len = snprintf(replacement, sizeof(replacement), "%s",
                                       (pal->mode == WAYWAL_THEME_LIGHT) ? "light" : "dark");
                } else {
                    char *dot = strchr(start_p, '.');
                    const char *fmt = "hex";
                    if (dot) {
                        *dot = '\0';
                        fmt = dot + 1;
                    }
                    for (size_t s = 0; s < n_slots; ++s) {
                        if (strcmp(slots[s].name, start_p) == 0) {
                            color_rgba_t c = slots[s].color;
                            if (strcmp(fmt, "hex") == 0) {
                                rep_len = snprintf(replacement, sizeof(replacement),
                                                   "#%02x%02x%02x", c.r, c.g, c.b);
                            } else if (strcmp(fmt, "strip") == 0) {
                                rep_len = snprintf(replacement, sizeof(replacement),
                                                   "%02x%02x%02x", c.r, c.g, c.b);
                            } else if (strcmp(fmt, "rgb") == 0) {
                                rep_len = snprintf(replacement, sizeof(replacement),
                                                   "%u, %u, %u", c.r, c.g, c.b);
                            } else if (strcmp(fmt, "rgba") == 0) {
                                rep_len = snprintf(replacement, sizeof(replacement),
                                                   "rgba(%u, %u, %u, 1.0)", c.r, c.g, c.b);
                            }
                            break;
                        }
                    }
                }

                if (rep_len >= 0) {
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
    free(out);
    return ok;
}

bool waywal_palette_render_templates(const waywal_palette_t *pal, const char *templates_dir,
                                     const char *out_dir) {
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

    named_color_slot_t slots[128];
    size_t n_slots = collect_color_slots(pal, slots);

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }

        path_buf_t src_file = tpl_path;
        if (!path_buf_push(&src_file, ent->d_name)) {
            continue;
        }

        struct stat st;
        if (stat(src_file.data, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
            st.st_size > (4 * 1024 * 1024)) {
            continue;
        }

        int fd = open(src_file.data, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        size_t fsize = (size_t)st.st_size;
        char *buf = (char *)malloc(fsize + 1);
        if (!buf) {
            close(fd);
            continue;
        }

        size_t nread_total = 0;
        while (nread_total < fsize) {
            ssize_t r = read(fd, buf + nread_total, fsize - nread_total);
            if (r < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (r == 0) break;
            nread_total += (size_t)r;
        }
        close(fd);
        buf[nread_total] = '\0';

        path_buf_t dst_file = target_dir;
        if (path_buf_push(&dst_file, ent->d_name)) {
            render_single_template(pal, slots, n_slots, buf, nread_total, dst_file.data);
        }
        free(buf);
    }

    closedir(dir);
    return true;
}

bool waywal_palette_run_hook(const char *cmd) {
    if (!cmd || cmd[0] == '\0') {
        return false;
    }

    pid_t pid = 0;
    char *const argv[] = {(char *)"sh", (char *)"-c", (char *)cmd, NULL};
    int rc = posix_spawn(&pid, "/bin/sh", NULL, NULL, argv, environ);
    return (rc == 0);
}

bool waywal_palette_export_all(const waywal_palette_t *pal, const char *override_base_dir) {
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

    size_t scheme_len = format_caelestia_json(pal, slots, n_slots, scheme_json, sizeof(scheme_json));
    size_t seq_len = waywal_palette_format_sequences(pal, seq_txt, sizeof(seq_txt));
    size_t cjson_len = format_wwal_colors_json(pal, slots, n_slots, colors_json, sizeof(colors_json));
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
        path_buf_from_str(&flat, override_base_dir);
        path_buf_push(&flat, "scheme.json");
        atomic_write_file(flat.data, scheme_json, scheme_len);

        path_buf_from_str(&flat, override_base_dir);
        path_buf_push(&flat, "sequences.txt");
        atomic_write_file(flat.data, seq_txt, seq_len);

        path_buf_from_str(&flat, override_base_dir);
        path_buf_push(&flat, "colors.json");
        atomic_write_file(flat.data, colors_json, cjson_len);

        path_buf_from_str(&flat, override_base_dir);
        path_buf_push(&flat, "colors.css");
        atomic_write_file(flat.data, colors_css, ccss_len);

        path_buf_from_str(&flat, override_base_dir);
        path_buf_push(&flat, "colors-hyprland.conf");
        atomic_write_file(flat.data, colors_hypr, chypr_len);
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

    target = cache_dir;
    if (!path_buf_push(&target, "colors.json") ||
        !atomic_write_file(target.data, colors_json, cjson_len)) {
        return false;
    }

    target = cache_dir;
    if (!path_buf_push(&target, "colors.css") ||
        !atomic_write_file(target.data, colors_css, ccss_len)) {
        return false;
    }

    target = cache_dir;
    if (!path_buf_push(&target, "colors-hyprland.conf") ||
        !atomic_write_file(target.data, colors_hypr, chypr_len)) {
        return false;
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

static bool is_numeric_name(const char *s) {
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

bool waywal_palette_broadcast_tty(const waywal_palette_t *pal) {
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
