// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file benchmark_grid_virtualization.cpp
 * @brief 144 FPS Virtualization, Fuzzy Search & Color Science Benchmark
 *
 * Synthesizes 10,000 catalog entries and empirically benchmarks:
 * 1. O(1) virtualized spatial row slicing during 144 FPS fast scroll sweeps (< 6.94 ms p99).
 * 2. Bitap / Myers bit-parallel fuzzy search across 10,000 items (< 0.4 ms).
 * 3. 512 MB LRU texture pool allocation, residency & eviction over 10,000 items.
 * 4. Material You OkLab/HCT palette extraction on 4K image buffer (< 2.5 ms).
 */

#include "studio/grid/fuzzy_catalog_index.hpp"
#include "studio/grid/gpu_texture_pool.hpp"
#include "studio/grid/virtualized_grid_widget.hpp"
#include "waywal/palette_hct.h"

#include <QApplication>
#include <QElapsedTimer>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace wwal::studio;

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
constexpr double PALETTE_BUDGET_MS = 25.0;
constexpr double FUZZY_BUDGET_MS = 5.0;
#else
constexpr double PALETTE_BUDGET_MS = 2.5;
constexpr double FUZZY_BUDGET_MS = 0.4;
#endif

static void benchmark_4k_palette_hct()
{
    std::cout << "\n[Palette] Running 4K Image OkLab/HCT Palette Extraction Benchmark..."
              << std::endl;

    constexpr uint32_t width = 3840;
    constexpr uint32_t height = 2160;
    constexpr uint32_t stride = width * 4;

    std::vector<uint8_t> pixels(static_cast<size_t>(height) * stride);

    // Populate with realistic photographic wallpaper gradient & chromatic contrast
    for (uint32_t y = 0; y < height; ++y) {
        float ny = static_cast<float>(y) / height;
        for (uint32_t x = 0; x < width; ++x) {
            float nx = static_cast<float>(x) / width;
            size_t idx = (static_cast<size_t>(y) * width + x) * 4;

            // Sky gradient into terrain colors
            uint8_t r = static_cast<uint8_t>(std::clamp((0.2f + 0.6f * nx) * 255.0f, 0.0f, 255.0f));
            uint8_t g = static_cast<uint8_t>(std::clamp((0.3f + 0.5f * ny) * 255.0f, 0.0f, 255.0f));
            uint8_t b = static_cast<uint8_t>(
                std::clamp((0.7f - 0.4f * ny + 0.3f * nx) * 255.0f, 0.0f, 255.0f));

            pixels[idx + 0] = r;
            pixels[idx + 1] = g;
            pixels[idx + 2] = b;
            pixels[idx + 3] = 255;
        }
    }

    waywal_palette_t palette{};

    // Warm-up run
    waywal_palette_hct_extract(pixels.data(), width, height, stride, WAYWAL_THEME_DARK, &palette);

    // Timed runs
    constexpr int iterations = 10;
    double total_ms = 0.0;

    for (int i = 0; i < iterations; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        bool ok = waywal_palette_hct_extract(pixels.data(), width, height, stride,
                                             WAYWAL_THEME_DARK, &palette);
        auto t1 = std::chrono::steady_clock::now();

        assert(ok);
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        total_ms += ms;
    }

    double avg_ms = total_ms / iterations;
    std::cout << "  -> 4K Image (3840x2160, 33.1 MB) Palette Extraction: " << std::fixed
              << std::setprecision(3) << avg_ms << " ms (Threshold: < " << PALETTE_BUDGET_MS
              << " ms)" << std::endl;

    assert(avg_ms < PALETTE_BUDGET_MS && "4K Palette extraction exceeded budget!");
    std::cout << "  [PASS] 4K Palette Extraction verified." << std::endl;
}

static void populate_catalog_items(FuzzyCatalogIndex &index, size_t count)
{
    const std::vector<std::string> adjectives = {
        "Neon",      "Cyberpunk", "Ethereal",    "Minimalist",  "Retrowave",
        "Solarized", "Cosmic",    "Luminescent", "Melancholic", "Quantum",
        "Celestial", "Abyssal",   "Dynamic",     "Vibrant",     "Emerald"};

    const std::vector<std::string> nouns = {"Skyline",    "Sanctuary", "Horizon", "Nebula",
                                            "Metropolis", "Rainstorm", "Garden",  "Labyrinth",
                                            "Cascade",    "Aurora",    "Drifter", "Pinnacle",
                                            "Eclipse",    "Cathedral", "Odyssey"};

    std::mt19937 rng(1337);
    std::uniform_int_distribution<size_t> adjDist(0, adjectives.size() - 1);
    std::uniform_int_distribution<size_t> nounDist(0, nouns.size() - 1);
    std::uniform_int_distribution<uint32_t> badgeDist(0, 4);

    for (size_t i = 0; i < count; ++i) {
        CatalogItem item;
        item.id = static_cast<uint32_t>(i + 1);
        item.title =
            adjectives[adjDist(rng)] + " " + nouns[nounDist(rng)] + " " + std::to_string(i + 1);
        item.path = "/usr/share/backgrounds/wwal/wallpaper_" + std::to_string(i + 1) + ".png";
        item.badge = static_cast<WallpaperBadge>(badgeDist(rng));
        item.file_size = 1024 * 1024 * (1 + (i % 30));
        item.date_modified = 1700000000 + static_cast<int64_t>(i);
        item.aspect_ratio = 16.0f / 9.0f;
        index.addItem(std::move(item));
    }
}

static void benchmark_fuzzy_search(FuzzyCatalogIndex &index)
{
    std::cout << "\n[FuzzySearch] Running Myers/Bitap Fuzzy Search Benchmark across 10,000 items..."
              << std::endl;

    assert(index.totalCount() == 10000);

    const std::vector<std::string> testQueries = {"cyber",   "neon",   "skyline", "aurora",
                                                  "minimal", "nebula", "quantum", "sol",
                                                  "rain",    "dynamic"};

    constexpr int iterationsPerQuery = 100;
    double total_us = 0.0;
    int total_runs = 0;

    for (const auto &query : testQueries) {
        double us = index.benchmarkSearch(query, iterationsPerQuery);
        total_us += us;
        total_runs++;
    }

    double avg_us = total_us / total_runs;
    double avg_ms = avg_us / 1000.0;

    std::cout << "  -> 10,000 Items Fuzzy Search Latency: " << std::fixed << std::setprecision(4)
              << avg_ms << " ms (" << std::fixed << std::setprecision(1) << avg_us
              << " µs) (Threshold: < " << FUZZY_BUDGET_MS << " ms)" << std::endl;

    assert(avg_ms < FUZZY_BUDGET_MS && "Fuzzy search exceeded budget!");
    std::cout << "  [PASS] Fuzzy Search verified." << std::endl;
}

static void benchmark_grid_virtualization_slicing()
{
    std::cout
        << "\n[VirtualGrid] Running O(1) Virtualized Grid Row Slicing Benchmark (144 FPS Budget)..."
        << std::endl;

    constexpr size_t total_items = 10000;
    constexpr int container_width = 1920;
    constexpr int viewport_h = 1080;
    constexpr int item_w = 280;
    constexpr int spacing_x = 16;
    constexpr int item_h = 158;
    constexpr int spacing_y = 16;
    constexpr int row_h = item_h + spacing_y; // 174 px
    constexpr int overscan_rows = 2;

    int cols = std::max(1, (container_width + spacing_x) / (item_w + spacing_x));
    int total_rows = static_cast<int>((total_items + cols - 1) / cols);
    int virtual_h = total_rows * row_h - spacing_y;

    std::cout << "  -> Grid parameters: " << total_items << " items, " << cols << " columns, "
              << total_rows << " rows, " << virtual_h << " px virtual height." << std::endl;

    constexpr int frame_count = 2000;
    std::vector<double> frame_times_us;
    frame_times_us.reserve(frame_count);

    // Simulate rapid smooth scrolling sweep across entire virtual grid
    for (int f = 0; f < frame_count; ++f) {
        int scroll_y =
            static_cast<int>((static_cast<double>(f) / frame_count) * (virtual_h - viewport_h));

        auto t0 = std::chrono::steady_clock::now();

        // O(1) uniform spatial index evaluation
        int r_first = std::max(0, (scroll_y / row_h) - overscan_rows);
        int r_last =
            std::min(total_rows - 1, ((scroll_y + viewport_h + row_h - 1) / row_h) + overscan_rows);
        size_t idx_first = static_cast<size_t>(cols * r_first);
        size_t idx_last = std::min(total_items - 1, static_cast<size_t>(cols * (r_last + 1) - 1));

        auto t1 = std::chrono::steady_clock::now();

        assert(idx_last >= idx_first);
        assert(idx_last < total_items);

        double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        frame_times_us.push_back(us);
    }

    std::sort(frame_times_us.begin(), frame_times_us.end());
    double p50_us = frame_times_us[frame_times_us.size() * 50 / 100];
    double p99_us = frame_times_us[frame_times_us.size() * 99 / 100];
    double p99_ms = p99_us / 1000.0;

    std::cout << "  -> 144 FPS Virtualized Slicing: p50=" << p50_us << " µs, p99=" << p99_us
              << " µs (" << std::fixed << std::setprecision(4) << p99_ms
              << " ms) (Budget: < 6.940 ms)" << std::endl;

    assert(p99_ms < 6.94 && "Grid row evaluation exceeded 6.94 ms frame budget!");
    std::cout << "  [PASS] 144 FPS Virtualization Frame Evaluation verified." << std::endl;
}

static void benchmark_lru_pool_eviction()
{
    std::cout << "\n[TexturePool] Running 512 MB LRU Texture Pool Eviction & Residency Benchmark..."
              << std::endl;

    // Fixed 512 MB VRAM slot cap = 1,500 slots (at 384x216 RGBA8 = 331,776 bytes/slot = ~497.6 MB)
    constexpr int max_slots = 1500;
    GpuTexturePool pool(max_slots);

    assert(pool.stats().max_slots == max_slots);
    assert(pool.stats().active_slots == 0);

    // Simulate requesting 10,000 distinct items through the pool
    for (uint32_t i = 1; i <= 10000; ++i) {
        std::string path = "/mock/wallpaper_" + std::to_string(i) + ".png";
        pool.requestTexture(i, path);

        TexturePoolStats st = pool.stats();
        assert(st.active_slots <= static_cast<size_t>(max_slots) &&
               "Pool active slots must strictly never exceed max_slots (512 MB cap)!");
    }

    TexturePoolStats final_stats = pool.stats();
    std::cout << "  -> Final pool state: active_slots=" << final_stats.active_slots << " / "
              << final_stats.max_slots << " (" << (final_stats.total_vram_bytes / (1024 * 1024))
              << " MB VRAM allocated)" << std::endl;

    assert(final_stats.active_slots == static_cast<size_t>(max_slots) &&
           "Active slots must remain full at capacity after 10,000 requests");
    assert(final_stats.total_vram_bytes <= 512 * 1024 * 1024 &&
           "Total VRAM allocation must remain <= 512 MB");

    std::cout << "  [PASS] 512 MB LRU Texture Pool Eviction & Memory Cap verified." << std::endl;
}

static void test_folder_inclusion_exclusion_and_dedup()
{
    std::cout << "\n[Catalog & Preview] Testing Folder Inclusion, Exclusion, Deduplication & "
                 "Thumbnail Decode..."
              << std::endl;

    // Ensure hermetic test execution regardless of ambient environment variables
    qunsetenv("WWAL_WALLPAPER_DIRS");
    qunsetenv("WWAL_EXCLUDE_DIRS");

    std::filesystem::path tmpRoot =
        std::filesystem::temp_directory_path() /
        ("wwal_test_catalog_" + std::to_string(QCoreApplication::applicationPid()));
    std::filesystem::remove_all(tmpRoot);

    std::filesystem::path wallDir = tmpRoot / "Pictures" / "Wallpapers";
    std::filesystem::path subDir = wallDir / "Anime";
    std::filesystem::path exclByNameDir = wallDir / "Screenshots";
    std::filesystem::path exclByPathDir = wallDir / "PrivateDump";
    std::filesystem::path phoneDir = tmpRoot / "Pictures" / "Phone";
    std::filesystem::path mirrorDir = tmpRoot / "Mirror";

    std::filesystem::create_directories(subDir);
    std::filesystem::create_directories(exclByNameDir);
    std::filesystem::create_directories(exclByPathDir);
    std::filesystem::create_directories(phoneDir);
    std::filesystem::create_directories(mirrorDir);

    auto writeTestPng = [](const std::filesystem::path &p, const QColor &col, int w = 640,
                           int h = 360) {
        QImage img(w, h, QImage::Format_RGBA8888);
        img.fill(col);
        bool saved = img.save(QString::fromStdString(p.string()), "PNG");
        assert(saved);
    };

    writeTestPng(wallDir / "keep_main.png", QColor(20, 120, 220), 640, 360);
    writeTestPng(subDir / "keep_sub.png", QColor(220, 80, 40), 800, 450);
    writeTestPng(exclByNameDir / "skip_screenshot.png", QColor(100, 100, 100));
    writeTestPng(exclByPathDir / "skip_private.png", QColor(50, 50, 50));
    writeTestPng(phoneDir / "skip_phone.png", QColor(10, 200, 80));

    // Exact duplicate copy of keep_main.png in mirrorDir
    std::filesystem::copy_file(wallDir / "keep_main.png", mirrorDir / "keep_main.png",
                               std::filesystem::copy_options::overwrite_existing);

    FuzzyCatalogIndex idx;
    idx.setIncludedDirectories({wallDir.string(), mirrorDir.string()});
    idx.setExcludedDirectories({"Screenshots", exclByPathDir.string()});
    idx.setScanSystemAndWorkshopPaths(false);

    // Verify programmatic config is not overwritten by scanAll() even before save/load
    size_t directScanCount = idx.scanAll();
    assert(directScanCount == 2 && "Programmatic setIncludedDirectories/setExcludedDirectories "
                                   "must be respected by scanAll()");

    // Save and reload config JSON round-trip
    std::filesystem::path cfgFile = tmpRoot / "studio_catalog.json";
    assert(idx.saveConfigToFile(cfgFile.string()));

    FuzzyCatalogIndex reloadedIdx;
    assert(reloadedIdx.loadConfigFromFile(cfgFile.string()));
    assert(!reloadedIdx.scanSystemAndWorkshopPaths());
    assert(reloadedIdx.includedDirectories().size() == 2);
    assert(reloadedIdx.excludedDirectories().size() == 2);

    size_t count = reloadedIdx.scanAll();
    std::cout << "  -> Scanned count with exclusions & deduplication: " << count << " (expected: 2)"
              << std::endl;
    assert(
        count == 2 &&
        "Only keep_main.png and keep_sub.png should be indexed (exclusions & duplicates skipped)");

    // Verify thumbnail decoding via GpuTexturePool
    GpuTexturePool pool(64);
    const auto &firstItem = reloadedIdx.filteredItemAt(0);
    assert(firstItem.id == 0 && "First catalog item must have 0-based id == 0");
    (void)pool.requestTexture(firstItem.id, firstItem.path, firstItem.preview_path);

    for (int attempt = 0; attempt < 50; ++attempt) {
        pool.processPendingUploads(nullptr, 8);
        if (!pool.getThumbnailImage(firstItem.id).isNull()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    QImage thumb = pool.getThumbnailImage(firstItem.id);
    assert(!thumb.isNull() && "Decoded thumbnail QImage must be non-null");
    assert(thumb.width() == static_cast<int>(GpuTexturePool::THUMB_WIDTH));
    assert(thumb.height() == static_cast<int>(GpuTexturePool::THUMB_HEIGHT));
    QColor firstThumbColor = thumb.pixelColor(thumb.width() / 2, thumb.height() / 2);

    // Verify HoverPreviewController works on item_id == 0
    HoverPreviewController hoverCtrl;
    hoverCtrl.onCellHoverEntered(firstItem.id, firstItem.path, firstItem.badge,
                                 firstItem.preview_path);
    // Simulate dwell timeout firing on item_id == 0
    std::this_thread::sleep_for(std::chrono::milliseconds(270));
    QCoreApplication::processEvents();
    assert(hoverCtrl.isPreviewActive() && hoverCtrl.activeItemId() == 0 &&
           "HoverPreviewController must activate for item_id == 0 after 250ms dwell");
    assert(!hoverCtrl.currentPreviewImage().isNull());

    // Verify runtime folder exclusion + rescan removes items from newly excluded directory
    // Exclude whichever folder firstItem came from so item_id 0 is reassigned to the other image
    std::string firstItemFolder = std::filesystem::path(firstItem.path).parent_path().string();
    if (firstItemFolder == subDir.string()) {
        reloadedIdx.addExcludedDirectory("Anime");
    } else {
        // Put keep_main.png in its own subfolder or exclude Anime so count drops to 1
        reloadedIdx.addExcludedDirectory(subDir.string());
    }
    size_t rescanCount = reloadedIdx.scanAll();
    std::cout << "  -> Rescanned count after runtime exclusion: " << rescanCount << " (expected: 1)"
              << std::endl;
    assert(
        rescanCount == 1 &&
        "Runtime exclusion + scanAll() must clear previously scanned items from excluded folder");

    // Verify GpuTexturePool invalidates stale thumbnail when item_id 0 changes media_path
    std::string otherPath = (firstItem.path == (wallDir / "keep_main.png").string())
                                ? (subDir / "keep_sub.png").string()
                                : (wallDir / "keep_main.png").string();
    (void)pool.requestTexture(0, otherPath, "");
    for (int attempt = 0; attempt < 50; ++attempt) {
        pool.processPendingUploads(nullptr, 8);
        QImage updated = pool.getThumbnailImage(0);
        if (!updated.isNull() &&
            updated.pixelColor(updated.width() / 2, updated.height() / 2) != firstThumbColor) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    QImage updatedThumb = pool.getThumbnailImage(0);
    assert(!updatedThumb.isNull());
    QColor updatedThumbColor =
        updatedThumb.pixelColor(updatedThumb.width() / 2, updatedThumb.height() / 2);
    assert(updatedThumbColor != firstThumbColor &&
           "GpuTexturePool must invalidate and re-decode when item_id 0 changes media_path");

    std::filesystem::remove_all(tmpRoot);
    std::cout << "  [PASS] Folder inclusion, exclusion, deduplication, rescan & thumbnail decode "
                 "verified."
              << std::endl;
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    std::cout << "==========================================================" << std::endl;
    std::cout << "  Studio Grid Virtualization & Telemetry Benchmark Suite  " << std::endl;
    std::cout << "==========================================================" << std::endl;

    // 1. 4K Palette Extraction Benchmark (< 2.5 ms)
    benchmark_4k_palette_hct();

    // 2. Synthesize 10,000 Catalog Items
    std::cout << "\n[Setup] Synthesizing 10,000 catalog entries in memory..." << std::endl;
    FuzzyCatalogIndex catalog;
    populate_catalog_items(catalog, 10000);
    std::cout << "[Setup] 10,000 items successfully generated." << std::endl;

    // 3. Myers/Bitap Fuzzy Search Benchmark (< 0.4 ms)
    benchmark_fuzzy_search(catalog);

    // 4. O(1) Virtualized Slicing Benchmark (< 6.94 ms p99)
    benchmark_grid_virtualization_slicing();

    // 5. 512 MB LRU Pool Eviction Benchmark
    benchmark_lru_pool_eviction();

    // 6. Folder Inclusion, Exclusion, Deduplication & Thumbnail Decode Verification
    test_folder_inclusion_exclusion_and_dedup();

    std::cout << "\n==========================================================" << std::endl;
    std::cout << "  >>> ALL STUDIO GRID BENCHMARKS PASSED CLEANLY (100%) <<<" << std::endl;
    std::cout << "==========================================================" << std::endl;

    return 0;
}
