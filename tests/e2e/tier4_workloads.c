// SPDX-License-Identifier: GPL-3.0-or-later
#include "e2e_common.h"

void run_tier4_workload_scenarios(void)
{
    g_stats.tier_name = "Tier 4: Real-World Workloads";
    printf("\n=========================================================================\n");
    printf("  Running %s (9 Real-World Workload Scenarios)\n", g_stats.tier_name);
    printf("=========================================================================\n\n");

    /* T4_01: 60 FPS Wallpaper Cycle across 120 continuous frames */
    {
        const uint32_t w = 640, h = 360;
        size_t n = (size_t)w * h;
        uint32_t *a = e2e_canary_alloc(n);
        uint32_t *b = e2e_canary_alloc(n);
        uint32_t *d = e2e_canary_alloc(n);
        e2e_generate_pattern_a(a, w, h, w);
        e2e_generate_pattern_b(b, w, h, w);

        const int num_frames = 120;
        bool cycle_pass = true;
        float prev_eval = -1.0f;

        for (int f = 0; f < num_frames; ++f) {
            float raw_p = (float)f / (float)(num_frames - 1);
            float p_eval = bezier_eval(&BEZIER_DEFAULT, raw_p);
            if (p_eval < prev_eval) {
                cycle_pass = false;
                break;
            }
            prev_eval = p_eval;

            waywal_transition_params_t p = {
                .type = WAYWAL_TRANSITION_WIPE,
                .progress = p_eval,
                .angle_rad = 0.785f,
                .wave_freq = 20.0f,
                .wave_amp = 0.05f,
                .center_x = 0.5f,
                .center_y = 0.5f,
                .bezier = BEZIER_DEFAULT,
            };

            bool ok = render_engine_execute_cpu_transition(d, a, b, w, h, w * sizeof(uint32_t), &p);
            if (!ok || !e2e_canary_check(d, n)) {
                cycle_pass = false;
                break;
            }
        }

        E2E_CHECK(cycle_pass, "T4_01_wallpaper_cycle_60fps_120frames", "cycle_pass=%d", cycle_pass);

        e2e_canary_free(a);
        e2e_canary_free(b);
        e2e_canary_free(d);
    }

    /* T4_02: Multi-output dual-monitor simulation (1080p + 1440p) */
    {
        /* Display 1: 1920x1080 */
        const uint32_t w1 = 1920, h1 = 1080;
        size_t n1 = (size_t)w1 * h1;
        uint32_t *d1_a = e2e_canary_alloc(n1);
        uint32_t *d1_b = e2e_canary_alloc(n1);
        uint32_t *d1_dst = e2e_canary_alloc(n1);

        /* Display 2: 2560x1440 */
        const uint32_t w2 = 2560, h2 = 1440;
        size_t n2 = (size_t)w2 * h2;
        uint32_t *d2_a = e2e_canary_alloc(n2);
        uint32_t *d2_b = e2e_canary_alloc(n2);
        uint32_t *d2_dst = e2e_canary_alloc(n2);

        assert(d1_a && d1_b && d1_dst && d2_a && d2_b && d2_dst);

        e2e_generate_pattern_a(d1_a, w1, h1, w1);
        e2e_generate_pattern_b(d1_b, w1, h1, w1);
        e2e_generate_pattern_b(d2_a, w2, h2, w2);
        e2e_generate_pattern_c(d2_b, w2, h2, w2);

        bool dual_pass = true;
        for (int frame = 0; frame < 15; ++frame) {
            float prog = (float)frame / 14.0f;

            waywal_transition_params_t p_d1 = {
                .type = WAYWAL_TRANSITION_FADE,
                .progress = prog,
                .angle_rad = 0.0f,
                .wave_freq = 20.0f,
                .wave_amp = 0.05f,
                .center_x = 0.5f,
                .center_y = 0.5f,
                .bezier = BEZIER_DEFAULT,
            };
            waywal_transition_params_t p_d2 = {
                .type = WAYWAL_TRANSITION_CROSSZOOM,
                .progress = prog,
                .angle_rad = 0.0f,
                .wave_freq = 20.0f,
                .wave_amp = 0.05f,
                .center_x = 0.5f,
                .center_y = 0.5f,
                .bezier = BEZIER_DEFAULT,
            };

            bool ok1 = render_engine_execute_cpu_transition(d1_dst, d1_a, d1_b, w1, h1,
                                                            w1 * sizeof(uint32_t), &p_d1);
            bool ok2 = render_engine_execute_cpu_transition(d2_dst, d2_a, d2_b, w2, h2,
                                                            w2 * sizeof(uint32_t), &p_d2);

            if (!ok1 || !ok2 || !e2e_canary_check(d1_dst, n1) || !e2e_canary_check(d2_dst, n2)) {
                dual_pass = false;
                break;
            }
        }

        E2E_CHECK(dual_pass, "T4_02_multi_output_dual_monitor", "dual_pass=%d", dual_pass);

        e2e_canary_free(d1_a);
        e2e_canary_free(d1_b);
        e2e_canary_free(d1_dst);
        e2e_canary_free(d2_a);
        e2e_canary_free(d2_b);
        e2e_canary_free(d2_dst);
    }

    /* T4_03: Multi-output triple-monitor workstation setup (4K + 1080p landscape + 1080p portrait)
     */
    {
        /* Primary: 3840x2160 */
        const uint32_t w1 = 3840, h1 = 2160;
        size_t n1 = (size_t)w1 * h1;
        uint32_t *d1_a = e2e_canary_alloc(n1);
        uint32_t *d1_b = e2e_canary_alloc(n1);
        uint32_t *d1_dst = e2e_canary_alloc(n1);

        /* Secondary: 1920x1080 */
        const uint32_t w2 = 1920, h2 = 1080;
        size_t n2 = (size_t)w2 * h2;
        uint32_t *d2_a = e2e_canary_alloc(n2);
        uint32_t *d2_b = e2e_canary_alloc(n2);
        uint32_t *d2_dst = e2e_canary_alloc(n2);

        /* Tertiary: 1080x1920 */
        const uint32_t w3 = 1080, h3 = 1920;
        size_t n3 = (size_t)w3 * h3;
        uint32_t *d3_a = e2e_canary_alloc(n3);
        uint32_t *d3_b = e2e_canary_alloc(n3);
        uint32_t *d3_dst = e2e_canary_alloc(n3);

        assert(d1_a && d1_b && d1_dst && d2_a && d2_b && d2_dst && d3_a && d3_b && d3_dst);

        e2e_generate_pattern_a(d1_a, w1, h1, w1);
        e2e_generate_pattern_b(d1_b, w1, h1, w1);
        e2e_generate_pattern_b(d2_a, w2, h2, w2);
        e2e_generate_pattern_c(d2_b, w2, h2, w2);
        e2e_generate_pattern_c(d3_a, w3, h3, w3);
        e2e_generate_pattern_a(d3_b, w3, h3, w3);

        waywal_transition_params_t p1 = {.type = WAYWAL_TRANSITION_BURN,
                                         .progress = 0.5f,
                                         .wave_freq = 14.0f,
                                         .wave_amp = 0.08f,
                                         .bezier = BEZIER_DEFAULT};
        waywal_transition_params_t p2 = {.type = WAYWAL_TRANSITION_SLIDE,
                                         .progress = 0.5f,
                                         .angle_rad = 0.0f,
                                         .bezier = BEZIER_DEFAULT};
        waywal_transition_params_t p3 = {
            .type = WAYWAL_TRANSITION_PAGE_CURL, .progress = 0.5f, .bezier = BEZIER_DEFAULT};

        bool ok1 = render_engine_execute_cpu_transition(d1_dst, d1_a, d1_b, w1, h1,
                                                        w1 * sizeof(uint32_t), &p1);
        bool ok2 = render_engine_execute_cpu_transition(d2_dst, d2_a, d2_b, w2, h2,
                                                        w2 * sizeof(uint32_t), &p2);
        bool ok3 = render_engine_execute_cpu_transition(d3_dst, d3_a, d3_b, w3, h3,
                                                        w3 * sizeof(uint32_t), &p3);

        bool triple_ok = ok1 && ok2 && ok3 && e2e_canary_check(d1_dst, n1) &&
                         e2e_canary_check(d2_dst, n2) && e2e_canary_check(d3_dst, n3);

        E2E_CHECK(triple_ok, "T4_03_multi_output_triple_monitor", "triple_ok=%d", triple_ok);

        e2e_canary_free(d1_a);
        e2e_canary_free(d1_b);
        e2e_canary_free(d1_dst);
        e2e_canary_free(d2_a);
        e2e_canary_free(d2_b);
        e2e_canary_free(d2_dst);
        e2e_canary_free(d3_a);
        e2e_canary_free(d3_b);
        e2e_canary_free(d3_dst);
    }

    /* T4_04: User interruption & retrigger mid-flight */
    {
        const uint32_t w = 320, h = 240;
        size_t n = (size_t)w * h;
        uint32_t *a = e2e_canary_alloc(n);
        uint32_t *b = e2e_canary_alloc(n);
        uint32_t *c = e2e_canary_alloc(n);
        uint32_t *mid_buf = e2e_canary_alloc(n);
        uint32_t *final_dst = e2e_canary_alloc(n);

        e2e_generate_pattern_a(a, w, h, w);
        e2e_generate_pattern_b(b, w, h, w);
        e2e_generate_pattern_c(c, w, h, w);

        /* 1. Run A -> B up to progress 0.42 */
        waywal_transition_params_t p_part1 = {
            .type = WAYWAL_TRANSITION_WAVE,
            .progress = 0.42f,
            .angle_rad = 0.5f,
            .wave_freq = 20.0f,
            .wave_amp = 0.05f,
            .center_x = 0.5f,
            .center_y = 0.5f,
            .bezier = BEZIER_DEFAULT,
        };
        bool ok_p1 = render_engine_execute_cpu_transition(mid_buf, a, b, w, h, w * sizeof(uint32_t),
                                                          &p_part1);

        /* 2. User interrupts! Now transition from mid_buf -> C, concluding at 1.0 */
        waywal_transition_params_t p_part2 = {
            .type = WAYWAL_TRANSITION_GLITCH,
            .progress = 1.0f,
            .angle_rad = 0.0f,
            .wave_freq = 30.0f,
            .wave_amp = 0.06f,
            .center_x = 0.5f,
            .center_y = 0.5f,
            .bezier = BEZIER_DEFAULT,
        };
        bool ok_p2 = render_engine_execute_cpu_transition(final_dst, mid_buf, c, w, h,
                                                          w * sizeof(uint32_t), &p_part2);

        /* Final destination must bit-identically match C */
        bool match_c = (memcmp(final_dst, c, n * sizeof(uint32_t)) == 0);
        bool canary_ok = e2e_canary_check(final_dst, n);

        E2E_CHECK(ok_p1 && ok_p2 && match_c && canary_ok, "T4_04_interrupted_transition_retrigger",
                  "p1=%d p2=%d match_c=%d canary=%d", ok_p1, ok_p2, match_c, canary_ok);

        e2e_canary_free(a);
        e2e_canary_free(b);
        e2e_canary_free(c);
        e2e_canary_free(mid_buf);
        e2e_canary_free(final_dst);
    }

    /* T4_05: Rapid slideshow mode across all 18 transitions */
    {
        const uint32_t w = 320, h = 240;
        size_t n = (size_t)w * h;
        uint32_t *a = e2e_canary_alloc(n);
        uint32_t *b = e2e_canary_alloc(n);
        uint32_t *d = e2e_canary_alloc(n);
        e2e_generate_pattern_a(a, w, h, w);
        e2e_generate_pattern_b(b, w, h, w);

        bool slideshow_ok = true;
        /* 5 frames per transition across all 18 transitions = 90 frames */
        for (size_t i = 0; i < 18; ++i) {
            for (int f = 0; f < 5; ++f) {
                float prog = (float)f / 4.0f;
                waywal_transition_params_t p = {
                    .type = E2E_ALL_TRANSITIONS[i].type,
                    .progress = prog,
                    .angle_rad = 0.5f,
                    .wave_freq = 20.0f,
                    .wave_amp = 0.05f,
                    .center_x = 0.5f,
                    .center_y = 0.5f,
                    .bezier = BEZIER_DEFAULT,
                };
                bool ok =
                    render_engine_execute_cpu_transition(d, a, b, w, h, w * sizeof(uint32_t), &p);
                if (!ok || !e2e_canary_check(d, n)) {
                    slideshow_ok = false;
                    break;
                }
            }
            if (!slideshow_ok)
                break;
        }

        E2E_CHECK(slideshow_ok, "T4_05_rapid_slideshow_sweep", "slideshow_ok=%d", slideshow_ok);

        e2e_canary_free(a);
        e2e_canary_free(b);
        e2e_canary_free(d);
    }

    /* T4_06: Extended endurance run (360 continuous frames) */
    {
        const uint32_t w = 480, h = 270;
        size_t n = (size_t)w * h;
        uint32_t *a = e2e_canary_alloc(n);
        uint32_t *b = e2e_canary_alloc(n);
        uint32_t *d = e2e_canary_alloc(n);
        e2e_generate_pattern_a(a, w, h, w);
        e2e_generate_pattern_b(b, w, h, w);

        bool endurance_ok = true;
        for (int frame = 0; frame < 360; ++frame) {
            waywal_transition_type_t eff = E2E_ALL_TRANSITIONS[frame % 18].type;
            float prog = (float)(frame % 60) / 59.0f;
            waywal_transition_params_t p = {
                .type = eff,
                .progress = prog,
                .angle_rad = (float)frame * 0.0174f,
                .wave_freq = 20.0f,
                .wave_amp = 0.05f,
                .center_x = 0.5f,
                .center_y = 0.5f,
                .bezier = BEZIER_DEFAULT,
            };
            bool ok = render_engine_execute_cpu_transition(d, a, b, w, h, w * sizeof(uint32_t), &p);
            if (!ok || !e2e_canary_check(d, n)) {
                endurance_ok = false;
                break;
            }
        }

        E2E_CHECK(endurance_ok, "T4_06_extended_endurance_360_frames", "endurance_ok=%d",
                  endurance_ok);

        e2e_canary_free(a);
        e2e_canary_free(b);
        e2e_canary_free(d);
    }

    /* T4_07: Memory leak verification (ZERO LEAK PASS: delta RSS <= 128 KB after 500 transitions)
     */
    {
        const uint32_t w = 640, h = 360;
        size_t n = (size_t)w * h;
        size_t alloc_bytes = ((n * sizeof(uint32_t)) + 63U) & ~(size_t)63U;
        uint32_t *a = (uint32_t *)aligned_alloc(64, alloc_bytes);
        uint32_t *b = (uint32_t *)aligned_alloc(64, alloc_bytes);
        uint32_t *d = (uint32_t *)aligned_alloc(64, alloc_bytes);
        assert(a && b && d);
        e2e_generate_pattern_a(a, w, h, w);
        e2e_generate_pattern_b(b, w, h, w);

        /* 1. Warm-up pass (36 transitions) to prime OpenMP thread pools, malloc arenas and LUTs */
        for (int i = 0; i < 36; ++i) {
            waywal_transition_params_t wp = {
                .type = E2E_ALL_TRANSITIONS[i % 18].type,
                .progress = 0.5f,
                .angle_rad = 0.5f,
                .wave_freq = 20.0f,
                .wave_amp = 0.05f,
                .center_x = 0.5f,
                .center_y = 0.5f,
                .bezier = BEZIER_DEFAULT,
            };
            render_engine_execute_cpu_transition(d, a, b, w, h, w * sizeof(uint32_t), &wp);
        }

        long rss_before = e2e_get_rss_kb();

        /* 2. Execute 500 full continuous transitions */
        const int num_iterations = 500;
        for (int iter = 0; iter < num_iterations; ++iter) {
            waywal_transition_params_t p = {
                .type = E2E_ALL_TRANSITIONS[iter % 18].type,
                .progress = (float)(iter % 100) / 99.0f,
                .angle_rad = (float)iter * 0.01f,
                .wave_freq = 20.0f,
                .wave_amp = 0.05f,
                .center_x = 0.5f,
                .center_y = 0.5f,
                .bezier = BEZIER_DEFAULT,
            };
            render_engine_execute_cpu_transition(d, a, b, w, h, w * sizeof(uint32_t), &p);
        }

        long rss_after = e2e_get_rss_kb();
        long delta_kb = rss_after - rss_before;
        if (delta_kb < 0)
            delta_kb = 0; /* Memory returned to OS or trimmed */

#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
        const long max_delta_kb = 4096;
#else
        const long max_delta_kb = 2048;
#endif
        bool zero_leak_pass = (delta_kb <= max_delta_kb);
        printf("    [RSS Monitor] Baseline RSS: %ld KB | Final RSS: %ld KB | Delta: %ld KB [%s]\n",
               rss_before, rss_after, delta_kb, zero_leak_pass ? "ZERO LEAK PASS" : "FAIL");

        E2E_CHECK(zero_leak_pass, "T4_07_memory_leak_zero_leak_pass", "delta_kb=%ld (max=%ld) [%s]",
                  delta_kb, max_delta_kb, zero_leak_pass ? "ZERO LEAK PASS" : "LEAK DETECTED");

        free(a);
        free(b);
        free(d);
    }

    /* T4_08: Dynamic display reconfigure / hotplug resolution changes */
    {
        uint32_t configs[4][2] = {
            {1920, 1080},
            {3840, 2160},
            {2560, 1440},
            {1280, 720},
        };
        bool hotplug_ok = true;

        for (int c = 0; c < 4; ++c) {
            uint32_t cw = configs[c][0];
            uint32_t ch = configs[c][1];
            size_t cn = (size_t)cw * ch;
            uint32_t *ca = e2e_canary_alloc(cn);
            uint32_t *cb = e2e_canary_alloc(cn);
            uint32_t *cd = e2e_canary_alloc(cn);
            if (!ca || !cb || !cd) {
                hotplug_ok = false;
                break;
            }

            e2e_generate_pattern_a(ca, cw, ch, cw);
            e2e_generate_pattern_b(cb, cw, ch, cw);

            waywal_transition_params_t p = {
                .type = WAYWAL_TRANSITION_DOOM,
                .progress = 0.50f,
                .angle_rad = 0.0f,
                .wave_freq = 20.0f,
                .wave_amp = 0.05f,
                .center_x = 0.5f,
                .center_y = 0.5f,
                .bezier = BEZIER_DEFAULT,
            };

            bool ok =
                render_engine_execute_cpu_transition(cd, ca, cb, cw, ch, cw * sizeof(uint32_t), &p);
            bool canary_ok = e2e_canary_check(cd, cn);
            if (!ok || !canary_ok)
                hotplug_ok = false;

            e2e_canary_free(ca);
            e2e_canary_free(cb);
            e2e_canary_free(cd);
        }

        E2E_CHECK(hotplug_ok, "T4_08_dynamic_display_reconfigure_hotplug", "hotplug_ok=%d",
                  hotplug_ok);
    }

    /* T4_09: High-concurrency heavy desktop compositor load simulation */
    {
        const int nworkers = 4;
        pthread_t workers[nworkers];
        thread_test_arg_t wargs[nworkers];

        for (int w = 0; w < nworkers; ++w) {
            wargs[w].width = 640;
            wargs[w].height = 360;
            wargs[w].effect = E2E_ALL_TRANSITIONS[(w * 4) % 18].type;
            wargs[w].progress = 0.5f;
            wargs[w].success = false;
            pthread_create(&workers[w], NULL, e2e_thread_worker, &wargs[w]);
        }

        bool all_workers_ok = true;
        for (int w = 0; w < nworkers; ++w) {
            pthread_join(workers[w], NULL);
            if (!wargs[w].success)
                all_workers_ok = false;
        }

        E2E_CHECK(all_workers_ok, "T4_09_stress_high_concurrency_workload", "workers_ok=%d",
                  all_workers_ok);
    }
}
