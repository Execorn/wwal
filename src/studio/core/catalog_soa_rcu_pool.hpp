// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sqlite3.h>

namespace wwal::studio {

// Notify drain waiters under mu_ so callers deleting the pool after close_and_drain() cannot race
// release().
class SqlitePoolShutdownGate {
  public:
    bool try_acquire() noexcept
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (closing_ || active_ == std::numeric_limits<uint64_t>::max()) {
            return false;
        }
        ++active_;
        return true;
    }

    bool release() noexcept
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (active_ == 0ULL) {
            return false;
        }
        --active_;
        if (closing_ && active_ == 0ULL) {
            cv_.notify_all();
        }
        return true;
    }

    void close_and_drain() noexcept
    {
        std::unique_lock<std::mutex> lk(mu_);
        closing_ = true;
        cv_.wait(lk, [this]() noexcept { return active_ == 0ULL; });
    }

    bool reopen() noexcept
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!closing_ || active_ != 0ULL) {
            return false;
        }
        closing_ = false;
        return true;
    }

    [[nodiscard]] uint64_t active_count() const noexcept
    {
        std::lock_guard<std::mutex> lk(mu_);
        return active_;
    }

  private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    uint64_t active_{0ULL};
    bool closing_{false};
};

struct SegmentPosting {
    double singleScore{0.0};
    uint16_t localIdx{0U};
    uint16_t tfTitle{0U};
    uint16_t tfTags{0U};
    uint16_t tfDesc{0U};
};

struct SegmentTokenDictEntry {
    uint32_t tokOff{0U};
    uint32_t tokLen{0U};
    uint32_t postBegin{0U};
    uint32_t postLen{0U};
};

// Immutable per-segment inverted index allowing O(K) top-K BM25 reads across 4096-doc chunks.
struct SegmentTokenIndex {
    std::string tokenArena;
    std::vector<SegmentTokenDictEntry> dict;
    std::vector<SegmentPosting> postings;

    [[nodiscard]] std::string_view tokenAt(const SegmentTokenDictEntry &e) const noexcept
    {
        return std::string_view(tokenArena.data() + e.tokOff, e.tokLen);
    }
};

// Fixed 4096-entry immutable SoA segment so single-item RCU upserts copy <= 1 chunk instead of all
// 100k rows.
struct alignas(64) ChunkedCatalogSegment {
    static constexpr std::size_t kChunkLen = 4096U;
    static constexpr std::size_t kBitsetWords = kChunkLen / 64U;

    uint32_t count{0U};
    std::array<uint64_t, kChunkLen> published_ids{};
    std::array<uint64_t, kChunkLen> tag_masks{};
    std::array<std::array<uint64_t, kBitsetWords>, 64> tag_bitsets{};

    static constexpr uint32_t kDeltaOffsetBit = 0x80000000U;

    std::array<uint32_t, kChunkLen> title_offsets{};
    std::array<uint32_t, kChunkLen> title_lengths{};
    std::shared_ptr<const std::string> base_title_arena{};
    std::string lower_title_arena{};

    std::array<uint32_t, kChunkLen> tags_offsets{};
    std::array<uint32_t, kChunkLen> tags_lengths{};
    std::shared_ptr<const std::string> base_tags_arena{};
    std::string lower_tags_arena{};

    std::array<uint32_t, kChunkLen> desc_offsets{};
    std::array<uint32_t, kChunkLen> desc_lengths{};
    std::shared_ptr<const std::string> base_desc_arena{};
    std::string lower_desc_arena{};

    std::array<uint64_t, kChunkLen> doc_bloom{};

    uint32_t dead_title_bytes{0U};
    uint32_t dead_tags_bytes{0U};
    uint32_t dead_desc_bytes{0U};

    // Shared immutable token index + bounded inline dirty overlay so single upserts avoid
    // copying string arenas or vector allocations.
    std::shared_ptr<const SegmentTokenIndex> token_index{};
    std::array<uint64_t, kBitsetWords> dirty_bitset{};
    std::array<uint16_t, 256> dirty_slots{};
    uint16_t dirty_count{0U};

    void markSlotDirty(std::size_t localIdx) noexcept
    {
        const std::size_t word = localIdx >> 6U;
        const uint64_t bit = 1ULL << (localIdx & 63U);
        if ((dirty_bitset[word] & bit) == 0ULL) {
            dirty_bitset[word] |= bit;
            if (dirty_count < dirty_slots.size()) {
                dirty_slots[dirty_count++] = static_cast<uint16_t>(localIdx);
            }
        }
    }

    [[nodiscard]] bool isSlotDirty(std::size_t localIdx) const noexcept
    {
        return (dirty_bitset[localIdx >> 6U] & (1ULL << (localIdx & 63U))) != 0ULL;
    }

    [[nodiscard]] static std::string_view
    readArenaSlot(const std::shared_ptr<const std::string> &base, const std::string &delta,
                  uint32_t encodedOff, uint32_t len) noexcept
    {
        if (len == 0U) {
            return {};
        }
        if ((encodedOff & kDeltaOffsetBit) != 0U || !base) {
            return std::string_view(delta.data() + (encodedOff & ~kDeltaOffsetBit), len);
        }
        return std::string_view(base->data() + encodedOff, len);
    }

    [[nodiscard]] std::string_view lowerTitleAt(std::size_t localIdx) const noexcept
    {
        return readArenaSlot(base_title_arena, lower_title_arena, title_offsets[localIdx],
                             title_lengths[localIdx]);
    }

    [[nodiscard]] std::string_view lowerTagsAt(std::size_t localIdx) const noexcept
    {
        return readArenaSlot(base_tags_arena, lower_tags_arena, tags_offsets[localIdx],
                             tags_lengths[localIdx]);
    }

    [[nodiscard]] std::string_view lowerDescAt(std::size_t localIdx) const noexcept
    {
        return readArenaSlot(base_desc_arena, lower_desc_arena, desc_offsets[localIdx],
                             desc_lengths[localIdx]);
    }
};

// Wait-free QSBR snapshot readers pin epochs on lock-free pointer loads instead of atomic
// shared_ptr.
class ChunkedCatalogSnapshot {
  public:
    using SegmentPtr = std::shared_ptr<const ChunkedCatalogSegment>;
    using SegmentList = std::vector<SegmentPtr>;

    struct Snapshot {
        uint64_t epoch{1ULL};
        std::size_t total_count{0U};
        SegmentList segments{};
    };

    class ReadGuard {
      public:
        ReadGuard() noexcept = default;
        ReadGuard(ReadGuard &&other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)),
              snap_(std::exchange(other.snap_, nullptr)),
              slot_idx_(std::exchange(other.slot_idx_, -2))
        {
        }

        ReadGuard &operator=(ReadGuard &&other) noexcept
        {
            if (this != &other) {
                reset();
                owner_ = std::exchange(other.owner_, nullptr);
                snap_ = std::exchange(other.snap_, nullptr);
                slot_idx_ = std::exchange(other.slot_idx_, -2);
            }
            return *this;
        }

        ReadGuard(const ReadGuard &) = delete;
        ReadGuard &operator=(const ReadGuard &) = delete;

        ~ReadGuard() { reset(); }

        void reset() noexcept
        {
            if (owner_ != nullptr) {
                if (slot_idx_ >= 0) {
                    owner_->reader_slots_[static_cast<std::size_t>(slot_idx_)].state.store(
                        0ULL, std::memory_order_release);
                } else if (slot_idx_ == -1) {
                    owner_->fallback_pins_.fetch_sub(1ULL, std::memory_order_acq_rel);
                }
            }
            owner_ = nullptr;
            snap_ = nullptr;
            slot_idx_ = -2;
        }

        [[nodiscard]] std::size_t total_count() const noexcept
        {
            return snap_ != nullptr ? snap_->total_count : 0U;
        }

        [[nodiscard]] const SegmentList &segments() const noexcept
        {
            static const SegmentList kEmpty{};
            return snap_ != nullptr ? snap_->segments : kEmpty;
        }

        [[nodiscard]] const SegmentList &operator*() const noexcept { return segments(); }
        [[nodiscard]] const SegmentList *operator->() const noexcept { return &segments(); }
        explicit operator bool() const noexcept { return snap_ != nullptr; }

      private:
        friend class ChunkedCatalogSnapshot;

        ReadGuard(const ChunkedCatalogSnapshot *owner, const Snapshot *snap, int slot_idx) noexcept
            : owner_(owner), snap_(snap), slot_idx_(slot_idx)
        {
        }

        const ChunkedCatalogSnapshot *owner_{nullptr};
        const Snapshot *snap_{nullptr};
        int slot_idx_{-2};
    };

    ChunkedCatalogSnapshot()
    {
        snapshot_.store(new Snapshot{1ULL, 0U, {}}, std::memory_order_release);
    }

    ChunkedCatalogSnapshot(const ChunkedCatalogSnapshot &) = delete;
    ChunkedCatalogSnapshot &operator=(const ChunkedCatalogSnapshot &) = delete;

    ~ChunkedCatalogSnapshot()
    {
        const Snapshot *live = snapshot_.exchange(nullptr, std::memory_order_acq_rel);
        delete live;
        for (const RetiredSnapshot &r : retired_) {
            delete r.ptr;
        }
        retired_.clear();
    }

    [[nodiscard]] ReadGuard acquire_snapshot() const noexcept
    {
        const uint64_t pinned_epoch = writer_epoch_.load(std::memory_order_seq_cst);
        int claimed_slot = -1;
        for (std::size_t i = 0U; i < kMaxReaderSlots; ++i) {
            uint64_t expected = 0ULL;
            if (reader_slots_[i].state.compare_exchange_strong(
                    expected, pinned_epoch, std::memory_order_seq_cst, std::memory_order_relaxed)) {
                claimed_slot = static_cast<int>(i);
                break;
            }
        }
        if (claimed_slot < 0) {
            fallback_pins_.fetch_add(1ULL, std::memory_order_seq_cst);
        }
        const Snapshot *snap = snapshot_.load(std::memory_order_seq_cst);
        return ReadGuard(this, snap, claimed_slot);
    }

    void replace_segment(std::size_t seg_idx, SegmentPtr updated)
    {
        std::lock_guard<std::mutex> lk(writer_mu_);
        const Snapshot *prev = snapshot_.load(std::memory_order_relaxed);
        SegmentList copy = prev != nullptr ? prev->segments : SegmentList{};
        if (seg_idx >= copy.size()) {
            copy.resize(seg_idx + 1U);
        }
        copy[seg_idx] = std::move(updated);
        while (!copy.empty() && (!copy.back() || copy.back()->count == 0U)) {
            copy.pop_back();
        }
        publish_locked(prev, std::move(copy));
    }

    void publish_segments(SegmentList segments)
    {
        std::lock_guard<std::mutex> lk(writer_mu_);
        const Snapshot *prev = snapshot_.load(std::memory_order_relaxed);
        while (!segments.empty() && (!segments.back() || segments.back()->count == 0U)) {
            segments.pop_back();
        }
        publish_locked(prev, std::move(segments));
    }

    void reclaim_quiescent()
    {
        std::lock_guard<std::mutex> lk(writer_mu_);
        reclaim_quiescent_locked();
    }

  private:
    static constexpr std::size_t kMaxReaderSlots = 64U;

    struct alignas(64) ReaderSlot {
        std::atomic<uint64_t> state{0ULL};
    };

    struct RetiredSnapshot {
        const Snapshot *ptr;
        uint64_t retire_epoch;
    };

    void publish_locked(const Snapshot *prev, SegmentList copy)
    {
        std::size_t total = 0U;
        for (const auto &seg : copy) {
            if (seg) {
                total += seg->count;
            }
        }
        if (prev != nullptr) {
            retired_.reserve(retired_.size() + 1U);
        }
        const uint64_t next_epoch = writer_epoch_.load(std::memory_order_relaxed) + 1ULL;
        const Snapshot *next = new Snapshot{next_epoch, total, std::move(copy)};
        snapshot_.store(next, std::memory_order_seq_cst);
        writer_epoch_.store(next_epoch, std::memory_order_seq_cst);
        if (prev != nullptr) {
            retired_.push_back(RetiredSnapshot{prev, prev->epoch});
        }
        reclaim_quiescent_locked();
    }

    void reclaim_quiescent_locked()
    {
        if (retired_.empty()) {
            return;
        }
        if (fallback_pins_.load(std::memory_order_seq_cst) != 0ULL) {
            return;
        }
        uint64_t min_active = std::numeric_limits<uint64_t>::max();
        for (const ReaderSlot &slot : reader_slots_) {
            const uint64_t s = slot.state.load(std::memory_order_seq_cst);
            if (s != 0ULL && s < min_active) {
                min_active = s;
            }
        }
        std::size_t write_pos = 0U;
        for (std::size_t i = 0U; i < retired_.size(); ++i) {
            if (retired_[i].retire_epoch < min_active) {
                delete retired_[i].ptr;
            } else {
                retired_[write_pos++] = retired_[i];
            }
        }
        retired_.resize(write_pos);
    }

    std::atomic<const Snapshot *> snapshot_{nullptr};
    static_assert(std::atomic<const Snapshot *>::is_always_lock_free,
                  "Wait-free QSBR snapshot pointer must be lock-free");
    std::atomic<uint64_t> writer_epoch_{1ULL};
    mutable std::array<ReaderSlot, kMaxReaderSlots> reader_slots_{};
    mutable std::atomic<uint64_t> fallback_pins_{0ULL};
    std::vector<RetiredSnapshot> retired_{};
    std::mutex writer_mu_{};
};

class ReadOnlySqlitePool {
  public:
    static constexpr size_t kMaxPoolSize = 16;

    struct ReadSlot {
        sqlite3 *db{nullptr};
        sqlite3_stmt *stmtFtsBm25{nullptr};
        sqlite3_stmt *stmtTrigram{nullptr};
        sqlite3_stmt *stmtGetById{nullptr};
        sqlite3_stmt *stmtGetBatch50{nullptr};
        sqlite3_stmt *stmtGetByPath{nullptr};
        sqlite3_stmt *stmtCount{nullptr};
    };

    class Lease {
      public:
        Lease() noexcept = default;
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
        Lease &operator=(Lease &&other) noexcept
        {
            if (this != &other) {
                if (m_pool) {
                    m_pool->release(m_idx);
                }
                m_pool = std::exchange(other.m_pool, nullptr);
                m_idx = other.m_idx;
            }
            return *this;
        }

        [[nodiscard]] bool valid() const noexcept { return m_pool != nullptr; }
        explicit operator bool() const noexcept { return valid(); }
        [[nodiscard]] ReadSlot &slot() noexcept { return m_pool->m_slots[m_idx]; }

      private:
        ReadOnlySqlitePool *m_pool{nullptr};
        size_t m_idx{0};
    };

    ReadOnlySqlitePool() = default;
    ~ReadOnlySqlitePool() { close(); }

    ReadOnlySqlitePool(const ReadOnlySqlitePool &) = delete;
    ReadOnlySqlitePool &operator=(const ReadOnlySqlitePool &) = delete;

    void setPreCloseCallback(std::function<void()> cb)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_preCloseCallback = std::move(cb);
    }

    bool open(const std::filesystem::path &dbPath, bool hasTrigramFts)
    {
        close();
        if (dbPath.empty() || dbPath == ":memory:") {
            return false;
        }
        (void)m_gate.reopen();
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_closing.store(false, std::memory_order_release);
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

        static constexpr const char *kSqlGetBatch50 = R"(
            SELECT published_file_id, owner_steam_id, title, type, file_path, preview_path,
                   file_size_bytes, time_created, time_updated, subscriptions, favorited,
                   views, rating, install_state, storage_backend, compat_tier, compat_score,
                   aspect_ratio, dominant_color, tag_mask, tags_string, description,
                   manifest_id, file_clock, signature_verified
            FROM workshop_items WHERE published_file_id IN (
                ?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,
                ?21,?22,?23,?24,?25,?26,?27,?28,?29,?30,?31,?32,?33,?34,?35,?36,?37,?38,?39,?40,
                ?41,?42,?43,?44,?45,?46,?47,?48,?49,?50
            );
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

        const size_t desiredSlots =
            std::clamp<size_t>(std::thread::hardware_concurrency(), 8U, kMaxPoolSize);
        m_slots.assign(desiredSlots, ReadSlot{});

        const std::string pathStr = dbPath.string();
        uint32_t mask = 0;
        for (size_t i = 0; i < desiredSlots; ++i) {
            if (sqlite3_open_v2(pathStr.c_str(), &m_slots[i].db,
                                SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
                close();
                return false;
            }
            sqlite3_busy_timeout(m_slots[i].db, 100);
            // Bound per-slot SQLite cache (2 MiB) and share a 64 MiB mmap ceiling across readers.
            sqlite3_exec(m_slots[i].db,
                         "PRAGMA query_only = ON; PRAGMA temp_store = MEMORY; "
                         "PRAGMA cache_size = -2048; PRAGMA mmap_size = 67108864;",
                         nullptr, nullptr, nullptr);

            if (sqlite3_prepare_v3(m_slots[i].db, kSqlFtsBm25, -1, SQLITE_PREPARE_PERSISTENT,
                                   &m_slots[i].stmtFtsBm25, nullptr) != SQLITE_OK ||
                sqlite3_prepare_v3(m_slots[i].db, kSqlGetById, -1, SQLITE_PREPARE_PERSISTENT,
                                   &m_slots[i].stmtGetById, nullptr) != SQLITE_OK ||
                sqlite3_prepare_v3(m_slots[i].db, kSqlGetBatch50, -1, SQLITE_PREPARE_PERSISTENT,
                                   &m_slots[i].stmtGetBatch50, nullptr) != SQLITE_OK ||
                sqlite3_prepare_v3(m_slots[i].db, kSqlGetByPath, -1, SQLITE_PREPARE_PERSISTENT,
                                   &m_slots[i].stmtGetByPath, nullptr) != SQLITE_OK ||
                sqlite3_prepare_v3(m_slots[i].db, kSqlCount, -1, SQLITE_PREPARE_PERSISTENT,
                                   &m_slots[i].stmtCount, nullptr) != SQLITE_OK) {
                close();
                return false;
            }

            if (hasTrigramFts) {
                if (sqlite3_prepare_v3(m_slots[i].db, kSqlTrigram, -1, SQLITE_PREPARE_PERSISTENT,
                                       &m_slots[i].stmtTrigram, nullptr) != SQLITE_OK) {
                    if (m_slots[i].stmtTrigram) {
                        sqlite3_finalize(m_slots[i].stmtTrigram);
                        m_slots[i].stmtTrigram = nullptr;
                    }
                }
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
        std::function<void()> preClose;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_closing.store(true, std::memory_order_release);
            preClose = m_preCloseCallback;
        }
        m_cv.notify_all();
        // Wait for every active Lease and acquire() waiter before finalizing any statement or db
        // handle (AUD-027).
        m_gate.close_and_drain();

        if (preClose) {
            preClose();
        }

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
            if (s.stmtGetBatch50) {
                sqlite3_finalize(s.stmtGetBatch50);
                s.stmtGetBatch50 = nullptr;
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
        m_slots.clear();
    }

    [[nodiscard]] bool isOpen() const noexcept { return m_open.load(std::memory_order_acquire); }
    [[nodiscard]] size_t poolSize() const noexcept { return m_slots.size(); }

    [[nodiscard]] Lease tryAcquire() noexcept
    {
        if (!m_gate.try_acquire()) {
            return Lease{};
        }
        std::unique_lock<std::mutex> lk(m_mtx);
        if (m_closing.load(std::memory_order_acquire) || m_freeMask == 0u) {
            lk.unlock();
            m_gate.release();
            return Lease{};
        }
        const size_t idx = static_cast<size_t>(std::countr_zero(m_freeMask));
        m_freeMask &= ~(1u << idx);
        return Lease(*this, idx);
    }

    void warmIdleSlots(uint64_t sampleRowId) noexcept
    {
        std::array<Lease, kMaxPoolSize> held{};
        size_t nHeld = 0U;
        while (nHeld < kMaxPoolSize) {
            Lease lease = tryAcquire();
            if (!lease.valid()) {
                break;
            }
            // Refresh SQLite schema/WAL/mmap state once on idle readers after bulk writes.
            sqlite3_stmt *stmt = lease.slot().stmtGetBatch50;
            if (stmt) {
                sqlite3_reset(stmt);
                sqlite3_clear_bindings(stmt);
                bool ok = sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(sampleRowId)) ==
                          SQLITE_OK;
                for (int k = 2; ok && k <= 50; ++k) {
                    ok = sqlite3_bind_null(stmt, k) == SQLITE_OK;
                }
                if (ok) {
                    while (sqlite3_step(stmt) == SQLITE_ROW) {
                    }
                }
                sqlite3_reset(stmt);
            }
            held[nHeld++] = std::move(lease);
        }
    }

    [[nodiscard]] Lease acquire()
    {
        if (!m_gate.try_acquire()) {
            return Lease{};
        }
        std::unique_lock<std::mutex> lk(m_mtx);
        m_cv.wait(lk,
                  [this] { return m_closing.load(std::memory_order_acquire) || m_freeMask != 0u; });
        if (m_closing.load(std::memory_order_acquire) || m_freeMask == 0u) {
            lk.unlock();
            m_gate.release();
            return Lease{};
        }
        const size_t idx = static_cast<size_t>(std::countr_zero(m_freeMask));
        m_freeMask &= ~(1u << idx);
        return Lease(*this, idx);
    }

  private:
    void release(size_t idx) noexcept
    {
        if (idx < m_slots.size()) {
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
            if (s.stmtGetBatch50) {
                sqlite3_reset(s.stmtGetBatch50);
            }
            if (s.stmtGetByPath) {
                sqlite3_reset(s.stmtGetByPath);
            }
            if (s.stmtCount) {
                sqlite3_reset(s.stmtCount);
            }
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                if (!m_closing.load(std::memory_order_acquire)) {
                    m_freeMask |= static_cast<uint32_t>(1u << idx);
                }
            }
            m_cv.notify_one();
        }
        m_gate.release();
    }

    std::vector<ReadSlot> m_slots{};
    std::mutex m_mtx;
    std::condition_variable m_cv;
    uint32_t m_freeMask{0};
    std::atomic<bool> m_open{false};
    std::atomic<bool> m_closing{false};
    std::function<void()> m_preCloseCallback;
    SqlitePoolShutdownGate m_gate;
};

} // namespace wwal::studio
