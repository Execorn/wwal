// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <sqlite3.h>

namespace wwal::studio {

struct alignas(64) CatalogSoaSnapshot {
    size_t count{0};
    std::vector<uint64_t> published_ids;
    std::vector<uint64_t> tag_masks;
    std::array<std::vector<uint64_t>, 64> tag_bitsets;
    std::vector<uint32_t> title_offsets;
    std::vector<uint32_t> title_lengths;
    std::string lower_title_arena;
    std::vector<uint32_t> tags_offsets;
    std::vector<uint32_t> tags_lengths;
    std::string lower_tags_arena;
    std::vector<uint16_t> desc_lengths;

    [[nodiscard]] std::string_view lowerTitleAt(size_t i) const noexcept
    {
        return std::string_view(lower_title_arena.data() + title_offsets[i], title_lengths[i]);
    }

    [[nodiscard]] std::string_view lowerTagsAt(size_t i) const noexcept
    {
        return std::string_view(lower_tags_arena.data() + tags_offsets[i], tags_lengths[i]);
    }
};

class ReadOnlySqlitePool {
  public:
    static constexpr size_t kPoolSize = 4;

    struct ReadSlot {
        sqlite3 *db{nullptr};
        sqlite3_stmt *stmtFtsBm25{nullptr};
        sqlite3_stmt *stmtTrigram{nullptr};
        sqlite3_stmt *stmtGetById{nullptr};
        sqlite3_stmt *stmtGetByPath{nullptr};
        sqlite3_stmt *stmtCount{nullptr};
    };

    class Lease {
      public:
        Lease(ReadOnlySqlitePool &pool, size_t idx) noexcept : m_pool(&pool), m_idx(idx) {}
        ~Lease()
        {
            if (m_pool) {
                m_pool->release(m_idx);
            }
        }
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        Lease(Lease &&other) noexcept : m_pool(other.m_pool), m_idx(other.m_idx)
        {
            other.m_pool = nullptr;
        }
        Lease &operator=(Lease &&) = delete;

        [[nodiscard]] ReadSlot &slot() noexcept { return m_pool->m_slots[m_idx]; }

      private:
        ReadOnlySqlitePool *m_pool{nullptr};
        size_t m_idx{0};
    };

    ReadOnlySqlitePool() = default;
    ~ReadOnlySqlitePool() { close(); }

    ReadOnlySqlitePool(const ReadOnlySqlitePool &) = delete;
    ReadOnlySqlitePool &operator=(const ReadOnlySqlitePool &) = delete;

    bool open(const std::filesystem::path &dbPath, bool hasTrigramFts)
    {
        close();
        if (dbPath.empty() || dbPath == ":memory:") {
            return false;
        }

        static constexpr const char *kSqlFtsBm25 = R"(
            SELECT i.published_file_id, i.owner_steam_id, i.title, i.type, i.file_path, i.preview_path,
                   i.file_size_bytes, i.time_created, i.time_updated, i.subscriptions, i.favorited,
                   i.views, i.rating, i.install_state, i.storage_backend, i.compat_tier, i.compat_score,
                   i.aspect_ratio, i.dominant_color, i.tag_mask, i.tags_string, i.description,
                   i.manifest_id, i.file_clock, i.signature_verified,
                   f.rank_score
            FROM (
                SELECT rowid, bm25(workshop_fts, 10.0, 5.0, 1.0) AS rank_score
                FROM workshop_fts
                WHERE workshop_fts MATCH ?1
                ORDER BY rank_score ASC, rowid ASC
                LIMIT ?2 OFFSET ?3
            ) f
            JOIN workshop_items i ON i.published_file_id = f.rowid
            ORDER BY f.rank_score ASC, i.published_file_id ASC;
        )";

        static constexpr const char *kSqlTrigram = R"(
            SELECT i.published_file_id, i.owner_steam_id, i.title, i.type, i.file_path, i.preview_path,
                   i.file_size_bytes, i.time_created, i.time_updated, i.subscriptions, i.favorited,
                   i.views, i.rating, i.install_state, i.storage_backend, i.compat_tier, i.compat_score,
                   i.aspect_ratio, i.dominant_color, i.tag_mask, i.tags_string, i.description,
                   i.manifest_id, i.file_clock, i.signature_verified,
                   f.rank
            FROM (
                SELECT rowid, bm25(workshop_fts_trigram) AS rank
                FROM workshop_fts_trigram
                WHERE workshop_fts_trigram MATCH ?1
                ORDER BY rank ASC, rowid ASC
                LIMIT ?2 OFFSET ?3
            ) f
            JOIN workshop_items i ON f.rowid = i.published_file_id
            ORDER BY f.rank ASC, i.published_file_id ASC;
        )";

        static constexpr const char *kSqlGetById = R"(
            SELECT published_file_id, owner_steam_id, title, type, file_path, preview_path,
                   file_size_bytes, time_created, time_updated, subscriptions, favorited,
                   views, rating, install_state, storage_backend, compat_tier, compat_score,
                   aspect_ratio, dominant_color, tag_mask, tags_string, description,
                   manifest_id, file_clock, signature_verified
            FROM workshop_items WHERE published_file_id = ?1;
        )";

        static constexpr const char *kSqlGetByPath = R"(
            SELECT published_file_id, owner_steam_id, title, type, file_path, preview_path,
                   file_size_bytes, time_created, time_updated, subscriptions, favorited,
                   views, rating, install_state, storage_backend, compat_tier, compat_score,
                   aspect_ratio, dominant_color, tag_mask, tags_string, description,
                   manifest_id, file_clock, signature_verified
            FROM workshop_items WHERE file_path = ?1;
        )";

        static constexpr const char *kSqlCount = "SELECT COUNT(*) FROM workshop_items;";

        const std::string pathStr = dbPath.string();
        uint32_t mask = 0;
        for (size_t i = 0; i < kPoolSize; ++i) {
            if (sqlite3_open_v2(pathStr.c_str(), &m_slots[i].db,
                                SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
                close();
                return false;
            }
            sqlite3_busy_timeout(m_slots[i].db, 50);
            sqlite3_exec(m_slots[i].db,
                         "PRAGMA query_only = ON; PRAGMA temp_store = MEMORY; "
                         "PRAGMA cache_size = -32768; PRAGMA mmap_size = 268435456;",
                         nullptr, nullptr, nullptr);

            if (sqlite3_prepare_v3(m_slots[i].db, kSqlFtsBm25, -1, SQLITE_PREPARE_PERSISTENT,
                                   &m_slots[i].stmtFtsBm25, nullptr) != SQLITE_OK ||
                sqlite3_prepare_v3(m_slots[i].db, kSqlGetById, -1, SQLITE_PREPARE_PERSISTENT,
                                   &m_slots[i].stmtGetById, nullptr) != SQLITE_OK ||
                sqlite3_prepare_v3(m_slots[i].db, kSqlGetByPath, -1, SQLITE_PREPARE_PERSISTENT,
                                   &m_slots[i].stmtGetByPath, nullptr) != SQLITE_OK ||
                sqlite3_prepare_v3(m_slots[i].db, kSqlCount, -1, SQLITE_PREPARE_PERSISTENT,
                                   &m_slots[i].stmtCount, nullptr) != SQLITE_OK) {
                close();
                return false;
            }

            if (hasTrigramFts) {
                sqlite3_prepare_v3(m_slots[i].db, kSqlTrigram, -1, SQLITE_PREPARE_PERSISTENT,
                                   &m_slots[i].stmtTrigram, nullptr);
            }
            mask |= static_cast<uint32_t>(1u << i);
        }

        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_freeMask = mask;
        }
        m_open.store(true, std::memory_order_release);
        return true;
    }

    void close() noexcept
    {
        m_open.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lk(m_mtx);
        m_freeMask = 0;
        for (auto &s : m_slots) {
            if (s.stmtFtsBm25) {
                sqlite3_finalize(s.stmtFtsBm25);
                s.stmtFtsBm25 = nullptr;
            }
            if (s.stmtTrigram) {
                sqlite3_finalize(s.stmtTrigram);
                s.stmtTrigram = nullptr;
            }
            if (s.stmtGetById) {
                sqlite3_finalize(s.stmtGetById);
                s.stmtGetById = nullptr;
            }
            if (s.stmtGetByPath) {
                sqlite3_finalize(s.stmtGetByPath);
                s.stmtGetByPath = nullptr;
            }
            if (s.stmtCount) {
                sqlite3_finalize(s.stmtCount);
                s.stmtCount = nullptr;
            }
            if (s.db) {
                sqlite3_close_v2(s.db);
                s.db = nullptr;
            }
        }
    }

    [[nodiscard]] bool isOpen() const noexcept { return m_open.load(std::memory_order_acquire); }

    [[nodiscard]] Lease acquire()
    {
        std::unique_lock<std::mutex> lk(m_mtx);
        m_cv.wait(lk, [this] { return m_freeMask != 0u; });
        const size_t idx = static_cast<size_t>(std::countr_zero(m_freeMask));
        m_freeMask &= ~(1u << idx);
        return Lease(*this, idx);
    }

  private:
    void release(size_t idx) noexcept
    {
        auto &s = m_slots[idx];
        // Reset persistent statements before releasing the slot so no reader retains an old WAL
        // snapshot across calls.
        if (s.stmtFtsBm25) {
            sqlite3_reset(s.stmtFtsBm25);
        }
        if (s.stmtTrigram) {
            sqlite3_reset(s.stmtTrigram);
        }
        if (s.stmtGetById) {
            sqlite3_reset(s.stmtGetById);
        }
        if (s.stmtGetByPath) {
            sqlite3_reset(s.stmtGetByPath);
        }
        if (s.stmtCount) {
            sqlite3_reset(s.stmtCount);
        }
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_freeMask |= static_cast<uint32_t>(1u << idx);
        }
        m_cv.notify_one();
    }

    std::array<ReadSlot, kPoolSize> m_slots{};
    std::mutex m_mtx;
    std::condition_variable m_cv;
    uint32_t m_freeMask{0};
    std::atomic<bool> m_open{false};
};

} // namespace wwal::studio
