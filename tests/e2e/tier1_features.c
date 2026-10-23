#include "e2e_common.h"

void run_tier1_feature_coverage(void)
{
    g_stats.tier_name = "Tier 1: Feature Coverage";
    printf("\n=========================================================================\n");
    printf("  Running %s (18 Effects x 5 Tests = 90 Test Cases)\n", g_stats.tier_name);
    printf("=========================================================================\n\n");

    const uint32_t standard_w = 320;
    const uint32_t standard_h = 240;
    const size_t standard_pixels = (size_t)standard_w * standard_h;

    uint32_t *src_a = e2e_canary_alloc(standard_pixels);
    uint32_t *src_b = e2e_canary_alloc(standard_pixels);
    uint32_t *dst = e2e_canary_alloc(standard_pixels);
    assert(src_a && src_b && dst);

    e2e_generate_pattern_a(src_a, standard_w, standard_h, standard_w);
    e2e_generate_pattern_b(src_b, standard_w, standard_h, standard_w);

    const uint32_t aniso_w = 640;
    const uint32_t aniso_h = 360;
    const size_t aniso_pixels = (size_t)aniso_w * aniso_h;

    uint32_t *src_a_aniso = e2e_canary_alloc(aniso_pixels);
    uint32_t *src_b_aniso = e2e_canary_alloc(aniso_pixels);
    uint32_t *dst_aniso = e2e_canary_alloc(aniso_pixels);
    assert(src_a_aniso && src_b_aniso && dst_aniso);

    e2e_generate_pattern_a(src_a_aniso, aniso_w, aniso_h, aniso_w);
    e2e_generate_pattern_b(src_b_aniso, aniso_w, aniso_h, aniso_w);

    for (size_t i = 0; i < 18; ++i) {
        const e2e_transition_desc_t *effect = &E2E_ALL_TRANSITIONS[i];
        char test_name[128];

        /* Test 1: Default isolated configuration */
        snprintf(test_name, sizeof(test_name), "T1_%02zu_%s_default_isolated", i * 5 + 1,
                 effect->name);
        memset(dst, 0, standard_pixels * sizeof(uint32_t));
        waywal_transition_params_t p1 = {
            .type = effect->type,
            .progress = 0.30f,
            .angle_rad = 0.0f,
            .wave_freq = 20.0f,
            .wave_amp = 0.05f,
            .center_x = 0.5f,
            .center_y = 0.5f,
            .bezier = BEZIER_DEFAULT,
        };
        bool ok1 = render_engine_execute_cpu_transition(dst, src_a, src_b, standard_w, standard_h,
                                                        standard_w * sizeof(uint32_t), &p1);
        bool canary_ok1 = e2e_canary_check(dst, standard_pixels);
        bool valid_argb1 = e2e_is_valid_argb_buffer(dst, standard_w, standard_h, standard_w);
        E2E_CHECK(ok1 && canary_ok1 && valid_argb1, test_name,
                  "execution=%d canary=%d valid_argb=%d", ok1, canary_ok1, valid_argb1);

        /* Test 2: Angle variation configuration */
        snprintf(test_name, sizeof(test_name), "T1_%02zu_%s_angle_sweep", i * 5 + 2,
                 effect->name);
        memset(dst, 0, standard_pixels * sizeof(uint32_t));
        waywal_transition_params_t p2 = {
            .type = effect->type,
            .progress = 0.65f,
            .angle_rad = 0.78539816f, /* 45 degrees */
            .wave_freq = 20.0f,
            .wave_amp = 0.05f,
            .center_x = 0.5f,
            .center_y = 0.5f,
            .bezier = BEZIER_DEFAULT,
        };
        bool ok2 = render_engine_execute_cpu_transition(dst, src_a, src_b, standard_w, standard_h,
                                                        standard_w * sizeof(uint32_t), &p2);
        bool canary_ok2 = e2e_canary_check(dst, standard_pixels);
        bool valid_argb2 = e2e_is_valid_argb_buffer(dst, standard_w, standard_h, standard_w);
        E2E_CHECK(ok2 && canary_ok2 && valid_argb2, test_name,
                  "execution=%d canary=%d valid_argb=%d", ok2, canary_ok2, valid_argb2);

        /* Test 3: Off-center spatial coordinate */
        snprintf(test_name, sizeof(test_name), "T1_%02zu_%s_offcenter_spatial", i * 5 + 3,
                 effect->name);
        memset(dst, 0, standard_pixels * sizeof(uint32_t));
        waywal_transition_params_t p3 = {
            .type = effect->type,
            .progress = 0.40f,
            .angle_rad = 0.0f,
            .wave_freq = 20.0f,
            .wave_amp = 0.05f,
            .center_x = 0.25f,
            .center_y = 0.75f,
            .bezier = BEZIER_DEFAULT,
        };
        bool ok3 = render_engine_execute_cpu_transition(dst, src_a, src_b, standard_w, standard_h,
                                                        standard_w * sizeof(uint32_t), &p3);
        bool canary_ok3 = e2e_canary_check(dst, standard_pixels);
        bool valid_argb3 = e2e_is_valid_argb_buffer(dst, standard_w, standard_h, standard_w);
        E2E_CHECK(ok3 && canary_ok3 && valid_argb3, test_name,
                  "execution=%d canary=%d valid_argb=%d", ok3, canary_ok3, valid_argb3);

        /* Test 4: High frequency, amplitude and Bézier ease-in */
        snprintf(test_name, sizeof(test_name), "T1_%02zu_%s_freq_amp_bezier", i * 5 + 4,
                 effect->name);
        memset(dst, 0, standard_pixels * sizeof(uint32_t));
        waywal_transition_params_t p4 = {
            .type = effect->type,
            .progress = 0.55f,
            .angle_rad = 1.5707963f, /* 90 degrees */
            .wave_freq = 40.0f,
            .wave_amp = 0.08f,
            .center_x = 0.5f,
            .center_y = 0.5f,
            .bezier = BEZIER_EASE_IN,
        };
        bool ok4 = render_engine_execute_cpu_transition(dst, src_a, src_b, standard_w, standard_h,
                                                        standard_w * sizeof(uint32_t), &p4);
        bool canary_ok4 = e2e_canary_check(dst, standard_pixels);
        bool valid_argb4 = e2e_is_valid_argb_buffer(dst, standard_w, standard_h, standard_w);
        E2E_CHECK(ok4 && canary_ok4 && valid_argb4, test_name,
                  "execution=%d canary=%d valid_argb=%d", ok4, canary_ok4, valid_argb4);

        /* Test 5: Anisotropic widescreen aspect ratio (640x360) */
        snprintf(test_name, sizeof(test_name), "T1_%02zu_%s_anisotropic_res", i * 5 + 5,
                 effect->name);
        memset(dst_aniso, 0, aniso_pixels * sizeof(uint32_t));
        waywal_transition_params_t p5 = {
            .type = effect->type,
            .progress = 0.45f,
            .angle_rad = -0.52359877f, /* -30 degrees */
            .wave_freq = 25.0f,
            .wave_amp = 0.06f,
            .center_x = 0.80f,
            .center_y = 0.20f,
            .bezier = BEZIER_EASE_OUT,
        };
        bool ok5 = render_engine_execute_cpu_transition(dst_aniso, src_a_aniso, src_b_aniso,
                                                        aniso_w, aniso_h,
                                                        aniso_w * sizeof(uint32_t), &p5);
        bool canary_ok5 = e2e_canary_check(dst_aniso, aniso_pixels);
        bool valid_argb5 = e2e_is_valid_argb_buffer(dst_aniso, aniso_w, aniso_h, aniso_w);
        E2E_CHECK(ok5 && canary_ok5 && valid_argb5, test_name,
                  "execution=%d canary=%d valid_argb=%d", ok5, canary_ok5, valid_argb5);
    }

    e2e_canary_free(src_a);
    e2e_canary_free(src_b);
    e2e_canary_free(dst);

    e2e_canary_free(src_a_aniso);
    e2e_canary_free(src_b_aniso);
    e2e_canary_free(dst_aniso);
}
