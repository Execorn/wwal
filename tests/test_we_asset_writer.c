// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "waywal/image_loader.h"
#include "waywal/we_asset_writer.h"
#include "waywal/we_json.h"
#include "waywal/we_mdl_writer.h"
#include "waywal/we_particle.h"
#include "waywal/we_pkg.h"
#include "waywal/we_project.h"
#include "waywal/we_puppet.h"
#include "waywal/we_scene.h"
#include "waywal/we_script.h"
#include "waywal/we_shader.h"
#include "waywal/we_tex.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
__attribute__((visibility("default"))) const char *__lsan_default_suppressions(void);
const char *__lsan_default_suppressions(void)
{
    return "leak:<unknown module>\nleak:gbm_create_device\nleak:libgbm\nleak:libEGL\n"
           "leak:libGLESv2\nleak:dri\nleak:libnvidia\n";
}
#endif

static int g_pass = 0;
static int g_fail = 0;

#define TEST_CHECK(cond)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
            ++g_fail;                                                                              \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define TEST_CHECK_GOTO(cond, label)                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
            ++g_fail;                                                                              \
            goto label;                                                                            \
        }                                                                                          \
    } while (0)

#define RUN_TEST(fn)                                                                               \
    do {                                                                                           \
        int before = g_fail;                                                                       \
        fn();                                                                                      \
        if (g_fail == before) {                                                                    \
            ++g_pass;                                                                              \
            printf("PASS %s\n", #fn);                                                              \
        }                                                                                          \
    } while (0)

static double monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        return;
    }
    if (!S_ISDIR(st.st_mode)) {
        unlink(path);
        return;
    }
    DIR *d = opendir(path);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            char child[4096];
            if (snprintf(child, sizeof(child), "%s/%s", path, ent->d_name) < (int)sizeof(child)) {
                rm_rf(child);
            }
        }
        closedir(d);
    }
    rmdir(path);
}

static void make_tmp_dir(char *out, size_t cap, const char *tag)
{
    snprintf(out, cap, "/tmp/waywal_aw_%s_%ld", tag, (long)getpid());
    rm_rf(out);
    mkdir(out, 0755);
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void test_lz4_bounds_and_invalid(void)
{
    TEST_CHECK(we_lz4_compress_bound(0u) == 0u);
    TEST_CHECK(we_lz4_compress_bound(1u) == 17u);
    TEST_CHECK(we_lz4_compress_bound(WE_LZ4_MAX_INPUT_SIZE) > WE_LZ4_MAX_INPUT_SIZE);
    TEST_CHECK(we_lz4_compress_bound((size_t)WE_LZ4_MAX_INPUT_SIZE + 1u) == 0u);

    uint8_t src[64] = {0};
    uint8_t dst[128] = {0};
    TEST_CHECK(we_lz4_compress_block(NULL, 16u, dst, sizeof(dst)) == 0u);
    TEST_CHECK(we_lz4_compress_block(src, 0u, dst, sizeof(dst)) == 0u);
    TEST_CHECK(we_lz4_compress_block(src, 16u, NULL, sizeof(dst)) == 0u);
    TEST_CHECK(we_lz4_compress_block(src, 16u, dst, 0u) == 0u);
    TEST_CHECK(we_lz4_compress_block(src, (size_t)WE_LZ4_MAX_INPUT_SIZE + 1u, dst, sizeof(dst)) ==
               0u);
}

static void test_lz4_roundtrip_patterns(void)
{
    static const size_t sizes[] = {1u, 2u, 4u, 7u, 12u, 13u, 14u, 255u, 4096u, 65536u};
    for (size_t s = 0u; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
        size_t n = sizes[s];
        uint8_t *src = (uint8_t *)malloc(n);
        size_t cap = we_lz4_compress_bound(n);
        uint8_t *comp = (uint8_t *)malloc(cap);
        uint8_t *dec = (uint8_t *)malloc(n);
        TEST_CHECK(src && comp && dec);

        /* Repeating texture scanline pattern */
        for (size_t i = 0u; i < n; ++i) {
            src[i] = (uint8_t)((i / 4u) & 0xFFu);
        }
        size_t csz = we_lz4_compress_block(src, n, comp, cap);
        TEST_CHECK(csz > 0u && csz <= cap);
        int dsz = we_lz4_decompress_safe(comp, csz, dec, n);
        TEST_CHECK(dsz == (int)n);
        TEST_CHECK(memcmp(src, dec, n) == 0);

        /* Solid zeros */
        memset(src, 0x5Au, n);
        csz = we_lz4_compress_block(src, n, comp, cap);
        TEST_CHECK(csz > 0u && csz <= cap);
        dsz = we_lz4_decompress_safe(comp, csz, dec, n);
        TEST_CHECK(dsz == (int)n);
        TEST_CHECK(memcmp(src, dec, n) == 0);

        free(src);
        free(comp);
        free(dec);
    }

    /* Incompressible random payload with tight dst_cap rejects cleanly rather than expanding */
    size_t rlen = 8192u;
    uint8_t *rnd = (uint8_t *)malloc(rlen);
    uint8_t *tight = (uint8_t *)malloc(rlen - 1u);
    uint8_t *bound_out = (uint8_t *)malloc(we_lz4_compress_bound(rlen));
    uint8_t *dec = (uint8_t *)malloc(rlen);
    TEST_CHECK(rnd && tight && bound_out && dec);

    uint32_t rng = 0x12345678u;
    for (size_t i = 0u; i < rlen; ++i) {
        rnd[i] = (uint8_t)(xorshift32(&rng) >> 24);
    }
    TEST_CHECK(we_lz4_compress_block(rnd, rlen, tight, rlen - 1u) == 0u);
    size_t csz = we_lz4_compress_block(rnd, rlen, bound_out, we_lz4_compress_bound(rlen));
    TEST_CHECK(csz > 0u);
    TEST_CHECK(we_lz4_decompress_safe(bound_out, csz, dec, rlen) == (int)rlen);
    TEST_CHECK(memcmp(rnd, dec, rlen) == 0);

    free(rnd);
    free(tight);
    free(bound_out);
    free(dec);
}

static void test_lz4_4k_rgba8_benchmark(void)
{
    const size_t w = 3840u;
    const size_t h = 2160u;
    const size_t sz = w * h * 4u; /* 33,177,600 bytes */
    uint8_t *src = (uint8_t *)malloc(sz);
    size_t cap = we_lz4_compress_bound(sz);
    uint8_t *dst = (uint8_t *)malloc(cap);
    uint8_t *dec = (uint8_t *)malloc(sz);
    TEST_CHECK_GOTO(src && dst && dec, cleanup);

    /* Structured 4K wallpaper gradient + blocks */
    for (size_t y = 0u; y < h; ++y) {
        uint8_t *row = src + y * w * 4u;
        uint8_t gy = (uint8_t)(y >> 4);
        for (size_t x = 0u; x < w; ++x) {
            uint8_t gx = (uint8_t)(x >> 4);
            row[x * 4u + 0u] = gx;
            row[x * 4u + 1u] = gy;
            row[x * 4u + 2u] = (uint8_t)(gx ^ gy);
            row[x * 4u + 3u] = 255u;
        }
    }

    /* Warmup */
    size_t comp_sz = we_lz4_compress_block(src, sz, dst, cap);
    TEST_CHECK_GOTO(comp_sz > 0u && comp_sz < sz, cleanup);

    double t0 = monotonic_ms();
    comp_sz = we_lz4_compress_block(src, sz, dst, cap);
    double elapsed_ms = monotonic_ms() - t0;
    double mib_s = ((double)sz / (1024.0 * 1024.0)) / (elapsed_ms / 1000.0);
    printf("  4K RGBA8888 LZ4: %.2f ms (%.1f MiB/s), ratio %.1f%%\n", elapsed_ms, mib_s,
           100.0 * (double)comp_sz / (double)sz);

    TEST_CHECK_GOTO(comp_sz > 0u, cleanup);
    const char *strict = getenv("WWAL_STRICT_PERF_ASSERTS");
    if (strict && strict[0] != '\0' && strcmp(strict, "0") != 0) {
        TEST_CHECK_GOTO(elapsed_ms < 20.0, cleanup);
    } else {
        TEST_CHECK_GOTO(elapsed_ms < 5000.0, cleanup);
    }
    TEST_CHECK_GOTO(we_lz4_decompress_safe(dst, comp_sz, dec, sz) == (int)sz, cleanup);
    TEST_CHECK_GOTO(memcmp(src, dec, sz) == 0, cleanup);

cleanup:
    free(src);
    free(dst);
    free(dec);
}

static void test_pkg_write_roundtrip_and_determinism(void)
{
    const uint8_t blob_a[] = "{\"name\":\"scene\"}";
    const uint8_t blob_b[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02};
    uint8_t *big_blob = (uint8_t *)malloc(512u * 1024u);
    TEST_CHECK(big_blob != NULL);
    for (size_t i = 0u; i < 512u * 1024u; ++i) {
        big_blob[i] = (uint8_t)(i * 31u + 7u);
    }

    we_pkg_write_entry_t order1[] = {
        {.path = "shaders\\Custom.Frag", .data = blob_b, .size = sizeof(blob_b)},
        {.path = "empty.bin", .data = NULL, .size = 0u},
        {.path = "scene.json", .data = blob_a, .size = sizeof(blob_a) - 1u},
        {.path = "Materials/Big.tex", .data = big_blob, .size = 512u * 1024u},
    };
    we_pkg_write_entry_t order2[] = {
        {.path = "Materials\\Big.TEX", .data = big_blob, .size = 512u * 1024u},
        {.path = "scene.json", .data = blob_a, .size = sizeof(blob_a) - 1u},
        {.path = "empty.bin", .data = NULL, .size = 0u},
        {.path = "shaders/custom.frag", .data = blob_b, .size = sizeof(blob_b)},
    };

    uint8_t *pkg1 = NULL, *pkg2 = NULL;
    size_t sz1 = 0u, sz2 = 0u;
    we_asset_writer_err_t err = WE_WRITER_OK;
    TEST_CHECK(we_pkg_write_mem(order1, 4u, &pkg1, &sz1, &err));
    TEST_CHECK(err == WE_WRITER_OK);
    TEST_CHECK(we_pkg_write_mem(order2, 4u, &pkg2, &sz2, &err));
    TEST_CHECK(err == WE_WRITER_OK);
    TEST_CHECK(sz1 == sz2);
    TEST_CHECK(memcmp(pkg1, pkg2, sz1) == 0);

    we_pkg_t *opened = we_pkg_open_mem(pkg1, sz1, NULL);
    TEST_CHECK(opened != NULL);
    TEST_CHECK(opened->entry_count == 4u);

    size_t found_sz = 0u;
    const uint8_t *p = we_pkg_find(opened, "scene.json", &found_sz);
    TEST_CHECK(p && found_sz == sizeof(blob_a) - 1u && memcmp(p, blob_a, found_sz) == 0);

    p = we_pkg_find(opened, "MATERIALS/big.tex", &found_sz);
    TEST_CHECK(p && found_sz == 512u * 1024u && memcmp(p, big_blob, found_sz) == 0);

    p = we_pkg_find(opened, "shaders/custom.frag", &found_sz);
    TEST_CHECK(p && found_sz == sizeof(blob_b) && memcmp(p, blob_b, found_sz) == 0);

    p = we_pkg_find(opened, "empty.bin", &found_sz);
    TEST_CHECK(p != NULL && found_sz == 0u);
    we_pkg_close(opened);

    /* File atomic write + open_file */
    char dir[512], fpath[600];
    make_tmp_dir(dir, sizeof(dir), "pkg");
    snprintf(fpath, sizeof(fpath), "%s/scene.pkg", dir);
    TEST_CHECK(we_pkg_write_file(fpath, order1, 4u, &err));
    we_pkg_t *fopened = we_pkg_open_file(fpath, dir);
    TEST_CHECK(fopened != NULL);
    TEST_CHECK(fopened->entry_count == 4u);
    p = we_pkg_find(fopened, "materials/big.tex", &found_sz);
    TEST_CHECK(p && found_sz == 512u * 1024u && memcmp(p, big_blob, found_sz) == 0);
    we_pkg_close(fopened);

    /* Empty pkg (0 entries) */
    uint8_t *empty_pkg = NULL;
    size_t empty_sz = 0u;
    TEST_CHECK(we_pkg_write_mem(NULL, 0u, &empty_pkg, &empty_sz, &err));
    we_pkg_t *eopened = we_pkg_open_mem(empty_pkg, empty_sz, NULL);
    TEST_CHECK(eopened && eopened->entry_count == 0u);
    we_pkg_close(eopened);

    rm_rf(dir);
    free(empty_pkg);
    free(pkg1);
    free(pkg2);
    free(big_blob);
}

static void test_pkg_rejections(void)
{
    const uint8_t byte = 42u;
    uint8_t *out = NULL;
    size_t sz = 0u;
    we_asset_writer_err_t err = WE_WRITER_OK;

    const char *bad_paths[] = {
        "",      "../secret", "a/../b",    "./rel.txt",  "a/./b",
        "a/.",   "/abs",      "/abs/path", "\\win\\abs", "C:\\Windows\\System32",
        "D:foo", "bad\nname", "bad\x01",   "bad\x7f",    "a/",
        ".",     "..",
    };
    for (size_t i = 0u; i < sizeof(bad_paths) / sizeof(bad_paths[0]); ++i) {
        we_pkg_write_entry_t e = {.path = bad_paths[i], .data = &byte, .size = 1u};
        TEST_CHECK(!we_pkg_write_mem(&e, 1u, &out, &sz, &err));
        TEST_CHECK(err == WE_WRITER_ERR_INVALID_PATH);
    }

    char over_len[1030];
    memset(over_len, 'a', 1025u);
    over_len[1025] = '\0';
    we_pkg_write_entry_t long_e = {.path = over_len, .data = &byte, .size = 1u};
    TEST_CHECK(!we_pkg_write_mem(&long_e, 1u, &out, &sz, &err));
    TEST_CHECK(err == WE_WRITER_ERR_INVALID_PATH);

    /* Duplicate after ASCII case + backslash folding */
    we_pkg_write_entry_t dup_entries[2] = {
        {.path = "Materials/Tex.tex", .data = &byte, .size = 1u},
        {.path = "materials\\tex.TEX", .data = &byte, .size = 1u},
    };
    TEST_CHECK(!we_pkg_write_mem(dup_entries, 2u, &out, &sz, &err));
    TEST_CHECK(err == WE_WRITER_ERR_DUPLICATE_PATH);

    /* Limit and overflow checks */
    we_pkg_write_entry_t dummy = {.path = "a.bin", .data = &byte, .size = 1u};
    TEST_CHECK(!we_pkg_write_mem(&dummy, (size_t)WE_PKG_MAX_WRITE_ENTRIES + 1u, &out, &sz, &err));
    TEST_CHECK(err == WE_WRITER_ERR_LIMIT_EXCEEDED);

    we_pkg_write_entry_t over_total = {
        .path = "huge_total.bin",
        .data = &byte,
        .size = (size_t)WE_PKG_MAX_WRITE_SIZE + 1ull,
    };
    TEST_CHECK(!we_pkg_write_mem(&over_total, 1u, &out, &sz, &err));
    TEST_CHECK(err == WE_WRITER_ERR_LIMIT_EXCEEDED);

    we_pkg_write_entry_t over_u32 = {
        .path = "huge_u32.bin",
        .data = &byte,
        .size = (size_t)UINT32_MAX + 1ull,
    };
    TEST_CHECK(!we_pkg_write_mem(&over_u32, 1u, &out, &sz, &err));
    TEST_CHECK(err == WE_WRITER_ERR_OVERFLOW);
}

static void test_tex_formats_mips_and_spritesheet(void)
{
    /* 1. RGBA8888 with mip generation (non-POT 13x7 -> 4 mips) + LZ4 */
    uint32_t w = 13u, h = 7u;
    uint8_t *rgba0 = (uint8_t *)malloc((size_t)w * h * 4u);
    TEST_CHECK(rgba0 != NULL);
    for (uint32_t y = 0u; y < h; ++y) {
        for (uint32_t x = 0u; x < w; ++x) {
            size_t idx = ((size_t)y * w + x) * 4u;
            rgba0[idx + 0u] = (uint8_t)((x * 19u) & 0xFFu);
            rgba0[idx + 1u] = (uint8_t)((y * 31u) & 0xFFu);
            rgba0[idx + 2u] = 128u;
            rgba0[idx + 3u] = 240u;
        }
    }

    we_tex_encode_mip_t *gen_mips = NULL;
    uint32_t gen_mc = 0u;
    TEST_CHECK(we_tex_generate_mips(WE_TEX_FMT_RGBA8888, w, h, rgba0, &gen_mips, &gen_mc));
    TEST_CHECK(gen_mc == 4u); /* 13x7 -> 6x3 -> 3x1 -> 1x1 */
    TEST_CHECK(gen_mips[3].width == 1u && gen_mips[3].height == 1u);

    we_tex_encode_image_t img0 = {.mip_count = gen_mc, .mips = gen_mips};
    we_tex_encode_spec_t spec = {
        .format = WE_TEX_FMT_RGBA8888,
        .flags = WE_TEX_FLAG_CLAMP_UVS,
        .texture_width = w,
        .texture_height = h,
        .image_width = w,
        .image_height = h,
        .enable_lz4 = true,
        .image_count = 1u,
        .images = &img0,
    };

    uint8_t *tex_buf = NULL;
    size_t tex_sz = 0u;
    we_asset_writer_err_t err = WE_WRITER_OK;
    TEST_CHECK(we_tex_encode_v5(&spec, &tex_buf, &tex_sz, &err));
    TEST_CHECK(err == WE_WRITER_OK && tex_sz > 63u);
    TEST_CHECK(memcmp(tex_buf, "TEXV0005", 9u) == 0);
    TEST_CHECK(memcmp(tex_buf + 9u, "TEXI0001", 9u) == 0);

    we_tex_t *parsed = we_tex_parse_cpu(tex_buf, tex_sz);
    TEST_CHECK(parsed != NULL);
    TEST_CHECK(parsed->texv_version == 5u && parsed->texi_version == 1u &&
               parsed->texb_version == 3u);
    TEST_CHECK(parsed->format == WE_TEX_FMT_RGBA8888);
    TEST_CHECK(parsed->free_image_format == WE_FIF_UNKNOWN);
    TEST_CHECK(!parsed->is_video_mp4);
    TEST_CHECK(parsed->images[0].mip_count == gen_mc);
    for (uint32_t m = 0u; m < gen_mc; ++m) {
        TEST_CHECK(parsed->images[0].mips[m].data_size == gen_mips[m].byte_size);
        TEST_CHECK(
            memcmp(parsed->images[0].mips[m].data, gen_mips[m].pixels, gen_mips[m].byte_size) == 0);
    }

    uint8_t *dec_rgba = NULL;
    uint32_t dw = 0u, dh = 0u;
    TEST_CHECK(we_tex_decode_rgba8(parsed, &dec_rgba, &dw, &dh));
    TEST_CHECK(dw == w && dh == h);
    TEST_CHECK(memcmp(dec_rgba, rgba0, (size_t)w * h * 4u) == 0);

    free(dec_rgba);
    we_tex_destroy(parsed);
    free(tex_buf);
    we_tex_free_generated_mips(gen_mips, gen_mc);
    free(rgba0);

    /* 2. RG88 (8) and R8 (9) formats + file atomic write */
    uint8_t rg_px[4u * 4u * 2u];
    uint8_t r8_px[4u * 4u];
    for (size_t i = 0u; i < 16u; ++i) {
        rg_px[i * 2u + 0u] = (uint8_t)(i * 15u);
        rg_px[i * 2u + 1u] = (uint8_t)(255u - i * 10u);
        r8_px[i] = (uint8_t)(i * 16u);
    }
    we_tex_encode_mip_t rg_mip = {.width = 4u, .height = 4u, .pixels = rg_px, .byte_size = 32u};
    we_tex_encode_image_t rg_img = {.mip_count = 1u, .mips = &rg_mip};
    we_tex_encode_spec_t rg_spec = {
        .format = WE_TEX_FMT_RG88,
        .flags = WE_TEX_FLAG_NO_INTERPOLATION,
        .texture_width = 4u,
        .texture_height = 4u,
        .image_width = 4u,
        .image_height = 4u,
        .enable_lz4 = true,
        .image_count = 1u,
        .images = &rg_img,
    };
    TEST_CHECK(we_tex_encode_v5(&rg_spec, &tex_buf, &tex_sz, &err));
    parsed = we_tex_parse_cpu(tex_buf, tex_sz);
    TEST_CHECK(parsed && parsed->format == WE_TEX_FMT_RG88);
    TEST_CHECK(we_tex_decode_rgba8(parsed, &dec_rgba, &dw, &dh));
    TEST_CHECK(dec_rgba[0] == rg_px[0] && dec_rgba[1] == rg_px[0] && dec_rgba[3] == rg_px[1]);
    free(dec_rgba);
    we_tex_destroy(parsed);
    free(tex_buf);

    we_tex_encode_mip_t r8_mip = {.width = 4u, .height = 4u, .pixels = r8_px, .byte_size = 16u};
    we_tex_encode_image_t r8_img = {.mip_count = 1u, .mips = &r8_mip};
    we_tex_encode_spec_t r8_spec = {
        .format = WE_TEX_FMT_R8,
        .flags = WE_TEX_FLAG_NONE,
        .texture_width = 4u,
        .texture_height = 4u,
        .image_width = 4u,
        .image_height = 4u,
        .enable_lz4 = false,
        .image_count = 1u,
        .images = &r8_img,
    };
    char dir[512], tpath[600];
    make_tmp_dir(dir, sizeof(dir), "tex");
    snprintf(tpath, sizeof(tpath), "%s/r8.tex", dir);
    TEST_CHECK(we_tex_write_v5_file(tpath, &r8_spec, &err));
    TEST_CHECK(file_exists(tpath));
    rm_rf(dir);

    /* 3. TEXS0003 spritesheet across frame counts 1, 2, 64, 256, 257 */
    static const uint32_t frame_counts[] = {1u, 2u, 64u, 256u, 257u};
    uint8_t sheet_px[16u * 16u * 4u];
    uint32_t rng = 0xCAFEBABEu;
    for (size_t i = 0u; i < sizeof(sheet_px); ++i) {
        sheet_px[i] = (uint8_t)(xorshift32(&rng) >> 24);
    }
    we_tex_encode_mip_t smip = {
        .width = 16u, .height = 16u, .pixels = sheet_px, .byte_size = sizeof(sheet_px)};
    we_tex_encode_image_t simgs[2] = {
        {.mip_count = 1u, .mips = &smip},
        {.mip_count = 1u, .mips = &smip},
    };
    for (size_t fc_i = 0u; fc_i < sizeof(frame_counts) / sizeof(frame_counts[0]); ++fc_i) {
        uint32_t fc = frame_counts[fc_i];
        we_tex_frame_t *frames = (we_tex_frame_t *)calloc(fc, sizeof(*frames));
        TEST_CHECK(frames != NULL);
        for (uint32_t f = 0u; f < fc; ++f) {
            frames[f].image_id = (int32_t)(f & 1u);
            frames[f].frametime = 0.033f + 0.001f * (float)(f % 7u);
            frames[f].x = (float)((f % 4u) * 4u);
            frames[f].y = (float)(((f / 4u) % 4u) * 4u);
            frames[f].width = 4.0f;
            frames[f].width_y = 0.0f;
            frames[f].height_x = 0.0f;
            frames[f].height = 4.0f;
        }
        we_tex_encode_spec_t gspec = {
            .format = WE_TEX_FMT_RGBA8888,
            .flags = WE_TEX_FLAG_IS_GIF,
            .texture_width = 16u,
            .texture_height = 16u,
            .image_width = 16u,
            .image_height = 16u,
            .enable_lz4 = true,
            .image_count = 2u,
            .images = simgs,
            .frame_count = fc,
            .gif_width = 4u,
            .gif_height = 4u,
            .frames = frames,
        };
        TEST_CHECK(we_tex_encode_v5(&gspec, &tex_buf, &tex_sz, &err));
        parsed = we_tex_parse_cpu(tex_buf, tex_sz);
        TEST_CHECK(parsed != NULL && parsed->texs_version == 3u);
        TEST_CHECK(parsed->frame_count == fc);
        TEST_CHECK(parsed->gif_width == 4u && parsed->gif_height == 4u);
        TEST_CHECK(parsed->image_count == 2u);
        for (uint32_t f = 0u; f < fc; ++f) {
            TEST_CHECK(memcmp(&parsed->frames[f], &frames[f], sizeof(we_tex_frame_t)) == 0);
        }
        we_tex_destroy(parsed);
        free(tex_buf);
        free(frames);
    }
}

static void test_tex_rejections(void)
{
    uint8_t px[16] = {0};
    we_tex_encode_mip_t mip = {.width = 2u, .height = 2u, .pixels = px, .byte_size = 16u};
    we_tex_encode_image_t img = {.mip_count = 1u, .mips = &mip};
    we_asset_writer_err_t err = WE_WRITER_OK;
    uint8_t *out = NULL;
    size_t out_sz = 0u;

    /* Unsupported GPU block format */
    we_tex_encode_spec_t bad_fmt = {
        .format = WE_TEX_FMT_DXT5,
        .texture_width = 2u,
        .texture_height = 2u,
        .image_width = 2u,
        .image_height = 2u,
        .image_count = 1u,
        .images = &img,
    };
    TEST_CHECK(!we_tex_encode_v5(&bad_fmt, &out, &out_sz, &err));
    TEST_CHECK(err == WE_WRITER_ERR_UNSUPPORTED_FMT);

    /* Decomp size > WE_TEX_MAX_DECOMP_SIZE (16384 x 16384 x 4 = 1 GiB) */
    we_tex_encode_mip_t huge_mip = {
        .width = 16384u,
        .height = 16384u,
        .pixels = px,
        .byte_size = (size_t)16384u * 16384u * 4u,
    };
    we_tex_encode_image_t huge_img = {.mip_count = 1u, .mips = &huge_mip};
    we_tex_encode_spec_t huge_spec = {
        .format = WE_TEX_FMT_RGBA8888,
        .texture_width = 16384u,
        .texture_height = 16384u,
        .image_width = 16384u,
        .image_height = 16384u,
        .image_count = 1u,
        .images = &huge_img,
    };
    TEST_CHECK(!we_tex_encode_v5(&huge_spec, &out, &out_sz, &err));
    TEST_CHECK(err == WE_WRITER_ERR_OVERFLOW);

    /* Invalid spritesheet frame (NaN / bad image_id) */
    we_tex_frame_t bad_fr = {.image_id = 0, .frametime = NAN};
    we_tex_encode_spec_t bad_gif = {
        .format = WE_TEX_FMT_RGBA8888,
        .flags = WE_TEX_FLAG_IS_GIF,
        .texture_width = 2u,
        .texture_height = 2u,
        .image_width = 2u,
        .image_height = 2u,
        .image_count = 1u,
        .images = &img,
        .frame_count = 1u,
        .gif_width = 2u,
        .gif_height = 2u,
        .frames = &bad_fr,
    };
    TEST_CHECK(!we_tex_encode_v5(&bad_gif, &out, &out_sz, &err));
    TEST_CHECK(err == WE_WRITER_ERR_NON_FINITE);

    bad_fr.frametime = 0.05f;
    bad_fr.image_id = 1;
    TEST_CHECK(!we_tex_encode_v5(&bad_gif, &out, &out_sz, &err));
    TEST_CHECK(err == WE_WRITER_ERR_INVALID_ARG);
}

static we_tex_t *make_cpu_tex(uint32_t w, uint32_t h, uint8_t seed)
{
    we_tex_t *t = (we_tex_t *)calloc(1u, sizeof(*t));
    if (!t) {
        return NULL;
    }
    t->texv_version = 5u;
    t->texi_version = 1u;
    t->texb_version = 3u;
    t->format = WE_TEX_FMT_RGBA8888;
    t->texture_width = w;
    t->texture_height = h;
    t->image_width = w;
    t->image_height = h;
    t->image_count = 1u;
    t->free_image_format = WE_FIF_UNKNOWN;
    t->images = (we_tex_image_t *)calloc(1u, sizeof(we_tex_image_t));
    t->images[0].mip_count = 1u;
    t->images[0].mips = (we_tex_mip_t *)calloc(1u, sizeof(we_tex_mip_t));
    size_t sz = (size_t)w * h * 4u;
    uint8_t *px = (uint8_t *)malloc(sz);
    for (size_t i = 0u; i < sz; i += 4u) {
        px[i + 0u] = seed;
        px[i + 1u] = (uint8_t)(seed + 40u);
        px[i + 2u] = (uint8_t)(255u - seed);
        px[i + 3u] = 255u;
    }
    t->images[0].mips[0].width = w;
    t->images[0].mips[0].height = h;
    t->images[0].mips[0].byte_count = (uint32_t)sz;
    t->images[0].mips[0].decomp_size = (uint32_t)sz;
    t->images[0].mips[0].data = px;
    t->images[0].mips[0].data_size = sz;
    return t;
}

static void free_synth_scene(we_scene_t *sc)
{
    for (uint32_t i = 0u; i < sc->object_count; ++i) {
        for (uint32_t s = 0u; s < WE_MAX_PASS_TEXTURES; ++s) {
            if (sc->objects[i].base_pass.textures[s]) {
                we_tex_destroy(sc->objects[i].base_pass.textures[s]);
            }
        }
        free(sc->objects[i].effects);
    }
    free(sc->objects);
}

static void test_scene_project_jcs_and_export_roundtrip(void)
{
    we_scene_t sc = {0};
    snprintf(sc.project.title, sizeof(sc.project.title), "Creator Roundtrip");
    snprintf(sc.project.type, sizeof(sc.project.type), "scene");
    snprintf(sc.project.file, sizeof(sc.project.file), "scene.json");
    snprintf(sc.project.preview, sizeof(sc.project.preview), "preview.webp");
    sc.project.prop_count = 4u;

    snprintf(sc.project.props[0].key, sizeof(sc.project.props[0].key), "opacity");
    snprintf(sc.project.props[0].text, sizeof(sc.project.props[0].text), "Opacity");
    sc.project.props[0].type = WE_PROP_FLOAT;
    sc.project.props[0].val[0] = 0.75f;
    sc.project.props[0].num_components = 1u;

    snprintf(sc.project.props[1].key, sizeof(sc.project.props[1].key), "tint");
    sc.project.props[1].type = WE_PROP_COLOR;
    sc.project.props[1].val[0] = 0.1f;
    sc.project.props[1].val[1] = 0.5f;
    sc.project.props[1].val[2] = 0.9f;
    sc.project.props[1].num_components = 3u;

    snprintf(sc.project.props[2].key, sizeof(sc.project.props[2].key), "showlogo");
    sc.project.props[2].type = WE_PROP_BOOL;
    sc.project.props[2].val[0] = 1.0f;
    sc.project.props[2].num_components = 1u;

    snprintf(sc.project.props[3].key, sizeof(sc.project.props[3].key), "mode");
    sc.project.props[3].type = WE_PROP_INT;
    sc.project.props[3].val[0] = 2.0f;
    sc.project.props[3].num_components = 1u;

    sc.ortho_width = 1920.0f;
    sc.ortho_height = 1080.0f;
    sc.clear_enabled = true;
    sc.clear_color[0] = 0.05f;
    sc.clear_color[1] = 0.1f;
    sc.clear_color[2] = 0.2f;
    sc.lighting.ambient_color_rgb[0] = 0.1f;
    sc.lighting.ambient_color_rgb[1] = 0.1f;
    sc.lighting.ambient_color_rgb[2] = 0.1f;
    sc.lighting.sky_color_rgb[0] = 0.2f;
    sc.lighting.sky_color_rgb[1] = 0.2f;
    sc.lighting.sky_color_rgb[2] = 0.3f;
    sc.is_3d = true;
    sc.camera.projection_mode = WE_PROJ_PERSPECTIVE_3D;
    sc.camera.fov_y_deg = 55.0f;
    sc.camera.near_z = 2.0f;
    sc.camera.far_z = 5000.0f;
    sc.camera.eye_pos[0] = 960.0f;
    sc.camera.eye_pos[1] = 540.0f;
    sc.camera.eye_pos[2] = -900.0f;
    sc.camera.target_pos[0] = 960.0f;
    sc.camera.target_pos[1] = 540.0f;
    sc.camera.up_vec[1] = 1.0f;
    sc.camera.path_loop_mode = WE_CAM_PATH_PINGPONG;
    sc.camera.keyframe_count = 2u;
    sc.camera.keyframes[0] = (we_camera_kf_t){
        .time_sec = 0.0f,
        .position = {960.0f, 540.0f, -900.0f},
        .orientation_quat = {0.0f, 0.0f, 0.0f, 1.0f},
        .fov_deg = 55.0f,
        .focal_distance = 1000.0f,
        .aperture_fstop = 2.8f,
        .tension = 0.5f,
    };
    sc.camera.keyframes[1] = (we_camera_kf_t){
        .time_sec = 4.5f,
        .position = {1000.0f, 560.0f, -800.0f},
        .orientation_quat = {0.0f, 0.0f, 0.0f, 1.0f},
        .fov_deg = 48.0f,
        .focal_distance = 1000.0f,
        .aperture_fstop = 2.8f,
        .tension = 0.5f,
    };

    sc.object_count = 3u;
    sc.objects = (we_scene_object_t *)calloc(sc.object_count, sizeof(we_scene_object_t));
    TEST_CHECK(sc.objects != NULL);

    /* Object 0: image layer (id 42 first in draw order) */
    sc.objects[0].id = 42;
    sc.objects[0].parent_id = -1;
    snprintf(sc.objects[0].name, sizeof(sc.objects[0].name), "background");
    snprintf(sc.objects[0].image_path, sizeof(sc.objects[0].image_path), "models/bg.json");
    sc.objects[0].origin[0] = 960.0f;
    sc.objects[0].origin[1] = 540.0f;
    sc.objects[0].scale[0] = 1.0f;
    sc.objects[0].scale[1] = 1.0f;
    sc.objects[0].scale[2] = 1.0f;
    sc.objects[0].size[0] = 1920.0f;
    sc.objects[0].size[1] = 1080.0f;
    sc.objects[0].alpha = 0.75f;
    snprintf(sc.objects[0].alpha_user_prop, sizeof(sc.objects[0].alpha_user_prop), "opacity");
    sc.objects[0].brightness = 1.0f;
    sc.objects[0].color[0] = 0.1f;
    sc.objects[0].color[1] = 0.5f;
    sc.objects[0].color[2] = 0.9f;
    snprintf(sc.objects[0].color_user_prop, sizeof(sc.objects[0].color_user_prop), "tint");
    sc.objects[0].visible = true;
    snprintf(sc.objects[0].visible_user_prop, sizeof(sc.objects[0].visible_user_prop), "mode!=0");
    sc.objects[0].has_base_pass = true;
    snprintf(sc.objects[0].base_pass.shader_name, sizeof(sc.objects[0].base_pass.shader_name),
             "genericimage2");
    sc.objects[0].base_pass.blending = WE_BLEND_NORMAL;
    sc.objects[0].base_pass.depthwrite_enabled = true;
    sc.objects[0].base_pass.cull_enabled = true;
    snprintf(sc.objects[0].base_pass.tex_paths[0], sizeof(sc.objects[0].base_pass.tex_paths[0]),
             "bg");
    sc.objects[0].base_pass.textures[0] = make_cpu_tex(8u, 8u, 20u);

    /* Object 1: text layer (id 7 second in draw order) */
    sc.objects[1].id = 7;
    sc.objects[1].parent_id = 42;
    snprintf(sc.objects[1].name, sizeof(sc.objects[1].name), "title_text");
    sc.objects[1].origin[0] = 200.0f;
    sc.objects[1].origin[1] = 100.0f;
    sc.objects[1].scale[0] = 1.0f;
    sc.objects[1].scale[1] = 1.0f;
    sc.objects[1].scale[2] = 1.0f;
    sc.objects[1].size[0] = 512.0f;
    sc.objects[1].size[1] = 128.0f;
    sc.objects[1].alpha = 1.0f;
    sc.objects[1].brightness = 1.0f;
    sc.objects[1].color[0] = 1.0f;
    sc.objects[1].color[1] = 1.0f;
    sc.objects[1].color[2] = 1.0f;
    sc.objects[1].visible = true;
    snprintf(sc.objects[1].visible_user_prop, sizeof(sc.objects[1].visible_user_prop), "showlogo");
    sc.objects[1].is_text_object = true;
    snprintf(sc.objects[1].text_str, sizeof(sc.objects[1].text_str), "Waywal Studio");
    snprintf(sc.objects[1].text_font, sizeof(sc.objects[1].text_font), "monospace");
    sc.objects[1].text_pointsize = 36;
    snprintf(sc.objects[1].text_halign, sizeof(sc.objects[1].text_halign), "center");
    snprintf(sc.objects[1].text_valign, sizeof(sc.objects[1].text_valign), "center");
    sc.objects[1].text_padding = 4;

    /* Object 2: 3D spot light (id 99 third in draw order) */
    sc.objects[2].id = 99;
    sc.objects[2].parent_id = -1;
    snprintf(sc.objects[2].name, sizeof(sc.objects[2].name), "key_light");
    sc.objects[2].origin[0] = 500.0f;
    sc.objects[2].origin[1] = 400.0f;
    sc.objects[2].origin[2] = 300.0f;
    sc.objects[2].scale[0] = 1.0f;
    sc.objects[2].scale[1] = 1.0f;
    sc.objects[2].scale[2] = 1.0f;
    sc.objects[2].size[0] = 100.0f;
    sc.objects[2].size[1] = 100.0f;
    sc.objects[2].alpha = 1.0f;
    sc.objects[2].brightness = 1.0f;
    sc.objects[2].color[0] = 1.0f;
    sc.objects[2].color[1] = 0.8f;
    sc.objects[2].color[2] = 0.6f;
    sc.objects[2].visible = true;
    sc.objects[2].is_light_object = true;
    sc.objects[2].light_type = WE_LIGHT_SPOT;
    sc.objects[2].light_intensity = 2.5f;
    sc.objects[2].light_radius = 1200.0f;
    sc.objects[2].light_cone_angle = 40.0f;
    sc.objects[2].light_inner_cone_angle = 25.0f;
    sc.objects[2].light_specular_power = 48.0f;
    sc.objects[2].light_direction[2] = -1.0f;

    /* Locale independence check (comma decimal locales must produce identical JCS output) */
    char *s_c = NULL, *s_de = NULL;
    size_t len_c = 0u, len_de = 0u;
    we_asset_writer_err_t err = WE_WRITER_OK;
    TEST_CHECK(we_scene_serialize_json(&sc, &s_c, &len_c, &err));
    TEST_CHECK(err == WE_WRITER_OK && s_c != NULL);

    char *old_loc = strdup(setlocale(LC_NUMERIC, NULL));
    if (setlocale(LC_NUMERIC, "de_DE.UTF-8") || setlocale(LC_NUMERIC, "de_DE.utf8")) {
        TEST_CHECK(we_scene_serialize_json(&sc, &s_de, &len_de, &err));
        TEST_CHECK(len_c == len_de && memcmp(s_c, s_de, len_c) == 0);
        free(s_de);
    }
    if (old_loc) {
        setlocale(LC_NUMERIC, old_loc);
        free(old_loc);
    }

    /* Non-finite float rejection */
    we_scene_t bad_sc = sc;
    bad_sc.ortho_width = NAN;
    char *bad_out = NULL;
    size_t bad_len = 0u;
    TEST_CHECK(!we_scene_serialize_json(&bad_sc, &bad_out, &bad_len, &err));
    TEST_CHECK(err == WE_WRITER_ERR_NON_FINITE);

    /* Export loose directory + packed scene.pkg directory, then reload & check idempotence */
    const bool modes[2] = {false, true};
    for (int m = 0; m < 2; ++m) {
        char dir[512];
        make_tmp_dir(dir, sizeof(dir), m == 0 ? "loose" : "packed");
        TEST_CHECK(we_scene_export_dir(&sc, dir, modes[m], &err));
        TEST_CHECK(err == WE_WRITER_OK);

        char chk[600];
        snprintf(chk, sizeof(chk), "%s/project.json", dir);
        TEST_CHECK(file_exists(chk));
        snprintf(chk, sizeof(chk), "%s/preview.webp", dir);
        TEST_CHECK(file_exists(chk));
        if (modes[m]) {
            snprintf(chk, sizeof(chk), "%s/scene.pkg", dir);
            TEST_CHECK(file_exists(chk));
        } else {
            snprintf(chk, sizeof(chk), "%s/scene.json", dir);
            TEST_CHECK(file_exists(chk));
            snprintf(chk, sizeof(chk), "%s/materials/bg.tex", dir);
            TEST_CHECK(file_exists(chk));
        }

        we_scene_t *reloaded = we_scene_load_cpu(dir, NULL);
        TEST_CHECK(reloaded != NULL);
        TEST_CHECK(strcmp(reloaded->project.title, sc.project.title) == 0);
        TEST_CHECK(reloaded->project.prop_count == 4u);
        TEST_CHECK(reloaded->object_count == 3u);
        TEST_CHECK(reloaded->objects[0].id == 42 && reloaded->objects[1].id == 7 &&
                   reloaded->objects[2].id == 99);
        TEST_CHECK(strcmp(reloaded->objects[0].alpha_user_prop, "opacity") == 0);
        TEST_CHECK(strcmp(reloaded->objects[0].color_user_prop, "tint") == 0);
        TEST_CHECK(strcmp(reloaded->objects[0].visible_user_prop, "mode!=0") == 0);
        TEST_CHECK(reloaded->objects[0].has_base_pass);
        TEST_CHECK(reloaded->objects[0].base_pass.textures[0] != NULL);
        TEST_CHECK(reloaded->objects[1].is_text_object &&
                   strcmp(reloaded->objects[1].text_str, "Waywal Studio") == 0);
        TEST_CHECK(reloaded->objects[2].is_light_object &&
                   reloaded->objects[2].light_type == WE_LIGHT_SPOT);
        TEST_CHECK(reloaded->is_3d && reloaded->camera.keyframe_count == 2u);
        TEST_CHECK(fabsf(reloaded->ortho_width - 1920.0f) < 1e-4f);

        /* JCS idempotence: S(P(S(x))) == S(x) */
        char *s2 = NULL;
        size_t len2 = 0u;
        TEST_CHECK(we_scene_serialize_json(reloaded, &s2, &len2, &err));
        if (len_c != len2 || memcmp(s_c, s2, len_c) != 0) {
            size_t pos = 0u;
            while (pos < len_c && pos < len2 && s_c[pos] == s2[pos]) {
                ++pos;
            }
            fprintf(stderr, "DIFF at %zu:\n  s_c: %.160s\n  s2 : %.160s\n", pos,
                    pos < len_c ? s_c + (pos > 32 ? pos - 32 : 0) : "",
                    pos < len2 ? s2 + (pos > 32 ? pos - 32 : 0) : "");
        }
        TEST_CHECK(len_c == len2 && memcmp(s_c, s2, len_c) == 0);

        /* Project round-trip idempotence */
        char *p1 = NULL, *p2 = NULL;
        size_t pl1 = 0u, pl2 = 0u;
        TEST_CHECK(we_project_serialize_json(&sc.project, &p1, &pl1, &err));
        we_project_t p_parsed = {0};
        TEST_CHECK(we_project_parse(p1, pl1, &p_parsed));
        TEST_CHECK(we_project_serialize_json(&p_parsed, &p2, &pl2, &err));
        TEST_CHECK(pl1 == pl2 && memcmp(p1, p2, pl1) == 0);

        free(p1);
        free(p2);
        free(s2);
        we_scene_destroy(reloaded);
        rm_rf(dir);
    }

    free(s_c);
    free_synth_scene(&sc);
}

static char *slurp_text_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)sz + 1u);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t nr = sz ? fread(buf, 1u, (size_t)sz, f) : 0u;
    fclose(f);
    if (nr != (size_t)sz) {
        free(buf);
        return NULL;
    }
    buf[nr] = '\0';
    if (out_len) {
        *out_len = nr;
    }
    return buf;
}

static void test_normalize_rel_path_and_atomic_write(void)
{
    char norm[256];
    uint32_t nlen = 0u;
    TEST_CHECK(we_normalize_rel_path("models/a.json", norm, sizeof(norm), &nlen));
    TEST_CHECK(strcmp(norm, "models/a.json") == 0 && nlen == 13u);

    TEST_CHECK(we_normalize_rel_path("Materials\\\\Sub/Tex.tex", norm, sizeof(norm), &nlen));
    TEST_CHECK(strcmp(norm, "materials/sub/tex.tex") == 0 && nlen == 21u);

    const char *bad[] = {
        NULL,     "",     "/abs/path", "\\win\\abs", "C:\\foo", "D:bar",
        ".",      "./a",  "a/./b",     "a/.",        "..",      "../a",
        "a/../b", "a/..", "a/",        "bad\x01c",   "bad\x1f", "bad\x7f",
    };
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        TEST_CHECK(!we_normalize_rel_path(bad[i], norm, sizeof(norm), &nlen));
    }

    char tiny[5];
    TEST_CHECK(!we_normalize_rel_path("abcdef", tiny, sizeof(tiny), &nlen));
    TEST_CHECK(!we_normalize_rel_path("a", tiny, 1u, &nlen));

    char tmpl[] = "/tmp/we_atomic_XXXXXX";
    char *dir = mkdtemp(tmpl);
    TEST_CHECK(dir != NULL);
    char fpath[512];
    snprintf(fpath, sizeof(fpath), "%s/out.txt", dir);
    we_asset_writer_err_t err = WE_WRITER_ERR_IO;
    TEST_CHECK(we_write_file_atomic(fpath, "first", 5u, &err));
    TEST_CHECK(err == WE_WRITER_OK);
    TEST_CHECK(we_write_file_atomic(fpath, "second!", 7u, &err));
    TEST_CHECK(err == WE_WRITER_OK);
    size_t rlen = 0u;
    char *got = slurp_text_file(fpath, &rlen);
    TEST_CHECK(got != NULL && rlen == 7u && memcmp(got, "second!", 7u) == 0);
    free(got);
    rm_rf(dir);
}

static void test_scene_export_security_and_edge_cases(void)
{
    we_asset_writer_err_t err = WE_WRITER_OK;

    /* 1. Up-front validation must not create export_dir/project.json/preview on invalid path */
    {
        char tmpl[] = "/tmp/we_upfront_XXXXXX";
        char *base = mkdtemp(tmpl);
        TEST_CHECK(base != NULL);
        char export_dir[512];
        snprintf(export_dir, sizeof(export_dir), "%s/nested_out", base);

        we_scene_t sc = {0};
        sc.ortho_width = 1920.0f;
        sc.ortho_height = 1080.0f;
        snprintf(sc.project.file, sizeof(sc.project.file), "../evil.json");
        TEST_CHECK(!we_scene_export_dir(&sc, export_dir, false, &err));
        TEST_CHECK(err == WE_WRITER_ERR_INVALID_PATH);
        TEST_CHECK(access(export_dir, F_OK) != 0);

        we_scene_object_t obj = {0};
        obj.id = 1;
        obj.has_base_pass = true;
        snprintf(obj.image_path, sizeof(obj.image_path), "C:\\evil.json");
        sc.project.file[0] = '\0';
        sc.object_count = 1u;
        sc.objects = &obj;
        TEST_CHECK(!we_scene_export_dir(&sc, export_dir, false, &err));
        TEST_CHECK(err == WE_WRITER_ERR_INVALID_PATH);
        TEST_CHECK(access(export_dir, F_OK) != 0);
        rm_rf(base);
    }

    /* 2. copy_existing_file rejects FIFO and symlink previews without clobbering out_err */
    for (int mode = 0; mode < 2; ++mode) {
        char wtmpl[] = "/tmp/we_ws_prev_XXXXXX";
        char *ws = mkdtemp(wtmpl);
        TEST_CHECK(ws != NULL);
        char prev_src[512];
        snprintf(prev_src, sizeof(prev_src), "%s/preview.jpg", ws);
        if (mode == 0) {
            TEST_CHECK(mkfifo(prev_src, 0600) == 0);
        } else {
            char real_f[512];
            snprintf(real_f, sizeof(real_f), "%s/target.jpg", ws);
            FILE *rf = fopen(real_f, "wb");
            TEST_CHECK(rf != NULL);
            fwrite("fake", 1u, 4u, rf);
            fclose(rf);
            TEST_CHECK(symlink(real_f, prev_src) == 0);
        }

        char etmpl[] = "/tmp/we_exp_prev_XXXXXX";
        char *exp = mkdtemp(etmpl);
        TEST_CHECK(exp != NULL);
        we_scene_t sc = {0};
        sc.ortho_width = 1920.0f;
        sc.ortho_height = 1080.0f;
        snprintf(sc.project.workshop_dir, sizeof(sc.project.workshop_dir), "%s", ws);
        TEST_CHECK(we_scene_export_dir(&sc, exp, false, &err));
        TEST_CHECK(err == WE_WRITER_OK);

        char copied_prev[512];
        snprintf(copied_prev, sizeof(copied_prev), "%s/preview.jpg", exp);
        TEST_CHECK(access(copied_prev, F_OK) != 0);
        rm_rf(ws);
        rm_rf(exp);
    }

    /* 3. Non-.json image_path (.png) must not be overwritten with model JSON + empty sound */
    {
        char etmpl[] = "/tmp/we_png_snd_XXXXXX";
        char *exp = mkdtemp(etmpl);
        TEST_CHECK(exp != NULL);

        we_scene_object_t objs[2] = {0};
        objs[0].id = 1;
        objs[0].visible = true;
        objs[0].alpha = 1.0f;
        objs[0].scale[0] = objs[0].scale[1] = objs[0].scale[2] = 1.0f;
        objs[0].size[0] = 640.0f;
        objs[0].size[1] = 360.0f;
        objs[0].color[0] = objs[0].color[1] = objs[0].color[2] = 1.0f;
        objs[0].has_base_pass = true;
        snprintf(objs[0].image_path, sizeof(objs[0].image_path), "wallpaper.png");
        snprintf(objs[0].base_pass.shader_name, sizeof(objs[0].base_pass.shader_name),
                 "genericimage2");

        objs[1].id = 2;
        objs[1].visible = true;
        objs[1].alpha = 1.0f;
        objs[1].is_sound_object = true;
        objs[1].sound_track_count = 0u;

        we_scene_t sc = {0};
        sc.ortho_width = 1920.0f;
        sc.ortho_height = 1080.0f;
        sc.object_count = 2u;
        sc.objects = objs;

        TEST_CHECK(we_scene_export_dir(&sc, exp, false, &err));
        TEST_CHECK(err == WE_WRITER_OK);

        char bad_png[512], mdl_json[512], mat_json[512];
        snprintf(bad_png, sizeof(bad_png), "%s/wallpaper.png", exp);
        snprintf(mdl_json, sizeof(mdl_json), "%s/models/layer_0.json", exp);
        snprintf(mat_json, sizeof(mat_json), "%s/materials/layer_0.json", exp);
        TEST_CHECK(access(bad_png, F_OK) != 0);
        TEST_CHECK(access(mdl_json, F_OK) == 0);
        TEST_CHECK(access(mat_json, F_OK) == 0);

        we_scene_t *reloaded = we_scene_load_cpu(exp, NULL);
        TEST_CHECK(reloaded != NULL);
        TEST_CHECK(reloaded->object_count == 2u);
        TEST_CHECK(reloaded->objects[1].is_sound_object);
        we_scene_destroy(reloaded);
        rm_rf(exp);
    }

    /* 4. NULL effects/passes guard */
    {
        we_scene_object_t obj = {0};
        obj.id = 1;
        obj.effect_count = 1u;
        obj.effects = NULL;
        we_scene_t sc = {0};
        sc.ortho_width = 1920.0f;
        sc.ortho_height = 1080.0f;
        sc.object_count = 1u;
        sc.objects = &obj;
        char *jout = NULL;
        size_t jlen = 0u;
        TEST_CHECK(!we_scene_serialize_json(&sc, &jout, &jlen, &err));
        TEST_CHECK(err == WE_WRITER_ERR_INVALID_ARG);
    }

    /* 5. we_tex_encode_v5 mip0 dimension & gif dimension validation */
    {
        uint8_t px[4 * 4 * 4] = {0};
        we_tex_encode_mip_t mip = {
            .width = 4u, .height = 4u, .pixels = px, .byte_size = sizeof(px)};
        we_tex_encode_image_t img = {.mip_count = 1u, .mips = &mip};
        we_tex_encode_spec_t bad_dim = {
            .format = WE_TEX_FMT_RGBA8888,
            .texture_width = 8u,
            .texture_height = 4u,
            .image_width = 4u,
            .image_height = 4u,
            .image_count = 1u,
            .images = &img,
        };
        uint8_t *tbuf = NULL;
        size_t tsz = 0u;
        TEST_CHECK(!we_tex_encode_v5(&bad_dim, &tbuf, &tsz, &err));
        TEST_CHECK(err == WE_WRITER_ERR_INVALID_ARG);

        we_tex_frame_t frm = {.image_id = 0u, .frametime = 0.1f, .width = 4.0f, .height = 4.0f};
        we_tex_encode_spec_t bad_gif = {
            .format = WE_TEX_FMT_RGBA8888,
            .flags = WE_TEX_FLAG_IS_GIF,
            .texture_width = 4u,
            .texture_height = 4u,
            .image_width = 4u,
            .image_height = 4u,
            .image_count = 1u,
            .images = &img,
            .frame_count = 1u,
            .gif_width = 0u,
            .gif_height = 4u,
            .frames = &frm,
        };
        TEST_CHECK(!we_tex_encode_v5(&bad_gif, &tbuf, &tsz, &err));
        TEST_CHECK(err == WE_WRITER_ERR_INVALID_ARG);
    }
}

static void test_live_shader_export_roundtrip(void)
{
    static const char *k_vert =
        "attribute vec3 a_Position;\n"
        "attribute vec2 a_TexCoord;\n"
        "varying vec2 v_TexCoord;\n"
        "uniform mat4 g_ModelViewProjectionMatrix;\n"
        "void main() {\n"
        "  v_TexCoord = a_TexCoord;\n"
        "  gl_Position = g_ModelViewProjectionMatrix * vec4(a_Position, 1.0);\n"
        "}\n";
    static const char *k_frag = "varying vec2 v_TexCoord;\n"
                                "uniform sampler2D g_Texture0;\n"
                                "void main() {\n"
                                "  vec4 live_custom_tint = texSample2D(g_Texture0, v_TexCoord);\n"
                                "  gl_FragColor = live_custom_tint;\n"
                                "}\n";

    we_scene_object_t obj = {0};
    obj.id = 9;
    obj.visible = true;
    obj.alpha = 1.0f;
    obj.scale[0] = obj.scale[1] = obj.scale[2] = 1.0f;
    obj.size[0] = 512.0f;
    obj.size[1] = 512.0f;
    obj.color[0] = obj.color[1] = obj.color[2] = 1.0f;
    obj.has_base_pass = true;
    snprintf(obj.base_pass.shader_name, sizeof(obj.base_pass.shader_name), "live_custom");
    TEST_CHECK(we_shader_preprocess_pair(k_vert, k_frag, NULL, NULL, 0u, 0x1u, false,
                                         &obj.base_pass.prep));

    we_scene_t sc = {0};
    sc.pkg = NULL;
    sc.ortho_width = 1920.0f;
    sc.ortho_height = 1080.0f;
    sc.object_count = 1u;
    sc.objects = &obj;

    char tmpl[] = "/tmp/we_live_sh_XXXXXX";
    char *exp = mkdtemp(tmpl);
    TEST_CHECK(exp != NULL);
    we_asset_writer_err_t err = WE_WRITER_OK;
    TEST_CHECK(we_scene_export_dir(&sc, exp, false, &err));
    TEST_CHECK(err == WE_WRITER_OK);

    char vpath[512], fpath[512];
    snprintf(vpath, sizeof(vpath), "%s/shaders/live_custom.vert", exp);
    snprintf(fpath, sizeof(fpath), "%s/shaders/live_custom.frag", exp);
    char *vtxt = slurp_text_file(vpath, NULL);
    char *ftxt = slurp_text_file(fpath, NULL);
    TEST_CHECK(vtxt != NULL && ftxt != NULL);
    TEST_CHECK(strncmp(vtxt, "#version 310 es", 15u) != 0);
    TEST_CHECK(strncmp(ftxt, "#version 310 es", 15u) != 0);
    TEST_CHECK(strstr(ftxt, "live_custom_tint") != NULL);

    we_scene_t *reloaded = we_scene_load_cpu(exp, NULL);
    TEST_CHECK(reloaded != NULL);
    TEST_CHECK(reloaded->object_count == 1u);
    TEST_CHECK(reloaded->objects[0].base_pass.prep.vert_glsl != NULL);
    TEST_CHECK(reloaded->objects[0].base_pass.prep.frag_glsl != NULL);
    TEST_CHECK(strstr(reloaded->objects[0].base_pass.prep.frag_glsl, "live_custom_tint") != NULL);

    free(vtxt);
    free(ftxt);
    free(obj.base_pass.prep.vert_glsl);
    free(obj.base_pass.prep.frag_glsl);
    we_scene_destroy(reloaded);
    rm_rf(exp);
}

static void test_we_script_template_lint_and_abort(void)
{
    we_script_diag_t diag = {0};
    const char *good_tpl = "export function update(v) { return `v=${v + 1}`; }";
    TEST_CHECK(we_script_lint(good_tpl, strlen(good_tpl), 0u, &diag));
    TEST_CHECK(diag.status_code == WE_SCRIPT_LINT_OK);

    const char *bad_tpl = "const msg = `line1\n  ${1 + }`;";
    TEST_CHECK(!we_script_lint(bad_tpl, strlen(bad_tpl), 0u, &diag));
    TEST_CHECK(diag.status_code == WE_SCRIPT_LINT_SYNTAX_ERR);
    TEST_CHECK(diag.error_line == 2u);
    TEST_CHECK(diag.error_col >= 5u);

    /* Exhaust the bounded 64 KiB lint arena via template chunks to verify abort propagation. */
    size_t big_cap = 96u * 1024u;
    char *big_src = (char *)malloc(big_cap);
    TEST_CHECK(big_src != NULL);
    size_t pos = 0u;
    pos += (size_t)snprintf(big_src + pos, big_cap - pos, "let s = `");
    while (pos + 24u < 72u * 1024u) {
        pos += (size_t)snprintf(big_src + pos, big_cap - pos, "ab${1+2}");
    }
    pos += (size_t)snprintf(big_src + pos, big_cap - pos, "`;");
    TEST_CHECK(!we_script_lint(big_src, pos, 0u, &diag));
    TEST_CHECK(diag.status_code != WE_SCRIPT_LINT_OK);
    free(big_src);
}

static int g_destroy_cb_calls = 0;
static uint64_t g_destroy_cb_rev = 0u;

static void on_scene_destroy_spy(const we_scene_t *scene)
{
    ++g_destroy_cb_calls;
    g_destroy_cb_rev = scene ? scene->authoring_revision : 0u;
}

static void test_scene_destroy_callback_and_revision(void)
{
    we_scene_t *sc = (we_scene_t *)calloc(1u, sizeof(*sc));
    TEST_CHECK(sc != NULL);
    g_destroy_cb_calls = 0;
    g_destroy_cb_rev = 0u;
    sc->authoring_revision = 42u;
    sc->on_destroy_cb = on_scene_destroy_spy;
    we_scene_destroy(sc);
    TEST_CHECK(g_destroy_cb_calls == 1);
    TEST_CHECK(g_destroy_cb_rev == 42u);
}

static void test_audio_binding_clamps_and_cooldown(void)
{
    we_scene_t *sc = (we_scene_t *)calloc(1u, sizeof(*sc));
    TEST_CHECK(sc != NULL);
    sc->objects = (we_scene_object_t *)calloc(1u, sizeof(*sc->objects));
    TEST_CHECK(sc->objects != NULL);
    sc->object_count = 1u;
    sc->objects[0].id = 7;
    sc->objects[0].alpha = 0.5f;
    sc->objects[0].particle_sys = we_particle_create_preset(WE_PARTICLE_PRESET_EMBERS, false);
    TEST_CHECK(sc->objects[0].particle_sys != NULL);
    sc->objects[0].particle_sys->alive_count = 0u;

    we_audio_binding_t bindings[2] = {0};
    bindings[0].binding_id = 1u;
    bindings[0].target_object_id = 7;
    bindings[0].target_kind = WE_AUDIO_TGT_PARTICLE_BURST_TRIG;
    bindings[0].src_kind = WE_AUDIO_SRC_BEAT_PULSE;
    bindings[0].input_gain = 1.0f;
    bindings[0].threshold_open = 0.1f;
    bindings[0].attack_sec = NAN;
    bindings[0].release_sec = INFINITY;
    bindings[0].curve_shape = WE_AUDIO_CURVE_LINEAR;
    bindings[0].blend_mode = WE_AUDIO_BLEND_REPLACE;
    bindings[0].out_min = 1.0f;
    bindings[0].out_max = 1e9f;
    bindings[0].enabled = 1u;

    bindings[1].binding_id = 2u;
    bindings[1].target_object_id = 7;
    bindings[1].target_kind = WE_AUDIO_TGT_LAYER_ALPHA;
    bindings[1].src_kind = WE_AUDIO_SRC_ENVELOPE;
    bindings[1].input_gain = 1.0f;
    bindings[1].threshold_open = 0.0f;
    bindings[1].curve_shape = WE_AUDIO_CURVE_LINEAR;
    bindings[1].blend_mode = WE_AUDIO_BLEND_REPLACE;
    bindings[1].out_min = 0.0f;
    bindings[1].out_max = 1e20f;
    bindings[1].enabled = 1u;

    TEST_CHECK(we_scene_apply_audio_bindings(sc, WE_AUDIO_BIND_OP_REPLACE_ALL, bindings, 2u, 0u));

    waywal_audio_frame_t frame = {0};
    frame.beat_pulse = 1.0f;
    frame.envelope = 1.0f;

    TEST_CHECK(we_scene_step_audio_bindings(sc, &frame, 0.016f) == 2u);
    TEST_CHECK(sc->audio_bindings[0].cooldown_rem_sec > 0.0f);
    TEST_CHECK(sc->objects[0].particle_sys->alive_count > 0u);
    TEST_CHECK(sc->objects[0].particle_sys->alive_count <= 4096u);
    TEST_CHECK(isfinite(sc->objects[0].alpha) && sc->objects[0].alpha <= 1e6f);

    uint32_t alive_after_first = sc->objects[0].particle_sys->alive_count;
    TEST_CHECK(we_scene_step_audio_bindings(sc, &frame, 0.016f) == 2u);
    /* Linear curve must also honor burst cooldown_rem_sec. */
    TEST_CHECK(sc->objects[0].particle_sys->alive_count == alive_after_first);

    we_scene_destroy(sc);
}

static int cmp_jcs_utf16_keys(const char *a, const char *b)
{
    const uint8_t *s1 = (const uint8_t *)(a ? a : "");
    const uint8_t *s2 = (const uint8_t *)(b ? b : "");
    while (*s1 && *s2) {
        uint32_t cp1 = 0u, cp2 = 0u;
        if (*s1 < 0x80u) {
            cp1 = *s1++;
        } else if ((*s1 & 0xE0u) == 0xC0u) {
            cp1 = ((uint32_t)(s1[0] & 0x1Fu) << 6) | (s1[1] & 0x3Fu);
            s1 += 2;
        } else if ((*s1 & 0xF0u) == 0xE0u) {
            cp1 = ((uint32_t)(s1[0] & 0x0Fu) << 12) | ((uint32_t)(s1[1] & 0x3Fu) << 6) |
                  (s1[2] & 0x3Fu);
            s1 += 3;
        } else {
            cp1 = ((uint32_t)(s1[0] & 0x07u) << 18) | ((uint32_t)(s1[1] & 0x3Fu) << 12) |
                  ((uint32_t)(s1[2] & 0x3Fu) << 6) | (s1[3] & 0x3Fu);
            s1 += 4;
        }
        if (*s2 < 0x80u) {
            cp2 = *s2++;
        } else if ((*s2 & 0xE0u) == 0xC0u) {
            cp2 = ((uint32_t)(s2[0] & 0x1Fu) << 6) | (s2[1] & 0x3Fu);
            s2 += 2;
        } else if ((*s2 & 0xF0u) == 0xE0u) {
            cp2 = ((uint32_t)(s2[0] & 0x0Fu) << 12) | ((uint32_t)(s2[1] & 0x3Fu) << 6) |
                  (s2[2] & 0x3Fu);
            s2 += 3;
        } else {
            cp2 = ((uint32_t)(s2[0] & 0x07u) << 18) | ((uint32_t)(s2[1] & 0x3Fu) << 12) |
                  ((uint32_t)(s2[2] & 0x3Fu) << 6) | (s2[3] & 0x3Fu);
            s2 += 4;
        }
        uint16_t u1[2], u2[2];
        size_t n1 = 1u, n2 = 1u;
        if (cp1 <= 0xFFFFu) {
            u1[0] = (uint16_t)cp1;
        } else {
            u1[0] = (uint16_t)(0xD800u + ((cp1 - 0x10000u) >> 10));
            u1[1] = (uint16_t)(0xDC00u + ((cp1 - 0x10000u) & 0x3FFu));
            n1 = 2u;
        }
        if (cp2 <= 0xFFFFu) {
            u2[0] = (uint16_t)cp2;
        } else {
            u2[0] = (uint16_t)(0xD800u + ((cp2 - 0x10000u) >> 10));
            u2[1] = (uint16_t)(0xDC00u + ((cp2 - 0x10000u) & 0x3FFu));
            n2 = 2u;
        }
        size_t mn = n1 < n2 ? n1 : n2;
        for (size_t i = 0u; i < mn; ++i) {
            if (u1[i] != u2[i]) {
                return u1[i] < u2[i] ? -1 : 1;
            }
        }
        if (n1 != n2) {
            return n1 < n2 ? -1 : 1;
        }
    }
    return *s1 ? 1 : (*s2 ? -1 : 0);
}

static bool check_json_rfc8785_order_recursive(const we_jval_t *v)
{
    if (!v) {
        return true;
    }
    if (v->type == JVAL_ARR) {
        for (size_t i = 0u; i < v->arr.count; ++i) {
            if (!check_json_rfc8785_order_recursive(v->arr.items[i])) {
                return false;
            }
        }
        return true;
    }
    if (v->type == JVAL_OBJ) {
        for (size_t i = 0u; i < v->obj.count; ++i) {
            if (i > 0u &&
                cmp_jcs_utf16_keys(v->obj.entries[i - 1u].key, v->obj.entries[i].key) >= 0) {
                return false;
            }
            if (!check_json_rfc8785_order_recursive(v->obj.entries[i].val)) {
                return false;
            }
        }
    }
    return true;
}

static bool verify_json_file_is_strict_rfc8785(const char *path)
{
    size_t len = 0u;
    char *raw = slurp_text_file(path, &len);
    if (!raw) {
        return false;
    }
    we_jval_t *root = we_json_parse_root(raw, len);
    bool ordered = root && check_json_rfc8785_order_recursive(root);
    size_t clen = 0u;
    char *canon = root ? we_jval_to_canonical_json(root, &clen) : NULL;
    bool exact = ordered && canon && clen == len && memcmp(canon, raw, len) == 0;
    free(canon);
    we_jval_free(root);
    free(raw);
    return exact;
}

static const char *set_comma_decimal_locale(char *saved_buf, size_t saved_cap)
{
    const char *cur = setlocale(LC_NUMERIC, NULL);
    snprintf(saved_buf, saved_cap, "%s", cur ? cur : "C");
    static const char *const k_cands[] = {
        "de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "ru_RU.UTF-8", "ru_RU.utf8",
    };
    for (size_t i = 0u; i < sizeof(k_cands) / sizeof(k_cands[0]); ++i) {
        const char *got = setlocale(LC_NUMERIC, k_cands[i]);
        if (got) {
            return got;
        }
    }
    return NULL;
}

static void test_e2e_f01_scripts_and_script_src_roundtrip(void)
{
    static const char *k_orig_js =
        "var sharedSpeed = 1.5;\n"
        "export function update(value) {\n"
        "  var acc = sharedSpeed;\n"
        "  for (var i = 0; i < 3; i++) { acc += i; }\n"
        "  switch (i) { case 3: acc = acc + 0.5; break; default: acc = 0; }\n"
        "  try { if (acc > 0) { return value + new Vec3(acc, 0, 0); } } catch (e) {}\n"
        "  return value;\n"
        "}\n";
    static const char *k_alpha_js =
        "export function update(v) { return (v * 0.5) > 0.2 ? 0.8 : 0.4; }";
    static const char *k_vis_js = "export function update(v) { return true; }";
    static const char *k_text_js = "export function update(v) { var n = 42; return `tick=${n}`; }";
    static const char *k_ps_js = "export function update(v) { return 28; }";
    static const char *k_vol_js = "export function update(v) { return 0.75; }";
    static const char *k_uni_src = "export function update(v) { return v + 0.25; }";
    static const char *k_rt_uni_js = "export function update(v) { return v * 1.25; }";

    we_scene_t sc = {0};
    snprintf(sc.project.title, sizeof(sc.project.title), "Script Roundtrip");
    snprintf(sc.project.type, sizeof(sc.project.type), "scene");
    snprintf(sc.project.file, sizeof(sc.project.file), "scene.json");
    sc.ortho_width = 1920.0f;
    sc.ortho_height = 1080.0f;

    sc.object_count = 3u;
    sc.objects = (we_scene_object_t *)calloc(3u, sizeof(we_scene_object_t));
    TEST_CHECK(sc.objects != NULL);

    /* Object 0: image layer with property scripts + constantshadervalues script_src */
    we_scene_object_t *o0 = &sc.objects[0];
    o0->id = 11;
    o0->parent_id = -1;
    snprintf(o0->name, sizeof(o0->name), "scripted_bg");
    o0->origin[0] = 100.5f;
    o0->origin[1] = 200.25f;
    o0->scale[0] = o0->scale[1] = o0->scale[2] = 1.0f;
    o0->size[0] = 640.0f;
    o0->size[1] = 360.0f;
    o0->alpha = 0.9f;
    snprintf(o0->alpha_user_prop, sizeof(o0->alpha_user_prop), "opacity");
    o0->brightness = 1.0f;
    o0->color[0] = o0->color[1] = o0->color[2] = 1.0f;
    o0->visible = true;
    o0->has_base_pass = true;
    snprintf(o0->base_pass.shader_name, sizeof(o0->base_pass.shader_name), "genericimage2");
    snprintf(o0->base_pass.tex_paths[0], sizeof(o0->base_pass.tex_paths[0]), "bgtex");
    o0->base_pass.textures[0] = make_cpu_tex(4u, 4u, 90u);
    o0->base_pass.const_val_count = 2u;
    snprintf(o0->base_pass.const_vals[0].name, sizeof(o0->base_pass.const_vals[0].name),
             "u_scroll");
    snprintf(o0->base_pass.const_vals[0].user_prop, sizeof(o0->base_pass.const_vals[0].user_prop),
             "scrollprop");
    o0->base_pass.const_vals[0].num_components = 1u;
    o0->base_pass.const_vals[0].val[0] = 0.5f;
    o0->base_pass.const_vals[0].script_src = strdup(k_uni_src);
    snprintf(o0->base_pass.const_vals[1].name, sizeof(o0->base_pass.const_vals[1].name), "u_gain");
    o0->base_pass.const_vals[1].num_components = 1u;
    o0->base_pass.const_vals[1].val[0] = 1.0f;

    o0->scripts[0].target = WE_SCRIPT_PROP_ORIGIN;
    o0->scripts[0].vm = we_script_create(k_orig_js, strlen(k_orig_js), &sc.script_shared);
    float sp_val[4] = {2.5f, 0.0f, 0.0f, 0.0f};
    we_script_set_prop(o0->scripts[0].vm, "boost", sp_val, 1u, NULL);
    we_script_set_prop(o0->scripts[0].vm, "tag", sp_val, 0u, "pulse");

    o0->scripts[1].target = WE_SCRIPT_PROP_ALPHA;
    o0->scripts[1].vm = we_script_create(k_alpha_js, strlen(k_alpha_js), &sc.script_shared);
    o0->scripts[2].target = WE_SCRIPT_PROP_VISIBLE;
    o0->scripts[2].vm = we_script_create(k_vis_js, strlen(k_vis_js), &sc.script_shared);
    o0->scripts[3].target = WE_SCRIPT_PROP_UNIFORM;
    snprintf(o0->scripts[3].uniform_name, sizeof(o0->scripts[3].uniform_name), "u_gain");
    o0->scripts[3].vm = we_script_create(k_rt_uni_js, strlen(k_rt_uni_js), &sc.script_shared);
    o0->script_count = 4u;

    /* Object 1: text layer with text + pointsize scripts */
    we_scene_object_t *o1 = &sc.objects[1];
    o1->id = 12;
    o1->parent_id = -1;
    snprintf(o1->name, sizeof(o1->name), "scripted_text");
    o1->scale[0] = o1->scale[1] = o1->scale[2] = 1.0f;
    o1->size[0] = 300.0f;
    o1->size[1] = 80.0f;
    o1->alpha = 1.0f;
    o1->brightness = 1.0f;
    o1->color[0] = o1->color[1] = o1->color[2] = 1.0f;
    o1->visible = true;
    o1->is_text_object = true;
    snprintf(o1->text_str, sizeof(o1->text_str), "initial");
    snprintf(o1->text_font, sizeof(o1->text_font), "monospace");
    o1->text_pointsize = 24;
    o1->scripts[0].target = WE_SCRIPT_PROP_TEXT;
    o1->scripts[0].vm = we_script_create(k_text_js, strlen(k_text_js), &sc.script_shared);
    o1->scripts[1].target = WE_SCRIPT_PROP_POINTSIZE;
    o1->scripts[1].vm = we_script_create(k_ps_js, strlen(k_ps_js), &sc.script_shared);
    o1->script_count = 2u;

    /* Object 2: sound layer with volume script */
    we_scene_object_t *o2 = &sc.objects[2];
    o2->id = 13;
    o2->parent_id = -1;
    snprintf(o2->name, sizeof(o2->name), "scripted_sound");
    o2->scale[0] = o2->scale[1] = o2->scale[2] = 1.0f;
    o2->alpha = 1.0f;
    o2->brightness = 1.0f;
    o2->color[0] = o2->color[1] = o2->color[2] = 1.0f;
    o2->visible = true;
    o2->is_sound_object = true;
    o2->sound_volume = 0.5f;
    o2->scripts[0].target = WE_SCRIPT_PROP_VOLUME;
    o2->scripts[0].vm = we_script_create(k_vol_js, strlen(k_vol_js), &sc.script_shared);
    o2->script_count = 1u;

    for (uint32_t oi = 0u; oi < 3u; ++oi) {
        for (uint32_t si = 0u; si < sc.objects[oi].script_count; ++si) {
            TEST_CHECK(sc.objects[oi].scripts[si].vm != NULL);
        }
    }

    char saved_loc[64];
    const char *comma_loc = set_comma_decimal_locale(saved_loc, sizeof(saved_loc));
    we_asset_writer_err_t err = WE_WRITER_OK;
    char *s_comma = NULL;
    size_t len_comma = 0u;
    TEST_CHECK(we_scene_serialize_json(&sc, &s_comma, &len_comma, &err));
    setlocale(LC_NUMERIC, saved_loc);

    char *s_c = NULL;
    size_t len_c = 0u;
    TEST_CHECK(we_scene_serialize_json(&sc, &s_c, &len_c, &err));
    TEST_CHECK(len_comma == len_c && memcmp(s_comma, s_c, len_c) == 0);
    TEST_CHECK(comma_loc == NULL || strstr(s_comma, "100,5") == NULL);

    char exp_dir[256];
    make_tmp_dir(exp_dir, sizeof(exp_dir), "scripts_rt");
    TEST_CHECK(we_scene_export_dir(&sc, exp_dir, false, &err));
    TEST_CHECK(err == WE_WRITER_OK);

    we_scene_t *reloaded = we_scene_load_cpu(exp_dir, NULL);
    TEST_CHECK(reloaded != NULL);
    TEST_CHECK(reloaded->object_count == 3u);
    TEST_CHECK(reloaded->objects[0].script_count == 5u);
    TEST_CHECK(reloaded->objects[0].base_pass.const_val_count == 2u);
    const we_const_val_t *cv_scroll = NULL;
    const we_const_val_t *cv_gain = NULL;
    for (uint32_t c = 0u; c < reloaded->objects[0].base_pass.const_val_count; ++c) {
        if (strcmp(reloaded->objects[0].base_pass.const_vals[c].name, "u_scroll") == 0) {
            cv_scroll = &reloaded->objects[0].base_pass.const_vals[c];
        } else if (strcmp(reloaded->objects[0].base_pass.const_vals[c].name, "u_gain") == 0) {
            cv_gain = &reloaded->objects[0].base_pass.const_vals[c];
        }
    }
    TEST_CHECK(cv_scroll != NULL && cv_scroll->script_src != NULL);
    TEST_CHECK(strcmp(cv_scroll->script_src, k_uni_src) == 0);
    TEST_CHECK(strcmp(cv_scroll->user_prop, "scrollprop") == 0);
    TEST_CHECK(cv_gain != NULL && cv_gain->script_src != NULL);
    /* Parser clears alpha_user_prop when attaching WE_SCRIPT_PROP_ALPHA, while serialized JSON
     * preserves both "script" and "user". */
    TEST_CHECK(strstr(s_c, "\"user\":\"opacity\"") != NULL);
    TEST_CHECK(reloaded->objects[1].script_count == 2u);
    TEST_CHECK(reloaded->objects[2].script_count == 1u);

    /* Re-export from reloaded scene preserves every script without silent loss. */
    char exp_dir2[256];
    make_tmp_dir(exp_dir2, sizeof(exp_dir2), "scripts_rt2");
    TEST_CHECK(we_scene_export_dir(reloaded, exp_dir2, true, &err));
    we_scene_t *reloaded2 = we_scene_load_cpu(exp_dir2, NULL);
    TEST_CHECK(reloaded2 != NULL);
    TEST_CHECK(reloaded2->objects[0].script_count == 5u);
    TEST_CHECK(reloaded2->objects[1].script_count == 2u);
    TEST_CHECK(reloaded2->objects[2].script_count == 1u);

    we_scene_destroy(reloaded2);
    we_scene_destroy(reloaded);
    rm_rf(exp_dir);
    rm_rf(exp_dir2);
    free(s_comma);
    free(s_c);

    for (uint32_t oi = 0u; oi < 3u; ++oi) {
        for (uint32_t si = 0u; si < sc.objects[oi].script_count; ++si) {
            we_script_destroy(sc.objects[oi].scripts[si].vm);
        }
    }
    free(sc.objects[0].base_pass.const_vals[0].script_src);
    free_synth_scene(&sc);
}

static void test_e2e_f01_particle_export_with_has_base_pass(void)
{
    we_particle_sys_t *ps0 = we_particle_create_preset(WE_PARTICLE_PRESET_EMBERS, false);
    we_particle_sys_t *ps1 = we_particle_create_preset(WE_PARTICLE_PRESET_FIREFLIES, false);
    TEST_CHECK(ps0 != NULL && ps1 != NULL);
    snprintf(ps0->material_path, sizeof(ps0->material_path), "materials/embers_mat.json");
    ps0->control_point_count = 1u;
    ps0->control_points[0].id = 0;
    ps0->control_points[0].offset[0] = 12.0f;
    ps0->control_points[0].offset[1] = -8.0f;
    ps0->control_points[0].lock_to_pointer = true;

    we_scene_object_t objs[2] = {0};
    objs[0].id = 20;
    objs[0].parent_id = -1;
    snprintf(objs[0].name, sizeof(objs[0].name), "EmbersLayer");
    /* Historically triggered material-overwrites-particle bug when has_base_pass == true. */
    snprintf(objs[0].image_path, sizeof(objs[0].image_path), "particles/embers.json");
    objs[0].scale[0] = objs[0].scale[1] = objs[0].scale[2] = 1.0f;
    objs[0].size[0] = 400.0f;
    objs[0].size[1] = 400.0f;
    objs[0].alpha = 1.0f;
    objs[0].brightness = 1.0f;
    objs[0].color[0] = objs[0].color[1] = objs[0].color[2] = 1.0f;
    objs[0].visible = true;
    objs[0].particle_sys = ps0;
    objs[0].has_base_pass = true;
    objs[0].base_pass.blending = WE_BLEND_ADDITIVE;
    snprintf(objs[0].base_pass.shader_name, sizeof(objs[0].base_pass.shader_name), "genericimage2");
    snprintf(objs[0].base_pass.tex_paths[0], sizeof(objs[0].base_pass.tex_paths[0]), "ember_dot");
    objs[0].base_pass.textures[0] = make_cpu_tex(8u, 8u, 220u);

    objs[1].id = 21;
    objs[1].parent_id = -1;
    snprintf(objs[1].name, sizeof(objs[1].name), "FirefliesLayer");
    objs[1].scale[0] = objs[1].scale[1] = objs[1].scale[2] = 1.0f;
    objs[1].size[0] = 300.0f;
    objs[1].size[1] = 300.0f;
    objs[1].alpha = 1.0f;
    objs[1].brightness = 1.0f;
    objs[1].color[0] = objs[1].color[1] = objs[1].color[2] = 1.0f;
    objs[1].visible = true;
    objs[1].particle_sys = ps1;
    objs[1].has_base_pass = true;
    objs[1].base_pass.blending = WE_BLEND_NORMAL;
    snprintf(objs[1].base_pass.shader_name, sizeof(objs[1].base_pass.shader_name), "genericimage2");
    snprintf(objs[1].base_pass.tex_paths[0], sizeof(objs[1].base_pass.tex_paths[0]), "firefly_dot");
    objs[1].base_pass.textures[0] = make_cpu_tex(8u, 8u, 150u);

    we_scene_t sc = {0};
    sc.ortho_width = 1920.0f;
    sc.ortho_height = 1080.0f;
    sc.object_count = 2u;
    sc.objects = objs;

    static const bool modes[2] = {false, true};
    for (int m = 0; m < 2; ++m) {
        char exp_dir[256];
        make_tmp_dir(exp_dir, sizeof(exp_dir), m ? "part_pkg" : "part_loose");
        we_asset_writer_err_t err = WE_WRITER_OK;
        TEST_CHECK(we_scene_export_dir(&sc, exp_dir, modes[m], &err));
        TEST_CHECK(err == WE_WRITER_OK);

        if (!modes[m]) {
            char p0_path[512], p1_path[512], mat0_path[512], s_path[512];
            snprintf(p0_path, sizeof(p0_path), "%s/particles/embers.json", exp_dir);
            snprintf(p1_path, sizeof(p1_path), "%s/particles/particle_21.json", exp_dir);
            snprintf(mat0_path, sizeof(mat0_path), "%s/materials/embers_mat.json", exp_dir);
            snprintf(s_path, sizeof(s_path), "%s/scene.json", exp_dir);
            TEST_CHECK(file_exists(p0_path));
            TEST_CHECK(file_exists(p1_path));
            TEST_CHECK(file_exists(mat0_path));
            TEST_CHECK(verify_json_file_is_strict_rfc8785(p0_path));
            TEST_CHECK(verify_json_file_is_strict_rfc8785(p1_path));
            TEST_CHECK(verify_json_file_is_strict_rfc8785(s_path));

            char *p0_txt = slurp_text_file(p0_path, NULL);
            TEST_CHECK(p0_txt != NULL);
            TEST_CHECK(strstr(p0_txt, "\"emitter\"") != NULL);
            TEST_CHECK(strstr(p0_txt, "\"initializer\"") != NULL);
            TEST_CHECK(strstr(p0_txt, "\"operator\"") != NULL);
            free(p0_txt);
        }

        we_scene_t *reloaded = we_scene_load_cpu(exp_dir, NULL);
        TEST_CHECK(reloaded != NULL);
        TEST_CHECK(reloaded->object_count == 2u);
        TEST_CHECK(reloaded->objects[0].particle_sys != NULL);
        TEST_CHECK(reloaded->objects[0].particle_sys->max_count == ps0->max_count);
        TEST_CHECK(reloaded->objects[0].particle_sys->emitter_count == ps0->emitter_count);
        TEST_CHECK(reloaded->objects[0].particle_sys->initializer_count == ps0->initializer_count);
        TEST_CHECK(reloaded->objects[0].particle_sys->operator_count == ps0->operator_count);
        TEST_CHECK(reloaded->objects[0].particle_sys->control_points[0].lock_to_pointer);
        TEST_CHECK(reloaded->objects[0].has_base_pass);
        TEST_CHECK(reloaded->objects[0].base_pass.blending == WE_BLEND_ADDITIVE);
        TEST_CHECK(reloaded->objects[0].base_pass.textures[0] != NULL);
        TEST_CHECK(reloaded->objects[1].particle_sys != NULL);
        TEST_CHECK(reloaded->objects[1].particle_sys->max_count == ps1->max_count);
        TEST_CHECK(reloaded->objects[1].particle_sys->emitter_count == ps1->emitter_count);
        we_particle_update(reloaded->objects[0].particle_sys, 0.016f);
        we_scene_destroy(reloaded);
        rm_rf(exp_dir);
    }

    we_tex_destroy(objs[0].base_pass.textures[0]);
    we_tex_destroy(objs[1].base_pass.textures[0]);
    we_particle_destroy(ps0);
    we_particle_destroy(ps1);
}

static void test_aud123_combo_shaders_and_large_pass_body(void)
{
    we_scene_object_t objs[2] = {0};
    for (int i = 0; i < 2; ++i) {
        objs[i].id = 30 + i;
        objs[i].parent_id = -1;
        snprintf(objs[i].name, sizeof(objs[i].name), "combo_%d", i);
        objs[i].scale[0] = objs[i].scale[1] = objs[i].scale[2] = 1.0f;
        objs[i].size[0] = 256.0f;
        objs[i].size[1] = 256.0f;
        objs[i].alpha = 1.0f;
        objs[i].brightness = 1.0f;
        objs[i].color[0] = objs[i].color[1] = objs[i].color[2] = 1.0f;
        objs[i].visible = true;
        objs[i].has_base_pass = true;
        snprintf(objs[i].base_pass.shader_name, sizeof(objs[i].base_pass.shader_name),
                 "genericimage2");
        objs[i].base_pass.combo_count = 1u;
        snprintf(objs[i].base_pass.combos[0].name, sizeof(objs[i].base_pass.combos[0].name),
                 "GRAYSCALE");
        objs[i].base_pass.combos[0].value = i;
        snprintf(objs[i].base_pass.tex_paths[0], sizeof(objs[i].base_pass.tex_paths[0]), "tex_%d",
                 i);
        objs[i].base_pass.textures[0] = make_cpu_tex(4u, 4u, (uint8_t)(40 + i * 30));

        /* Force pass->prep to hold already-preprocessed GLSL to verify raw source recovery. */
        const char *vr = we_shader_get_builtin("genericimage2.vert");
        const char *fr = we_shader_get_builtin("genericimage2.frag");
        TEST_CHECK(we_shader_preprocess_pair(vr, fr, NULL, objs[i].base_pass.combos, 1u, 0x1u,
                                             false, &objs[i].base_pass.prep));
    }

    /* Inject a >36 KiB pass constant shader script to exercise dynamic pass serialization >32 KiB.
     */
    size_t big_cap = 38u * 1024u;
    char *big_js = (char *)malloc(big_cap);
    TEST_CHECK(big_js != NULL);
    size_t pos = (size_t)snprintf(big_js, big_cap, "export function update(v) { var s = 0;");
    while (pos + 24u < big_cap - 48u) {
        pos += (size_t)snprintf(big_js + pos, big_cap - pos, "s += 0.001;");
    }
    snprintf(big_js + pos, big_cap - pos, "return v + s; }");
    objs[1].base_pass.const_val_count = 1u;
    snprintf(objs[1].base_pass.const_vals[0].name, sizeof(objs[1].base_pass.const_vals[0].name),
             "u_big");
    objs[1].base_pass.const_vals[0].num_components = 1u;
    objs[1].base_pass.const_vals[0].val[0] = 0.25f;
    objs[1].base_pass.const_vals[0].script_src = big_js;

    we_scene_t sc = {0};
    sc.ortho_width = 1920.0f;
    sc.ortho_height = 1080.0f;
    sc.object_count = 2u;
    sc.objects = objs;

    char exp_dir[256];
    make_tmp_dir(exp_dir, sizeof(exp_dir), "combo_sh");
    we_asset_writer_err_t err = WE_WRITER_OK;
    TEST_CHECK(we_scene_export_dir(&sc, exp_dir, false, &err));
    TEST_CHECK(err == WE_WRITER_OK);

    /* Object 0 keeps shaders/genericimage2.* with un-preprocessed raw source + GRAYSCALE=0. */
    char f0_path[512], mat1_path[512];
    snprintf(f0_path, sizeof(f0_path), "%s/shaders/genericimage2.frag", exp_dir);
    snprintf(mat1_path, sizeof(mat1_path), "%s/materials/layer_1.json", exp_dir);
    size_t f0_len = 0u, mat1_len = 0u;
    char *f0_txt = slurp_text_file(f0_path, &f0_len);
    char *mat1_txt = slurp_text_file(mat1_path, &mat1_len);
    TEST_CHECK(f0_txt != NULL && mat1_txt != NULL);
    TEST_CHECK(strstr(f0_txt, "#define GRAYSCALE 0") != NULL);
    TEST_CHECK(strncmp(f0_txt, "#version 310 es", 15u) != 0);
    TEST_CHECK(strstr(f0_txt, "varying ") != NULL);
    TEST_CHECK(mat1_len > 36u * 1024u);

    /* Object 1 gets a disambiguated shader containing GRAYSCALE=1 rather than being dropped. */
    we_jval_t *m1 = we_json_parse_root(mat1_txt, mat1_len);
    const we_jval_t *passes = m1 ? we_jobj_get(m1, "passes") : NULL;
    const char *sh1 = (passes && passes->type == JVAL_ARR && passes->arr.count > 0u)
                          ? we_jval_str(we_jobj_get(passes->arr.items[0], "shader"), "")
                          : "";
    TEST_CHECK(strncmp(sh1, "genericimage2__v_", 17u) == 0);
    char f1_path[512];
    snprintf(f1_path, sizeof(f1_path), "%s/shaders/%s.frag", exp_dir, sh1);
    char *f1_txt = slurp_text_file(f1_path, NULL);
    TEST_CHECK(f1_txt != NULL && strstr(f1_txt, "#define GRAYSCALE 1") != NULL);

    we_scene_t *reloaded = we_scene_load_cpu(exp_dir, NULL);
    TEST_CHECK(reloaded != NULL);
    TEST_CHECK(reloaded->object_count == 2u);
    TEST_CHECK(reloaded->objects[0].base_pass.combos[0].value == 0);
    TEST_CHECK(reloaded->objects[1].base_pass.combos[0].value == 1);
    TEST_CHECK(reloaded->objects[1].base_pass.const_vals[0].script_src != NULL);
    TEST_CHECK(strlen(reloaded->objects[1].base_pass.const_vals[0].script_src) > 35000u);

    we_scene_destroy(reloaded);
    we_jval_free(m1);
    free(f0_txt);
    free(f1_txt);
    free(mat1_txt);
    for (int i = 0; i < 2; ++i) {
        free(objs[i].base_pass.prep.vert_glsl);
        free(objs[i].base_pass.prep.frag_glsl);
        we_tex_destroy(objs[i].base_pass.textures[0]);
    }
    free(big_js);
    rm_rf(exp_dir);
}

typedef struct {
    int drm_fd;
    struct gbm_device *gbm;
    EGLDisplay dpy;
    EGLContext ctx;
} test_gles_t;

static bool init_test_gles(test_gles_t *out)
{
    memset(out, 0, sizeof(*out));
    out->drm_fd = -1;
    out->dpy = EGL_NO_DISPLAY;
    out->ctx = EGL_NO_CONTEXT;
    static const EGLint attribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1,
                                     EGL_NONE};
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_dpy =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");

    if (get_platform_dpy) {
        EGLDisplay dpy = get_platform_dpy(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        if (dpy != EGL_NO_DISPLAY) {
            EGLint maj = 0, min = 0;
            if (eglInitialize(dpy, &maj, &min) && eglBindAPI(EGL_OPENGL_ES_API)) {
                EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attribs);
                if (ctx != EGL_NO_CONTEXT &&
                    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
                    out->dpy = dpy;
                    out->ctx = ctx;
                    return true;
                }
                if (ctx != EGL_NO_CONTEXT) {
                    eglDestroyContext(dpy, ctx);
                }
            }
            eglTerminate(dpy);
        }
    }

    for (int idx = 128; idx < 136; ++idx) {
        char node_path[64];
        snprintf(node_path, sizeof(node_path), "/dev/dri/renderD%d", idx);
        int fd = open(node_path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        struct gbm_device *gbm = gbm_create_device(fd);
        if (!gbm) {
            close(fd);
            continue;
        }
        EGLDisplay dpy = get_platform_dpy ? get_platform_dpy(EGL_PLATFORM_GBM_KHR,
                                                             (EGLNativeDisplayType)gbm, NULL)
                                          : eglGetDisplay((EGLNativeDisplayType)gbm);
        if (dpy != EGL_NO_DISPLAY) {
            EGLint maj = 0, min = 0;
            if (eglInitialize(dpy, &maj, &min) && eglBindAPI(EGL_OPENGL_ES_API)) {
                EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attribs);
                if (ctx != EGL_NO_CONTEXT &&
                    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
                    out->drm_fd = fd;
                    out->gbm = gbm;
                    out->dpy = dpy;
                    out->ctx = ctx;
                    return true;
                }
                if (ctx != EGL_NO_CONTEXT) {
                    eglDestroyContext(dpy, ctx);
                }
            }
            eglTerminate(dpy);
        }
        gbm_device_destroy(gbm);
        close(fd);
    }
    return false;
}

static void destroy_test_gles(test_gles_t *g)
{
    if (g->dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g->ctx != EGL_NO_CONTEXT) {
            eglDestroyContext(g->dpy, g->ctx);
        }
        eglTerminate(g->dpy);
    }
    if (g->gbm) {
        gbm_device_destroy(g->gbm);
    }
    if (g->drm_fd >= 0) {
        close(g->drm_fd);
    }
}

static void test_aud025_aud123_vram_and_compressed_texture_export(void)
{
    we_asset_writer_err_t err = WE_WRITER_OK;

    /* 1. Compressed DXT5 CPU texture with rgba8 == NULL and pkg == NULL */
    {
        we_tex_t *dxt = (we_tex_t *)calloc(1u, sizeof(*dxt));
        TEST_CHECK(dxt != NULL);
        dxt->texv_version = 5u;
        dxt->texi_version = 1u;
        dxt->texb_version = 3u;
        dxt->format = WE_TEX_FMT_DXT5;
        dxt->texture_width = 4u;
        dxt->texture_height = 4u;
        dxt->image_width = 4u;
        dxt->image_height = 4u;
        dxt->image_count = 1u;
        dxt->free_image_format = WE_FIF_UNKNOWN;
        dxt->images = (we_tex_image_t *)calloc(1u, sizeof(we_tex_image_t));
        dxt->images[0].mip_count = 1u;
        dxt->images[0].mips = (we_tex_mip_t *)calloc(1u, sizeof(we_tex_mip_t));
        uint8_t *blk = (uint8_t *)calloc(16u, 1u);
        blk[0] = 0xFFu;
        blk[1] = 0xFFu;
        blk[8] = 0x00u;
        blk[9] = 0xF8u; /* RGB565 red */
        blk[10] = 0x00u;
        blk[11] = 0xF8u;
        dxt->images[0].mips[0].width = 4u;
        dxt->images[0].mips[0].height = 4u;
        dxt->images[0].mips[0].data = blk;
        dxt->images[0].mips[0].data_size = 16u;
        dxt->images[0].mips[0].byte_count = 16u;
        dxt->images[0].mips[0].decomp_size = 16u;

        we_scene_object_t obj = {0};
        obj.id = 40;
        obj.parent_id = -1;
        obj.visible = true;
        obj.alpha = 1.0f;
        obj.brightness = 1.0f;
        obj.scale[0] = obj.scale[1] = obj.scale[2] = 1.0f;
        obj.size[0] = 4.0f;
        obj.size[1] = 4.0f;
        obj.color[0] = obj.color[1] = obj.color[2] = 1.0f;
        obj.has_base_pass = true;
        snprintf(obj.base_pass.shader_name, sizeof(obj.base_pass.shader_name), "genericimage2");
        snprintf(obj.base_pass.tex_paths[0], sizeof(obj.base_pass.tex_paths[0]), "comp_dxt5");
        obj.base_pass.textures[0] = dxt;

        we_scene_t sc = {0};
        sc.ortho_width = 64.0f;
        sc.ortho_height = 64.0f;
        sc.object_count = 1u;
        sc.objects = &obj;

        char exp_dir[256];
        make_tmp_dir(exp_dir, sizeof(exp_dir), "dxt5_nopkg");
        TEST_CHECK(we_scene_export_dir(&sc, exp_dir, false, &err));
        TEST_CHECK(err == WE_WRITER_OK);

        char tpath[512];
        snprintf(tpath, sizeof(tpath), "%s/materials/comp_dxt5.tex", exp_dir);
        TEST_CHECK(file_exists(tpath));
        size_t tsz = 0u;
        char *tbytes = slurp_text_file(tpath, &tsz);
        we_tex_t *parsed = we_tex_parse_cpu((const uint8_t *)tbytes, tsz);
        TEST_CHECK(parsed != NULL);
        uint8_t *rgba = NULL;
        uint32_t rw = 0u, rh = 0u;
        TEST_CHECK(we_tex_decode_rgba8(parsed, &rgba, &rw, &rh));
        TEST_CHECK(rw == 4u && rh == 4u && rgba[0] > 240u && rgba[3] == 255u);
        free(rgba);
        we_tex_destroy(parsed);
        free(tbytes);
        we_tex_destroy(dxt);
        rm_rf(exp_dir);
    }

    /* 2. Non-.tex raw PNG in scene->pkg must be transcoded to valid .tex (never raw PNG bytes),
     *    and corrupt non-image bytes must fail explicitly. */
    {
        char tmp_png_dir[256];
        make_tmp_dir(tmp_png_dir, sizeof(tmp_png_dir), "png_pkg");
        char png_path[512];
        snprintf(png_path, sizeof(png_path), "%s/tiny.png", tmp_png_dir);
        static const uint8_t k_px[16] = {
            10, 200, 30, 255, 10, 200, 30, 255, 10, 200, 30, 255, 10, 200, 30, 255,
        };
        TEST_CHECK(image_save_png_rgba(png_path, k_px, 2u, 2u));
        size_t png_sz = 0u;
        char *png_bytes = slurp_text_file(png_path, &png_sz);
        TEST_CHECK(png_bytes != NULL && png_sz > 8u);

        we_pkg_write_entry_t entry = {
            .path = "photo.png", .data = (const uint8_t *)png_bytes, .size = png_sz};
        uint8_t *pkg_buf = NULL;
        size_t pkg_sz = 0u;
        TEST_CHECK(we_pkg_write_mem(&entry, 1u, &pkg_buf, &pkg_sz, &err));
        we_pkg_t *pkg = we_pkg_open_mem(pkg_buf, pkg_sz, NULL);
        TEST_CHECK(pkg != NULL);

        we_scene_object_t obj = {0};
        obj.id = 41;
        obj.parent_id = -1;
        obj.visible = true;
        obj.alpha = 1.0f;
        obj.brightness = 1.0f;
        obj.scale[0] = obj.scale[1] = obj.scale[2] = 1.0f;
        obj.size[0] = 2.0f;
        obj.size[1] = 2.0f;
        obj.color[0] = obj.color[1] = obj.color[2] = 1.0f;
        obj.has_base_pass = true;
        snprintf(obj.base_pass.shader_name, sizeof(obj.base_pass.shader_name), "genericimage2");
        snprintf(obj.base_pass.tex_paths[0], sizeof(obj.base_pass.tex_paths[0]), "photo.png");

        we_scene_t sc = {0};
        sc.pkg = pkg;
        sc.ortho_width = 64.0f;
        sc.ortho_height = 64.0f;
        sc.object_count = 1u;
        sc.objects = &obj;

        char exp_dir[256];
        make_tmp_dir(exp_dir, sizeof(exp_dir), "png_exp");
        TEST_CHECK(we_scene_export_dir(&sc, exp_dir, false, &err));
        char out_tex[512];
        snprintf(out_tex, sizeof(out_tex), "%s/materials/photo.png.tex", exp_dir);
        size_t otsz = 0u;
        char *otbytes = slurp_text_file(out_tex, &otsz);
        TEST_CHECK(otbytes != NULL && otsz >= 8u && memcmp(otbytes, "TEXV", 4u) == 0);
        free(otbytes);
        we_pkg_close(pkg);
        free(pkg_buf);
        free(png_bytes);
        rm_rf(exp_dir);
        rm_rf(tmp_png_dir);

        /* Corrupt non-image payload under bad.png must fail with WE_WRITER_ERR_UNSUPPORTED_FMT. */
        static const uint8_t k_junk[8] = {'N', 'O', 'T', 'I', 'M', 'G', '!', '\0'};
        we_pkg_write_entry_t bad_entry = {
            .path = "bad.png", .data = k_junk, .size = sizeof(k_junk)};
        uint8_t *bad_buf = NULL;
        size_t bad_sz = 0u;
        TEST_CHECK(we_pkg_write_mem(&bad_entry, 1u, &bad_buf, &bad_sz, &err));
        we_pkg_t *bad_pkg = we_pkg_open_mem(bad_buf, bad_sz, NULL);
        TEST_CHECK(bad_pkg != NULL);
        sc.pkg = bad_pkg;
        snprintf(obj.base_pass.tex_paths[0], sizeof(obj.base_pass.tex_paths[0]), "bad.png");
        make_tmp_dir(exp_dir, sizeof(exp_dir), "bad_png_exp");
        TEST_CHECK(!we_scene_export_dir(&sc, exp_dir, false, &err));
        TEST_CHECK(err == WE_WRITER_ERR_UNSUPPORTED_FMT);
        we_pkg_close(bad_pkg);
        free(bad_buf);
        rm_rf(exp_dir);
    }

    /* 3. VRAM GPU-painted texture readback (after we_tex_upload_gl frees CPU mips) */
    test_gles_t gles;
    if (init_test_gles(&gles)) {
        we_tex_t *gpu_tex = make_cpu_tex(8u, 8u, 0u);
        TEST_CHECK(gpu_tex != NULL);
        TEST_CHECK(we_tex_upload_gl(gpu_tex));
        TEST_CHECK(gpu_tex->gl_tex != 0u);
        TEST_CHECK(gpu_tex->images[0].mips[0].data == NULL);
        TEST_CHECK(gpu_tex->images[0].mips[0].rgba8 == NULL);

        /* Simulate GPU brush drawing into tex->gl_tex in VRAM without touching CPU memory. */
        GLuint fbo = 0u;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               (GLuint)gpu_tex->gl_tex, 0);
        TEST_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        glClearColor(42.0f / 255.0f, 173.0f / 255.0f, 219.0f / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);

        /* Release current context so export_texture_from_gl also verifies scene EGL binding. */
        eglMakeCurrent(gles.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        we_scene_object_t obj = {0};
        obj.id = 42;
        obj.parent_id = -1;
        obj.visible = true;
        obj.alpha = 1.0f;
        obj.brightness = 1.0f;
        obj.scale[0] = obj.scale[1] = obj.scale[2] = 1.0f;
        obj.size[0] = 8.0f;
        obj.size[1] = 8.0f;
        obj.color[0] = obj.color[1] = obj.color[2] = 1.0f;
        obj.has_base_pass = true;
        snprintf(obj.base_pass.shader_name, sizeof(obj.base_pass.shader_name), "genericimage2");
        snprintf(obj.base_pass.tex_paths[0], sizeof(obj.base_pass.tex_paths[0]), "painted");
        obj.base_pass.textures[0] = gpu_tex;

        we_scene_t sc = {0};
        sc.gl_initialized = true;
        sc.egl_display = gles.dpy;
        sc.egl_context = gles.ctx;
        sc.ortho_width = 64.0f;
        sc.ortho_height = 64.0f;
        sc.object_count = 1u;
        sc.objects = &obj;

        char exp_dir[256];
        make_tmp_dir(exp_dir, sizeof(exp_dir), "vram_tex");
        TEST_CHECK(we_scene_export_dir(&sc, exp_dir, false, &err));
        TEST_CHECK(err == WE_WRITER_OK);

        char tpath[512];
        snprintf(tpath, sizeof(tpath), "%s/materials/painted.tex", exp_dir);
        TEST_CHECK(file_exists(tpath));
        size_t tsz = 0u;
        char *tbytes = slurp_text_file(tpath, &tsz);
        we_tex_t *parsed = we_tex_parse_cpu((const uint8_t *)tbytes, tsz);
        TEST_CHECK(parsed != NULL);
        uint8_t *rgba = NULL;
        uint32_t rw = 0u, rh = 0u;
        TEST_CHECK(we_tex_decode_rgba8(parsed, &rgba, &rw, &rh));
        TEST_CHECK(rw == 8u && rh == 8u);
        TEST_CHECK(abs((int)rgba[0] - 42) <= 1 && abs((int)rgba[1] - 173) <= 1 &&
                   abs((int)rgba[2] - 219) <= 1 && rgba[3] == 255u);
        free(rgba);
        we_tex_destroy(parsed);
        free(tbytes);

        TEST_CHECK(eglMakeCurrent(gles.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, gles.ctx));
        GLuint gt = (GLuint)gpu_tex->gl_tex;
        glDeleteTextures(1, &gt);
        gpu_tex->gl_tex = 0u;
        we_tex_destroy(gpu_tex);
        rm_rf(exp_dir);
        destroy_test_gles(&gles);
    } else {
        /* Without GL context or pkg, freed mips with nonzero gl_tex must fail instead of silent
         * true. */
        we_tex_t missing_gl = {0};
        missing_gl.gl_tex = 999u;
        missing_gl.texture_width = 4u;
        missing_gl.texture_height = 4u;
        we_scene_object_t obj = {0};
        obj.id = 43;
        obj.parent_id = -1;
        obj.visible = true;
        obj.alpha = 1.0f;
        obj.brightness = 1.0f;
        obj.scale[0] = obj.scale[1] = obj.scale[2] = 1.0f;
        obj.color[0] = obj.color[1] = obj.color[2] = 1.0f;
        obj.has_base_pass = true;
        snprintf(obj.base_pass.shader_name, sizeof(obj.base_pass.shader_name), "genericimage2");
        snprintf(obj.base_pass.tex_paths[0], sizeof(obj.base_pass.tex_paths[0]), "painted");
        obj.base_pass.textures[0] = &missing_gl;

        we_scene_t sc = {0};
        sc.ortho_width = 64.0f;
        sc.ortho_height = 64.0f;
        sc.object_count = 1u;
        sc.objects = &obj;

        char exp_dir[256];
        make_tmp_dir(exp_dir, sizeof(exp_dir), "vram_nogl");
        TEST_CHECK(!we_scene_export_dir(&sc, exp_dir, false, &err));
        TEST_CHECK(err != WE_WRITER_OK);
        rm_rf(exp_dir);
    }
}

static we_puppet_t *make_test_puppet(const char *mat_json_file)
{
    we_puppet_t *p = (we_puppet_t *)calloc(1u, sizeof(*p));
    if (!p) {
        return NULL;
    }
    p->mdlv_version = 23u;
    p->mdls_version = 2u;
    snprintf(p->mat_json_file, sizeof(p->mat_json_file), "%s", mat_json_file);
    p->vertex_count = 3u;
    p->vertices = (we_mdl_vertex_t *)calloc(3u, sizeof(we_mdl_vertex_t));
    p->vertices[0] = (we_mdl_vertex_t){
        .pos = {0.0f, 0.0f, 0.0f}, .weight = {1.0f, 0.0f, 0.0f, 0.0f}, .texcoord = {0.0f, 0.0f}};
    p->vertices[1] = (we_mdl_vertex_t){
        .pos = {100.0f, 0.0f, 0.0f}, .weight = {1.0f, 0.0f, 0.0f, 0.0f}, .texcoord = {1.0f, 0.0f}};
    p->vertices[2] = (we_mdl_vertex_t){
        .pos = {0.0f, 100.0f, 0.0f}, .weight = {1.0f, 0.0f, 0.0f, 0.0f}, .texcoord = {0.0f, 1.0f}};
    p->triangle_count = 1u;
    p->index_count = 3u;
    p->indices = (uint16_t *)calloc(3u, sizeof(uint16_t));
    p->indices[0] = 0u;
    p->indices[1] = 1u;
    p->indices[2] = 2u;
    p->bone_count = 1u;
    p->bones = (we_mdl_bone_t *)calloc(1u, sizeof(we_mdl_bone_t));
    snprintf(p->bones[0].name, sizeof(p->bones[0].name), "root");
    p->bones[0].parent = 0xFFFFFFFFu;
    p->bones[0].local_trans[0] = 1.0f;
    p->bones[0].local_trans[5] = 1.0f;
    p->bones[0].local_trans[10] = 1.0f;
    p->bones[0].local_trans[15] = 1.0f;
    memcpy(p->bones[0].world_bind, p->bones[0].local_trans, sizeof(p->bones[0].world_bind));
    memcpy(p->bones[0].offset_trans, p->bones[0].local_trans, sizeof(p->bones[0].offset_trans));
    p->bones[0].bind_scale[0] = p->bones[0].bind_scale[1] = p->bones[0].bind_scale[2] = 1.0f;
    p->max_pos[0] = 100.0f;
    p->max_pos[1] = 100.0f;
    return p;
}

static void free_test_puppet(we_puppet_t *p)
{
    if (!p) {
        return;
    }
    free(p->vertices);
    free(p->indices);
    free(p->bones);
    free(p);
}

static void test_aud303_aud409_rfc8785_puppets_and_64mib_preview(void)
{
    we_asset_writer_err_t err = WE_WRITER_OK;

    /* 1. Puppet export preserves shared mat_json_file overrides & emits no orphan models JSON */
    {
        we_puppet_t *p0 = make_test_puppet("materials/shared_pup.json");
        we_puppet_t *p1 = make_test_puppet("materials/shared_pup.json");
        TEST_CHECK(p0 && p1);

        we_scene_object_t objs[2] = {0};
        for (int i = 0; i < 2; ++i) {
            objs[i].id = 50 + i;
            objs[i].parent_id = -1;
            snprintf(objs[i].name, sizeof(objs[i].name), "puppet_%d", i);
            objs[i].scale[0] = objs[i].scale[1] = objs[i].scale[2] = 1.0f;
            objs[i].size[0] = 100.0f;
            objs[i].size[1] = 100.0f;
            objs[i].alpha = 1.0f;
            objs[i].brightness = 1.0f;
            objs[i].color[0] = objs[i].color[1] = objs[i].color[2] = 1.0f;
            objs[i].visible = true;
            objs[i].puppet = (i == 0) ? p0 : p1;
            objs[i].has_base_pass = true;
            snprintf(objs[i].base_pass.shader_name, sizeof(objs[i].base_pass.shader_name),
                     "genericimage2");
            objs[i].base_pass.blending = (i == 0) ? WE_BLEND_ADDITIVE : WE_BLEND_MULTIPLY;
            snprintf(objs[i].base_pass.tex_paths[0], sizeof(objs[i].base_pass.tex_paths[0]),
                     "pup_%d", i);
            objs[i].base_pass.textures[0] = make_cpu_tex(4u, 4u, (uint8_t)(20 + i * 80));
        }

        we_scene_t sc = {0};
        sc.ortho_width = 1920.0f;
        sc.ortho_height = 1080.0f;
        sc.object_count = 2u;
        sc.objects = objs;

        char exp_dir[256];
        make_tmp_dir(exp_dir, sizeof(exp_dir), "puppet_shared");
        TEST_CHECK(we_scene_export_dir(&sc, exp_dir, false, &err));
        TEST_CHECK(err == WE_WRITER_OK);

        char orphan0[512], orphan1[512], rig0[512], rig1[512], scene_path[512], proj_path[512];
        snprintf(orphan0, sizeof(orphan0), "%s/models/puppet_0_50.json", exp_dir);
        snprintf(orphan1, sizeof(orphan1), "%s/models/puppet_1_51.json", exp_dir);
        snprintf(rig0, sizeof(rig0), "%s/studio_rig_50.json", exp_dir);
        snprintf(rig1, sizeof(rig1), "%s/studio_rig_51.json", exp_dir);
        snprintf(scene_path, sizeof(scene_path), "%s/scene.json", exp_dir);
        snprintf(proj_path, sizeof(proj_path), "%s/project.json", exp_dir);
        TEST_CHECK(!file_exists(orphan0));
        TEST_CHECK(!file_exists(orphan1));
        TEST_CHECK(verify_json_file_is_strict_rfc8785(rig0));
        TEST_CHECK(verify_json_file_is_strict_rfc8785(rig1));
        TEST_CHECK(verify_json_file_is_strict_rfc8785(scene_path));
        TEST_CHECK(verify_json_file_is_strict_rfc8785(proj_path));

        we_scene_t *reloaded = we_scene_load_cpu(exp_dir, NULL);
        TEST_CHECK(reloaded != NULL && reloaded->object_count == 2u);
        TEST_CHECK(reloaded->objects[0].base_pass.blending == WE_BLEND_ADDITIVE);
        TEST_CHECK(reloaded->objects[1].base_pass.blending == WE_BLEND_MULTIPLY);
        TEST_CHECK(reloaded->objects[0].puppet != NULL && reloaded->objects[1].puppet != NULL);
        TEST_CHECK(strcmp(reloaded->objects[0].puppet->mat_json_file,
                          reloaded->objects[1].puppet->mat_json_file) != 0);
        we_scene_destroy(reloaded);
        rm_rf(exp_dir);
        we_tex_destroy(objs[0].base_pass.textures[0]);
        we_tex_destroy(objs[1].base_pass.textures[0]);
        free_test_puppet(p0);
        free_test_puppet(p1);
    }

    /* 2. Preview > 64 MiB fails explicitly with WE_WRITER_ERR_LIMIT_EXCEEDED (no silent skip) */
    {
        char ws_dir[256], exp_dir[256];
        make_tmp_dir(ws_dir, sizeof(ws_dir), "prev_65m_ws");
        make_tmp_dir(exp_dir, sizeof(exp_dir), "prev_65m_exp");
        char proj_path[512];
        snprintf(proj_path, sizeof(proj_path), "%s/project.json", ws_dir);
        FILE *pf = fopen(proj_path, "wb");
        TEST_CHECK(pf != NULL);
        fputs("{\"file\":\"scene.json\",\"preview\":\"preview.jpg\",\"type\":\"scene\"}", pf);
        fclose(pf);
        char prev_path[512];
        snprintf(prev_path, sizeof(prev_path), "%s/preview.jpg", ws_dir);
        int fd = open(prev_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        TEST_CHECK(fd >= 0);
        TEST_CHECK(ftruncate(fd, (off_t)(65u * 1024u * 1024u)) == 0);
        close(fd);

        we_scene_t sc = {0};
        sc.ortho_width = 1920.0f;
        sc.ortho_height = 1080.0f;
        snprintf(sc.project.workshop_dir, sizeof(sc.project.workshop_dir), "%s", ws_dir);
        TEST_CHECK(!we_scene_export_dir(&sc, exp_dir, false, &err));
        TEST_CHECK(err == WE_WRITER_ERR_LIMIT_EXCEEDED);
        rm_rf(ws_dir);
        rm_rf(exp_dir);
    }
}

int main(void)
{
    RUN_TEST(test_lz4_bounds_and_invalid);
    RUN_TEST(test_lz4_roundtrip_patterns);
    RUN_TEST(test_lz4_4k_rgba8_benchmark);
    RUN_TEST(test_pkg_write_roundtrip_and_determinism);
    RUN_TEST(test_pkg_rejections);
    RUN_TEST(test_tex_formats_mips_and_spritesheet);
    RUN_TEST(test_tex_rejections);
    RUN_TEST(test_scene_project_jcs_and_export_roundtrip);
    RUN_TEST(test_normalize_rel_path_and_atomic_write);
    RUN_TEST(test_scene_export_security_and_edge_cases);
    RUN_TEST(test_live_shader_export_roundtrip);
    RUN_TEST(test_we_script_template_lint_and_abort);
    RUN_TEST(test_scene_destroy_callback_and_revision);
    RUN_TEST(test_audio_binding_clamps_and_cooldown);
    RUN_TEST(test_e2e_f01_scripts_and_script_src_roundtrip);
    RUN_TEST(test_e2e_f01_particle_export_with_has_base_pass);
    RUN_TEST(test_aud123_combo_shaders_and_large_pass_body);
    RUN_TEST(test_aud025_aud123_vram_and_compressed_texture_export);
    RUN_TEST(test_aud303_aud409_rfc8785_puppets_and_64mib_preview);

    printf("SUMMARY: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
