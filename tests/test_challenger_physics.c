// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_challenger_physics.c
 * @brief XPBD Physics, Divergence-Free Curl Noise & Variable dt Stress Suite
 */

#include "waywal/we_physics.h"
#include "waywal/we_puppet.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EPSILON_RAD 1e-4f

// 1. Variable / Erratic dt substepping invariance
static void test_xpbd_variable_dt_matrix(void)
{
    printf("[CHALLENGER 1] 1. Stress-testing 240 Hz XPBD substepping invariance under variable "
           "dt...\n");

    const float target_duration = 2.0f; // 2 seconds
    const float test_framerates[] = {60.0f, 120.0f, 144.0f, 240.0f, 360.0f};
    enum { num_rates = sizeof(test_framerates) / sizeof(test_framerates[0]) };
    float final_angles[num_rates];
    float final_positions[num_rates][3];

    for (int r = 0; r < num_rates; ++r) {
        float fps = test_framerates[r];
        float dt_frame = 1.0f / fps;
        int frames = (int)(target_duration * fps + 0.5f);

        we_puppet_physics_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.active_bone_count = 1;
        ctx.initialized = true;

        we_physics_apply_preset(&ctx.configs[0], WE_PHYS_PRESET_HEAVY_CAPE);
        ctx.configs[0].rest_length_px = 60.0f;
        ctx.rest_dir_local[0][1] = -1.0f;  // Points down
        ctx.states[0].pos_curr[0] = 15.0f; // Deflection

        ctx.wind.enabled = 1;
        ctx.wind.base_speed_px_s = 75.0f;
        ctx.wind.gravity_vec[1] = -980.0f;

        float pivot[3] = {100.0f, 500.0f, 0.0f};
        float W[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 100, 500, 0, 1};

        for (int f = 0; f < frames; ++f) {
            ctx.time_accumulator_sec += dt_frame;
            while (ctx.time_accumulator_sec + 1.0e-5f >= WE_PHYSICS_SUBSTEP_DT) {
                we_physics_substep_bone(&ctx, 0, pivot, W, WE_PHYSICS_SUBSTEP_DT,
                                        ctx.total_sim_time_sec);
                ctx.total_sim_time_sec += WE_PHYSICS_SUBSTEP_DT;
                ctx.time_accumulator_sec -= WE_PHYSICS_SUBSTEP_DT;
            }
        }

        final_angles[r] = ctx.states[0].current_angle_rad;
        final_positions[r][0] = ctx.states[0].pos_curr[0];
        final_positions[r][1] = ctx.states[0].pos_curr[1];
        final_positions[r][2] = ctx.states[0].pos_curr[2];
        printf("    FPS %5.1f -> final angle: %10.6f rad, pos: (%7.2f, %7.2f, %7.2f)\n", fps,
               final_angles[r], final_positions[r][0], final_positions[r][1],
               final_positions[r][2]);
    }

    // Compare all to 240 Hz baseline
    int idx_240 = 3;
    for (int r = 0; r < num_rates; ++r) {
        float drift = fabsf(final_angles[r] - final_angles[idx_240]);
        float pdist = sqrtf(powf(final_positions[r][0] - final_positions[idx_240][0], 2) +
                            powf(final_positions[r][1] - final_positions[idx_240][1], 2) +
                            powf(final_positions[r][2] - final_positions[idx_240][2], 2));
        assert(drift <= 1e-4f && "Angular drift between refresh rates exceeds 1e-4 rad");
        assert(pdist <= 0.05f && "Position drift between refresh rates exceeds 0.05 px");
    }
    printf("    -> Substepping invariance verified across 60, 120, 144, 240, 360 Hz (max drift < "
           "1e-4 rad)!\n");

    // Chaotic jitter test: variable dt per frame
    printf("    Testing chaotic frame jitter (uniformly distributed dt between 2ms and 25ms)...\n");
    we_puppet_physics_ctx_t ctx_jitter;
    memset(&ctx_jitter, 0, sizeof(ctx_jitter));
    ctx_jitter.active_bone_count = 1;
    ctx_jitter.initialized = true;
    we_physics_apply_preset(&ctx_jitter.configs[0], WE_PHYS_PRESET_HEAVY_CAPE);
    ctx_jitter.configs[0].rest_length_px = 60.0f;
    ctx_jitter.rest_dir_local[0][1] = -1.0f;
    ctx_jitter.states[0].pos_curr[0] = 15.0f;
    ctx_jitter.wind.enabled = 1;
    ctx_jitter.wind.base_speed_px_s = 75.0f;
    ctx_jitter.wind.gravity_vec[1] = -980.0f;

    float pivot[3] = {100.0f, 500.0f, 0.0f};
    float W[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 100, 500, 0, 1};

    float sim_accum = 0.0f;
    srand(42);
    while (sim_accum < target_duration) {
        float rand_dt = 0.002f + ((float)(rand() % 23000) / 1000000.0f); // 2ms .. 25ms
        if (sim_accum + rand_dt > target_duration) {
            rand_dt = target_duration - sim_accum;
        }
        sim_accum += rand_dt;

        ctx_jitter.time_accumulator_sec += rand_dt;
        while (ctx_jitter.time_accumulator_sec + 1.0e-5f >= WE_PHYSICS_SUBSTEP_DT) {
            we_physics_substep_bone(&ctx_jitter, 0, pivot, W, WE_PHYSICS_SUBSTEP_DT,
                                    ctx_jitter.total_sim_time_sec);
            ctx_jitter.total_sim_time_sec += WE_PHYSICS_SUBSTEP_DT;
            ctx_jitter.time_accumulator_sec -= WE_PHYSICS_SUBSTEP_DT;
        }
    }

    float jitter_drift = fabsf(ctx_jitter.states[0].current_angle_rad - final_angles[idx_240]);
    printf("    Chaotic jitter drift vs 240Hz: %.2e rad (threshold <= 1e-4 rad)\n", jitter_drift);
    assert(jitter_drift <= 1e-4f && "Jitter drift exceeded 1e-4 rad tolerance");
    printf("    -> PASS: Substepping is completely invariant to frame jitter.\n");
}

// 2. Mathematical Compliance Formulation alpha_tilde = alpha / dt^2
static void test_xpbd_compliance_formulation(void)
{
    printf("[CHALLENGER 1] 2. Verifying XPBD compliance formulation (Angular bending spring "
           "oracle)...\n");

    // Consider a bone resting vertically (0, -1, 0) with length 100px.
    // Tip mass m = 1.0 kg.
    // Apply lateral gravity force (gx = 400.0f, gy = -800.0f).
    // The bone will settle at an equilibrium angle where restoring spring torque balances gravity
    // torque. In XPBD, with compliance alpha_bend, the equilibrium angle must be invariant to
    // substep dt (120 Hz vs 240 Hz vs 480 Hz).

    const float compliance_bend = 0.05f;
    const float dt_substeps[] = {1.0f / 120.0f, 1.0f / 240.0f, 1.0f / 480.0f};
    float eq_angles[3];

    for (int s = 0; s < 3; ++s) {
        float sub_dt = dt_substeps[s];
        we_puppet_physics_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.active_bone_count = 1;
        ctx.initialized = true;

        ctx.configs[0].flags = WE_BONE_PHYS_ENABLED;
        ctx.configs[0].mass_kg = 1.0f;
        ctx.configs[0].rest_length_px = 100.0f;
        ctx.configs[0].compliance_stretch = 1e-6f;
        ctx.configs[0].compliance_bend = compliance_bend;
        ctx.configs[0].damping_ratio = 1.5f; // heavy damping to settle rapidly at equilibrium
        ctx.configs[0].gravity_scale = 1.0f;
        ctx.rest_dir_local[0][1] = -1.0f;

        ctx.wind.enabled = 0;
        ctx.wind.gravity_vec[0] = 300.0f; // lateral load
        ctx.wind.gravity_vec[1] = -800.0f;

        float pivot[3] = {0.0f, 0.0f, 0.0f};
        float W[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

        // Run for 3 seconds to reach static equilibrium
        int total_steps = (int)(3.0f / sub_dt);
        for (int step = 0; step < total_steps; ++step) {
            we_physics_substep_bone(&ctx, 0, pivot, W, sub_dt, step * sub_dt);
        }

        eq_angles[s] = ctx.states[0].current_angle_rad;
        printf("    sub_dt = 1/%d s -> Equilibrium angle = %.6f rad (%.2f deg)\n",
               (int)(1.0f / sub_dt + 0.5f), eq_angles[s], eq_angles[s] * (180.0f / 3.14159265f));
    }

    // Verify equilibrium angles match across substep sizes within tight tolerance (<= 0.005 rad)
    float diff_120_240 = fabsf(eq_angles[0] - eq_angles[1]);
    float diff_480_240 = fabsf(eq_angles[2] - eq_angles[1]);
    printf("    Angle variation across 120/240/480Hz: %.2e rad and %.2e rad\n", diff_120_240,
           diff_480_240);
    assert(diff_120_240 <= 0.005f && "XPBD compliance is not timestep-invariant!");
    assert(diff_480_240 <= 0.005f && "XPBD compliance is not timestep-invariant!");
    printf("    -> PASS: Macklin et al. XPBD time-scaled compliance alpha_tilde = alpha/dt^2 "
           "confirmed.\n");
}

// 3. Angular Limit Constraints & Singularity Resistance
static void test_xpbd_angular_limits_and_singularities(void)
{
    printf(
        "[CHALLENGER 1] 3. Testing angular limit cone constraint and singularity resistance...\n");

    we_puppet_physics_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.active_bone_count = 1;
    ctx.initialized = true;

    const float max_ang = 0.523598775f; // 30 degrees
    ctx.configs[0].flags = WE_BONE_PHYS_ENABLED | WE_BONE_PHYS_ANGULAR_LIMIT;
    ctx.configs[0].rest_length_px = 50.0f;
    ctx.configs[0].angle_limit_max_rad = max_ang;
    ctx.configs[0].angle_limit_min_rad = -max_ang;
    ctx.configs[0].compliance_bend = 0.001f;
    ctx.configs[0].mass_kg = 1.0f;
    ctx.rest_dir_local[0][1] = -1.0f; // Rest is (0, -1, 0)

    // Apply massive hurricane horizontal force that would deflect 90 degrees
    ctx.wind.enabled = 1;
    ctx.wind.base_direction_rad = 0.0f;
    ctx.wind.base_speed_px_s = 50000.0f; // Extreme
    ctx.wind.gravity_vec[1] = 0.0f;

    float pivot[3] = {0.0f, 0.0f, 0.0f};
    float W[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    for (int step = 0; step < 100; ++step) {
        we_physics_substep_bone(&ctx, 0, pivot, W, WE_PHYSICS_SUBSTEP_DT,
                                step * WE_PHYSICS_SUBSTEP_DT);
        float d0[3] = {0.0f, -1.0f, 0.0f};
        float cur[3] = {ctx.states[0].pos_curr[0] - pivot[0], ctx.states[0].pos_curr[1] - pivot[1],
                        ctx.states[0].pos_curr[2] - pivot[2]};
        float len = sqrtf(cur[0] * cur[0] + cur[1] * cur[1] + cur[2] * cur[2]);
        float dot = (cur[0] * d0[0] + cur[1] * d0[1] + cur[2] * d0[2]) / len;
        float angle = acosf(fminf(fmaxf(dot, -1.0f), 1.0f));
        assert(angle <= max_ang + 1e-4f && "Bone exceeded max cone limit!");
        assert(!isnan(angle) && !isinf(angle));
    }
    printf("    -> Cone limit theta_max = 30 deg held under extreme wind (angle never exceeded 30 "
           "deg + 1e-4 rad)!\n");

    // Singularity test: initial position antiparallel to d0 (deflection of 180 deg)
    printf("    Testing antiparallel singularity (cos theta = -1.0)...\n");
    ctx.states[0].pos_curr[0] = 0.0f;
    ctx.states[0].pos_curr[1] = 50.0f; // Exact opposite of d0 (0, -50, 0)
    ctx.states[0].pos_curr[2] = 0.0f;
    ctx.states[0].vel[0] = 0.0f;
    ctx.states[0].vel[1] = 0.0f;
    ctx.states[0].vel[2] = 0.0f;

    we_physics_substep_bone(&ctx, 0, pivot, W, WE_PHYSICS_SUBSTEP_DT, 0.0);
    assert(!isnan(ctx.states[0].pos_curr[0]) && !isnan(ctx.states[0].pos_curr[1]) &&
           !isnan(ctx.states[0].pos_curr[2]));
    assert(!isinf(ctx.states[0].pos_curr[0]) && !isinf(ctx.states[0].pos_curr[1]) &&
           !isinf(ctx.states[0].pos_curr[2]));
    printf("    -> PASS: Antiparallel singularity handled without NaN or inf.\n");
}

// 4. Large-Scale Numerical Zero-Divergence Monte Carlo Audit
static void test_curl_noise_monte_carlo(void)
{
    printf("[CHALLENGER 1] 4. Monte Carlo divergence-free audit (1,000 random 3D points in "
           "space-time)...\n");

    we_wind_field_t wind;
    memset(&wind, 0, sizeof(wind));
    wind.enabled = 1;
    wind.base_direction_rad = 1.2f;
    wind.base_speed_px_s = 200.0f;
    wind.gust_frequency_hz = 1.5f;
    wind.gust_amplitude = 2.5f;
    wind.curl_spatial_scale = 0.008f;

    const int N = 1000;
    const float h = 0.01f;
    float max_div = 0.0f;
    double sum_div = 0.0;

    srand(1337);
    for (int i = 0; i < N; ++i) {
        float x = ((float)(rand() % 40000) - 20000.0f) * 0.1f; // -2000 .. +2000
        float y = ((float)(rand() % 40000) - 20000.0f) * 0.1f;
        float z = ((float)(rand() % 40000) - 20000.0f) * 0.1f;
        double t = (double)(rand() % 10000) * 0.01;

        float u_xp[3], u_xm[3], u_yp[3], u_ym[3], u_zp[3], u_zm[3];
        we_physics_eval_curl_wind_3d(&wind, x + h, y, z, t, u_xp);
        we_physics_eval_curl_wind_3d(&wind, x - h, y, z, t, u_xm);
        we_physics_eval_curl_wind_3d(&wind, x, y + h, z, t, u_yp);
        we_physics_eval_curl_wind_3d(&wind, x, y - h, z, t, u_ym);
        we_physics_eval_curl_wind_3d(&wind, x, y, z + h, t, u_zp);
        we_physics_eval_curl_wind_3d(&wind, x, y, z - h, t, u_zm);

        float dux_dx = (u_xp[0] - u_xm[0]) / (2.0f * h);
        float duy_dy = (u_yp[1] - u_ym[1]) / (2.0f * h);
        float duz_dz = (u_zp[2] - u_zm[2]) / (2.0f * h);

        float div = fabsf(dux_dx + duy_dy + duz_dz);
        if (div > max_div) {
            max_div = div;
        }
        sum_div += div;
    }

    float avg_div = (float)(sum_div / N);
    printf("    Across %d 3D points: Max |div u| = %.4f, Avg |div u| = %.4f (numerical O(h^2) "
           "truncation)\n",
           N, max_div, avg_div);
    assert(max_div < 0.08f && "Curl noise divergence exceeds numerical difference tolerance");
    printf("    -> PASS: 3D Curl-noise field is analytically divergence-free (div u = 0).\n");
}

// 5. Cursor SDF Edge Cases: dist = 0, dist > R, extreme velocity
static void test_cursor_sdf_edge_cases(void)
{
    printf("[CHALLENGER 1] 5. Testing Cursor SDF boundary conditions (dist=0, dist=R, extreme "
           "v)...\n");

    we_puppet_physics_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.active_bone_count = 1;
    ctx.initialized = true;
    we_physics_apply_preset(&ctx.configs[0], WE_PHYS_PRESET_SILK);
    ctx.configs[0].flags |= WE_BONE_PHYS_CURSOR_COLLIDE;
    ctx.configs[0].rest_length_px = 50.0f;
    ctx.rest_dir_local[0][1] = -1.0f;

    ctx.cursor.enabled = 1;
    ctx.cursor.radius_px = 50.0f;
    ctx.cursor.impulse_strength = 200.0f;
    ctx.cursor.falloff_power = 2.0f;

    float pivot[3] = {0.0f, 0.0f, 0.0f};
    float W[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    // Edge case A: cursor EXACTLY at tip position (dist = 0)
    we_physics_set_cursor(&ctx, 0.0f, -50.0f, 0.0f, 0.0f);
    we_physics_substep_bone(&ctx, 0, pivot, W, WE_PHYSICS_SUBSTEP_DT, 0.0);
    assert(!isnan(ctx.states[0].pos_curr[0]) && !isnan(ctx.states[0].pos_curr[1]));
    assert(!isinf(ctx.states[0].pos_curr[0]) && !isinf(ctx.states[0].pos_curr[1]));

    // Edge case B: cursor outside radius (dist = 100 > R=50) -> force must be 0
    ctx.states[0].vel[0] = 0.0f;
    ctx.states[0].vel[1] = 0.0f;
    we_physics_set_cursor(&ctx, 100.0f, -50.0f, 0.0f, 0.0f);
    we_physics_substep_bone(&ctx, 0, pivot, W, WE_PHYSICS_SUBSTEP_DT, 0.0);
    assert(fabsf(ctx.states[0].vel[0]) < 1e-4f && "Cursor outside radius must not exert force");

    // Edge case C: supersonic cursor velocity (100,000 px/s)
    we_physics_set_cursor(&ctx, 20.0f, -50.0f, 100000.0f, 0.0f);
    we_physics_substep_bone(&ctx, 0, pivot, W, WE_PHYSICS_SUBSTEP_DT, 0.0);
    assert(!isnan(ctx.states[0].vel[0]) && !isinf(ctx.states[0].vel[0]));

    printf("    -> PASS: Cursor SDF is robust against dist=0, dist>R, and extreme velocities.\n");
}

// 6. Antiparallel 180-Degree SO(3) Bone Rotation & Capsule Mid-Shaft SDF & Asymmetric Cone
static void test_antiparallel_so3_capsule_and_asymmetric_cone(void)
{
    printf("[CHALLENGER 1] 6. Testing 180-deg SO(3) rotation, capsule mid-shaft SDF & asymmetric "
           "cone...\n");

    we_puppet_physics_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.active_bone_count = 1;
    ctx.initialized = true;
    we_physics_apply_preset(&ctx.configs[0], WE_PHYS_PRESET_SILK);
    ctx.configs[0].flags = WE_BONE_PHYS_ENABLED | WE_BONE_PHYS_CURSOR_COLLIDE;
    ctx.configs[0].rest_length_px = 100.0f;
    ctx.configs[0].collision_radius_px = 5.0f;
    ctx.configs[0].mass_kg = 1.0f;
    ctx.rest_dir_local[0][1] = -1.0f;

    // 6A: Exact 180-deg antiparallel bone rotation must preserve det(R) == +1
    ctx.states[0].pos_curr[0] = 0.0f;
    ctx.states[0].pos_curr[1] = 100.0f;
    ctx.states[0].pos_curr[2] = 0.0f;
    float pivot[3] = {0.0f, 0.0f, 0.0f};
    float W[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    we_physics_apply_bone_rotation(&ctx, 0, pivot, W);

    float det = W[0] * (W[5] * W[10] - W[9] * W[6]) - W[4] * (W[1] * W[10] - W[9] * W[2]) +
                W[8] * (W[1] * W[6] - W[5] * W[2]);
    assert(fabsf(det - 1.0f) < 1e-4f && "180-deg bone rotation collapsed SO(3) determinant!");
    // Rest vector (0, -1, 0) transformed by W must map to (0, +1, 0)
    assert(fabsf(-W[5] - 1.0f) < 1e-4f);

    // 6B: Mid-shaft capsule SDF repulsion where tip is far outside cursor radius
    const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float I[16];
    memcpy(I, kIdentity, sizeof(I));
    ctx.states[0].pos_curr[0] = 0.0f;
    ctx.states[0].pos_curr[1] = -100.0f;
    ctx.states[0].pos_curr[2] = 0.0f;
    memset(ctx.states[0].vel, 0, sizeof(ctx.states[0].vel));
    ctx.cursor.enabled = 1;
    ctx.cursor.radius_px = 15.0f;
    ctx.cursor.impulse_strength = 400.0f;
    ctx.cursor.falloff_power = 2.0f;
    // Cursor at (8, -50): tip distance is 50.6 > 20, but capsule shaft distance is 8 < 20
    we_physics_set_cursor(&ctx, 8.0f, -50.0f, 0.0f, 0.0f);
    we_physics_substep_bone(&ctx, 0, pivot, I, WE_PHYSICS_SUBSTEP_DT, 0.0);
    assert(ctx.states[0].vel[0] < -1e-3f && "Capsule mid-shaft SDF failed to repel bone!");

    // Disabled radius_px = 0 must exert zero force
    memcpy(I, kIdentity, sizeof(I));
    ctx.states[0].pos_curr[0] = 0.0f;
    ctx.states[0].pos_curr[1] = -100.0f;
    ctx.states[0].pos_curr[2] = 0.0f;
    memset(ctx.states[0].vel, 0, sizeof(ctx.states[0].vel));
    ctx.cursor.radius_px = 0.0f;
    we_physics_substep_bone(&ctx, 0, pivot, I, WE_PHYSICS_SUBSTEP_DT, 0.0);
    assert(fabsf(ctx.states[0].vel[0]) < 1e-6f && "Zero cursor radius must disable repulsion!");

    // 6C: Asymmetric angular limits (-0.10 rad left vs +0.60 rad right)
    memcpy(I, kIdentity, sizeof(I));
    ctx.cursor.enabled = 0;
    ctx.configs[0].flags = WE_BONE_PHYS_ENABLED | WE_BONE_PHYS_ANGULAR_LIMIT;
    ctx.configs[0].angle_limit_min_rad = -0.10f;
    ctx.configs[0].angle_limit_max_rad = 0.60f;
    ctx.configs[0].compliance_bend = 1.0f;
    ctx.states[0].pos_curr[0] = -80.0f;
    ctx.states[0].pos_curr[1] = -60.0f;
    ctx.states[0].pos_curr[2] = 0.0f;
    we_physics_substep_bone(&ctx, 0, pivot, I, WE_PHYSICS_SUBSTEP_DT, 0.0);
    float ang_neg = atan2f(-ctx.states[0].pos_curr[0], -ctx.states[0].pos_curr[1]);
    assert(fabsf(ang_neg) <= 0.105f && "Asymmetric negative cone limit violated!");

    printf("    -> PASS: 180-deg SO(3), capsule mid-shaft SDF & asymmetric cone verified.\n");
}

// 7. Extreme Coordinates, NaN/Inf Sanitization & Multi-Child Hub Rest Direction
static void test_nan_extreme_coords_and_multichild_hub(void)
{
    printf(
        "[CHALLENGER 1] 7. Fuzzing NaN/Inf/1e20f coordinates & multi-child hub rest vector...\n");

    we_wind_field_t wind = {
        .enabled = 1,
        .base_direction_rad = NAN,
        .base_speed_px_s = INFINITY,
        .gust_frequency_hz = 2.0f,
        .gust_amplitude = 3.0f,
        .curl_spatial_scale = 1.0f,
    };
    float v[3];
    we_physics_eval_curl_wind_3d(&wind, 1.0e20f, -1.0e20f, NAN, 1.0e9, v);
    assert(isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]));

    // Multi-day uptime (t = 200,000s) curl-wind temporal continuity check
    we_wind_field_t clean_wind = {
        .enabled = 1,
        .base_direction_rad = 0.4f,
        .base_speed_px_s = 120.0f,
        .gust_frequency_hz = 1.5f,
        .gust_amplitude = 2.0f,
        .curl_spatial_scale = 0.008f,
    };
    float v_day0[3], v_day1[3];
    we_physics_eval_curl_wind_3d(&clean_wind, 250.0f, 310.0f, 15.0f, 200000.0, v_day0);
    we_physics_eval_curl_wind_3d(&clean_wind, 250.0f, 310.0f, 15.0f, 200000.0 + (1.0 / 240.0),
                                 v_day1);
    float dv_day = hypotf(v_day1[0] - v_day0[0], v_day1[1] - v_day0[1]);
    assert(dv_day > 1e-4f && dv_day < 20.0f &&
           "Multi-day curl wind froze or jumped discontinuously!");

    // Multi-child symmetric Y-branch hub: children at (-30, -40, 0) and (+30, -40, 0)
    we_mdl_bone_t bones[3];
    memset(bones, 0, sizeof(bones));
    for (int i = 0; i < 3; ++i) {
        bones[i].local_trans[0] = 1.0f;
        bones[i].local_trans[5] = 1.0f;
        bones[i].local_trans[10] = 1.0f;
        bones[i].local_trans[15] = 1.0f;
        bones[i].offset_trans[0] = 1.0f;
        bones[i].offset_trans[5] = 1.0f;
        bones[i].offset_trans[10] = 1.0f;
        bones[i].offset_trans[15] = 1.0f;
    }
    bones[0].parent = 0xFFFFFFFFu;
    snprintf(bones[0].simulation_json, sizeof(bones[0].simulation_json), "{\"stiffness\":0.5}");
    bones[1].parent = 0u;
    bones[1].local_trans[12] = -30.0f;
    bones[1].local_trans[13] = -40.0f;
    bones[2].parent = 0u;
    bones[2].local_trans[12] = 30.0f;
    bones[2].local_trans[13] = -40.0f;

    we_puppet_t puppet;
    memset(&puppet, 0, sizeof(puppet));
    puppet.bone_count = 3;
    puppet.bones = bones;

    we_puppet_physics_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    we_physics_init_from_puppet(&ctx, &puppet);
    assert(fabsf(ctx.rest_dir_local[0][0]) < 1e-5f && ctx.rest_dir_local[0][1] < -0.99f &&
           "Multi-child hub rest direction must average all child offsets!");

    // Fuzz dt_s <= 0, NaN compliance, and negative damping without producing NaN matrices
    float W_fuzz[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float piv_fuzz[3] = {0.0f, 0.0f, 0.0f};
    ctx.configs[0].compliance_stretch = -5.0f;
    ctx.configs[0].compliance_bend = NAN;
    ctx.configs[0].damping_ratio = -10.0f;
    we_physics_substep_bone(&ctx, 0, piv_fuzz, W_fuzz, 0.0f, 0.0);
    we_physics_substep_bone(&ctx, 0, piv_fuzz, W_fuzz, NAN, 0.0);
    we_physics_substep_bone(&ctx, 0, piv_fuzz, W_fuzz, WE_PHYSICS_SUBSTEP_DT, 0.0);
    for (int m = 0; m < 16; ++m) {
        assert(isfinite(W_fuzz[m]) && "NaN/negative compliance or zero dt poisoned bone matrix!");
    }

    // Wall-clock suspend spike (+7200s) and 240 Hz post-spike recovery
    we_puppet_physics_ctx_t clk_ctx;
    memset(&clk_ctx, 0, sizeof(clk_ctx));
    we_physics_init_from_puppet(&clk_ctx, &puppet);
    assert(we_physics_advance_wall_clock(&clk_ctx, 50.0) == 4u);
    clk_ctx.time_accumulator_sec = 0.0f;
    assert(we_physics_advance_wall_clock(&clk_ctx, 7250.0) == WE_PHYSICS_MAX_SUBSTEPS);
    // Subsequent 240 Hz frame right after a clamped spike must advance exactly 1 substep (not 4!)
    clk_ctx.time_accumulator_sec = 0.0f;
    assert(we_physics_advance_wall_clock(&clk_ctx, 7250.0 + (1.0 / 240.0)) == 1u);

    printf("    -> PASS: NaN/Inf/1e20f fuzzing, suspend clock & multi-child hub verified.\n");
}

int main(void)
{
    printf("====================================================================\n");
    printf("  [CHALLENGER 1] XPBD Physics & Curl Noise Empirical Suite\n");
    printf("====================================================================\n");

    test_xpbd_variable_dt_matrix();
    test_xpbd_compliance_formulation();
    test_xpbd_angular_limits_and_singularities();
    test_curl_noise_monte_carlo();
    test_cursor_sdf_edge_cases();
    test_antiparallel_so3_capsule_and_asymmetric_cone();
    test_nan_extreme_coords_and_multichild_hub();

    printf("====================================================================\n");
    printf("  >>> ALL XPBD CHALLENGE TESTS PASSED CLEANLY (100%%) <<<\n");
    printf("====================================================================\n");
    return 0;
}
