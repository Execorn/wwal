// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef E2E_COMMON_H
#define E2E_COMMON_H

#include "../src/daemon/simd/simd_blend.h"
#include "waywal/bezier.h"
#include "waywal/dmabuf.h"
#include "waywal/log.h"
#include "waywal/render_engine.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
__attribute__((weak, visibility("default"))) const char *__lsan_default_suppressions(void);
const char *__lsan_default_suppressions(void)
{
    return "leak:<unknown module>\nleak:gbm_create_device\nleak:libgbm\nleak:libEGL\nleak:"
           "libGLESv2\nleak:dri\nleak:libnvidia\n";
}
#endif

#define E2E_CANARY_GUARD_WORDS 16
#define E2E_CANARY_PATTERN     0xDEADBEEFU

typedef struct {
    int total;
    int passed;
    int failed;
    const char *tier_name;
} e2e_test_stats_t;

extern e2e_test_stats_t g_stats;

#define E2E_CHECK(cond, name, fmt, ...)                                                            \
    do {                                                                                           \
        g_stats.total++;                                                                           \
        if (cond) {                                                                                \
            g_stats.passed++;                                                                      \
            printf("  [PASS] [%s] %s\n", g_stats.tier_name, (name));                               \
        } else {                                                                                   \
            g_stats.failed++;                                                                      \
            fprintf(stderr, "  [FAIL] [%s] %s: " fmt "\n", g_stats.tier_name,                      \
                    (name)__VA_OPT__(, ) __VA_ARGS__);                                             \
        }                                                                                          \
    } while (0)

typedef struct {
    const char *name;
    waywal_transition_type_t type;
    bool is_threshold;
} e2e_transition_desc_t;

static const e2e_transition_desc_t E2E_ALL_TRANSITIONS[18] = {
    {"fade", WAYWAL_TRANSITION_FADE, false},
    {"wipe", WAYWAL_TRANSITION_WIPE, true},
    {"grow", WAYWAL_TRANSITION_GROW, false},
    {"outer", WAYWAL_TRANSITION_OUTER, false},
    {"wave", WAYWAL_TRANSITION_WAVE, true},
    {"noise", WAYWAL_TRANSITION_NOISE, true},
    {"crosszoom", WAYWAL_TRANSITION_CROSSZOOM, false},
    {"slide", WAYWAL_TRANSITION_SLIDE, false},
    {"glitch", WAYWAL_TRANSITION_GLITCH, false},
    {"burn", WAYWAL_TRANSITION_BURN, true},
    {"ripple", WAYWAL_TRANSITION_RIPPLE, false},
    {"pixelate", WAYWAL_TRANSITION_PIXELATE, false},
    {"doom", WAYWAL_TRANSITION_DOOM, false},
    {"swirl", WAYWAL_TRANSITION_SWIRL, false},
    {"cube", WAYWAL_TRANSITION_CUBE, false},
    {"luma", WAYWAL_TRANSITION_LUMA, false},
    {"light_leak", WAYWAL_TRANSITION_LIGHT_LEAK, false},
    {"page_curl", WAYWAL_TRANSITION_PAGE_CURL, false},
};

static inline uint64_t e2e_get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline long e2e_get_rss_kb(void)
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

static inline uint32_t *e2e_canary_alloc(size_t num_pixels)
{
    size_t total_words = E2E_CANARY_GUARD_WORDS + num_pixels + E2E_CANARY_GUARD_WORDS;
    size_t byte_size = total_words * sizeof(uint32_t);
    size_t alloc_bytes = (byte_size + 63U) & ~(size_t)63U;
    uint32_t *raw = (uint32_t *)aligned_alloc(64, alloc_bytes);
    if (!raw)
        return NULL;
    for (size_t i = 0; i < E2E_CANARY_GUARD_WORDS; ++i) {
        raw[i] = E2E_CANARY_PATTERN;
        raw[E2E_CANARY_GUARD_WORDS + num_pixels + i] = E2E_CANARY_PATTERN;
    }
    return raw + E2E_CANARY_GUARD_WORDS;
}

static inline bool e2e_canary_check(const uint32_t *buf, size_t num_pixels)
{
    if (!buf)
        return false;
    const uint32_t *pre = buf - E2E_CANARY_GUARD_WORDS;
    const uint32_t *post = buf + num_pixels;
    for (size_t i = 0; i < E2E_CANARY_GUARD_WORDS; ++i) {
        if (pre[i] != E2E_CANARY_PATTERN)
            return false;
        if (post[i] != E2E_CANARY_PATTERN)
            return false;
    }
    return true;
}

static inline void e2e_canary_free(uint32_t *buf)
{
    if (!buf)
        return;
    uint32_t *raw = buf - E2E_CANARY_GUARD_WORDS;
    free(raw);
}

static inline void e2e_generate_pattern_a(uint32_t *buf, uint32_t width, uint32_t height,
                                          uint32_t stride_pixels)
{
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t r = (uint8_t)((x * 255) / (width > 1 ? width - 1 : 1));
            uint8_t g = (uint8_t)((y * 255) / (height > 1 ? height - 1 : 1));
            uint8_t b = (uint8_t)(((x + y) * 128) / ((width + height) > 1 ? (width + height) : 1));
            buf[y * stride_pixels + x] =
                0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

static inline void e2e_generate_pattern_b(uint32_t *buf, uint32_t width, uint32_t height,
                                          uint32_t stride_pixels)
{
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t r = (uint8_t)(255 - ((x * 255) / (width > 1 ? width - 1 : 1)));
            uint8_t g = (uint8_t)(255 - ((y * 255) / (height > 1 ? height - 1 : 1)));
            uint8_t b = (uint8_t)((x * 37 + y * 59) & 0xFF);
            buf[y * stride_pixels + x] =
                0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

static inline void e2e_generate_pattern_c(uint32_t *buf, uint32_t width, uint32_t height,
                                          uint32_t stride_pixels)
{
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t r = (uint8_t)((x * 17 + y * 23) & 0xFF);
            uint8_t g = (uint8_t)((x * 43) / (width > 1 ? width - 1 : 1) + 100);
            uint8_t b = (uint8_t)(255 - ((y * 71) / (height > 1 ? height - 1 : 1)));
            buf[y * stride_pixels + x] =
                0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

static inline size_t e2e_count_diff_pixels(const uint32_t *p1, const uint32_t *p2, uint32_t width,
                                           uint32_t height, uint32_t stride_pixels)
{
    size_t diff = 0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            if (p1[y * stride_pixels + x] != p2[y * stride_pixels + x]) {
                diff++;
            }
        }
    }
    return diff;
}

static inline int e2e_max_channel_diff(const uint32_t *p1, const uint32_t *p2, uint32_t width,
                                       uint32_t height, uint32_t stride_pixels)
{
    int max_diff = 0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t c1 = p1[y * stride_pixels + x];
            uint32_t c2 = p2[y * stride_pixels + x];
            int dr = abs((int)((c1 >> 16) & 0xFF) - (int)((c2 >> 16) & 0xFF));
            int dg = abs((int)((c1 >> 8) & 0xFF) - (int)((c2 >> 8) & 0xFF));
            int db = abs((int)(c1 & 0xFF) - (int)(c2 & 0xFF));
            if (dr > max_diff)
                max_diff = dr;
            if (dg > max_diff)
                max_diff = dg;
            if (db > max_diff)
                max_diff = db;
        }
    }
    return max_diff;
}

static inline bool e2e_is_valid_argb_buffer(const uint32_t *buf, uint32_t width, uint32_t height,
                                            uint32_t stride_pixels)
{
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t c = buf[y * stride_pixels + x];
            if ((c & 0xFF000000) != 0xFF000000) {
                return false;
            }
        }
    }
    return true;
}

/* Tier execution entry points */
typedef struct {
    uint32_t width;
    uint32_t height;
    waywal_transition_type_t effect;
    float progress;
    bool success;
} thread_test_arg_t;

void *e2e_thread_worker(void *arg);

void run_tier1_feature_coverage(void);
void run_tier2_boundary_corner(void);
void run_tier3_cross_feature(void);
void run_tier4_workload_scenarios(void);

#endif /* E2E_COMMON_H */
