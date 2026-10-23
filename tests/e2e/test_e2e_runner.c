#include "e2e_common.h"

e2e_test_stats_t g_stats = {0, 0, 0, ""};

void *e2e_thread_worker(void *arg)
{
    thread_test_arg_t *targ = (thread_test_arg_t *)arg;
    size_t npixels = (size_t)targ->width * targ->height;
    uint32_t *a = e2e_canary_alloc(npixels);
    uint32_t *b = e2e_canary_alloc(npixels);
    uint32_t *d = e2e_canary_alloc(npixels);
    if (!a || !b || !d) {
        targ->success = false;
        return NULL;
    }

    e2e_generate_pattern_a(a, targ->width, targ->height, targ->width);
    e2e_generate_pattern_b(b, targ->width, targ->height, targ->width);

    waywal_transition_params_t p = {
        .type = targ->effect,
        .progress = targ->progress,
        .angle_rad = 0.5f,
        .wave_freq = 20.0f,
        .wave_amp = 0.05f,
        .center_x = 0.5f,
        .center_y = 0.5f,
        .bezier = BEZIER_DEFAULT,
    };

    bool ok = render_engine_execute_cpu_transition(d, a, b, targ->width, targ->height,
                                                   targ->width * sizeof(uint32_t), &p);
    bool c_ok = e2e_canary_check(d, npixels);
    bool v_ok = e2e_is_valid_argb_buffer(d, targ->width, targ->height, targ->width);

    targ->success = ok && c_ok && v_ok;

    e2e_canary_free(a);
    e2e_canary_free(b);
    e2e_canary_free(d);
    return NULL;
}

int main(void)
{
    waywal_log_set_level(WAYWAL_LOG_LEVEL_WARN);

    printf("=========================================================================\n");
    printf("   WAYWAL END-TO-END (E2E) TRANSITION TEST SUITE (TIERS 1 - 4)\n");
    printf("=========================================================================\n");

    uint64_t t_start = e2e_get_time_ns();

    /* 1. Execute Tier 1: Feature Coverage (90 tests) */
    run_tier1_feature_coverage();

    /* 2. Execute Tier 2: Boundary & Corner Cases (90 tests) */
    run_tier2_boundary_corner();

    /* 3. Execute Tier 3: Cross-Feature Combinations (18 tests) */
    run_tier3_cross_feature();

    /* 4. Execute Tier 4: Real-World Workload Scenarios (9 tests) */
    run_tier4_workload_scenarios();

    uint64_t t_end = e2e_get_time_ns();
    double elapsed_sec = (double)(t_end - t_start) / 1e9;

    printf("\n=========================================================================\n");
    printf("   E2E TEST SUITE EXECUTION SUMMARY\n");
    printf("=========================================================================\n");
    printf("  Tier 1 (Feature Coverage):        90 Tests Target\n");
    printf("  Tier 2 (Boundary & Corner Cases): 90 Tests Target\n");
    printf("  Tier 3 (Cross-Feature Combos):    18 Tests Target\n");
    printf("  Tier 4 (Real-World Workloads):     9 Tests Target\n");
    printf("-------------------------------------------------------------------------\n");
    printf("  Total Test Cases Executed: %d\n", g_stats.total);
    printf("  Total Test Cases Passed:   %d\n", g_stats.passed);
    printf("  Total Test Cases Failed:   %d\n", g_stats.failed);
    printf("  Total Execution Duration:  %.3f seconds\n", elapsed_sec);
    printf("=========================================================================\n\n");

    if (g_stats.failed == 0 && g_stats.total >= 207) {
        printf(">>> ALL E2E TESTS PASSED SUCCESSFULLY! (%d/%d) <<<\n\n", g_stats.passed,
               g_stats.total);
        return 0;
    } else {
        fprintf(stderr, ">>> E2E TEST SUITE FAILED (%d failed out of %d tests) <<<\n\n",
                g_stats.failed, g_stats.total);
        return 1;
    }
}
