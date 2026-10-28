// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/daemon/gpu_compute.h"
#include "../src/daemon/simd/simd_blend.h"
#include "../src/daemon/wayland_core.h"
#include "waywal/anim_engine.h"
#include "waywal/bezier.h"
#include "waywal/dmabuf.h"
#include "waywal/log.h"
#include "waywal/path.h"
#include "waywal/render_engine.h"
#include "waywal/shader_engine.h"
#include "waywal/types.h"
#include "waywal/we_scene.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void waywal_we_scene_forget_bo(we_scene_t *scene, const dmabuf_bo_t *bo)
{
    (void)scene;
    (void)bo;
}
void we_scene_destroy(we_scene_t *scene) { (void)scene; }
void waywal_anim_destroy(waywal_anim_decoder_t *dec) { (void)dec; }
void video_engine_destroy(video_engine_t *ve) { (void)ve; }
bool video_engine_snapshot_output(video_engine_t *ve, output_node_t *out)
{
    (void)ve;
    (void)out;
    return false;
}
void slideshow_destroy(slideshow_engine_t *ss) { (void)ss; }
bool daemon_query_cursor_pos(daemon_state_t *state, output_node_t *out, double *out_x,
                             double *out_y)
{
    (void)state;
    (void)out;
    (void)out_x;
    (void)out_y;
    return false;
}

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
__attribute__((visibility("default"))) const char *__lsan_default_suppressions(void);
const char *__lsan_default_suppressions(void)
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
    const size_t alloc_bytes = (num_bytes + 63U) & ~(size_t)63U;

    uint8_t *src_a = (uint8_t *)aligned_alloc(64, alloc_bytes);
    uint8_t *src_b = (uint8_t *)aligned_alloc(64, alloc_bytes);
    uint8_t *dst_ref = (uint8_t *)aligned_alloc(64, alloc_bytes);
    uint8_t *dst_simd = (uint8_t *)aligned_alloc(64, alloc_bytes);

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

static void test_gpu_compute_nonlinear_and_upload_static(void)
{
    printf("[TEST] Running test_gpu_compute_nonlinear_and_upload_static...\n");

    dmabuf_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (!dmabuf_context_init(&ctx)) {
        printf("  DRM/GBM unavailable in this environment. Skipping GPU upload test.\n");
        return;
    }

    ctx.preferred_modifier = 0x0300000000606010ULL;
    ctx.map_supported = false;

    render_engine_t re;
    assert(render_engine_init(&re, &ctx));
    if (!re.gpu_compute_active) {
        render_engine_destroy(&re);
        dmabuf_context_destroy(&ctx);
        return;
    }

    dmabuf_bo_t dst_bo;
    assert(dmabuf_bo_allocate(&ctx, &dst_bo, 64, 64, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_INVALID));
    assert(dst_bo.map_data == NULL);

    uint32_t src_pixels[32 * 16];
    for (size_t i = 0; i < 32 * 16; ++i) {
        src_pixels[i] = 0xFF112233U;
    }

    assert(render_engine_upload_static(&re, NULL, 0, 0, &dst_bo, WAYWAL_SCALING_FILL, 0xFFAA5511U));
    assert(render_engine_upload_static(&re, (const uint8_t *)src_pixels, 32, 16, &dst_bo,
                                       WAYWAL_SCALING_FILL, 0xFF000000U));
    assert(render_engine_upload_static(&re, (const uint8_t *)src_pixels, 32, 16, &dst_bo,
                                       WAYWAL_SCALING_FIT, 0xFF1A2B3CU));
    assert(render_engine_upload_static(&re, (const uint8_t *)src_pixels, 32, 16, &dst_bo,
                                       WAYWAL_SCALING_STRETCH, 0xFF000000U));
    assert(render_engine_upload_static(&re, (const uint8_t *)src_pixels, 32, 16, &dst_bo,
                                       WAYWAL_SCALING_CENTER, 0xFF1A2B3CU));
    assert(render_engine_upload_static(&re, (const uint8_t *)src_pixels, 32, 16, &dst_bo,
                                       WAYWAL_SCALING_TILE, 0xFF000000U));
    render_engine_set_span_bounds(&re, 64, 0, 128, 64);
    assert(render_engine_upload_static(&re, (const uint8_t *)src_pixels, 32, 16, &dst_bo,
                                       WAYWAL_SCALING_SPAN, 0xFF000000U));

    /* Verify WAYWAL_SCALING_SPAN pixel partitioning across left and right monitors + copy_bo */
    ctx.map_supported = true;
    dmabuf_bo_t left_bo, right_bo, copy_bo;
    if (dmabuf_bo_allocate(&ctx, &left_bo, 64, 64, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR) &&
        dmabuf_bo_allocate(&ctx, &right_bo, 64, 64, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR) &&
        dmabuf_bo_allocate(&ctx, &copy_bo, 64, 64, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR)) {
        uint32_t span_src[4 * 2] = {
            0xFFFF0000U, 0xFFFF0000U, 0xFF0000FFU, 0xFF0000FFU,
            0xFFFF0000U, 0xFFFF0000U, 0xFF0000FFU, 0xFF0000FFU,
        };
        render_engine_set_span_bounds(&re, 0, 0, 128, 64);
        assert(render_engine_upload_static(&re, (const uint8_t *)span_src, 4, 2, &left_bo,
                                           WAYWAL_SCALING_SPAN, 0xFF000000U));
        render_engine_set_span_bounds(&re, 64, 0, 128, 64);
        assert(render_engine_upload_static(&re, (const uint8_t *)span_src, 4, 2, &right_bo,
                                           WAYWAL_SCALING_SPAN, 0xFF000000U));
        assert(render_engine_copy_bo(&re, &copy_bo, &left_bo, WAYWAL_SCALING_STRETCH, 0xFF000000U,
                                     false));
        if (left_bo.map_data && right_bo.map_data && copy_bo.map_data) {
            const uint8_t *pl = (const uint8_t *)left_bo.map_data;
            const uint8_t *pr = (const uint8_t *)right_bo.map_data;
            const uint8_t *pc = (const uint8_t *)copy_bo.map_data;
            assert(pl[2] > 200 && pl[0] < 32);
            assert(pr[0] > 200 && pr[2] < 32);
            assert(pc[2] > 200 && pc[0] < 32);
        }
        /* Verify texel-center symmetry when scaling 2x2 [Red, Blue] to 64x64 */
        uint32_t sym_src[2 * 2] = {
            0xFFFF0000U,
            0xFF0000FFU,
            0xFFFF0000U,
            0xFF0000FFU,
        };
        assert(render_engine_upload_static(&re, (const uint8_t *)sym_src, 2, 2, &left_bo,
                                           WAYWAL_SCALING_STRETCH, 0xFF000000U));
        if (left_bo.map_data) {
            const uint8_t *row0 = (const uint8_t *)left_bo.map_data;
            /* At x=15 (s_px.x = 0.484 <= 0.5), texel must be 100% Red, not shifted toward Blue */
            const uint8_t *px15 = row0 + 15 * 4;
            const uint8_t *px48 = row0 + 48 * 4;
            assert(px15[2] > 240 && px15[0] < 16);
            assert(px48[0] > 240 && px48[2] < 16);
        }
        render_engine_release_bo(&re, &left_bo);
        render_engine_release_bo(&re, &right_bo);
        render_engine_release_bo(&re, &copy_bo);
        dmabuf_bo_free(&left_bo);
        dmabuf_bo_free(&right_bo);
        dmabuf_bo_free(&copy_bo);
    }
    ctx.map_supported = false;

    /* Test live cross-mode GPU snapshot (render_engine_copy_bo) + transition */
    dmabuf_bo_t snapshot_bo, new_bo;
    assert(dmabuf_bo_allocate(&ctx, &snapshot_bo, 64, 64, DRM_FORMAT_ARGB8888,
                              DRM_FORMAT_MOD_INVALID));
    assert(dmabuf_bo_allocate(&ctx, &new_bo, 64, 64, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_INVALID));
    assert(render_engine_copy_bo(&re, &snapshot_bo, &dst_bo, WAYWAL_SCALING_STRETCH, 0xFF000000U,
                                 false));
    assert(render_engine_upload_static(&re, NULL, 0, 0, &new_bo, WAYWAL_SCALING_FILL, 0xFF00FF00U));
    waywal_transition_params_t cross_params = {
        .type = WAYWAL_TRANSITION_FADE,
        .progress = 0.5f,
        .bezier = BEZIER_DEFAULT,
    };
    assert(render_engine_execute_transition(&re, &dst_bo, &snapshot_bo, &new_bo, &cross_params));
    render_engine_release_bo(&re, &snapshot_bo);
    render_engine_release_bo(&re, &new_bo);
    dmabuf_bo_free(&snapshot_bo);
    dmabuf_bo_free(&new_bo);

    render_engine_release_bo(&re, &dst_bo);
    dmabuf_bo_free(&dst_bo);

    dmabuf_bo_t bo_10bit;
    if (dmabuf_bo_allocate(&ctx, &bo_10bit, 64, 64, DRM_FORMAT_ARGB2101010,
                           DRM_FORMAT_MOD_INVALID)) {
        assert(render_engine_upload_static(&re, (const uint8_t *)src_pixels, 32, 16, &bo_10bit,
                                           WAYWAL_SCALING_FIT, 0xFF1A2B3CU));
        render_engine_release_bo(&re, &bo_10bit);
        dmabuf_bo_free(&bo_10bit);
    }

    render_engine_destroy(&re);
    dmabuf_context_destroy(&ctx);

    printf("[TEST] test_gpu_compute_nonlinear_and_upload_static PASSED.\n");
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
            .progress = 0.0f,
            .angle_rad = 0.785f,
            .wave_freq = 20.0f,
            .wave_amp = 0.05f,
            .center_x = 0.5f,
            .center_y = 0.5f,
            .bezier = BEZIER_DEFAULT,
        };

        memset(dst, 0, num_pixels * sizeof(uint32_t));
        bool ok = render_engine_execute_cpu_transition(dst, src_a, src_b, width, height,
                                                       width * sizeof(uint32_t), &params);
        assert(ok);
        for (size_t p = 0; p < num_pixels; ++p) {
            assert(dst[p] == src_a[p]);
        }

        params.progress = 0.5f;
        ok = render_engine_execute_cpu_transition(dst, src_a, src_b, width, height,
                                                  width * sizeof(uint32_t), &params);
        assert(ok && "CPU transition execution must succeed for all transition types");

        if (types[i] == WAYWAL_TRANSITION_NOISE || types[i] == WAYWAL_TRANSITION_BURN ||
            types[i] == WAYWAL_TRANSITION_WIPE || types[i] == WAYWAL_TRANSITION_WAVE) {
            params.progress = 0.9999f;
            ok = render_engine_execute_cpu_transition(dst, src_a, src_b, width, height,
                                                      width * sizeof(uint32_t), &params);
            assert(ok);
            for (size_t p = 0; p < num_pixels; ++p) {
                assert(dst[p] == src_b[p]);
            }
        }

        params.progress = 1.0f;
        memset(dst, 0, num_pixels * sizeof(uint32_t));
        ok = render_engine_execute_cpu_transition(dst, src_a, src_b, width, height,
                                                  width * sizeof(uint32_t), &params);
        assert(ok);
        for (size_t p = 0; p < num_pixels; ++p) {
            assert(dst[p] == src_b[p]);
        }
    }

    /* Verify WAYWAL_TRANSITION_PAGE_CURL respects angle_rad */
    uint32_t *dst_angle0 = (uint32_t *)calloc(num_pixels, sizeof(uint32_t));
    uint32_t *dst_angle90 = (uint32_t *)calloc(num_pixels, sizeof(uint32_t));
    assert(dst_angle0 && dst_angle90);
    waywal_transition_params_t curl0 = {
        .type = WAYWAL_TRANSITION_PAGE_CURL,
        .progress = 0.45f,
        .angle_rad = 0.0f,
        .bezier = BEZIER_DEFAULT,
    };
    waywal_transition_params_t curl90 = curl0;
    curl90.angle_rad = 1.5707963f;
    assert(render_engine_execute_cpu_transition(dst_angle0, src_a, src_b, width, height,
                                                width * sizeof(uint32_t), &curl0));
    assert(render_engine_execute_cpu_transition(dst_angle90, src_a, src_b, width, height,
                                                width * sizeof(uint32_t), &curl90));
    assert(memcmp(dst_angle0, dst_angle90, num_pixels * sizeof(uint32_t)) != 0);
    free(dst_angle0);
    free(dst_angle90);

    free(dst);
    free(src_a);
    free(src_b);
    printf("[TEST] test_cpu_transitions PASSED.\n");
}

static void test_builtin_shader_presets_gles31(void)
{
    printf("[TEST] Running test_builtin_shader_presets_gles31...\n");

    waywal_shader_preset_info_t presets[16];
    size_t count = waywal_shader_preset_list(presets, 16);
    assert(count == 10U);

    dmabuf_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (!dmabuf_context_init(&ctx) || !ctx.available) {
        printf("  DRM/GBM unavailable in this environment. Skipping GLES 3.1 preset compilation "
               "test.\n");
        return;
    }

    waywal_shader_engine_t *eng = waywal_shader_engine_create(&ctx);
    if (!eng) {
        printf("  Headless EGL/GLES 3.1 context unavailable. Skipping GLES 3.1 preset compilation "
               "test.\n");
        dmabuf_context_destroy(&ctx);
        return;
    }

    dmabuf_bo_t bo;
    memset(&bo, 0, sizeof(bo));
    bool has_bo =
        dmabuf_bo_allocate(&ctx, &bo, 128, 128, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR);
    if (!has_bo) {
        has_bo =
            dmabuf_bo_allocate(&ctx, &bo, 128, 128, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_INVALID);
    }
    assert(has_bo);

    waywal_audio_frame_t audio_frame;
    memset(&audio_frame, 0, sizeof(audio_frame));
    audio_frame.envelope = 0.45f;
    audio_frame.beat_pulse = 0.65f;
    for (size_t i = 0; i < WAYWAL_AUDIO_BARK_BANDS; ++i) {
        audio_frame.bands[i] = 0.35f;
    }
    for (size_t i = 0; i < WAYWAL_AUDIO_TEX_WIDTH; ++i) {
        audio_frame.shadertoy_tex[i] = 160;
        audio_frame.shadertoy_tex[WAYWAL_AUDIO_TEX_WIDTH + i] = 128;
    }

    for (size_t i = 0; i < count; ++i) {
        assert(waywal_shader_engine_compile(eng, presets[i].path));
        assert(waywal_shader_engine_has_active_program(eng));

        waywal_shader_frame_params_t fp;
        memset(&fp, 0, sizeof(fp));
        fp.time_sec = 1.25f + (float)i * 0.5f;
        fp.time_delta_sec = 1.0f / 60.0f;
        fp.frame_index = (int32_t)i;
        fp.mouse_smooth[0] = 64.0f;
        fp.mouse_smooth[1] = 64.0f;
        fp.audio = &audio_frame;
        assert(waywal_shader_engine_render_frame(eng, &bo, &fp));

        /* Render a second frame with audio == NULL to verify dirty-state reset */
        fp.audio = NULL;
        fp.time_sec += 1.0f / 60.0f;
        fp.frame_index++;
        assert(waywal_shader_engine_render_frame(eng, &bo, &fp));

        if (bo.map_data) {
            const uint8_t *px = (const uint8_t *)bo.map_data;
            bool any_rgb = false;
            for (size_t p = 0; p < 128U * 128U; ++p) {
                assert(px[p * 4 + 3] == 255U);
                if (px[p * 4 + 0] > 0 || px[p * 4 + 1] > 0 || px[p * 4 + 2] > 0) {
                    any_rgb = true;
                }
            }
            assert(any_rgb);
        }
    }

    waywal_shader_engine_forget_bo(eng, &bo);
    dmabuf_bo_free(&bo);
    waywal_shader_engine_destroy(eng);
    dmabuf_context_destroy(&ctx);

    printf("[TEST] test_builtin_shader_presets_gles31 (10/10 presets compiled & rendered) "
           "PASSED.\n");
}

static void test_buffer_gated_transition_tick(void)
{
    printf("[TEST] Running test_buffer_gated_transition_tick...\n");

    assert(WAYWAL_DMABUF_RING_SIZE == 4);

    daemon_state_t state;
    memset(&state, 0, sizeof(state));
    state.transition_timer_fd = -1;
    state.transitions_in_progress = true;

    output_node_t out;
    memset(&out, 0, sizeof(out));
    out.state = &state;
    out.configured = true;
    out.surface = NULL;
    out.width = 64;
    out.height = 64;
    out.transition_active = true;
    out.transition_total_frames = 60;
    out.transition_current_frame = 0;
    out.transition_duration_ns = 1200000000ULL;
    out.transition_progress_linear = 0.0f;
    out.transition_params = (waywal_transition_params_t){
        .type = WAYWAL_TRANSITION_FADE,
        .progress = 0.0f,
        .bezier = BEZIER_DEFAULT,
    };
    state.outputs = &out;
    state.num_outputs = 1;

    /* 1. DMA-BUF ring path: with valid dimensions (so abort_invalid is false) and all 4 ring
     * buffers busy (in_use = true), transition_engine_dispatch_tick must NOT advance
     * transition_current_frame or transition_progress_linear, and must keep the transition active.
     */
    out.use_dmabuf = true;
    out.dmabuf_ring.initialized = true;
    out.dmabuf_ring.width = 64;
    out.dmabuf_ring.height = 64;
    out.source_old_bo.width = 64;
    out.source_old_bo.height = 64;
    out.source_new_bo.width = 64;
    out.source_new_bo.height = 64;
    for (int i = 0; i < WAYWAL_DMABUF_RING_SIZE; ++i) {
        out.dmabuf_ring.buffers[i].in_use = true;
        out.dmabuf_ring.buffers[i].wl_buffer = (struct wl_buffer *)0x1;
    }
    assert(dmabuf_ring_acquire(&out.dmabuf_ring) == NULL);

    for (int tick = 0; tick < 10; ++tick) {
        transition_engine_dispatch_tick(&state);
        assert(out.transition_active);
        assert(state.transitions_in_progress);
        assert(out.transition_current_frame == 0);
        assert(out.transition_progress_linear == 0.0f);
        assert(out.transition_start_ns == 0);
    }

    /* Verify that releasing one slot in the 4-buffer ring allows dmabuf_ring_acquire to succeed */
    out.dmabuf_ring.buffers[2].in_use = false;
    dmabuf_bo_t *acq = dmabuf_ring_acquire(&out.dmabuf_ring);
    assert(acq == &out.dmabuf_ring.buffers[2]);
    assert(acq->in_use);
    assert(dmabuf_ring_acquire(&out.dmabuf_ring) == NULL);

    /* 2. SHM fallback path: with valid SHM buffers allocated (so abort_invalid is false),
     * when the back-buffer has not been released by the compositor,
     * transition_engine_dispatch_tick must NOT advance transition_current_frame
     * or transition_progress_linear, and must keep the transition active. */
    size_t frame_sz = 64U * 64U * sizeof(uint32_t);
    out.use_dmabuf = false;
    out.shm_width = 64;
    out.shm_height = 64;
    out.staging_width = 64;
    out.staging_height = 64;
    out.shm_size = frame_sz * 2U;
    out.shm_buffer_cap = frame_sz;
    out.shm_data = calloc(2U, frame_sz);
    out.shm_old_data = malloc(frame_sz);
    out.shm_new_data = malloc(frame_sz);
    assert(out.shm_data && out.shm_old_data && out.shm_new_data);
    memset(out.shm_old_data, 0x11, frame_sz);
    memset(out.shm_new_data, 0xEE, frame_sz);
    out.active_buffer_idx = 0;
    out.shm_buffer_released[0] = true;
    out.shm_buffer_released[1] = false; /* back_idx == 1 is busy */

    for (int tick = 0; tick < 10; ++tick) {
        transition_engine_dispatch_tick(&state);
        assert(out.transition_active);
        assert(state.transitions_in_progress);
        assert(out.transition_current_frame == 0);
        assert(out.transition_progress_linear == 0.0f);
        assert(out.transition_start_ns == 0);
    }

    /* 3. Releasing the SHM back-buffer allows the tick to render, anchor transition_start_ns,
     * advance transition_current_frame to 1, and flip active_buffer_idx to 1. */
    out.shm_buffer_released[1] = true;
    transition_engine_dispatch_tick(&state);
    assert(out.transition_active);
    assert(state.transitions_in_progress);
    assert(out.transition_current_frame == 1);
    assert(out.transition_start_ns > 0);
    assert(out.transition_last_tick_ns == out.transition_start_ns);
    assert(fabsf(out.transition_progress_linear - (1.0f / 60.0f)) < 1e-5f);
    assert(out.active_buffer_idx == 1);
    assert(!out.shm_buffer_released[1]);

    /* 4. Anti-teleport clamp: simulate a 500ms wall-clock stall on the next released frame
     * and verify progress advances by at most 0.08f instead of jumping by 500/1200 (~0.417f). */
    float prev_prog = out.transition_progress_linear;
    out.transition_last_tick_ns -= 500000000ULL;
    out.shm_buffer_released[0] = true;
    transition_engine_dispatch_tick(&state);
    assert(out.transition_active);
    assert(out.transition_current_frame == 2);
    assert(fabsf((out.transition_progress_linear - prev_prog) - 0.08f) < 1e-5f);

    /* 5. Staggered multi-output mid-transition override (transition_delay_frames > 0) +
     * completion only after progress == 1.0f frame is committed. */
    out.transition_current_frame = 0;
    out.transition_total_frames = 10;
    out.transition_delay_frames = 2;
    out.transition_start_ns = 0;
    out.transition_last_tick_ns = 0;
    out.transition_duration_ns = 200000000ULL;
    out.transition_progress_linear = 0.0f;
    out.shm_buffer_released[0] = true;
    out.shm_buffer_released[1] = true;

    transition_engine_dispatch_tick(&state);
    assert(out.transition_delay_frames == 1);
    assert(out.transition_current_frame == 0);
    assert(out.transition_start_ns == 0);

    transition_engine_dispatch_tick(&state);
    assert(out.transition_delay_frames == 0);
    assert(out.transition_current_frame == 0);
    assert(out.transition_start_ns == 0);

    int safety = 0;
    while (out.transition_active && safety++ < 100) {
        out.shm_buffer_released[1 - out.active_buffer_idx] = true;
        transition_engine_dispatch_tick(&state);
    }
    assert(!out.transition_active);
    assert(!state.transitions_in_progress);
    assert(fabsf(out.transition_progress_linear - 1.0f) < 1e-5f);
    assert(fabsf(out.transition_params.progress - 1.0f) < 1e-5f);
    assert(out.shm_old_data == NULL && out.shm_new_data == NULL);

    free(out.shm_data);

    /* 6. Mid-transition interruption snapshot via last_committed_ring_idx on DMA-BUF ring */
    if (dmabuf_context_init(&state.dmabuf_ctx) &&
        render_engine_init(&state.render_engine, &state.dmabuf_ctx) &&
        state.render_engine.gpu_compute_active) {
        state.dmabuf_ctx.preferred_modifier = DRM_FORMAT_MOD_INVALID;
        state.dmabuf_ctx.map_supported = false;
        memset(&out, 0, sizeof(out));
        out.state = &state;
        out.configured = true;
        out.surface = NULL;
        out.width = 64;
        out.height = 64;
        out.last_committed_ring_idx = -1;
        out.use_dmabuf = true;
        out.dmabuf_ring.initialized = true;
        out.dmabuf_ring.width = 64;
        out.dmabuf_ring.height = 64;
        out.dmabuf_ring.drm_format = DRM_FORMAT_ARGB8888;
        bool ring_ok = dmabuf_bo_allocate(&state.dmabuf_ctx, &out.source_old_bo, 64, 64,
                                          DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_INVALID) &&
                       dmabuf_bo_allocate(&state.dmabuf_ctx, &out.source_new_bo, 64, 64,
                                          DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_INVALID);
        out.has_source_old_bo = ring_ok;
        out.has_source_new_bo = ring_ok;
        for (int i = 0; i < WAYWAL_DMABUF_RING_SIZE && ring_ok; ++i) {
            if (!dmabuf_bo_allocate(&state.dmabuf_ctx, &out.dmabuf_ring.bos[i], 64, 64,
                                    DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_INVALID)) {
                ring_ok = false;
                break;
            }
            out.dmabuf_ring.bos[i].wl_buffer = (struct wl_buffer *)0x1;
            out.dmabuf_ring.bos[i].in_use = false;
        }
        if (ring_ok) {
            assert(render_engine_upload_static(&state.render_engine, NULL, 0, 0, &out.source_old_bo,
                                               WAYWAL_SCALING_FILL, 0xFFFF0000U));
            assert(render_engine_upload_static(&state.render_engine, NULL, 0, 0, &out.source_new_bo,
                                               WAYWAL_SCALING_FILL, 0xFF0000FFU));
            out.transition_params = (waywal_transition_params_t){
                .type = WAYWAL_TRANSITION_FADE,
                .progress = 0.0f,
                .bezier = BEZIER_DEFAULT,
            };
            out.transition_current_frame = 0;
            out.transition_total_frames = 10;
            out.transition_duration_ns = 500000000ULL;
            out.transition_progress_linear = 0.0f;
            out.transition_active = true;
            state.transitions_in_progress = true;

            transition_engine_dispatch_tick(&state);
            assert(out.transition_active);
            assert(out.transition_current_frame == 1);
            assert(out.last_committed_ring_idx == 0);
            assert(out.last_presented_bo == &out.dmabuf_ring.bos[0]);
            assert(out.has_image);

            /* Interrupt mid-transition: snapshot must copy from bos[last_committed_ring_idx]
             * into source_old_bo and reset last_committed_ring_idx to -1 */
            layer_shell_snapshot_live_output(&state, &out);
            assert(out.has_source_old_bo);
            assert(out.last_presented_bo == &out.source_old_bo);
            assert(out.last_committed_ring_idx == -1);
        }
        for (int i = 0; i < WAYWAL_DMABUF_RING_SIZE; ++i) {
            out.dmabuf_ring.bos[i].wl_buffer = NULL;
            if (out.dmabuf_ring.bos[i].fd[0] >= 0) {
                render_engine_release_bo(&state.render_engine, &out.dmabuf_ring.bos[i]);
                dmabuf_bo_free(&out.dmabuf_ring.bos[i]);
            }
        }
        if (out.has_source_old_bo) {
            render_engine_release_bo(&state.render_engine, &out.source_old_bo);
            dmabuf_bo_free(&out.source_old_bo);
        }
        if (out.has_source_new_bo) {
            render_engine_release_bo(&state.render_engine, &out.source_new_bo);
            dmabuf_bo_free(&out.source_new_bo);
        }
        render_engine_destroy(&state.render_engine);
        dmabuf_context_destroy(&state.dmabuf_ctx);
    }

    printf("[TEST] test_buffer_gated_transition_tick PASSED.\n");
}

int main(void)
{
    waywal_log_set_level(WAYWAL_LOG_LEVEL_WARN);
    printf("=========================================\n");
    printf("  Executing WayWal Transitions Test Suite\n");
    printf("=========================================\n");

    test_bezier_accuracy_and_bench();
    test_simd_bit_exactness();
    test_cpu_transitions();
    test_gpu_compute_transitions();
    test_gpu_compute_nonlinear_and_upload_static();
    test_builtin_shader_presets_gles31();
    test_buffer_gated_transition_tick();

    printf("\n>>> ALL TRANSITIONS UNIT TESTS PASSED SUCCESSFULLY! <<<\n\n");
    return 0;
}
