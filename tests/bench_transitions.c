// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/bezier.h"
#include "waywal/dmabuf.h"
#include "waywal/image_loader.h"
#include "waywal/log.h"
#include "waywal/palette_hct.h"
#include "waywal/render_engine.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
__attribute__((visibility("default"))) const char *__lsan_default_suppressions(void);
const char *__lsan_default_suppressions(void)
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
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
        char line[256];
        long vmrss_kb = 0;
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "VmRSS:", 6) == 0) {
                if (sscanf(line + 6, "%ld", &vmrss_kb) == 1) {
                    fclose(f);
                    return vmrss_kb;
                }
            }
        }
        fclose(f);
    }
    f = fopen("/proc/self/statm", "r");
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

static long get_hwm_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        return get_rss_kb();
    char line[256];
    long vmhwm_kb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmHWM:", 6) == 0) {
            if (sscanf(line + 6, "%ld", &vmhwm_kb) == 1) {
                fclose(f);
                return vmhwm_kb;
            }
        }
    }
    fclose(f);
    return get_rss_kb();
}

static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

typedef struct {
    const char *name;
    waywal_transition_type_t type;
    double mean_ms;
    double p50_ms;
    double p99_ms;
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
    if (num_frames <= 0)
        num_frames = 1;

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
    if (!render_engine_init(&re, &ctx)) {
        printf("  [SKIP] EGL/GLES 3.1 compute shader context unavailable.\n");
        dmabuf_context_destroy(&ctx);
        return;
    }
    if (!re.has_gpu_compute) {
        printf("  [SKIP] EGL/GLES 3.1 compute shader context unavailable.\n");
        render_engine_destroy(&re);
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
    double *frame_samples = (double *)malloc((size_t)num_frames * sizeof(double));
    assert(frame_samples != NULL);

    /* Warm-up pass to trigger driver shader compilation, pipeline caches, and command ring steady-state */
    for (int wf = 0; wf < 4; ++wf) {
        float wprog = (float)wf / 3.0f;
        for (size_t i = 0; i < count; ++i) {
            waywal_transition_params_t p = {
                .type = ALL_TRANSITIONS[i].type,
                .progress = wprog,
                .angle_rad = 0.785f,
                .wave_freq = 20.0f,
                .wave_amp = 0.05f,
                .center_x = 0.5f,
                .center_y = 0.5f,
                .bezier = BEZIER_DEFAULT,
            };
            render_engine_execute_transition(&re, &target_bo, &old_bo, &new_bo, &p);
        }
    }

    long warmup_rss = get_rss_kb();

    printf("%-12s | %10s | %10s | %10s | %10s | %10s | %10s | %8s\n", "Transition", "Mean (ms)",
           "P50 (ms)", "P99 (ms)", "Min (ms)", "Max (ms)", "Max FPS", "CPU %");
    printf(
        "-------------+------------+------------+------------+------------+------------+------------+----------\n");

    size_t p50_idx = (size_t)(num_frames / 2);
    size_t p99_idx = (size_t)((num_frames * 99) / 100);
    if (p99_idx >= (size_t)num_frames)
        p99_idx = (size_t)num_frames - 1;

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
            p.progress = (num_frames > 1) ? ((float)f / (float)(num_frames - 1)) : 0.5f;
            uint64_t frame_t0 = get_time_ns();
            render_engine_execute_transition(&re, &target_bo, &old_bo, &new_bo, &p);
            uint64_t frame_t1 = get_time_ns();

            double frame_ms = (double)(frame_t1 - frame_t0) / 1e6;
            frame_samples[f] = frame_ms;
            if (frame_ms < min_t)
                min_t = frame_ms;
            if (frame_ms > max_t)
                max_t = frame_ms;
        }

        uint64_t wall_t1 = get_time_ns();
        uint64_t cpu_t1 = get_cputime_ns();

        qsort(frame_samples, (size_t)num_frames, sizeof(double), cmp_double);

        double total_wall_ms = (double)(wall_t1 - wall_t0) / 1e6;
        double total_cpu_ms = (double)(cpu_t1 - cpu_t0) / 1e6;
        double mean_ms = total_wall_ms / (double)num_frames;
        double cpu_pct = (total_wall_ms > 0) ? (total_cpu_ms / total_wall_ms * 100.0) : 0.0;
        double fps_eq = 1000.0 / mean_ms;

        results[i] = (bench_result_t){
            .name = ALL_TRANSITIONS[i].name,
            .type = ALL_TRANSITIONS[i].type,
            .mean_ms = mean_ms,
            .p50_ms = frame_samples[p50_idx],
            .p99_ms = frame_samples[p99_idx],
            .min_ms = min_t,
            .max_ms = max_t,
            .fps_equiv = fps_eq,
            .cpu_percent = cpu_pct,
        };

        printf("%-12s | %10.3f | %10.3f | %10.3f | %10.3f | %10.3f | %10.1f | %7.1f%%\n",
               results[i].name, results[i].mean_ms, results[i].p50_ms, results[i].p99_ms,
               results[i].min_ms, results[i].max_ms, results[i].fps_equiv, results[i].cpu_percent);
    }

    free(frame_samples);

    long final_rss = get_rss_kb();
    long hwm_rss = get_hwm_kb();

    printf("\nMemory Footprint (Continuous GPU Transitions):\n");
    printf("  Initial VmRSS: %ld KB\n", initial_rss);
    printf("  Warm-up VmRSS: %ld KB\n", warmup_rss);
    printf("  Final VmRSS:   %ld KB (VmHWM: %ld KB)\n", final_rss, hwm_rss);
    printf("  RSS Delta (Leak Check): %ld KB [%s]\n\n", final_rss - warmup_rss,
           (final_rss - warmup_rss <= 128) ? "ZERO LEAK PASS" : "WARN");

    render_engine_release_bo(&re, &target_bo);
    render_engine_release_bo(&re, &old_bo);
    render_engine_release_bo(&re, &new_bo);
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
    const size_t alloc_bytes = ((num_pixels * sizeof(uint32_t)) + 63U) & ~(size_t)63U;
    uint32_t *dst = (uint32_t *)aligned_alloc(64, alloc_bytes);
    uint32_t *src_a = (uint32_t *)aligned_alloc(64, alloc_bytes);
    uint32_t *src_b = (uint32_t *)aligned_alloc(64, alloc_bytes);
    assert(dst && src_a && src_b);

    for (size_t i = 0; i < num_pixels; ++i) {
        src_a[i] = 0xFF336699U;
        src_b[i] = 0xFFCC6633U;
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
            p.progress = (num_frames > 1) ? ((float)f / (float)(num_frames - 1)) : 0.5f;
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
    printf("  Initial VmRSS: %ld KB, Final VmRSS: %ld KB, Delta: %ld KB\n\n", initial_rss,
           final_rss, final_rss - initial_rss);

    free(dst);
    free(src_a);
    free(src_b);
}

static void run_palette_and_template_benchmarks(int num_iters)
{
    if (num_iters <= 0)
        num_iters = 1000;

    printf("=========================================================================\n");
    printf("  WayWal Dual-Mode OKLab/HCT Palette & 7-Filter Template Benchmark (%d iters)\n",
           num_iters);
    printf("=========================================================================\n\n");

    const uint32_t width = 3840;
    const uint32_t height = 2160;
    const size_t num_pixels = (size_t)width * height;
    const size_t alloc_bytes = ((num_pixels * sizeof(uint32_t)) + 63U) & ~(size_t)63U;
    uint32_t *pixels_4k = (uint32_t *)aligned_alloc(64, alloc_bytes);
    assert(pixels_4k != NULL);

    /* Deterministic multi-hue 4K gradient to exercise all 16 K-Means++ clusters & 4 seed ranks */
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t g = (uint8_t)((y * 255U) / height);
        uint32_t *row = pixels_4k + (size_t)y * width;
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t r = (uint8_t)((x * 255U) / width);
            uint8_t b = (uint8_t)(((x ^ y) * 197U) & 0xFFU);
            row[x] = (0xFFU << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
        }
    }

    char tpl_in[256];
    char tpl_out[256];
    snprintf(tpl_in, sizeof(tpl_in), "/tmp/wwal_bench_tpl_%d.in", (int)getpid());
    snprintf(tpl_out, sizeof(tpl_out), "/tmp/wwal_bench_tpl_%d.out", (int)getpid());

    static const char BENCH_TEMPLATE_SRC[] =
        "/* Matugen dual-mode role & tonal tokens */\n"
        "primary_default = {{colors.primary.default.hex}}\n"
        "surface_dark_rgb = {{colors.surface.dark.rgb}}\n"
        "primary_light_rgba = {{colors.primary.light.rgba}}\n"
        "tonal_primary_40 = {{palettes.primary.40.hex}}\n"
        "source_color = {{colors.source_color.default.hex}}\n"
        "primary_oklch = {{colors.primary.default.oklch}}\n"
        "/* All 7 pipe filters: lighten, darken, saturate, alpha, blend, invert, complementary */\n"
        "f_chain_1 = {{ (colors.primary.default | lighten(0.15) | saturate(0.10) | alpha(0.85)).rgba }}\n"
        "f_blend   = {{ colors.secondary.dark | blend(colors.tertiary.dark, 0.35) | hex }}\n"
        "f_comp    = {{ colors.error.default | complementary | strip }}\n"
        "f_inv     = {{ colors.surface.default | darken(0.10) | invert | rgb_css }}\n"
        "f_all7    = {{ colors.tertiary.light | lighten(10) | darken(5) | saturate(0.20) | blend(colors.primary.dark, 0.25) | invert | complementary | alpha(0.75) | rgba }}\n"
        "/* Pywal single-brace compatibility tokens */\n"
        "bg = {background}\n"
        "fg = {foreground}\n"
        "cursor = {cursor}\n"
        "c0 = #{color0.strip}\n"
        "c1 = rgb({color1.rgb})\n"
        "c2 = {color2.xrgba}\n"
        "c3 = {color3.octal}\n"
        "c4 = {color4}\n"
        "c5 = {color5}\n"
        "c6 = {color6}\n"
        "c7 = {color7}\n"
        "c8 = {color8}\n"
        "c9 = {color9}\n"
        "c10 = {color10}\n"
        "c11 = {color11}\n"
        "c12 = {color12}\n"
        "c13 = {color13}\n"
        "c14 = {color14}\n"
        "c15 = rgb({color15.rgb})\n";

    FILE *tf = fopen(tpl_in, "wb");
    assert(tf != NULL);
    assert(fwrite(BENCH_TEMPLATE_SRC, 1, sizeof(BENCH_TEMPLATE_SRC) - 1, tf) ==
           sizeof(BENCH_TEMPLATE_SRC) - 1);
    fclose(tf);

    double *samples_extract = (double *)malloc((size_t)num_iters * sizeof(double));
    double *samples_dual = (double *)malloc((size_t)num_iters * sizeof(double));
    double *samples_tpl = (double *)malloc((size_t)num_iters * sizeof(double));
    double *samples_total = (double *)malloc((size_t)num_iters * sizeof(double));
    assert(samples_extract && samples_dual && samples_tpl && samples_total);

    waywal_palette_t dark_pal, light_pal;
    memset(&dark_pal, 0, sizeof(dark_pal));
    memset(&light_pal, 0, sizeof(light_pal));

    /* Warm up LUTs and file cache (5 iterations) */
    for (int w = 0; w < 5; ++w) {
        assert(waywal_palette_extract((const uint8_t *)pixels_4k, width, height, width * 4U,
                                      WAYWAL_THEME_DARK, &dark_pal));
        waywal_palette_derive_opposite(&dark_pal, &light_pal);
        assert(waywal_palette_render_template_file(&dark_pal, &dark_pal, &light_pal, tpl_in,
                                                   tpl_out));
    }

    double sum_extract = 0.0, sum_dual = 0.0, sum_tpl = 0.0, sum_total = 0.0;

    for (int i = 0; i < num_iters; ++i) {
        uint64_t t0 = get_time_ns();
        bool ok_ext = waywal_palette_extract((const uint8_t *)pixels_4k, width, height, width * 4U,
                                             WAYWAL_THEME_DARK, &dark_pal);
        waywal_palette_derive_opposite(&dark_pal, &light_pal);
        uint64_t t1 = get_time_ns();
        bool ok_tpl =
            waywal_palette_render_template_file(&dark_pal, &dark_pal, &light_pal, tpl_in, tpl_out);
        uint64_t t2 = get_time_ns();
        assert(ok_ext && ok_tpl);

        uint64_t td0 = get_time_ns();
        bool ok_dual = waywal_palette_extract_dual_ex(
            (const uint8_t *)pixels_4k, width, height, width * 4U, true, WAYWAL_SCHEME_TONAL_SPOT,
            0.0f, (uint8_t)(i & 3), false, 0U, &dark_pal, &light_pal);
        uint64_t td1 = get_time_ns();
        assert(ok_dual);

        double ext_ms = (double)(t1 - t0) / 1e6;
        double tpl_ms = (double)(t2 - t1) / 1e6;
        double tot_ms = (double)(t2 - t0) / 1e6;
        double dual_ms = (double)(td1 - td0) / 1e6;

        samples_extract[i] = ext_ms;
        samples_tpl[i] = tpl_ms;
        samples_total[i] = tot_ms;
        samples_dual[i] = dual_ms;

        sum_extract += ext_ms;
        sum_tpl += tpl_ms;
        sum_total += tot_ms;
        sum_dual += dual_ms;
    }

    qsort(samples_extract, (size_t)num_iters, sizeof(double), cmp_double);
    qsort(samples_dual, (size_t)num_iters, sizeof(double), cmp_double);
    qsort(samples_tpl, (size_t)num_iters, sizeof(double), cmp_double);
    qsort(samples_total, (size_t)num_iters, sizeof(double), cmp_double);

    size_t p99_idx = (size_t)((num_iters * 99) / 100);
    if (p99_idx >= (size_t)num_iters)
        p99_idx = (size_t)num_iters - 1;

    printf("%-40s | %10s | %10s | %10s | %10s\n", "Stage / Operation", "Mean (ms)", "Min (ms)",
           "P99 (ms)", "Max (ms)");
    printf("-----------------------------------------+------------+------------+------------+------------\n");
    printf("%-40s | %10.3f | %10.3f | %10.3f | %10.3f\n",
           "extract + derive_opposite (4K Dark+Light)", sum_extract / num_iters, samples_extract[0],
           samples_extract[p99_idx], samples_extract[num_iters - 1]);
    printf("%-40s | %10.3f | %10.3f | %10.3f | %10.3f\n",
           "extract_dual_ex (4K Seed 0..3 Dark+Light)", sum_dual / num_iters, samples_dual[0],
           samples_dual[p99_idx], samples_dual[num_iters - 1]);
    printf("%-40s | %10.3f | %10.3f | %10.3f | %10.3f\n",
           "render_template (7 Filters + 32 Tokens)", sum_tpl / num_iters, samples_tpl[0],
           samples_tpl[p99_idx], samples_tpl[num_iters - 1]);
    printf("%-40s | %10.3f | %10.3f | %10.3f | %10.3f\n\n",
           "Total End-to-End (Extract+Derive+Render)", sum_total / num_iters, samples_total[0],
           samples_total[p99_idx], samples_total[num_iters - 1]);

    unlink(tpl_in);
    unlink(tpl_out);
    free(samples_extract);
    free(samples_dual);
    free(samples_tpl);
    free(samples_total);
    free(pixels_4k);
}

static void run_lockscreen_png_benchmarks(int num_iters)
{
    if (num_iters <= 0)
        num_iters = 20;

    printf("=========================================================================\n");
    printf("  WayWal Lockscreen PNG Frame Export Benchmark (%d iters)\n", num_iters);
    printf("=========================================================================\n\n");

    const size_t pixels_4k_count = 3840ULL * 2160ULL;
    const size_t pixels_720p_count = 1280ULL * 720ULL;
    uint32_t *bgra_4k = (uint32_t *)aligned_alloc(64, pixels_4k_count * sizeof(uint32_t));
    uint32_t *bgra_720p = (uint32_t *)aligned_alloc(64, pixels_720p_count * sizeof(uint32_t));
    assert(bgra_4k != NULL && bgra_720p != NULL);

    for (uint32_t y = 0; y < 2160U; ++y) {
        uint8_t g = (uint8_t)((y * 255U) / 2160U);
        uint32_t *row = bgra_4k + (size_t)y * 3840U;
        for (uint32_t x = 0; x < 3840U; ++x) {
            uint8_t r = (uint8_t)((x * 255U) / 3840U);
            uint8_t b = (uint8_t)((x + y) >> 5);
            row[x] = (0xFFU << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }

    for (uint32_t y = 0; y < 720U; ++y) {
        uint8_t g = (uint8_t)((y * 255U) / 720U);
        uint32_t *row = bgra_720p + (size_t)y * 1280U;
        for (uint32_t x = 0; x < 1280U; ++x) {
            uint8_t r = (uint8_t)((x * 255U) / 1280U);
            uint8_t b = (uint8_t)((x + y) >> 4);
            row[x] = (0xFFU << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }

    char out_720p[256];
    char out_4k[256];
    snprintf(out_720p, sizeof(out_720p), "/tmp/wwal_bench_lock_720p_%d.png", (int)getpid());
    snprintf(out_4k, sizeof(out_4k), "/tmp/wwal_bench_lock_4k_%d.png", (int)getpid());

    double *samples_720p = (double *)malloc((size_t)num_iters * sizeof(double));
    double *samples_4k = (double *)malloc((size_t)num_iters * sizeof(double));
    assert(samples_720p && samples_4k);

    double sum_720p = 0.0, sum_4k = 0.0;
    for (int i = 0; i < num_iters; ++i) {
        uint64_t t0 = get_time_ns();
        bool ok1 = image_save_png_bgra(out_720p, (const uint8_t *)bgra_720p, 1280, 720);
        uint64_t t1 = get_time_ns();
        bool ok2 = image_save_png_bgra(out_4k, (const uint8_t *)bgra_4k, 3840, 2160);
        uint64_t t2 = get_time_ns();
        assert(ok1 && ok2);

        samples_720p[i] = (double)(t1 - t0) / 1e6;
        samples_4k[i] = (double)(t2 - t1) / 1e6;
        sum_720p += samples_720p[i];
        sum_4k += samples_4k[i];
    }

    qsort(samples_720p, (size_t)num_iters, sizeof(double), cmp_double);
    qsort(samples_4k, (size_t)num_iters, sizeof(double), cmp_double);
    size_t p99_idx = (size_t)((num_iters * 99) / 100);
    if (p99_idx >= (size_t)num_iters)
        p99_idx = (size_t)num_iters - 1;

    printf("%-40s | %10s | %10s | %10s | %10s\n", "Lockscreen Export Mode", "Mean (ms)",
           "Min (ms)", "P99 (ms)", "Max (ms)");
    printf("-----------------------------------------+------------+------------+------------+------------\n");
    printf("%-40s | %10.3f | %10.3f | %10.3f | %10.3f\n",
           "Native 1280x720 BGRA -> PNG", sum_720p / num_iters, samples_720p[0],
           samples_720p[p99_idx], samples_720p[num_iters - 1]);
    printf("%-40s | %10.3f | %10.3f | %10.3f | %10.3f\n\n",
           "4K 3840x2160 -> 1280x720 Downscale + PNG", sum_4k / num_iters, samples_4k[0],
           samples_4k[p99_idx], samples_4k[num_iters - 1]);

    unlink(out_720p);
    unlink(out_4k);
    free(samples_720p);
    free(samples_4k);
    free(bgra_720p);
    free(bgra_4k);
}

int main(int argc, char *argv[])
{
    waywal_log_set_level(WAYWAL_LOG_LEVEL_WARN);

    bool palette_only = false;
    int num_frames = 120;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--palette") == 0) {
            palette_only = true;
        } else {
            int val = atoi(argv[i]);
            if (val > 0)
                num_frames = val;
        }
    }

    if (palette_only) {
        run_palette_and_template_benchmarks(1000);
        run_lockscreen_png_benchmarks(20);
        printf(">>> PALETTE & TEMPLATE BENCHMARK COMPLETED SUCCESSFULLY! <<<\n");
        return 0;
    }

    printf("=========================================================================\n");
    printf("  WayWal Comprehensive Transition Benchmark Suite (All 18 Effects)\n");
    printf("=========================================================================\n\n");

    /* 1. Benchmark 1080p GPU Compute (Standard Full HD) */
    run_gpu_benchmarks(1920, 1080, num_frames);

    /* 2. Benchmark 4K UHD GPU Compute (Extreme High Resolution) */
    run_gpu_benchmarks(3840, 2160, (num_frames / 2 > 0) ? (num_frames / 2) : 1);

    /* 3. Benchmark 720p & 4K CPU SIMD / Software Fallback */
    run_cpu_benchmarks(1280, 720, (num_frames / 2 > 0) ? (num_frames / 2) : 1);
    run_cpu_benchmarks(3840, 2160, (num_frames / 6 > 0) ? (num_frames / 6) : 10);

    /* 4. Benchmark Dual-Mode OKLab/HCT Palette & 7-Filter Template Engine (1,000 iters) */
    run_palette_and_template_benchmarks(1000);

    /* 5. Benchmark Lockscreen PNG Frame Export */
    run_lockscreen_png_benchmarks(20);

    printf(">>> BENCHMARK COMPLETED SUCCESSFULLY! <<<\n");
    return 0;
}
