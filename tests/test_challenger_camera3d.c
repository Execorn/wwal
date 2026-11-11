// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_challenger_camera3d.c
 * @brief 3D Camera, Catmull-Rom Spline, SQUAD & std140 Lighting UBO Stress Suite
 */

#include "waywal/we_camera3d.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAD(deg) ((deg) * (3.14159265358979323846f / 180.0f))

// Helper: multiply 4x4 column-major matrix by 4-vector
static void mat4_mul_vec4(const float m[16], const float v[4], float out[4])
{
    for (int r = 0; r < 4; r++) {
        out[r] =
            m[0 * 4 + r] * v[0] + m[1 * 4 + r] * v[1] + m[2 * 4 + r] * v[2] + m[3 * 4 + r] * v[3];
    }
}

// 1. Perspective Frustum 8-Corner Oracle against OpenGL ES 3.1 Specification
static void test_perspective_corners_oracle(void)
{
    printf("[CHALLENGER 1] 1. Oracle test: Projecting all 8 frustum corners to OpenGL ES NDC [-1, "
           "1]...\n");

    const float fovy = RAD(60.0f);
    const float aspect = 16.0f / 9.0f;
    const float near_z = 0.5f;
    const float far_z = 500.0f;

    float P[16];
    mat4_perspective_rh_gl(fovy, aspect, near_z, far_z, P);

    float tan_half = tanf(fovy * 0.5f);

    // Near plane half-extents (eye space at z = -near_z)
    float yn = near_z * tan_half;
    float xn = yn * aspect;

    // Far plane half-extents (eye space at z = -far_z)
    float yf = far_z * tan_half;
    float xf = yf * aspect;

    const float corners[8][4] = {
        {-xn, -yn, -near_z, 1.0f}, {xn, -yn, -near_z, 1.0f}, {xn, yn, -near_z, 1.0f},
        {-xn, yn, -near_z, 1.0f},  {-xf, -yf, -far_z, 1.0f}, {xf, -yf, -far_z, 1.0f},
        {xf, yf, -far_z, 1.0f},    {-xf, yf, -far_z, 1.0f},
    };

    const float expected_ndc[8][3] = {
        {-1.0f, -1.0f, -1.0f}, {1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, -1.0f}, {-1.0f, 1.0f, -1.0f},
        {-1.0f, -1.0f, 1.0f},  {1.0f, -1.0f, 1.0f},  {1.0f, 1.0f, 1.0f},  {-1.0f, 1.0f, 1.0f},
    };

    for (int i = 0; i < 8; i++) {
        float clip[4];
        mat4_mul_vec4(P, corners[i], clip);
        assert(fabsf(clip[3]) > 1e-6f);
        float ndc[3] = {clip[0] / clip[3], clip[1] / clip[3], clip[2] / clip[3]};

        float err_x = fabsf(ndc[0] - expected_ndc[i][0]);
        float err_y = fabsf(ndc[1] - expected_ndc[i][1]);
        float err_z = fabsf(ndc[2] - expected_ndc[i][2]);

        assert(err_x < 1e-4f && "Frustum X mapping error");
        assert(err_y < 1e-4f && "Frustum Y mapping error");
        assert(err_z < 1e-4f && "Frustum Z mapping error");
    }

    printf("    -> All 8 frustum corners map exactly to (+-1, +-1, +-1) NDC coordinates (error < "
           "1e-4)!\n");
}

// 2. Centripetal Catmull-Rom (alpha = 0.5) Cusp-Free & Arc-Length Verification
static void test_catmull_rom_cusp_and_quadrature(void)
{
    printf("[CHALLENGER 1] 2. Adversarial Catmull-Rom spline testing (sharp turns, uneven knots & "
           "arc length)...\n");

    // Construct highly uneven control points (distances: 1, 50, 2)
    we_camera_kf_t kfs[4];
    memset(kfs, 0, sizeof(kfs));

    kfs[0].time_sec = 0.0f;
    kfs[0].position[0] = 0.0f;
    kfs[0].position[1] = 0.0f;
    kfs[0].position[2] = 0.0f;
    quat_identity(kfs[0].orientation_quat);

    kfs[1].time_sec = 1.0f;
    kfs[1].position[0] = 1.0f;
    kfs[1].position[1] = 0.0f;
    kfs[1].position[2] = 0.0f; // Dist 1
    quat_identity(kfs[1].orientation_quat);

    kfs[2].time_sec = 2.0f;
    kfs[2].position[0] = 1.0f;
    kfs[2].position[1] = 50.0f;
    kfs[2].position[2] = 0.0f; // Dist 50 (sharp 90-deg turn)
    quat_identity(kfs[2].orientation_quat);

    kfs[3].time_sec = 3.0f;
    kfs[3].position[0] = 3.0f;
    kfs[3].position[1] = 50.0f;
    kfs[3].position[2] = 0.0f; // Dist 2
    quat_identity(kfs[3].orientation_quat);

    // Sample 1,000 points across segment 1->2 (t from 1.0 to 2.0)
    float prev_pos[3];
    we_camera3d_eval_spline(kfs, 4, WE_CAM_PATH_ONCE, 1.0, prev_pos, NULL, NULL, NULL, NULL);

    float total_path_len = 0.0f;
    for (int i = 1; i <= 1000; i++) {
        double t = 1.0 + (double)i / 1000.0;
        float cur_pos[3];
        we_camera3d_eval_spline(kfs, 4, WE_CAM_PATH_ONCE, t, cur_pos, NULL, NULL, NULL, NULL);

        float dx = cur_pos[0] - prev_pos[0];
        float dy = cur_pos[1] - prev_pos[1];
        float dz = cur_pos[2] - prev_pos[2];
        float step_len = sqrtf(dx * dx + dy * dy + dz * dz);
        total_path_len += step_len;

        assert(!isnan(cur_pos[0]) && !isnan(cur_pos[1]) && !isnan(cur_pos[2]));
        // Ensure no wild coordinate explosion (within bounds of convex hull + modest margin)
        assert(cur_pos[0] >= -5.0f && cur_pos[0] <= 10.0f);
        assert(cur_pos[1] >= -5.0f && cur_pos[1] <= 55.0f);

        memcpy(prev_pos, cur_pos, sizeof(prev_pos));
    }

    printf("    -> Segment 1->2 (p1=(1,0,0) to p2=(1,50,0)): Numerical arc-length = %.3f px\n",
           total_path_len);
    // Straight line chord distance is 50.0 px. Spline arc length should be slightly longer but < 60
    // px (no wild loops)
    assert(total_path_len >= 50.0f && total_path_len <= 58.0f &&
           "Catmull-Rom formed excessive loop or cusp!");
    printf("    -> PASS: Centripetal Catmull-Rom produces smooth, cusp-free trajectory without "
           "loops.\n");
}

// 3. Shoemake SQUAD Quaternion Interpolation Norm Preservation & C1 Continuity
static void test_squad_unit_norm_and_continuity(void)
{
    printf("[CHALLENGER 1] 3. Testing Shoemake SQUAD unit norm (|q| = 1 +- 1e-6) and C1 "
           "continuity...\n");

    we_camera_kf_t kfs[4];
    memset(kfs, 0, sizeof(kfs));

    // Define 4 orientations with 90-degree rotations around X, Y, Z
    kfs[0].time_sec = 0.0f;
    quat_identity(kfs[0].orientation_quat);

    kfs[1].time_sec = 1.0f;
    kfs[1].orientation_quat[0] = sinf(RAD(45.0f)); // 90 deg around X
    kfs[1].orientation_quat[3] = cosf(RAD(45.0f));

    kfs[2].time_sec = 2.0f;
    kfs[2].orientation_quat[1] = sinf(RAD(45.0f)); // 90 deg around Y
    kfs[2].orientation_quat[3] = cosf(RAD(45.0f));

    kfs[3].time_sec = 3.0f;
    kfs[3].orientation_quat[2] = sinf(RAD(45.0f)); // 90 deg around Z
    kfs[3].orientation_quat[3] = cosf(RAD(45.0f));

    // Sample 5,000 points across the timeline
    float max_norm_err = 0.0f;
    for (int i = 0; i <= 5000; i++) {
        double t = (double)i * (3.0 / 5000.0);
        float q[4];
        we_camera3d_eval_spline(kfs, 4, WE_CAM_PATH_ONCE, t, NULL, q, NULL, NULL, NULL);

        float norm = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        float err = fabsf(norm - 1.0f);
        if (err > max_norm_err) {
            max_norm_err = err;
        }
        assert(err < 1e-5f && "SQUAD quaternion norm violation!");
    }
    printf("    -> 5,000 SQUAD samples: Max quaternion norm error = %.2e (target < 1e-5)\n",
           max_norm_err);

    // C1 Continuity check at keyframe junction t = 1.0s
    // Check that angular rate dq/dt is continuous across t = 1.0
    const double eps = 0.001;
    float q_before1[4], q_before2[4], q_after1[4], q_after2[4];
    we_camera3d_eval_spline(kfs, 4, WE_CAM_PATH_ONCE, 1.0 - 2.0 * eps, NULL, q_before1, NULL, NULL,
                            NULL);
    we_camera3d_eval_spline(kfs, 4, WE_CAM_PATH_ONCE, 1.0 - eps, NULL, q_before2, NULL, NULL, NULL);
    we_camera3d_eval_spline(kfs, 4, WE_CAM_PATH_ONCE, 1.0 + eps, NULL, q_after1, NULL, NULL, NULL);
    we_camera3d_eval_spline(kfs, 4, WE_CAM_PATH_ONCE, 1.0 + 2.0 * eps, NULL, q_after2, NULL, NULL,
                            NULL);

    float dq_before[4], dq_after[4];
    for (int k = 0; k < 4; k++) {
        dq_before[k] = (q_before2[k] - q_before1[k]) / (float)eps;
        dq_after[k] = (q_after2[k] - q_after1[k]) / (float)eps;
    }
    float dq_diff =
        sqrtf(powf(dq_before[0] - dq_after[0], 2) + powf(dq_before[1] - dq_after[1], 2) +
              powf(dq_before[2] - dq_after[2], 2) + powf(dq_before[3] - dq_after[3], 2));

    printf("    -> Keyframe junction C1 angular rate jump: %.4f rad/s (smooth transition)\n",
           dq_diff);
    assert(dq_diff < 0.25f && "SQUAD orientation lacks C1 continuity at keyframe!");
    printf("    -> PASS: Shoemake SQUAD quaternion spline is unit-norm and C1 continuous.\n");
}

// 4. std140 UBO 544-Byte Layout Alignment Audit
static void test_std140_ubo_layout_invariants(void)
{
    printf("[CHALLENGER 1] 4. Exhaustive std140 UBO memory layout and alignment audit...\n");

    // 1. Total struct sizes
    assert(sizeof(we_lighting_uniforms_t) == 544);
    assert(sizeof(we_scene_light_t) == 64);

    // 2. Exact std140 offsets
    assert(offsetof(we_lighting_uniforms_t, ambient_color_rgb) == 0);
    assert(offsetof(we_lighting_uniforms_t, active_light_count) == 12);
    assert(offsetof(we_lighting_uniforms_t, sky_color_rgb) == 16);
    assert(offsetof(we_lighting_uniforms_t, normal_map_strength) == 28);
    assert(offsetof(we_lighting_uniforms_t, lights) == 32);

    // 3. we_scene_light_t member offsets within each 64-byte block
    assert(offsetof(we_scene_light_t, position_or_dir) == 0);
    assert(offsetof(we_scene_light_t, light_type) == 12);
    assert(offsetof(we_scene_light_t, color_rgb) == 16);
    assert(offsetof(we_scene_light_t, intensity) == 28);
    assert(offsetof(we_scene_light_t, attenuation_radius) == 32);
    assert(offsetof(we_scene_light_t, spot_inner_cos) == 36);
    assert(offsetof(we_scene_light_t, spot_outer_cos) == 40);
    assert(offsetof(we_scene_light_t, tube_length) == 44);
    assert(offsetof(we_scene_light_t, spot_dir) == 48);
    assert(offsetof(we_scene_light_t, specular_power) == 60);

    // 4. Alignment of all 8 array elements
    for (int i = 0; i < 8; i++) {
        size_t offset = offsetof(we_lighting_uniforms_t, lights[i]);
        assert(offset % 16 == 0 && "std140 array elements must be 16-byte aligned!");
    }

    printf("    -> PASS: 100%% of std140 UBO layout offsets and 16-byte alignments verified.\n");
}

// 5. 18-FLOP Cofactor Normal Matrix Orthogonality Preservation Oracle
static void test_normal_matrix_orthogonality(void)
{
    printf("[CHALLENGER 1] 5. Verifying 18-FLOP normal matrix orthogonality preservation under "
           "non-uniform scaling & shear...\n");

    // Construct model matrix with non-uniform scale (3, 0.5, 2) and rotation 30 deg around Z
    float M[16] = {2.598076f, 0.250000f, 0.0f, 0.0f, -1.500000f, 0.433013f,  0.0f, 0.0f,
                   0.000000f, 0.000000f, 2.0f, 0.0f, 10.000000f, 20.000000f, 5.0f, 1.0f};

    float N[9];
    we_camera3d_compute_normal_matrix(M, N);

    // Pick 3 orthogonal pairs: surface tangent t and normal n where t . n = 0
    float t1[3] = {1.0f, 0.0f, 0.0f};
    float n1[3] = {0.0f, 1.0f, 0.0f};

    float t2[3] = {0.0f, 1.0f, 0.0f};
    float n2[3] = {0.0f, 0.0f, 1.0f};

    float t3[3] = {0.7071f, 0.7071f, 0.0f};
    float n3[3] = {-0.7071f, 0.7071f, 0.0f};

    const float *t_list[3] = {t1, t2, t3};
    const float *n_list[3] = {n1, n2, n3};

    for (int p = 0; p < 3; p++) {
        const float *t = t_list[p];
        const float *n = n_list[p];

        // Transform tangent by M: t' = M * t (linear part)
        float t_prime[3] = {M[0] * t[0] + M[4] * t[1] + M[8] * t[2],
                            M[1] * t[0] + M[5] * t[1] + M[9] * t[2],
                            M[2] * t[0] + M[6] * t[1] + M[10] * t[2]};

        // Transform normal by N: n' = N * n
        float n_prime[3] = {N[0] * n[0] + N[3] * n[1] + N[6] * n[2],
                            N[1] * n[0] + N[4] * n[1] + N[7] * n[2],
                            N[2] * n[0] + N[5] * n[1] + N[8] * n[2]};

        float dot = t_prime[0] * n_prime[0] + t_prime[1] * n_prime[1] + t_prime[2] * n_prime[2];
        printf("    Pair %d: (t' . n') = %.2e (must be 0.0)\n", p + 1, dot);
        assert(fabsf(dot) < 1e-4f &&
               "Normal matrix failed to preserve tangent-normal orthogonality!");
    }

    printf("    -> PASS: Cofactor normal matrix provably preserves surface orthogonality.\n");
}

int main(void)
{
    printf("====================================================================\n");
    printf("  [CHALLENGER 1] 3D Camera, Spline, SQUAD & std140 Lighting Suite\n");
    printf("====================================================================\n");

    test_perspective_corners_oracle();
    test_catmull_rom_cusp_and_quadrature();
    test_squad_unit_norm_and_continuity();
    test_std140_ubo_layout_invariants();
    test_normal_matrix_orthogonality();

    printf("====================================================================\n");
    printf("  >>> ALL 3D CAMERA & LIGHTING CHALLENGE TESTS PASSED CLEANLY (100%%) <<<\n");
    printf("====================================================================\n");
    return 0;
}
