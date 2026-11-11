// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "studio/grid/fuzzy_catalog_index.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace wwal::studio {

struct WorkshopItemRecord {
    uint64_t published_file_id{0};
    uint64_t owner_steam_id{0};
    std::string title;
    std::string type{"scene"};
    std::string file_path;
    std::string preview_path;
    uint64_t file_size_bytes{0};
    uint64_t time_created{0};
    uint64_t time_updated{0};
    uint32_t subscriptions{0};
    uint32_t favorited{0};
    uint32_t views{0};
    float rating{0.0f};
    uint32_t install_state{0};
    uint32_t storage_backend{0};
    uint32_t compat_tier{1};
    uint32_t compat_score{100};
    float aspect_ratio{1.7777778f};
    uint32_t dominant_color{0x2b303c};
    uint64_t tag_mask{0};
    std::string tags_string;
    std::string description;
    uint64_t manifest_id{0};
    uint64_t file_clock{0};
    bool signature_verified{true};

    double search_rank{0.0};

    [[nodiscard]] CatalogItem toCatalogItem() const
    {
        CatalogItem item;
        item.id = static_cast<uint32_t>(published_file_id);
        item.workshop_id = std::to_string(published_file_id);
        item.title = title;
        item.path = file_path;
        item.preview_path = preview_path;
        item.file_size = file_size_bytes;
        item.date_modified = static_cast<int64_t>(time_updated);
        item.aspect_ratio = aspect_ratio;
        item.dominant_color = dominant_color;
        item.tag_mask = tag_mask;
        if (type == "video") {
            item.badge = WallpaperBadge::Video;
        } else if (type == "scene") {
            item.badge = WallpaperBadge::Scene;
        } else if (type == "web") {
            item.badge = WallpaperBadge::Image;
        } else if (type.find("shader") != std::string::npos) {
            item.badge = WallpaperBadge::Shader;
        } else {
            item.badge = WallpaperBadge::Image;
        }
        return item;
    }
};

using CatalogItemRecord = WorkshopItemRecord;

enum class CatalogSortOrder {
    SubscriptionsDesc = 0,
    RatingDesc = 1,
    RecentlyUpdated = 2,
    CompatibilityScoreDesc = 3,
    TitleAsc = 4,
    FileSizeDesc = 5,
};

struct CatalogFilter {
    std::string type;
    uint32_t compat_tier{0};
    uint64_t required_tag_mask{0};
    uint32_t install_state_mask{0};
    CatalogSortOrder sort_order{CatalogSortOrder::SubscriptionsDesc};
    int limit{50};
    int offset{0};
};

class CatalogIndexManager {
  public:
    explicit CatalogIndexManager(const std::filesystem::path &dbPath = "");
    ~CatalogIndexManager();

    CatalogIndexManager(const CatalogIndexManager &) = delete;
    CatalogIndexManager &operator=(const CatalogIndexManager &) = delete;

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] const std::filesystem::path &databasePath() const noexcept { return m_dbPath; }
    [[nodiscard]] size_t totalItemCount() const;

    [[nodiscard]] static std::string sanitizeFts5Query(std::string_view rawQuery);

    bool upsertItem(const WorkshopItemRecord &item);
    bool upsertItems(const std::vector<WorkshopItemRecord> &items);
    bool removeItem(uint64_t publishedFileId);
    [[nodiscard]] std::optional<WorkshopItemRecord> getItem(uint64_t publishedFileId) const;
    [[nodiscard]] std::optional<WorkshopItemRecord>
    getItemByPath(const std::string &filePath) const;

    // FTS5 BM25 + trigram substring + 64-bit Myers bit-parallel fuzzy fallback
    [[nodiscard]] std::vector<WorkshopItemRecord>
    searchFts5Bm25(const std::string &query, int limit = 50, int offset = 0) const;

    [[nodiscard]] std::vector<WorkshopItemRecord>
    searchFuzzy(const std::string &query, uint64_t requiredTagMask = 0, int limit = 50) const;

    // Contiguous 64-bit SoA tag-mask bitmap scan (< 25 us across 50k items)
    [[nodiscard]] size_t filterByTagMaskBitmap(uint64_t requiredMask,
                                               std::vector<uint64_t> *outIds = nullptr) const;

    [[nodiscard]] std::vector<WorkshopItemRecord> queryByFilters(const CatalogFilter &filter) const;

    [[nodiscard]] std::vector<WorkshopItemRecord>
    queryByFilters(const std::string &type, uint32_t compat_tier = 0, uint64_t tag_mask = 0,
                   CatalogSortOrder sort_order = CatalogSortOrder::SubscriptionsDesc,
                   int limit = 50, int offset = 0) const;

    size_t reconcileAllLibraries();
    size_t reconcileDirectory(const std::filesystem::path &workshopDir,
                              uint32_t storageBackend = 0);
    bool reconcileItemDirectory(const std::filesystem::path &itemDir, uint64_t publishedFileId = 0,
                                uint32_t storageBackend = 0);

    [[nodiscard]] size_t lastReconcileStatCount() const noexcept;

  private:
    void initSchema();
    void prepareStatements();
    void finalizeStatements();

    std::filesystem::path m_dbPath;
    sqlite3 *m_db{nullptr};

    sqlite3_stmt *m_stmtUpsert{nullptr};
    sqlite3_stmt *m_stmtDelete{nullptr};
    sqlite3_stmt *m_stmtGetById{nullptr};
    sqlite3_stmt *m_stmtGetByPath{nullptr};
    sqlite3_stmt *m_stmtCount{nullptr};
    sqlite3_stmt *m_stmtFts5{nullptr};
    sqlite3_stmt *m_stmtFtsTrigram{nullptr};

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wwal::studio

namespace waywal::studio {
using WorkshopItemRecord = ::wwal::studio::WorkshopItemRecord;
using CatalogItemRecord = ::wwal::studio::CatalogItemRecord;
using CatalogSortOrder = ::wwal::studio::CatalogSortOrder;
using CatalogFilter = ::wwal::studio::CatalogFilter;
using CatalogIndexManager = ::wwal::studio::CatalogIndexManager;
} // namespace waywal::studio
