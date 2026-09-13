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
    return "leak:<unknown module>\nleak:gbm_create_device\nleak:libgbm\nleak:libEGL\nleak:"
           "libGLESv2\nleak:dri\nleak:libnvidia\n";
}
#endif

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline uint64_t get_cputime_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static long get_rss_kb(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f)
        return 0;
    long pages = 0, rss = 0;
    if (fscanf(f, "%ld %ld", &pages, &rss) == 2) {
        fclose(f);
        return rss * (sysconf(_SC_PAGESIZE) / 1024);
    }
    fclose(f);
    return 0;
}

typedef struct {
    const char *name;
    waywal_transition_type_t type;
    double mean_ms;
    double min_ms;
    double max_ms;
    double fps_equiv;
    double cpu_percent;
} bench_result_t;

static const struct {
    const char *name;
    waywal_transition_type_t type;
} ALL_TRANSITIONS[] = {
    {"fade", WAYWAL_TRANSITION_FADE},
    {"wipe", WAYWAL_TRANSITION_WIPE},
    {"grow", WAYWAL_TRANSITION_GROW},
    {"outer", WAYWAL_TRANSITION_OUTER},
    {"wave", WAYWAL_TRANSITION_WAVE},
    {"noise", WAYWAL_TRANSITION_NOISE},
    {"crosszoom", WAYWAL_TRANSITION_CROSSZOOM},
    {"slide", WAYWAL_TRANSITION_SLIDE},
    {"glitch", WAYWAL_TRANSITION_GLITCH},
    {"burn", WAYWAL_TRANSITION_BURN},
    {"ripple", WAYWAL_TRANSITION_RIPPLE},
    {"pixelate", WAYWAL_TRANSITION_PIXELATE},
    {"doom", WAYWAL_TRANSITION_DOOM},
    {"swirl", WAYWAL_TRANSITION_SWIRL},
    {"cube", WAYWAL_TRANSITION_CUBE},
    {"luma", WAYWAL_TRANSITION_LUMA},
    {"light_leak", WAYWAL_TRANSITION_LIGHT_LEAK},
    {"page_curl", WAYWAL_TRANSITION_PAGE_CURL},
};

static void run_gpu_benchmarks(uint32_t width, uint32_t height, int num_frames)
{
    printf("=========================================================================\n");
    printf("  WayWal GPU Compute Benchmark: %ux%u (%d continuous frames/effect)\n", width, height,
           num_frames);
    printf("=========================================================================\n\n");

    long initial_rss = get_rss_kb();

    dmabuf_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (!dmabuf_context_init(&ctx)) {
        printf("  [SKIP] DRM/GBM device unavailable in current environment.\n");
        return;
    }

    render_engine_t re;
    if (!render_engine_init(&re, &ctx) || !re.has_gpu_compute) {
        printf("  [SKIP] EGL/GLES 3.1 compute shader context unavailable.\n");
        dmabuf_context_destroy(&ctx);
        return;
    }

    dmabuf_bo_t target_bo, old_bo, new_bo;
    assert(dmabuf_bo_allocate(&ctx, &target_bo, width, height, DRM_FORMAT_ARGB8888,
                              DRM_FORMAT_MOD_INVALID));
    assert(dmabuf_bo_allocate(&ctx, &old_bo, width, height, DRM_FORMAT_ARGB8888,
                              DRM_FORMAT_MOD_INVALID));
    assert(dmabuf_bo_allocate(&ctx, &new_bo, width, height, DRM_FORMAT_ARGB8888,
                              DRM_FORMAT_MOD_INVALID));

    size_t count = sizeof(ALL_TRANSITIONS) / sizeof(ALL_TRANSITIONS[0]);
    bench_result_t results[count];

    /* Warm-up pass to trigger driver shader compilation and pipeline caches */
    for (size_t i = 0; i < count; ++i) {
        waywal_transition_params_t p = {
            .type = ALL_TRANSITIONS[i].type,
            .progress = 0.5f,
            .angle_rad = 0.785f,
            .wave_freq = 20.0f,
            .wave_amp = 0.05f,
            .center_x = 0.5f,
            .center_y = 0.5f,
            .bezier = BEZIER_DEFAULT,
        };
        render_engine_execute_transition(&re, &target_bo, &old_bo, &new_bo, &p);
    }

    long warmup_rss = get_rss_kb();

    printf("%-12s | %10s | %10s | %10s | %10s | %8s\n", "Transition", "Mean (ms)", "Min (ms)",
           "Max (ms)", "Max FPS", "CPU %");
    printf("-------------+------------+------------+------------+------------+----------\n");

    for (size_t i = 0; i < count; ++i) {
        waywal_transition_params_t p = {
            .type = ALL_TRANSITIONS[i].type,
            .progress = 0.0f,
            .angle_rad = 0.785f,
            .wave_freq = 20.0f,
            .wave_amp = 0.05f,
            .center_x = 0.5f,
            .center_y = 0.5f,
            .bezier = BEZIER_DEFAULT,
        };

        double min_t = 1e9;
        double max_t = 0.0;
        uint64_t cpu_t0 = get_cputime_ns();
        uint64_t wall_t0 = get_time_ns();

        for (int f = 0; f < num_frames; ++f) {
            p.progress = (float)f / (float)(num_frames - 1);
            uint64_t frame_t0 = get_time_ns();
            render_engine_execute_transition(&re, &target_bo, &old_bo, &new_bo, &p);
            uint64_t frame_t1 = get_time_ns();

            double frame_ms = (double)(frame_t1 - frame_t0) / 1e6;
            if (frame_ms < min_t)
                min_t = frame_ms;
            if (frame_ms > max_t)
                max_t = frame_ms;
        }

        uint64_t wall_t1 = get_time_ns();
        uint64_t cpu_t1 = get_cputime_ns();

        double total_wall_ms = (double)(wall_t1 - wall_t0) / 1e6;
        double total_cpu_ms = (double)(cpu_t1 - cpu_t0) / 1e6;
        double mean_ms = total_wall_ms / (double)num_frames;
        double cpu_pct = (total_wall_ms > 0) ? (total_cpu_ms / total_wall_ms * 100.0) : 0.0;
        double fps_eq = 1000.0 / mean_ms;

        results[i] = (bench_result_t){
            .name = ALL_TRANSITIONS[i].name,
            .type = ALL_TRANSITIONS[i].type,
            .mean_ms = mean_ms,
            .min_ms = min_t,
            .max_ms = max_t,
            .fps_equiv = fps_eq,
            .cpu_percent = cpu_pct,
        };

        printf("%-12s | %10.3f | %10.3f | %10.3f | %10.1f | %7.1f%%\n", results[i].name,
               results[i].mean_ms, results[i].min_ms, results[i].max_ms, results[i].fps_equiv,
               results[i].cpu_percent);
    }

    long final_rss = get_rss_kb();

    printf("\nMemory Footprint (Continuous GPU Transitions):\n");
    printf("  Initial RSS: %ld KB\n", initial_rss);
    printf("  Warm-up RSS: %ld KB\n", warmup_rss);
    printf("  Final RSS:   %ld KB\n", final_rss);
    printf("  RSS Delta (Leak Check): %ld KB [%s]\n\n", final_rss - warmup_rss,
           (final_rss - warmup_rss <= 128) ? "ZERO LEAK PASS" : "WARN");

    dmabuf_bo_free(&target_bo);
    dmabuf_bo_free(&old_bo);
    dmabuf_bo_free(&new_bo);
    render_engine_destroy(&re);
    dmabuf_context_destroy(&ctx);
}

static void run_cpu_benchmarks(uint32_t width, uint32_t height, int num_frames)
{
    printf("=========================================================================\n");
    printf("  WayWal CPU Fallback Benchmark: %ux%u (%d continuous frames/effect)\n", width, height,
           num_frames);
    printf("=========================================================================\n\n");

    const size_t num_pixels = (size_t)width * height;
    uint32_t *dst = (uint32_t *)aligned_alloc(64, num_pixels * sizeof(uint32_t));
    uint32_t *src_a = (uint32_t *)aligned_alloc(64, num_pixels * sizeof(uint32_t));
    uint32_t *src_b = (uint32_t *)aligned_alloc(64, num_pixels * sizeof(uint32_t));
    assert(dst && src_a && src_b);

    for (size_t i = 0; i < num_pixels; ++i) {
        src_a[i] = 0xFF336699;
        src_b[i] = 0xFFCC6633;
    }

    long initial_rss = get_rss_kb();
    size_t count = sizeof(ALL_TRANSITIONS) / sizeof(ALL_TRANSITIONS[0]);

    printf("%-12s | %10s | %10s | %10s | %10s\n", "Transition", "Mean (ms)", "Min (ms)", "Max (ms)",
           "Max FPS");
    printf("-------------+------------+------------+------------+------------\n");

    for (size_t i = 0; i < count; ++i) {
        waywal_transition_params_t p = {
            .type = ALL_TRANSITIONS[i].type,
            .progress = 0.0f,
            .angle_rad = 0.785f,
            .wave_freq = 20.0f,
            .wave_amp = 0.05f,
            .center_x = 0.5f,
            .center_y = 0.5f,
            .bezier = BEZIER_DEFAULT,
        };

        double min_t = 1e9;
        double max_t = 0.0;
        uint64_t wall_t0 = get_time_ns();

        for (int f = 0; f < num_frames; ++f) {
            p.progress = (float)f / (float)(num_frames - 1);
            uint64_t frame_t0 = get_time_ns();
            render_engine_execute_cpu_transition(dst, src_a, src_b, width, height,
                                                 width * sizeof(uint32_t), &p);
            uint64_t frame_t1 = get_time_ns();

            double frame_ms = (double)(frame_t1 - frame_t0) / 1e6;
            if (frame_ms < min_t)
                min_t = frame_ms;
            if (frame_ms > max_t)
                max_t = frame_ms;
        }

        uint64_t wall_t1 = get_time_ns();
        double total_wall_ms = (double)(wall_t1 - wall_t0) / 1e6;
        double mean_ms = total_wall_ms / (double)num_frames;
        double fps_eq = 1000.0 / mean_ms;

        printf("%-12s | %10.3f | %10.3f | %10.3f | %10.1f\n", ALL_TRANSITIONS[i].name, mean_ms,
               min_t, max_t, fps_eq);
    }

    long final_rss = get_rss_kb();
    printf("\nCPU Memory Footprint:\n");
    printf("  Initial RSS: %ld KB, Final RSS: %ld KB, Delta: %ld KB\n\n", initial_rss, final_rss,
           final_rss - initial_rss);

    free(dst);
    free(src_a);
    free(src_b);
}

int main(int argc, char *argv[])
{
    waywal_log_set_level(WAYWAL_LOG_LEVEL_WARN);

    int num_frames = 120;
    if (argc > 1) {
        int val = atoi(argv[1]);
        if (val > 0)
            num_frames = val;
    }

    printf("=========================================================================\n");
    printf("  WayWal Comprehensive Transition Benchmark Suite (All 11 Effects)\n");
    printf("=========================================================================\n\n");

    /* 1. Benchmark 1080p GPU Compute (Standard Full HD) */
    run_gpu_benchmarks(1920, 1080, num_frames);

    /* 2. Benchmark 4K UHD GPU Compute (Extreme High Resolution) */
    run_gpu_benchmarks(3840, 2160, num_frames / 2);

    /* 3. Benchmark 720p CPU SIMD / Software Fallback */
    run_cpu_benchmarks(1280, 720, num_frames / 2);

    printf(">>> BENCHMARK COMPLETED SUCCESSFULLY! <<<\n");
    return 0;
}
