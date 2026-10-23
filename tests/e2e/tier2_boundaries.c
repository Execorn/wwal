#include "e2e_common.h"

void run_tier2_boundary_corner(void)
{
    g_stats.tier_name = "Tier 2: Boundary & Corner Cases";
    printf("\n=========================================================================\n");
    printf("  Running %s (18 Effects x 5 Tests = 90 Test Cases)\n", g_stats.tier_name);
    printf("=========================================================================\n\n");

    const uint32_t width = 320;
    const uint32_t height = 240;
    const size_t num_pixels = (size_t)width * height;
    const size_t buf_bytes = num_pixels * sizeof(uint32_t);

    uint32_t *src_a = e2e_canary_alloc(num_pixels);
    uint32_t *src_b = e2e_canary_alloc(num_pixels);
    uint32_t *dst = e2e_canary_alloc(num_pixels);
    assert(src_a && src_b && dst);

    e2e_generate_pattern_a(src_a, width, height, width);
    e2e_generate_pattern_b(src_b, width, height, width);

    for (size_t i = 0; i < 18; ++i) {
        const e2e_transition_desc_t *effect = &E2E_ALL_TRANSITIONS[i];
        char test_name[128];

        /* Test 1: Bit-exact convergence at progress = 0.0f */
        snprintf(test_name, sizeof(test_name), "T2_%02zu_%s_p0_bit_exact", i * 5 + 1, effect->name);
        memset(dst, 0, buf_bytes);
        waywal_transition_params_t p0 = {
            .type = effect->type,
            .progress = 0.0f,
            .angle_rad = 0.78539816f,
            .wave_freq = 20.0f,
            .wave_amp = 0.05f,
            .center_x = 0.5f,
            .center_y = 0.5f,
            .bezier = BEZIER_DEFAULT,
        };
        bool ok0 = render_engine_execute_cpu_transition(dst, src_a, src_b, width, height,
                                                        width * sizeof(uint32_t), &p0);
        bool match_a = (memcmp(dst, src_a, buf_bytes) == 0);
        bool canary_ok0 = e2e_canary_check(dst, num_pixels);
        E2E_CHECK(ok0 && match_a && canary_ok0, test_name,
                  "execution=%d bit_exact_a=%d canary=%d", ok0, match_a, canary_ok0);

        /* Test 2: Bit-exact convergence at progress = 1.0f */
        snprintf(test_name, sizeof(test_name), "T2_%02zu_%s_p1_bit_exact", i * 5 + 2, effect->name);
        memset(dst, 0, buf_bytes);
        waywal_transition_params_t p1 = {
            .type = effect->type,
            .progress = 1.0f,
            .angle_rad = 0.78539816f,
            .wave_freq = 20.0f,
            .wave_amp = 0.05f,
            .center_x = 0.5f,
            .center_y = 0.5f,
            .bezier = BEZIER_DEFAULT,
        };
        bool ok1 = render_engine_execute_cpu_transition(dst, src_a, src_b, width, height,
                                                        width * sizeof(uint32_t), &p1);
        bool match_b = (memcmp(dst, src_b, buf_bytes) == 0);
        bool canary_ok1 = e2e_canary_check(dst, num_pixels);
        E2E_CHECK(ok1 && match_b && canary_ok1, test_name,
                  "execution=%d bit_exact_b=%d canary=%d", ok1, match_b, canary_ok1);

        /* Test 3: Convergence at progress = 0.9999f (Threshold bit-exact / Continuous converged) */
        snprintf(test_name, sizeof(test_name), "T2_%02zu_%s_p09999_convergence", i * 5 + 3,
                 effect->name);
        memset(dst, 0, buf_bytes);
        waywal_transition_params_t p_near1 = {
            .type = effect->type,
            .progress = 0.9999f,
            .angle_rad = 0.78539816f,
            .wave_freq = 20.0f,
            .wave_amp = 0.05f,
            .center_x = 0.5f,
            .center_y = 0.5f,
            .bezier = BEZIER_DEFAULT,
        };
        bool ok_near1 = render_engine_execute_cpu_transition(dst, src_a, src_b, width, height,
                                                             width * sizeof(uint32_t), &p_near1);
        bool canary_ok_near1 = e2e_canary_check(dst, num_pixels);
        bool conv_pass = false;
        if (effect->is_threshold) {
            /* Procedural threshold effects (noise, burn, wipe, wave) MUST match src_b bit-exact */
            conv_pass = (memcmp(dst, src_b, buf_bytes) == 0);
        } else {
            /* Continuous blend/warp effects: valid ARGB output and majority transitioned away from src_a */
            bool valid_argb = e2e_is_valid_argb_buffer(dst, width, height, width);
            size_t diff_a = e2e_count_diff_pixels(dst, src_a, width, height, width);
            conv_pass = valid_argb && (diff_a >= (num_pixels * 50 / 100));
        }
        E2E_CHECK(ok_near1 && canary_ok_near1 && conv_pass, test_name,
                  "execution=%d threshold=%d conv_pass=%d canary=%d", ok_near1,
                  effect->is_threshold, conv_pass, canary_ok_near1);

        /* Test 4: Non-trivial visual blend at progress = 0.5f */
        snprintf(test_name, sizeof(test_name), "T2_%02zu_%s_p05_nontrivial_blend", i * 5 + 4,
                 effect->name);
        memset(dst, 0, buf_bytes);
        waywal_transition_params_t p_mid = {
            .type = effect->type,
            .progress = 0.5f,
            .angle_rad = 0.78539816f,
            .wave_freq = 20.0f,
            .wave_amp = 0.05f,
            .center_x = 0.5f,
            .center_y = 0.5f,
            .bezier = BEZIER_DEFAULT,
        };
        bool ok_mid = render_engine_execute_cpu_transition(dst, src_a, src_b, width, height,
                                                           width * sizeof(uint32_t), &p_mid);
        bool canary_ok_mid = e2e_canary_check(dst, num_pixels);
        size_t diff_a = e2e_count_diff_pixels(dst, src_a, width, height, width);
        size_t diff_b = e2e_count_diff_pixels(dst, src_b, width, height, width);
        /* Non-trivial blend implies significant difference from both solid sources */
        bool nontrivial = (diff_a >= (num_pixels * 15 / 100)) && (diff_b >= (num_pixels * 15 / 100));
        E2E_CHECK(ok_mid && canary_ok_mid && nontrivial, test_name,
                  "execution=%d diff_a=%zu diff_b=%zu (min=%zu) canary=%d", ok_mid, diff_a, diff_b,
                  (num_pixels * 15 / 100), canary_ok_mid);

        /* Test 5: Corner / Extreme parameter handling */
        snprintf(test_name, sizeof(test_name), "T2_%02zu_%s_corner_params", i * 5 + 5,
                 effect->name);
        memset(dst, 0, buf_bytes);
        waywal_transition_params_t p_corner = {
            .type = effect->type,
            .progress = 0.0001f,       /* infinitesimal epsilon */
            .angle_rad = -3.14159265f, /* extreme negative angle */
            .wave_freq = 150.0f,       /* very high frequency */
            .wave_amp = 0.25f,         /* high amplitude */
            .center_x = -0.5f,         /* out of bounds center X */
            .center_y = 1.5f,          /* out of bounds center Y */
            .bezier = BEZIER_LINEAR,
        };
        bool ok_corner = render_engine_execute_cpu_transition(dst, src_a, src_b, width, height,
                                                              width * sizeof(uint32_t), &p_corner);
        bool canary_ok_corner = e2e_canary_check(dst, num_pixels);
        bool valid_argb = e2e_is_valid_argb_buffer(dst, width, height, width);
        E2E_CHECK(ok_corner && canary_ok_corner && valid_argb, test_name,
                  "execution=%d canary=%d valid_argb=%d", ok_corner, canary_ok_corner, valid_argb);
    }

    e2e_canary_free(src_a);
    e2e_canary_free(src_b);
    e2e_canary_free(dst);
}
