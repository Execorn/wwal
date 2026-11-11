// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_catalog_index.cpp
 * @brief SQLite3 WAL, FTS5 BM25 + Trigram, Myers Fuzzy, Tag Bitmap & Delta Reconcile Suite
 */

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "studio/core/catalog_index_manager.hpp"

extern "C" {
#include <waywal/wwpkg.h>
}

using namespace wwal::studio;

namespace {

bool is_sanitized_build()
{
#if defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
    return true;
#endif
#endif
    return std::getenv("ASAN_OPTIONS") != nullptr || std::getenv("UBSAN_OPTIONS") != nullptr;
}

void cleanup_db_files(const std::filesystem::path &dbPath)
{
    std::error_code ec;
    std::filesystem::remove(dbPath, ec);
    std::filesystem::remove(dbPath.string() + "-wal", ec);
    std::filesystem::remove(dbPath.string() + "-shm", ec);
}

void test_triggers_and_crud(CatalogIndexManager &mgr)
{
    std::cout << "[Catalog 1/7] Testing CRUD operations and FTS5 triggers..." << std::endl;

    WorkshopItemRecord item{};
    item.published_file_id = 1001;
    item.owner_steam_id = 76561198000000001ULL;
    item.title = "Neon Samurai 2077";
    item.type = "scene";
    item.file_path = "/wallpapers/1001/scene.pkg";
    item.preview_path = "/wallpapers/1001/preview.jpg";
    item.file_size_bytes = 45000000;
    item.subscriptions = 15000;
    item.rating = 4.85f;
    item.compat_tier = 1;
    item.compat_score = 95;
    item.tags_string = "Cyberpunk, Anime, Glowing";
    item.description = "Futuristic blade warrior in neon rain with audio reactive lights.";

    bool ok = mgr.upsertItem(item);
    assert(ok && "Initial upsert must succeed");
    assert(mgr.totalItemCount() == 1);

    auto firstFetch = mgr.getItem(1001);
    assert(firstFetch.has_value());
    assert(firstFetch->file_clock == 1);

    // Identical re-upsert must be suppressed by dirty-row WHERE guard
    ok = mgr.upsertItem(item);
    assert(ok && "Idempotent upsert must succeed");
    auto sameFetch = mgr.getItem(1001);
    assert(sameFetch.has_value());
    assert(sameFetch->file_clock == 1 && "Unchanged row must not advance file_clock");

    auto results = mgr.searchFts5Bm25("Samurai");
    assert(!results.empty() && "Item must be discoverable via FTS5 after insert");
    assert(results[0].published_file_id == 1001);

    item.title = "Cyberblade Warrior 2077";
    item.description = "Updated description with synthwave soundtrack.";
    ok = mgr.upsertItem(item);
    assert(ok && "Update upsert must succeed");

    auto updatedFetch = mgr.getItem(1001);
    assert(updatedFetch.has_value());
    assert(updatedFetch->file_clock == 2 && "Dirty update must increment file_clock");

    auto old_results = mgr.searchFts5Bm25("Samurai");
    assert(old_results.empty() && "Old title token must be purged by trigger");

    auto new_results = mgr.searchFts5Bm25("Cyberblade");
    assert(!new_results.empty() && "New title token must match via update trigger");
    assert(new_results[0].published_file_id == 1001);

    ok = mgr.removeItem(1001);
    assert(ok && "Remove item must succeed");
    assert(mgr.totalItemCount() == 0);

    auto del_results = mgr.searchFts5Bm25("Cyberblade");
    assert(del_results.empty() && "Deleted item must be removed from FTS5 index");

    std::cout << "  -> CRUD, dirty-row file_clock guard & FTS5 trigger sync verified." << std::endl;
}

void test_adversarial_fts5_sanitization()
{
    std::cout << "[Catalog 2/7] Testing adversarial FTS5 query sanitization..." << std::endl;

    std::vector<std::string> malicious_queries = {
        "\"\"",
        "\"unclosed quote",
        "*** dangerous wildcards ***",
        "OR AND NOT NEAR",
        "::: column:filter:injection",
        "nested (parentheses (and)) syntax",
        "hello^3 caret operator",
        "trailing minus -",
        "---",
        "random ; DROP TABLE workshop_items; --",
        "special *chars* & symbols %",
    };

    for (const auto &raw : malicious_queries) {
        std::string sanitized = CatalogIndexManager::sanitizeFts5Query(raw);
        assert(!sanitized.empty() || raw.find_first_not_of(" *\"-:^()") == std::string::npos);
    }

    std::cout << "  -> Adversarial FTS5 input sanitization passed." << std::endl;
}

void test_batch_ingest_and_bm25_benchmark(CatalogIndexManager &mgr)
{
    constexpr size_t TOTAL_ITEMS = 50000;
    std::cout << "[Catalog 3/7] Ingesting " << TOTAL_ITEMS
              << " synthetic records into SQLite3 WAL..." << std::endl;

    const std::vector<std::string> themes = {"Cyberpunk", "Anime", "Sci-Fi",  "Nature",
                                             "Space",     "Retro", "Fantasy", "Abstract",
                                             "Pixel Art", "Lofi"};
    const std::vector<std::string> base_adjs = {"Neon",     "Cosmic",   "Mystic", "Luminescent",
                                                "Ethereal", "Infinite", "Solar",  "Glitch",
                                                "Cozy",     "Vibrant"};
    const std::vector<std::string> base_nouns = {"City",       "Galaxy",  "Forest", "Rain",
                                                 "Horizon",    "Nebula",  "Oasis",  "Sunset",
                                                 "Metropolis", "Wanderer"};
    std::vector<std::string> adjectives;
    std::vector<std::string> nouns;
    for (int k = 0; k < 10; ++k) {
        for (const auto &a : base_adjs)
            adjectives.push_back(a + std::to_string(k));
        for (const auto &n : base_nouns)
            nouns.push_back(n + std::to_string(k));
    }

    std::vector<WorkshopItemRecord> batch;
    batch.reserve(TOTAL_ITEMS);

    for (size_t i = 1; i <= TOTAL_ITEMS; ++i) {
        WorkshopItemRecord rec{};
        rec.published_file_id = 2000000000ULL + i;
        rec.owner_steam_id = 76561198000000000ULL + (i % 1000);

        std::string theme = themes[i % themes.size()];
        std::string adj = adjectives[(i / 7) % adjectives.size()];
        std::string noun = nouns[(i / 13) % nouns.size()];

        rec.title = (i == 4242) ? "Luminescent Cyberblade Horizon"
                                : (adj + " " + noun + " " + std::to_string(i));
        rec.type = (i % 5 == 0) ? "video" : (i % 7 == 0) ? "web" : "scene";
        rec.file_path =
            "/workshop/content/431960/" + std::to_string(rec.published_file_id) + "/project.json";
        rec.preview_path =
            "/workshop/content/431960/" + std::to_string(rec.published_file_id) + "/preview.jpg";
        rec.file_size_bytes = 1000000ULL + (i * 31ULL) % 50000000ULL;
        rec.time_created = 1600000000ULL + (i * 100ULL);
        rec.time_updated = 1700000000ULL + (i * 50ULL);
        rec.subscriptions = static_cast<uint32_t>((i * 17) % 500000);
        rec.favorited = rec.subscriptions / 5;
        rec.views = rec.subscriptions * 8;
        rec.rating = 3.0f + static_cast<float>((i % 200)) / 100.0f;
        rec.install_state = (i % 2 == 0) ? 5 : 1;
        rec.storage_backend = 0;
        rec.compat_tier = static_cast<uint32_t>(1 + (i % 4));
        rec.compat_score = static_cast<uint32_t>(70 + (i % 31));
        rec.aspect_ratio = 1.7777778f;
        rec.dominant_color = 0xFF000000 | static_cast<uint32_t>(i & 0x00FFFFFF);
        rec.tag_mask = 1ULL << (i % 32);
        rec.tags_string = theme + ", " + adj + ", 4K, 60FPS";
        rec.description = "A breathtaking " + adj + " wallpaper featuring " + noun +
                          " with high resolution textures.";

        batch.push_back(std::move(rec));
    }

    auto start_ingest = std::chrono::steady_clock::now();
    bool ok = mgr.upsertItems(batch);
    auto end_ingest = std::chrono::steady_clock::now();

    assert(ok && "Batch upsert of 50,000 items must succeed");
    assert(mgr.totalItemCount() == TOTAL_ITEMS);

    double ingest_sec = std::chrono::duration<double>(end_ingest - start_ingest).count();
    double items_per_sec = static_cast<double>(TOTAL_ITEMS) / ingest_sec;
    std::cout << "  -> Ingested " << TOTAL_ITEMS << " items in " << ingest_sec << " s ("
              << static_cast<uint64_t>(items_per_sec) << " items/sec)." << std::endl;

    std::cout << "[Catalog 4/7] Benchmarking FTS5 BM25 search across 50,000 items..." << std::endl;

    const std::vector<std::string> query_terms = {
        "Neon0",  "Cosmic1", "Mystic2", "Luminescent3", "Infinite4",
        "Solar5", "Glitch6", "Cozy7",   "Vibrant8",     "Ethereal9"};

    for (int it = 0; it < 50; ++it) {
        const std::string &q = query_terms[it % query_terms.size()];
        auto hits = mgr.searchFts5Bm25(q, 50, 0);
        (void)hits;
    }

    constexpr int BENCH_ITERATIONS = 300;
    std::vector<double> latencies_ms;
    latencies_ms.reserve(BENCH_ITERATIONS);

    for (int it = 0; it < BENCH_ITERATIONS; ++it) {
        const std::string &q = query_terms[it % query_terms.size()];
        auto t0 = std::chrono::high_resolution_clock::now();
        auto hits = mgr.searchFts5Bm25(q, 50, 0);
        auto t1 = std::chrono::high_resolution_clock::now();

        assert(!hits.empty() && "Search must find hits in synthetic catalog");
        latencies_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }

    std::sort(latencies_ms.begin(), latencies_ms.end());
    double median_ms = latencies_ms[latencies_ms.size() / 2];
    double p95_ms = latencies_ms[static_cast<size_t>(latencies_ms.size() * 0.95)];

    std::cout << "  -> FTS5 BM25 Query Latency: Median = " << median_ms << " ms | P95 = " << p95_ms
              << " ms" << std::endl;

    double budget_ms = is_sanitized_build() ? 5.0 : 0.8;
    assert(median_ms <= budget_ms && "Median BM25 search latency must meet performance budget");
}

void test_filter_sort_trigram_myers_and_bitmap(CatalogIndexManager &mgr)
{
    std::cout
        << "[Catalog 5/7] Testing covering sort indices, FTS5 trigram, Myers fuzzy & 50k bitmap..."
        << std::endl;

    CatalogFilter filter{};
    filter.type = "scene";
    filter.compat_tier = 1;
    filter.sort_order = CatalogSortOrder::SubscriptionsDesc;
    filter.limit = 20;
    filter.offset = 0;

    auto res = mgr.queryByFilters(filter);
    assert(!res.empty());
    for (size_t i = 1; i < res.size(); ++i) {
        assert(res[i].type == "scene");
        assert(res[i].compat_tier == 1);
        assert(res[i - 1].subscriptions >= res[i].subscriptions);
    }

    filter.sort_order = CatalogSortOrder::RecentlyUpdated;
    auto resUpdated = mgr.queryByFilters(filter);
    assert(!resUpdated.empty());
    for (size_t i = 1; i < resUpdated.size(); ++i) {
        assert(resUpdated[i - 1].time_updated >= resUpdated[i].time_updated);
    }

    filter.sort_order = CatalogSortOrder::TitleAsc;
    auto resTitle = mgr.queryByFilters(filter);
    assert(!resTitle.empty());

    // Substring match via FTS5 trigram ("erblad" inside "Luminescent Cyberblade Horizon")
    auto subHits = mgr.searchFts5Bm25("erblad", 10, 0);
    assert(!subHits.empty() && "FTS5 trigram must resolve mid-token substring 'erblad'");
    assert(subHits[0].published_file_id == 2000004242ULL);

    // Typo-tolerant match via 64-bit Myers bit-parallel fuzzy ranker
    auto typoHits = mgr.searchFuzzy("Luminescnt Cybrblade", 0, 10);
    assert(!typoHits.empty() && "Myers bit-parallel fuzzy search must resolve multi-char typos");
    assert(typoHits[0].published_file_id == 2000004242ULL);

    // Benchmark 50k contiguous 64-bit tag-mask bitmap scan
    std::vector<uint64_t> matchedIds;
    (void)mgr.filterByTagMaskBitmap(1ULL << 3, &matchedIds);
    assert(!matchedIds.empty());

    std::vector<double> bitmap_us;
    bitmap_us.reserve(200);
    for (int it = 0; it < 200; ++it) {
        auto t0 = std::chrono::high_resolution_clock::now();
        size_t count = mgr.filterByTagMaskBitmap(1ULL << (it % 16), nullptr);
        auto t1 = std::chrono::high_resolution_clock::now();
        assert(count > 0);
        bitmap_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    std::sort(bitmap_us.begin(), bitmap_us.end());
    double median_us = bitmap_us[bitmap_us.size() / 2];
    std::cout << "  -> 50k Tag-Bitmap Scan Median = " << median_us << " us" << std::endl;

    double bitmap_budget_us = is_sanitized_build() ? 350.0 : 25.0;
    assert(median_us <= bitmap_budget_us && "50k tag-bitmap scan must meet latency budget");
}

void test_zero_stat_delta_reconciliation()
{
    std::cout << "[Catalog 6/7] Testing zero-stat manifest delta reconciliation & io_uring STATX..."
              << std::endl;

    std::filesystem::path root = "/tmp/test_wwal_cat_delta";
    std::filesystem::path dbPath = "/tmp/test_wwal_cat_delta.db";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    cleanup_db_files(dbPath);

    std::filesystem::path wsContent = root / "content" / "431960";
    std::filesystem::create_directories(wsContent / "3001", ec);
    std::filesystem::create_directories(wsContent / "3002", ec);

    // Write project.json with nested property title before root title to verify root-only parsing
    {
        std::ofstream f1(wsContent / "3001" / "project.json");
        f1 << "{\n"
           << "  \"general\": {\n"
           << "    \"properties\": {\n"
           << "      \"schemecolor\": { \"title\": \"Nested Property Title\", \"value\": \"0 0 0\" "
              "}\n"
           << "    }\n"
           << "  },\n"
           << "  \"title\": \"Root Cyberpunk Skyline\",\n"
           << "  \"type\": \"Scene\",\n"
           << "  \"tags\": [\"Cyberpunk\", \"Sci-Fi\"]\n"
           << "}\n";
    }
    {
        std::ofstream f2(wsContent / "3002" / "project.json");
        f2 << "{ \"title\": \"Solar Nebula Loop\", \"type\": \"video\", \"tags\": [\"Space\"] }\n";
    }

    auto writeManifest = [&](uint64_t t1, uint64_t t2) {
        std::ofstream acf(root / "appworkshop_431960.acf");
        acf << "\"AppWorkshop\"\n{\n"
            << "  \"appid\" \"431960\"\n"
            << "  \"WorkshopItemsInstalled\"\n  {\n"
            << "    \"3001\"\n    {\n"
            << "      \"size\" \"4096\"\n"
            << "      \"timeupdated\" \"" << t1 << "\"\n"
            << "      \"manifest\" \"9900112233\"\n"
            << "    }\n"
            << "    \"3002\"\n    {\n"
            << "      \"size\" \"8192\"\n"
            << "      \"timeupdated\" \"" << t2 << "\"\n"
            << "      \"manifest\" \"9900112244\"\n"
            << "    }\n"
            << "  }\n}\n";
    };

    writeManifest(1710000100ULL, 1710000200ULL);

    {
        CatalogIndexManager mgr(dbPath);
        size_t indexed = mgr.reconcileDirectory(wsContent, 0);
        assert(indexed == 2);

        auto item1 = mgr.getItem(3001);
        assert(item1.has_value());
        assert(item1->title == "Root Cyberpunk Skyline" &&
               "Must extract root title rather than nested property title");
        assert(item1->manifest_id == 9900112233ULL);
        assert(item1->time_updated == 1710000100ULL);
        assert(item1->tags_string.find("Cyberpunk") != std::string::npos);
        assert(item1->file_clock == 1);

        // Warm delta pass with unchanged manifest must perform ZERO stat syscalls
        auto t0 = std::chrono::high_resolution_clock::now();
        size_t warmIndexed = mgr.reconcileDirectory(wsContent, 0);
        auto t1 = std::chrono::high_resolution_clock::now();
        double warmMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

        assert(warmIndexed == 2);
        const size_t warmStats = mgr.lastReconcileStatCount();
        assert(warmStats == 0 && "Unchanged manifest entries must trigger zero stat syscalls");
        double warmBudgetMs = is_sanitized_build() ? 25.0 : 3.0;
        assert(warmMs <= warmBudgetMs && "Warm delta reconcile must complete within budget");

        auto item1Warm = mgr.getItem(3001);
        assert(item1Warm.has_value() && item1Warm->file_clock == 1);

        // Mutate only 3002's timeupdated in manifest
        writeManifest(1710000100ULL, 1710000999ULL);
        (void)mgr.reconcileDirectory(wsContent, 0);

        auto item1After = mgr.getItem(3001);
        auto item2After = mgr.getItem(3002);
        assert(item1After.has_value() && item1After->file_clock == 1);
        assert(item2After.has_value() && item2After->time_updated == 1710000999ULL);
        assert(item2After->file_clock == 2);

        // Verify confined preview validation (reject absolute, ../ traversal, and symlinks)
        // and unmanifested directory io_uring STATX_TYPE | STATX_SIZE | STATX_MTIME sizing.
        std::filesystem::path outsideSecret = root / "outside_secret.png";
        {
            std::ofstream sec(outsideSecret);
            sec << "secret-outside-item-dir";
        }
        std::filesystem::create_directories(wsContent / "3003", ec);
        std::filesystem::create_directories(wsContent / "3004", ec);
        std::filesystem::create_directories(wsContent / "3005", ec);
        std::filesystem::create_directories(wsContent / "3006" / "thumbs", ec);

        {
            std::ofstream f3(wsContent / "3003" / "project.json");
            f3 << "{ \"title\": \"Abs Escape\", \"preview\": \"/etc/passwd\" }\n";
        }
        {
            std::ofstream f4(wsContent / "3004" / "project.json");
            f4 << "{ \"title\": \"Rel Escape\", \"preview\": \"../../../outside_secret.png\" }\n";
        }
        {
            std::ofstream f5(wsContent / "3005" / "project.json");
            f5 << "{ \"title\": \"Symlink Escape\", \"preview\": \"preview.jpg\" }\n";
            std::filesystem::create_symlink(outsideSecret, wsContent / "3005" / "preview.jpg", ec);
        }
        {
            std::ofstream f6(wsContent / "3006" / "project.json");
            f6 << "{ \"title\": \"Valid Subdir Preview\", \"preview\": \"thumbs/card.png\" }\n";
            std::ofstream img(wsContent / "3006" / "thumbs" / "card.png", std::ios::binary);
            img << std::string(512, 'P');
        }

        size_t totalAfterSecurity = mgr.reconcileDirectory(wsContent, 0);
        assert(totalAfterSecurity == 6);

        auto recAbs = mgr.getItem(3003);
        auto recRel = mgr.getItem(3004);
        auto recSym = mgr.getItem(3005);
        auto recValid = mgr.getItem(3006);
        assert(recAbs.has_value() && recAbs->preview_path.empty() &&
               "Absolute preview path in project.json must be rejected");
        assert(recRel.has_value() && recRel->preview_path.empty() &&
               "Relative ../ preview traversal must be rejected");
        assert(recSym.has_value() && recSym->preview_path.empty() &&
               "Symlink preview escaping item directory must be rejected");
        assert(recValid.has_value() && !recValid->preview_path.empty() &&
               "Valid confined relative preview file must be accepted");
        assert(recValid->file_size_bytes >= 512 &&
               "io_uring STATX_TYPE|STATX_SIZE must accumulate regular file bytes");

        std::cout << "  -> Warm zero-stat delta pass completed in " << warmMs
                  << " ms (warm_stats=" << warmStats
                  << ", unmanifested_statx=" << mgr.lastReconcileStatCount() << ")." << std::endl;
    }

    std::filesystem::remove_all(root, ec);
    cleanup_db_files(dbPath);
}

void test_wwpkg_signature_gate_in_reconcile()
{
    std::cout << "[Catalog 7/7] Testing .wwpkg Ed25519 signature verification gate..." << std::endl;

    std::filesystem::path hubDir = "/tmp/test_wwal_cat_hub";
    std::filesystem::path srcDir = "/tmp/test_wwal_cat_hub_src";
    std::filesystem::path dbPath = "/tmp/test_wwal_cat_hub.db";
    std::error_code ec;
    std::filesystem::remove_all(hubDir, ec);
    std::filesystem::remove_all(srcDir, ec);
    cleanup_db_files(dbPath);

    std::filesystem::create_directories(hubDir, ec);
    std::filesystem::create_directories(srcDir, ec);
    {
        std::ofstream shader(srcDir / "scene.frag");
        shader << "#version 300 es\nprecision highp float;\nout vec4 fragColor;\nvoid main(){ "
                  "fragColor = vec4(1.0); }\n";
    }

    uint8_t pubkey[WWPKG_PUBKEY_BYTES]{};
    uint8_t privkey[64]{};
    wwpkg_generate_keypair(pubkey, privkey);

    wwpkg_manifest_t manGood{};
    std::snprintf(manGood.id, sizeof(manGood.id), "pkg-good-001");
    std::snprintf(manGood.slug, sizeof(manGood.slug), "good-shader");
    std::snprintf(manGood.name, sizeof(manGood.name), "Verified Aurora Shader");
    std::snprintf(manGood.version, sizeof(manGood.version), "1.0.0");
    std::snprintf(manGood.author_name, sizeof(manGood.author_name), "Studio Test");
    std::snprintf(manGood.license, sizeof(manGood.license), "MIT");
    std::memcpy(manGood.author_pubkey, pubkey, WWPKG_PUBKEY_BYTES);
    manGood.type = WWPKG_TYPE_SHADER_FRAGMENT;
    std::snprintf(manGood.entrypoint_pipeline, sizeof(manGood.entrypoint_pipeline), "scene.frag");

    std::filesystem::path goodPkg = hubDir / "good.wwpkg";
    std::filesystem::path forgedSigPkg = hubDir / "forged_sig.wwpkg";
    std::filesystem::path badPkg = hubDir / "tampered.wwpkg";
    assert(
        wwpkg_build_package(srcDir.string().c_str(), &manGood, privkey, goodPkg.string().c_str()));

    // Validly compressed container whose Ed25519 signature bytes in the outer envelope are forged.
    wwpkg_manifest_t manForged = manGood;
    std::snprintf(manForged.id, sizeof(manForged.id), "pkg-forged-002");
    std::snprintf(manForged.slug, sizeof(manForged.slug), "forged-shader");
    std::snprintf(manForged.name, sizeof(manForged.name), "Forged Signature Shader");
    assert(wwpkg_build_package(srcDir.string().c_str(), &manForged, privkey,
                               forgedSigPkg.string().c_str()));
    {
        std::fstream f(forgedSigPkg, std::ios::in | std::ios::out | std::ios::binary);
        assert(f.is_open());
        f.seekg(112, std::ios::beg);
        char sigByte = 0;
        f.read(&sigByte, 1);
        sigByte ^= static_cast<char>(0x01);
        f.seekp(112, std::ios::beg);
        f.write(&sigByte, 1);
    }

    wwpkg_manifest_t manBad = manGood;
    std::snprintf(manBad.id, sizeof(manBad.id), "pkg-bad-003");
    std::snprintf(manBad.slug, sizeof(manBad.slug), "tampered-shader");
    std::snprintf(manBad.name, sizeof(manBad.name), "Tampered Payload Shader");
    assert(wwpkg_build_package(srcDir.string().c_str(), &manBad, privkey, badPkg.string().c_str()));

    // Flip a byte inside tampered.wwpkg compressed stream to verify corrupt-archive quarantine.
    {
        std::fstream f(badPkg, std::ios::in | std::ios::out | std::ios::binary);
        assert(f.is_open());
        f.seekg(0, std::ios::end);
        auto sz = f.tellg();
        assert(sz > 32);
        f.seekg(sz - static_cast<std::streamoff>(16));
        char byte = 0;
        f.read(&byte, 1);
        byte ^= static_cast<char>(0xFF);
        f.seekp(sz - static_cast<std::streamoff>(16));
        f.write(&byte, 1);
    }

    {
        CatalogIndexManager mgr(dbPath);
        size_t count = mgr.reconcileDirectory(hubDir, 2);
        assert(count == 3);

        auto goodRec = mgr.getItemByPath(goodPkg.string());
        auto forgedRec = mgr.getItemByPath(forgedSigPkg.string());
        auto badRec = mgr.getItemByPath(badPkg.string());
        assert(goodRec.has_value());
        assert(forgedRec.has_value());
        assert(badRec.has_value());

        assert(goodRec->signature_verified && goodRec->compat_tier == 1 &&
               goodRec->compat_score == 100);
        assert(!forgedRec->signature_verified && forgedRec->compat_tier == 4 &&
               forgedRec->compat_score == 0 &&
               "Forged Ed25519 signature must fail wwpkg_verify_signature and be marked blocked");
        assert(!badRec->signature_verified && badRec->compat_tier == 4 &&
               badRec->compat_score == 0 &&
               "Corrupt .wwpkg must be quarantined and marked blocked");
    }

    std::filesystem::remove_all(hubDir, ec);
    std::filesystem::remove_all(srcDir, ec);
    cleanup_db_files(dbPath);
    std::cout << "  -> .wwpkg cryptographic signature verification gate passed." << std::endl;
}

void test_concurrent_wal_pool_rcu_and_pruning()
{
    std::cout << "[Catalog 8/8] Testing concurrent WAL reader pool, lock-free RCU snapshot & "
                 "deleted-directory pruning..."
              << std::endl;

    std::filesystem::path root = "/tmp/test_wwal_cat_rcu";
    std::filesystem::path dbPath = "/tmp/test_wwal_cat_rcu.db";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    cleanup_db_files(dbPath);

    std::filesystem::path wsContent = root / "content" / "431960";
    std::filesystem::create_directories(wsContent / "4001", ec);
    std::filesystem::create_directories(wsContent / "4002", ec);

    {
        std::ofstream f1(wsContent / "4001" / "project.json");
        f1 << "{ \"title\": \"Persistent Nebula\", \"type\": \"scene\", \"tags\": [\"Space\"] }\n";
    }
    {
        std::ofstream f2(wsContent / "4002" / "project.json");
        f2 << "{ \"title\": \"Ephemeral Mirage\", \"type\": \"video\", \"tags\": [\"Retro\"] }\n";
    }
    {
        std::ofstream acf(root / "appworkshop_431960.acf");
        acf << "\"AppWorkshop\"\n{\n"
            << "  \"appid\" \"431960\"\n"
            << "  \"WorkshopItemsInstalled\"\n  {\n"
            << "    \"4001\"\n    {\n      \"size\" \"1024\"\n      \"timeupdated\" \"1710001000\"\n"
            << "      \"manifest\" \"111\"\n    }\n"
            << "    \"4002\"\n    {\n      \"size\" \"2048\"\n      \"timeupdated\" \"1710002000\"\n"
            << "      \"manifest\" \"222\"\n    }\n"
            << "  }\n}\n";
    }

    {
        CatalogIndexManager mgr(dbPath);
        assert(mgr.reconcileDirectory(wsContent, 0) == 2);
        assert(mgr.getItem(4001).has_value());
        assert(mgr.getItem(4002).has_value());

        // Verify aspect_ratio-only mutation triggers dirty-row update.
        WorkshopItemRecord arItem = *mgr.getItem(4001);
        arItem.aspect_ratio = 1.7777778f;
        assert(mgr.upsertItem(arItem));
        uint64_t clockBefore = mgr.getItem(4001)->file_clock;
        arItem.aspect_ratio = 2.3888889f;
        assert(mgr.upsertItem(arItem));
        auto arAfter = mgr.getItem(4001);
        assert(arAfter.has_value());
        assert(std::fabs(arAfter->aspect_ratio - 2.3888889f) < 1e-4f);
        assert(arAfter->file_clock == clockBefore + 1 &&
               "Aspect-ratio change must update row and advance file_clock");

        // Remove 4002 from filesystem and manifest, then verify lexical-prefix reconcile prunes it.
        std::filesystem::remove_all(wsContent / "4002", ec);
        {
            std::ofstream acf(root / "appworkshop_431960.acf", std::ios::trunc);
            acf << "\"AppWorkshop\"\n{\n"
                << "  \"appid\" \"431960\"\n"
                << "  \"WorkshopItemsInstalled\"\n  {\n"
                << "    \"4001\"\n    {\n      \"size\" \"1024\"\n"
                << "      \"timeupdated\" \"1710001000\"\n      \"manifest\" \"111\"\n    }\n"
                << "  }\n}\n";
        }
        assert(mgr.reconcileDirectory(wsContent, 0) == 1);
        assert(mgr.getItem(4001).has_value());
        assert(!mgr.getItem(4002).has_value() &&
               "Deleted Workshop subdirectory must be pruned from catalog");

        // Concurrent 4-reader WAL pool + RCU snapshot stress against active writer thread.
        std::vector<WorkshopItemRecord> seedBatch;
        seedBatch.reserve(256);
        for (uint64_t i = 0; i < 256; ++i) {
            WorkshopItemRecord rec{};
            rec.published_file_id = 800000ULL + i;
            rec.title = "Concurrent Aurora Shader " + std::to_string(i);
            rec.type = (i % 2 == 0) ? "scene" : "video";
            rec.file_path = "/concurrent/" + std::to_string(rec.published_file_id) + "/scene.pkg";
            rec.subscriptions = static_cast<uint32_t>(1000 + i * 7);
            rec.compat_tier = 1;
            rec.compat_score = 95;
            rec.tag_mask = 1ULL << (i % 8);
            rec.tags_string = "Aurora, Concurrent";
            rec.description = "Lock-free RCU reader and WAL pool verification payload";
            seedBatch.push_back(std::move(rec));
        }
        assert(mgr.upsertItems(seedBatch));

        std::atomic<bool> stopReaders{false};
        std::atomic<size_t> totalReadOps{0};
        std::vector<std::thread> readers;
        for (int r = 0; r < 4; ++r) {
            readers.emplace_back([&, r]() {
                const std::string wantType =
                    (r % 2 == 0) ? std::string("scene", 5) : std::string("video", 5);
                uint64_t iter = 0;
                while (!stopReaders.load(std::memory_order_acquire)) {
                    auto fts = mgr.searchFts5Bm25("Aurora", 20, 0);
                    assert(!fts.empty());

                    CatalogFilter cf{};
                    cf.type.assign(wantType.data(), wantType.size());
                    cf.limit = 16;
                    auto filtered = mgr.queryByFilters(cf);
                    assert(!filtered.empty());

                    auto fuzzy = mgr.searchFuzzy("Auror Shadr", 0, 10);
                    assert(!fuzzy.empty());

                    size_t bits = mgr.filterByTagMaskBitmap(1ULL << (iter % 8), nullptr);
                    assert(bits > 0);

                    (void)mgr.getItem(800000ULL + (iter % 256));
                    totalReadOps.fetch_add(1, std::memory_order_relaxed);
                    ++iter;
                }
            });
        }

        for (uint64_t w = 0; w < 80; ++w) {
            WorkshopItemRecord mut{};
            mut.published_file_id = 900000ULL + (w % 32);
            mut.title = "Dynamic Streamed Shader " + std::to_string(w);
            mut.type = "scene";
            mut.file_path = "/dynamic/" + std::to_string(mut.published_file_id) + "/scene.pkg";
            mut.subscriptions = static_cast<uint32_t>(5000 + w);
            mut.compat_tier = 1;
            mut.compat_score = 99;
            mut.tag_mask = 1ULL << (w % 8);
            mut.tags_string = "Dynamic, Stream";
            assert(mgr.upsertItem(mut));
            if (w % 5 == 4) {
                (void)mgr.removeItem(900000ULL + ((w - 2) % 32));
            }
        }

        stopReaders.store(true, std::memory_order_release);
        for (auto &t : readers) {
            t.join();
        }
        assert(totalReadOps.load() >= 4);
    }

    std::filesystem::remove_all(root, ec);
    cleanup_db_files(dbPath);
    std::cout << "  -> Concurrent 4-slot WAL pool, lock-free RCU snapshot & directory pruning "
                 "passed."
              << std::endl;
}

} // namespace

int main(int argc, char **argv)
{
    bool benchmark_mode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--benchmark") {
            benchmark_mode = true;
        }
    }
    (void)benchmark_mode;

    std::cout << "==========================================================" << std::endl;
    std::cout << "  SQLite3 WAL + FTS5 + Myers + Delta Catalog Suite        " << std::endl;
    std::cout << "==========================================================" << std::endl;

    std::filesystem::path test_db_path = "/tmp/test_waywal_catalog_bench.db";
    cleanup_db_files(test_db_path);

    {
        CatalogIndexManager mgr(test_db_path);
        assert(mgr.isOpen() && "Database must open in WAL mode");

        test_triggers_and_crud(mgr);
        test_adversarial_fts5_sanitization();
        test_batch_ingest_and_bm25_benchmark(mgr);
        test_filter_sort_trigram_myers_and_bitmap(mgr);
    }

    cleanup_db_files(test_db_path);

    test_zero_stat_delta_reconciliation();
    test_wwpkg_signature_gate_in_reconcile();
    test_concurrent_wal_pool_rcu_and_pruning();

    std::cout << "==========================================================" << std::endl;
    std::cout << "  >>> CATALOG INDEX SUITE PASSED (8/8 SUITES, 100%) <<<   " << std::endl;
    std::cout << "==========================================================" << std::endl;

    return 0;
}
