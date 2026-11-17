// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WWAL_STUDIO_GRID_FUZZY_CATALOG_INDEX_HPP
#define WWAL_STUDIO_GRID_FUZZY_CATALOG_INDEX_HPP

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <waywal/thumb_cache.h>
#include <waywal/we_project.h>

namespace wwal::studio {

enum class WallpaperBadge : uint32_t {
    Video = WAYWAL_THUMB_FLAG_VIDEO,
    Shader = WAYWAL_THUMB_FLAG_SHADER,
    Scene = WAYWAL_THUMB_FLAG_SCENE,
    Favorite = WAYWAL_THUMB_FLAG_FAVORITE,
    GnomeXml = (1u << 4),
    Image = (1u << 5),
};

struct SortKeyItem {
    uint32_t item_index;
    uint16_t oklab_hue_q16;
};

// Sort up to 100k catalog entries linearly in two 8-bit passes instead of O(N log N) compares.
inline void radix_sort_oklab_hue(std::vector<SortKeyItem> &items, std::vector<SortKeyItem> &scratch)
{
    if (items.size() <= 1U) {
        return;
    }
    scratch.resize(items.size());
    std::array<uint32_t, 256> counts{};

    for (const auto &it : items) {
        counts[it.oklab_hue_q16 & 0xFFU]++;
    }
    uint32_t offset = 0U;
    for (uint32_t &c : counts) {
        const uint32_t n = c;
        c = offset;
        offset += n;
    }
    for (const auto &it : items) {
        scratch[counts[it.oklab_hue_q16 & 0xFFU]++] = it;
    }

    counts.fill(0U);
    for (const auto &it : scratch) {
        counts[(it.oklab_hue_q16 >> 8U) & 0xFFU]++;
    }
    offset = 0U;
    for (uint32_t &c : counts) {
        const uint32_t n = c;
        c = offset;
        offset += n;
    }
    for (const auto &it : scratch) {
        items[counts[(it.oklab_hue_q16 >> 8U) & 0xFFU]++] = it;
    }
}

inline void radix_sort_oklab_hue(std::vector<SortKeyItem> &items)
{
    std::vector<SortKeyItem> scratch;
    radix_sort_oklab_hue(items, scratch);
}

enum class SortCriteria {
    NameAsc,
    NameDesc,
    DateNewest,
    DateOldest,
    SizeLargest,
    SizeSmallest,
    Type,
    DominantHue
};

namespace WallpaperTags {
inline constexpr uint64_t None = 0;
inline constexpr uint64_t Anime = 1ULL << 0;
inline constexpr uint64_t Nature = 1ULL << 1;
inline constexpr uint64_t Abstract = 1ULL << 2;
inline constexpr uint64_t Cyberpunk = 1ULL << 3;
inline constexpr uint64_t Minimal = 1ULL << 4;
inline constexpr uint64_t SciFi = 1ULL << 5;
inline constexpr uint64_t Landscape = 1ULL << 6;
inline constexpr uint64_t Architecture = 1ULL << 7;
inline constexpr uint64_t Dark = 1ULL << 8;
inline constexpr uint64_t Light = 1ULL << 9;
inline constexpr uint64_t Retro = 1ULL << 10;
inline constexpr uint64_t Fantasy = 1ULL << 11;
inline constexpr uint64_t Vehicles = 1ULL << 12;
inline constexpr uint64_t Animals = 1ULL << 13;
inline constexpr uint64_t PixelArt = 1ULL << 14;
inline constexpr uint64_t Space = 1ULL << 15;
inline constexpr uint64_t Urban = 1ULL << 16;
inline constexpr uint64_t DigitalArt = 1ULL << 17;
inline constexpr uint64_t Photography = 1ULL << 18;
inline constexpr uint64_t All = ~0ULL;

struct TagDefinition {
    uint64_t mask;
    const char *name;
    const char *display_name;
};

const std::vector<TagDefinition> &allKnownTags();
uint64_t parseTagsFromString(std::string_view text);
} // namespace WallpaperTags

struct CatalogItem {
    uint32_t id{0};
    WallpaperBadge badge{WallpaperBadge::Image};
    uint32_t flags{0};
    uint64_t tag_mask{0};
    std::string title;
    std::string path;
    float aspect_ratio{16.0f / 9.0f};
    uint32_t dominant_color{0x2b303c}; // Default neutral slate
    uint64_t file_size{0};
    int64_t date_modified{0};
    std::string workshop_id;
    std::string preview_path;

    [[nodiscard]] waywal_thumb_meta_t toThumbMeta() const noexcept;
    [[nodiscard]] float computeHue() const noexcept;
};

class FuzzyCatalogIndex {
  public:
    FuzzyCatalogIndex();
    ~FuzzyCatalogIndex();

    FuzzyCatalogIndex(const FuzzyCatalogIndex &) = delete;
    FuzzyCatalogIndex &operator=(const FuzzyCatalogIndex &) = delete;
    FuzzyCatalogIndex(FuzzyCatalogIndex &&) noexcept;
    FuzzyCatalogIndex &operator=(FuzzyCatalogIndex &&) noexcept;

    // Discovers wallpapers from configured/standard paths (~/Pictures/Wallpapers,
    // $XDG_DATA_HOME/wallpapers, Steam Workshop 431960) and any additional user paths,
    // honoring excluded folders.
    size_t scanAll(const std::vector<std::string> &extra_search_paths = {});

    // Folder Inclusion / Exclusion Configuration & Persistence
    // ($XDG_CONFIG_HOME/wwal/studio_catalog.json)
    void setIncludedDirectories(const std::vector<std::string> &dirs);
    void setExcludedDirectories(const std::vector<std::string> &dirs);
    void addExcludedDirectory(const std::string &dir);
    void setScanSystemAndWorkshopPaths(bool enabled);

    [[nodiscard]] const std::vector<std::string> &includedDirectories() const noexcept
    {
        return m_includedDirs;
    }
    [[nodiscard]] const std::vector<std::string> &excludedDirectories() const noexcept
    {
        return m_excludedDirs;
    }
    [[nodiscard]] bool scanSystemAndWorkshopPaths() const noexcept { return m_scanSystemPaths; }
    [[nodiscard]] bool isPathExcluded(const std::filesystem::path &p) const;

    bool loadConfigFromFile(const std::string &file_path = "");
    bool saveConfigToFile(const std::string &file_path = "") const;
    void resetFolderConfigToDefaults();
    [[nodiscard]] static std::string defaultConfigFilePath();
    [[nodiscard]] static std::string expandUserPath(std::string_view raw_path);

    // Manual item insertion
    uint32_t addItem(CatalogItem item);
    void clear();

    // Query & Filtering
    void setSearchQuery(std::string_view query);
    void setTagMask(uint64_t tag_mask, bool match_all = true);
    void setBadgeFilter(uint32_t badge_mask);
    void setSorting(SortCriteria criteria);

    // Applies active search query, tag mask, and sort criteria, populating visible indices
    void refilter();

    // Accessors
    [[nodiscard]] size_t totalCount() const noexcept { return m_items.size(); }
    [[nodiscard]] size_t filteredCount() const noexcept { return m_filteredIndices.size(); }
    [[nodiscard]] const CatalogItem &itemAt(size_t index) const { return m_items[index]; }
    [[nodiscard]] const CatalogItem &filteredItemAt(size_t filtered_index) const
    {
        return m_items[m_filteredIndices[filtered_index]];
    }
    [[nodiscard]] uint32_t filteredIdAt(size_t filtered_index) const
    {
        return m_filteredIndices[filtered_index];
    }
    [[nodiscard]] const std::vector<uint32_t> &filteredIndices() const noexcept
    {
        return m_filteredIndices;
    }
    [[nodiscard]] const std::vector<CatalogItem> &allItems() const noexcept { return m_items; }
    [[nodiscard]] std::optional<size_t> filteredPositionOfId(uint32_t item_id) const;

    // Performance verification: benchmarks Myers/Bitap scoring across items in microseconds
    [[nodiscard]] double benchmarkSearch(std::string_view query, size_t iterations = 100) const;

    // Get last refilter duration in microseconds
    [[nodiscard]] double lastFilterDurationUs() const noexcept { return m_lastFilterDurationUs; }

    // Tag management
    [[nodiscard]] uint64_t activeTagMask() const noexcept { return m_activeTagMask; }
    [[nodiscard]] uint32_t activeBadgeMask() const noexcept { return m_activeBadgeMask; }
    [[nodiscard]] std::string_view activeSearchQuery() const noexcept { return m_activeQuery; }
    [[nodiscard]] SortCriteria activeSorting() const noexcept { return m_activeSorting; }

  private:
    std::vector<CatalogItem> m_items;
    std::vector<uint16_t> m_itemHueQ16;
    std::vector<uint32_t> m_itemBadges;
    std::vector<uint64_t> m_itemTagMasks;
    std::vector<uint32_t> m_titleOff;
    std::vector<uint32_t> m_titleLen;
    std::string m_lowerTitleArena;
    std::vector<uint64_t> m_titleCharMasks;
    std::vector<uint64_t> m_titleCharMasks2;
    std::vector<uint64_t> m_titleBigramMasks0;
    std::vector<uint64_t> m_titleBigramMasks1;
    std::vector<uint64_t> m_titleBigramMasks2;
    std::vector<uint32_t> m_pathOff;
    std::vector<uint32_t> m_pathLen;
    std::vector<uint32_t> m_pathDirLen;
    std::vector<uint32_t> m_itemDirIds;
    struct DirMeta {
        std::string lowerDir;
        uint64_t charMask{0ULL};
        uint64_t charMask2{0ULL};
    };
    std::vector<DirMeta> m_uniqueDirs;
    std::unordered_map<std::string, uint32_t> m_dirToId;
    uint32_t m_lastAddDirId{UINT32_MAX};
    std::string m_lowerPathArena;
    std::vector<uint64_t> m_pathCharMasks;
    std::vector<uint64_t> m_fileCharMasks;
    std::vector<uint64_t> m_fileCharMasks2;
    std::vector<uint64_t> m_fileBigramMasks0;
    std::vector<uint64_t> m_fileBigramMasks1;
    std::vector<uint64_t> m_fileBigramMasks2;

    std::vector<uint32_t> m_filteredIndices;
    mutable std::unordered_map<uint32_t, size_t> m_idToFilteredPos;
    mutable bool m_filteredPosDirty{true};
    mutable std::vector<SortKeyItem> m_radixKeysScratch;
    mutable std::vector<SortKeyItem> m_radixTmpScratch;
    mutable std::vector<std::pair<int, uint32_t>> m_scoredScratch;
    struct FilterWorkerPool;
    struct FilterImpl;
    friend struct FilterImpl;
    mutable std::unique_ptr<FilterWorkerPool> m_filterPool;

    std::unordered_set<std::string> m_seenPaths;
    std::unordered_map<std::string, std::string> m_sizeNameFirstPath;
    std::unordered_set<std::string> m_seenContentDigests;
    uint32_t m_nextAutoId{0U};

    std::vector<std::string> m_includedDirs;
    std::vector<std::string> m_excludedDirs;
    std::vector<std::string> m_excludedNamesLower;
    std::vector<std::string> m_excludedNormPaths;
    bool m_scanSystemPaths{true};
    bool m_configLoaded{false};

    std::string m_activeQuery;
    uint64_t m_activeTagMask{0};
    bool m_tagMatchAll{true};
    uint32_t m_activeBadgeMask{0xFFFFFFFFu};
    SortCriteria m_activeSorting{SortCriteria::NameAsc};

    double m_lastFilterDurationUs{0.0};

    // Fast Bitap / Myers bit-parallel fuzzy scorer
    static int scoreItemFuzzy(std::string_view text, std::string_view pattern,
                              const std::array<uint64_t, 256> &char_mask, uint64_t pattern_len);

    void rebuildExcludedCache();
    void scanDirectory(const std::filesystem::path &dir_path, size_t max_depth = 4);
    void scanSteamWorkshop();
    void sortFiltered();
};

} // namespace wwal::studio

#endif // WWAL_STUDIO_GRID_FUZZY_CATALOG_INDEX_HPP
