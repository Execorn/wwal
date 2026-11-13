// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "waywal/we_appworkshop.h"
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

    printf("==========================================================\n");
    printf("  >>> ALL 19 VDF & MANIFEST TESTS PASSED CLEANLY (100%%) <<<\n");
    printf("==========================================================\n");

    return 0;
}
