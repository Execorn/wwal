// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_we_vdf.c
 * @brief Comprehensive Valve KeyValues VDF & AppWorkshop Manifest Verification Suite
 *
 * Exercises the bump-arena VDF parser across 10+ complex real-world and synthetic
 * VDF documents: quoted/unquoted tokens, escape sequences, mixed CRLF, comments,
 * recursion depth clamping, libraryfolders.vdf, appworkshop_431960.vdf, and multi-drive
 * Steam library discovery with zero memory leaks.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>

#include "waywal/we_vdf.h"
#include "waywal/we_appworkshop.h"

static void test_vdf_basic_kv(void)
{
    printf("[VDF 1/12] Testing basic quoted key-value pairs...\n");
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
    printf("[VDF 2/12] Testing unquoted keys and values...\n");
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
    printf("[VDF 3/12] Testing single-line comments in diverse positions...\n");
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
    printf("[VDF 4/12] Testing mixed CRLF and LF line endings...\n");
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
    printf("[VDF 5/12] Testing escape sequences in strings (\\\", \\\\, \\n, \\t)...\n");
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
    printf("[VDF 6/12] Testing multi-level nested hierarchy and traversal...\n");
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
    printf("[VDF 7/12] Testing recursion depth clamp (> 64 nested levels)...\n");
    char deep_buf[4096];
    size_t pos = 0;

    for (int i = 0; i < 70; ++i) {
        pos += snprintf(deep_buf + pos, sizeof(deep_buf) - pos, "\"node%d\" { ", i);
    }
    pos += snprintf(deep_buf + pos, sizeof(deep_buf) - pos, "\"val\" \"123\" ");
    for (int i = 0; i < 70; ++i) {
        pos += snprintf(deep_buf + pos, sizeof(deep_buf) - pos, "} ");
    }

    // Must parse without stack overflow or abort
    waywal_vdf_doc_t *doc = waywal_vdf_parse(deep_buf, pos);
    if (doc) {
        waywal_vdf_free(doc);
    }
    printf("  -> Passed.\n");
}

static void test_vdf_malformed_graceful(void)
{
    printf("[VDF 8/12] Testing graceful failure on malformed VDFs...\n");
    const char *unclosed = "\"root\" { \"k1\" \"v1\" "; // missing closing brace
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
    printf("[VDF 9/12] Testing real-world Steam libraryfolders.vdf parser...\n");
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
    printf("[VDF 10/12] Testing appworkshop_431960.vdf parsing into structured manifest...\n");
    const char *tmp_path = "/tmp/test_appworkshop_431960.vdf";
    FILE *f = fopen(tmp_path, "wb");
    assert(f != NULL);

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

    fwrite(manifest_content, 1, strlen(manifest_content), f);
    fclose(f);

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
    printf("  -> Passed.\n");
}

static void test_vdf_multi_drive_discovery(void)
{
    printf("[VDF 11/12] Testing multi-drive Steam library discovery API...\n");
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
    printf("[VDF 12/12] Testing whitespace edge cases and empty string values...\n");
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

    printf("==========================================================\n");
    printf("  >>> ALL 12 VDF & MANIFEST TESTS PASSED CLEANLY (100%%) <<<\n");
    printf("==========================================================\n");

    return 0;
}
