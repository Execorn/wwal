// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "waywal/ipc_proto_authoring.h"
#include "waywal/we_appworkshop.h"
#include "waywal/we_asset_writer.h"
#include "waywal/we_compat_scanner.h"
#include "waywal/we_pkg.h"
#include "waywal/we_script.h"
#include "waywal/we_tex.h"
#include "waywal/we_vdf.h"
#include "waywal/wwpkg.h"

extern wwpkg_package_t *fuzz_wwpkg_open_memory_sym(const uint8_t *data,
                                                   size_t len) __asm__("wwpkg_open_memory")
    __attribute__((weak));
extern char *fuzz_wwpkg_jcs_canonicalize_sym(const char *json_src, size_t len,
                                             size_t *out_len) __asm__("wwpkg_jcs_canonicalize")
    __attribute__((weak));

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
__attribute__((visibility("default"))) const char *__asan_default_options(void);
__attribute__((visibility("default"))) const char *__asan_default_options(void)
{
    return "allocator_may_return_null=1:detect_leaks=1";
}
#endif

static we_compat_scanner_t *g_scanner = NULL;
static we_compat_report_t *g_report = NULL;
static char g_shm_dir[128] = {0};
static int g_shm_dfd = -1;

static void cleanup_fuzz_state(void)
{
    if (g_report) {
        we_compat_report_free(g_report);
        g_report = NULL;
    }
    if (g_scanner) {
        we_compat_scanner_destroy(g_scanner);
        g_scanner = NULL;
    }
    if (g_shm_dfd >= 0) {
        close(g_shm_dfd);
        g_shm_dfd = -1;
    }
    if (g_shm_dir[0] != '\0') {
        (void)rmdir(g_shm_dir);
        g_shm_dir[0] = '\0';
    }
}

static bool ensure_fuzz_init(void)
{
    if (!g_scanner) {
        g_scanner = we_compat_scanner_create(true, true);
    }
    if (!g_report) {
        g_report = we_compat_report_alloc();
    }
    if (g_shm_dfd < 0) {
        snprintf(g_shm_dir, sizeof(g_shm_dir), "/dev/shm/wwal_fuzz_XXXXXX");
        if (!mkdtemp(g_shm_dir)) {
            snprintf(g_shm_dir, sizeof(g_shm_dir), "/tmp/wwal_fuzz_XXXXXX");
            if (!mkdtemp(g_shm_dir)) {
                g_shm_dir[0] = '\0';
                return false;
            }
        }
        g_shm_dfd = open(g_shm_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (g_shm_dfd < 0) {
            (void)rmdir(g_shm_dir);
            g_shm_dir[0] = '\0';
            return false;
        }
        (void)atexit(cleanup_fuzz_state);
    }
    return g_scanner != NULL && g_report != NULL && g_shm_dfd >= 0;
}

static bool write_excl_at(int dfd, const char *name, const uint8_t *buf, size_t len)
{
    int fd = openat(dfd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n <= 0) {
            break;
        }
        off += (size_t)n;
    }
    close(fd);
    return off == len;
}

static void exercise_wwpkg_handle(wwpkg_package_t *pkg)
{
    if (!pkg) {
        return;
    }
    (void)wwpkg_get_manifest(pkg);
    (void)wwpkg_get_signature(pkg);
    bool is_trusted = false;
    (void)wwpkg_verify_signature(pkg, &is_trusted);
    size_t asset_sz = 0;
    (void)wwpkg_read_asset(pkg, "scene.json", &asset_sz);
    wwpkg_close(pkg);
}

static void fuzz_wwpkg_in_memory(const uint8_t *data, size_t size)
{
    if (fuzz_wwpkg_open_memory_sym) {
        wwpkg_package_t *pkg = fuzz_wwpkg_open_memory_sym(data, size);
        exercise_wwpkg_handle(pkg);
        return;
    }

    int mfd = memfd_create("wwpkg_fuzz_mem", MFD_CLOEXEC);
    if (mfd < 0) {
        return;
    }
    size_t off = 0;
    while (off < size) {
        ssize_t n = write(mfd, data + off, size - off);
        if (n <= 0) {
            break;
        }
        off += (size_t)n;
    }
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", mfd);
    wwpkg_package_t *pkg = wwpkg_open(proc_path);
    exercise_wwpkg_handle(pkg);
    close(mfd);
}

static void fuzz_jcs_canonicalizer(const uint8_t *data, size_t size)
{
    size_t out_len = 0;
    char *canon = wwpkg_canonicalize_json((const char *)data, size, &out_len);
    free(canon);

    if (fuzz_wwpkg_jcs_canonicalize_sym &&
        fuzz_wwpkg_jcs_canonicalize_sym != wwpkg_canonicalize_json) {
        size_t alt_len = 0;
        char *alt = fuzz_wwpkg_jcs_canonicalize_sym((const char *)data, size, &alt_len);
        free(alt);
    }
}

static void fuzz_loose_directory(const uint8_t *data, size_t size)
{
    if (mkdirat(g_shm_dfd, "loose", 0700) != 0) {
        return;
    }
    int ldfd = openat(g_shm_dfd, "loose", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (ldfd < 0) {
        (void)unlinkat(g_shm_dfd, "loose", AT_REMOVEDIR);
        return;
    }

    static const char *const LOOSE_NAMES[] = {
        "project.json", "scene.json", "effect.vert", "effect.frag",
        "logic.js",     ".hidden.js", "abcd",
    };
    const size_t nfiles = sizeof(LOOSE_NAMES) / sizeof(LOOSE_NAMES[0]);
    size_t chunk = size / nfiles;
    size_t cur = 0;

    for (size_t i = 0; i < nfiles; i++) {
        size_t flen = (i + 1u == nfiles) ? (size - cur) : chunk;
        (void)write_excl_at(ldfd, LOOSE_NAMES[i], data + cur, flen);
        cur += flen;
    }

    char loose_path[256];
    snprintf(loose_path, sizeof(loose_path), "%s/loose", g_shm_dir);
    if (we_compat_scan_directory(g_scanner, loose_path, g_report) && g_report->repair_count > 0u) {
        (void)we_compat_apply_repairs(g_report, loose_path);
    }

    for (size_t i = 0; i < nfiles; i++) {
        (void)unlinkat(ldfd, LOOSE_NAMES[i], 0);
    }
    (void)unlinkat(ldfd, ".waywal_compat.json", 0);
    close(ldfd);
    (void)unlinkat(g_shm_dfd, "loose", AT_REMOVEDIR);
}

static void fuzz_lz4_and_pkg_writer(const uint8_t *data, size_t size)
{
    uint8_t lz4_dst[4096];
    (void)we_lz4_decompress_safe(data, size, lz4_dst, sizeof(lz4_dst));
    size_t small_cap = (size_t)(data[0] & 0x3Fu);
    (void)we_lz4_decompress_safe(data, size, lz4_dst, small_cap);

    size_t comp_in_len = size < 2048u ? size : 2048u;
    size_t bound = we_lz4_compress_bound(comp_in_len);
    if (bound > 0u && bound <= sizeof(lz4_dst)) {
        size_t clen = we_lz4_compress_block(data, comp_in_len, lz4_dst, bound);
        if (clen > 0u) {
            uint8_t roundtrip[2048];
            int dlen = we_lz4_decompress_safe(lz4_dst, clen, roundtrip, comp_in_len);
            if (dlen != (int)comp_in_len || memcmp(roundtrip, data, comp_in_len) != 0) {
                abort();
            }
        }
    }

    char path0[192];
    char path1[192];
    size_t p0_len = size < (sizeof(path0) - 1u) ? size : (sizeof(path0) - 1u);
    memcpy(path0, data, p0_len);
    path0[p0_len] = '\0';

    size_t half = size / 2u;
    size_t p1_len = half < (sizeof(path1) - 1u) ? half : (sizeof(path1) - 1u);
    memcpy(path1, data + (size - half), p1_len);
    path1[p1_len] = '\0';

    size_t payload_sz = size < 1024u ? size : 1024u;
    we_pkg_write_entry_t entries[2] = {
        {.path = path0, .data = data, .size = payload_sz},
        {.path = (data[0] & 1u) ? path1 : "scene.json", .data = data, .size = payload_sz / 2u},
    };
    we_asset_writer_err_t err = WE_WRITER_OK;
    uint8_t *pkg_buf = NULL;
    size_t pkg_sz = 0u;
    if (we_pkg_write_mem(&entries[0], 1u, &pkg_buf, &pkg_sz, &err)) {
        we_pkg_t *opened = we_pkg_open_mem(pkg_buf, pkg_sz, NULL);
        if (opened) {
            we_pkg_close(opened);
        }
        free(pkg_buf);
    }

    pkg_buf = NULL;
    pkg_sz = 0u;
    if (we_pkg_write_mem(entries, 2u, &pkg_buf, &pkg_sz, &err)) {
        we_pkg_t *opened = we_pkg_open_mem(pkg_buf, pkg_sz, NULL);
        if (opened) {
            we_pkg_close(opened);
        }
        free(pkg_buf);
    }
}

static void fuzz_tex_encoder_and_script_lint(const uint8_t *data, size_t size)
{
    static const we_tex_format_t FMTS[4] = {
        WE_TEX_FMT_RGBA8888,
        WE_TEX_FMT_RG88,
        WE_TEX_FMT_R8,
        WE_TEX_FMT_DXT5,
    };
    we_tex_format_t fmt = FMTS[data[0] & 3u];
    uint32_t bpp = (fmt == WE_TEX_FMT_RGBA8888) ? 4u : ((fmt == WE_TEX_FMT_RG88) ? 2u : 1u);
    uint32_t w = (uint32_t)((data[0] & 7u) + 1u);
    uint32_t h = (uint32_t)(((size > 1u ? data[1] : data[0]) & 7u) + 1u);

    uint8_t px[256];
    for (size_t i = 0; i < sizeof(px); ++i) {
        px[i] = data[i % size];
    }

    we_tex_encode_mip_t mip0 = {
        .width = w,
        .height = h,
        .pixels = px,
        .byte_size = (size_t)w * (size_t)h * (size_t)bpp,
    };
    we_tex_encode_image_t img0 = {
        .mip_count = 1u,
        .mips = &mip0,
    };
    we_tex_frame_t frames[2] = {
        {.image_id = (data[0] & 0x40u) ? -1 : 0,
         .frametime = 0.05f,
         .x = 0.0f,
         .y = 0.0f,
         .width = (float)w,
         .width_y = 0.0f,
         .height_x = 0.0f,
         .height = (float)h},
        {.image_id = 0,
         .frametime = 0.10f,
         .x = 0.0f,
         .y = 0.0f,
         .width = (float)w,
         .width_y = 0.0f,
         .height_x = 0.0f,
         .height = (float)h},
    };
    bool is_gif = (data[0] & 0x20u) != 0u;
    we_tex_encode_spec_t spec = {
        .format = fmt,
        .flags = is_gif ? WE_TEX_FLAG_IS_GIF : WE_TEX_FLAG_NONE,
        .texture_width = w,
        .texture_height = h,
        .image_width = w,
        .image_height = h,
        .enable_lz4 = (data[0] & 0x10u) != 0u,
        .image_count = 1u,
        .images = &img0,
        .frame_count = is_gif ? 2u : 0u,
        .gif_width = is_gif ? w : 0u,
        .gif_height = is_gif ? h : 0u,
        .frames = is_gif ? frames : NULL,
    };

    uint8_t *tex_buf = NULL;
    size_t tex_sz = 0u;
    we_asset_writer_err_t err = WE_WRITER_OK;
    if (we_tex_encode_v5(&spec, &tex_buf, &tex_sz, &err)) {
        we_tex_t *parsed = we_tex_parse_cpu(tex_buf, tex_sz);
        if (parsed) {
            we_tex_destroy(parsed);
        }
        free(tex_buf);
    }

    if (fmt != WE_TEX_FMT_DXT5) {
        we_tex_encode_mip_t *gen_mips = NULL;
        uint32_t gen_count = 0u;
        if (we_tex_generate_mips(fmt, w, h, px, &gen_mips, &gen_count)) {
            we_tex_free_generated_mips(gen_mips, gen_count);
        }
    }

    we_script_diag_t diag;
    (void)we_script_lint((const char *)data, size, 0u, &diag);
    (void)we_script_lint((const char *)data, size, (uint32_t)(data[0] & 0x3u), &diag);
}

static void fuzz_authoring_ipc_validators(const uint8_t *data, size_t size)
{
    uint8_t aligned_buf[2048];
    size_t copy_len = size < sizeof(aligned_buf) ? size : sizeof(aligned_buf);
    memset(aligned_buf, 0, sizeof(aligned_buf));
    memcpy(aligned_buf, data, copy_len);

    const char *str_ptr = NULL;
    const we_brush_dab_t *dabs = NULL;
    uint32_t dab_count = 0u;
    const void *part_payload = NULL;
    uint32_t part_len = 0u;
    const we_audio_binding_t *bindings = NULL;
    uint32_t bind_count = 0u;

    /* Exercise raw fuzz sizes as well as zero-padded struct-sized buffers. */
    const uint8_t *bufs[2] = {data, aligned_buf};
    const size_t lens[2] = {size, sizeof(aligned_buf)};
    for (size_t k = 0u; k < 2u; ++k) {
        const uint8_t *b = bufs[k];
        size_t l = lens[k];
        (void)waywal_ipc_validate_req_scene_mutate((const waywal_ipc_req_scene_mutate_t *)b, l,
                                                   &str_ptr);
        (void)waywal_ipc_validate_req_hot_reload_code((const waywal_ipc_req_hot_reload_code_t *)b,
                                                      l, &str_ptr);
        (void)waywal_ipc_validate_req_query_scene_tree((const waywal_ipc_req_query_scene_tree_t *)b,
                                                       l);
        (void)waywal_ipc_validate_req_set_shader_combo((const waywal_ipc_req_set_shader_combo_t *)b,
                                                       l);
        (void)waywal_ipc_validate_req_brush_stroke((const waywal_ipc_req_brush_stroke_t *)b, l,
                                                   &dabs, &dab_count);
        (void)waywal_ipc_validate_req_particle_config((const waywal_ipc_req_particle_config_t *)b,
                                                      l, &part_payload, &part_len);
        (void)waywal_ipc_validate_req_set_audio_bindings(
            (const waywal_ipc_req_set_audio_bindings_t *)b, l, &bindings, &bind_count);
        (void)waywal_ipc_validate_req_export_scene_pkg((const waywal_ipc_req_export_scene_pkg_t *)b,
                                                       l);
        (void)waywal_ipc_validate_req_lint_scenescript((const waywal_ipc_req_lint_scenescript_t *)b,
                                                       l, &str_ptr);
        (void)waywal_ipc_validate_req_readback_texture((const waywal_ipc_req_readback_texture_t *)b,
                                                       l);
    }

    (void)waywal_ipc_validate_req_query_scene_tree(
        (const waywal_ipc_req_query_scene_tree_t *)aligned_buf,
        sizeof(waywal_ipc_req_query_scene_tree_t));
    (void)waywal_ipc_validate_req_set_shader_combo(
        (const waywal_ipc_req_set_shader_combo_t *)aligned_buf,
        sizeof(waywal_ipc_req_set_shader_combo_t));
    (void)waywal_ipc_validate_req_export_scene_pkg(
        (const waywal_ipc_req_export_scene_pkg_t *)aligned_buf,
        sizeof(waywal_ipc_req_export_scene_pkg_t));
    (void)waywal_ipc_validate_req_readback_texture(
        (const waywal_ipc_req_readback_texture_t *)aligned_buf,
        sizeof(waywal_ipc_req_readback_texture_t));
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!data || size == 0u) {
        return 0;
    }

    /* 1. Fuzz Valve KeyValues VDF parser */
    waywal_vdf_doc_t *vdf = waywal_vdf_parse((const char *)data, size);
    if (vdf) {
        if (vdf->root) {
            (void)waywal_vdf_get_string(vdf->root, "title", "");
            (void)waywal_vdf_find_child(vdf->root, "apps");
            (void)waywal_vdf_find_child(vdf->root, "0");
        }
        waywal_vdf_free(vdf);
    }

    /* 2. Fuzz PKGV in-memory container parser */
    we_pkg_t *mem_pkg = we_pkg_open_mem(data, size, NULL);
    if (mem_pkg) {
        size_t esz = 0;
        (void)we_pkg_find(mem_pkg, "scene.json", &esz);
        we_pkg_close(mem_pkg);
    }

    /* 3. Fuzz RFC 8785 JCS canonicalizer and in-memory .wwpkg parser */
    fuzz_jcs_canonicalizer(data, size);
    fuzz_wwpkg_in_memory(data, size);

    /* 4. Fuzz file-backed .wwpkg, appworkshop, single-file and multi-file loose directory paths */
    if (!ensure_fuzz_init()) {
        return 0;
    }

    if (write_excl_at(g_shm_dfd, "input.bin", data, size)) {
        char input_path[256];
        snprintf(input_path, sizeof(input_path), "%s/input.bin", g_shm_dir);

        wwpkg_package_t *pkg = wwpkg_open(input_path);
        exercise_wwpkg_handle(pkg);

        waywal_appworkshop_manifest_t ws_man;
        if (waywal_appworkshop_parse_file(input_path, &ws_man)) {
            waywal_appworkshop_free(&ws_man);
        }

        (void)we_compat_scan_directory(g_scanner, input_path, g_report);
        (void)unlinkat(g_shm_dfd, "input.bin", 0);
    }

    fuzz_loose_directory(data, size);

    /* 5. Fuzz LZ4 safe decompressor, PKGV0022 writer paths, TEXV0005 encoder, SceneScript linter,
     * and authoring IPC packet validators */
    fuzz_lz4_and_pkg_writer(data, size);
    fuzz_tex_encoder_and_script_lint(data, size);
    fuzz_authoring_ipc_validators(data, size);
    return 0;
}

#ifdef WWAL_FUZZ_STANDALONE

typedef struct {
    const uint8_t *bytes;
    size_t len;
} fuzz_seed_t;

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

int main(void)
{
    printf("[FUZZ] Starting fuzz_workshop_parsers standalone fuzzing harness...\n");

    if (!ensure_fuzz_init()) {
        fprintf(stderr, "[FUZZ] Failed to initialize /dev/shm workspace\n");
        return 1;
    }

    static const uint8_t seed0[] =
        "\"AppWorkshop\" { \"appid\" \"431960\" \"WorkshopItemsInstalled\" { \"101\" { \"size\" "
        "\"100\" } } }";
    static const uint8_t seed1[] =
        "\"libraryfolders\" { \"0\" { \"path\" \"/steam\" \"apps\" { \"431960\" \"1000\" } } }";
    static const uint8_t seed2[] =
        "{\"title\":\"Test Scene\",\"type\":\"scene\",\"general\":{\"properties\":{}}}";
    /* Valid 4-byte length-prefixed PKGV0001 archive with 1 embedded scene.json entry */
    static const uint8_t seed3[] = "\x08\x00\x00\x00PKGV0001"
                                   "\x01\x00\x00\x00"
                                   "\x0a\x00\x00\x00scene.json"
                                   "\x00\x00\x00\x00"
                                   "\x0e\x00\x00\x00"
                                   "{\"objects\":[]}";
    static const uint8_t seed4[] = "\x28\xB5\x2F\xFD\x00\x00\x00\x00tar.zst mock header";
    static const uint8_t seed5[] =
        "{\"z\":1,\"a\":\"\\uD83D\\uDE80\",\"nested\":{\"b\":true,\"a\":-0.0}}";
    static const uint8_t seed6[] = "// Comments and unclosed quotes \"broken \\uD800";
    static const uint8_t seed7[] = "{{{{{{{{{{ deep recursion }}}}}}}}}}";
    static const uint8_t seed8[] =
        "'use strict';\nexport function update(v) { return v + `${thisLayer.origin.x}`; }\n";
    static const uint8_t seed9[] = "C:\\Windows\\System32\\cmd.exe";
    static const uint8_t seed10[] = "/etc/passwd";

    const fuzz_seed_t corpus[] = {
        {seed0, sizeof(seed0) - 1u}, {seed1, sizeof(seed1) - 1u},   {seed2, sizeof(seed2) - 1u},
        {seed3, sizeof(seed3) - 1u}, {seed4, sizeof(seed4) - 1u},   {seed5, sizeof(seed5) - 1u},
        {seed6, sizeof(seed6) - 1u}, {seed7, sizeof(seed7) - 1u},   {seed8, sizeof(seed8) - 1u},
        {seed9, sizeof(seed9) - 1u}, {seed10, sizeof(seed10) - 1u},
    };

    const size_t corpus_count = sizeof(corpus) / sizeof(corpus[0]);
    for (size_t i = 0; i < corpus_count; ++i) {
        LLVMFuzzerTestOneInput(corpus[i].bytes, corpus[i].len);
    }

    const int MUTATION_ROUNDS = 2500;
    uint32_t rng = 0xC001CAFEU;
    uint8_t mut_buf[2048];

    for (int r = 0; r < MUTATION_ROUNDS; ++r) {
        size_t base_idx = xorshift32(&rng) % corpus_count;
        size_t base_len = corpus[base_idx].len;
        size_t len = base_len < sizeof(mut_buf) ? base_len : sizeof(mut_buf) - 1u;
        memcpy(mut_buf, corpus[base_idx].bytes, len);

        uint32_t flips = (xorshift32(&rng) % 8U) + 1U;
        for (uint32_t f = 0; f < flips; ++f) {
            if (len > 0u) {
                size_t p = xorshift32(&rng) % len;
                mut_buf[p] ^= (uint8_t)(xorshift32(&rng) & 0xFFU);
            }
        }

        size_t test_len = (len > 0u) ? ((xorshift32(&rng) % len) + 1U) : 0u;
        LLVMFuzzerTestOneInput(mut_buf, test_len);
    }

    cleanup_fuzz_state();
    printf("[FUZZ] fuzz_workshop_parsers (%d mutation rounds) PASSED CLEANLY (100%%).\n",
           MUTATION_ROUNDS);
    return 0;
}
#endif
