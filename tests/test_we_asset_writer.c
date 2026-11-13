// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "waywal/we_asset_writer.h"
#include "waywal/we_particle.h"
#include "waywal/we_pkg.h"
#include "waywal/we_project.h"
#include "waywal/we_scene.h"
#include "waywal/we_script.h"
#include "waywal/we_shader.h"
#include "waywal/we_tex.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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

    printf("SUMMARY: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
