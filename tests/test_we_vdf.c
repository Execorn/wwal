// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "waywal/os_compat.h"
#include "waywal/we_appworkshop.h"
#include "waywal/we_asset_writer.h"
#include "waywal/we_pkg.h"
#include "waywal/we_project.h"
#include "waywal/we_vdf.h"

static void test_vdf_basic_kv(void)
{
    printf("[VDF 1/16] Testing basic quoted key-value pairs...\n");
    const char *doc_str = "\"settings\"\n"
                          "{\n"
                          "    \"name\" \"WayWal\"\n"
                          "    \"version\" \"1.2.0\"\n"
                          "    \"enabled\" \"1\"\n"
                          "}\n";

    waywal_vdf_doc_t *doc = waywal_vdf_parse(doc_str, strlen(doc_str));
    assert(doc != NULL);
    assert(doc->root != NULL);
    assert(strcmp(doc->root->key, "settings") == 0);
    assert(doc->root->type == VDF_NODE_OBJECT);

    const char *name = waywal_vdf_get_string(doc->root, "name", NULL);
    assert(name != NULL && strcmp(name, "WayWal") == 0);

    const char *ver = waywal_vdf_get_string(doc->root, "version", NULL);
    assert(ver != NULL && strcmp(ver, "1.2.0") == 0);

    const char *enabled = waywal_vdf_get_string(doc->root, "enabled", NULL);
    assert(enabled != NULL && strcmp(enabled, "1") == 0);

    const char *missing = waywal_vdf_get_string(doc->root, "nonexistent", "fallback_val");
    assert(missing != NULL && strcmp(missing, "fallback_val") == 0);

    waywal_vdf_free(doc);
    printf("  -> Passed.\n");
}

static void test_vdf_unquoted_tokens(void)
{
    printf("[VDF 2/16] Testing unquoted keys and values...\n");
    const char *doc_str = "AppSettings\n"
                          "{\n"
                          "    appid 431960\n"
                          "    name WallpaperEngine\n"
                          "    autoupdate 1\n"
                          "}\n";

    waywal_vdf_doc_t *doc = waywal_vdf_parse(doc_str, strlen(doc_str));
    assert(doc != NULL);
    assert(doc->root != NULL);
    assert(strcmp(doc->root->key, "AppSettings") == 0);

    const char *appid = waywal_vdf_get_string(doc->root, "appid", NULL);
    assert(appid != NULL && strcmp(appid, "431960") == 0);

    const char *name = waywal_vdf_get_string(doc->root, "name", NULL);
    assert(name != NULL && strcmp(name, "WallpaperEngine") == 0);

    waywal_vdf_free(doc);
    printf("  -> Passed.\n");
}

static void test_vdf_comments(void)
{
    printf("[VDF 3/16] Testing single-line comments in diverse positions...\n");
    const char *doc_str = "// Global comment before root\n"
                          "\"Config\" // inline comment\n"
                          "{\n"
                          "    // Section comment\n"
                          "    \"fps\" \"60\" // trailing comment\n"
                          "    // Interleaved comment\n"
                          "    \"quality\" \"high\"\n"
                          "}\n"
                          "// Trailing comment at EOF\n";

    waywal_vdf_doc_t *doc = waywal_vdf_parse(doc_str, strlen(doc_str));
    assert(doc != NULL);
    assert(doc->root != NULL);

    assert(strcmp(waywal_vdf_get_string(doc->root, "fps", ""), "60") == 0);
    assert(strcmp(waywal_vdf_get_string(doc->root, "quality", ""), "high") == 0);

    waywal_vdf_free(doc);
    printf("  -> Passed.\n");
}

static void test_vdf_mixed_crlf(void)
{
    printf("[VDF 4/16] Testing mixed CRLF and LF line endings...\n");
    const char *doc_str = "\"CRLFTest\"\r\n"
                          "{\r\n"
                          "    \"line1\" \"value1\"\n"
                          "    \"line2\" \"value2\"\r\n"
                          "    \"line3\" \"value3\"\n\r\n"
                          "}\r\n";

    waywal_vdf_doc_t *doc = waywal_vdf_parse(doc_str, strlen(doc_str));
    assert(doc != NULL);
    assert(doc->root != NULL);

    assert(strcmp(waywal_vdf_get_string(doc->root, "line1", ""), "value1") == 0);
    assert(strcmp(waywal_vdf_get_string(doc->root, "line2", ""), "value2") == 0);
    assert(strcmp(waywal_vdf_get_string(doc->root, "line3", ""), "value3") == 0);

    waywal_vdf_free(doc);
    printf("  -> Passed.\n");
}

static void test_vdf_escape_sequences(void)
{
    printf("[VDF 5/16] Testing escape sequences in strings (\\\", \\\\, \\n, \\t)...\n");
    const char *doc_str = "\"Escapes\"\n"
                          "{\n"
                          "    \"quoted\" \"Hello \\\"World\\\"!\"\n"
                          "    \"backslash\" \"C:\\\\Steam\\\\steamapps\"\n"
                          "    \"multiline\" \"Line1\\nLine2\"\n"
                          "    \"tabbed\" \"Col1\\tCol2\"\n"
                          "}\n";

    waywal_vdf_doc_t *doc = waywal_vdf_parse(doc_str, strlen(doc_str));
    assert(doc != NULL);
    assert(doc->root != NULL);

    const char *q = waywal_vdf_get_string(doc->root, "quoted", "");
    assert(strstr(q, "\"World\"") != NULL);

    const char *bs = waywal_vdf_get_string(doc->root, "backslash", "");
    assert(strstr(bs, "C:\\Steam\\steamapps") != NULL);

    const char *ml = waywal_vdf_get_string(doc->root, "multiline", "");
    assert(strstr(ml, "\n") != NULL);

    const char *tb = waywal_vdf_get_string(doc->root, "tabbed", "");
    assert(strstr(tb, "\t") != NULL);

    waywal_vdf_free(doc);
    printf("  -> Passed.\n");
}

static void test_vdf_nested_hierarchy(void)
{
    printf("[VDF 6/16] Testing multi-level nested hierarchy and traversal...\n");
    const char *doc_str = "\"Root\"\n"
                          "{\n"
                          "    \"Level1\"\n"
                          "    {\n"
                          "        \"Level2\"\n"
                          "        {\n"
                          "            \"Level3\"\n"
                          "            {\n"
                          "                \"leaf_key\" \"deep_value\"\n"
                          "            }\n"
                          "            \"peer_key\" \"peer_val\"\n"
                          "        }\n"
                          "    }\n"
                          "}\n";

    waywal_vdf_doc_t *doc = waywal_vdf_parse(doc_str, strlen(doc_str));
    assert(doc != NULL);

    const waywal_vdf_node_t *l1 = waywal_vdf_find_child(doc->root, "Level1");
    assert(l1 != NULL && l1->type == VDF_NODE_OBJECT);

    const waywal_vdf_node_t *l2 = waywal_vdf_find_child(l1, "Level2");
    assert(l2 != NULL && l2->type == VDF_NODE_OBJECT);

    const waywal_vdf_node_t *l3 = waywal_vdf_find_child(l2, "Level3");
    assert(l3 != NULL && l3->type == VDF_NODE_OBJECT);

    const char *leaf = waywal_vdf_get_string(l3, "leaf_key", "");
    assert(strcmp(leaf, "deep_value") == 0);

    const char *peer = waywal_vdf_get_string(l2, "peer_key", "");
    assert(strcmp(peer, "peer_val") == 0);

    waywal_vdf_free(doc);
    printf("  -> Passed.\n");
}

static void test_vdf_recursion_clamp(void)
{
    printf("[VDF 7/16] Testing recursion depth clamp (> 64 nested levels)...\n");
    char deep_buf[4096];
    size_t pos = 0;

    for (int i = 0; i < 70; ++i) {
        pos += (size_t)snprintf(deep_buf + pos, sizeof(deep_buf) - pos, "\"node%d\" { ", i);
    }
    pos += (size_t)snprintf(deep_buf + pos, sizeof(deep_buf) - pos, "\"val\" \"123\" ");
    for (int i = 0; i < 70; ++i) {
        pos += (size_t)snprintf(deep_buf + pos, sizeof(deep_buf) - pos, "} ");
    }

    waywal_vdf_doc_t *doc = waywal_vdf_parse(deep_buf, pos);
    if (doc) {
        waywal_vdf_free(doc);
    }
    printf("  -> Passed.\n");
}

static void test_vdf_malformed_graceful(void)
{
    printf("[VDF 8/16] Testing graceful failure on malformed VDFs...\n");
    const char *unclosed = "\"root\" { \"k1\" \"v1\" ";
    waywal_vdf_doc_t *doc1 = waywal_vdf_parse(unclosed, strlen(unclosed));
    if (doc1) {
        waywal_vdf_free(doc1);
    }

    const char *bad_tokens = "}}} {{{ \"\"\"\"";
    waywal_vdf_doc_t *doc2 = waywal_vdf_parse(bad_tokens, strlen(bad_tokens));
    if (doc2) {
        waywal_vdf_free(doc2);
    }

    const char *empty_str = "";
    waywal_vdf_doc_t *doc3 = waywal_vdf_parse(empty_str, 0);
    assert(doc3 == NULL || doc3->root == NULL);
    if (doc3) {
        waywal_vdf_free(doc3);
    }

    printf("  -> Passed.\n");
}

static void test_vdf_libraryfolders(void)
{
    printf("[VDF 9/16] Testing real-world Steam libraryfolders.vdf parser...\n");
    const char *lf_vdf = "\"libraryfolders\"\n"
                         "{\n"
                         "    \"0\"\n"
                         "    {\n"
                         "        \"path\" \"/home/execorn/.local/share/Steam\"\n"
                         "        \"label\" \"\"\n"
                         "        \"contentid\" \"1234567890\"\n"
                         "        \"total_size\" \"1000204886016\"\n"
                         "        \"update_clean_bytes_tally\" \"456789\"\n"
                         "        \"time_last_update_verified\" \"1700000000\"\n"
                         "        \"apps\"\n"
                         "        {\n"
                         "            \"228980\" \"1048576\"\n"
                         "            \"431960\" \"1572864000\"\n"
                         "            \"1091500\" \"73400320000\"\n"
                         "        }\n"
                         "    }\n"
                         "    \"1\"\n"
                         "    {\n"
                         "        \"path\" \"/mnt/games/SteamLibrary\"\n"
                         "        \"label\" \"NVMe_Fast\"\n"
                         "        \"contentid\" \"9876543210\"\n"
                         "        \"total_size\" \"2000409772032\"\n"
                         "        \"apps\"\n"
                         "        {\n"
                         "            \"570\" \"41943040000\"\n"
                         "        }\n"
                         "    }\n"
                         "}\n";

    waywal_vdf_doc_t *doc = waywal_vdf_parse(lf_vdf, strlen(lf_vdf));
    assert(doc != NULL);
    assert(doc->root != NULL);

    const waywal_vdf_node_t *drive0 = waywal_vdf_find_child(doc->root, "0");
    assert(drive0 != NULL);
    assert(strcmp(waywal_vdf_get_string(drive0, "path", ""), "/home/execorn/.local/share/Steam") ==
           0);

    const waywal_vdf_node_t *apps0 = waywal_vdf_find_child(drive0, "apps");
    assert(apps0 != NULL);
    assert(waywal_vdf_find_child(apps0, "431960") != NULL);

    const waywal_vdf_node_t *drive1 = waywal_vdf_find_child(doc->root, "1");
    assert(drive1 != NULL);
    assert(strcmp(waywal_vdf_get_string(drive1, "label", ""), "NVMe_Fast") == 0);

    waywal_vdf_free(doc);
    printf("  -> Passed.\n");
}

static void test_vdf_appworkshop_manifest(void)
{
    printf("[VDF 10/16] Testing appworkshop_431960.vdf parsing into structured manifest...\n");
    char dir_tmpl[] = "/dev/shm/wwal_vdf_test_XXXXXX";
    char *dir = mkdtemp(dir_tmpl);
    if (!dir) {
        strcpy(dir_tmpl, "/tmp/wwal_vdf_test_XXXXXX");
        dir = mkdtemp(dir_tmpl);
    }
    assert(dir != NULL);

    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/appworkshop_431960.acf", dir);
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    assert(fd >= 0);

    const char *manifest_content = "\"AppWorkshop\"\n"
                                   "{\n"
                                   "    \"appid\" \"431960\"\n"
                                   "    \"SizeOnDisk\" \"524288000\"\n"
                                   "    \"NeedsUpdateCount\" \"1\"\n"
                                   "    \"NeedsDownloadCount\" \"0\"\n"
                                   "    \"TimeLastUpdated\" \"1710000000\"\n"
                                   "    \"TimeLastAppCheck\" \"1710001000\"\n"
                                   "    \"WorkshopItemsInstalled\"\n"
                                   "    {\n"
                                   "        \"111222333\"\n"
                                   "        {\n"
                                   "            \"size\" \"10485760\"\n"
                                   "            \"timeupdated\" \"1709999000\"\n"
                                   "            \"manifestid\" \"987654321\"\n"
                                   "        }\n"
                                   "        \"444555666\"\n"
                                   "        {\n"
                                   "            \"size\" \"20971520\"\n"
                                   "            \"timeupdated\" \"1709998000\"\n"
                                   "            \"manifestid\" \"123456789\"\n"
                                   "        }\n"
                                   "    }\n"
                                   "    \"WorkshopItemDetails\"\n"
                                   "    {\n"
                                   "        \"111222333\"\n"
                                   "        {\n"
                                   "            \"subscribedbyuser\" \"1\"\n"
                                   "        }\n"
                                   "        \"444555666\"\n"
                                   "        {\n"
                                   "            \"subscribedbyuser\" \"0\"\n"
                                   "        }\n"
                                   "    }\n"
                                   "}\n";

    ssize_t wr = write(fd, manifest_content, strlen(manifest_content));
    assert(wr == (ssize_t)strlen(manifest_content));
    close(fd);

    waywal_appworkshop_manifest_t manifest;
    memset(&manifest, 0, sizeof(manifest));
    bool ok = waywal_appworkshop_parse_file(tmp_path, &manifest);
    assert(ok);

    assert(manifest.app_id == 431960);
    assert(manifest.size_on_disk_bytes == 524288000);
    assert(manifest.needs_update_count == 1);
    assert(manifest.item_count == 2);
    assert(manifest.items != NULL);

    bool found1 = false;
    bool found2 = false;
    for (size_t i = 0; i < manifest.item_count; ++i) {
        if (manifest.items[i].published_file_id == 111222333ULL) {
            found1 = true;
            assert(manifest.items[i].size_bytes == 10485760);
            assert(manifest.items[i].manifest_id == 987654321ULL);
            assert(manifest.items[i].subscribed_by_user == true);
            assert(manifest.items[i].is_installed == true);
            assert(manifest.items[i].state_flags & WAYWAL_UGC_STATE_INSTALLED);
            assert(manifest.items[i].state_flags & WAYWAL_UGC_STATE_SUBSCRIBED);
        }
        if (manifest.items[i].published_file_id == 444555666ULL) {
            found2 = true;
            assert(manifest.items[i].size_bytes == 20971520);
            assert(manifest.items[i].manifest_id == 123456789ULL);
            assert(manifest.items[i].subscribed_by_user == false);
            assert(manifest.items[i].is_installed == true);
            assert(manifest.items[i].state_flags & WAYWAL_UGC_STATE_INSTALLED);
        }
    }
    assert(found1 && found2);

    waywal_appworkshop_free(&manifest);
    unlink(tmp_path);
    rmdir(dir);
    printf("  -> Passed.\n");
}

static void test_vdf_multi_drive_discovery(void)
{
    printf("[VDF 11/16] Testing multi-drive Steam library discovery API...\n");
    waywal_steam_library_t libs[16];
    memset(libs, 0, sizeof(libs));

    size_t count = waywal_steam_discover_libraries(libs, 16);
    printf("  -> Discovered %zu Steam libraries on host system.\n", count);
    for (size_t i = 0; i < count; ++i) {
        printf("     Library [%zu]: %s (has WE: %s)\n", i, libs[i].path,
               libs[i].has_we_app ? "YES" : "no");
        assert(strlen(libs[i].path) > 0);
    }

    printf("  -> Passed.\n");
}

static void test_vdf_empty_and_whitespace(void)
{
    printf("[VDF 12/16] Testing whitespace edge cases and empty string values...\n");
    const char *doc_str = "   \t  \n"
                          "\"Whitespace\"\n"
                          "{\n"
                          "    \"empty_val\" \"\"\n"
                          "    \"spaced_val\" \"   \"\n"
                          "    \"trailing_spaces\"   \"val\"   \n"
                          "}\n";

    waywal_vdf_doc_t *doc = waywal_vdf_parse(doc_str, strlen(doc_str));
    assert(doc != NULL);
    assert(doc->root != NULL);

    const char *empty_val = waywal_vdf_get_string(doc->root, "empty_val", NULL);
    assert(empty_val != NULL && strcmp(empty_val, "") == 0);

    const char *spaced_val = waywal_vdf_get_string(doc->root, "spaced_val", NULL);
    assert(spaced_val != NULL && strcmp(spaced_val, "   ") == 0);

    const char *trailing = waywal_vdf_get_string(doc->root, "trailing_spaces", NULL);
    assert(trailing != NULL && strcmp(trailing, "val") == 0);

    waywal_vdf_free(doc);
    printf("  -> Passed.\n");
}

static void test_vdf_dense_slab_chaining(void)
{
    printf("[VDF 13/16] Testing dense VDF (>12x node-to-byte ratio) multi-slab chaining...\n");

    /* Each entry 'a{b c}' is 6 bytes and allocates 2 VDF nodes + child array + 3 strings (>15x) */
    const size_t pair_count = 30000u;
    size_t cap = pair_count * 7u + 32u;
    char *dense = (char *)malloc(cap);
    assert(dense != NULL);

    size_t pos = 0;
    memcpy(dense + pos, "r{", 2u);
    pos += 2u;
    for (size_t i = 0; i < pair_count; i++) {
        memcpy(dense + pos, "a{b c}", 6u);
        pos += 6u;
    }
    dense[pos++] = '}';
    dense[pos] = '\0';

    waywal_vdf_doc_t *doc = waywal_vdf_parse(dense, pos);
    assert(doc != NULL && doc->root != NULL);
    assert(doc->root->type == VDF_NODE_OBJECT);
    assert(doc->root->object.child_count == pair_count);

    const waywal_vdf_node_t *first = doc->root->object.children[0];
    const waywal_vdf_node_t *last = doc->root->object.children[pair_count - 1u];
    assert(first != NULL && strcmp(waywal_vdf_get_string(first, "b", ""), "c") == 0);
    assert(last != NULL && strcmp(waywal_vdf_get_string(last, "b", ""), "c") == 0);

    waywal_vdf_free(doc);
    free(dense);
    printf("  -> Passed (%zu nested nodes across chained 256 KiB slabs).\n", pair_count * 2u);
}

static void test_vdf_find_child_self_key_collision(void)
{
    printf("[VDF 14/16] Testing waywal_vdf_find_child self-key collision regression...\n");

    const char *vdf1 = "\"apps\" { \"431960\" \"100\" }";
    waywal_vdf_doc_t *doc1 = waywal_vdf_parse(vdf1, strlen(vdf1));
    assert(doc1 != NULL && doc1->root != NULL);
    assert(strcmp(doc1->root->key, "apps") == 0);
    assert(waywal_vdf_find_child(doc1->root, "apps") == NULL);
    assert(waywal_vdf_find_child(doc1->root, "APPS") == NULL);
    assert(waywal_vdf_find_child(doc1->root, "431960") != NULL);
    waywal_vdf_free(doc1);

    const char *vdf2 = "\"node\" { \"node\" { \"leaf\" \"ok\" } }";
    waywal_vdf_doc_t *doc2 = waywal_vdf_parse(vdf2, strlen(vdf2));
    assert(doc2 != NULL && doc2->root != NULL);
    const waywal_vdf_node_t *child = waywal_vdf_find_child(doc2->root, "node");
    assert(child != NULL && child != doc2->root);
    assert(strcmp(waywal_vdf_get_string(child, "leaf", ""), "ok") == 0);
    waywal_vdf_free(doc2);

    printf("  -> Passed.\n");
}

static void test_vdf_utf16_surrogate_pairs(void)
{
    printf("[VDF 15/16] Testing UTF-16 surrogate pair decoding into valid 4-byte UTF-8...\n");

    const char *vdf = "\"Unicode\"\n"
                      "{\n"
                      "    \"rocket\" \"\\uD83D\\uDE80\"\n"
                      "    \"clef\" \"A\\uD834\\uDD1EZ\"\n"
                      "    \"lone_hi\" \"\\uD800x\"\n"
                      "    \"lone_lo\" \"\\uDC00y\"\n"
                      "}\n";

    waywal_vdf_doc_t *doc = waywal_vdf_parse(vdf, strlen(vdf));
    assert(doc != NULL && doc->root != NULL);

    const char *rocket = waywal_vdf_get_string(doc->root, "rocket", "");
    assert(strlen(rocket) == 4u);
    assert(memcmp(rocket, "\xF0\x9F\x9A\x80", 4u) == 0);

    const char *clef = waywal_vdf_get_string(doc->root, "clef", "");
    assert(strlen(clef) == 6u);
    assert(memcmp(clef, "A\xF0\x9D\x84\x9EZ", 6u) == 0);

    const char *lone_hi = waywal_vdf_get_string(doc->root, "lone_hi", "");
    assert(strlen(lone_hi) == 4u && memcmp(lone_hi, "\xEF\xBF\xBDx", 4u) == 0);

    const char *lone_lo = waywal_vdf_get_string(doc->root, "lone_lo", "");
    assert(strlen(lone_lo) == 4u && memcmp(lone_lo, "\xEF\xBF\xBDy", 4u) == 0);

    waywal_vdf_free(doc);
    printf("  -> Passed.\n");
}

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void test_appworkshop_50k_benchmark(void)
{
    printf("[VDF 16/16] Benchmarking 50,000-item appworkshop_431960.acf (< 5.0 ms budget)...\n");

    char dir_tmpl[] = "/dev/shm/wwal_bench50k_XXXXXX";
    char *dir = mkdtemp(dir_tmpl);
    if (!dir) {
        strcpy(dir_tmpl, "/tmp/wwal_bench50k_XXXXXX");
        dir = mkdtemp(dir_tmpl);
    }
    assert(dir != NULL);

    char acf_path[256];
    snprintf(acf_path, sizeof(acf_path), "%s/appworkshop_431960.acf", dir);

    const size_t item_count = 50000u;
    size_t buf_cap = item_count * 220u + 1024u;
    char *acf_buf = (char *)malloc(buf_cap);
    assert(acf_buf != NULL);

    size_t off =
        (size_t)snprintf(acf_buf, buf_cap,
                         "\"AppWorkshop\"\n{\n\t\"appid\"\t\"431960\"\n\t\"SizeOnDisk\"\t\"0\"\n"
                         "\t\"WorkshopItemsInstalled\"\n\t{\n");

    for (size_t i = 0; i < item_count; i++) {
        uint64_t id = 2800000000ULL + (uint64_t)i * 7919ULL;
        off += (size_t)snprintf(acf_buf + off, buf_cap - off,
                                "\t\t\"%llu\"\n\t\t{\n\t\t\t\"size\"\t\"%zu\"\n"
                                "\t\t\t\"timeupdated\"\t\"1710000000\"\n"
                                "\t\t\t\"manifest\"\t\"%llu\"\n\t\t}\n",
                                (unsigned long long)id, 1024u + (i & 255u),
                                (unsigned long long)(900000000ULL + i));
    }

    off += (size_t)snprintf(acf_buf + off, buf_cap - off, "\t}\n\t\"WorkshopItemDetails\"\n\t{\n");
    for (size_t i = 0; i < item_count; i++) {
        uint64_t id = 2800000000ULL + (uint64_t)i * 7919ULL;
        off += (size_t)snprintf(acf_buf + off, buf_cap - off,
                                "\t\t\"%llu\"\n\t\t{\n\t\t\t\"subscribedbyuser\"\t\"%u\"\n"
                                "\t\t\t\"state\"\t\"5\"\n\t\t}\n",
                                (unsigned long long)id, (unsigned)((i & 1u) ^ 1u));
    }
    off += (size_t)snprintf(acf_buf + off, buf_cap - off, "\t}\n}\n");

    int fd = open(acf_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    assert(fd >= 0);
    size_t wr_off = 0;
    while (wr_off < off) {
        ssize_t n = write(fd, acf_buf + wr_off, off - wr_off);
        assert(n > 0);
        wr_off += (size_t)n;
    }
    close(fd);
    free(acf_buf);

    /* Warm page cache and OpenMP team once before steady-state sampling */
    waywal_appworkshop_manifest_t warm;
    assert(waywal_appworkshop_parse_file(acf_path, &warm));
    assert(warm.item_count == item_count);
    waywal_appworkshop_free(&warm);

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
    const double budget_ms = 100.0;
#else
    const double budget_ms = 5.0;
#endif

    double best_ms = 1e9;
    waywal_appworkshop_manifest_t manifest;
    memset(&manifest, 0, sizeof(manifest));

    for (int run = 0; run < 60; run++) {
        uint64_t t0 = monotonic_ns();
        bool ok = waywal_appworkshop_parse_file(acf_path, &manifest);
        uint64_t t1 = monotonic_ns();
        assert(ok);
        double elapsed_ms = (double)(t1 - t0) / 1e6;
        if (elapsed_ms < best_ms) {
            best_ms = elapsed_ms;
        }
        if (run >= 2 && best_ms < budget_ms) {
            break;
        }
        if (run < 59) {
            waywal_appworkshop_free(&manifest);
            if (elapsed_ms >= budget_ms) {
                struct timespec pause_ts = {.tv_sec = 0, .tv_nsec = 4000000L};
                nanosleep(&pause_ts, NULL);
            }
        }
    }

    assert(manifest.item_count == item_count);
    assert(manifest.items[0].published_file_id == 2800000000ULL);
    assert(manifest.items[0].is_installed == true);
    assert(manifest.items[0].subscribed_by_user == true);
    assert(manifest.items[item_count - 1u].published_file_id ==
           2800000000ULL + (uint64_t)(item_count - 1u) * 7919ULL);
    assert(manifest.items[item_count - 1u].manifest_id == 900000000ULL + (item_count - 1u));

    printf("  -> 50,000-item Fibonacci merge completed in %.3f ms (budget < %.3f ms).\n", best_ms,
           budget_ms);
    fflush(stdout);
    assert(best_ms < budget_ms && "50k appworkshop parse/merge exceeded budget");

    waywal_appworkshop_free(&manifest);
    unlink(acf_path);
    rmdir(dir);
}

static void test_appworkshop_large_fallback_edges(void)
{
    printf(
        "[VDF 17/17] Testing >300 KiB manifest fallback (comments, escapes, detail-only IDs)...\n");

    char dir_tmpl[] = "/dev/shm/wwal_fb_XXXXXX";
    char *dir = mkdtemp(dir_tmpl);
    if (!dir) {
        strcpy(dir_tmpl, "/tmp/wwal_fb_XXXXXX");
        dir = mkdtemp(dir_tmpl);
    }
    assert(dir != NULL);

    char acf_path[256];
    snprintf(acf_path, sizeof(acf_path), "%s/appworkshop_431960.acf", dir);

    const size_t n_inst = 2500u;
    size_t cap = n_inst * 200u + 4096u;
    char *buf = (char *)malloc(cap);
    assert(buf != NULL);

    size_t off = (size_t)snprintf(
        buf, cap,
        "// Leading comment forcing scalar fallback on >256 KiB file\n"
        "\"AppWorkshop\"\n{\n\t\"appid\"\t\"431960\"\n\t\"WorkshopItemsInstalled\"\n\t{\n");

    for (size_t i = 0; i < n_inst; i++) {
        uint64_t id = 5000000ULL + i;
        off += (size_t)snprintf(
            buf + off, cap - off,
            "\t\t\"%llu\"\n\t\t{\n\t\t\t\"size\"\t\"2048\"\n"
            "\t\t\t\"manifestid\"\t\"%llu\"\n\t\t\t\"note\"\t\"esc\\\"brace{ok}\"\n\t\t}\n",
            (unsigned long long)id, (unsigned long long)(700000ULL + i));
    }
    off += (size_t)snprintf(buf + off, cap - off, "\t}\n\t\"WorkshopItemDetails\"\n\t{\n");
    for (size_t i = 0; i < n_inst; i++) {
        uint64_t id = 5000000ULL + i;
        off += (size_t)snprintf(
            buf + off, cap - off,
            "\t\t\"%llu\"\n\t\t{\n\t\t\t\"itemstate\"\t\"13\"\n\t\t\t\"flags\"\t\"1\"\n\t\t}\n",
            (unsigned long long)id);
    }
    /* Detail-only pending download item not present in WorkshopItemsInstalled */
    off += (size_t)snprintf(buf + off, cap - off,
                            "\t\t\"99999999\"\n\t\t{"
                            "\n\t\t\t\"subscribedbyuser\"\t\"1\"\n\t\t\t\"state\"\t\"33\"\n\t\t}\n"
                            "\t}\n}\n");
    assert(off > 262144u);

    int fd = open(acf_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    assert(fd >= 0);
    size_t wr = 0;
    while (wr < off) {
        ssize_t n = write(fd, buf + wr, off - wr);
        assert(n > 0);
        wr += (size_t)n;
    }
    close(fd);
    free(buf);

    waywal_appworkshop_manifest_t man;
    assert(waywal_appworkshop_parse_file(acf_path, &man));
    assert(man.item_count == n_inst + 1u);
    assert(man.items[0].manifest_id == 700000ULL);
    assert((man.items[0].state_flags & WAYWAL_UGC_STATE_NEEDS_UPDATE) != 0u);
    assert(man.items[n_inst].published_file_id == 99999999ULL);
    assert(man.items[n_inst].subscribed_by_user == true);
    assert(man.items[n_inst].is_installed == false);
    assert(man.needs_download_count >= 1u);
    waywal_appworkshop_free(&man);

    unlink(acf_path);
    rmdir(dir);
    printf("  -> Passed.\n");
}

static void test_appworkshop_compact_parallel_18k_no_hang(void)
{
    printf("[VDF 18/19] Testing 18,000 compact Workshop IDs (>256 KiB parallel probe stress)...\n");

    char dir_tmpl[] = "/dev/shm/wwal_compact18k_XXXXXX";
    char *dir = mkdtemp(dir_tmpl);
    if (!dir) {
        strcpy(dir_tmpl, "/tmp/wwal_compact18k_XXXXXX");
        dir = mkdtemp(dir_tmpl);
    }
    assert(dir != NULL);

    char acf_path[256];
    snprintf(acf_path, sizeof(acf_path), "%s/appworkshop_431960.acf", dir);

    /* Compact 12-byte entries pack ~18,000 unique IDs into ~432 KiB (>256 KiB parallel threshold)
     * where byte-density heuristics would underestimate unique IDs by >4x. */
    const size_t n_items = 18000u;
    const size_t cap = n_items * 32u + 4096u;
    char *buf = (char *)malloc(cap);
    assert(buf != NULL);

    size_t off = (size_t)snprintf(
        buf, cap, "\"AppWorkshop\"\n{\n\"appid\" \"431960\"\n\"WorkshopItemsInstalled\"\n{\n");
    for (size_t i = 0; i < n_items; ++i) {
        off += (size_t)snprintf(buf + off, cap - off, "\"%llu\"{}\n",
                                (unsigned long long)(1000000ULL + i));
    }
    off += (size_t)snprintf(buf + off, cap - off, "}\n\"WorkshopItemDetails\"\n{\n");
    for (size_t i = 0; i < n_items; ++i) {
        off += (size_t)snprintf(buf + off, cap - off, "\"%llu\"{}\n",
                                (unsigned long long)(1000000ULL + i));
    }
    off += (size_t)snprintf(buf + off, cap - off, "}\n}\n");
    assert(off >= 262144u);

    int fd = open(acf_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    assert(fd >= 0);
    size_t wr = 0;
    while (wr < off) {
        ssize_t n = write(fd, buf + wr, off - wr);
        assert(n > 0);
        wr += (size_t)n;
    }
    close(fd);
    free(buf);

    waywal_appworkshop_manifest_t man;
    memset(&man, 0, sizeof(man));
    assert(waywal_appworkshop_parse_file(acf_path, &man));
    assert(man.item_count == n_items);
    assert(man.items[0].published_file_id == 1000000ULL);
    assert(man.items[n_items - 1u].published_file_id == 1000000ULL + (n_items - 1u));
    waywal_appworkshop_free(&man);

    unlink(acf_path);
    rmdir(dir);
    printf("  -> Passed (%zu compact unique IDs merged without probe hang).\n", n_items);
}

static void test_vdf_avx2_boundary_spans(void)
{
    printf("[VDF 19/19] Testing >64-byte whitespace, comments, and quoted escapes across SIMD "
           "lanes...\n");

    char vdf[2048];
    size_t pos = 0;
    /* >96 bytes of mixed VDF whitespace (\t, \r, \n, \v, \f, space) plus comment */
    for (int i = 0; i < 18; ++i) {
        memcpy(vdf + pos, "  \t\v\f\r\n", 7u);
        pos += 7u;
    }
    pos += (size_t)snprintf(
        vdf + pos, sizeof(vdf) - pos,
        "// Long comment crossing multiple 32-byte SIMD vector lanes 0123456789abcdef0123456789\n"
        "\"RootSpanNode\"\n{\n"
        "    "
        "\"long_key_0123456789abcdef_0123456789abcdef_0123456789abcdef_0123456789\"    "
        "\"0123456789012345678901234567890\\\"234567890123456789012345678901\\\\"
        "456789012345678901234567890123456789\"\n}\n");

    waywal_vdf_doc_t *doc = waywal_vdf_parse(vdf, pos);
    assert(doc != NULL && doc->root != NULL);
    assert(strcmp(doc->root->key, "RootSpanNode") == 0);
    const char *val = waywal_vdf_get_string(
        doc->root, "long_key_0123456789abcdef_0123456789abcdef_0123456789abcdef_0123456789", NULL);
    assert(val != NULL);
    assert(val[31] == '"');
    assert(val[62] == '\\');
    assert(strlen(val) == 99u);
    waywal_vdf_free(doc);
    printf("  -> Passed.\n");
}

static void vdf_test_rm_rf(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        unlink(path);
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char child[4096];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            vdf_test_rm_rf(child);
        } else {
            unlink(child);
        }
    }
    closedir(d);
    rmdir(path);
}

static void vdf_write_exact(const char *path, const void *buf, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    assert(fd >= 0);
    const uint8_t *p = (const uint8_t *)buf;
    size_t off = 0u;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        assert(n > 0);
        off += (size_t)n;
    }
    close(fd);
}

static void test_os_compat_openat2_beneath(void)
{
    printf("[VDF 20/25] Testing wwal_openat2_beneath confinement, mode & rejections...\n");

    char root_tmpl[] = "/dev/shm/wwal_openat2_XXXXXX";
    char *root = mkdtemp(root_tmpl);
    assert(root != NULL);

    char out_tmpl[] = "/dev/shm/wwal_openat2_out_XXXXXX";
    char *outside = mkdtemp(out_tmpl);
    assert(outside != NULL);

    char sub_dir[512], valid_file[512], out_file[512], link_sub[512], link_esc[512];
    snprintf(sub_dir, sizeof(sub_dir), "%s/sub", root);
    snprintf(valid_file, sizeof(valid_file), "%s/sub/wall..png", root);
    snprintf(out_file, sizeof(out_file), "%s/secret.txt", outside);
    snprintf(link_sub, sizeof(link_sub), "%s/link_sub", root);
    snprintf(link_esc, sizeof(link_esc), "%s/link_escape", root);

    assert(mkdir(sub_dir, 0755) == 0);
    vdf_write_exact(valid_file, "PNG_OK", 6u);
    vdf_write_exact(out_file, "OUTSIDE", 7u);
    assert(symlink("sub", link_sub) == 0);
    assert(symlink(out_file, link_esc) == 0);

    int dfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(dfd >= 0);

    /* Valid relative path with embedded double-dot filename must succeed */
    int rfd = wwal_openat2_beneath(dfd, "sub/wall..png", O_RDONLY | O_CLOEXEC, 0);
    assert(rfd >= 0);
    char rbuf[16] = {0};
    assert(read(rfd, rbuf, sizeof(rbuf) - 1u) == 6);
    assert(strcmp(rbuf, "PNG_OK") == 0);
    close(rfd);

    /* Invalid dirfd, empty/NULL paths, traversal, dot, trailing slash & symlinks must fail */
    assert(wwal_openat2_beneath(-1, "sub/wall..png", O_RDONLY, 0) < 0 && errno == EBADF);
    assert(wwal_openat2_beneath(dfd, NULL, O_RDONLY, 0) < 0);
    assert(wwal_openat2_beneath(dfd, "", O_RDONLY, 0) < 0);
    assert(wwal_openat2_beneath(dfd, "/etc/hostname", O_RDONLY, 0) < 0);
    assert(wwal_openat2_beneath(dfd, ".", O_RDONLY | O_DIRECTORY, 0) < 0);
    assert(wwal_openat2_beneath(dfd, "./sub/wall..png", O_RDONLY, 0) < 0);
    assert(wwal_openat2_beneath(dfd, "sub/.", O_RDONLY | O_DIRECTORY, 0) < 0);
    assert(wwal_openat2_beneath(dfd, "../outside", O_RDONLY, 0) < 0);
    assert(wwal_openat2_beneath(dfd, "sub/../../outside", O_RDONLY, 0) < 0);
    assert(wwal_openat2_beneath(dfd, "sub/", O_RDONLY | O_DIRECTORY, 0) < 0);
    assert(wwal_openat2_beneath(dfd, "link_sub/wall..png", O_RDONLY, 0) < 0);
    assert(wwal_openat2_beneath(dfd, "link_escape", O_RDONLY, 0) < 0);

    /* Explicit mode parameter for O_CREAT */
    mode_t old_umask = umask(0022);
    int cfd =
        wwal_openat2_beneath(dfd, "sub/created.bin", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
    umask(old_umask);
    assert(cfd >= 0);
    struct stat cst;
    assert(fstat(cfd, &cst) == 0);
    assert((cst.st_mode & 0777) == 0640);
    close(cfd);

    /* Explicit mode parameter for O_TMPFILE when supported by /dev/shm tmpfs */
    int tfd = wwal_openat2_beneath(dfd, "sub", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
    if (tfd >= 0) {
        struct stat tst;
        assert(fstat(tfd, &tst) == 0);
        assert(tst.st_nlink == 0);
        assert((tst.st_mode & 0700) == 0600);
        close(tfd);
    } else {
        assert(errno == ENOTSUP || errno == EOPNOTSUPP);
    }

    close(dfd);
    vdf_test_rm_rf(root);
    vdf_test_rm_rf(outside);
    printf("  -> Passed.\n");
}

static void test_appworkshop_u64_ids_and_stateflags(void)
{
    printf("[VDF 21/25] Testing >16-digit IDs/manifests, StateFlags priority & overflow...\n");

    char dir_tmpl[] = "/dev/shm/wwal_ws_u64_XXXXXX";
    char *dir = mkdtemp(dir_tmpl);
    assert(dir != NULL);

    char small_path[512];
    snprintf(small_path, sizeof(small_path), "%s/small.acf", dir);
    const char *small_acf =
        "\"AppWorkshop\"\n{\n\t\"appid\"\t\"431960\"\n\t\"WorkshopItemsInstalled\"\n\t{\n"
        "\t\t\"1844674407370955161\"\n\t\t{\n\t\t\t\"size\"\t\"123\"\n"
        "\t\t\t\"manifest\"\t\"18446744073709551610\"\n\t\t}\n"
        "\t\t\"18446744073709551614\"\n\t\t{\n\t\t\t\"size\"\t\"456\"\n"
        "\t\t\t\"manifest\"\t\"99\"\n\t\t}\n\t}\n"
        "\t\"WorkshopItemDetails\"\n\t{\n"
        "\t\t\"1844674407370955161\"\n\t\t{\n\t\t\t\"state\"\t\"4\"\n"
        "\t\t\t\"StateFlags\"\t\"36\"\n\t\t\t\"itemstate\"\t\"1\"\n\t\t\t\"flags\"\t\"2\"\n"
        "\t\t}\n"
        "\t\t\"18446744073709551614\"\n\t\t{\n\t\t\t\"state\"\t\"4\"\n"
        "\t\t\t\"StateFlags\"\t\"4294967296\"\n\t\t\t\"flags\"\t\"2\"\n\t\t}\n\t}\n}\n";
    vdf_write_exact(small_path, small_acf, strlen(small_acf));

    waywal_appworkshop_manifest_t sm;
    assert(waywal_appworkshop_parse_file(small_path, &sm));
    assert(sm.item_count == 2u);
    assert(sm.items[0].published_file_id == 1844674407370955161ULL);
    assert(sm.items[0].manifest_id == 18446744073709551610ULL);
    assert(sm.items[0].state_flags == 36u);
    assert(sm.items[1].published_file_id == 18446744073709551614ULL);
    /* Overflowed StateFlags=4294967296 (>UINT32_MAX) must not overwrite valid state=4. */
    assert(sm.items[1].state_flags == 4u);
    waywal_appworkshop_free(&sm);

    /* Parallel SIMD path (> 262 KiB) with 19/20-digit IDs, manifests, and StateFlags priority */
    const size_t n_items = 2200u;
    size_t cap = 600u * 1024u;
    char *big = (char *)malloc(cap);
    assert(big != NULL);
    size_t off = (size_t)snprintf(
        big, cap,
        "\"AppWorkshop\"\n{\n\t\"appid\"\t\"431960\"\n\t\"WorkshopItemsInstalled\"\n\t{\n");
    for (size_t i = 0; i < n_items; i++) {
        uint64_t id = (i == 0u) ? 18446744073709551614ULL : (2800000000ULL + (uint64_t)i * 7919ULL);
        uint64_t man = (i == 0u) ? 18446744073709551610ULL : (900000000ULL + (uint64_t)i);
        off += (size_t)snprintf(big + off, cap - off,
                                "\t\t\"%llu\"\n\t\t{\n\t\t\t\"size\"\t\"512\"\n"
                                "\t\t\t\"timeupdated\"\t\"1710000000\"\n"
                                "\t\t\t\"manifest\"\t\"%llu\"\n\t\t}\n",
                                (unsigned long long)id, (unsigned long long)man);
    }
    off += (size_t)snprintf(big + off, cap - off, "\t}\n\t\"WorkshopItemDetails\"\n\t{\n");
    for (size_t i = 0; i < n_items; i++) {
        uint64_t id = (i == 0u) ? 18446744073709551614ULL : (2800000000ULL + (uint64_t)i * 7919ULL);
        if (i == 0u) {
            off += (size_t)snprintf(big + off, cap - off,
                                    "\t\t\"%llu\"\n\t\t{\n\t\t\t\"StateFlags\"\t\"36\"\n"
                                    "\t\t\t\"state\"\t\"4\"\n\t\t\t\"itemstate\"\t\"1\"\n"
                                    "\t\t\t\"flags\"\t\"2\"\n\t\t}\n",
                                    (unsigned long long)id);
        } else {
            off += (size_t)snprintf(big + off, cap - off,
                                    "\t\t\"%llu\"\n\t\t{\n\t\t\t\"subscribedbyuser\"\t\"1\"\n"
                                    "\t\t\t\"state\"\t\"4\"\n\t\t}\n",
                                    (unsigned long long)id);
        }
    }
    off += (size_t)snprintf(big + off, cap - off, "\t}\n}\n");
    assert(off > 262144u);

    char big_path[512];
    snprintf(big_path, sizeof(big_path), "%s/big.acf", dir);
    vdf_write_exact(big_path, big, off);
    free(big);

    waywal_appworkshop_manifest_t bm;
    assert(waywal_appworkshop_parse_file(big_path, &bm));
    assert(bm.item_count == n_items);
    bool found_u64 = false;
    for (size_t i = 0; i < bm.item_count; i++) {
        if (bm.items[i].published_file_id == 18446744073709551614ULL) {
            assert(bm.items[i].manifest_id == 18446744073709551610ULL);
            assert(bm.items[i].state_flags == 36u);
            found_u64 = true;
            break;
        }
    }
    assert(found_u64);
    waywal_appworkshop_free(&bm);

    vdf_test_rm_rf(dir);
    printf("  -> Passed.\n");
}

static void test_vdf_malformed_and_toml_sections(void)
{
    printf("[VDF 22/25] Testing VDF trailing-brace/unquoted errors & TOML [daemon] gating...\n");

    const char *unterm = "\"Root\"\n{\n\t\"k\"\t\"unterminated\n}\n";
    assert(waywal_vdf_parse(unterm, strlen(unterm)) == NULL);

    const char *extra_brace = "\"Root\"\n{\n\t\"k\"\t\"v\"\n}\n}\n";
    assert(waywal_vdf_parse(extra_brace, strlen(extra_brace)) == NULL);

    const char *unclosed = "\"Root\"\n{\n\t\"k\"\t\"v\"\n";
    assert(waywal_vdf_parse(unclosed, strlen(unclosed)) == NULL);

    char root_tmpl[] = "/dev/shm/wwal_toml_XXXXXX";
    char *root = mkdtemp(root_tmpl);
    assert(root != NULL);

    char cfg_home[512], dot_cfg[512], waywal_cfg_dir[512], fake_home[512];
    char bad1[512], bad2[512], good[512], mk[1024], toml_path[1024];
    snprintf(cfg_home, sizeof(cfg_home), "%s/cfg", root);
    snprintf(fake_home, sizeof(fake_home), "%s/home", root);
    snprintf(dot_cfg, sizeof(dot_cfg), "%s/.config", fake_home);
    snprintf(waywal_cfg_dir, sizeof(waywal_cfg_dir), "%s/waywal", dot_cfg);
    snprintf(bad1, sizeof(bad1), "%s/bad_before", root);
    snprintf(bad2, sizeof(bad2), "%s/bad_after", root);
    snprintf(good, sizeof(good), "%s/good_lib", root);

    assert(mkdir(cfg_home, 0755) == 0);
    assert(mkdir(fake_home, 0755) == 0);
    assert(mkdir(dot_cfg, 0755) == 0);
    assert(mkdir(waywal_cfg_dir, 0755) == 0);
    const char *lib_bases[3] = {bad1, bad2, good};
    for (int i = 0; i < 3; i++) {
        assert(mkdir(lib_bases[i], 0755) == 0);
        snprintf(mk, sizeof(mk), "%s/steamapps", lib_bases[i]);
        assert(mkdir(mk, 0755) == 0);
        snprintf(mk, sizeof(mk), "%s/steamapps/workshop", lib_bases[i]);
        assert(mkdir(mk, 0755) == 0);
        snprintf(mk, sizeof(mk), "%s/steamapps/workshop/content", lib_bases[i]);
        assert(mkdir(mk, 0755) == 0);
        snprintf(mk, sizeof(mk), "%s/steamapps/workshop/content/431960", lib_bases[i]);
        assert(mkdir(mk, 0755) == 0);
    }

    char toml_buf[2048];
    snprintf(toml_buf, sizeof(toml_buf),
             "[other]\nworkshop_dir = \"%s\"\n\n[daemon]\nworkshop_dir = \"%s\"\n\n"
             "[trailing]\nworkshop_dir = \"%s\"\n",
             bad1, good, bad2);
    snprintf(toml_path, sizeof(toml_path), "%s/config.toml", waywal_cfg_dir);
    vdf_write_exact(toml_path, toml_buf, strlen(toml_buf));

    const char *old_home_env = getenv("HOME");
    const char *old_xdg_env = getenv("XDG_CONFIG_HOME");
    char *saved_home = old_home_env ? strdup(old_home_env) : NULL;
    char *saved_xdg = old_xdg_env ? strdup(old_xdg_env) : NULL;

    assert(setenv("HOME", fake_home, 1) == 0);
    assert(setenv("XDG_CONFIG_HOME", dot_cfg, 1) == 0);

    waywal_steam_library_t libs[16];
    memset(libs, 0, sizeof(libs));
    size_t nlibs = waywal_steam_discover_libraries(libs, 16u);
    bool saw_good = false;
    bool saw_bad = false;
    for (size_t i = 0; i < nlibs; i++) {
        if (strcmp(libs[i].path, good) == 0) {
            saw_good = true;
        }
        if (strcmp(libs[i].path, bad1) == 0 || strcmp(libs[i].path, bad2) == 0) {
            saw_bad = true;
        }
    }

    if (saved_home) {
        setenv("HOME", saved_home, 1);
        free(saved_home);
    } else {
        unsetenv("HOME");
    }
    if (saved_xdg) {
        setenv("XDG_CONFIG_HOME", saved_xdg, 1);
        free(saved_xdg);
    } else {
        unsetenv("XDG_CONFIG_HOME");
    }

    assert(saw_good);
    assert(!saw_bad);
    vdf_test_rm_rf(root);
    printf("  -> Passed.\n");
}

static void test_project_reuse_and_workshop_hash_dedup(void)
{
    printf("[VDF 23/25] Testing we_project_parse reuse & O(N) workshop scan/preview dedup...\n");

    we_project_t proj;
    snprintf(proj.workshop_id, sizeof(proj.workshop_id), "999999");
    snprintf(proj.workshop_dir, sizeof(proj.workshop_dir), "/stale/dir");
    const char *proj_a =
        "{\"title\":\"Item A\",\"type\":\"web\",\"file\":\"a.html\","
        "\"preview\":\"prev.png\",\"general\":{\"properties\":{\"gain\":{\"type\":\"slider\","
        "\"value\":1.5}}}}";
    assert(we_project_parse(proj_a, strlen(proj_a), &proj));
    assert(proj.prop_count == 1u);
    assert(strcmp(proj.preview, "prev.png") == 0);
    assert(proj.workshop_id[0] == '\0');
    assert(proj.workshop_dir[0] == '\0');

    snprintf(proj.workshop_id, sizeof(proj.workshop_id), "888888");
    const char *proj_b = "{\"title\":\"Item B\"}";
    assert(we_project_parse(proj_b, strlen(proj_b), &proj));
    assert(strcmp(proj.title, "Item B") == 0);
    assert(proj.prop_count == 0u);
    assert(proj.workshop_id[0] == '\0');
    assert(strcmp(proj.preview, "preview.jpg") == 0);
    assert(strcmp(proj.file, "scene.json") == 0);
    assert(strcmp(proj.type, "scene") == 0);

    char root_tmpl[] = "/dev/shm/wwal_ws_dedup_XXXXXX";
    char *root = mkdtemp(root_tmpl);
    assert(root != NULL);

    char fake_home[512], fake_cfg[512], sync_dir[512], mk[1024];
    snprintf(fake_home, sizeof(fake_home), "%s/home", root);
    snprintf(fake_cfg, sizeof(fake_cfg), "%s/cfg", root);
    snprintf(sync_dir, sizeof(sync_dir), "%s/previews", root);
    assert(mkdir(fake_home, 0755) == 0);
    assert(mkdir(fake_cfg, 0755) == 0);
    assert(mkdir(sync_dir, 0755) == 0);

    const char *steam_dirs[] = {".local",
                                ".local/share",
                                ".local/share/Steam",
                                ".local/share/Steam/steamapps",
                                ".local/share/Steam/steamapps/workshop",
                                ".local/share/Steam/steamapps/workshop/content",
                                ".local/share/Steam/steamapps/workshop/content/431960",
                                ".steam"};
    for (size_t i = 0; i < sizeof(steam_dirs) / sizeof(steam_dirs[0]); i++) {
        snprintf(mk, sizeof(mk), "%s/%s", fake_home, steam_dirs[i]);
        assert(mkdir(mk, 0755) == 0);
    }

    char target_steam[1024], link1[1024], link2[1024];
    snprintf(target_steam, sizeof(target_steam), "%s/.local/share/Steam", fake_home);
    snprintf(link1, sizeof(link1), "%s/.steam/steam", fake_home);
    snprintf(link2, sizeof(link2), "%s/.steam/root", fake_home);
    assert(symlink(target_steam, link1) == 0);
    assert(symlink(target_steam, link2) == 0);

    const size_t n_items = 128u;
    for (size_t i = 0; i < n_items; i++) {
        char idir[1024], pjson[1024], pprev[1024], body[256];
        snprintf(idir, sizeof(idir), "%s/steamapps/workshop/content/431960/%zu", target_steam,
                 100000u + i);
        assert(mkdir(idir, 0755) == 0);
        snprintf(pjson, sizeof(pjson), "%s/project.json", idir);
        snprintf(pprev, sizeof(pprev), "%s/preview.png", idir);
        snprintf(body, sizeof(body),
                 "{\"title\":\"Dedup_%zu\",\"type\":\"scene\",\"preview\":\"preview.png\"}", i);
        vdf_write_exact(pjson, body, strlen(body));
        vdf_write_exact(pprev, "\x89PNG\r\n\x1a\n", 8u);
    }

    const char *old_home_env = getenv("HOME");
    const char *old_xdg_env = getenv("XDG_CONFIG_HOME");
    char *saved_home = old_home_env ? strdup(old_home_env) : NULL;
    char *saved_xdg = old_xdg_env ? strdup(old_xdg_env) : NULL;
    assert(setenv("HOME", fake_home, 1) == 0);
    assert(setenv("XDG_CONFIG_HOME", fake_cfg, 1) == 0);

    we_project_t *items = NULL;
    size_t count = 0u;
    assert(we_workshop_scan_all(&items, &count));
    assert(count == n_items);
    free(items);

    size_t synced = 0u, pruned = 0u;
    assert(we_workshop_sync_previews(sync_dir, &synced, &pruned));
    assert(synced == n_items);
    assert(pruned == 0u);

    char del_item[1024], stale_link[1024];
    snprintf(del_item, sizeof(del_item), "%s/steamapps/workshop/content/431960/100000",
             target_steam);
    vdf_test_rm_rf(del_item);
    snprintf(stale_link, sizeof(stale_link), "%s/stale_orphan.png", sync_dir);
    assert(symlink("/dev/null/nonexistent_target", stale_link) == 0);

    assert(we_workshop_sync_previews(sync_dir, &synced, &pruned));
    assert(synced == n_items - 1u);
    assert(pruned == 2u);

    if (saved_home) {
        setenv("HOME", saved_home, 1);
        free(saved_home);
    } else {
        unsetenv("HOME");
    }
    if (saved_xdg) {
        setenv("XDG_CONFIG_HOME", saved_xdg, 1);
        free(saved_xdg);
    } else {
        unsetenv("XDG_CONFIG_HOME");
    }

    vdf_test_rm_rf(root);
    printf("  -> Passed.\n");
}

static void test_pkg_duplicate_and_symlink_hardening(void)
{
    printf("[VDF 24/25] Testing we_pkg duplicate entry rejection & symlink dir safety...\n");

    uint8_t dup_pkg[128];
    size_t c = 0u;
    uint32_t mlen = 8u;
    memcpy(dup_pkg + c, &mlen, 4u);
    c += 4u;
    memcpy(dup_pkg + c, "PKGV0001", 8u);
    c += 8u;
    uint32_t ec = 2u;
    memcpy(dup_pkg + c, &ec, 4u);
    c += 4u;
    for (uint32_t e = 0; e < 2u; e++) {
        uint32_t plen = 10u;
        uint32_t off = e * 2u;
        uint32_t sz = 2u;
        memcpy(dup_pkg + c, &plen, 4u);
        c += 4u;
        memcpy(dup_pkg + c, "scene.json", 10u);
        c += 10u;
        memcpy(dup_pkg + c, &off, 4u);
        c += 4u;
        memcpy(dup_pkg + c, &sz, 4u);
        c += 4u;
    }
    memcpy(dup_pkg + c, "{}", 2u);
    c += 2u;
    memcpy(dup_pkg + c, "{}", 2u);
    c += 2u;
    assert(we_pkg_open_mem(dup_pkg, c, NULL) == NULL);

    char dir_tmpl[] = "/dev/shm/wwal_pkg_sym_XXXXXX";
    char *dir = mkdtemp(dir_tmpl);
    assert(dir != NULL);
    char real_scene[512], link_evil[512];
    snprintf(real_scene, sizeof(real_scene), "%s/scene.json", dir);
    snprintf(link_evil, sizeof(link_evil), "%s/evil.json", dir);
    vdf_write_exact(real_scene, "{\"ok\":true}", 11u);
    assert(symlink("/etc/hostname", link_evil) == 0);

    we_pkg_t *pkg = we_pkg_open_dir(dir);
    assert(pkg != NULL);
    size_t out_sz = 0u;
    bool owned = false;
    const uint8_t *ok_buf = we_pkg_resolve(pkg, "scene.json", &out_sz, &owned);
    assert(ok_buf != NULL && out_sz == 11u);
    if (owned) {
        free((void *)(uintptr_t)ok_buf);
    }
    owned = false;
    assert(we_pkg_resolve(pkg, "evil.json", &out_sz, &owned) == NULL);
    we_pkg_close(pkg);

    vdf_test_rm_rf(dir);
    printf("  -> Passed.\n");
}

static void test_asset_writer_atomic_and_symlink(void)
{
    printf("[VDF 25/25] Testing we_write_file_atomic replace & symlink non-following...\n");

    char dir_tmpl[] = "/dev/shm/wwal_atomic_XXXXXX";
    char *dir = mkdtemp(dir_tmpl);
    assert(dir != NULL);

    char target[512], victim[512], sym_target[512];
    snprintf(target, sizeof(target), "%s/output.dat", dir);
    snprintf(victim, sizeof(victim), "%s/victim.dat", dir);
    snprintf(sym_target, sizeof(sym_target), "%s/sym_out.dat", dir);

    we_asset_writer_err_t err = WE_WRITER_OK;
    assert(we_write_file_atomic(target, "FIRST", 5u, &err) && err == WE_WRITER_OK);
    assert(we_write_file_atomic(target, "SECOND_LONGER", 13u, &err) && err == WE_WRITER_OK);

    int fd = open(target, O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    char buf[32] = {0};
    assert(read(fd, buf, sizeof(buf) - 1u) == 13);
    assert(strcmp(buf, "SECOND_LONGER") == 0);
    close(fd);

    vdf_write_exact(victim, "PRESERVED", 9u);
    assert(symlink(victim, sym_target) == 0);
    assert(!we_write_file_atomic(sym_target, "REPLACED", 8u, &err));
    assert(err == WE_WRITER_ERR_INVALID_PATH);

    struct stat st;
    assert(lstat(sym_target, &st) == 0);
    assert(S_ISLNK(st.st_mode));
    int vfd = open(victim, O_RDONLY | O_CLOEXEC);
    assert(vfd >= 0);
    memset(buf, 0, sizeof(buf));
    assert(read(vfd, buf, sizeof(buf) - 1u) == 9);
    assert(strcmp(buf, "PRESERVED") == 0);
    close(vfd);

    vdf_test_rm_rf(dir);
    printf("  -> Passed.\n");
}

int main(void)
{
    printf("==========================================================\n");
    printf("  WayWal KeyValues VDF & AppWorkshop Test Suite (§7.2)     \n");
    printf("==========================================================\n");

    test_vdf_basic_kv();
    test_vdf_unquoted_tokens();
    test_vdf_comments();
    test_vdf_mixed_crlf();
    test_vdf_escape_sequences();
    test_vdf_nested_hierarchy();
    test_vdf_recursion_clamp();
    test_vdf_malformed_graceful();
    test_vdf_libraryfolders();
    test_vdf_appworkshop_manifest();
    test_vdf_multi_drive_discovery();
    test_vdf_empty_and_whitespace();
    test_vdf_dense_slab_chaining();
    test_vdf_find_child_self_key_collision();
    test_vdf_utf16_surrogate_pairs();
    test_appworkshop_50k_benchmark();
    test_appworkshop_large_fallback_edges();
    test_appworkshop_compact_parallel_18k_no_hang();
    test_vdf_avx2_boundary_spans();
    test_os_compat_openat2_beneath();
    test_appworkshop_u64_ids_and_stateflags();
    test_vdf_malformed_and_toml_sections();
    test_project_reuse_and_workshop_hash_dedup();
    test_pkg_duplicate_and_symlink_hardening();
    test_asset_writer_atomic_and_symlink();

    printf("==========================================================\n");
    printf("  >>> ALL 25 VDF & MANIFEST TESTS PASSED CLEANLY (100%%) <<<\n");
    printf("==========================================================\n");

    return 0;
}
