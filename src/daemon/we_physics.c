// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/we_physics.h"
#include "waywal/we_puppet.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        if (endptr != cur) {
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

/* Analytic 3D Simplex Noise value + gradient evaluation (Perlin 2001 / Stefan Gustavson)
 * Computes Psi_m(p) and dPsi_m/dx, dPsi_m/dy, dPsi_m/dz in a single pass without finite
 * differences. */
static inline float simplex3d_grad(float x, float y, float z, float *gx, float *gy, float *gz)
{
    const float F3 = 0.333333333333f;
    const float G3 = 0.166666666667f;
    float s = (x + y + z) * F3;
    int i = (int)floorf(x + s), j = (int)floorf(y + s), k = (int)floorf(z + s);
    float t = (float)(i + j + k) * G3;
    float x0 = x - ((float)i - t), y0 = y - ((float)j - t), z0 = z - ((float)k - t);

    int i1, j1, k1, i2, j2, k2;
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

    int gi[4] = {
        (int)((uint32_t)(i * 1664525u ^ j * 1013904223u ^ k * 2654435761u) & 11u),
        (int)((uint32_t)((i + i1) * 1664525u ^ (j + j1) * 1013904223u ^ (k + k1) * 2654435761u) &
              11u),
        (int)((uint32_t)((i + i2) * 1664525u ^ (j + j2) * 1013904223u ^ (k + k2) * 2654435761u) &
              11u),
        (int)((uint32_t)((i + 1) * 1664525u ^ (j + 1) * 1013904223u ^ (k + 1) * 2654435761u) &
              11u)};

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

    float v_base = wind->base_speed_px_s;
    float dir = wind->base_direction_rad;
    float base_u = v_base * cosf(dir);
    float base_v = v_base * sinf(dir);

    float scale = (wind->curl_spatial_scale > 1e-6f) ? wind->curl_spatial_scale : 0.008f;
    float freq = (wind->gust_frequency_hz > 0.0f) ? wind->gust_frequency_hz : 0.5f;
    float t = (float)(time_sec * (double)freq);

    float sx = x * scale - t * 0.45f;
    float sy = y * scale;
    float sz = z * scale + t * 0.25f;

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

    float gust = wind->gust_amplitude;
    float turb_gain = gust * (v_base * 0.5f + 40.0f);

    out_vel[0] = base_u + curl_x * turb_gain;
    out_vel[1] = base_v + curl_y * turb_gain;
    out_vel[2] = curl_z * turb_gain;
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
    ctx->cursor_pos[0] = x;
    ctx->cursor_pos[1] = y;
    ctx->cursor_vel[0] = vx;
    ctx->cursor_vel[1] = vy;
}

void we_physics_set_audio_bands(we_puppet_physics_ctx_t *ctx, const float bands[64])
{
    if (!ctx || !bands) {
        return;
    }
    memcpy(ctx->audio_bands, bands, sizeof(ctx->audio_bands));
}

/* ========================================================================= */
/* 5. Skeleton Physics Initialization                                        */
/* ========================================================================= */

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

    for (uint32_t k = 0; k < bcnt; k++) {
        const we_mdl_bone_t *b = &puppet->bones[k];
        we_bone_sim_config_t *cfg = &ctx->configs[k];
        cfg->bone_index = k;

        /* Parse simulation_json if present in MDL bone */
        if (b->simulation_json[0] != '\0') {
            we_physics_parse_simulation_json(b->simulation_json, cfg);
        }

        /* Determine rest length and local rest direction */
        float pivot[3] = {b->world_bind[12], b->world_bind[13], b->world_bind[14]};
        float tip_dir_world[3] = {0.0f, 1.0f, 0.0f};
        float rest_len = cfg->rest_length_px;

        /* First look for a child bone to determine rest direction */
        bool found_child = false;
        for (uint32_t c = 0; c < bcnt; c++) {
            if (puppet->bones[c].parent == k) {
                float cpivot[3] = {puppet->bones[c].world_bind[12], puppet->bones[c].world_bind[13],
                                   puppet->bones[c].world_bind[14]};
                float dx = cpivot[0] - pivot[0];
                float dy = cpivot[1] - pivot[1];
                float dz = cpivot[2] - pivot[2];
                float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                if (dist > 1.0f) {
                    tip_dir_world[0] = dx / dist;
                    tip_dir_world[1] = dy / dist;
                    tip_dir_world[2] = dz / dist;
                    if (rest_len <= 0.0f) {
                        rest_len = dist;
                    }
                    found_child = true;
                    break;
                }
            }
        }

        /* If leaf bone, continue along parent vector if available */
        if (!found_child && b->parent != 0xFFFFFFFFu && b->parent < bcnt) {
            uint32_t p = b->parent;
            float ppivot[3] = {puppet->bones[p].world_bind[12], puppet->bones[p].world_bind[13],
                               puppet->bones[p].world_bind[14]};
            float dx = pivot[0] - ppivot[0];
            float dy = pivot[1] - ppivot[1];
            float dz = pivot[2] - ppivot[2];
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
        float rx = b->world_bind[0] * tip_dir_world[0] + b->world_bind[1] * tip_dir_world[1] +
                   b->world_bind[2] * tip_dir_world[2];
        float ry = b->world_bind[4] * tip_dir_world[0] + b->world_bind[5] * tip_dir_world[1] +
                   b->world_bind[6] * tip_dir_world[2];
        float rz = b->world_bind[8] * tip_dir_world[0] + b->world_bind[9] * tip_dir_world[1] +
                   b->world_bind[10] * tip_dir_world[2];
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

void we_physics_substep_bone(we_puppet_physics_ctx_t *ctx, uint32_t k, const float pivot[3],
                             float W[16], float dt_s, double t)
{
    we_bone_sim_config_t *cfg = &ctx->configs[k];
    we_bone_sim_state_t *st = &ctx->states[k];

    if (!(cfg->flags & WE_BONE_PHYS_ENABLED) || (cfg->flags & WE_BONE_PHYS_PINNED_ROOT)) {
        return;
    }

    /* 1. Compute animated rest tip direction d0 and rest target position x^target */
    float d0[3] = {
        W[0] * ctx->rest_dir_local[k][0] + W[4] * ctx->rest_dir_local[k][1] +
            W[8] * ctx->rest_dir_local[k][2],
        W[1] * ctx->rest_dir_local[k][0] + W[5] * ctx->rest_dir_local[k][1] +
            W[9] * ctx->rest_dir_local[k][2],
        W[2] * ctx->rest_dir_local[k][0] + W[6] * ctx->rest_dir_local[k][1] +
            W[10] * ctx->rest_dir_local[k][2],
    };
    float len_d0 = sqrtf(d0[0] * d0[0] + d0[1] * d0[1] + d0[2] * d0[2]);
    if (len_d0 > 1e-5f) {
        d0[0] /= len_d0;
        d0[1] /= len_d0;
        d0[2] /= len_d0;
    } else {
        d0[0] = 0.0f;
        d0[1] = 1.0f;
        d0[2] = 0.0f;
    }

    float L = cfg->rest_length_px > 0.0f ? cfg->rest_length_px : 30.0f;
    float x_target[3] = {pivot[0] + d0[0] * L, pivot[1] + d0[1] * L, pivot[2] + d0[2] * L};
    st->target_pos[0] = x_target[0];
    st->target_pos[1] = x_target[1];
    st->target_pos[2] = x_target[2];

    /* 2. Determine current relative tip direction from previous state */
    float r_old[3] = {st->pos_curr[0] - st->target_pos[0] + d0[0] * L,
                      st->pos_curr[1] - st->target_pos[1] + d0[1] * L,
                      st->pos_curr[2] - st->target_pos[2] + d0[2] * L};
    float r_len = sqrtf(r_old[0] * r_old[0] + r_old[1] * r_old[1] + r_old[2] * r_old[2]);
    if (r_len > 1e-4f) {
        r_old[0] = (r_old[0] / r_len) * L;
        r_old[1] = (r_old[1] / r_len) * L;
        r_old[2] = (r_old[2] / r_len) * L;
    } else {
        r_old[0] = d0[0] * L;
        r_old[1] = d0[1] * L;
        r_old[2] = d0[2] * L;
    }

    /* Inverse tip mass */
    float mass = cfg->mass_kg > 0.01f ? cfg->mass_kg : 0.01f;
    float w = 1.0f / mass;

    /* 3. External forces at tip position */
    float tip_world[3] = {pivot[0] + r_old[0], pivot[1] + r_old[1], pivot[2] + r_old[2]};

    /* Gravity */
    float g_force[3] = {cfg->gravity_scale * ctx->wind.gravity_vec[0] * mass,
                        cfg->gravity_scale * ctx->wind.gravity_vec[1] * mass, 0.0f};

    /* 3D Divergence-Free Curl Wind */
    float f_wind[3] = {0.0f, 0.0f, 0.0f};
    if (ctx->wind.enabled && cfg->wind_drag_coeff > 0.0f) {
        float wind_vel[3];
        we_physics_eval_curl_wind_3d(&ctx->wind, tip_world[0], tip_world[1], tip_world[2], t,
                                     wind_vel);
        float rel_vx = wind_vel[0] - st->vel[0];
        float rel_vy = wind_vel[1] - st->vel[1];
        float rel_vz = wind_vel[2] - st->vel[2];
        float rel_speed = sqrtf(rel_vx * rel_vx + rel_vy * rel_vy + rel_vz * rel_vz);
        float drag = cfg->wind_drag_coeff * (rel_speed * 0.05f + 1.0f);
        f_wind[0] = rel_vx * drag;
        f_wind[1] = rel_vy * drag;
        f_wind[2] = rel_vz * drag;
    }

    /* Cursor SDF force */
    float f_cursor[3] = {0.0f, 0.0f, 0.0f};
    if (ctx->cursor.enabled && (cfg->flags & WE_BONE_PHYS_CURSOR_COLLIDE)) {
        float dx = tip_world[0] - ctx->cursor_pos[0];
        float dy = tip_world[1] - ctx->cursor_pos[1];
        float dist = sqrtf(dx * dx + dy * dy);
        float R = ctx->cursor.radius_px > 1.0f ? ctx->cursor.radius_px : 120.0f;
        if (dist < R) {
            float d_norm = dist / R;
            float p = ctx->cursor.falloff_power > 0.1f ? ctx->cursor.falloff_power : 2.0f;
            float falloff = powf(1.0f - d_norm, p);
            float inv_d = 1.0f / (dist + 1e-4f);
            float rdir[2] = {dx * inv_d, dy * inv_d};

            float rep = ctx->cursor.impulse_strength * falloff;
            f_cursor[0] += rdir[0] * rep;
            f_cursor[1] += rdir[1] * rep;

            f_cursor[0] += ctx->cursor_vel[0] * falloff * 0.5f;
            f_cursor[1] += ctx->cursor_vel[1] * falloff * 0.5f;

            float v_dot_t = ctx->cursor_vel[0] * (-rdir[1]) + ctx->cursor_vel[1] * rdir[0];
            float vortex_gain =
                0.25f * rep * (v_dot_t > 0.0f ? 1.0f : (v_dot_t < 0.0f ? -1.0f : 0.0f));
            f_cursor[0] += -rdir[1] * vortex_gain;
            f_cursor[1] += rdir[0] * vortex_gain;
        }
    }

    /* Audio impulse force */
    float f_audio[3] = {0.0f, 0.0f, 0.0f};
    if (cfg->audio_drag_coeff > 0.0f) {
        float bass = 0.0f;
        for (int b = 0; b < 8; b++) {
            bass += ctx->audio_bands[b];
        }
        bass *= 0.125f;
        float flux = fmaxf(0.0f, bass - ctx->prev_audio_bass);
        float impulse = cfg->audio_drag_coeff * flux * 5000.0f;
        f_audio[0] = -d0[1] * impulse;
        f_audio[1] = d0[0] * impulse;
    }

    float f_ext[3] = {g_force[0] + f_wind[0] + f_cursor[0] + f_audio[0],
                      g_force[1] + f_wind[1] + f_cursor[1] + f_audio[1],
                      g_force[2] + f_wind[2] + f_cursor[2] + f_audio[2]};

    /* 4. Symplectic Euler prediction */
    float damp = expf(-cfg->damping_ratio * dt_s * 10.0f);
    st->vel[0] = st->vel[0] * damp + dt_s * w * f_ext[0];
    st->vel[1] = st->vel[1] * damp + dt_s * w * f_ext[1];
    st->vel[2] = st->vel[2] * damp + dt_s * w * f_ext[2];

    float r[3] = {r_old[0] + dt_s * st->vel[0], r_old[1] + dt_s * st->vel[1],
                  r_old[2] + dt_s * st->vel[2]};

    st->lambda_stretch = 0.0f;
    st->lambda_bend = 0.0f;

    /* Time-scaled compliance */
    float inv_dt2 = 1.0f / (dt_s * dt_s);
    float alpha_tilde_dist = cfg->compliance_stretch * inv_dt2;
    float alpha_tilde_bend = cfg->compliance_bend * inv_dt2;

    float rayleigh =
        (cfg->rayleigh_damping > 0.0f) ? cfg->rayleigh_damping : (cfg->damping_ratio * 0.05f);
    float gamma = alpha_tilde_bend * rayleigh * dt_s;

    /* 5. XPBD Solver Iterations */
    for (uint32_t iter = 0; iter < WE_PHYSICS_SOLVER_ITERS; iter++) {
        /* Inextensible rod constraint: ||r|| - L = 0 */
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

        /* Rest-pose angular spring: ||r - L*d0|| = 0 with Kelvin-Voigt damping */
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

    /* 6. Hard angular cone limit (theta_max) */
    float cur_len = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    float cur_dir[3];
    if (cur_len > 1e-5f) {
        cur_dir[0] = r[0] / cur_len;
        cur_dir[1] = r[1] / cur_len;
        cur_dir[2] = r[2] / cur_len;
    } else {
        cur_dir[0] = d0[0];
        cur_dir[1] = d0[1];
        cur_dir[2] = d0[2];
    }

    float max_angle = cfg->angle_limit_max_rad;
    if (max_angle <= 0.01f && (cfg->flags & WE_BONE_PHYS_ANGULAR_LIMIT)) {
        max_angle = 1.04719755f; /* 60 deg */
    }

    if (max_angle > 0.01f) {
        float cos_theta = cur_dir[0] * d0[0] + cur_dir[1] * d0[1] + cur_dir[2] * d0[2];
        cos_theta = fminf(fmaxf(cos_theta, -1.0f), 1.0f);
        float cos_max = cosf(max_angle);
        if (cos_theta < cos_max) {
            float ax = d0[1] * cur_dir[2] - d0[2] * cur_dir[1];
            float ay = d0[2] * cur_dir[0] - d0[0] * cur_dir[2];
            float az = d0[0] * cur_dir[1] - d0[1] * cur_dir[0];
            float a_len = sqrtf(ax * ax + ay * ay + az * az);
            if (a_len > 1e-5f) {
                ax /= a_len;
                ay /= a_len;
                az /= a_len;
                float bx = ay * d0[2] - az * d0[1];
                float by = az * d0[0] - ax * d0[2];
                float bz = ax * d0[1] - ay * d0[0];
                float s_max = sinf(max_angle);
                cur_dir[0] = cos_max * d0[0] + s_max * bx;
                cur_dir[1] = cos_max * d0[1] + s_max * by;
                cur_dir[2] = cos_max * d0[2] + s_max * bz;
            } else {
                cur_dir[0] = d0[0];
                cur_dir[1] = d0[1];
                cur_dir[2] = d0[2];
            }
        }
    }

    /* Final projected offset */
    r[0] = cur_dir[0] * L;
    r[1] = cur_dir[1] * L;
    r[2] = cur_dir[2] * L;

    /* 7. Velocity update and state update */
    st->vel[0] = (r[0] - r_old[0]) / dt_s;
    st->vel[1] = (r[1] - r_old[1]) / dt_s;
    st->vel[2] = (r[2] - r_old[2]) / dt_s;

    st->pos_prev[0] = pivot[0] + r_old[0];
    st->pos_prev[1] = pivot[1] + r_old[1];
    st->pos_prev[2] = pivot[2] + r_old[2];

    st->pos_curr[0] = pivot[0] + r[0];
    st->pos_curr[1] = pivot[1] + r[1];
    st->pos_curr[2] = pivot[2] + r[2];

    /* 8. Shortest-arc SO(3) rotation from d0 to cur_dir */
    float v_cross[3] = {d0[1] * cur_dir[2] - d0[2] * cur_dir[1],
                        d0[2] * cur_dir[0] - d0[0] * cur_dir[2],
                        d0[0] * cur_dir[1] - d0[1] * cur_dir[0]};
    float c = d0[0] * cur_dir[0] + d0[1] * cur_dir[1] + d0[2] * cur_dir[2];
    c = fminf(fmaxf(c, -0.9999f), 1.0f);

    st->current_angle_rad = atan2f(v_cross[2], c);

    float R[3][3];
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

    for (int col = 0; col < 3; col++) {
        float w0 = W[col * 4 + 0];
        float w1 = W[col * 4 + 1];
        float w2 = W[col * 4 + 2];
        W[col * 4 + 0] = R[0][0] * w0 + R[0][1] * w1 + R[0][2] * w2;
        W[col * 4 + 1] = R[1][0] * w0 + R[1][1] * w1 + R[1][2] * w2;
        W[col * 4 + 2] = R[2][0] * w0 + R[2][1] * w1 + R[2][2] * w2;
    }
}

void we_physics_apply_bone_rotation(const we_puppet_physics_ctx_t *ctx, uint32_t k,
                                    const float pivot[3], float W[16])
{
    if (!ctx || k >= WE_MDL_MAX_BONES) {
        return;
    }
    const we_bone_sim_state_t *st = &ctx->states[k];

    /* Rest direction d0 from rest_dir_local */
    float d0[3] = {
        W[0] * ctx->rest_dir_local[k][0] + W[4] * ctx->rest_dir_local[k][1] +
            W[8] * ctx->rest_dir_local[k][2],
        W[1] * ctx->rest_dir_local[k][0] + W[5] * ctx->rest_dir_local[k][1] +
            W[9] * ctx->rest_dir_local[k][2],
        W[2] * ctx->rest_dir_local[k][0] + W[6] * ctx->rest_dir_local[k][1] +
            W[10] * ctx->rest_dir_local[k][2],
    };
    float len_d0 = sqrtf(d0[0] * d0[0] + d0[1] * d0[1] + d0[2] * d0[2]);
    if (len_d0 > 1e-5f) {
        d0[0] /= len_d0;
        d0[1] /= len_d0;
        d0[2] /= len_d0;
    } else {
        d0[0] = 0.0f;
        d0[1] = 1.0f;
        d0[2] = 0.0f;
    }

    /* Current direction from pivot to pos_curr */
    float cur_dir[3] = {st->pos_curr[0] - pivot[0], st->pos_curr[1] - pivot[1],
                        st->pos_curr[2] - pivot[2]};
    float len_cur =
        sqrtf(cur_dir[0] * cur_dir[0] + cur_dir[1] * cur_dir[1] + cur_dir[2] * cur_dir[2]);
    if (len_cur <= 1e-5f) {
        return;
    }
    cur_dir[0] /= len_cur;
    cur_dir[1] /= len_cur;
    cur_dir[2] /= len_cur;

    /* Shortest-arc SO(3) rotation from d0 to cur_dir */
    float v_cross[3] = {d0[1] * cur_dir[2] - d0[2] * cur_dir[1],
                        d0[2] * cur_dir[0] - d0[0] * cur_dir[2],
                        d0[0] * cur_dir[1] - d0[1] * cur_dir[0]};
    float c = d0[0] * cur_dir[0] + d0[1] * cur_dir[1] + d0[2] * cur_dir[2];
    c = fminf(fmaxf(c, -0.9999f), 1.0f);

    float R[3][3];
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

    for (int col = 0; col < 3; col++) {
        float w0 = W[col * 4 + 0];
        float w1 = W[col * 4 + 1];
        float w2 = W[col * 4 + 2];
        W[col * 4 + 0] = R[0][0] * w0 + R[0][1] * w1 + R[0][2] * w2;
        W[col * 4 + 1] = R[1][0] * w0 + R[1][1] * w1 + R[1][2] * w2;
        W[col * 4 + 2] = R[2][0] * w0 + R[2][1] * w1 + R[2][2] * w2;
    }
}

/* ========================================================================= */
/* 7. Puppet Fixed-Timestep Physics Step                                     */
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

void we_physics_step_puppet(we_puppet_physics_ctx_t *ctx, const we_puppet_t *puppet,
                            float frame_dt_sec, float (*inout_skin_mats)[16])
{
    if (!ctx || !ctx->initialized || ctx->active_bone_count == 0 || !puppet) {
        return;
    }

    float dt = fminf(fmaxf(frame_dt_sec, 0.0f), 0.10f);
    ctx->time_accumulator_sec += dt;

    uint32_t bcnt = puppet->bone_count;
    if (bcnt > WE_MDL_MAX_BONES) {
        bcnt = WE_MDL_MAX_BONES;
    }

    /* Guard rational refresh rates (e.g. 144 Hz = 5/3 of 240 Hz) against float32 ULP truncation */
    float adj_acc = ctx->time_accumulator_sec + 1.0e-5f;
    uint32_t substeps =
        (adj_acc >= WE_PHYSICS_SUBSTEP_DT) ? (uint32_t)(adj_acc / WE_PHYSICS_SUBSTEP_DT) : 0u;
    if (substeps > WE_PHYSICS_MAX_SUBSTEPS) {
        substeps = WE_PHYSICS_MAX_SUBSTEPS;
        ctx->time_accumulator_sec = (float)WE_PHYSICS_MAX_SUBSTEPS * WE_PHYSICS_SUBSTEP_DT;
    }

    float world_mats[WE_MDL_MAX_BONES][16];

    for (uint32_t s = 0; s < substeps; s++) {
        double cur_time = ctx->total_sim_time_sec;

        /* Forward topological traversal down bone hierarchy */
        for (uint32_t k = 0; k < bcnt; k++) {
            uint32_t parent = puppet->bones[k].parent;
            if (parent == 0xFFFFFFFFu || parent >= k) {
                memcpy(world_mats[k], puppet->bones[k].local_trans, sizeof(world_mats[k]));
            } else {
                phys_mat4_mul(world_mats[parent], puppet->bones[k].local_trans, world_mats[k]);
            }

            if (ctx->configs[k].flags & WE_BONE_PHYS_ENABLED) {
                float pivot[3] = {world_mats[k][12], world_mats[k][13], world_mats[k][14]};
                we_physics_substep_bone(ctx, k, pivot, world_mats[k], WE_PHYSICS_SUBSTEP_DT,
                                        cur_time);
            }
        }

        ctx->time_accumulator_sec -= WE_PHYSICS_SUBSTEP_DT;
        ctx->total_sim_time_sec += (double)WE_PHYSICS_SUBSTEP_DT;
    }

    /* Post-multiply world matrices by inverse bind matrices into inout_skin_mats */
    if (inout_skin_mats) {
        if (substeps == 0) {
            for (uint32_t k = 0; k < bcnt; k++) {
                uint32_t parent = puppet->bones[k].parent;
                if (parent == 0xFFFFFFFFu || parent >= k) {
                    memcpy(world_mats[k], puppet->bones[k].local_trans, sizeof(world_mats[k]));
                } else {
                    phys_mat4_mul(world_mats[parent], puppet->bones[k].local_trans, world_mats[k]);
                }
                if (ctx->configs[k].flags & WE_BONE_PHYS_ENABLED) {
                    float pivot[3] = {world_mats[k][12], world_mats[k][13], world_mats[k][14]};
                    we_physics_apply_bone_rotation(ctx, k, pivot, world_mats[k]);
                }
            }
        }
        for (uint32_t k = 0; k < bcnt; k++) {
            phys_mat4_mul(world_mats[k], puppet->bones[k].offset_trans, inout_skin_mats[k]);
        }
    }

    /* Update bass tracking for audio transient flux */
    float bass = 0.0f;
    for (int b = 0; b < 8; b++) {
        bass += ctx->audio_bands[b];
    }
    ctx->prev_audio_bass = bass * 0.125f;
}
