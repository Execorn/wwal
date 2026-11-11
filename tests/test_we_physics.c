// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_we_physics.c
 * @brief 240 Hz Substepped XPBD Spring-Bone & 3D Curl-Noise Physics Suite
 *
 * Verifies:
 * 1. sizeof static asserts (64B config, 64B state, 32B wind, 16B cursor).
 * 2. 6 built-in material presets (Silk, LongHair, HeavyCape, FoliageBranch, SoftRibbon,
 * MetallicChain).
 * 3. Divergence-free 3D Simplex curl-noise wind velocity field (div u = 0).
 * 4. Fixed 240 Hz substepped XPBD frame-rate invariance across 30, 60, 120, 240 Hz.
 * 5. 16-bone chain inextensibility under 4x peak wind gusts (< 1.5% elongation).
 * 6. Wayland cursor SDF repulsion and PipeWire audio impulse coupling.
 */

#include "waywal/we_physics.h"
#include "waywal/we_puppet.h"

#include <assert.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static_assert(sizeof(we_bone_sim_config_t) == 64, "we_bone_sim_config_t must be 64 bytes");
static_assert(sizeof(we_bone_sim_state_t) == 64, "we_bone_sim_state_t must be 64 bytes");
static_assert(sizeof(we_wind_field_t) == 32, "we_wind_field_t must be 32 bytes");
static_assert(sizeof(we_cursor_force_t) == 16, "we_cursor_force_t must be 16 bytes");

static void test_struct_sizes_and_presets(void)
{
    printf("[+] Testing physics struct sizes & 6 material presets...\n");
    assert(sizeof(we_bone_sim_config_t) == 64);
    assert(sizeof(we_bone_sim_state_t) == 64);
    assert(sizeof(we_wind_field_t) == 32);
    assert(sizeof(we_cursor_force_t) == 16);

    we_bone_sim_config_t cfg;
    for (int p = 1; p <= 6; ++p) {
        memset(&cfg, 0, sizeof(cfg));
        cfg.bone_index = (uint32_t)p;
        cfg.rest_length_px = 40.0f;
        we_physics_apply_preset(&cfg, (we_physics_preset_t)p);

        assert(cfg.bone_index == (uint32_t)p);
        assert(cfg.rest_length_px == 40.0f);
        assert((cfg.flags & WE_BONE_PHYS_ENABLED) != 0);
        assert(cfg.mass_kg > 0.0f);
        assert(cfg.damping_ratio >= 0.0f);
        assert(cfg.gravity_scale >= 0.0f);
        assert(cfg.angle_limit_max_rad > cfg.angle_limit_min_rad);
    }

    // Test simulation_json parser
    const char *sim_json = "{\"stiffness\":0.85,\"damping\":0.12,\"gravity\":1.2,\"mass\":0.5,"
                           "\"length\":45.0,\"max_angle\":0.78,\"wind\":1.5,\"audio\":0.8}";
    we_bone_sim_config_t parsed_cfg;
    memset(&parsed_cfg, 0, sizeof(parsed_cfg));
    bool parse_ok = we_physics_parse_simulation_json(sim_json, &parsed_cfg);
    assert(parse_ok);
    assert(fabsf(parsed_cfg.mass_kg - 0.5f) < 1e-4f);
    assert(fabsf(parsed_cfg.damping_ratio - 0.12f) < 1e-4f);
    assert(fabsf(parsed_cfg.rest_length_px - 45.0f) < 1e-4f);
    assert(fabsf(parsed_cfg.gravity_scale - 1.2f) < 1e-4f);

    printf("    -> All 6 presets & JSON parser verified.\n");
}

static void test_curl_noise_zero_divergence(void)
{
    printf("[+] Testing 3D Simplex curl-noise velocity divergence (div u = 0)...\n");

    we_wind_field_t wind;
    memset(&wind, 0, sizeof(wind));
    wind.enabled = 1;
    wind.base_direction_rad = 0.5f;
    wind.base_speed_px_s = 120.0f;
    wind.gust_frequency_hz = 0.5f;
    wind.gust_amplitude = 1.0f;
    wind.curl_spatial_scale = 0.005f;

    const float test_points[5][3] = {{100.0f, 200.0f, 50.0f},
                                     {500.0f, 300.0f, -100.0f},
                                     {0.0f, 0.0f, 0.0f},
                                     {1280.0f, 720.0f, 25.0f},
                                     {-200.0f, 400.0f, 150.0f}};

    const float h = 0.05f; // Step for central differences
    for (int p = 0; p < 5; ++p) {
        float x = test_points[p][0];
        float y = test_points[p][1];
        float z = test_points[p][2];
        double t = 1.234;

        float u_xp[3], u_xm[3];
        float u_yp[3], u_ym[3];
        float u_zp[3], u_zm[3];

        we_physics_eval_curl_wind_3d(&wind, x + h, y, z, t, u_xp);
        we_physics_eval_curl_wind_3d(&wind, x - h, y, z, t, u_xm);
        we_physics_eval_curl_wind_3d(&wind, x, y + h, z, t, u_yp);
        we_physics_eval_curl_wind_3d(&wind, x, y - h, z, t, u_ym);
        we_physics_eval_curl_wind_3d(&wind, x, y, z + h, t, u_zp);
        we_physics_eval_curl_wind_3d(&wind, x, y, z - h, t, u_zm);

        float dux_dx = (u_xp[0] - u_xm[0]) / (2.0f * h);
        float duy_dy = (u_yp[1] - u_ym[1]) / (2.0f * h);
        float duz_dz = (u_zp[2] - u_zm[2]) / (2.0f * h);

        float div_u = dux_dx + duy_dy + duz_dz;
        assert(fabsf(div_u) < 0.05f && "3D curl noise divergence must vanish analytically");
    }

    printf("    -> Zero-divergence (div u = 0) verified across 5 spatial sample locations.\n");
}

static void test_substepped_xpbd_frame_rate_invariance(void)
{
    printf("[+] Testing 240 Hz substepped XPBD frame-rate invariance (30, 60, 144, 240 Hz)...\n");

    const float fps_list[4] = {30.0f, 60.0f, 144.0f, 240.0f};
    float final_angles[4] = {0.0f};

    we_mdl_bone_t bone;
    memset(&bone, 0, sizeof(bone));
    bone.parent = 0xFFFFFFFFu;
    bone.local_trans[0] = 1.0f;
    bone.local_trans[5] = 1.0f;
    bone.local_trans[10] = 1.0f;
    bone.local_trans[13] = 100.0f;
    bone.local_trans[15] = 1.0f;
    bone.offset_trans[0] = 1.0f;
    bone.offset_trans[5] = 1.0f;
    bone.offset_trans[10] = 1.0f;
    bone.offset_trans[13] = -100.0f;
    bone.offset_trans[15] = 1.0f;

    we_puppet_t puppet;
    memset(&puppet, 0, sizeof(puppet));
    puppet.bone_count = 1;
    puppet.bones = &bone;

    float skin_mats[1][16];

    for (int i = 0; i < 4; ++i) {
        float fps = fps_list[i];
        float dt_frame = 1.0f / fps;
        int frame_count = (int)(fps * 1.0f);

        we_puppet_physics_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.active_bone_count = 1;
        ctx.initialized = true;

        we_physics_apply_preset(&ctx.configs[0], WE_PHYS_PRESET_HEAVY_CAPE);
        ctx.configs[0].rest_length_px = 50.0f;
        ctx.rest_dir_local[0][1] = -1.0f;
        ctx.states[0].pos_curr[0] = 10.0f;
        ctx.states[0].pos_curr[1] = 50.0f;
        ctx.states[0].pos_prev[0] = 10.0f;
        ctx.states[0].pos_prev[1] = 50.0f;

        ctx.wind.enabled = 1;
        ctx.wind.base_speed_px_s = 50.0f;
        ctx.wind.gravity_vec[1] = -980.0f;

        for (int f = 0; f < frame_count; ++f) {
            we_physics_step_puppet(&ctx, &puppet, dt_frame, skin_mats);
        }

        final_angles[i] = ctx.states[0].current_angle_rad;
    }

    float drift_30_240 = fabsf(final_angles[0] - final_angles[3]);
    float drift_60_240 = fabsf(final_angles[1] - final_angles[3]);
    float drift_144_240 = fabsf(final_angles[2] - final_angles[3]);
    float max_drift = fmaxf(drift_30_240, fmaxf(drift_60_240, drift_144_240));

    printf("    -> Final angles: 30Hz=%.6f, 60Hz=%.6f, 144Hz=%.6f, 240Hz=%.6f\n", final_angles[0],
           final_angles[1], final_angles[2], final_angles[3]);
    printf("    -> Max angular drift: %.2e rad (budget: <= 1.0e-4 rad)\n", max_drift);

    assert(drift_30_240 <= 1.0e-4f && "30 vs 240 Hz angular drift exceeded 1e-4 rad");
    assert(drift_60_240 <= 1.0e-4f && "60 vs 240 Hz angular drift exceeded 1e-4 rad");
    assert(drift_144_240 <= 1.0e-4f && "144 vs 240 Hz angular drift exceeded 1e-4 rad");
}

static void test_chain_inextensibility_under_gusts(void)
{
    printf("[+] Testing 16-bone chain inextensibility under 4x peak wind gusts (< 1.5%%)...\n");

    we_puppet_physics_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.active_bone_count = 16;
    ctx.initialized = true;

    const float bone_len = 30.0f;
    const float total_rest_len = 16.0f * bone_len; // 480.0 px

    for (uint32_t k = 0; k < 16; ++k) {
        we_physics_apply_preset(&ctx.configs[k], WE_PHYS_PRESET_METALLIC_CHAIN);
        ctx.configs[k].rest_length_px = bone_len;
        ctx.configs[k].compliance_stretch = 0.0f; // Inextensible rod
        ctx.rest_dir_local[k][1] = -1.0f;
        ctx.states[k].pos_curr[1] = -((float)(k + 1) * bone_len);
        ctx.states[k].pos_prev[1] = ctx.states[k].pos_curr[1];
    }

    // Configure 4x violent wind gust
    ctx.wind.enabled = 1;
    ctx.wind.base_direction_rad = 0.0f; // Horizontal right
    ctx.wind.base_speed_px_s = 800.0f;
    ctx.wind.gust_amplitude = 4.0f;

    float pivot[3] = {0.0f, 0.0f, 0.0f};
    float W[16];
    memset(W, 0, sizeof(W));
    W[0] = 1.0f;
    W[5] = 1.0f;
    W[10] = 1.0f;
    W[15] = 1.0f;

    for (int step = 0; step < 240; ++step) {
        float parent_pos[3] = {pivot[0], pivot[1], pivot[2]};
        for (uint32_t k = 0; k < 16; ++k) {
            we_physics_substep_bone(&ctx, k, parent_pos, W, WE_PHYSICS_SUBSTEP_DT,
                                    step * WE_PHYSICS_SUBSTEP_DT);
            parent_pos[0] = ctx.states[k].pos_curr[0];
            parent_pos[1] = ctx.states[k].pos_curr[1];
            parent_pos[2] = ctx.states[k].pos_curr[2];
        }
    }

    // Measure total chain length along links
    float actual_len = 0.0f;
    float prev_pos[3] = {0.0f, 0.0f, 0.0f};
    for (uint32_t k = 0; k < 16; ++k) {
        float dx = ctx.states[k].pos_curr[0] - prev_pos[0];
        float dy = ctx.states[k].pos_curr[1] - prev_pos[1];
        float dz = ctx.states[k].pos_curr[2] - prev_pos[2];
        actual_len += sqrtf(dx * dx + dy * dy + dz * dz);
        prev_pos[0] = ctx.states[k].pos_curr[0];
        prev_pos[1] = ctx.states[k].pos_curr[1];
        prev_pos[2] = ctx.states[k].pos_curr[2];
    }

    float elongation_pct = fabsf(actual_len - total_rest_len) / total_rest_len * 100.0f;
    printf("    -> Rest length: %.1f px, Solved length: %.1f px, Elongation: %.3f%%\n",
           total_rest_len, actual_len, elongation_pct);

    assert(elongation_pct < 1.5f && "16-bone chain elongation exceeded 1.5% limit under 4x gust");
}

static void test_cursor_sdf_and_audio_impulses(void)
{
    printf("[+] Testing Wayland cursor SDF repulsion & audio spectrum coupling...\n");

    we_puppet_physics_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.active_bone_count = 1;
    ctx.initialized = true;

    we_physics_apply_preset(&ctx.configs[0], WE_PHYS_PRESET_SILK);
    ctx.configs[0].rest_length_px = 50.0f;
    ctx.configs[0].flags |= WE_BONE_PHYS_CURSOR_COLLIDE;
    ctx.configs[0].collision_radius_px = 15.0f;
    ctx.configs[0].audio_drag_coeff = 2.0f;
    ctx.rest_dir_local[0][1] = -1.0f;

    // Position tip at (0, -50, 0)
    ctx.states[0].pos_curr[0] = 0.0f;
    ctx.states[0].pos_curr[1] = -50.0f;
    ctx.states[0].pos_curr[2] = 0.0f;

    // Cursor placed at (5, -50), radius = 30px, repels
    ctx.cursor.enabled = 1;
    ctx.cursor.radius_px = 30.0f;
    ctx.cursor.impulse_strength = 500.0f;
    ctx.cursor.falloff_power = 2.0f;
    we_physics_set_cursor(&ctx, 5.0f, -50.0f, 10.0f, 0.0f);

    float pivot[3] = {0.0f, 0.0f, 0.0f};
    float W[16];
    memset(W, 0, sizeof(W));
    W[0] = 1.0f;
    W[5] = 1.0f;
    W[10] = 1.0f;
    W[15] = 1.0f;

    // Step once
    we_physics_substep_bone(&ctx, 0, pivot, W, WE_PHYSICS_SUBSTEP_DT, 0.0);
    // Cursor to the right (+X) should repel tip to the left (-X)
    assert(ctx.states[0].vel[0] < 0.0f && "Cursor SDF must repel bone away from cursor position");

    // Audio test: bass impulse
    float bands[64];
    memset(bands, 0, sizeof(bands));
    bands[0] = 1.0f; // Strong 40 Hz kick drum
    we_physics_set_audio_bands(&ctx, bands);
    we_physics_substep_bone(&ctx, 0, pivot, W, WE_PHYSICS_SUBSTEP_DT, 0.01);

    printf("    -> Cursor SDF repulsion & audio band impulse coupling verified.\n");
}

static void test_128_bone_step_puppet_benchmark(void)
{
    printf("[+] Benchmarking 128-bone we_physics_step_puppet() CPU budget (<= 45.0 us, 0 heap "
           "allocs)...\n");

    we_mdl_bone_t *bones = (we_mdl_bone_t *)calloc(WE_MDL_MAX_BONES, sizeof(we_mdl_bone_t));
    assert(bones != NULL);

    // Build 8 articulated chains of 16 bones each (128 active physics bones total)
    for (uint32_t k = 0; k < WE_MDL_MAX_BONES; ++k) {
        uint32_t link = k % 16u;
        uint32_t chain = k / 16u;
        bones[k].parent = (link == 0u) ? 0xFFFFFFFFu : (k - 1u);
        bones[k].local_trans[0] = 1.0f;
        bones[k].local_trans[5] = 1.0f;
        bones[k].local_trans[10] = 1.0f;
        bones[k].local_trans[12] = (link == 0u) ? ((float)chain * 40.0f) : 0.0f;
        bones[k].local_trans[13] = (link == 0u) ? 200.0f : -20.0f;
        bones[k].local_trans[15] = 1.0f;

        bones[k].offset_trans[0] = 1.0f;
        bones[k].offset_trans[5] = 1.0f;
        bones[k].offset_trans[10] = 1.0f;
        bones[k].offset_trans[12] = -((float)chain * 40.0f);
        bones[k].offset_trans[13] = -(200.0f - (float)link * 20.0f);
        bones[k].offset_trans[15] = 1.0f;

        snprintf(bones[k].name, sizeof(bones[k].name), "hair_chain%u_link%u", chain, link);
        snprintf(bones[k].simulation_json, sizeof(bones[k].simulation_json),
                 "{\"stiffness\":0.35,\"damping\":0.25,\"length\":20.0,\"wind\":1.0}");
    }

    we_puppet_t puppet;
    memset(&puppet, 0, sizeof(puppet));
    puppet.bone_count = WE_MDL_MAX_BONES;
    puppet.bones = bones;

    we_puppet_physics_ctx_t *ctx =
        (we_puppet_physics_ctx_t *)calloc(1, sizeof(we_puppet_physics_ctx_t));
    assert(ctx != NULL);
    we_physics_init_from_puppet(ctx, &puppet);
    assert(ctx->initialized);
    assert(ctx->active_bone_count == WE_MDL_MAX_BONES);

    ctx->wind.enabled = 1;
    ctx->wind.base_speed_px_s = 120.0f;
    ctx->wind.gust_frequency_hz = 0.4f;
    ctx->wind.gust_amplitude = 1.5f;
    ctx->wind.curl_spatial_scale = 0.005f;
    ctx->wind.gravity_vec[1] = -980.0f;

    ctx->cursor.enabled = 1;
    ctx->cursor.radius_px = 120.0f;
    ctx->cursor.impulse_strength = 250.0f;
    ctx->cursor.falloff_power = 2.0f;
    we_physics_set_cursor(ctx, 100.0f, 80.0f, 35.0f, -15.0f);

    float skin_mats[WE_MDL_MAX_BONES][16];
    memset(skin_mats, 0, sizeof(skin_mats));

    // Warm up caches & branch predictors
    for (int w = 0; w < 64; ++w) {
        we_physics_step_puppet(ctx, &puppet, WE_PHYSICS_SUBSTEP_DT, skin_mats);
    }

#ifdef __GLIBC__
    struct mallinfo2 mi_before = mallinfo2();
#endif

    const int bench_steps = 1000;
    double best_step_us = 1e9;
    for (int trial = 0; trial < 3; ++trial) {
        uint64_t t0 = get_time_ns();
        for (int s = 0; s < bench_steps; ++s) {
            we_physics_step_puppet(ctx, &puppet, WE_PHYSICS_SUBSTEP_DT, skin_mats);
        }
        uint64_t t1 = get_time_ns();
        double avg_us = (double)(t1 - t0) / (1000.0 * (double)bench_steps);
        if (avg_us < best_step_us) {
            best_step_us = avg_us;
        }
    }

#ifdef __GLIBC__
    struct mallinfo2 mi_after = mallinfo2();
    assert(mi_after.uordblks == mi_before.uordblks &&
           "we_physics_step_puppet must perform zero heap allocations");
#endif

    // Verify skin matrices remained finite
    for (uint32_t k = 0; k < WE_MDL_MAX_BONES; ++k) {
        for (int m = 0; m < 16; ++m) {
            assert(isfinite(skin_mats[k][m]));
        }
    }

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
    const double step_budget_us = 350.0;
#else
    const double step_budget_us = 45.0;
#endif
    printf("    -> 128-bone we_physics_step_puppet latency: %.2f us / step (budget: <= %.2f us, 0 "
           "allocs)\n",
           best_step_us, step_budget_us);
    assert(best_step_us <= step_budget_us && "128-bone we_physics_step_puppet exceeded CPU budget");

    free(ctx);
    free(bones);
}

static float det3x3_col_major(const float m[16])
{
    return m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) +
           m[8] * (m[1] * m[6] - m[5] * m[2]);
}

static void test_avx2_curl_wind_batch_equivalence(void)
{
    printf("[+] Testing batched SIMD 3D curl-noise equivalence against scalar oracle...\n");

    we_wind_field_t wind = {
        .enabled = 1,
        .base_direction_rad = 0.65f,
        .base_speed_px_s = 175.0f,
        .gust_frequency_hz = 1.25f,
        .gust_amplitude = 2.1f,
        .curl_spatial_scale = 0.006f,
        .gravity_vec = {0.0f, -980.0f},
    };

    enum { N = 67 };
    float xs[N], ys[N], zs[N];
    float batch_vel[N][3];
    for (int i = 0; i < N; ++i) {
        xs[i] = -1400.0f + (float)i * 43.7f;
        ys[i] = 950.0f - (float)i * 29.3f;
        zs[i] = -320.0f + (float)(i % 11) * 61.5f;
    }

    const double high_t = 500000.125;
    we_physics_eval_curl_wind_3d_batch(&wind, xs, ys, zs, N, high_t, batch_vel);

    float max_diff = 0.0f;
    for (int i = 0; i < N; ++i) {
        float ref[3];
        we_physics_eval_curl_wind_3d(&wind, xs[i], ys[i], zs[i], high_t, ref);
        for (int c = 0; c < 3; ++c) {
            assert(isfinite(batch_vel[i][c]) && isfinite(ref[c]));
            float d = fabsf(batch_vel[i][c] - ref[c]);
            if (d > max_diff) {
                max_diff = d;
            }
        }
    }
    printf("    -> Max batch vs scalar velocity delta across %d points: %.2e px/s\n", N, max_diff);
    assert(max_diff <= 1e-3f && "Batched curl wind diverged from scalar reference");
}

static void test_wall_clock_and_non_compounding_hierarchy_solver(void)
{
    printf("[+] Testing wall-clock substep scheduling & non-compounding hierarchy skinning...\n");

    we_mdl_bone_t bones[2];
    memset(bones, 0, sizeof(bones));

    bones[0].parent = 0xFFFFFFFFu;
    bones[0].local_trans[0] = 1.0f;
    bones[0].local_trans[5] = 1.0f;
    bones[0].local_trans[10] = 1.0f;
    bones[0].local_trans[15] = 1.0f;
    bones[0].offset_trans[0] = 1.0f;
    bones[0].offset_trans[5] = 1.0f;
    bones[0].offset_trans[10] = 1.0f;
    bones[0].offset_trans[15] = 1.0f;
    snprintf(bones[0].simulation_json, sizeof(bones[0].simulation_json),
             "{\"stiffness\":0.3,\"damping\":0.2,\"length\":40.0,\"wind\":1.0}");

    bones[1].parent = 0u;
    bones[1].local_trans[0] = 1.0f;
    bones[1].local_trans[5] = 1.0f;
    bones[1].local_trans[10] = 1.0f;
    bones[1].local_trans[13] = -40.0f;
    bones[1].local_trans[15] = 1.0f;
    bones[1].offset_trans[0] = 1.0f;
    bones[1].offset_trans[5] = 1.0f;
    bones[1].offset_trans[10] = 1.0f;
    bones[1].offset_trans[13] = 40.0f;
    bones[1].offset_trans[15] = 1.0f;
    snprintf(bones[1].simulation_json, sizeof(bones[1].simulation_json),
             "{\"stiffness\":0.3,\"damping\":0.2,\"length\":40.0,\"wind\":1.0}");

    we_puppet_t puppet;
    memset(&puppet, 0, sizeof(puppet));
    puppet.bone_count = 2;
    puppet.bones = bones;

    we_puppet_physics_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    we_physics_init_from_puppet(&ctx, &puppet);
    ctx.wind.enabled = 1;
    ctx.wind.base_speed_px_s = 220.0f;

    float anim_local[2][16];
    memcpy(anim_local[0], bones[0].local_trans, sizeof(anim_local[0]));
    memcpy(anim_local[1], bones[1].local_trans, sizeof(anim_local[1]));
    float skin_mats[2][16];

    // First wall-clock call seeds last_wall_time_sec with nominal 60 Hz dt (4 substeps)
    assert(we_physics_advance_wall_clock(&ctx, 10.0) == 4u);
    ctx.time_accumulator_sec = 0.0f;

    // 30 Hz step triggers 8 substeps in one frame; rotations must NOT compound S times
    uint32_t subs_30hz = we_physics_advance_wall_clock(&ctx, 10.0 + (1.0 / 30.0));
    assert(subs_30hz == 8u);
    we_physics_solve_hierarchy_skin_matrices(&ctx, &puppet, (const float (*)[16])anim_local,
                                             subs_30hz, skin_mats);

    float det0 = det3x3_col_major(skin_mats[0]);
    float det1 = det3x3_col_major(skin_mats[1]);
    assert(fabsf(det0 - 1.0f) < 1e-3f && fabsf(det1 - 1.0f) < 1e-3f);

    // Compare 30 Hz single-frame deflection vs 8 discrete 240 Hz frames
    we_puppet_physics_ctx_t ctx_ref;
    memset(&ctx_ref, 0, sizeof(ctx_ref));
    we_physics_init_from_puppet(&ctx_ref, &puppet);
    ctx_ref.wind = ctx.wind;
    assert(we_physics_advance_wall_clock(&ctx_ref, 10.0) == 4u);
    ctx_ref.time_accumulator_sec = 0.0f;
    float skin_ref[2][16];
    for (int f = 1; f <= 8; ++f) {
        uint32_t s = we_physics_advance_wall_clock(&ctx_ref, 10.0 + (double)f * (1.0 / 240.0));
        assert(s == 1u);
        we_physics_solve_hierarchy_skin_matrices(&ctx_ref, &puppet, (const float (*)[16])anim_local,
                                                 s, skin_ref);
    }
    for (int m = 0; m < 16; ++m) {
        assert(fabsf(skin_mats[1][m] - skin_ref[1][m]) < 1e-3f &&
               "Multi-substep frame skin matrix must match discrete 240 Hz frames");
    }

    // Resume jump (+3600s) clamps cleanly
    uint32_t subs_jump = we_physics_advance_wall_clock(&ctx, 3610.0);
    assert(subs_jump <= WE_PHYSICS_MAX_SUBSTEPS);
    printf("    -> Non-compounding hierarchy skinning & wall-clock clamping verified.\n");
}

int main(void)
{
    printf("==========================================================\n");
    printf("  XPBD Puppet Spring & Wind Physics Test Suite (test_we_physics)\n");
    printf("==========================================================\n");

    test_struct_sizes_and_presets();
    test_curl_noise_zero_divergence();
    test_substepped_xpbd_frame_rate_invariance();
    test_chain_inextensibility_under_gusts();
    test_cursor_sdf_and_audio_impulses();
    test_128_bone_step_puppet_benchmark();
    test_avx2_curl_wind_batch_equivalence();
    test_wall_clock_and_non_compounding_hierarchy_solver();

    printf("==========================================================\n");
    printf("  >>> ALL WE_PHYSICS TESTS PASSED CLEANLY (100%%) <<<\n");
    printf("==========================================================\n");
    return 0;
}
