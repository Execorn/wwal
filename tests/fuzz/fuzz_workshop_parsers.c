// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file fuzz_workshop_parsers.c
 * @brief Corpus & Mutation Fuzzing Harness for VDF, Compatibility Scanner, and WWPKG Parsers
 *
 * Targets:
 * 1. Valve KeyValues VDF Parser (waywal_vdf_parse)
 * 2. Wallpaper Engine Compatibility Scanner (we_compat_scan_directory)
 * 3. WayWal Open Hub .wwpkg Container Parser (wwpkg_open)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "waywal/we_vdf.h"
#include "waywal/we_compat_scanner.h"
#include "waywal/wwpkg.h"

static we_compat_scanner_t *g_scanner = NULL;
static we_compat_report_t *g_report = NULL;

static void ensure_scanner_init(void)
{
    if (!g_scanner) {
        g_scanner = we_compat_scanner_create(true, true);
    }
    if (!g_report) {
        g_report = we_compat_report_alloc();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!data || size == 0) {
        return 0;
    }

    // 1. Fuzz KeyValues VDF Parser
    waywal_vdf_doc_t *vdf = waywal_vdf_parse((const char *)data, size);
    if (vdf) {
        if (vdf->root) {
            (void)waywal_vdf_get_string(vdf->root, "title", "");
            (void)waywal_vdf_find_child(vdf->root, "apps");
            (void)waywal_vdf_find_child(vdf->root, "0");
        }
        waywal_vdf_free(vdf);
    }

    // 2. Fuzz .wwpkg container parser & Compat Scanner via temporary file/dir
    ensure_scanner_init();

    char tmp_file[64];
    snprintf(tmp_file, sizeof(tmp_file), "/tmp/fuzz_p2_%d.tmp", getpid());
    int fd = open(tmp_file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        (void)write(fd, data, size);
        close(fd);

        // Fuzz wwpkg_open on arbitrary byte stream
        wwpkg_package_t *pkg = wwpkg_open(tmp_file);
        if (pkg) {
            (void)wwpkg_get_manifest(pkg);
            (void)wwpkg_get_signature(pkg);
            bool is_trusted = false;
            (void)wwpkg_verify_signature(pkg, &is_trusted);
            size_t asset_sz = 0;
            (void)wwpkg_read_asset(pkg, "scene.json", &asset_sz);
            wwpkg_close(pkg);
        }

        // Fuzz compat scanner on arbitrary file
        if (g_scanner && g_report) {
            (void)we_compat_scan_directory(g_scanner, tmp_file, g_report);
        }

        unlink(tmp_file);
    }

    return 0;
}

#ifdef WWAL_FUZZ_STANDALONE

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

    ensure_scanner_init();

    // Initial seed corpus
    const char *corpus[] = {
        "\"AppWorkshop\" { \"appid\" \"431960\" \"WorkshopItemsInstalled\" { \"101\" { \"size\" "
        "\"100\" } } }",
        "\"libraryfolders\" { \"0\" { \"path\" \"/steam\" \"apps\" { \"431960\" \"1000\" } } }",
        "{\"title\":\"Test Scene\",\"type\":\"scene\",\"general\":{\"properties\":{}}}",
        "PKGV0001\x00\x00\x00\x00\x01\x00\x00\x00scene.json\x00\x00",
        "\x28\xB5\x2F\xFD\x00\x00\x00\x00tar.zst mock header",
        "",
        "// Comments and unclosed quotes \"broken",
        "{{{{{{{{{{ deep recursion }}}}}}}}}}",
    };

    size_t corpus_count = sizeof(corpus) / sizeof(corpus[0]);
    for (size_t i = 0; i < corpus_count; ++i) {
        LLVMFuzzerTestOneInput((const uint8_t *)corpus[i], strlen(corpus[i]));
    }

    // Run 5,000 randomized mutations
    const int MUTATION_ROUNDS = 5000;
    uint32_t rng = 0xC001CAFEU;
    uint8_t mut_buf[2048];

    for (int r = 0; r < MUTATION_ROUNDS; ++r) {
        size_t base_idx = xorshift32(&rng) % corpus_count;
        size_t base_len = strlen(corpus[base_idx]);
        size_t len = base_len < sizeof(mut_buf) ? base_len : sizeof(mut_buf) - 1;
        memcpy(mut_buf, corpus[base_idx], len);

        // Apply 1 to 8 random mutations (byte flip, insert, truncate)
        uint32_t flips = (xorshift32(&rng) % 8U) + 1U;
        for (uint32_t f = 0; f < flips; ++f) {
            if (len > 0) {
                size_t p = xorshift32(&rng) % len;
                mut_buf[p] ^= (uint8_t)(xorshift32(&rng) & 0xFFU);
            }
        }

        size_t test_len = (len > 0) ? ((xorshift32(&rng) % len) + 1U) : 0;
        LLVMFuzzerTestOneInput(mut_buf, test_len);
    }

    if (g_report) {
        we_compat_report_free(g_report);
        g_report = NULL;
    }
    if (g_scanner) {
        we_compat_scanner_destroy(g_scanner);
        g_scanner = NULL;
    }

    printf("[FUZZ] fuzz_workshop_parsers (%d mutation rounds) PASSED CLEANLY (100%%).\n",
           MUTATION_ROUNDS);
    return 0;
}
#endif
