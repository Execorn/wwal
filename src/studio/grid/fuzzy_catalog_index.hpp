// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WWAL_STUDIO_GRID_FUZZY_CATALOG_INDEX_HPP
#define WWAL_STUDIO_GRID_FUZZY_CATALOG_INDEX_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <chrono>
#include <string_view>
#include <filesystem>
#include <functional>

#include <waywal/thumb_cache.h>
#include <waywal/we_project.h>

#include <unordered_set>

namespace wwal::studio {

enum class WallpaperBadge : uint32_t {
    Image = 0,
    Video = WAYWAL_THUMB_FLAG_VIDEO,
    Shader = WAYWAL_THUMB_FLAG_SHADER,
    Scene = WAYWAL_THUMB_FLAG_SCENE,
    GnomeXml = (1u << 4),
    Favorite = WAYWAL_THUMB_FLAG_FAVORITE,
};

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
    ~FuzzyCatalogIndex() = default;

    FuzzyCatalogIndex(const FuzzyCatalogIndex &) = delete;
    FuzzyCatalogIndex &operator=(const FuzzyCatalogIndex &) = delete;
    FuzzyCatalogIndex(FuzzyCatalogIndex &&) noexcept = default;
    FuzzyCatalogIndex &operator=(FuzzyCatalogIndex &&) noexcept = default;

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

    // Performance verification: benchmarks Myers/Bitap scoring across items in microseconds
    [[nodiscard]] double benchmarkSearch(std::string_view query, size_t iterations = 100) const;

    // Get last refilter duration in microseconds
    [[nodiscard]] double lastFilterDurationUs() const noexcept { return m_lastFilterDurationUs; }

    // Tag management
    [[nodiscard]] uint64_t activeTagMask() const noexcept { return m_activeTagMask; }
    [[nodiscard]] std::string_view activeSearchQuery() const noexcept { return m_activeQuery; }
    [[nodiscard]] SortCriteria activeSorting() const noexcept { return m_activeSorting; }

  private:
    std::vector<CatalogItem> m_items;
    std::vector<uint32_t> m_filteredIndices;
    std::unordered_set<std::string> m_seenPaths;

    std::vector<std::string> m_includedDirs;
    std::vector<std::string> m_excludedDirs;
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

    void scanDirectory(const std::filesystem::path &dir_path, size_t max_depth = 4);
    void scanSteamWorkshop();
    void sortFiltered();
};

} // namespace wwal::studio

#endif // WWAL_STUDIO_GRID_FUZZY_CATALOG_INDEX_HPP
