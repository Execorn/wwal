// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/core/catalog_index_manager.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <sstream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <unordered_map>

#if __has_include(<linux/io_uring.h>)
#include <linux/io_uring.h>
#define WWAL_HAS_LINUX_IO_URING_HDR 1
#else
#define WWAL_HAS_LINUX_IO_URING_HDR 0
#endif

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include "waywal/we_appworkshop.h"
#include "waywal/we_compat_scanner.h"
#include "waywal/we_project.h"
#include "waywal/we_vdf.h"
#include "waywal/wwpkg.h"

namespace wwal::studio {

namespace {

std::string toLowerAscii(std::string_view s)
{
    std::string out;
    out.resize(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    }
    return out;
}

/* ========================================================================= */
/* Batched Linux io_uring IORING_OP_STATX Helper (with statx() Fallback)     */
/* ========================================================================= */

class IoUringStatxBatcher {
  public:
    static constexpr unsigned kRingEntries = 64;

    IoUringStatxBatcher()
    {
#if WWAL_HAS_LINUX_IO_URING_HDR && defined(__NR_io_uring_setup) && defined(__NR_io_uring_enter)
        io_uring_params params{};
        long fd = ::syscall(__NR_io_uring_setup, kRingEntries, &params);
        if (fd < 0) {
            return;
        }
        m_ringFd = static_cast<int>(fd);

        size_t sqRingSz = params.sq_off.array + params.sq_entries * sizeof(unsigned);
        size_t cqRingSz = params.cq_off.cqes + params.cq_entries * sizeof(io_uring_cqe);
        m_singleMmap = (params.features & IORING_FEAT_SINGLE_MMAP) != 0;
        if (m_singleMmap) {
            sqRingSz = std::max(sqRingSz, cqRingSz);
            cqRingSz = sqRingSz;
        }

        m_sqRingSize = sqRingSz;
        m_sqPtr = ::mmap(nullptr, m_sqRingSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                         m_ringFd, IORING_OFF_SQ_RING);
        if (m_sqPtr == MAP_FAILED) {
            m_sqPtr = nullptr;
            teardown();
            return;
        }

        if (m_singleMmap) {
            m_cqPtr = m_sqPtr;
            m_cqRingSize = 0;
        } else {
            m_cqRingSize = cqRingSz;
            m_cqPtr = ::mmap(nullptr, m_cqRingSize, PROT_READ | PROT_WRITE,
                             MAP_SHARED | MAP_POPULATE, m_ringFd, IORING_OFF_CQ_RING);
            if (m_cqPtr == MAP_FAILED) {
                m_cqPtr = nullptr;
                teardown();
                return;
            }
        }

        m_sqesSize = params.sq_entries * sizeof(io_uring_sqe);
        void *sqesMem = ::mmap(nullptr, m_sqesSize, PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_POPULATE, m_ringFd, IORING_OFF_SQES);
        if (sqesMem == MAP_FAILED) {
            teardown();
            return;
        }
        m_sqes = static_cast<io_uring_sqe *>(sqesMem);

        auto *sqBase = static_cast<uint8_t *>(m_sqPtr);
        auto *cqBase = static_cast<uint8_t *>(m_cqPtr);
        m_sqHead = reinterpret_cast<unsigned *>(sqBase + params.sq_off.head);
        m_sqTail = reinterpret_cast<unsigned *>(sqBase + params.sq_off.tail);
        m_sqMask = *reinterpret_cast<unsigned *>(sqBase + params.sq_off.ring_mask);
        m_sqArray = reinterpret_cast<unsigned *>(sqBase + params.sq_off.array);

        m_cqHead = reinterpret_cast<unsigned *>(cqBase + params.cq_off.head);
        m_cqTail = reinterpret_cast<unsigned *>(cqBase + params.cq_off.tail);
        m_cqMask = *reinterpret_cast<unsigned *>(cqBase + params.cq_off.ring_mask);
        m_cqes = reinterpret_cast<io_uring_cqe *>(cqBase + params.cq_off.cqes);
        m_ringReady = true;
#endif
    }

    ~IoUringStatxBatcher() { teardown(); }

    IoUringStatxBatcher(const IoUringStatxBatcher &) = delete;
    IoUringStatxBatcher &operator=(const IoUringStatxBatcher &) = delete;

    void queryFiles(const std::vector<std::string> &paths, uint64_t &outTotalBytes,
                    uint64_t &outLatestMtime, size_t &statCounter)
    {
        outTotalBytes = 0;
        outLatestMtime = 0;
        if (paths.empty()) {
            return;
        }

#if WWAL_HAS_LINUX_IO_URING_HDR && defined(__NR_io_uring_enter)
        if (m_ringReady) {
            constexpr size_t kBatchCap = 32;
            std::array<struct statx, kBatchCap> stxBatch{};

            size_t pos = 0;
            while (pos < paths.size()) {
                size_t count = std::min(kBatchCap, paths.size() - pos);
                std::memset(stxBatch.data(), 0, count * sizeof(struct statx));

                unsigned tail = *m_sqTail;
                for (size_t j = 0; j < count; ++j) {
                    unsigned slot = (tail + static_cast<unsigned>(j)) & m_sqMask;
                    io_uring_sqe *sqe = &m_sqes[slot];
                    std::memset(sqe, 0, sizeof(*sqe));
                    sqe->opcode = IORING_OP_STATX;
                    sqe->fd = AT_FDCWD;
                    sqe->addr = reinterpret_cast<uint64_t>(paths[pos + j].c_str());
                    sqe->len = STATX_TYPE | STATX_SIZE | STATX_MTIME;
                    sqe->statx_flags = AT_SYMLINK_NOFOLLOW;
                    sqe->off = reinterpret_cast<uint64_t>(&stxBatch[j]);
                    sqe->user_data = j;
                    m_sqArray[slot] = slot;
                }

                const unsigned targetSqHead = tail + static_cast<unsigned>(count);
                std::atomic_ref<unsigned>(*m_sqTail).store(targetSqHead, std::memory_order_release);

                bool enterFailed = false;
                while (true) {
                    unsigned curSqHead =
                        std::atomic_ref<unsigned>(*m_sqHead).load(std::memory_order_acquire);
                    unsigned toSubmit = targetSqHead - curSqHead;
                    unsigned curCqHead =
                        std::atomic_ref<unsigned>(*m_cqHead).load(std::memory_order_acquire);
                    unsigned curCqTail =
                        std::atomic_ref<unsigned>(*m_cqTail).load(std::memory_order_acquire);
                    unsigned readyCqes = curCqTail - curCqHead;
                    if (toSubmit == 0 && readyCqes >= count) {
                        break;
                    }

                    unsigned minComplete =
                        (readyCqes < count) ? static_cast<unsigned>(count - readyCqes) : 0u;
                    unsigned flags = (minComplete > 0) ? IORING_ENTER_GETEVENTS : 0u;
                    long ret = ::syscall(__NR_io_uring_enter, m_ringFd, toSubmit, minComplete,
                                         flags, nullptr, 0);
                    if (ret < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        // Degrade to direct statx() if seccomp blocks io_uring_enter.
                        enterFailed = true;
                        break;
                    }
                }
                if (enterFailed) {
                    m_ringReady = false;
                    break;
                }

                statCounter += count;
                unsigned head =
                    std::atomic_ref<unsigned>(*m_cqHead).load(std::memory_order_acquire);
                unsigned cqEnd =
                    std::atomic_ref<unsigned>(*m_cqTail).load(std::memory_order_acquire);
                while (head != cqEnd) {
                    const io_uring_cqe &cqe = m_cqes[head & m_cqMask];
                    if (cqe.res == 0 && cqe.user_data < count) {
                        const struct statx &stx = stxBatch[static_cast<size_t>(cqe.user_data)];
                        if (S_ISREG(stx.stx_mode)) {
                            outTotalBytes += stx.stx_size;
                            if (stx.stx_mtime.tv_sec > 0 &&
                                static_cast<uint64_t>(stx.stx_mtime.tv_sec) > outLatestMtime) {
                                outLatestMtime = static_cast<uint64_t>(stx.stx_mtime.tv_sec);
                            }
                        }
                    }
                    ++head;
                }
                std::atomic_ref<unsigned>(*m_cqHead).store(head, std::memory_order_release);
                pos += count;
            }

            if (pos == paths.size()) {
                return;
            }
            // Reset partial results and run fallback across all paths if io_uring_enter failed.
            outTotalBytes = 0;
            outLatestMtime = 0;
        }
#endif

        for (const auto &p : paths) {
            struct statx stx{};
            ++statCounter;
            if (::statx(AT_FDCWD, p.c_str(), AT_SYMLINK_NOFOLLOW,
                        STATX_TYPE | STATX_SIZE | STATX_MTIME, &stx) == 0) {
                if (S_ISREG(stx.stx_mode)) {
                    outTotalBytes += stx.stx_size;
                    if (stx.stx_mtime.tv_sec > 0 &&
                        static_cast<uint64_t>(stx.stx_mtime.tv_sec) > outLatestMtime) {
                        outLatestMtime = static_cast<uint64_t>(stx.stx_mtime.tv_sec);
                    }
                }
            }
        }
    }

  private:
    void teardown()
    {
        if (m_sqes) {
            ::munmap(m_sqes, m_sqesSize);
            m_sqes = nullptr;
        }
        if (m_cqPtr && !m_singleMmap) {
            ::munmap(m_cqPtr, m_cqRingSize);
        }
        m_cqPtr = nullptr;
        if (m_sqPtr) {
            ::munmap(m_sqPtr, m_sqRingSize);
            m_sqPtr = nullptr;
        }
        if (m_ringFd >= 0) {
            ::close(m_ringFd);
            m_ringFd = -1;
        }
        m_ringReady = false;
    }

    int m_ringFd{-1};
    bool m_ringReady{false};
    bool m_singleMmap{false};
    void *m_sqPtr{nullptr};
    void *m_cqPtr{nullptr};
    size_t m_sqRingSize{0};
    size_t m_cqRingSize{0};
    size_t m_sqesSize{0};
#if WWAL_HAS_LINUX_IO_URING_HDR
    io_uring_sqe *m_sqes{nullptr};
    io_uring_cqe *m_cqes{nullptr};
#else
    void *m_sqes{nullptr};
    void *m_cqes{nullptr};
#endif
    unsigned *m_sqHead{nullptr};
    unsigned *m_sqTail{nullptr};
    unsigned m_sqMask{0};
    unsigned *m_sqArray{nullptr};
    unsigned *m_cqHead{nullptr};
    unsigned *m_cqTail{nullptr};
    unsigned m_cqMask{0};
};

void collectRegularFilesNoStat(const std::string &dirPath, std::vector<std::string> &outFiles,
                               int depth = 0)
{
    if (depth > 16 || outFiles.size() >= 16384) {
        return;
    }
    DIR *dir = ::opendir(dirPath.c_str());
    if (!dir) {
        return;
    }
    while (struct dirent *ent = ::readdir(dir)) {
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        std::string full = dirPath + "/" + ent->d_name;
        if (ent->d_type == DT_DIR) {
            collectRegularFilesNoStat(full, outFiles, depth + 1);
        } else if (ent->d_type == DT_REG || ent->d_type == DT_UNKNOWN) {
            outFiles.push_back(std::move(full));
        }
    }
    ::closedir(dir);
}

/* ========================================================================= */
/* 64-Bit Myers (1999) Semi-Global Bit-Parallel Edit Distance                */
/* ========================================================================= */

uint32_t myersSubstringDistance64(std::string_view textLower, const std::array<uint64_t, 256> &peq,
                                  uint32_t m) noexcept
{
    if (m == 0) {
        return 0;
    }
    if (textLower.empty()) {
        return m;
    }

    const uint64_t highBit = (m == 64) ? (1ULL << 63) : (1ULL << (m - 1));
    const uint64_t mask = (m == 64) ? ~0ULL : ((1ULL << m) - 1ULL);

    uint64_t pv = mask;
    uint64_t mv = 0ULL;
    uint32_t score = m;
    uint32_t best = m;

    for (unsigned char ch : textLower) {
        uint64_t eq = peq[ch];
        uint64_t xv = eq | mv;
        uint64_t xh = ((((eq & pv) + pv) ^ pv) | eq) & mask;

        uint64_t ph = (mv | ~(xh | pv)) & mask;
        uint64_t mh = pv & xh;

        if (ph & highBit) {
            ++score;
        } else if (mh & highBit) {
            --score;
        }
        if (score < best) {
            best = score;
            if (best == 0) {
                return 0;
            }
        }

        uint64_t phShift = (ph << 1) & mask;
        uint64_t mhShift = (mh << 1) & mask;
        pv = (mhShift | ~(xv | phShift)) & mask;
        mv = phShift & xv;
    }

    return best;
}

std::string resolveConfinedPreviewPath(const std::filesystem::path &itemDir,
                                       std::string_view rawPreview)
{
    if (rawPreview.empty() || rawPreview.front() == '/' ||
        rawPreview.find('\\') != std::string_view::npos ||
        rawPreview.find('\0') != std::string_view::npos) {
        return {};
    }

    std::filesystem::path relInput(rawPreview);
    if (relInput.is_absolute()) {
        return {};
    }

    const std::filesystem::path normItem = itemDir.lexically_normal();
    const std::filesystem::path cand = (normItem / relInput).lexically_normal();
    const std::filesystem::path rel = cand.lexically_relative(normItem);
    if (rel.empty() || rel.is_absolute()) {
        return {};
    }
    const std::string firstComp = rel.begin()->string();
    if (firstComp.empty() || firstComp == "." || firstComp == "..") {
        return {};
    }

    std::filesystem::path cur = normItem;
    std::error_code ec;
    std::filesystem::file_status st{};
    for (const auto &part : rel) {
        if (part == "." || part == "..") {
            return {};
        }
        cur /= part;
        st = std::filesystem::symlink_status(cur, ec);
        if (ec || std::filesystem::is_symlink(st)) {
            return {};
        }
    }
    if (!std::filesystem::is_regular_file(st)) {
        return {};
    }
    return cur.string();
}

void parseRootProjectJsonMetadata(const std::filesystem::path &itemDir, std::string &ioTitle,
                                  std::string &ioType, std::string &ioPreview, std::string &outTags,
                                  std::string &outDescription)
{
    std::ifstream ifs(itemDir / "project.json", std::ios::binary | std::ios::ate);
    if (!ifs) {
        return;
    }
    auto sz = ifs.tellg();
    if (sz <= 0 || sz > 4 * 1024 * 1024) {
        return;
    }
    ifs.seekg(0, std::ios::beg);
    QByteArray raw(static_cast<int>(sz), Qt::Uninitialized);
    if (!ifs.read(raw.data(), sz)) {
        return;
    }

    QJsonParseError err{};
    QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return;
    }

    const QJsonObject root = doc.object();
    if (root.contains(QStringLiteral("title")) && root.value(QStringLiteral("title")).isString()) {
        std::string t = root.value(QStringLiteral("title")).toString().trimmed().toStdString();
        if (!t.empty()) {
            ioTitle = std::move(t);
        }
    }
    if (root.contains(QStringLiteral("type")) && root.value(QStringLiteral("type")).isString()) {
        std::string tp =
            root.value(QStringLiteral("type")).toString().trimmed().toLower().toStdString();
        if (!tp.empty()) {
            ioType = std::move(tp);
        }
    }
    if (root.contains(QStringLiteral("preview")) &&
        root.value(QStringLiteral("preview")).isString()) {
        std::string pv = root.value(QStringLiteral("preview")).toString().trimmed().toStdString();
        ioPreview = resolveConfinedPreviewPath(itemDir, pv);
    }
    if (root.contains(QStringLiteral("description")) &&
        root.value(QStringLiteral("description")).isString()) {
        outDescription = root.value(QStringLiteral("description")).toString().toStdString();
    }
    if (root.contains(QStringLiteral("tags"))) {
        QJsonValue tv = root.value(QStringLiteral("tags"));
        if (tv.isArray()) {
            QJsonArray arr = tv.toArray();
            std::string joined;
            for (const auto &v : arr) {
                if (v.isString()) {
                    std::string tag = v.toString().trimmed().toStdString();
                    if (!tag.empty()) {
                        if (!joined.empty()) {
                            joined += ", ";
                        }
                        joined += tag;
                    }
                }
            }
            outTags = std::move(joined);
        } else if (tv.isString()) {
            outTags = tv.toString().trimmed().toStdString();
        }
    }
}

void bindRecordToStmt(sqlite3_stmt *stmt, const WorkshopItemRecord &rec)
{
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(rec.published_file_id));
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(rec.owner_steam_id));
    sqlite3_bind_text(stmt, 3, rec.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, rec.type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, rec.file_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, rec.preview_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 7, static_cast<sqlite3_int64>(rec.file_size_bytes));
    sqlite3_bind_int64(stmt, 8, static_cast<sqlite3_int64>(rec.time_created));
    sqlite3_bind_int64(stmt, 9, static_cast<sqlite3_int64>(rec.time_updated));
    sqlite3_bind_int64(stmt, 10, static_cast<sqlite3_int64>(rec.subscriptions));
    sqlite3_bind_int64(stmt, 11, static_cast<sqlite3_int64>(rec.favorited));
    sqlite3_bind_int64(stmt, 12, static_cast<sqlite3_int64>(rec.views));
    sqlite3_bind_double(stmt, 13, static_cast<double>(rec.rating));
    sqlite3_bind_int64(stmt, 14, static_cast<sqlite3_int64>(rec.install_state));
    sqlite3_bind_int64(stmt, 15, static_cast<sqlite3_int64>(rec.storage_backend));
    sqlite3_bind_int64(stmt, 16, static_cast<sqlite3_int64>(rec.compat_tier));
    sqlite3_bind_int64(stmt, 17, static_cast<sqlite3_int64>(rec.compat_score));
    sqlite3_bind_double(stmt, 18, static_cast<double>(rec.aspect_ratio));
    sqlite3_bind_int64(stmt, 19, static_cast<sqlite3_int64>(rec.dominant_color));
    sqlite3_bind_int64(stmt, 20, static_cast<sqlite3_int64>(rec.tag_mask));
    sqlite3_bind_text(stmt, 21, rec.tags_string.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 22, rec.description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 23, static_cast<sqlite3_int64>(rec.manifest_id));
    sqlite3_bind_int64(stmt, 24,
                       static_cast<sqlite3_int64>(rec.file_clock > 0 ? rec.file_clock : 1ULL));
    sqlite3_bind_int(stmt, 25, rec.signature_verified ? 1 : 0);
}

WorkshopItemRecord readRecordFromStmt(sqlite3_stmt *stmt)
{
    auto colStr = [stmt](int col, const char *fallback = "") -> std::string {
        const auto *p = reinterpret_cast<const char *>(sqlite3_column_text(stmt, col));
        if (!p) {
            return fallback;
        }
        return std::string(p, static_cast<size_t>(sqlite3_column_bytes(stmt, col)));
    };

    WorkshopItemRecord rec;
    rec.published_file_id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
    rec.owner_steam_id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
    rec.title = colStr(2);
    rec.type = colStr(3, "scene");
    rec.file_path = colStr(4);
    rec.preview_path = colStr(5);
    rec.file_size_bytes = static_cast<uint64_t>(sqlite3_column_int64(stmt, 6));
    rec.time_created = static_cast<uint64_t>(sqlite3_column_int64(stmt, 7));
    rec.time_updated = static_cast<uint64_t>(sqlite3_column_int64(stmt, 8));
    rec.subscriptions = static_cast<uint32_t>(sqlite3_column_int64(stmt, 9));
    rec.favorited = static_cast<uint32_t>(sqlite3_column_int64(stmt, 10));
    rec.views = static_cast<uint32_t>(sqlite3_column_int64(stmt, 11));
    rec.rating = static_cast<float>(sqlite3_column_double(stmt, 12));
    rec.install_state = static_cast<uint32_t>(sqlite3_column_int64(stmt, 13));
    rec.storage_backend = static_cast<uint32_t>(sqlite3_column_int64(stmt, 14));
    rec.compat_tier = static_cast<uint32_t>(sqlite3_column_int64(stmt, 15));
    rec.compat_score = static_cast<uint32_t>(sqlite3_column_int64(stmt, 16));
    rec.aspect_ratio = static_cast<float>(sqlite3_column_double(stmt, 17));
    rec.dominant_color = static_cast<uint32_t>(sqlite3_column_int64(stmt, 18));
    rec.tag_mask = static_cast<uint64_t>(sqlite3_column_int64(stmt, 19));
    rec.tags_string = colStr(20);
    rec.description = colStr(21);
    rec.manifest_id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 22));
    rec.file_clock = static_cast<uint64_t>(sqlite3_column_int64(stmt, 23));
    rec.signature_verified = sqlite3_column_int(stmt, 24) != 0;
    if (sqlite3_column_count(stmt) > 25) {
        rec.search_rank = sqlite3_column_double(stmt, 25);
    }
    return rec;
}

std::filesystem::path defaultDatabasePath()
{
    const char *xdgData = std::getenv("XDG_DATA_HOME");
    std::filesystem::path baseDir;
    if (xdgData && xdgData[0] != '\0') {
        baseDir = std::filesystem::path(xdgData);
    } else {
        const char *home = std::getenv("HOME");
        baseDir = (home && home[0] != '\0') ? std::filesystem::path(home) / ".local" / "share"
                                            : std::filesystem::current_path();
    }
    return baseDir / "wwal" / "catalog.db";
}

} // namespace

/* ========================================================================= */
/* In-Memory SoA Tag Bitmap & Lowercase Title Arena                          */
/* ========================================================================= */

struct CatalogIndexManager::Impl {
    mutable std::mutex dbMutex;
    bool hasTrigramFts{false};
    std::atomic<size_t> lastReconcileStatCalls{0};

    // Contiguous cache-friendly SoA arrays and inverted 64-bit tag bitplanes.
    std::vector<uint64_t> soaPubIds;
    std::vector<uint64_t> soaTagMasks;
    std::array<std::vector<uint64_t>, 64> tagBitsets;
    std::vector<uint32_t> soaTitleOffsets;
    std::vector<uint32_t> soaTitleLengths;
    std::string soaLowerTitles;
    std::vector<uint32_t> soaTagsOffsets;
    std::vector<uint32_t> soaTagsLengths;
    std::string soaLowerTags;
    std::vector<uint16_t> soaDescLengths;
    uint64_t totalTitleBytes{0};
    uint64_t totalTagsBytes{0};
    uint64_t totalDescBytes{0};
    std::unordered_map<uint64_t, size_t> idToSlot;

    void clearSoaLocked()
    {
        soaPubIds.clear();
        soaTagMasks.clear();
        for (auto &plane : tagBitsets) {
            plane.clear();
        }
        soaTitleOffsets.clear();
        soaTitleLengths.clear();
        soaLowerTitles.clear();
        soaTagsOffsets.clear();
        soaTagsLengths.clear();
        soaLowerTags.clear();
        soaDescLengths.clear();
        totalTitleBytes = 0;
        totalTagsBytes = 0;
        totalDescBytes = 0;
        idToSlot.clear();
    }

    void overwriteSlotTagBitsLocked(size_t slot, uint64_t oldMask, uint64_t newMask)
    {
        const size_t wordIdx = slot >> 6;
        const uint64_t bit = 1ULL << (slot & 63u);
        uint64_t cleared = oldMask & ~newMask;
        while (cleared != 0) {
            unsigned b = static_cast<unsigned>(std::countr_zero(cleared));
            tagBitsets[b][wordIdx] &= ~bit;
            cleared &= (cleared - 1ULL);
        }
        uint64_t added = newMask & ~oldMask;
        while (added != 0) {
            unsigned b = static_cast<unsigned>(std::countr_zero(added));
            tagBitsets[b][wordIdx] |= bit;
            added &= (added - 1ULL);
        }
    }

    void upsertSoaLocked(uint64_t pubId, uint64_t tagMask, std::string_view title,
                         std::string_view tagsStr, std::string_view desc)
    {
        std::string lowerTitle = toLowerAscii(title);
        std::string lowerTags = toLowerAscii(tagsStr);
        uint16_t descLen = static_cast<uint16_t>(std::min<size_t>(desc.size(), 65535u));

        auto it = idToSlot.find(pubId);
        if (it != idToSlot.end()) {
            size_t slot = it->second;
            overwriteSlotTagBitsLocked(slot, soaTagMasks[slot], tagMask);
            soaTagMasks[slot] = tagMask;

            uint32_t oldTitleLen = soaTitleLengths[slot];
            totalTitleBytes = totalTitleBytes - oldTitleLen + lowerTitle.size();
            if (lowerTitle.size() <= oldTitleLen) {
                std::memcpy(soaLowerTitles.data() + soaTitleOffsets[slot], lowerTitle.data(),
                            lowerTitle.size());
                soaTitleLengths[slot] = static_cast<uint32_t>(lowerTitle.size());
            } else {
                uint32_t newOff = static_cast<uint32_t>(soaLowerTitles.size());
                soaLowerTitles.append(lowerTitle);
                soaTitleOffsets[slot] = newOff;
                soaTitleLengths[slot] = static_cast<uint32_t>(lowerTitle.size());
            }

            uint32_t oldTagsLen = soaTagsLengths[slot];
            totalTagsBytes = totalTagsBytes - oldTagsLen + lowerTags.size();
            if (lowerTags.size() <= oldTagsLen) {
                std::memcpy(soaLowerTags.data() + soaTagsOffsets[slot], lowerTags.data(),
                            lowerTags.size());
                soaTagsLengths[slot] = static_cast<uint32_t>(lowerTags.size());
            } else {
                uint32_t newOff = static_cast<uint32_t>(soaLowerTags.size());
                soaLowerTags.append(lowerTags);
                soaTagsOffsets[slot] = newOff;
                soaTagsLengths[slot] = static_cast<uint32_t>(lowerTags.size());
            }

            totalDescBytes = totalDescBytes - soaDescLengths[slot] + descLen;
            soaDescLengths[slot] = descLen;
            return;
        }

        size_t slot = soaPubIds.size();
        if ((slot & 63u) == 0) {
            for (auto &plane : tagBitsets) {
                plane.push_back(0ULL);
            }
        }
        const size_t wordIdx = slot >> 6;
        const uint64_t bit = 1ULL << (slot & 63u);
        for (uint64_t m = tagMask; m != 0; m &= (m - 1ULL)) {
            tagBitsets[static_cast<unsigned>(std::countr_zero(m))][wordIdx] |= bit;
        }

        idToSlot.emplace(pubId, slot);
        soaPubIds.push_back(pubId);
        soaTagMasks.push_back(tagMask);

        uint32_t tOff = static_cast<uint32_t>(soaLowerTitles.size());
        soaLowerTitles.append(lowerTitle);
        soaTitleOffsets.push_back(tOff);
        soaTitleLengths.push_back(static_cast<uint32_t>(lowerTitle.size()));
        totalTitleBytes += lowerTitle.size();

        uint32_t gOff = static_cast<uint32_t>(soaLowerTags.size());
        soaLowerTags.append(lowerTags);
        soaTagsOffsets.push_back(gOff);
        soaTagsLengths.push_back(static_cast<uint32_t>(lowerTags.size()));
        totalTagsBytes += lowerTags.size();

        soaDescLengths.push_back(descLen);
        totalDescBytes += descLen;
    }

    void removeSoaLocked(uint64_t pubId)
    {
        auto it = idToSlot.find(pubId);
        if (it == idToSlot.end()) {
            return;
        }
        size_t slot = it->second;
        totalTitleBytes -= soaTitleLengths[slot];
        totalTagsBytes -= soaTagsLengths[slot];
        totalDescBytes -= soaDescLengths[slot];

        size_t last = soaPubIds.size() - 1;
        uint64_t lastMask = soaTagMasks[last];
        if (slot != last) {
            uint64_t movedId = soaPubIds[last];
            overwriteSlotTagBitsLocked(slot, soaTagMasks[slot], lastMask);
            soaPubIds[slot] = movedId;
            soaTagMasks[slot] = lastMask;
            soaTitleOffsets[slot] = soaTitleOffsets[last];
            soaTitleLengths[slot] = soaTitleLengths[last];
            soaTagsOffsets[slot] = soaTagsOffsets[last];
            soaTagsLengths[slot] = soaTagsLengths[last];
            soaDescLengths[slot] = soaDescLengths[last];
            idToSlot[movedId] = slot;
        }
        overwriteSlotTagBitsLocked(last, lastMask, 0ULL);
        if ((last & 63u) == 0) {
            for (auto &plane : tagBitsets) {
                plane.pop_back();
            }
        }
        soaPubIds.pop_back();
        soaTagMasks.pop_back();
        soaTitleOffsets.pop_back();
        soaTitleLengths.pop_back();
        soaTagsOffsets.pop_back();
        soaTagsLengths.pop_back();
        soaDescLengths.pop_back();
        idToSlot.erase(it);
    }

    void loadInitialSoaFromDbLocked(sqlite3 *db)
    {
        clearSoaLocked();
        if (!db) {
            return;
        }
        sqlite3_stmt *stmt = nullptr;
        const char *sql = "SELECT published_file_id, tag_mask, title, tags_string, description "
                          "FROM workshop_items;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return;
        }
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            uint64_t id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
            uint64_t mask = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
            const char *t = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
            const char *g = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
            const char *d = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
            upsertSoaLocked(id, mask, t ? t : "", g ? g : "", d ? d : "");
        }
        sqlite3_finalize(stmt);
    }
};

CatalogIndexManager::CatalogIndexManager(const std::filesystem::path &dbPath)
    : m_impl(std::make_unique<Impl>())
{
    m_dbPath = dbPath.empty() ? defaultDatabasePath() : dbPath;

    std::error_code ec;
    if (m_dbPath != ":memory:") {
        std::filesystem::create_directories(m_dbPath.parent_path(), ec);
    }

    int rc = sqlite3_open_v2(m_dbPath.string().c_str(), &m_db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             nullptr);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "[CatalogIndexManager] Failed to open SQLite database %s: %s\n",
                     m_dbPath.string().c_str(), sqlite3_errmsg(m_db));
        return;
    }

    initSchema();
    prepareStatements();
    m_impl->loadInitialSoaFromDbLocked(m_db);
}

CatalogIndexManager::~CatalogIndexManager()
{
    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    finalizeStatements();
    if (m_db) {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
    }
}

bool CatalogIndexManager::isOpen() const noexcept { return m_db != nullptr; }

size_t CatalogIndexManager::lastReconcileStatCount() const noexcept
{
    return m_impl->lastReconcileStatCalls.load(std::memory_order_relaxed);
}

void CatalogIndexManager::initSchema()
{
    if (!m_db) {
        return;
    }

    sqlite3_exec(m_db, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "PRAGMA temp_store = MEMORY;", nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "PRAGMA cache_size = -131072;", nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "PRAGMA mmap_size = 536870912;", nullptr, nullptr, nullptr);

    const char *sqlTable = R"(
        CREATE TABLE IF NOT EXISTS workshop_items (
            published_file_id   INTEGER PRIMARY KEY NOT NULL,
            owner_steam_id      INTEGER NOT NULL DEFAULT 0,
            title               TEXT NOT NULL,
            type                TEXT NOT NULL,
            file_path           TEXT NOT NULL UNIQUE,
            preview_path        TEXT,
            file_size_bytes     INTEGER NOT NULL DEFAULT 0,
            time_created        INTEGER NOT NULL DEFAULT 0,
            time_updated        INTEGER NOT NULL DEFAULT 0,
            subscriptions       INTEGER NOT NULL DEFAULT 0,
            favorited           INTEGER NOT NULL DEFAULT 0,
            views               INTEGER NOT NULL DEFAULT 0,
            rating              REAL NOT NULL DEFAULT 0.0,
            install_state       INTEGER NOT NULL DEFAULT 0,
            storage_backend     INTEGER NOT NULL DEFAULT 0,
            compat_tier         INTEGER NOT NULL DEFAULT 1,
            compat_score        INTEGER NOT NULL DEFAULT 100,
            aspect_ratio        REAL NOT NULL DEFAULT 1.7777778,
            dominant_color      INTEGER NOT NULL DEFAULT 0x2b303c,
            tag_mask            INTEGER NOT NULL DEFAULT 0,
            tags_string         TEXT NOT NULL DEFAULT '',
            description         TEXT NOT NULL DEFAULT '',
            manifest_id         INTEGER NOT NULL DEFAULT 0,
            file_clock          INTEGER NOT NULL DEFAULT 0,
            signature_verified  INTEGER NOT NULL DEFAULT 1
        );
    )";
    sqlite3_exec(m_db, sqlTable, nullptr, nullptr, nullptr);

    // Idempotent column migrations when opening pre-existing database files.
    sqlite3_exec(m_db,
                 "ALTER TABLE workshop_items ADD COLUMN manifest_id INTEGER NOT NULL DEFAULT 0;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(m_db,
                 "ALTER TABLE workshop_items ADD COLUMN file_clock INTEGER NOT NULL DEFAULT 0;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(
        m_db,
        "ALTER TABLE workshop_items ADD COLUMN signature_verified INTEGER NOT NULL DEFAULT 1;",
        nullptr, nullptr, nullptr);

    const char *sqlIndices = R"(
        CREATE INDEX IF NOT EXISTS idx_items_type_tier_subs ON workshop_items(type, compat_tier, subscriptions DESC);
        CREATE INDEX IF NOT EXISTS idx_items_type_subs      ON workshop_items(type, subscriptions DESC);
        CREATE INDEX IF NOT EXISTS idx_items_tier_subs      ON workshop_items(compat_tier, subscriptions DESC);
        CREATE INDEX IF NOT EXISTS idx_items_tag_rating     ON workshop_items(tag_mask, rating DESC, published_file_id);
        CREATE INDEX IF NOT EXISTS idx_items_time_updated   ON workshop_items(time_updated DESC, published_file_id ASC);
        CREATE INDEX IF NOT EXISTS idx_items_title_nocase   ON workshop_items(title COLLATE NOCASE ASC);
        CREATE INDEX IF NOT EXISTS idx_items_compat_score   ON workshop_items(compat_score DESC, subscriptions DESC);
        CREATE INDEX IF NOT EXISTS idx_items_file_size      ON workshop_items(file_size_bytes DESC);
        CREATE INDEX IF NOT EXISTS idx_items_install_state  ON workshop_items(install_state);
    )";
    sqlite3_exec(m_db, sqlIndices, nullptr, nullptr, nullptr);

    const char *sqlFts = R"(
        CREATE VIRTUAL TABLE IF NOT EXISTS workshop_fts USING fts5 (
            title,
            tags_string,
            description,
            content='workshop_items',
            content_rowid='published_file_id',
            tokenize='unicode61 remove_diacritics 2',
            columnsize=0,
            detail=column
        );
    )";
    sqlite3_exec(m_db, sqlFts, nullptr, nullptr, nullptr);

    const char *sqlTriggers = R"(
        CREATE TRIGGER IF NOT EXISTS trg_workshop_items_ai AFTER INSERT ON workshop_items BEGIN
            INSERT INTO workshop_fts (rowid, title, tags_string, description)
            VALUES (new.published_file_id, new.title, new.tags_string, new.description);
        END;

        CREATE TRIGGER IF NOT EXISTS trg_workshop_items_ad AFTER DELETE ON workshop_items BEGIN
            INSERT INTO workshop_fts (workshop_fts, rowid, title, tags_string, description)
            VALUES ('delete', old.published_file_id, old.title, old.tags_string, old.description);
        END;

        CREATE TRIGGER IF NOT EXISTS trg_workshop_items_au
        AFTER UPDATE OF title, tags_string, description ON workshop_items BEGIN
            INSERT INTO workshop_fts (workshop_fts, rowid, title, tags_string, description)
            VALUES ('delete', old.published_file_id, old.title, old.tags_string, old.description);
            INSERT INTO workshop_fts (rowid, title, tags_string, description)
            VALUES (new.published_file_id, new.title, new.tags_string, new.description);
        END;
    )";
    sqlite3_exec(m_db, sqlTriggers, nullptr, nullptr, nullptr);

    // Optional FTS5 trigram index on title for fast 3+ character substring lookup.
    const char *sqlTrigram = R"(
        CREATE VIRTUAL TABLE IF NOT EXISTS workshop_fts_trigram USING fts5 (
            title,
            content='workshop_items',
            content_rowid='published_file_id',
            tokenize='trigram'
        );
    )";
    if (sqlite3_exec(m_db, sqlTrigram, nullptr, nullptr, nullptr) == SQLITE_OK) {
        m_impl->hasTrigramFts = true;
        const char *sqlTriTriggers = R"(
            CREATE TRIGGER IF NOT EXISTS trg_workshop_tri_ai AFTER INSERT ON workshop_items BEGIN
                INSERT INTO workshop_fts_trigram (rowid, title)
                VALUES (new.published_file_id, new.title);
            END;

            CREATE TRIGGER IF NOT EXISTS trg_workshop_tri_ad AFTER DELETE ON workshop_items BEGIN
                INSERT INTO workshop_fts_trigram (workshop_fts_trigram, rowid, title)
                VALUES ('delete', old.published_file_id, old.title);
            END;

            CREATE TRIGGER IF NOT EXISTS trg_workshop_tri_au
            AFTER UPDATE OF title ON workshop_items BEGIN
                INSERT INTO workshop_fts_trigram (workshop_fts_trigram, rowid, title)
                VALUES ('delete', old.published_file_id, old.title);
                INSERT INTO workshop_fts_trigram (rowid, title)
                VALUES (new.published_file_id, new.title);
            END;
        )";
        sqlite3_exec(m_db, sqlTriTriggers, nullptr, nullptr, nullptr);
    }
}

void CatalogIndexManager::prepareStatements()
{
    if (!m_db) {
        return;
    }

    const char *sqlUpsert = R"(
        INSERT INTO workshop_items (
            published_file_id, owner_steam_id, title, type, file_path, preview_path,
            file_size_bytes, time_created, time_updated, subscriptions, favorited,
            views, rating, install_state, storage_backend, compat_tier, compat_score,
            aspect_ratio, dominant_color, tag_mask, tags_string, description,
            manifest_id, file_clock, signature_verified
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(published_file_id) DO UPDATE SET
            owner_steam_id     = excluded.owner_steam_id,
            title              = excluded.title,
            type               = excluded.type,
            file_path          = excluded.file_path,
            preview_path       = excluded.preview_path,
            file_size_bytes    = excluded.file_size_bytes,
            time_created       = excluded.time_created,
            time_updated       = excluded.time_updated,
            subscriptions      = excluded.subscriptions,
            favorited          = excluded.favorited,
            views              = excluded.views,
            rating             = excluded.rating,
            install_state      = excluded.install_state,
            storage_backend    = excluded.storage_backend,
            compat_tier        = excluded.compat_tier,
            compat_score       = excluded.compat_score,
            aspect_ratio       = excluded.aspect_ratio,
            dominant_color     = excluded.dominant_color,
            tag_mask           = excluded.tag_mask,
            tags_string        = excluded.tags_string,
            description        = excluded.description,
            manifest_id        = excluded.manifest_id,
            file_clock         = CASE
                                     WHEN excluded.file_clock > workshop_items.file_clock
                                     THEN excluded.file_clock
                                     ELSE workshop_items.file_clock + 1
                                 END,
            signature_verified = excluded.signature_verified
        WHERE workshop_items.owner_steam_id != excluded.owner_steam_id
           OR workshop_items.title != excluded.title
           OR workshop_items.type != excluded.type
           OR workshop_items.file_path != excluded.file_path
           OR IFNULL(workshop_items.preview_path, '') != IFNULL(excluded.preview_path, '')
           OR workshop_items.file_size_bytes != excluded.file_size_bytes
           OR workshop_items.time_created != excluded.time_created
           OR workshop_items.time_updated != excluded.time_updated
           OR workshop_items.subscriptions != excluded.subscriptions
           OR workshop_items.favorited != excluded.favorited
           OR workshop_items.views != excluded.views
           OR workshop_items.rating != excluded.rating
           OR workshop_items.install_state != excluded.install_state
           OR workshop_items.storage_backend != excluded.storage_backend
           OR workshop_items.compat_tier != excluded.compat_tier
           OR workshop_items.compat_score != excluded.compat_score
           OR workshop_items.dominant_color != excluded.dominant_color
           OR workshop_items.tag_mask != excluded.tag_mask
           OR workshop_items.tags_string != excluded.tags_string
           OR workshop_items.description != excluded.description
           OR workshop_items.manifest_id != excluded.manifest_id
           OR workshop_items.signature_verified != excluded.signature_verified;
    )";
    sqlite3_prepare_v2(m_db, sqlUpsert, -1, &m_stmtUpsert, nullptr);

    const char *sqlDelete = "DELETE FROM workshop_items WHERE published_file_id = ?;";
    sqlite3_prepare_v2(m_db, sqlDelete, -1, &m_stmtDelete, nullptr);

    const char *sqlGetById = R"(
        SELECT published_file_id, owner_steam_id, title, type, file_path, preview_path,
               file_size_bytes, time_created, time_updated, subscriptions, favorited,
               views, rating, install_state, storage_backend, compat_tier, compat_score,
               aspect_ratio, dominant_color, tag_mask, tags_string, description,
               manifest_id, file_clock, signature_verified
        FROM workshop_items WHERE published_file_id = ?;
    )";
    sqlite3_prepare_v2(m_db, sqlGetById, -1, &m_stmtGetById, nullptr);

    const char *sqlGetByPath = R"(
        SELECT published_file_id, owner_steam_id, title, type, file_path, preview_path,
               file_size_bytes, time_created, time_updated, subscriptions, favorited,
               views, rating, install_state, storage_backend, compat_tier, compat_score,
               aspect_ratio, dominant_color, tag_mask, tags_string, description,
               manifest_id, file_clock, signature_verified
        FROM workshop_items WHERE file_path = ?;
    )";
    sqlite3_prepare_v2(m_db, sqlGetByPath, -1, &m_stmtGetByPath, nullptr);

    const char *sqlCount = "SELECT COUNT(*) FROM workshop_items;";
    sqlite3_prepare_v2(m_db, sqlCount, -1, &m_stmtCount, nullptr);

    const char *sqlFts5 = "SELECT rowid FROM workshop_fts WHERE workshop_fts MATCH ? LIMIT 4096;";
    sqlite3_prepare_v2(m_db, sqlFts5, -1, &m_stmtFts5, nullptr);

    if (m_impl->hasTrigramFts) {
        const char *sqlTri = R"(
            SELECT i.published_file_id, i.owner_steam_id, i.title, i.type, i.file_path, i.preview_path,
                   i.file_size_bytes, i.time_created, i.time_updated, i.subscriptions, i.favorited,
                   i.views, i.rating, i.install_state, i.storage_backend, i.compat_tier, i.compat_score,
                   i.aspect_ratio, i.dominant_color, i.tag_mask, i.tags_string, i.description,
                   i.manifest_id, i.file_clock, i.signature_verified,
                   f.rank
            FROM (
                SELECT rowid, bm25(workshop_fts_trigram) AS rank
                FROM workshop_fts_trigram
                WHERE workshop_fts_trigram MATCH ?
                ORDER BY rank
                LIMIT ? OFFSET ?
            ) f
            JOIN workshop_items i ON f.rowid = i.published_file_id
            ORDER BY f.rank;
        )";
        sqlite3_prepare_v2(m_db, sqlTri, -1, &m_stmtFtsTrigram, nullptr);
    }
}

void CatalogIndexManager::finalizeStatements()
{
    auto finalizeOne = [](sqlite3_stmt *&s) {
        if (s) {
            sqlite3_finalize(s);
            s = nullptr;
        }
    };
    finalizeOne(m_stmtUpsert);
    finalizeOne(m_stmtDelete);
    finalizeOne(m_stmtGetById);
    finalizeOne(m_stmtGetByPath);
    finalizeOne(m_stmtCount);
    finalizeOne(m_stmtFts5);
    finalizeOne(m_stmtFtsTrigram);
}

std::string CatalogIndexManager::sanitizeFts5Query(std::string_view rawQuery)
{
    if (rawQuery.empty()) {
        return {};
    }

    std::string cleaned;
    cleaned.reserve(rawQuery.size());
    for (char ch : rawQuery) {
        switch (ch) {
        case '*':
        case '"':
        case '-':
        case ':':
        case '^':
        case '(':
        case ')':
        case '{':
        case '}':
        case '+':
        case '~':
        case '[':
        case ']':
        case '?':
        case '=':
        case '<':
        case '>':
        case '/':
        case '\\':
        case ';':
        case '\'':
            cleaned.push_back(' ');
            break;
        default:
            cleaned.push_back(ch);
            break;
        }
    }

    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < cleaned.size()) {
        while (i < cleaned.size() && static_cast<unsigned char>(cleaned[i]) <= ' ') {
            ++i;
        }
        if (i >= cleaned.size()) {
            break;
        }
        size_t start = i;
        while (i < cleaned.size() && static_cast<unsigned char>(cleaned[i]) > ' ') {
            ++i;
        }
        std::string token = cleaned.substr(start, i - start);
        if (!token.empty()) {
            tokens.push_back(std::move(token));
        }
    }

    std::string result;
    for (size_t t = 0; t < tokens.size(); ++t) {
        if (!result.empty()) {
            result.push_back(' ');
        }
        result.push_back('"');
        result.append(tokens[t]);
        result.push_back('"');
        if (t + 1 == tokens.size()) {
            result.push_back('*');
        }
    }

    return result;
}

bool CatalogIndexManager::upsertItem(const WorkshopItemRecord &item)
{
    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    if (!m_db || !m_stmtUpsert) {
        return false;
    }

    sqlite3_reset(m_stmtUpsert);
    sqlite3_clear_bindings(m_stmtUpsert);
    bindRecordToStmt(m_stmtUpsert, item);

    int rc = sqlite3_step(m_stmtUpsert);
    sqlite3_reset(m_stmtUpsert);
    if (rc == SQLITE_DONE) {
        m_impl->upsertSoaLocked(item.published_file_id, item.tag_mask, item.title, item.tags_string,
                                item.description);
        return true;
    }
    return false;
}

bool CatalogIndexManager::upsertItems(const std::vector<WorkshopItemRecord> &items)
{
    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    if (!m_db || !m_stmtUpsert) {
        return false;
    }

    sqlite3_exec(m_db, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, nullptr);

    bool allOk = true;
    for (const auto &item : items) {
        sqlite3_reset(m_stmtUpsert);
        sqlite3_clear_bindings(m_stmtUpsert);
        bindRecordToStmt(m_stmtUpsert, item);
        int rc = sqlite3_step(m_stmtUpsert);
        if (rc != SQLITE_DONE) {
            allOk = false;
            break;
        }
    }

    if (allOk) {
        if (items.size() >= 1000) {
            sqlite3_exec(m_db, "INSERT INTO workshop_fts(workshop_fts) VALUES('optimize');",
                         nullptr, nullptr, nullptr);
            if (m_impl->hasTrigramFts) {
                sqlite3_exec(
                    m_db,
                    "INSERT INTO workshop_fts_trigram(workshop_fts_trigram) VALUES('optimize');",
                    nullptr, nullptr, nullptr);
            }
        }
        sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr);
        if (items.size() >= 1000) {
            // Checkpoint bulk batches into the main database file so mmap I/O covers all pages.
            sqlite3_wal_checkpoint_v2(m_db, nullptr, SQLITE_CHECKPOINT_PASSIVE, nullptr, nullptr);
        }
        m_impl->soaPubIds.reserve(m_impl->soaPubIds.size() + items.size());
        m_impl->soaTagMasks.reserve(m_impl->soaTagMasks.size() + items.size());
        for (const auto &item : items) {
            m_impl->upsertSoaLocked(item.published_file_id, item.tag_mask, item.title,
                                    item.tags_string, item.description);
        }
    } else {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
    }

    sqlite3_reset(m_stmtUpsert);
    return allOk;
}

bool CatalogIndexManager::removeItem(uint64_t publishedFileId)
{
    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    if (!m_db || !m_stmtDelete) {
        return false;
    }

    sqlite3_reset(m_stmtDelete);
    sqlite3_clear_bindings(m_stmtDelete);
    sqlite3_bind_int64(m_stmtDelete, 1, static_cast<sqlite3_int64>(publishedFileId));

    int rc = sqlite3_step(m_stmtDelete);
    sqlite3_reset(m_stmtDelete);
    if (rc == SQLITE_DONE) {
        m_impl->removeSoaLocked(publishedFileId);
        return true;
    }
    return false;
}

std::optional<WorkshopItemRecord> CatalogIndexManager::getItem(uint64_t publishedFileId) const
{
    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    if (!m_db || !m_stmtGetById) {
        return std::nullopt;
    }

    sqlite3_reset(m_stmtGetById);
    sqlite3_clear_bindings(m_stmtGetById);
    sqlite3_bind_int64(m_stmtGetById, 1, static_cast<sqlite3_int64>(publishedFileId));

    std::optional<WorkshopItemRecord> result;
    if (sqlite3_step(m_stmtGetById) == SQLITE_ROW) {
        result = readRecordFromStmt(m_stmtGetById);
    }
    sqlite3_reset(m_stmtGetById);
    return result;
}

std::optional<WorkshopItemRecord>
CatalogIndexManager::getItemByPath(const std::string &filePath) const
{
    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    if (!m_db || !m_stmtGetByPath) {
        return std::nullopt;
    }

    sqlite3_reset(m_stmtGetByPath);
    sqlite3_clear_bindings(m_stmtGetByPath);
    sqlite3_bind_text(m_stmtGetByPath, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<WorkshopItemRecord> result;
    if (sqlite3_step(m_stmtGetByPath) == SQLITE_ROW) {
        result = readRecordFromStmt(m_stmtGetByPath);
    }
    sqlite3_reset(m_stmtGetByPath);
    return result;
}

size_t CatalogIndexManager::totalItemCount() const
{
    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    if (!m_db || !m_stmtCount) {
        return 0;
    }

    sqlite3_reset(m_stmtCount);
    size_t count = 0;
    if (sqlite3_step(m_stmtCount) == SQLITE_ROW) {
        count = static_cast<size_t>(sqlite3_column_int64(m_stmtCount, 0));
    }
    sqlite3_reset(m_stmtCount);
    return count;
}

size_t CatalogIndexManager::filterByTagMaskBitmap(uint64_t requiredMask,
                                                  std::vector<uint64_t> *outIds) const
{
    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    const size_t n = m_impl->soaPubIds.size();
    const uint64_t *ids = m_impl->soaPubIds.data();

    if (outIds) {
        outIds->clear();
    }
    if (n == 0) {
        return 0;
    }
    if (requiredMask == 0) {
        if (outIds) {
            outIds->assign(ids, ids + n);
        }
        return n;
    }

    std::array<const uint64_t *, 64> activePlanes{};
    size_t planeCount = 0;
    for (uint64_t m = requiredMask; m != 0; m &= (m - 1ULL)) {
        unsigned b = static_cast<unsigned>(std::countr_zero(m));
        activePlanes[planeCount++] = m_impl->tagBitsets[b].data();
    }

    const size_t wordCount = (n + 63u) >> 6;
    const unsigned tailBits = static_cast<unsigned>(n & 63u);
    const uint64_t tailMask = (tailBits == 0u) ? ~0ULL : ((1ULL << tailBits) - 1ULL);

    if (outIds) {
        for (size_t w = 0; w < wordCount; ++w) {
            uint64_t bits = activePlanes[0][w];
            for (size_t p = 1; p < planeCount; ++p) {
                bits &= activePlanes[p][w];
            }
            if (w + 1 == wordCount) {
                bits &= tailMask;
            }
            while (bits != 0) {
                unsigned bitIdx = static_cast<unsigned>(std::countr_zero(bits));
                outIds->push_back(ids[(w << 6) + bitIdx]);
                bits &= (bits - 1ULL);
            }
        }
        return outIds->size();
    }

    size_t matches = 0;
    for (size_t w = 0; w < wordCount; ++w) {
        uint64_t bits = activePlanes[0][w];
        for (size_t p = 1; p < planeCount; ++p) {
            bits &= activePlanes[p][w];
        }
        if (w + 1 == wordCount) {
            bits &= tailMask;
        }
        matches += static_cast<size_t>(std::popcount(bits));
    }
    return matches;
}

std::vector<WorkshopItemRecord> CatalogIndexManager::searchFuzzy(const std::string &query,
                                                                 uint64_t requiredTagMask,
                                                                 int limit) const
{
    const int cappedLimit = limit > 0 ? limit : 50;

    // Trim whitespace for pattern extraction.
    std::string trimmed;
    for (char ch : query) {
        if (static_cast<unsigned char>(ch) > ' ' || !trimmed.empty()) {
            trimmed.push_back(ch);
        }
    }
    while (!trimmed.empty() && static_cast<unsigned char>(trimmed.back()) <= ' ') {
        trimmed.pop_back();
    }
    if (trimmed.empty()) {
        return {};
    }

    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    if (!m_db) {
        return {};
    }

    // 1. Try trigram substring index when pattern has >= 3 characters and no tag filter.
    std::vector<WorkshopItemRecord> results;
    if (m_stmtFtsTrigram && requiredTagMask == 0 && trimmed.size() >= 3 &&
        trimmed.find('"') == std::string::npos) {
        std::string triPhrase = "\"" + trimmed + "\"";
        sqlite3_reset(m_stmtFtsTrigram);
        sqlite3_clear_bindings(m_stmtFtsTrigram);
        sqlite3_bind_text(m_stmtFtsTrigram, 1, triPhrase.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(m_stmtFtsTrigram, 2, cappedLimit);
        sqlite3_bind_int(m_stmtFtsTrigram, 3, 0);
        while (sqlite3_step(m_stmtFtsTrigram) == SQLITE_ROW) {
            results.push_back(readRecordFromStmt(m_stmtFtsTrigram));
        }
        sqlite3_reset(m_stmtFtsTrigram);
        if (!results.empty()) {
            return results;
        }
    }

    // 2. 64-bit Myers bit-parallel approximate substring scan combined with tag-mask bitmap.
    std::string pattern = toLowerAscii(trimmed);
    if (pattern.size() > 64) {
        pattern.resize(64);
    }
    const uint32_t m = static_cast<uint32_t>(pattern.size());
    const uint32_t maxDist = (m <= 3) ? 0u : ((m <= 5) ? 1u : std::min<uint32_t>(2u, m / 3u));

    std::array<uint64_t, 256> peq{};
    for (uint32_t i = 0; i < m; ++i) {
        peq[static_cast<unsigned char>(pattern[i])] |= (1ULL << i);
    }

    struct CandidateHit {
        uint32_t dist;
        uint32_t titleLen;
        uint64_t pubId;
    };
    std::vector<CandidateHit> hits;
    hits.reserve(static_cast<size_t>(cappedLimit) * 2);

    const size_t n = m_impl->soaPubIds.size();
    const uint64_t *masks = m_impl->soaTagMasks.data();
    const uint64_t *ids = m_impl->soaPubIds.data();
    const uint32_t *offs = m_impl->soaTitleOffsets.data();
    const uint32_t *lens = m_impl->soaTitleLengths.data();
    const char *arena = m_impl->soaLowerTitles.data();

    for (size_t i = 0; i < n; ++i) {
        if (requiredTagMask != 0 && (masks[i] & requiredTagMask) != requiredTagMask) {
            continue;
        }
        std::string_view titleView(arena + offs[i], lens[i]);
        uint32_t d = myersSubstringDistance64(titleView, peq, m);
        if (d <= maxDist) {
            hits.push_back(CandidateHit{d, lens[i], ids[i]});
        }
    }

    if (hits.empty()) {
        return {};
    }

    size_t keep = std::min(hits.size(), static_cast<size_t>(cappedLimit));
    std::partial_sort(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(keep), hits.end(),
                      [](const CandidateHit &a, const CandidateHit &b) {
                          if (a.dist != b.dist) {
                              return a.dist < b.dist;
                          }
                          if (a.titleLen != b.titleLen) {
                              return a.titleLen < b.titleLen;
                          }
                          return a.pubId < b.pubId;
                      });

    results.reserve(keep);
    if (m_stmtGetById) {
        for (size_t i = 0; i < keep; ++i) {
            sqlite3_reset(m_stmtGetById);
            sqlite3_clear_bindings(m_stmtGetById);
            sqlite3_bind_int64(m_stmtGetById, 1, static_cast<sqlite3_int64>(hits[i].pubId));
            if (sqlite3_step(m_stmtGetById) == SQLITE_ROW) {
                WorkshopItemRecord rec = readRecordFromStmt(m_stmtGetById);
                rec.search_rank = -static_cast<double>(100u - hits[i].dist * 25u);
                results.push_back(std::move(rec));
            }
            sqlite3_reset(m_stmtGetById);
        }
    }
    return results;
}

std::vector<WorkshopItemRecord> CatalogIndexManager::searchFts5Bm25(const std::string &query,
                                                                    int limit, int offset) const
{
    std::string sanitized = sanitizeFts5Query(query);
    if (sanitized.empty()) {
        return {};
    }

    const int cappedLimit = limit > 0 ? limit : 50;
    const size_t startOffset = static_cast<size_t>(std::max(0, offset));

    std::vector<std::string> lowerTokens;
    {
        std::string cur;
        for (char ch : sanitized) {
            if (ch == '"' || ch == '*') {
                continue;
            }
            if (static_cast<unsigned char>(ch) <= ' ') {
                if (!cur.empty()) {
                    lowerTokens.push_back(toLowerAscii(cur));
                    cur.clear();
                }
            } else {
                cur.push_back(ch);
            }
        }
        if (!cur.empty()) {
            lowerTokens.push_back(toLowerAscii(cur));
        }
    }

    std::vector<WorkshopItemRecord> results;
    results.reserve(static_cast<size_t>(cappedLimit));
    {
        std::lock_guard<std::mutex> lock(m_impl->dbMutex);
        if (!m_db || !m_stmtFts5 || !m_stmtGetById) {
            return {};
        }

        // Stream matching rowids directly from the FTS5 inverted posting list without
        // invoking SQLite per-row bm25() virtual-table callbacks.
        std::vector<uint64_t> candIds;
        candIds.reserve(256);
        sqlite3_reset(m_stmtFts5);
        sqlite3_clear_bindings(m_stmtFts5);
        sqlite3_bind_text(m_stmtFts5, 1, sanitized.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(m_stmtFts5) == SQLITE_ROW) {
            candIds.push_back(static_cast<uint64_t>(sqlite3_column_int64(m_stmtFts5, 0)));
        }
        sqlite3_reset(m_stmtFts5);

        if (!candIds.empty()) {
            const double totalDocs =
                static_cast<double>(std::max<size_t>(1, m_impl->soaPubIds.size()));
            const double df = static_cast<double>(candIds.size());
            const double idf = std::log((totalDocs - df + 0.5) / (df + 0.5) + 1.0);

            const double avgTitleLen =
                std::max(1.0, static_cast<double>(m_impl->totalTitleBytes) / totalDocs);
            const double avgTagsLen =
                std::max(1.0, static_cast<double>(m_impl->totalTagsBytes) / totalDocs);
            const double avgDescLen =
                std::max(1.0, static_cast<double>(m_impl->totalDescBytes) / totalDocs);

            constexpr double k1 = 1.2;
            constexpr double b = 0.75;

            struct ScoredRow {
                double score;
                uint64_t pubId;
            };
            std::vector<ScoredRow> scored;
            scored.reserve(candIds.size());

            const char *titleArena = m_impl->soaLowerTitles.data();
            const char *tagsArena = m_impl->soaLowerTags.data();

            for (uint64_t pubId : candIds) {
                double docScore = 0.0;
                auto slotIt = m_impl->idToSlot.find(pubId);
                if (slotIt != m_impl->idToSlot.end()) {
                    size_t slot = slotIt->second;
                    std::string_view tv(titleArena + m_impl->soaTitleOffsets[slot],
                                        m_impl->soaTitleLengths[slot]);
                    std::string_view gv(tagsArena + m_impl->soaTagsOffsets[slot],
                                        m_impl->soaTagsLengths[slot]);
                    double descLen = static_cast<double>(m_impl->soaDescLengths[slot]);

                    double titleNorm = 1.0 + k1 * ((1.0 - b) + b * (tv.size() / avgTitleLen));
                    double tagsNorm = 1.0 + k1 * ((1.0 - b) + b * (gv.size() / avgTagsLen));
                    double descNorm = 1.0 + k1 * ((1.0 - b) + b * (descLen / avgDescLen));

                    for (const auto &tok : lowerTokens) {
                        bool matchedTitleOrTag = false;
                        if (tv.find(tok) != std::string_view::npos) {
                            docScore += 5.0 * idf * ((k1 + 1.0) / titleNorm);
                            matchedTitleOrTag = true;
                        }
                        if (gv.find(tok) != std::string_view::npos) {
                            docScore += 2.0 * idf * ((k1 + 1.0) / tagsNorm);
                            matchedTitleOrTag = true;
                        }
                        if (!matchedTitleOrTag) {
                            docScore += 1.0 * idf * ((k1 + 1.0) / descNorm);
                        }
                    }
                } else {
                    docScore = idf;
                }
                scored.push_back(ScoredRow{docScore, pubId});
            }

            if (startOffset < scored.size()) {
                const size_t endIdx =
                    std::min(scored.size(), startOffset + static_cast<size_t>(cappedLimit));
                std::partial_sort(scored.begin(),
                                  scored.begin() + static_cast<std::ptrdiff_t>(endIdx),
                                  scored.end(), [](const ScoredRow &lhs, const ScoredRow &rhs) {
                                      if (lhs.score != rhs.score) {
                                          return lhs.score > rhs.score;
                                      }
                                      return lhs.pubId < rhs.pubId;
                                  });

                for (size_t i = startOffset; i < endIdx; ++i) {
                    sqlite3_reset(m_stmtGetById);
                    sqlite3_clear_bindings(m_stmtGetById);
                    sqlite3_bind_int64(m_stmtGetById, 1,
                                       static_cast<sqlite3_int64>(scored[i].pubId));
                    if (sqlite3_step(m_stmtGetById) == SQLITE_ROW) {
                        WorkshopItemRecord rec = readRecordFromStmt(m_stmtGetById);
                        rec.search_rank = -scored[i].score;
                        results.push_back(std::move(rec));
                    }
                    sqlite3_reset(m_stmtGetById);
                }
            }
        }
    }

    if (results.empty() && offset == 0) {
        return searchFuzzy(query, 0, limit);
    }
    return results;
}

std::vector<WorkshopItemRecord>
CatalogIndexManager::queryByFilters(const CatalogFilter &filter) const
{
    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    if (!m_db) {
        return {};
    }

    std::ostringstream sql;
    sql << "SELECT published_file_id, owner_steam_id, title, type, file_path, preview_path, "
           "file_size_bytes, time_created, time_updated, subscriptions, favorited, "
           "views, rating, install_state, storage_backend, compat_tier, compat_score, "
           "aspect_ratio, dominant_color, tag_mask, tags_string, description, "
           "manifest_id, file_clock, signature_verified "
           "FROM workshop_items";

    std::vector<std::string> conditions;
    if (!filter.type.empty() && filter.type != "all" && filter.type != "All") {
        conditions.push_back("type = ?");
    }
    if (filter.compat_tier > 0) {
        conditions.push_back("compat_tier = ?");
    }
    if (filter.required_tag_mask != 0) {
        conditions.push_back("(tag_mask & ?) = ?");
    }
    if (filter.install_state_mask != 0) {
        conditions.push_back("(install_state & ?) != 0");
    }

    if (!conditions.empty()) {
        sql << " WHERE ";
        for (size_t i = 0; i < conditions.size(); ++i) {
            if (i > 0) {
                sql << " AND ";
            }
            sql << conditions[i];
        }
    }

    switch (filter.sort_order) {
    case CatalogSortOrder::SubscriptionsDesc:
        sql << " ORDER BY subscriptions DESC";
        break;
    case CatalogSortOrder::RatingDesc:
        sql << " ORDER BY rating DESC, published_file_id ASC";
        break;
    case CatalogSortOrder::RecentlyUpdated:
        sql << " ORDER BY time_updated DESC, published_file_id ASC";
        break;
    case CatalogSortOrder::CompatibilityScoreDesc:
        sql << " ORDER BY compat_score DESC, subscriptions DESC";
        break;
    case CatalogSortOrder::TitleAsc:
        sql << " ORDER BY title COLLATE NOCASE ASC";
        break;
    case CatalogSortOrder::FileSizeDesc:
        sql << " ORDER BY file_size_bytes DESC";
        break;
    }

    sql << " LIMIT ? OFFSET ?;";

    std::string sqlStr = sql.str();
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sqlStr.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return {};
    }

    int bindIdx = 1;
    if (!filter.type.empty() && filter.type != "all" && filter.type != "All") {
        sqlite3_bind_text(stmt, bindIdx++, filter.type.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (filter.compat_tier > 0) {
        sqlite3_bind_int(stmt, bindIdx++, static_cast<int>(filter.compat_tier));
    }
    if (filter.required_tag_mask != 0) {
        sqlite3_bind_int64(stmt, bindIdx++, static_cast<sqlite3_int64>(filter.required_tag_mask));
        sqlite3_bind_int64(stmt, bindIdx++, static_cast<sqlite3_int64>(filter.required_tag_mask));
    }
    if (filter.install_state_mask != 0) {
        sqlite3_bind_int64(stmt, bindIdx++, static_cast<sqlite3_int64>(filter.install_state_mask));
    }

    int lim = filter.limit > 0 ? filter.limit : 50;
    int off = filter.offset >= 0 ? filter.offset : 0;
    sqlite3_bind_int(stmt, bindIdx++, lim);
    sqlite3_bind_int(stmt, bindIdx++, off);

    std::vector<WorkshopItemRecord> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(readRecordFromStmt(stmt));
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<WorkshopItemRecord> CatalogIndexManager::queryByFilters(const std::string &type,
                                                                    uint32_t compat_tier,
                                                                    uint64_t tag_mask,
                                                                    CatalogSortOrder sort_order,
                                                                    int limit, int offset) const
{
    CatalogFilter f;
    f.type = type;
    f.compat_tier = compat_tier;
    f.required_tag_mask = tag_mask;
    f.sort_order = sort_order;
    f.limit = limit;
    f.offset = offset;
    return queryByFilters(f);
}

size_t CatalogIndexManager::reconcileAllLibraries()
{
    size_t totalIndexed = 0;

    waywal_steam_library_t libs[32];
    size_t libCount = waywal_steam_discover_libraries(libs, 32);

    for (size_t i = 0; i < libCount; ++i) {
        std::filesystem::path libPath(libs[i].path);
        std::filesystem::path wsDir = libPath / "steamapps" / "workshop" / "content" / "431960";
        std::error_code ec;
        if (std::filesystem::exists(wsDir, ec)) {
            totalIndexed += reconcileDirectory(wsDir, 0);
        }
    }

    const char *xdgData = std::getenv("XDG_DATA_HOME");
    std::filesystem::path dataRoot;
    if (xdgData && xdgData[0] != '\0') {
        dataRoot = std::filesystem::path(xdgData);
    } else {
        const char *home = std::getenv("HOME");
        dataRoot = (home && home[0] != '\0') ? std::filesystem::path(home) / ".local" / "share"
                                             : std::filesystem::current_path();
    }

    std::error_code ec;
    for (const auto &hubPath : {dataRoot / "waywal" / "hub", dataRoot / "wwal" / "hub"}) {
        if (std::filesystem::exists(hubPath, ec)) {
            totalIndexed += reconcileDirectory(hubPath, 2);
        }
    }

    return totalIndexed;
}

namespace {

bool buildLooseItemRecord(const std::filesystem::path &itemDir, uint64_t publishedFileId,
                          uint32_t storageBackend, const waywal_workshop_item_manifest_t *acfEntry,
                          IoUringStatxBatcher &batcher, size_t &statCounter,
                          WorkshopItemRecord &outRec)
{
    if (publishedFileId == 0) {
        try {
            publishedFileId = std::stoull(itemDir.filename().string());
        } catch (...) {
            publishedFileId = std::hash<std::string>{}(itemDir.filename().string());
        }
    }

    WorkshopItemRecord rec;
    rec.published_file_id = publishedFileId;
    rec.file_path = itemDir.string();
    rec.storage_backend = storageBackend;
    rec.compat_tier = 1;
    rec.compat_score = 100;
    rec.aspect_ratio = 1.7777778f;
    rec.dominant_color = 0x2b303c;
    rec.signature_verified = true;

    we_project_t proj{};
    bool hasProject = we_project_load_dir(itemDir.string().c_str(), &proj);
    if (hasProject) {
        rec.title = proj.title[0] ? proj.title : itemDir.filename().string();
        rec.type = proj.type[0] ? proj.type : "scene";
        if (proj.preview[0]) {
            rec.preview_path = resolveConfinedPreviewPath(itemDir, proj.preview);
        }
    } else {
        rec.title = itemDir.filename().string();
        rec.type = "scene";
    }

    parseRootProjectJsonMetadata(itemDir, rec.title, rec.type, rec.preview_path, rec.tags_string,
                                 rec.description);

    if (rec.preview_path.empty()) {
        const char *previewCandidates[] = {"preview.jpg", "preview.png", "preview.webp",
                                           "preview.gif", "preview_card.jpg"};
        for (const char *cand : previewCandidates) {
            std::string resolved = resolveConfinedPreviewPath(itemDir, cand);
            if (!resolved.empty()) {
                rec.preview_path = std::move(resolved);
                break;
            }
        }
    }

    we_compat_scanner_t *scanner = we_compat_scanner_create(true, true);
    we_compat_report_t *report = we_compat_report_alloc();
    if (scanner && report) {
        if (we_compat_scan_directory(scanner, itemDir.string().c_str(), report)) {
            rec.compat_tier = static_cast<uint32_t>(report->tier);
            rec.compat_score = report->compatibility_score;
            if (rec.title.empty() && report->title[0]) {
                rec.title = report->title;
            }
        }
    }
    if (report) {
        we_compat_report_free(report);
    }
    if (scanner) {
        we_compat_scanner_destroy(scanner);
    }

    if (acfEntry && acfEntry->time_updated > 0 && acfEntry->size_bytes > 0) {
        rec.file_size_bytes = acfEntry->size_bytes;
        rec.time_updated = acfEntry->time_updated;
        rec.manifest_id = acfEntry->manifest_id;
        rec.install_state = acfEntry->state_flags | WAYWAL_UGC_STATE_INSTALLED;
    } else {
        std::vector<std::string> files;
        collectRegularFilesNoStat(itemDir.string(), files);
        uint64_t totalBytes = 0;
        uint64_t latestMtime = 0;
        batcher.queryFiles(files, totalBytes, latestMtime, statCounter);

        rec.file_size_bytes =
            (acfEntry && acfEntry->size_bytes > 0) ? acfEntry->size_bytes : totalBytes;
        rec.time_updated =
            (acfEntry && acfEntry->time_updated > 0) ? acfEntry->time_updated : latestMtime;
        rec.manifest_id = acfEntry ? acfEntry->manifest_id : 0;
        rec.install_state = (acfEntry ? acfEntry->state_flags : 0u) | WAYWAL_UGC_STATE_INSTALLED;
    }

    rec.tag_mask = WallpaperTags::parseTagsFromString(rec.title + " " + rec.tags_string);
    outRec = std::move(rec);
    return true;
}

} // namespace

size_t CatalogIndexManager::reconcileDirectory(const std::filesystem::path &workshopDir,
                                               uint32_t storageBackend)
{
    m_impl->lastReconcileStatCalls.store(0, std::memory_order_relaxed);

    DIR *topDir = ::opendir(workshopDir.c_str());
    if (!topDir) {
        return 0;
    }

    // 1. Parse Steam appworkshop_431960.acf / .vdf manifest when reconciling SteamLocal.
    std::unordered_map<uint64_t, waywal_workshop_item_manifest_t> manifestMap;
    if (storageBackend == 0) {
        const std::vector<std::filesystem::path> candidateManifests = {
            workshopDir.parent_path() / "appworkshop_431960.acf",
            workshopDir.parent_path() / "appworkshop_431960.vdf",
            workshopDir.parent_path().parent_path() / "appworkshop_431960.acf",
            workshopDir.parent_path().parent_path() / "appworkshop_431960.vdf",
            workshopDir / "appworkshop_431960.acf",
            workshopDir / "appworkshop_431960.vdf",
        };

        for (const auto &mPath : candidateManifests) {
            if (::access(mPath.c_str(), R_OK) == 0) {
                waywal_appworkshop_manifest_t manifest{};
                if (waywal_appworkshop_parse_file(mPath.string().c_str(), &manifest)) {
                    manifestMap.reserve(manifest.item_count);
                    for (size_t i = 0; i < manifest.item_count; ++i) {
                        manifestMap[manifest.items[i].published_file_id] = manifest.items[i];
                    }
                    waywal_appworkshop_free(&manifest);
                    break;
                }
            }
        }
    }

    // 2. Snapshot existing (published_file_id -> time_updated) pairs from SQLite in one pass.
    std::unordered_map<uint64_t, uint64_t> existingTimestamps;
    {
        std::lock_guard<std::mutex> lock(m_impl->dbMutex);
        if (m_db) {
            sqlite3_stmt *stmt = nullptr;
            const char *sql = "SELECT published_file_id, time_updated FROM workshop_items WHERE "
                              "storage_backend = ?;";
            if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(storageBackend));
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    uint64_t id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
                    uint64_t ts = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
                    existingTimestamps.emplace(id, ts);
                }
                sqlite3_finalize(stmt);
            }
        }
    }

    size_t indexedCount = 0;
    size_t statCalls = 0;
    IoUringStatxBatcher statxBatcher;
    std::vector<WorkshopItemRecord> dirtyRecords;

    while (struct dirent *ent = ::readdir(topDir)) {
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        std::filesystem::path entryPath = workshopDir / ent->d_name;
        bool isDir = (ent->d_type == DT_DIR);
        bool isReg = (ent->d_type == DT_REG);
        if (ent->d_type == DT_UNKNOWN) {
            struct statx stx{};
            ++statCalls;
            if (::statx(AT_FDCWD, entryPath.c_str(), AT_SYMLINK_NOFOLLOW, STATX_TYPE, &stx) == 0) {
                isDir = S_ISDIR(stx.stx_mode);
                isReg = S_ISREG(stx.stx_mode);
            }
        }

        if (isDir) {
            uint64_t id = 0;
            try {
                id = std::stoull(ent->d_name);
            } catch (...) {
                id = std::hash<std::string>{}(ent->d_name);
            }

            const waywal_workshop_item_manifest_t *acfPtr = nullptr;
            auto mfIt = manifestMap.find(id);
            if (mfIt != manifestMap.end()) {
                acfPtr = &mfIt->second;
            }

            // Zero-stat fast path: unchanged (published_file_id, time_updated) in manifest.
            if (acfPtr && acfPtr->time_updated > 0) {
                auto exIt = existingTimestamps.find(id);
                if (exIt != existingTimestamps.end() && exIt->second == acfPtr->time_updated) {
                    ++indexedCount;
                    continue;
                }
            }

            WorkshopItemRecord rec;
            if (buildLooseItemRecord(entryPath, id, storageBackend, acfPtr, statxBatcher, statCalls,
                                     rec)) {
                dirtyRecords.push_back(std::move(rec));
            }
        } else if (storageBackend == 2 && isReg && entryPath.extension() == ".wwpkg") {
            struct statx stx{};
            ++statCalls;
            uint64_t fsize = 0;
            uint64_t mtime = 0;
            if (::statx(AT_FDCWD, entryPath.c_str(), AT_SYMLINK_NOFOLLOW, STATX_SIZE | STATX_MTIME,
                        &stx) == 0) {
                fsize = stx.stx_size;
                if (stx.stx_mtime.tv_sec > 0) {
                    mtime = static_cast<uint64_t>(stx.stx_mtime.tv_sec);
                }
            }

            wwpkg_package_t *pkg = wwpkg_open(entryPath.string().c_str());
            if (pkg) {
                const wwpkg_manifest_t *man = wwpkg_get_manifest(pkg);
                bool isTrusted = false;
                bool sigVerified = wwpkg_verify_signature(pkg, &isTrusted);
                if (man) {
                    uint64_t id = 0;
                    for (size_t k = 0; k < sizeof(man->id) && man->id[k]; ++k) {
                        id = (id * 1099511628211ULL) ^ static_cast<uint8_t>(man->id[k]);
                    }
                    if (id == 0) {
                        id = std::hash<std::string>{}(man->slug);
                    }

                    WorkshopItemRecord rec;
                    rec.published_file_id = id;
                    rec.title = man->name[0] ? man->name : man->slug;
                    if (man->type == WWPKG_TYPE_VIDEO_STREAM) {
                        rec.type = "video";
                    } else if (man->type == WWPKG_TYPE_SHADER_FRAGMENT ||
                               man->type == WWPKG_TYPE_SHADER_COMPUTE) {
                        rec.type = "shader";
                    } else {
                        rec.type = "scene";
                    }
                    rec.file_path = entryPath.string();
                    rec.file_size_bytes = fsize;
                    rec.time_updated = mtime;
                    rec.storage_backend = 2;
                    rec.signature_verified = sigVerified;
                    rec.compat_tier = sigVerified ? 1 : 4;
                    rec.compat_score = sigVerified ? 100 : 0;
                    rec.description = man->description;

                    std::string tags;
                    for (uint32_t t = 0; t < man->tag_count; ++t) {
                        if (!tags.empty()) {
                            tags += ", ";
                        }
                        tags += man->tags[t];
                    }
                    rec.tags_string = tags;
                    rec.tag_mask = WallpaperTags::parseTagsFromString(rec.title + " " + tags);
                    rec.install_state = WAYWAL_UGC_STATE_INSTALLED;

                    dirtyRecords.push_back(std::move(rec));
                }
                wwpkg_close(pkg);
            } else {
                // Quarantine corrupt or unreadable .wwpkg archives so UI callers keep Apply
                // disabled.
                uint64_t id = 14695981039346656037ULL;
                const std::string pathStr = entryPath.string();
                for (unsigned char ch : pathStr) {
                    id ^= ch;
                    id *= 1099511628211ULL;
                }
                WorkshopItemRecord rec;
                rec.published_file_id = id;
                rec.title = entryPath.stem().string();
                rec.type = "unknown";
                rec.file_path = pathStr;
                rec.file_size_bytes = fsize;
                rec.time_updated = mtime;
                rec.storage_backend = 2;
                rec.signature_verified = false;
                rec.compat_tier = 4;
                rec.compat_score = 0;
                rec.install_state = WAYWAL_UGC_STATE_INSTALLED;
                dirtyRecords.push_back(std::move(rec));
            }
        }
    }

    ::closedir(topDir);
    m_impl->lastReconcileStatCalls.store(statCalls, std::memory_order_relaxed);

    if (!dirtyRecords.empty()) {
        if (upsertItems(dirtyRecords)) {
            indexedCount += dirtyRecords.size();
        }
    }

    return indexedCount;
}

bool CatalogIndexManager::reconcileItemDirectory(const std::filesystem::path &itemDir,
                                                 uint64_t publishedFileId, uint32_t storageBackend)
{
    std::error_code ec;
    if (!std::filesystem::exists(itemDir, ec) || !std::filesystem::is_directory(itemDir, ec)) {
        return false;
    }

    IoUringStatxBatcher batcher;
    size_t statCalls = 0;
    WorkshopItemRecord rec;
    if (!buildLooseItemRecord(itemDir, publishedFileId, storageBackend, nullptr, batcher, statCalls,
                              rec)) {
        return false;
    }
    m_impl->lastReconcileStatCalls.store(statCalls, std::memory_order_relaxed);
    return upsertItem(rec);
}

} // namespace wwal::studio
