// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/we_physics.h"
#include "waywal/we_puppet.h"

#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#define WE_PI 3.14159265358979323846f

/* ========================================================================= */
/* 1. Material Presets Implementation                                        */
/* ========================================================================= */

void we_physics_apply_preset(we_bone_sim_config_t *cfg, we_physics_preset_t preset)
{
    if (!cfg) {
        return;
    }
    uint32_t bidx = cfg->bone_index;
    float rlen = cfg->rest_length_px > 0.0f ? cfg->rest_length_px : 30.0f;
    memset(cfg, 0, sizeof(*cfg));
    cfg->bone_index = bidx;
    cfg->rest_length_px = rlen;

    switch (preset) {
    case WE_PHYS_PRESET_SILK:
        cfg->flags =
            WE_BONE_PHYS_ENABLED | WE_BONE_PHYS_ANGULAR_LIMIT | WE_BONE_PHYS_CURSOR_COLLIDE;
        cfg->compliance_stretch = 1e-5f;
        cfg->compliance_bend = 0.04f;
        cfg->damping_ratio = 0.08f;
        cfg->mass_kg = 0.05f;
        cfg->gravity_scale = 0.4f;
        cfg->wind_drag_coeff = 2.5f;
        cfg->angle_limit_min_rad = -1.2f;
        cfg->angle_limit_max_rad = 1.2f;
        cfg->collision_radius_px = 15.0f;
        cfg->audio_drag_coeff = 0.5f;
        cfg->rayleigh_damping = 0.02f;
        break;

    case WE_PHYS_PRESET_LONG_HAIR:
        cfg->flags =
            WE_BONE_PHYS_ENABLED | WE_BONE_PHYS_ANGULAR_LIMIT | WE_BONE_PHYS_CURSOR_COLLIDE;
        cfg->compliance_stretch = 1e-6f;
        cfg->compliance_bend = 0.012f;
        cfg->damping_ratio = 0.15f;
        cfg->mass_kg = 0.15f;
        cfg->gravity_scale = 0.8f;
        cfg->wind_drag_coeff = 1.8f;
        cfg->angle_limit_min_rad = -0.9f;
        cfg->angle_limit_max_rad = 0.9f;
        cfg->collision_radius_px = 12.0f;
        cfg->audio_drag_coeff = 0.8f;
        cfg->rayleigh_damping = 0.04f;
        break;

    case WE_PHYS_PRESET_HEAVY_CAPE:
        cfg->flags =
            WE_BONE_PHYS_ENABLED | WE_BONE_PHYS_ANGULAR_LIMIT | WE_BONE_PHYS_CURSOR_COLLIDE;
        cfg->compliance_stretch = 1e-7f;
        cfg->compliance_bend = 0.003f;
        cfg->damping_ratio = 0.35f;
        cfg->mass_kg = 2.0f;
        cfg->gravity_scale = 1.2f;
        cfg->wind_drag_coeff = 1.2f;
        cfg->angle_limit_min_rad = -0.7f;
        cfg->angle_limit_max_rad = 0.7f;
        cfg->collision_radius_px = 25.0f;
        cfg->audio_drag_coeff = 0.3f;
        cfg->rayleigh_damping = 0.08f;
        break;

    case WE_PHYS_PRESET_FOLIAGE_BRANCH:
        cfg->flags =
            WE_BONE_PHYS_ENABLED | WE_BONE_PHYS_ANGULAR_LIMIT | WE_BONE_PHYS_CURSOR_COLLIDE;
        cfg->compliance_stretch = 1e-7f;
        cfg->compliance_bend = 0.001f;
        cfg->damping_ratio = 0.50f;
        cfg->mass_kg = 0.8f;
        cfg->gravity_scale = 0.2f;
        cfg->wind_drag_coeff = 2.0f;
        cfg->angle_limit_min_rad = -0.5f;
        cfg->angle_limit_max_rad = 0.5f;
        cfg->collision_radius_px = 20.0f;
        cfg->audio_drag_coeff = 0.6f;
        cfg->rayleigh_damping = 0.12f;
        break;

    case WE_PHYS_PRESET_SOFT_RIBBON:
        cfg->flags =
            WE_BONE_PHYS_ENABLED | WE_BONE_PHYS_ANGULAR_LIMIT | WE_BONE_PHYS_CURSOR_COLLIDE;
        cfg->compliance_stretch = 5e-6f;
        cfg->compliance_bend = 0.025f;
        cfg->damping_ratio = 0.10f;
        cfg->mass_kg = 0.08f;
        cfg->gravity_scale = 0.5f;
        cfg->wind_drag_coeff = 3.0f;
        cfg->angle_limit_min_rad = -1.4f;
        cfg->angle_limit_max_rad = 1.4f;
        cfg->collision_radius_px = 10.0f;
        cfg->audio_drag_coeff = 0.4f;
        cfg->rayleigh_damping = 0.03f;
        break;

    case WE_PHYS_PRESET_METALLIC_CHAIN:
        cfg->flags =
            WE_BONE_PHYS_ENABLED | WE_BONE_PHYS_ANGULAR_LIMIT | WE_BONE_PHYS_CURSOR_COLLIDE;
        cfg->compliance_stretch = 1e-8f;
        cfg->compliance_bend = 0.10f;
        cfg->damping_ratio = 0.20f;
        cfg->mass_kg = 5.0f;
        cfg->gravity_scale = 1.5f;
        cfg->wind_drag_coeff = 0.3f;
        cfg->angle_limit_min_rad = -1.5f;
        cfg->angle_limit_max_rad = 1.5f;
        cfg->collision_radius_px = 18.0f;
        cfg->audio_drag_coeff = 0.1f;
        cfg->rayleigh_damping = 0.05f;
        break;

    case WE_PHYS_PRESET_CUSTOM:
    default:
        cfg->flags =
            WE_BONE_PHYS_ENABLED | WE_BONE_PHYS_ANGULAR_LIMIT | WE_BONE_PHYS_CURSOR_COLLIDE;
        cfg->compliance_stretch = 1e-6f;
        cfg->compliance_bend = 0.01f;
        cfg->damping_ratio = 0.15f;
        cfg->mass_kg = 1.0f;
        cfg->gravity_scale = 1.0f;
        cfg->wind_drag_coeff = 1.0f;
        cfg->angle_limit_min_rad = -1.0f;
        cfg->angle_limit_max_rad = 1.0f;
        cfg->collision_radius_px = 15.0f;
        cfg->audio_drag_coeff = 0.5f;
        cfg->rayleigh_damping = 0.04f;
        break;
    }
}

void we_physics_load_preset(we_bone_sim_config_t *cfg, we_physics_preset_t preset)
{
    we_physics_apply_preset(cfg, preset);
}

/* ========================================================================= */
/* 2. Simulation JSON Parser                                                 */
/* ========================================================================= */

static bool parse_json_number(const char *json, const char *key, float *out_val)
{
    if (!json || !key || !out_val) {
        return false;
    }
    size_t klen = strlen(key);
    const char *p = json;

    while ((p = strstr(p, key)) != NULL) {
        /* Check word boundary before key */
        if (p > json) {
            char prev = *(p - 1);
            if (isalnum((unsigned char)prev) || prev == '_') {
                p += klen;
                continue;
            }
        }
        /* Check boundary after key */
        char after = p[klen];
        if (isalnum((unsigned char)after) || after == '_') {
            p += klen;
            continue;
        }

        const char *cur = p + klen;
        if (*cur == '"' || *cur == '\'') {
            cur++;
        }
        while (*cur == ' ' || *cur == '\t' || *cur == '\r' || *cur == '\n') {
            cur++;
        }
        if (*cur != ':') {
            p += klen;
            continue;
        }
        cur++;
        while (*cur == ' ' || *cur == '\t' || *cur == '\r' || *cur == '\n') {
            cur++;
        }

        char *endptr = NULL;
        float v = strtof(cur, &endptr);
        if (endptr != cur && isfinite(v)) {
            *out_val = v;
            return true;
        }
        p += klen;
    }
    return false;
}

bool we_physics_parse_simulation_json(const char *json, we_bone_sim_config_t *out_cfg)
{
    if (!json || json[0] == '\0' || !out_cfg) {
        return false;
    }

    bool matched = false;
    float val = 0.0f;

    if (parse_json_number(json, "stiffness", &val) ||
        parse_json_number(json, "\"stiffness\"", &val)) {
        val = fminf(fmaxf(val, 0.0f), 1.0f);
        /* alpha_bend = (1 - stiffness)^3 * 0.05 */
        float inv_s = 1.0f - val;
        out_cfg->compliance_bend = inv_s * inv_s * inv_s * 0.05f;
        out_cfg->compliance_stretch = inv_s * 1e-6f;
        out_cfg->flags |= WE_BONE_PHYS_ENABLED;
        matched = true;
    }

    if (parse_json_number(json, "damping", &val) || parse_json_number(json, "\"damping\"", &val)) {
        out_cfg->damping_ratio = fmaxf(val, 0.0f);
        out_cfg->rayleigh_damping = out_cfg->damping_ratio * 0.05f;
        out_cfg->flags |= WE_BONE_PHYS_ENABLED;
        matched = true;
    }

    if (parse_json_number(json, "gravity", &val) || parse_json_number(json, "\"gravity\"", &val)) {
        if (fabsf(val) > 2.0f) {
            out_cfg->gravity_scale = val / 9.8f;
        } else {
            out_cfg->gravity_scale = val;
        }
        out_cfg->flags |= WE_BONE_PHYS_ENABLED;
        matched = true;
    }

    if (parse_json_number(json, "mass", &val) || parse_json_number(json, "\"mass\"", &val)) {
        out_cfg->mass_kg = fmaxf(val, 0.01f);
        out_cfg->flags |= WE_BONE_PHYS_ENABLED;
        matched = true;
    }

    if (parse_json_number(json, "length", &val) || parse_json_number(json, "\"length\"", &val)) {
        out_cfg->rest_length_px = fmaxf(val, 1.0f);
        out_cfg->flags |= WE_BONE_PHYS_ENABLED;
        matched = true;
    }

    if (parse_json_number(json, "maxangle", &val) ||
        parse_json_number(json, "\"maxangle\"", &val) ||
        parse_json_number(json, "max_angle", &val) ||
        parse_json_number(json, "\"max_angle\"", &val)) {
        float ang = val;
        if (ang > 3.14159f) {
            ang = ang * (WE_PI / 180.0f);
        }
        ang = fabsf(ang);
        out_cfg->angle_limit_max_rad = ang;
        out_cfg->angle_limit_min_rad = -ang;
        out_cfg->flags |= (WE_BONE_PHYS_ENABLED | WE_BONE_PHYS_ANGULAR_LIMIT);
        matched = true;
    }

    if (parse_json_number(json, "wind", &val) || parse_json_number(json, "\"wind\"", &val)) {
        out_cfg->wind_drag_coeff = fmaxf(val, 0.0f);
        out_cfg->flags |= WE_BONE_PHYS_ENABLED;
        matched = true;
    }

    if (parse_json_number(json, "audio", &val) || parse_json_number(json, "\"audio\"", &val)) {
        out_cfg->audio_drag_coeff = fmaxf(val, 0.0f);
        out_cfg->flags |= WE_BONE_PHYS_ENABLED;
        matched = true;
    }

    if (parse_json_number(json, "radius", &val) || parse_json_number(json, "\"radius\"", &val)) {
        out_cfg->collision_radius_px = fmaxf(val, 1.0f);
        out_cfg->flags |= (WE_BONE_PHYS_ENABLED | WE_BONE_PHYS_CURSOR_COLLIDE);
        matched = true;
    }

    return matched;
}

/* ========================================================================= */
/* 3. Divergence-Free 3D Simplex Curl-Noise Wind (Bridson et al. 2007)       */
/* ========================================================================= */

static inline float sanitize_simplex_coord(float v)
{
    if (!isfinite(v)) {
        return 0.0f;
    }
    return fminf(fmaxf(v, -3.0e6f), 3.0e6f);
}

/* Clamps float lattice coordinates before int32_t conversion to prevent signed overflow UB
 * when world coordinates or elapsed runtime grow large. */
static inline int32_t fast_floor_f(float x)
{
    if (!isfinite(x)) {
        return 0;
    }
    float clamped = fminf(fmaxf(x, -1.0e7f), 1.0e7f);
    int32_t xi = (int32_t)clamped;
    return (clamped < (float)xi) ? (xi - 1) : xi;
}

/* Analytic 3D Simplex Noise value + gradient evaluation (Perlin 2001 / Stefan Gustavson)
 * Computes Psi_m(p) and dPsi_m/dx, dPsi_m/dy, dPsi_m/dz in a single pass without finite
 * differences. */
static inline float simplex3d_grad(float x_in, float y_in, float z_in, float *gx, float *gy,
                                   float *gz)
{
    const float F3 = 0.333333333333f;
    const float G3 = 0.166666666667f;
    float x = sanitize_simplex_coord(x_in);
    float y = sanitize_simplex_coord(y_in);
    float z = sanitize_simplex_coord(z_in);
    float s = (x + y + z) * F3;
    int32_t i = fast_floor_f(x + s);
    int32_t j = fast_floor_f(y + s);
    int32_t k = fast_floor_f(z + s);
    float t = (float)(i + j + k) * G3;
    float x0 = x - ((float)i - t), y0 = y - ((float)j - t), z0 = z - ((float)k - t);

    int32_t i1, j1, k1, i2, j2, k2;
    if (x0 >= y0) {
        if (y0 >= z0) {
            i1 = 1;
            j1 = 0;
            k1 = 0;
            i2 = 1;
            j2 = 1;
            k2 = 0;
        } else if (x0 >= z0) {
            i1 = 1;
            j1 = 0;
            k1 = 0;
            i2 = 1;
            j2 = 0;
            k2 = 1;
        } else {
            i1 = 0;
            j1 = 0;
            k1 = 1;
            i2 = 1;
            j2 = 0;
            k2 = 1;
        }
    } else {
        if (y0 < z0) {
            i1 = 0;
            j1 = 0;
            k1 = 1;
            i2 = 0;
            j2 = 1;
            k2 = 1;
        } else if (x0 < z0) {
            i1 = 0;
            j1 = 1;
            k1 = 0;
            i2 = 0;
            j2 = 1;
            k2 = 1;
        } else {
            i1 = 0;
            j1 = 1;
            k1 = 0;
            i2 = 1;
            j2 = 1;
            k2 = 0;
        }
    }

    float x1 = x0 - (float)i1 + G3, y1 = y0 - (float)j1 + G3, z1 = z0 - (float)k1 + G3;
    float x2 = x0 - (float)i2 + 2.0f * G3, y2 = y0 - (float)j2 + 2.0f * G3,
          z2 = z0 - (float)k2 + 2.0f * G3;
    float x3 = x0 - 1.0f + 3.0f * G3, y3 = y0 - 1.0f + 3.0f * G3, z3 = z0 - 1.0f + 3.0f * G3;

    float rx[4] = {x0, x1, x2, x3};
    float ry[4] = {y0, y1, y2, y3};
    float rz[4] = {z0, z1, z2, z3};

    uint32_t ui = (uint32_t)i, uj = (uint32_t)j, uk = (uint32_t)k;
    int gi[4] = {
        (int)((ui * 1664525u ^ uj * 1013904223u ^ uk * 2654435761u) & 11u),
        (int)(((ui + (uint32_t)i1) * 1664525u ^ (uj + (uint32_t)j1) * 1013904223u ^
               (uk + (uint32_t)k1) * 2654435761u) &
              11u),
        (int)(((ui + (uint32_t)i2) * 1664525u ^ (uj + (uint32_t)j2) * 1013904223u ^
               (uk + (uint32_t)k2) * 2654435761u) &
              11u),
        (int)(((ui + 1u) * 1664525u ^ (uj + 1u) * 1013904223u ^ (uk + 1u) * 2654435761u) & 11u)};

    static const float grad3[12][3] = {{1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0},
                                       {1, 0, 1}, {-1, 0, 1}, {1, 0, -1}, {-1, 0, -1},
                                       {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}};

    float val = 0.0f, dx = 0.0f, dy = 0.0f, dz = 0.0f;
    for (int c = 0; c < 4; ++c) {
        float t_c = 0.6f - rx[c] * rx[c] - ry[c] * ry[c] - rz[c] * rz[c];
        if (t_c > 0.0f) {
            float t2 = t_c * t_c, t3 = t2 * t_c, t4 = t2 * t2;
            const float *g = grad3[gi[c]];
            float gdot = g[0] * rx[c] + g[1] * ry[c] + g[2] * rz[c];
            val += t4 * gdot;
            float factor = -8.0f * t3 * gdot;
            dx += factor * rx[c] + t4 * g[0];
            dy += factor * ry[c] + t4 * g[1];
            dz += factor * rz[c] + t4 * g[2];
        }
    }
    *gx = 32.0f * dx;
    *gy = 32.0f * dy;
    *gz = 32.0f * dz;
    return 32.0f * val;
}

#if defined(__x86_64__) || defined(_M_X64)
static bool we_physics_has_avx2_fma(void)
{
    static atomic_int s_cached = 0;
    int st = atomic_load_explicit(&s_cached, memory_order_relaxed);
    if (st != 0) {
        return st > 0;
    }
    bool ok = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
    atomic_store_explicit(&s_cached, ok ? 1 : -1, memory_order_relaxed);
    return ok;
}

__attribute__((target("avx2,fma"))) static inline void
simplex3d_grad_avx2_8x(__m256 x_in, __m256 y_in, __m256 z_in, __m256 *out_gx, __m256 *out_gy,
                       __m256 *out_gz)
{
    const __m256 min_c = _mm256_set1_ps(-3.0e6f);
    const __m256 max_c = _mm256_set1_ps(3.0e6f);
    __m256 x = _mm256_min_ps(
        _mm256_max_ps(_mm256_and_ps(_mm256_cmp_ps(x_in, x_in, _CMP_ORD_Q), x_in), min_c), max_c);
    __m256 y = _mm256_min_ps(
        _mm256_max_ps(_mm256_and_ps(_mm256_cmp_ps(y_in, y_in, _CMP_ORD_Q), y_in), min_c), max_c);
    __m256 z = _mm256_min_ps(
        _mm256_max_ps(_mm256_and_ps(_mm256_cmp_ps(z_in, z_in, _CMP_ORD_Q), z_in), min_c), max_c);

    const __m256 F3 = _mm256_set1_ps(0.333333333333f);
    const __m256 G3 = _mm256_set1_ps(0.166666666667f);
    const __m256 one = _mm256_set1_ps(1.0f);
    const __m256 two = _mm256_set1_ps(2.0f);
    const __m256 zero = _mm256_setzero_ps();
    const __m256 all_ones = _mm256_castsi256_ps(_mm256_set1_epi32(-1));

    __m256 s = _mm256_mul_ps(_mm256_add_ps(_mm256_add_ps(x, y), z), F3);
#if defined(__GNUC__) || defined(__clang__)
    /* Prevent FMA contraction on large lattice coordinates before integer floor reduction so
     * tetrahedral cell boundaries match IEEE-754 scalar rounding bit-for-bit. */
    __asm__("" : "+x"(s));
#endif
    __m256 fi = _mm256_floor_ps(_mm256_add_ps(x, s));
    __m256 fj = _mm256_floor_ps(_mm256_add_ps(y, s));
    __m256 fk = _mm256_floor_ps(_mm256_add_ps(z, s));

    __m256i ii = _mm256_cvttps_epi32(fi);
    __m256i jj = _mm256_cvttps_epi32(fj);
    __m256i kk = _mm256_cvttps_epi32(fk);

    __m256 t = _mm256_mul_ps(_mm256_add_ps(_mm256_add_ps(fi, fj), fk), G3);
#if defined(__GNUC__) || defined(__clang__)
    __asm__("" : "+x"(t));
#endif
    __m256 x0 = _mm256_sub_ps(x, _mm256_sub_ps(fi, t));
    __m256 y0 = _mm256_sub_ps(y, _mm256_sub_ps(fj, t));
    __m256 z0 = _mm256_sub_ps(z, _mm256_sub_ps(fk, t));

    __m256 ge_xy = _mm256_cmp_ps(x0, y0, _CMP_GE_OQ);
    __m256 ge_yz = _mm256_cmp_ps(y0, z0, _CMP_GE_OQ);
    __m256 ge_xz = _mm256_cmp_ps(x0, z0, _CMP_GE_OQ);

    __m256 fi1 = _mm256_and_ps(one, _mm256_and_ps(ge_xy, ge_xz));
    __m256 fj1 = _mm256_and_ps(one, _mm256_andnot_ps(ge_xy, ge_yz));
    __m256 fk1 = _mm256_sub_ps(one, _mm256_add_ps(fi1, fj1));

    __m256 fi2 = _mm256_and_ps(one, _mm256_or_ps(ge_xy, ge_xz));
    __m256 fj2 = _mm256_and_ps(one, _mm256_or_ps(_mm256_andnot_ps(ge_xy, all_ones), ge_yz));
    __m256 fk2 = _mm256_sub_ps(two, _mm256_add_ps(fi2, fj2));

    __m256 rx[4], ry[4], rz[4];
    rx[0] = x0;
    ry[0] = y0;
    rz[0] = z0;

    rx[1] = _mm256_add_ps(_mm256_sub_ps(x0, fi1), G3);
    ry[1] = _mm256_add_ps(_mm256_sub_ps(y0, fj1), G3);
    rz[1] = _mm256_add_ps(_mm256_sub_ps(z0, fk1), G3);

    __m256 two_g3 = _mm256_mul_ps(two, G3);
    rx[2] = _mm256_add_ps(_mm256_sub_ps(x0, fi2), two_g3);
    ry[2] = _mm256_add_ps(_mm256_sub_ps(y0, fj2), two_g3);
    rz[2] = _mm256_add_ps(_mm256_sub_ps(z0, fk2), two_g3);

    const __m256 off3 = _mm256_set1_ps(-1.0f + 3.0f * 0.166666666667f);
    rx[3] = _mm256_add_ps(x0, off3);
    ry[3] = _mm256_add_ps(y0, off3);
    rz[3] = _mm256_add_ps(z0, off3);

    __m256i ci[4], cj[4], ck[4];
    ci[0] = ii;
    cj[0] = jj;
    ck[0] = kk;

    ci[1] = _mm256_add_epi32(ii, _mm256_cvttps_epi32(fi1));
    cj[1] = _mm256_add_epi32(jj, _mm256_cvttps_epi32(fj1));
    ck[1] = _mm256_add_epi32(kk, _mm256_cvttps_epi32(fk1));

    ci[2] = _mm256_add_epi32(ii, _mm256_cvttps_epi32(fi2));
    cj[2] = _mm256_add_epi32(jj, _mm256_cvttps_epi32(fj2));
    ck[2] = _mm256_add_epi32(kk, _mm256_cvttps_epi32(fk2));

    const __m256i i_one = _mm256_set1_epi32(1);
    ci[3] = _mm256_add_epi32(ii, i_one);
    cj[3] = _mm256_add_epi32(jj, i_one);
    ck[3] = _mm256_add_epi32(kk, i_one);

    const __m256i m1 = _mm256_set1_epi32((int32_t)1664525u);
    const __m256i m2 = _mm256_set1_epi32((int32_t)1013904223u);
    const __m256i m3 = _mm256_set1_epi32((int32_t)2654435761u);
    const __m256i mask11 = _mm256_set1_epi32(11);
    const __m256i mask3 = _mm256_set1_epi32(3);
    const __m256i mask4 = _mm256_set1_epi32(4);

    const __m256 lut_gx = _mm256_setr_ps(1.0f, -1.0f, 1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    const __m256 lut_gy = _mm256_setr_ps(1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f);
    const __m256 lut_gz = _mm256_setr_ps(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, -1.0f, -1.0f);

    const __m256 c06 = _mm256_set1_ps(0.6f);
    const __m256 neg8 = _mm256_set1_ps(-8.0f);

    __m256 dx = zero, dy = zero, dz = zero;
    for (int c = 0; c < 4; ++c) {
        __m256 r2 = _mm256_fmadd_ps(rx[c], rx[c],
                                    _mm256_fmadd_ps(ry[c], ry[c], _mm256_mul_ps(rz[c], rz[c])));
        __m256 tc = _mm256_max_ps(_mm256_sub_ps(c06, r2), zero);
        __m256 t2 = _mm256_mul_ps(tc, tc);
        __m256 t3 = _mm256_mul_ps(t2, tc);
        __m256 t4 = _mm256_mul_ps(t2, t2);

        __m256i h = _mm256_xor_si256(
            _mm256_xor_si256(_mm256_mullo_epi32(ci[c], m1), _mm256_mullo_epi32(cj[c], m2)),
            _mm256_mullo_epi32(ck[c], m3));
        __m256i gi = _mm256_and_si256(h, mask11);
        __m256i idx8 = _mm256_or_si256(_mm256_and_si256(gi, mask3),
                                       _mm256_and_si256(_mm256_srli_epi32(gi, 1), mask4));

        __m256 gx_c = _mm256_permutevar8x32_ps(lut_gx, idx8);
        __m256 gy_c = _mm256_permutevar8x32_ps(lut_gy, idx8);
        __m256 gz_c = _mm256_permutevar8x32_ps(lut_gz, idx8);

        __m256 gdot =
            _mm256_fmadd_ps(gx_c, rx[c], _mm256_fmadd_ps(gy_c, ry[c], _mm256_mul_ps(gz_c, rz[c])));
        __m256 factor = _mm256_mul_ps(neg8, _mm256_mul_ps(t3, gdot));

        dx = _mm256_fmadd_ps(factor, rx[c], _mm256_fmadd_ps(t4, gx_c, dx));
        dy = _mm256_fmadd_ps(factor, ry[c], _mm256_fmadd_ps(t4, gy_c, dy));
        dz = _mm256_fmadd_ps(factor, rz[c], _mm256_fmadd_ps(t4, gz_c, dz));
    }

    const __m256 c32 = _mm256_set1_ps(32.0f);
    *out_gx = _mm256_mul_ps(c32, dx);
    *out_gy = _mm256_mul_ps(c32, dy);
    *out_gz = _mm256_mul_ps(c32, dz);
}

__attribute__((target("avx2,fma"))) static void
eval_curl_wind_3d_avx2_8x(const we_wind_field_t *wind, const float xs[8], const float ys[8],
                          const float zs[8], float t_phase, float out_vx[8], float out_vy[8],
                          float out_vz[8])
{
    float v_base = isfinite(wind->base_speed_px_s) ? wind->base_speed_px_s : 0.0f;
    float dir = isfinite(wind->base_direction_rad) ? wind->base_direction_rad : 0.0f;
    float scale = (isfinite(wind->curl_spatial_scale) && wind->curl_spatial_scale > 1e-6f)
                      ? wind->curl_spatial_scale
                      : 0.008f;
    float gust = isfinite(wind->gust_amplitude) ? wind->gust_amplitude : 0.0f;
    float turb_gain = gust * (v_base * 0.5f + 40.0f);

    __m256 vscale = _mm256_set1_ps(scale);
    __m256 xs_scaled = _mm256_mul_ps(_mm256_loadu_ps(xs), vscale);
    __m256 sy = _mm256_mul_ps(_mm256_loadu_ps(ys), vscale);
    __m256 zs_scaled = _mm256_mul_ps(_mm256_loadu_ps(zs), vscale);
#if defined(__GNUC__) || defined(__clang__)
    __asm__("" : "+x"(xs_scaled), "+x"(zs_scaled));
#endif
    __m256 sx = _mm256_sub_ps(xs_scaled, _mm256_set1_ps(t_phase * 0.45f));
    __m256 sz = _mm256_add_ps(zs_scaled, _mm256_set1_ps(t_phase * 0.25f));

    __m256 dP1x, dP1y, dP1z;
    __m256 dP2x, dP2y, dP2z;
    __m256 dP3x, dP3y, dP3z;

    simplex3d_grad_avx2_8x(sx, sy, sz, &dP1x, &dP1y, &dP1z);
    simplex3d_grad_avx2_8x(_mm256_add_ps(sx, _mm256_set1_ps(31.41f)),
                           _mm256_add_ps(sy, _mm256_set1_ps(47.85f)),
                           _mm256_sub_ps(sz, _mm256_set1_ps(19.13f)), &dP2x, &dP2y, &dP2z);
    simplex3d_grad_avx2_8x(_mm256_sub_ps(sx, _mm256_set1_ps(73.19f)),
                           _mm256_add_ps(sy, _mm256_set1_ps(12.64f)),
                           _mm256_add_ps(sz, _mm256_set1_ps(88.07f)), &dP3x, &dP3y, &dP3z);

    __m256 gain = _mm256_set1_ps(scale * turb_gain);
    __m256 vx =
        _mm256_fmadd_ps(_mm256_sub_ps(dP3y, dP2z), gain, _mm256_set1_ps(v_base * cosf(dir)));
    __m256 vy =
        _mm256_fmadd_ps(_mm256_sub_ps(dP1z, dP3x), gain, _mm256_set1_ps(v_base * sinf(dir)));
    __m256 vz = _mm256_mul_ps(_mm256_sub_ps(dP2x, dP1y), gain);

    _mm256_storeu_ps(out_vx, vx);
    _mm256_storeu_ps(out_vy, vy);
    _mm256_storeu_ps(out_vz, vz);
}
#endif

static inline float compute_wind_phase_f32(const we_wind_field_t *wind, double time_sec)
{
    float freq = (isfinite(wind->gust_frequency_hz) && wind->gust_frequency_hz > 0.0f)
                     ? wind->gust_frequency_hz
                     : 0.5f;
    if (!isfinite(time_sec)) {
        return 0.0f;
    }
    /* Reduce temporal phase in double precision so multi-day daemon sessions never lose sub-frame
     * gust resolution when converted to float32. */
    double phase = fmod(time_sec * (double)freq, 65536.0);
    if (phase < 0.0) {
        phase += 65536.0;
    }
    return (float)phase;
}

void we_physics_eval_curl_wind_3d(const we_wind_field_t *wind, float x, float y, float z,
                                  double time_sec, float out_vel[3])
{
    if (!out_vel) {
        return;
    }
    if (!wind || !wind->enabled) {
        out_vel[0] = 0.0f;
        out_vel[1] = 0.0f;
        out_vel[2] = 0.0f;
        return;
    }

    float v_base = isfinite(wind->base_speed_px_s) ? wind->base_speed_px_s : 0.0f;
    float dir = isfinite(wind->base_direction_rad) ? wind->base_direction_rad : 0.0f;
    float base_u = v_base * cosf(dir);
    float base_v = v_base * sinf(dir);

    float scale = (isfinite(wind->curl_spatial_scale) && wind->curl_spatial_scale > 1e-6f)
                      ? wind->curl_spatial_scale
                      : 0.008f;
    float t = compute_wind_phase_f32(wind, time_sec);

    float px = isfinite(x) ? x : 0.0f;
    float py = isfinite(y) ? y : 0.0f;
    float pz = isfinite(z) ? z : 0.0f;

    float sx = px * scale - t * 0.45f;
    float sy = py * scale;
    float sz = pz * scale + t * 0.25f;

    float dP1x, dP1y, dP1z;
    float dP2x, dP2y, dP2z;
    float dP3x, dP3y, dP3z;

    (void)simplex3d_grad(sx, sy, sz, &dP1x, &dP1y, &dP1z);
    (void)simplex3d_grad(sx + 31.41f, sy + 47.85f, sz - 19.13f, &dP2x, &dP2y, &dP2z);
    (void)simplex3d_grad(sx - 73.19f, sy + 12.64f, sz + 88.07f, &dP3x, &dP3y, &dP3z);

    /* Analytical curl: u = curl(Psi)
     * u_x = dPsi_3/dy - dPsi_2/dz
     * u_y = dPsi_1/dz - dPsi_3/dx
     * u_z = dPsi_2/dx - dPsi_1/dy
     * Divergence div(u) = d/dx(u_x) + d/dy(u_y) + d/dz(u_z) == 0 (provably exact). */
    float curl_x = (dP3y - dP2z) * scale;
    float curl_y = (dP1z - dP3x) * scale;
    float curl_z = (dP2x - dP1y) * scale;

    float gust = isfinite(wind->gust_amplitude) ? wind->gust_amplitude : 0.0f;
    float turb_gain = gust * (v_base * 0.5f + 40.0f);

    out_vel[0] = base_u + curl_x * turb_gain;
    out_vel[1] = base_v + curl_y * turb_gain;
    out_vel[2] = curl_z * turb_gain;
}

void we_physics_eval_curl_wind_3d_batch(const we_wind_field_t *wind, const float *xs,
                                        const float *ys, const float *zs, uint32_t count,
                                        double time_sec, float (*out_vel)[3])
{
    if (!out_vel || count == 0u) {
        return;
    }
    if (!wind || !wind->enabled || !xs || !ys || !zs) {
        memset(out_vel, 0, (size_t)count * sizeof(out_vel[0]));
        return;
    }

    uint32_t idx = 0u;
#if defined(__x86_64__) || defined(_M_X64)
    if (count >= 8u && we_physics_has_avx2_fma()) {
        float t_phase = compute_wind_phase_f32(wind, time_sec);
        float vx[8], vy[8], vz[8];
        for (; idx + 8u <= count; idx += 8u) {
            eval_curl_wind_3d_avx2_8x(wind, xs + idx, ys + idx, zs + idx, t_phase, vx, vy, vz);
            for (uint32_t lane = 0; lane < 8u; ++lane) {
                out_vel[idx + lane][0] = vx[lane];
                out_vel[idx + lane][1] = vy[lane];
                out_vel[idx + lane][2] = vz[lane];
            }
        }
    }
#endif
    for (; idx < count; ++idx) {
        we_physics_eval_curl_wind_3d(wind, xs[idx], ys[idx], zs[idx], time_sec, out_vel[idx]);
    }
}

void we_physics_sample_curl_wind(const we_wind_field_t *wind, float x, float y, double time_sec,
                                 float out_wind_vel[2])
{
    if (!out_wind_vel) {
        return;
    }
    float v3[3] = {0.0f, 0.0f, 0.0f};
    we_physics_eval_curl_wind_3d(wind, x, y, 0.0f, time_sec, v3);
    out_wind_vel[0] = v3[0];
    out_wind_vel[1] = v3[1];
}

/* ========================================================================= */
/* 4. Dynamic Cursor & Audio Coupling Setters                                */
/* ========================================================================= */

void we_physics_set_cursor(we_puppet_physics_ctx_t *ctx, float x, float y, float vx, float vy)
{
    if (!ctx) {
        return;
    }
    ctx->cursor_pos[0] = isfinite(x) ? x : 0.0f;
    ctx->cursor_pos[1] = isfinite(y) ? y : 0.0f;
    ctx->cursor_vel[0] = isfinite(vx) ? fminf(fmaxf(vx, -50000.0f), 50000.0f) : 0.0f;
    ctx->cursor_vel[1] = isfinite(vy) ? fminf(fmaxf(vy, -50000.0f), 50000.0f) : 0.0f;
}

void we_physics_set_audio_bands(we_puppet_physics_ctx_t *ctx, const float bands[64])
{
    if (!ctx || !bands) {
        return;
    }
    for (int i = 0; i < 64; ++i) {
        float b = bands[i];
        ctx->audio_bands[i] = (isfinite(b) && b > 0.0f) ? fminf(b, 100.0f) : 0.0f;
    }
}

/* ========================================================================= */
/* 5. Skeleton Physics Initialization                                        */
/* ========================================================================= */

static inline void phys_mat4_mul(const float a[16], const float b[16], float out[16])
{
    float r[16];
    for (int col = 0; col < 4; col++) {
        const float b0 = b[col * 4 + 0];
        const float b1 = b[col * 4 + 1];
        const float b2 = b[col * 4 + 2];
        const float b3 = b[col * 4 + 3];
        r[col * 4 + 0] = a[0] * b0 + a[4] * b1 + a[8] * b2 + a[12] * b3;
        r[col * 4 + 1] = a[1] * b0 + a[5] * b1 + a[9] * b2 + a[13] * b3;
        r[col * 4 + 2] = a[2] * b0 + a[6] * b1 + a[10] * b2 + a[14] * b3;
        r[col * 4 + 3] = a[3] * b0 + a[7] * b1 + a[11] * b2 + a[15] * b3;
    }
    memcpy(out, r, sizeof(r));
}

void we_physics_init_from_puppet(we_puppet_physics_ctx_t *ctx, const we_puppet_t *puppet)
{
    if (!ctx) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));

    /* Default divergence-free curl wind */
    ctx->wind.enabled = 1;
    ctx->wind.base_direction_rad = 0.0f;
    ctx->wind.base_speed_px_s = 60.0f;
    ctx->wind.gust_frequency_hz = 0.5f;
    ctx->wind.gust_amplitude = 1.5f;
    ctx->wind.curl_spatial_scale = 0.005f;
    ctx->wind.gravity_vec[0] = 0.0f;
    ctx->wind.gravity_vec[1] = -980.0f;

    /* Default cursor interaction */
    ctx->cursor.enabled = 1;
    ctx->cursor.radius_px = 120.0f;
    ctx->cursor.impulse_strength = 200.0f;
    ctx->cursor.falloff_power = 2.0f;

    if (!puppet || puppet->bone_count == 0 || !puppet->bones) {
        ctx->initialized = true;
        return;
    }

    uint32_t bcnt = puppet->bone_count;
    if (bcnt > WE_MDL_MAX_BONES) {
        bcnt = WE_MDL_MAX_BONES;
    }

    /* Reconstruct world bind matrices on the stack when callers supply synthetic test bones whose
     * world_bind field was left zero-initialized. */
    float world_bind_buf[WE_MDL_MAX_BONES][16];
    for (uint32_t k = 0; k < bcnt; k++) {
        const we_mdl_bone_t *b = &puppet->bones[k];
        if (fabsf(b->world_bind[15]) > 1e-6f) {
            memcpy(world_bind_buf[k], b->world_bind, sizeof(world_bind_buf[k]));
        } else if (b->parent == 0xFFFFFFFFu || b->parent >= k) {
            memcpy(world_bind_buf[k], b->local_trans, sizeof(world_bind_buf[k]));
        } else {
            phys_mat4_mul(world_bind_buf[b->parent], b->local_trans, world_bind_buf[k]);
        }
    }

    for (uint32_t k = 0; k < bcnt; k++) {
        const we_mdl_bone_t *b = &puppet->bones[k];
        const float *wb = world_bind_buf[k];
        we_bone_sim_config_t *cfg = &ctx->configs[k];
        cfg->bone_index = k;

        /* Parse simulation_json if present in MDL bone */
        if (b->simulation_json[0] != '\0') {
            we_physics_parse_simulation_json(b->simulation_json, cfg);
        }

        /* Determine rest length and local rest direction */
        float pivot[3] = {wb[12], wb[13], wb[14]};
        float tip_dir_world[3] = {0.0f, 1.0f, 0.0f};
        float rest_len = cfg->rest_length_px;

        /* Average all direct child bone vectors so branching hubs (e.g. chest/pelvis) align with
         * their symmetric centroid rather than biasing toward whichever child came first. */
        float sum_dx = 0.0f, sum_dy = 0.0f, sum_dz = 0.0f, sum_dist = 0.0f;
        float first_dir[3] = {0.0f, 1.0f, 0.0f};
        uint32_t child_cnt = 0u;
        for (uint32_t c = 0; c < bcnt; c++) {
            if (puppet->bones[c].parent == k && c != k) {
                const float *cwb = world_bind_buf[c];
                float dx = cwb[12] - pivot[0];
                float dy = cwb[13] - pivot[1];
                float dz = cwb[14] - pivot[2];
                float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                if (dist > 1.0e-3f) {
                    if (child_cnt == 0u) {
                        first_dir[0] = dx / dist;
                        first_dir[1] = dy / dist;
                        first_dir[2] = dz / dist;
                    }
                    sum_dx += dx;
                    sum_dy += dy;
                    sum_dz += dz;
                    sum_dist += dist;
                    child_cnt++;
                }
            }
        }

        bool found_child = false;
        if (child_cnt > 0u) {
            float avg_len = sqrtf(sum_dx * sum_dx + sum_dy * sum_dy + sum_dz * sum_dz);
            if (avg_len > 1.0e-4f) {
                tip_dir_world[0] = sum_dx / avg_len;
                tip_dir_world[1] = sum_dy / avg_len;
                tip_dir_world[2] = sum_dz / avg_len;
            } else {
                tip_dir_world[0] = first_dir[0];
                tip_dir_world[1] = first_dir[1];
                tip_dir_world[2] = first_dir[2];
            }
            if (rest_len <= 0.0f) {
                rest_len = sum_dist / (float)child_cnt;
            }
            found_child = true;
        }

        /* If leaf bone, continue along parent vector if available */
        if (!found_child && b->parent != 0xFFFFFFFFu && b->parent < bcnt) {
            const float *pwb = world_bind_buf[b->parent];
            float dx = pivot[0] - pwb[12];
            float dy = pivot[1] - pwb[13];
            float dz = pivot[2] - pwb[14];
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dist > 1.0f) {
                tip_dir_world[0] = dx / dist;
                tip_dir_world[1] = dy / dist;
                tip_dir_world[2] = dz / dist;
                if (rest_len <= 0.0f) {
                    rest_len = dist;
                }
                found_child = true;
            }
        }

        if (rest_len <= 0.0f) {
            rest_len = 30.0f;
        }
        cfg->rest_length_px = rest_len;

        /* Transform world tip direction to local rest direction via transpose of upper-left 3x3 of
         * world_bind */
        float rx = wb[0] * tip_dir_world[0] + wb[1] * tip_dir_world[1] + wb[2] * tip_dir_world[2];
        float ry = wb[4] * tip_dir_world[0] + wb[5] * tip_dir_world[1] + wb[6] * tip_dir_world[2];
        float rz = wb[8] * tip_dir_world[0] + wb[9] * tip_dir_world[1] + wb[10] * tip_dir_world[2];
        float rlen = sqrtf(rx * rx + ry * ry + rz * rz);
        if (rlen > 1e-5f) {
            ctx->rest_dir_local[k][0] = rx / rlen;
            ctx->rest_dir_local[k][1] = ry / rlen;
            ctx->rest_dir_local[k][2] = rz / rlen;
        } else {
            ctx->rest_dir_local[k][0] = 0.0f;
            ctx->rest_dir_local[k][1] = 1.0f;
            ctx->rest_dir_local[k][2] = 0.0f;
        }

        /* Initialize bone runtime state */
        we_bone_sim_state_t *st = &ctx->states[k];
        st->pos_curr[0] = pivot[0] + tip_dir_world[0] * rest_len;
        st->pos_curr[1] = pivot[1] + tip_dir_world[1] * rest_len;
        st->pos_curr[2] = pivot[2] + tip_dir_world[2] * rest_len;
        st->pos_prev[0] = st->pos_curr[0];
        st->pos_prev[1] = st->pos_curr[1];
        st->pos_prev[2] = st->pos_curr[2];
        st->vel[0] = 0.0f;
        st->vel[1] = 0.0f;
        st->vel[2] = 0.0f;
        st->lambda_stretch = 0.0f;
        st->lambda_bend = 0.0f;
        st->current_angle_rad = 0.0f;
        st->target_pos[0] = st->pos_curr[0];
        st->target_pos[1] = st->pos_curr[1];
        st->target_pos[2] = st->pos_curr[2];

        if (cfg->flags & WE_BONE_PHYS_ENABLED) {
            ctx->active_bone_count++;
        }
    }

    ctx->initialized = true;
}

/* ========================================================================= */
/* 6. Single Substep XPBD Solver for a Bone (Zero Heap Allocations)          */
/* ========================================================================= */

static inline void compute_orthogonal_unit_axis(const float d0[3], float out_u[3])
{
    /* Prefer projecting the Z axis perpendicular to d0 so 2D XY-plane bones rotate purely in-plane
     * at the 180-degree antipodal singularity without flipping mesh Z winding. */
    float ux = -d0[2] * d0[0];
    float uy = -d0[2] * d0[1];
    float uz = 1.0f - d0[2] * d0[2];
    if (uz <= 0.1f) {
        ux = 1.0f - d0[0] * d0[0];
        uy = -d0[0] * d0[1];
        uz = -d0[0] * d0[2];
    }
    float inv_l = 1.0f / sqrtf(fmaxf(ux * ux + uy * uy + uz * uz, 1.0e-12f));
    out_u[0] = ux * inv_l;
    out_u[1] = uy * inv_l;
    out_u[2] = uz * inv_l;
}

static inline void apply_shortest_arc_so3(const float d0[3], const float cur_dir[3], float W[16],
                                          float *out_angle_rad)
{
    float v_cross[3] = {d0[1] * cur_dir[2] - d0[2] * cur_dir[1],
                        d0[2] * cur_dir[0] - d0[0] * cur_dir[2],
                        d0[0] * cur_dir[1] - d0[1] * cur_dir[0]};
    float c = d0[0] * cur_dir[0] + d0[1] * cur_dir[1] + d0[2] * cur_dir[2];
    c = fminf(fmaxf(c, -1.0f), 1.0f);

    float R[3][3];
    if (c < -0.9999f) {
        /* Exact 180-degree rotation R = 2 u u^T - I avoids the (1 + c) -> 0 division singularity
         * of the Rodrigues rational form while preserving det(R) == +1. */
        float u[3];
        compute_orthogonal_unit_axis(d0, u);
        R[0][0] = 2.0f * u[0] * u[0] - 1.0f;
        R[0][1] = 2.0f * u[0] * u[1];
        R[0][2] = 2.0f * u[0] * u[2];
        R[1][0] = 2.0f * u[1] * u[0];
        R[1][1] = 2.0f * u[1] * u[1] - 1.0f;
        R[1][2] = 2.0f * u[1] * u[2];
        R[2][0] = 2.0f * u[2] * u[0];
        R[2][1] = 2.0f * u[2] * u[1];
        R[2][2] = 2.0f * u[2] * u[2] - 1.0f;
        if (out_angle_rad) {
            *out_angle_rad = (v_cross[2] < 0.0f) ? -WE_PI : WE_PI;
        }
    } else {
        float inv_c = 1.0f / (1.0f + c);
        float vx = v_cross[0], vy = v_cross[1], vz = v_cross[2];

        R[0][0] = 1.0f + inv_c * (-vy * vy - vz * vz);
        R[0][1] = -vz + inv_c * (vx * vy);
        R[0][2] = vy + inv_c * (vx * vz);

        R[1][0] = vz + inv_c * (vx * vy);
        R[1][1] = 1.0f + inv_c * (-vx * vx - vz * vz);
        R[1][2] = -vx + inv_c * (vy * vz);

        R[2][0] = -vy + inv_c * (vx * vz);
        R[2][1] = vx + inv_c * (vy * vz);
        R[2][2] = 1.0f + inv_c * (-vx * vx - vy * vy);
        if (out_angle_rad) {
            *out_angle_rad = atan2f(vz, c);
        }
    }

    for (int col = 0; col < 3; col++) {
        float w0 = W[col * 4 + 0];
        float w1 = W[col * 4 + 1];
        float w2 = W[col * 4 + 2];
        W[col * 4 + 0] = R[0][0] * w0 + R[0][1] * w1 + R[0][2] * w2;
        W[col * 4 + 1] = R[1][0] * w0 + R[1][1] * w1 + R[1][2] * w2;
        W[col * 4 + 2] = R[2][0] * w0 + R[2][1] * w1 + R[2][2] * w2;
    }
}

static inline void compute_bone_d0_and_tip(const we_puppet_physics_ctx_t *ctx, uint32_t k,
                                           const float pivot[3], const float W[16], float out_d0[3],
                                           float *out_L, float out_r_old[3], float out_tip_world[3])
{
    const we_bone_sim_config_t *cfg = &ctx->configs[k];
    const we_bone_sim_state_t *st = &ctx->states[k];

    float d0[3] = {
        W[0] * ctx->rest_dir_local[k][0] + W[4] * ctx->rest_dir_local[k][1] +
            W[8] * ctx->rest_dir_local[k][2],
        W[1] * ctx->rest_dir_local[k][0] + W[5] * ctx->rest_dir_local[k][1] +
            W[9] * ctx->rest_dir_local[k][2],
        W[2] * ctx->rest_dir_local[k][0] + W[6] * ctx->rest_dir_local[k][1] +
            W[10] * ctx->rest_dir_local[k][2],
    };
    float len_d0 = sqrtf(d0[0] * d0[0] + d0[1] * d0[1] + d0[2] * d0[2]);
    if (isfinite(len_d0) && len_d0 > 1e-5f) {
        d0[0] /= len_d0;
        d0[1] /= len_d0;
        d0[2] /= len_d0;
    } else {
        d0[0] = 0.0f;
        d0[1] = 1.0f;
        d0[2] = 0.0f;
    }

    float L = (isfinite(cfg->rest_length_px) && cfg->rest_length_px > 1.0e-3f) ? cfg->rest_length_px
                                                                               : 30.0f;
    float r_old[3];
    if (isfinite(st->pos_curr[0]) && isfinite(st->pos_curr[1]) && isfinite(st->pos_curr[2])) {
        r_old[0] = st->pos_curr[0] - pivot[0];
        r_old[1] = st->pos_curr[1] - pivot[1];
        r_old[2] = st->pos_curr[2] - pivot[2];
    } else {
        r_old[0] = d0[0] * L;
        r_old[1] = d0[1] * L;
        r_old[2] = d0[2] * L;
    }

    float r_len = sqrtf(r_old[0] * r_old[0] + r_old[1] * r_old[1] + r_old[2] * r_old[2]);
    if (isfinite(r_len) && r_len > 1e-4f) {
        float scale_l = L / r_len;
        r_old[0] *= scale_l;
        r_old[1] *= scale_l;
        r_old[2] *= scale_l;
    } else {
        r_old[0] = d0[0] * L;
        r_old[1] = d0[1] * L;
        r_old[2] = d0[2] * L;
    }

    out_d0[0] = d0[0];
    out_d0[1] = d0[1];
    out_d0[2] = d0[2];
    *out_L = L;
    out_r_old[0] = r_old[0];
    out_r_old[1] = r_old[1];
    out_r_old[2] = r_old[2];
    out_tip_world[0] = pivot[0] + r_old[0];
    out_tip_world[1] = pivot[1] + r_old[1];
    out_tip_world[2] = pivot[2] + r_old[2];
}

static void substep_bone_ex(we_puppet_physics_ctx_t *ctx, uint32_t k, const float pivot[3],
                            float W[16], float dt_s, double t, const float pre_wind_vel[3])
{
    if (!ctx || k >= WE_MDL_MAX_BONES || !pivot || !W || !isfinite(dt_s) || dt_s <= 1.0e-6f) {
        return;
    }
    if (!isfinite(pivot[0]) || !isfinite(pivot[1]) || !isfinite(pivot[2])) {
        return;
    }
    if (dt_s > 0.05f) {
        dt_s = 0.05f;
    }

    we_bone_sim_config_t *cfg = &ctx->configs[k];
    we_bone_sim_state_t *st = &ctx->states[k];

    if (!(cfg->flags & WE_BONE_PHYS_ENABLED) || (cfg->flags & WE_BONE_PHYS_PINNED_ROOT)) {
        return;
    }

    float d0[3], r_old[3], tip_world[3];
    float L = 30.0f;
    compute_bone_d0_and_tip(ctx, k, pivot, W, d0, &L, r_old, tip_world);

    st->target_pos[0] = pivot[0] + d0[0] * L;
    st->target_pos[1] = pivot[1] + d0[1] * L;
    st->target_pos[2] = pivot[2] + d0[2] * L;

    if (!isfinite(st->vel[0]) || !isfinite(st->vel[1]) || !isfinite(st->vel[2])) {
        st->vel[0] = 0.0f;
        st->vel[1] = 0.0f;
        st->vel[2] = 0.0f;
    }

    float mass = (isfinite(cfg->mass_kg) && cfg->mass_kg > 0.01f) ? cfg->mass_kg : 0.01f;
    float w = 1.0f / mass;
    float grav_scale = isfinite(cfg->gravity_scale) ? cfg->gravity_scale : 1.0f;
    float gx_world = isfinite(ctx->wind.gravity_vec[0]) ? ctx->wind.gravity_vec[0] : 0.0f;
    float gy_world = isfinite(ctx->wind.gravity_vec[1]) ? ctx->wind.gravity_vec[1] : 0.0f;

    float g_force[3] = {grav_scale * gx_world * mass, grav_scale * gy_world * mass, 0.0f};

    float f_wind[3] = {0.0f, 0.0f, 0.0f};
    float wind_drag = (isfinite(cfg->wind_drag_coeff) && cfg->wind_drag_coeff > 0.0f)
                          ? cfg->wind_drag_coeff
                          : 0.0f;
    if (ctx->wind.enabled && wind_drag > 0.0f) {
        float wind_vel[3];
        if (pre_wind_vel) {
            wind_vel[0] = pre_wind_vel[0];
            wind_vel[1] = pre_wind_vel[1];
            wind_vel[2] = pre_wind_vel[2];
        } else {
            we_physics_eval_curl_wind_3d(&ctx->wind, tip_world[0], tip_world[1], tip_world[2], t,
                                         wind_vel);
        }
        float rel_vx = wind_vel[0] - st->vel[0];
        float rel_vy = wind_vel[1] - st->vel[1];
        float rel_vz = wind_vel[2] - st->vel[2];
        float rel_speed = sqrtf(rel_vx * rel_vx + rel_vy * rel_vy + rel_vz * rel_vz);
        float drag = wind_drag * (rel_speed * 0.05f + 1.0f);
        f_wind[0] = rel_vx * drag;
        f_wind[1] = rel_vy * drag;
        f_wind[2] = rel_vz * drag;
    }

    float f_cursor[3] = {0.0f, 0.0f, 0.0f};
    if (ctx->cursor.enabled && (cfg->flags & WE_BONE_PHYS_CURSOR_COLLIDE) &&
        isfinite(ctx->cursor.radius_px) && ctx->cursor.radius_px > 0.0f) {
        float seg_x = tip_world[0] - pivot[0];
        float seg_y = tip_world[1] - pivot[1];
        float seg_len2 = seg_x * seg_x + seg_y * seg_y;
        float t_seg = 1.0f;
        if (seg_len2 > 1e-6f) {
            t_seg = ((ctx->cursor_pos[0] - pivot[0]) * seg_x +
                     (ctx->cursor_pos[1] - pivot[1]) * seg_y) /
                    seg_len2;
            t_seg = fminf(fmaxf(t_seg, 0.0f), 1.0f);
        }
        float closest_x = pivot[0] + t_seg * seg_x;
        float closest_y = pivot[1] + t_seg * seg_y;
        float dx = closest_x - ctx->cursor_pos[0];
        float dy = closest_y - ctx->cursor_pos[1];
        float dist = sqrtf(dx * dx + dy * dy);
        float cap_r = (isfinite(cfg->collision_radius_px) && cfg->collision_radius_px > 0.0f)
                          ? cfg->collision_radius_px
                          : 0.0f;
        float R = ctx->cursor.radius_px + cap_r;
        if (dist < R) {
            float sdf = fmaxf(0.0f, dist - cap_r);
            float d_norm = fminf(sdf / fmaxf(ctx->cursor.radius_px, 1.0e-3f), 1.0f);
            float p = (isfinite(ctx->cursor.falloff_power) && ctx->cursor.falloff_power > 0.1f)
                          ? fminf(ctx->cursor.falloff_power, 16.0f)
                          : 2.0f;
            float falloff = powf(1.0f - d_norm, p);
            float rdir[2];
            if (dist > 1.0e-4f) {
                rdir[0] = dx / dist;
                rdir[1] = dy / dist;
            } else {
                float nx = -d0[1];
                float ny = d0[0];
                float nlen = sqrtf(nx * nx + ny * ny);
                rdir[0] = (nlen > 1.0e-4f) ? (nx / nlen) : 1.0f;
                rdir[1] = (nlen > 1.0e-4f) ? (ny / nlen) : 0.0f;
            }

            float strength =
                isfinite(ctx->cursor.impulse_strength) ? ctx->cursor.impulse_strength : 0.0f;
            float lever = fmaxf(t_seg, 0.25f);
            float rep = strength * falloff * lever;
            f_cursor[0] += rdir[0] * rep;
            f_cursor[1] += rdir[1] * rep;

            f_cursor[0] += ctx->cursor_vel[0] * falloff * 0.5f * lever;
            f_cursor[1] += ctx->cursor_vel[1] * falloff * 0.5f * lever;

            float v_dot_t = ctx->cursor_vel[0] * (-rdir[1]) + ctx->cursor_vel[1] * rdir[0];
            float vortex_gain =
                0.25f * rep * (v_dot_t > 0.0f ? 1.0f : (v_dot_t < 0.0f ? -1.0f : 0.0f));
            f_cursor[0] += -rdir[1] * vortex_gain;
            f_cursor[1] += rdir[0] * vortex_gain;
        }
    }

    float f_audio[3] = {0.0f, 0.0f, 0.0f};
    if (isfinite(cfg->audio_drag_coeff) && cfg->audio_drag_coeff > 0.0f) {
        float bass = 0.0f;
        for (int b = 0; b < 8; b++) {
            bass += ctx->audio_bands[b];
        }
        bass *= 0.125f;
        float prev_bass = isfinite(ctx->prev_audio_bass) ? ctx->prev_audio_bass : 0.0f;
        float flux = fmaxf(0.0f, bass - prev_bass);
        float impulse = cfg->audio_drag_coeff * flux * 5000.0f;
        f_audio[0] = -d0[1] * impulse;
        f_audio[1] = d0[0] * impulse;
    }

    float f_ext[3] = {g_force[0] + f_wind[0] + f_cursor[0] + f_audio[0],
                      g_force[1] + f_wind[1] + f_cursor[1] + f_audio[1],
                      g_force[2] + f_wind[2] + f_cursor[2] + f_audio[2]};

    float damp_ratio =
        isfinite(cfg->damping_ratio) ? fminf(fmaxf(cfg->damping_ratio, 0.0f), 10.0f) : 0.15f;
    float damp = expf(-damp_ratio * dt_s * 10.0f);
    st->vel[0] = st->vel[0] * damp + dt_s * w * f_ext[0];
    st->vel[1] = st->vel[1] * damp + dt_s * w * f_ext[1];
    st->vel[2] = st->vel[2] * damp + dt_s * w * f_ext[2];

    float r[3] = {r_old[0] + dt_s * st->vel[0], r_old[1] + dt_s * st->vel[1],
                  r_old[2] + dt_s * st->vel[2]};

    st->lambda_stretch = 0.0f;
    st->lambda_bend = 0.0f;

    float comp_stretch = (isfinite(cfg->compliance_stretch) && cfg->compliance_stretch >= 0.0f)
                             ? cfg->compliance_stretch
                             : 0.0f;
    float comp_bend = (isfinite(cfg->compliance_bend) && cfg->compliance_bend >= 0.0f)
                          ? cfg->compliance_bend
                          : 0.0f;

    float inv_dt2 = 1.0f / (dt_s * dt_s);
    float alpha_tilde_dist = comp_stretch * inv_dt2;
    float alpha_tilde_bend = comp_bend * inv_dt2;

    float rayleigh = (isfinite(cfg->rayleigh_damping) && cfg->rayleigh_damping > 0.0f)
                         ? cfg->rayleigh_damping
                         : (damp_ratio * 0.05f);
    float gamma = alpha_tilde_bend * rayleigh * dt_s;

    for (uint32_t iter = 0; iter < WE_PHYSICS_SOLVER_ITERS; iter++) {
        float d_cur = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        if (d_cur > 1e-6f) {
            float c_dist = d_cur - L;
            float grad_dist[3] = {r[0] / d_cur, r[1] / d_cur, r[2] / d_cur};
            float dlambda_dist =
                (-c_dist - alpha_tilde_dist * st->lambda_stretch) / (w + alpha_tilde_dist);
            st->lambda_stretch += dlambda_dist;
            r[0] += w * dlambda_dist * grad_dist[0];
            r[1] += w * dlambda_dist * grad_dist[1];
            r[2] += w * dlambda_dist * grad_dist[2];
        }

        float target_r[3] = {d0[0] * L, d0[1] * L, d0[2] * L};
        float sx = r[0] - target_r[0];
        float sy = r[1] - target_r[1];
        float sz = r[2] - target_r[2];
        float d_spring = sqrtf(sx * sx + sy * sy + sz * sz);
        if (d_spring > 1e-6f) {
            float grad_spring[3] = {sx / d_spring, sy / d_spring, sz / d_spring};
            float v_proj = grad_spring[0] * (r[0] - r_old[0]) + grad_spring[1] * (r[1] - r_old[1]) +
                           grad_spring[2] * (r[2] - r_old[2]);
            float denom = (1.0f + gamma) * w + alpha_tilde_bend;
            float dlambda_bend =
                (-d_spring - alpha_tilde_bend * st->lambda_bend - gamma * v_proj) / denom;
            st->lambda_bend += dlambda_bend;
            r[0] += w * dlambda_bend * grad_spring[0];
            r[1] += w * dlambda_bend * grad_spring[1];
            r[2] += w * dlambda_bend * grad_spring[2];
        }
    }

    float cur_len = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    float cur_dir[3];
    if (isfinite(cur_len) && cur_len > 1e-5f) {
        cur_dir[0] = r[0] / cur_len;
        cur_dir[1] = r[1] / cur_len;
        cur_dir[2] = r[2] / cur_len;
    } else {
        cur_dir[0] = d0[0];
        cur_dir[1] = d0[1];
        cur_dir[2] = d0[2];
    }

    float min_lim = isfinite(cfg->angle_limit_min_rad)
                        ? fminf(fmaxf(cfg->angle_limit_min_rad, -WE_PI), 0.0f)
                        : 0.0f;
    float max_lim = isfinite(cfg->angle_limit_max_rad)
                        ? fminf(fmaxf(cfg->angle_limit_max_rad, 0.0f), WE_PI)
                        : 0.0f;
    bool has_ang_limit =
        (cfg->flags & WE_BONE_PHYS_ANGULAR_LIMIT) != 0u || max_lim > 0.0f || min_lim < 0.0f;
    if ((cfg->flags & WE_BONE_PHYS_ANGULAR_LIMIT) != 0u && max_lim == 0.0f && min_lim == 0.0f) {
        max_lim = 1.04719755f;
        min_lim = -1.04719755f;
    }

    if (has_ang_limit) {
        float vz_sign = d0[0] * cur_dir[1] - d0[1] * cur_dir[0];
        float active_max = (vz_sign < 0.0f) ? (-min_lim) : max_lim;
        if (active_max <= 1.0e-4f) {
            cur_dir[0] = d0[0];
            cur_dir[1] = d0[1];
            cur_dir[2] = d0[2];
        } else if (active_max < WE_PI - 1.0e-4f) {
            float cos_theta = cur_dir[0] * d0[0] + cur_dir[1] * d0[1] + cur_dir[2] * d0[2];
            cos_theta = fminf(fmaxf(cos_theta, -1.0f), 1.0f);
            float cos_max = cosf(active_max);
            if (cos_theta < cos_max) {
                float ax = d0[1] * cur_dir[2] - d0[2] * cur_dir[1];
                float ay = d0[2] * cur_dir[0] - d0[0] * cur_dir[2];
                float az = d0[0] * cur_dir[1] - d0[1] * cur_dir[0];
                float a_len = sqrtf(ax * ax + ay * ay + az * az);
                if (a_len > 1e-5f) {
                    ax /= a_len;
                    ay /= a_len;
                    az /= a_len;
                } else {
                    float u[3];
                    compute_orthogonal_unit_axis(d0, u);
                    ax = u[0];
                    ay = u[1];
                    az = u[2];
                }
                float bx = ay * d0[2] - az * d0[1];
                float by = az * d0[0] - ax * d0[2];
                float bz = ax * d0[1] - ay * d0[0];
                float s_max = sinf(active_max);
                cur_dir[0] = cos_max * d0[0] + s_max * bx;
                cur_dir[1] = cos_max * d0[1] + s_max * by;
                cur_dir[2] = cos_max * d0[2] + s_max * bz;
            }
        }
    }

    r[0] = cur_dir[0] * L;
    r[1] = cur_dir[1] * L;
    r[2] = cur_dir[2] * L;

    st->vel[0] = (r[0] - r_old[0]) / dt_s;
    st->vel[1] = (r[1] - r_old[1]) / dt_s;
    st->vel[2] = (r[2] - r_old[2]) / dt_s;

    st->pos_prev[0] = pivot[0] + r_old[0];
    st->pos_prev[1] = pivot[1] + r_old[1];
    st->pos_prev[2] = pivot[2] + r_old[2];

    st->pos_curr[0] = pivot[0] + r[0];
    st->pos_curr[1] = pivot[1] + r[1];
    st->pos_curr[2] = pivot[2] + r[2];

    apply_shortest_arc_so3(d0, cur_dir, W, &st->current_angle_rad);
}

void we_physics_substep_bone(we_puppet_physics_ctx_t *ctx, uint32_t k, const float pivot[3],
                             float W[16], float dt_s, double t)
{
    substep_bone_ex(ctx, k, pivot, W, dt_s, t, NULL);
}

void we_physics_apply_bone_rotation(const we_puppet_physics_ctx_t *ctx, uint32_t k,
                                    const float pivot[3], float W[16])
{
    if (!ctx || k >= WE_MDL_MAX_BONES || !pivot || !W) {
        return;
    }
    if (!isfinite(pivot[0]) || !isfinite(pivot[1]) || !isfinite(pivot[2])) {
        return;
    }
    const we_bone_sim_state_t *st = &ctx->states[k];
    if (!isfinite(st->pos_curr[0]) || !isfinite(st->pos_curr[1]) || !isfinite(st->pos_curr[2])) {
        return;
    }

    float d0[3] = {
        W[0] * ctx->rest_dir_local[k][0] + W[4] * ctx->rest_dir_local[k][1] +
            W[8] * ctx->rest_dir_local[k][2],
        W[1] * ctx->rest_dir_local[k][0] + W[5] * ctx->rest_dir_local[k][1] +
            W[9] * ctx->rest_dir_local[k][2],
        W[2] * ctx->rest_dir_local[k][0] + W[6] * ctx->rest_dir_local[k][1] +
            W[10] * ctx->rest_dir_local[k][2],
    };
    float len_d0 = sqrtf(d0[0] * d0[0] + d0[1] * d0[1] + d0[2] * d0[2]);
    if (isfinite(len_d0) && len_d0 > 1e-5f) {
        d0[0] /= len_d0;
        d0[1] /= len_d0;
        d0[2] /= len_d0;
    } else {
        d0[0] = 0.0f;
        d0[1] = 1.0f;
        d0[2] = 0.0f;
    }

    float cur_dir[3] = {st->pos_curr[0] - pivot[0], st->pos_curr[1] - pivot[1],
                        st->pos_curr[2] - pivot[2]};
    float len_cur =
        sqrtf(cur_dir[0] * cur_dir[0] + cur_dir[1] * cur_dir[1] + cur_dir[2] * cur_dir[2]);
    if (!isfinite(len_cur) || len_cur <= 1e-5f) {
        return;
    }
    cur_dir[0] /= len_cur;
    cur_dir[1] /= len_cur;
    cur_dir[2] /= len_cur;

    apply_shortest_arc_so3(d0, cur_dir, W, NULL);
}

/* ========================================================================= */
/* 7. Puppet Fixed-Timestep Physics Step                                     */
/* ========================================================================= */

uint32_t we_physics_advance_wall_clock(we_puppet_physics_ctx_t *ctx, double wall_time_sec)
{
    if (!ctx || !ctx->initialized || ctx->active_bone_count == 0u || !isfinite(wall_time_sec)) {
        return 0u;
    }

    float frame_dt = (1.0f / 60.0f);
    if (ctx->has_last_wall_time) {
        double raw_dt = wall_time_sec - ctx->last_wall_time_sec;
        frame_dt = (raw_dt > 0.0) ? (float)fmin(raw_dt, 0.10) : 0.0f;
    }
    ctx->last_wall_time_sec = wall_time_sec;
    ctx->has_last_wall_time = true;

    ctx->time_accumulator_sec += frame_dt;
    float adj_acc = ctx->time_accumulator_sec + 1.0e-5f;
    uint32_t substeps =
        (adj_acc >= WE_PHYSICS_SUBSTEP_DT) ? (uint32_t)(adj_acc / WE_PHYSICS_SUBSTEP_DT) : 0u;
    if (substeps > WE_PHYSICS_MAX_SUBSTEPS) {
        substeps = WE_PHYSICS_MAX_SUBSTEPS;
        ctx->time_accumulator_sec = (float)WE_PHYSICS_MAX_SUBSTEPS * WE_PHYSICS_SUBSTEP_DT;
    }
    return substeps;
}

void we_physics_solve_hierarchy_skin_matrices(we_puppet_physics_ctx_t *ctx,
                                              const we_puppet_t *puppet,
                                              const float (*anim_local)[16], uint32_t substeps,
                                              float (*out_skin_mats)[16])
{
    if (!puppet || !puppet->bones || puppet->bone_count == 0u) {
        return;
    }

    uint32_t bcnt = puppet->bone_count;
    if (bcnt > WE_MDL_MAX_BONES) {
        bcnt = WE_MDL_MAX_BONES;
    }

    float world_mats[WE_MDL_MAX_BONES][16];
    bool phys_active = (ctx != NULL && ctx->initialized && ctx->active_bone_count > 0u);

    if (phys_active && substeps > 0u) {
        /* Group bones by topological depth so sibling chains batch 8-wide AVX2 curl-noise wind
         * samples while still propagating per-substep parent kinematics down every chain. */
        uint8_t depth[WE_MDL_MAX_BONES];
        uint8_t max_depth = 0u;
        bool use_level_batch = (ctx->wind.enabled != 0u && bcnt >= 8u);
        if (use_level_batch) {
            for (uint32_t k = 0; k < bcnt; k++) {
                uint32_t parent = puppet->bones[k].parent;
                uint8_t d =
                    (parent == 0xFFFFFFFFu || parent >= k) ? 0u : (uint8_t)(depth[parent] + 1u);
                depth[k] = d;
                if (d > max_depth) {
                    max_depth = d;
                }
            }
        }

        for (uint32_t s = 0; s < substeps; s++) {
            double cur_time = ctx->total_sim_time_sec;

            if (use_level_batch) {
                for (uint8_t d = 0; d <= max_depth; d++) {
                    uint32_t lvl_bones[WE_MDL_MAX_BONES];
                    uint32_t lvl_cnt = 0u;
                    uint32_t wind_idx_map[WE_MDL_MAX_BONES];
                    float wx[WE_MDL_MAX_BONES], wy[WE_MDL_MAX_BONES], wz[WE_MDL_MAX_BONES];
                    float wind_vels[WE_MDL_MAX_BONES][3];
                    uint32_t wcnt = 0u;

                    for (uint32_t k = 0; k < bcnt; k++) {
                        if (depth[k] != d) {
                            continue;
                        }
                        const float *loc =
                            anim_local ? anim_local[k] : puppet->bones[k].local_trans;
                        uint32_t parent = puppet->bones[k].parent;
                        if (parent == 0xFFFFFFFFu || parent >= k) {
                            memcpy(world_mats[k], loc, sizeof(world_mats[k]));
                        } else {
                            phys_mat4_mul(world_mats[parent], loc, world_mats[k]);
                        }
                        lvl_bones[lvl_cnt] = k;

                        const we_bone_sim_config_t *cfg = &ctx->configs[k];
                        if ((cfg->flags & WE_BONE_PHYS_ENABLED) &&
                            !(cfg->flags & WE_BONE_PHYS_PINNED_ROOT) &&
                            isfinite(cfg->wind_drag_coeff) && cfg->wind_drag_coeff > 0.0f) {
                            float pivot[3] = {world_mats[k][12], world_mats[k][13],
                                              world_mats[k][14]};
                            if (isfinite(pivot[0]) && isfinite(pivot[1]) && isfinite(pivot[2])) {
                                float d0_tmp[3], r_old_tmp[3], tip_tmp[3];
                                float L_tmp = 30.0f;
                                compute_bone_d0_and_tip(ctx, k, pivot, world_mats[k], d0_tmp,
                                                        &L_tmp, r_old_tmp, tip_tmp);
                                wx[wcnt] = tip_tmp[0];
                                wy[wcnt] = tip_tmp[1];
                                wz[wcnt] = tip_tmp[2];
                                wind_idx_map[lvl_cnt] = wcnt++;
                            } else {
                                wind_idx_map[lvl_cnt] = 0xFFFFFFFFu;
                            }
                        } else {
                            wind_idx_map[lvl_cnt] = 0xFFFFFFFFu;
                        }
                        lvl_cnt++;
                    }

                    if (wcnt > 0u) {
                        we_physics_eval_curl_wind_3d_batch(&ctx->wind, wx, wy, wz, wcnt, cur_time,
                                                           wind_vels);
                    }

                    for (uint32_t idx = 0; idx < lvl_cnt; idx++) {
                        uint32_t k = lvl_bones[idx];
                        if (ctx->configs[k].flags & WE_BONE_PHYS_ENABLED) {
                            float pivot[3] = {world_mats[k][12], world_mats[k][13],
                                              world_mats[k][14]};
                            uint32_t w_slot = wind_idx_map[idx];
                            const float *pre_w = (w_slot != 0xFFFFFFFFu) ? wind_vels[w_slot] : NULL;
                            substep_bone_ex(ctx, k, pivot, world_mats[k], WE_PHYSICS_SUBSTEP_DT,
                                            cur_time, pre_w);
                        }
                    }
                }
            } else {
                for (uint32_t k = 0; k < bcnt; k++) {
                    const float *loc = anim_local ? anim_local[k] : puppet->bones[k].local_trans;
                    uint32_t parent = puppet->bones[k].parent;
                    if (parent == 0xFFFFFFFFu || parent >= k) {
                        memcpy(world_mats[k], loc, sizeof(world_mats[k]));
                    } else {
                        phys_mat4_mul(world_mats[parent], loc, world_mats[k]);
                    }

                    if (ctx->configs[k].flags & WE_BONE_PHYS_ENABLED) {
                        float pivot[3] = {world_mats[k][12], world_mats[k][13], world_mats[k][14]};
                        substep_bone_ex(ctx, k, pivot, world_mats[k], WE_PHYSICS_SUBSTEP_DT,
                                        cur_time, NULL);
                    }
                }
            }

            ctx->time_accumulator_sec =
                fmaxf(0.0f, ctx->time_accumulator_sec - WE_PHYSICS_SUBSTEP_DT);
            ctx->total_sim_time_sec += (double)WE_PHYSICS_SUBSTEP_DT;
        }

        float bass = 0.0f;
        for (int b = 0; b < 8; b++) {
            bass += ctx->audio_bands[b];
        }
        ctx->prev_audio_bass = bass * 0.125f;
    } else if (out_skin_mats) {
        for (uint32_t k = 0; k < bcnt; k++) {
            const float *loc = anim_local ? anim_local[k] : puppet->bones[k].local_trans;
            uint32_t parent = puppet->bones[k].parent;
            if (parent == 0xFFFFFFFFu || parent >= k) {
                memcpy(world_mats[k], loc, sizeof(world_mats[k]));
            } else {
                phys_mat4_mul(world_mats[parent], loc, world_mats[k]);
            }
            if (phys_active && (ctx->configs[k].flags & WE_BONE_PHYS_ENABLED)) {
                float pivot[3] = {world_mats[k][12], world_mats[k][13], world_mats[k][14]};
                we_physics_apply_bone_rotation(ctx, k, pivot, world_mats[k]);
            }
        }
    }

    if (out_skin_mats) {
        for (uint32_t k = 0; k < bcnt; k++) {
            phys_mat4_mul(world_mats[k], puppet->bones[k].offset_trans, out_skin_mats[k]);
        }
    }
}

void we_physics_step_puppet(we_puppet_physics_ctx_t *ctx, const we_puppet_t *puppet,
                            float frame_dt_sec, float (*inout_skin_mats)[16])
{
    if (!ctx || !ctx->initialized || ctx->active_bone_count == 0 || !puppet) {
        return;
    }

    float dt = isfinite(frame_dt_sec) ? fminf(fmaxf(frame_dt_sec, 0.0f), 0.10f) : 0.0f;
    ctx->time_accumulator_sec += dt;

    /* Guard rational refresh rates (e.g. 144 Hz = 5/3 of 240 Hz) against float32 ULP truncation */
    float adj_acc = ctx->time_accumulator_sec + 1.0e-5f;
    uint32_t substeps =
        (adj_acc >= WE_PHYSICS_SUBSTEP_DT) ? (uint32_t)(adj_acc / WE_PHYSICS_SUBSTEP_DT) : 0u;
    if (substeps > WE_PHYSICS_MAX_SUBSTEPS) {
        substeps = WE_PHYSICS_MAX_SUBSTEPS;
        ctx->time_accumulator_sec = (float)WE_PHYSICS_MAX_SUBSTEPS * WE_PHYSICS_SUBSTEP_DT;
    }

    we_physics_solve_hierarchy_skin_matrices(ctx, puppet, NULL, substeps, inout_skin_mats);
}
