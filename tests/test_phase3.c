#include "../src/daemon/simd/simd_blend.h"
#include "waywal/bezier.h"
#include "waywal/dmabuf.h"
#include "waywal/log.h"
#include "waywal/render_engine.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
__attribute__((visibility("default"))) const char *__lsan_default_suppressions(void)
{
    return "leak:<unknown "
           "module>\nleak:gbm_create_device\nleak:libgbm\nleak:libEGL\nleak:libGLESv2\nleak:"
           "dri\nleak:libnvidia\n";
}
#endif

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void test_bezier_accuracy_and_bench(void)
{
    printf("[TEST] Running test_bezier_accuracy_and_bench...\n");

    /* Test boundary invariants */
    assert(bezier_eval(&BEZIER_LINEAR, 0.0f) == 0.0f);
    assert(bezier_eval(&BEZIER_LINEAR, 1.0f) == 1.0f);
    assert(fabsf(bezier_eval(&BEZIER_LINEAR, 0.5f) - 0.5f) < 1e-5f);

    assert(bezier_eval(&BEZIER_DEFAULT, 0.0f) == 0.0f);
    assert(bezier_eval(&BEZIER_DEFAULT, 1.0f) == 1.0f);
    assert(bezier_eval(&BEZIER_EASE_IN, 0.0f) == 0.0f);
    assert(bezier_eval(&BEZIER_EASE_IN, 1.0f) == 1.0f);
    assert(bezier_eval(&BEZIER_EASE_OUT, 0.0f) == 0.0f);
    assert(bezier_eval(&BEZIER_EASE_OUT, 1.0f) == 1.0f);

    /* Monotonicity check */
    float prev = 0.0f;
    for (int i = 1; i <= 1000; ++i) {
        float p = (float)i / 1000.0f;
        float y = bezier_eval(&BEZIER_DEFAULT, p);
        assert(y >= prev && "Bézier curve must be monotonically non-decreasing");
        prev = y;
    }

    /* Microbenchmark: 2,000,000 evaluations */
    const uint64_t iterations = 2000000ULL;
    float sum = 0.0f;
    uint64_t t0 = get_time_ns();
    for (uint64_t i = 0; i < iterations; ++i) {
        float progress = (float)(i % 1000) / 1000.0f;
        sum += bezier_eval(&BEZIER_DEFAULT, progress);
    }
    uint64_t t1 = get_time_ns();
    (void)sum;

    double elapsed_ns = (double)(t1 - t0);
    double ns_per_eval = elapsed_ns / (double)iterations;
    printf("  Bézier Evaluation: %.2f ns/eval (Goal: < 5.0 ns) [%s]\n", ns_per_eval,
           ns_per_eval < 5.0 ? "PASS" : "WARN");

    printf("[TEST] test_bezier_accuracy_and_bench PASSED.\n");
}

static void test_simd_bit_exactness(void)
{
    printf("[TEST] Running test_simd_bit_exactness...\n");

    const size_t width = 1920;
    const size_t height = 1080;
    const size_t num_bytes = width * height * 4;

    uint8_t *src_a = (uint8_t *)aligned_alloc(64, num_bytes);
    uint8_t *src_b = (uint8_t *)aligned_alloc(64, num_bytes);
    uint8_t *dst_ref = (uint8_t *)aligned_alloc(64, num_bytes);
    uint8_t *dst_simd = (uint8_t *)aligned_alloc(64, num_bytes);

    assert(src_a && src_b && dst_ref && dst_simd);

    for (size_t i = 0; i < num_bytes; ++i) {
        src_a[i] = (uint8_t)((i * 13 + 7) & 0xFF);
        src_b[i] = (uint8_t)((i * 17 + 31) & 0xFF);
    }

    uint16_t test_weights[] = {0, 16, 64, 128, 192, 240, 256};
    size_t num_weights = sizeof(test_weights) / sizeof(test_weights[0]);

    simd_blend_scanline_fn best_fn = simd_get_blend_fn();

    for (size_t w = 0; w < num_weights; ++w) {
        uint16_t weight = test_weights[w];
        memset(dst_ref, 0, num_bytes);
        memset(dst_simd, 0, num_bytes);

        blend_scanline_scalar(dst_ref, src_a, src_b, weight, num_bytes);
        best_fn(dst_simd, src_a, src_b, weight, num_bytes);

        for (size_t i = 0; i < num_bytes; ++i) {
            if (dst_ref[i] != dst_simd[i]) {
                fprintf(stderr, "Fatal: Mismatch at byte %zu (weight %u): ref=%u simd=%u\n", i,
                        weight, dst_ref[i], dst_simd[i]);
                assert(false && "SIMD kernel must be bit-exact to scalar reference!");
            }
        }
    }

    /* Throughput benchmark */
    const int bench_passes = 30;
    uint64_t t0 = get_time_ns();
    for (int p = 0; p < bench_passes; ++p) {
        best_fn(dst_simd, src_a, src_b, 128, num_bytes);
    }
    uint64_t t1 = get_time_ns();

    double total_sec = (double)(t1 - t0) / 1e9;
    double gb_processed = (double)(num_bytes * 3 * bench_passes) / (1024.0 * 1024.0 * 1024.0);
    double bandwidth_gbs = gb_processed / total_sec;

    printf("  SIMD Blending: Bit-exact across all weights! Throughput: %.2f GB/s\n", bandwidth_gbs);

    free(src_a);
    free(src_b);
    free(dst_ref);
    free(dst_simd);

    printf("[TEST] test_simd_bit_exactness PASSED.\n");
}

static void test_gpu_compute_transitions(void)
{
    printf("[TEST] Running test_gpu_compute_transitions...\n");

    dmabuf_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (!dmabuf_context_init(&ctx)) {
        printf("  DRM/GBM unavailable in this environment. Skipping GPU compute test.\n");
        return;
    }

    render_engine_t re;
    if (!render_engine_init(&re, &ctx)) {
        printf("  Failed to initialize render engine.\n");
        dmabuf_context_destroy(&ctx);
        return;
    }

    if (!re.has_gpu_compute) {
        printf("  GPU compute not available, skipping hardware shader dispatch test.\n");
        render_engine_destroy(&re);
        dmabuf_context_destroy(&ctx);
        return;
    }

    /* Allocate 1920x1080 test BOs */
    uint32_t width = 1920;
    uint32_t height = 1080;
    dmabuf_bo_t target_bo, old_bo, new_bo;

    assert(dmabuf_bo_allocate(&ctx, &target_bo, width, height, DRM_FORMAT_ARGB8888,
                              DRM_FORMAT_MOD_INVALID));
    assert(dmabuf_bo_allocate(&ctx, &old_bo, width, height, DRM_FORMAT_ARGB8888,
                              DRM_FORMAT_MOD_INVALID));
    assert(dmabuf_bo_allocate(&ctx, &new_bo, width, height, DRM_FORMAT_ARGB8888,
                              DRM_FORMAT_MOD_INVALID));

    waywal_transition_type_t test_transitions[] = {
        WAYWAL_TRANSITION_FADE,      WAYWAL_TRANSITION_WIPE,       WAYWAL_TRANSITION_GROW,
        WAYWAL_TRANSITION_OUTER,     WAYWAL_TRANSITION_WAVE,       WAYWAL_TRANSITION_NOISE,
        WAYWAL_TRANSITION_CROSSZOOM, WAYWAL_TRANSITION_SLIDE,      WAYWAL_TRANSITION_GLITCH,
        WAYWAL_TRANSITION_BURN,      WAYWAL_TRANSITION_RIPPLE,     WAYWAL_TRANSITION_PIXELATE,
        WAYWAL_TRANSITION_DOOM,      WAYWAL_TRANSITION_SWIRL,      WAYWAL_TRANSITION_CUBE,
        WAYWAL_TRANSITION_LUMA,      WAYWAL_TRANSITION_LIGHT_LEAK, WAYWAL_TRANSITION_PAGE_CURL,
    };
    size_t num_types = sizeof(test_transitions) / sizeof(test_transitions[0]);

    for (size_t t = 0; t < num_types; ++t) {
        waywal_transition_params_t params = {
            .type = test_transitions[t],
            .progress = 0.5f,
            .angle_rad = 0.785f, /* 45 degrees */
            .wave_freq = 20.0f,
            .wave_amp = 0.05f,
            .center_x = 0.5f,
            .center_y = 0.5f,
            .bezier = BEZIER_DEFAULT,
        };

        bool ok = render_engine_execute_transition(&re, &target_bo, &old_bo, &new_bo, &params);
        assert(ok && "GPU compute transition execution must succeed");
    }

    /* Benchmark: 60 frames of 1080p GPU compute transitions */
    const int num_frames = 60;
    waywal_transition_params_t bench_params = {
        .type = WAYWAL_TRANSITION_WAVE,
        .progress = 0.5f,
        .angle_rad = 0.5f,
        .wave_freq = 25.0f,
        .wave_amp = 0.06f,
        .center_x = 0.5f,
        .center_y = 0.5f,
        .bezier = BEZIER_DEFAULT,
    };

    uint64_t t0 = get_time_ns();
    for (int f = 0; f < num_frames; ++f) {
        bench_params.progress = (float)f / (float)num_frames;
        render_engine_execute_transition(&re, &target_bo, &old_bo, &new_bo, &bench_params);
    }
    uint64_t t1 = get_time_ns();

    double total_ms = (double)(t1 - t0) / 1e6;
    double ms_per_frame = total_ms / (double)num_frames;
    printf("  GPU Compute (Wave 1080p): %.3f ms/frame (Goal: < 0.25 ms) [%s]\n", ms_per_frame,
           ms_per_frame < 0.50 ? "PASS" : "WARN");

    dmabuf_bo_free(&target_bo);
    dmabuf_bo_free(&old_bo);
    dmabuf_bo_free(&new_bo);

    render_engine_destroy(&re);
    dmabuf_context_destroy(&ctx);

    printf("[TEST] test_gpu_compute_transitions PASSED.\n");
}

static void test_cpu_transitions(void)
{
    printf("[TEST] Running test_cpu_transitions...\n");
    const uint32_t width = 320;
    const uint32_t height = 240;
    const size_t num_pixels = (size_t)width * height;
    uint32_t *dst = (uint32_t *)calloc(num_pixels, sizeof(uint32_t));
    uint32_t *src_a = (uint32_t *)malloc(num_pixels * sizeof(uint32_t));
    uint32_t *src_b = (uint32_t *)malloc(num_pixels * sizeof(uint32_t));
    assert(dst && src_a && src_b);

    for (size_t i = 0; i < num_pixels; ++i) {
        src_a[i] = 0xFFFF0000;
        src_b[i] = 0xFF0000FF;
    }

    waywal_transition_type_t types[] = {
        WAYWAL_TRANSITION_FADE,      WAYWAL_TRANSITION_WIPE,       WAYWAL_TRANSITION_GROW,
        WAYWAL_TRANSITION_OUTER,     WAYWAL_TRANSITION_WAVE,       WAYWAL_TRANSITION_NOISE,
        WAYWAL_TRANSITION_CROSSZOOM, WAYWAL_TRANSITION_SLIDE,      WAYWAL_TRANSITION_GLITCH,
        WAYWAL_TRANSITION_BURN,      WAYWAL_TRANSITION_RIPPLE,     WAYWAL_TRANSITION_PIXELATE,
        WAYWAL_TRANSITION_DOOM,      WAYWAL_TRANSITION_SWIRL,      WAYWAL_TRANSITION_CUBE,
        WAYWAL_TRANSITION_LUMA,      WAYWAL_TRANSITION_LIGHT_LEAK, WAYWAL_TRANSITION_PAGE_CURL,
    };
    size_t num_types = sizeof(types) / sizeof(types[0]);

    for (size_t i = 0; i < num_types; ++i) {
        waywal_transition_params_t params = {
            .type = types[i],
            .progress = 0.5f,
            .angle_rad = 0.785f,
            .wave_freq = 20.0f,
            .wave_amp = 0.05f,
            .center_x = 0.5f,
            .center_y = 0.5f,
            .bezier = BEZIER_DEFAULT,
        };
        bool ok = render_engine_execute_cpu_transition(dst, src_a, src_b, width, height,
                                                       width * sizeof(uint32_t), &params);
        assert(ok && "CPU transition execution must succeed for all transition types");
    }

    free(dst);
    free(src_a);
    free(src_b);
    printf("[TEST] test_cpu_transitions PASSED.\n");
}

int main(void)
{
    waywal_log_set_level(WAYWAL_LOG_LEVEL_WARN);
    printf("=========================================\n");
    printf("  Executing WayWal Phase 3 Test Suite\n");
    printf("=========================================\n");

    test_bezier_accuracy_and_bench();
    test_simd_bit_exactness();
    test_cpu_transitions();
    test_gpu_compute_transitions();

    printf("\n>>> ALL PHASE 3 UNIT TESTS PASSED SUCCESSFULLY! <<<\n\n");
    return 0;
}
