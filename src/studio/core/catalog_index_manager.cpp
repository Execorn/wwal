// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/core/catalog_index_manager.hpp"
#include "studio/core/catalog_soa_rcu_pool.hpp"

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
#include <memory>
#include <mutex>
#include <sstream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

#if __has_include(<linux/io_uring.h>)
#include <linux/io_uring.h>
#define WWAL_HAS_LINUX_IO_URING_HDR 1
#else
#define WWAL_HAS_LINUX_IO_URING_HDR 0
#endif

#include <charconv>
#include <limits>
#include <span>

#include "waywal/os_compat.h"
#include "waywal/we_appworkshop.h"
#include "waywal/we_compat_scanner.h"
#include "waywal/we_json.h"
#include "waywal/we_project.h"
#include "waywal/we_vdf.h"
#include "waywal/wwpkg.h"

namespace wwal::studio {

namespace {

uint32_t decodeUtf8Next(std::string_view sv, size_t &pos) noexcept
{
    const auto b0 = static_cast<unsigned char>(sv[pos]);
    if (b0 < 0x80u) {
        ++pos;
        return b0;
    }
    if ((b0 & 0xE0u) == 0xC0u && pos + 1 < sv.size()) {
        const auto b1 = static_cast<unsigned char>(sv[pos + 1]);
        if ((b1 & 0xC0u) == 0x80u) {
            const uint32_t cp =
                (static_cast<uint32_t>(b0 & 0x1Fu) << 6) | static_cast<uint32_t>(b1 & 0x3Fu);
            if (cp >= 0x80u) {
                pos += 2;
                return cp;
            }
        }
    } else if ((b0 & 0xF0u) == 0xE0u && pos + 2 < sv.size()) {
        const auto b1 = static_cast<unsigned char>(sv[pos + 1]);
        const auto b2 = static_cast<unsigned char>(sv[pos + 2]);
        if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u) {
            const uint32_t cp = (static_cast<uint32_t>(b0 & 0x0Fu) << 12) |
                                (static_cast<uint32_t>(b1 & 0x3Fu) << 6) |
                                static_cast<uint32_t>(b2 & 0x3Fu);
            if (cp >= 0x800u && !(cp >= 0xD800u && cp <= 0xDFFFu)) {
                pos += 3;
                return cp;
            }
        }
    } else if ((b0 & 0xF8u) == 0xF0u && pos + 3 < sv.size()) {
        const auto b1 = static_cast<unsigned char>(sv[pos + 1]);
        const auto b2 = static_cast<unsigned char>(sv[pos + 2]);
        const auto b3 = static_cast<unsigned char>(sv[pos + 3]);
        if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u && (b3 & 0xC0u) == 0x80u) {
            const uint32_t cp = (static_cast<uint32_t>(b0 & 0x07u) << 18) |
                                (static_cast<uint32_t>(b1 & 0x3Fu) << 12) |
                                (static_cast<uint32_t>(b2 & 0x3Fu) << 6) |
                                static_cast<uint32_t>(b3 & 0x3Fu);
            if (cp >= 0x10000u && cp <= 0x10FFFFu) {
                pos += 4;
                return cp;
            }
        }
    }
    ++pos;
    return b0;
}

void appendUtf8(std::string &out, uint32_t cp)
{
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x10FFFFu) {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

uint32_t foldCodepointLower(uint32_t cp) noexcept
{
    if (cp >= 'A' && cp <= 'Z') {
        return cp + 32u;
    }
    if (cp >= 0x00C0u && cp <= 0x00DEu && cp != 0x00D7u) {
        return cp + 0x20u;
    }
    if (cp >= 0x0100u && cp <= 0x012Eu && (cp & 1u) == 0u) {
        return cp + 1u;
    }
    if (cp == 0x0130u) {
        return 'i';
    }
    if (cp >= 0x0132u && cp <= 0x0136u && (cp & 1u) == 0u) {
        return cp + 1u;
    }
    if (cp >= 0x0139u && cp <= 0x0147u && (cp & 1u) == 1u) {
        return cp + 1u;
    }
    if (cp >= 0x014Au && cp <= 0x0176u && (cp & 1u) == 0u) {
        return cp + 1u;
    }
    if (cp == 0x0178u) {
        return 0x00FFu;
    }
    if (cp >= 0x0179u && cp <= 0x017Du && (cp & 1u) == 1u) {
        return cp + 1u;
    }
    if ((cp >= 0x0391u && cp <= 0x03A1u) || (cp >= 0x03A3u && cp <= 0x03ABu)) {
        return cp + 0x20u;
    }
    if (cp >= 0x0400u && cp <= 0x040Fu) {
        return cp + 0x50u;
    }
    if (cp >= 0x0410u && cp <= 0x042Fu) {
        return cp + 0x20u;
    }
    if (cp >= 0xFF21u && cp <= 0xFF3Au) {
        return cp + 0x20u;
    }
    return cp;
}

std::string toLowerUtf8(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const auto b0 = static_cast<unsigned char>(s[i]);
        if (b0 < 0x80u) {
            out.push_back((b0 >= 'A' && b0 <= 'Z') ? static_cast<char>(b0 + 32u)
                                                   : static_cast<char>(b0));
            ++i;
            continue;
        }
        const size_t start = i;
        const uint32_t cp = decodeUtf8Next(s, i);
        const uint32_t folded = foldCodepointLower(cp);
        if (folded == cp) {
            out.append(s.data() + start, i - start);
        } else {
            appendUtf8(out, folded);
        }
    }
    return out;
}

size_t utf8CodepointCount(std::string_view sv) noexcept
{
    size_t count = 0;
    for (unsigned char c : sv) {
        if ((c & 0xC0u) != 0x80u) {
            ++count;
        }
    }
    return count;
}

std::string trimAsciiWs(std::string_view sv)
{
    size_t start = 0;
    while (start < sv.size() && static_cast<unsigned char>(sv[start]) <= ' ') {
        ++start;
    }
    size_t end = sv.size();
    while (end > start && static_cast<unsigned char>(sv[end - 1]) <= ' ') {
        --end;
    }
    return std::string(sv.substr(start, end - start));
}

uint64_t parseStrictBase10Uint64(std::string_view sv) noexcept
{
    if (sv.empty()) {
        return 0;
    }
    for (unsigned char c : sv) {
        if (c < '0' || c > '9') {
            return 0;
        }
    }
    uint64_t val = 0;
    const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val, 10);
    if (ec != std::errc{} || ptr != sv.data() + sv.size()) {
        return 0;
    }
    return val;
}

uint64_t fnv1a64(std::string_view sv) noexcept
{
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char ch : sv) {
        h ^= ch;
        h *= 1099511628211ULL;
    }
    return h != 0 ? h : 14695981039346656037ULL;
}

bool execSql(sqlite3 *db, const char *sql, const char *context)
{
    char *errMsg = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "[CatalogIndexManager] SQLite error (%s): %s (rc=%d)\n", context,
                     errMsg ? errMsg : sqlite3_errstr(rc), rc);
        if (errMsg) {
            sqlite3_free(errMsg);
        }
        return false;
    }
    return true;
}

void rollbackIfInTx(sqlite3 *db, const char *context)
{
    if (db && sqlite3_get_autocommit(db) == 0) {
        (void)execSql(db, "ROLLBACK;", context);
    }
}

std::string serializeIdsAsJson(const std::vector<uint64_t> &ids)
{
    std::string json;
    json.reserve(ids.size() * 14u + 2u);
    json.push_back('[');
    char buf[32];
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) {
            json.push_back(',');
        }
        const auto signedId = static_cast<int64_t>(ids[i]);
        const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), signedId);
        if (ec == std::errc{}) {
            json.append(buf, static_cast<size_t>(ptr - buf));
        }
    }
    json.push_back(']');
    return json;
}

uint32_t mapPublishedIdToCatalogId(uint64_t pubId)
{
    if (pubId == 0) {
        return 0;
    }
    struct Registry {
        std::mutex mu;
        std::unordered_map<uint64_t, uint32_t> pubToCat;
        std::unordered_map<uint32_t, uint64_t> catToPub;
        uint32_t nextHighId{0x80000000u};
    };
    static Registry reg;
    std::lock_guard<std::mutex> lock(reg.mu);
    if (auto it = reg.pubToCat.find(pubId); it != reg.pubToCat.end()) {
        return it->second;
    }
    uint32_t cand = 0;
    if (pubId <= 0x7FFFFFFFULL) {
        cand = static_cast<uint32_t>(pubId);
    } else {
        const uint64_t h = pubId * 0x9E3779B97F4A7C15ULL;
        cand = 0x80000000u | static_cast<uint32_t>((h ^ (h >> 32)) & 0x7FFFFFFFu);
    }
    while (cand == 0 || reg.catToPub.contains(cand)) {
        cand = reg.nextHighId++;
        if (reg.nextHighId == 0) {
            reg.nextHighId = 0x80000000u;
        }
    }
    reg.pubToCat.emplace(pubId, cand);
    reg.catToPub.emplace(cand, pubId);
    return cand;
}

/* ========================================================================= */
/* Batched Linux io_uring IORING_OP_STATX Helper (with statx() Fallback)     */
/* ========================================================================= */

class IoUringStatxBatcher {
  public:
    static constexpr unsigned kRingEntries = 64;

    IoUringStatxBatcher() = default;

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
        // Skip io_uring setup/mmap overhead for tiny directories.
        if (paths.size() > 4 && ensureRingInit()) {
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
                    teardown();
                    m_ringDisabled = true;
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
    bool ensureRingInit()
    {
        if (m_ringReady) {
            return true;
        }
        if (m_ringDisabled) {
            return false;
        }
#if WWAL_HAS_LINUX_IO_URING_HDR && defined(__NR_io_uring_setup) && defined(__NR_io_uring_enter)
        io_uring_params params{};
        long fd = ::syscall(__NR_io_uring_setup, kRingEntries, &params);
        if (fd < 0) {
            m_ringDisabled = true;
            return false;
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
            m_ringDisabled = true;
            return false;
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
                m_ringDisabled = true;
                return false;
            }
        }

        m_sqesSize = params.sq_entries * sizeof(io_uring_sqe);
        void *sqesMem = ::mmap(nullptr, m_sqesSize, PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_POPULATE, m_ringFd, IORING_OFF_SQES);
        if (sqesMem == MAP_FAILED) {
            teardown();
            m_ringDisabled = true;
            return false;
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
        return true;
#else
        m_ringDisabled = true;
        return false;
#endif
    }

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
    bool m_ringDisabled{false};
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

IoUringStatxBatcher &threadLocalStatxBatcher()
{
    thread_local IoUringStatxBatcher tlsBatcher;
    return tlsBatcher;
}

void collectRegularFilesNoStat(const std::string &dirPath, std::vector<std::string> &outFiles,
                               size_t &statCounter, int depth = 0)
{
    if (depth > 16 || outFiles.size() >= 16384) {
        return;
    }
    DIR *dir = ::opendir(dirPath.c_str());
    if (!dir) {
        return;
    }
    const int dfd = ::dirfd(dir);
    while (struct dirent *ent = ::readdir(dir)) {
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        std::string full = dirPath + "/" + ent->d_name;
        if (ent->d_type == DT_DIR) {
            collectRegularFilesNoStat(full, outFiles, statCounter, depth + 1);
        } else if (ent->d_type == DT_REG) {
            outFiles.push_back(std::move(full));
        } else if (ent->d_type == DT_UNKNOWN && dfd >= 0) {
            struct stat st{};
            ++statCounter;
            if (::fstatat(dfd, ent->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    collectRegularFilesNoStat(full, outFiles, statCounter, depth + 1);
                } else if (S_ISREG(st.st_mode)) {
                    outFiles.push_back(std::move(full));
                }
            }
        }
    }
    ::closedir(dir);
}

/* ========================================================================= */
/* 64-Bit Myers (1999) Semi-Global Bit-Parallel Edit Distance                */
/* ========================================================================= */

inline bool myersStep(uint64_t eq, uint64_t highBit, uint64_t mask, uint64_t &pv, uint64_t &mv,
                      uint32_t &score, uint32_t &best) noexcept
{
    const uint64_t xv = eq | mv;
    const uint64_t xh = ((((eq & pv) + pv) ^ pv) | eq) & mask;

    const uint64_t ph = (mv | ~(xh | pv)) & mask;
    const uint64_t mh = pv & xh;

    if (ph & highBit) {
        ++score;
    } else if (mh & highBit) {
        --score;
    }
    if (score < best) {
        best = score;
        if (best == 0) {
            return true;
        }
    }

    const uint64_t phShift = (ph << 1) & mask;
    const uint64_t mhShift = (mh << 1) & mask;
    pv = (mhShift | ~(xv | phShift)) & mask;
    mv = phShift & xv;
    return false;
}

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
        if (myersStep(peq[ch], highBit, mask, pv, mv, score, best)) {
            return 0;
        }
    }

    return best;
}

struct PatternCodepointMap {
    std::array<uint32_t, 64> cps{};
    std::array<uint64_t, 64> masks{};
    uint32_t uniqueCount{0};
    uint32_t m{0};

    void build(std::span<const uint32_t> patternCps) noexcept
    {
        m = static_cast<uint32_t>(std::min<size_t>(patternCps.size(), 64u));
        uniqueCount = 0;
        for (uint32_t i = 0; i < m; ++i) {
            const uint32_t cp = patternCps[i];
            uint32_t slot = uniqueCount;
            for (uint32_t j = 0; j < uniqueCount; ++j) {
                if (cps[j] == cp) {
                    slot = j;
                    break;
                }
            }
            if (slot == uniqueCount) {
                cps[uniqueCount] = cp;
                masks[uniqueCount] = 0ULL;
                ++uniqueCount;
            }
            masks[slot] |= (1ULL << i);
        }
    }

    [[nodiscard]] uint64_t eqFor(uint32_t cp) const noexcept
    {
        for (uint32_t j = 0; j < uniqueCount; ++j) {
            if (cps[j] == cp) {
                return masks[j];
            }
        }
        return 0ULL;
    }
};

uint32_t myersSubstringDistanceCodepoints64(std::string_view textLower,
                                            const PatternCodepointMap &pm) noexcept
{
    if (pm.m == 0) {
        return 0;
    }
    if (textLower.empty()) {
        return pm.m;
    }

    const uint64_t highBit = (pm.m == 64) ? (1ULL << 63) : (1ULL << (pm.m - 1));
    const uint64_t mask = (pm.m == 64) ? ~0ULL : ((1ULL << pm.m) - 1ULL);

    uint64_t pv = mask;
    uint64_t mv = 0ULL;
    uint32_t score = pm.m;
    uint32_t best = pm.m;

    size_t pos = 0;
    while (pos < textLower.size()) {
        const uint32_t cp = decodeUtf8Next(textLower, pos);
        if (myersStep(pm.eqFor(cp), highBit, mask, pv, mv, score, best)) {
            return 0;
        }
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
    const int dirFd = ::open(itemDir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirFd < 0) {
        return;
    }
    const int fd = wwal_openat2_beneath(dirFd, "project.json", O_RDONLY | O_CLOEXEC, 0);
    ::close(dirFd);
    if (fd < 0) {
        return;
    }

    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > 4 * 1024 * 1024) {
        ::close(fd);
        return;
    }

    std::string raw(static_cast<size_t>(st.st_size), '\0');
    size_t off = 0;
    while (off < raw.size()) {
        const ssize_t n = ::read(fd, raw.data() + off, raw.size() - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            return;
        }
        if (n == 0) {
            raw.resize(off);
            break;
        }
        off += static_cast<size_t>(n);
    }
    ::close(fd);

    if (raw.empty()) {
        return;
    }

    struct JValGuard {
        we_jval_t *root{nullptr};
        ~JValGuard()
        {
            if (root) {
                we_jval_free(root);
            }
        }
    } guard{we_json_parse_root(raw.data(), raw.size())};

    if (!guard.root || guard.root->type != JVAL_OBJ) {
        return;
    }

    auto readStrField = [](const we_jval_t *obj, const char *key, std::string *out) -> bool {
        const we_jval_t *v = we_jobj_get(obj, key);
        if (!v || v->type != JVAL_STR || !v->str) {
            return false;
        }
        *out = v->str;
        return true;
    };

    std::string field;
    if (readStrField(guard.root, "title", &field)) {
        std::string t = trimAsciiWs(field);
        if (!t.empty()) {
            ioTitle = std::move(t);
        }
    }
    if (readStrField(guard.root, "type", &field)) {
        std::string tp = toLowerUtf8(trimAsciiWs(field));
        if (!tp.empty()) {
            ioType = std::move(tp);
        }
    }
    if (readStrField(guard.root, "preview", &field)) {
        const std::string pv = trimAsciiWs(field);
        ioPreview = resolveConfinedPreviewPath(itemDir, pv);
    }
    if (readStrField(guard.root, "description", &field)) {
        outDescription = std::move(field);
    }

    if (const we_jval_t *tv = we_jobj_get(guard.root, "tags")) {
        if (tv->type == JVAL_ARR && tv->arr.items) {
            std::string joined;
            for (size_t i = 0; i < tv->arr.count; ++i) {
                const we_jval_t *elem = tv->arr.items[i];
                if (elem && elem->type == JVAL_STR && elem->str) {
                    const std::string tag = trimAsciiWs(elem->str);
                    if (!tag.empty()) {
                        if (!joined.empty()) {
                            joined += ", ";
                        }
                        joined += tag;
                    }
                }
            }
            outTags = std::move(joined);
        } else if (tv->type == JVAL_STR && tv->str) {
            outTags = trimAsciiWs(tv->str);
        }
    }
}

bool bindRecordAtOffset(sqlite3_stmt *stmt, int baseCol, const WorkshopItemRecord &rec)
{
    const double safeRating =
        std::isfinite(static_cast<double>(rec.rating)) ? static_cast<double>(rec.rating) : 0.0;
    const double safeAspect = std::isfinite(static_cast<double>(rec.aspect_ratio))
                                  ? static_cast<double>(rec.aspect_ratio)
                                  : (16.0 / 9.0);

    bool ok = true;
    ok = ok && sqlite3_bind_int64(stmt, baseCol + 1,
                                  static_cast<sqlite3_int64>(rec.published_file_id)) == SQLITE_OK;
    ok = ok && sqlite3_bind_int64(stmt, baseCol + 2,
                                  static_cast<sqlite3_int64>(rec.owner_steam_id)) == SQLITE_OK;
    ok = ok && sqlite3_bind_text(stmt, baseCol + 3, rec.title.data(),
                                 static_cast<int>(rec.title.size()), SQLITE_STATIC) == SQLITE_OK;
    ok = ok && sqlite3_bind_text(stmt, baseCol + 4, rec.type.data(),
                                 static_cast<int>(rec.type.size()), SQLITE_STATIC) == SQLITE_OK;
    ok =
        ok && sqlite3_bind_text(stmt, baseCol + 5, rec.file_path.data(),
                                static_cast<int>(rec.file_path.size()), SQLITE_STATIC) == SQLITE_OK;
    ok = ok &&
         sqlite3_bind_text(stmt, baseCol + 6, rec.preview_path.data(),
                           static_cast<int>(rec.preview_path.size()), SQLITE_STATIC) == SQLITE_OK;
    ok = ok && sqlite3_bind_int64(stmt, baseCol + 7,
                                  static_cast<sqlite3_int64>(rec.file_size_bytes)) == SQLITE_OK;
    ok = ok && sqlite3_bind_int64(stmt, baseCol + 8,
                                  static_cast<sqlite3_int64>(rec.time_created)) == SQLITE_OK;
    ok = ok && sqlite3_bind_int64(stmt, baseCol + 9,
                                  static_cast<sqlite3_int64>(rec.time_updated)) == SQLITE_OK;
    ok = ok && sqlite3_bind_int64(stmt, baseCol + 10,
                                  static_cast<sqlite3_int64>(rec.subscriptions)) == SQLITE_OK;
    ok = ok && sqlite3_bind_int64(stmt, baseCol + 11, static_cast<sqlite3_int64>(rec.favorited)) ==
                   SQLITE_OK;
    ok = ok &&
         sqlite3_bind_int64(stmt, baseCol + 12, static_cast<sqlite3_int64>(rec.views)) == SQLITE_OK;
    ok = ok && sqlite3_bind_double(stmt, baseCol + 13, safeRating) == SQLITE_OK;
    ok = ok && sqlite3_bind_int64(stmt, baseCol + 14,
                                  static_cast<sqlite3_int64>(rec.install_state)) == SQLITE_OK;
    ok = ok && sqlite3_bind_int64(stmt, baseCol + 15,
                                  static_cast<sqlite3_int64>(rec.storage_backend)) == SQLITE_OK;
    ok = ok && sqlite3_bind_int64(stmt, baseCol + 16,
                                  static_cast<sqlite3_int64>(rec.compat_tier)) == SQLITE_OK;
    ok = ok && sqlite3_bind_int64(stmt, baseCol + 17,
                                  static_cast<sqlite3_int64>(rec.compat_score)) == SQLITE_OK;
    ok = ok && sqlite3_bind_double(stmt, baseCol + 18, safeAspect) == SQLITE_OK;
    ok = ok && sqlite3_bind_int64(stmt, baseCol + 19,
                                  static_cast<sqlite3_int64>(rec.dominant_color)) == SQLITE_OK;
    ok = ok && sqlite3_bind_int64(stmt, baseCol + 20, static_cast<sqlite3_int64>(rec.tag_mask)) ==
                   SQLITE_OK;
    ok = ok &&
         sqlite3_bind_text(stmt, baseCol + 21, rec.tags_string.data(),
                           static_cast<int>(rec.tags_string.size()), SQLITE_STATIC) == SQLITE_OK;
    ok = ok &&
         sqlite3_bind_text(stmt, baseCol + 22, rec.description.data(),
                           static_cast<int>(rec.description.size()), SQLITE_STATIC) == SQLITE_OK;
    ok = ok && sqlite3_bind_int64(stmt, baseCol + 23,
                                  static_cast<sqlite3_int64>(rec.manifest_id)) == SQLITE_OK;
    ok = ok && sqlite3_bind_int64(stmt, baseCol + 24,
                                  static_cast<sqlite3_int64>(
                                      rec.file_clock > 0 ? rec.file_clock : 1ULL)) == SQLITE_OK;
    ok = ok && sqlite3_bind_int(stmt, baseCol + 25, rec.signature_verified ? 1 : 0) == SQLITE_OK;
    return ok;
}

bool bindRecordToStmt(sqlite3_stmt *stmt, const WorkshopItemRecord &rec)
{
    return bindRecordAtOffset(stmt, 0, rec);
}

inline bool isAsciiTokenChar(unsigned char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || c >= 0x80u;
}

uint64_t prefixTokenBloomMask(std::string_view lowerToken) noexcept
{
    if (lowerToken.empty()) {
        return 0ULL;
    }
    uint64_t mask = 0ULL;
    const auto b0 = static_cast<unsigned char>(lowerToken[0]);
    mask |= 1ULL << ((static_cast<uint64_t>(b0) * 11400714819323198485ULL) >> 58);
    if (lowerToken.size() >= 2u) {
        const uint32_t p2 =
            (static_cast<uint32_t>(b0) << 8) | static_cast<unsigned char>(lowerToken[1]);
        mask |= 1ULL << ((static_cast<uint64_t>(p2) * 0x9E3779B97F4A7C15ULL) >> 58);
        if (lowerToken.size() >= 3u) {
            const uint32_t p3 = (p2 << 8) | static_cast<unsigned char>(lowerToken[2]);
            mask |= 1ULL << ((static_cast<uint64_t>(p3) * 0xBF58476D1CE4E5B9ULL) >> 58);
            if (lowerToken.size() >= 4u) {
                const uint32_t p4 = (p3 << 8) | static_cast<unsigned char>(lowerToken[3]);
                mask |= 1ULL << ((static_cast<uint64_t>(p4) * 0x94D049BB133111EBULL) >> 58);
            }
        }
    }
    return mask;
}

uint64_t computeDocBloom64(std::string_view lowerTitle, std::string_view lowerTags,
                           std::string_view lowerDesc) noexcept
{
    uint64_t bloom = 0ULL;
    auto scanField = [&bloom](std::string_view sv) noexcept {
        size_t i = 0;
        while (i < sv.size()) {
            while (i < sv.size() && !isAsciiTokenChar(static_cast<unsigned char>(sv[i]))) {
                ++i;
            }
            if (i >= sv.size()) {
                break;
            }
            const size_t start = i;
            while (i < sv.size() && isAsciiTokenChar(static_cast<unsigned char>(sv[i]))) {
                ++i;
            }
            bloom |= prefixTokenBloomMask(std::string_view(sv.data() + start, i - start));
        }
    };
    scanField(lowerTitle);
    scanField(lowerTags);
    scanField(lowerDesc);
    return bloom;
}

uint32_t countWordMatches(std::string_view haystack, std::string_view tok, bool exactWord) noexcept
{
    if (tok.empty() || haystack.size() < tok.size()) {
        return 0U;
    }
    uint32_t count = 0U;
    const char *const base = haystack.data();
    const char *const end = base + haystack.size();
    const char *cursor = base;
    const char first = tok[0];
    while (static_cast<size_t>(end - cursor) >= tok.size()) {
        const size_t searchLen = static_cast<size_t>(end - cursor) - tok.size() + 1u;
        const auto *hit = static_cast<const char *>(std::memchr(cursor, first, searchLen));
        if (!hit) {
            break;
        }
        const bool leftOk = (hit == base) || !isAsciiTokenChar(static_cast<unsigned char>(hit[-1]));
        if (leftOk && std::memcmp(hit, tok.data(), tok.size()) == 0) {
            const char *after = hit + tok.size();
            const bool rightOk = !exactWord || (after == end) ||
                                 !isAsciiTokenChar(static_cast<unsigned char>(*after));
            if (rightOk) {
                ++count;
                cursor = after;
                continue;
            }
        }
        cursor = hit + 1;
    }
    return count;
}

inline double bm25TfNorm(uint32_t tf, size_t len, double avgLen) noexcept
{
    if (tf == 0U) {
        return 0.0;
    }
    const double dtf = static_cast<double>(tf);
    return (dtf * 2.2) / (dtf + 1.2 * (0.25 + 0.75 * (static_cast<double>(len) / avgLen)));
}

std::shared_ptr<const SegmentTokenIndex> buildSegmentTokenIndex(const ChunkedCatalogSegment &seg)
{
    auto out = std::make_shared<SegmentTokenIndex>();
    if (seg.count == 0U) {
        return out;
    }

    struct TokOcc {
        const char *ptr;
        uint32_t len;
        uint16_t localIdx;
        uint8_t field;
    };
    std::vector<TokOcc> occs;
    occs.reserve(static_cast<size_t>(seg.count) * 14U);

    auto scanSv = [&](std::string_view sv, uint16_t localIdx, uint8_t field) {
        const size_t n = sv.size();
        size_t i = 0;
        while (i < n) {
            while (i < n && !isAsciiTokenChar(static_cast<unsigned char>(sv[i]))) {
                ++i;
            }
            if (i >= n) {
                break;
            }
            const size_t start = i;
            while (i < n && isAsciiTokenChar(static_cast<unsigned char>(sv[i]))) {
                ++i;
            }
            occs.push_back(
                TokOcc{sv.data() + start, static_cast<uint32_t>(i - start), localIdx, field});
        }
    };

    for (uint32_t i = 0U; i < seg.count; ++i) {
        const auto lIdx = static_cast<uint16_t>(i);
        scanSv(seg.lowerTitleAt(i), lIdx, 0U);
        scanSv(seg.lowerTagsAt(i), lIdx, 1U);
        scanSv(seg.lowerDescAt(i), lIdx, 2U);
    }

    if (occs.empty()) {
        return out;
    }

    std::sort(occs.begin(), occs.end(), [](const TokOcc &a, const TokOcc &b) noexcept {
        const std::string_view sa(a.ptr, a.len);
        const std::string_view sb(b.ptr, b.len);
        if (sa != sb) {
            return sa < sb;
        }
        return a.localIdx < b.localIdx;
    });

    out->postings.reserve(occs.size());
    size_t pos = 0U;
    while (pos < occs.size()) {
        const std::string_view tok(occs[pos].ptr, occs[pos].len);
        const uint32_t tokOff = static_cast<uint32_t>(out->tokenArena.size());
        out->tokenArena.append(tok.data(), tok.size());
        const uint32_t postBegin = static_cast<uint32_t>(out->postings.size());

        while (pos < occs.size() && std::string_view(occs[pos].ptr, occs[pos].len) == tok) {
            const uint16_t docIdx = occs[pos].localIdx;
            uint32_t tfT = 0U;
            uint32_t tfG = 0U;
            uint32_t tfD = 0U;
            while (pos < occs.size() && occs[pos].localIdx == docIdx &&
                   std::string_view(occs[pos].ptr, occs[pos].len) == tok) {
                if (occs[pos].field == 0U) {
                    ++tfT;
                } else if (occs[pos].field == 1U) {
                    ++tfG;
                } else {
                    ++tfD;
                }
                ++pos;
            }
            const double score = 10.0 * bm25TfNorm(tfT, seg.title_lengths[docIdx], 24.0) +
                                 5.0 * bm25TfNorm(tfG, seg.tags_lengths[docIdx], 16.0) +
                                 1.0 * bm25TfNorm(tfD, seg.desc_lengths[docIdx], 64.0);
            out->postings.push_back(SegmentPosting{
                score,
                docIdx,
                static_cast<uint16_t>(std::min<uint32_t>(tfT, 65535U)),
                static_cast<uint16_t>(std::min<uint32_t>(tfG, 65535U)),
                static_cast<uint16_t>(std::min<uint32_t>(tfD, 65535U)),
            });
        }

        const uint32_t postEnd = static_cast<uint32_t>(out->postings.size());
        std::sort(out->postings.begin() + postBegin, out->postings.begin() + postEnd,
                  [&seg](const SegmentPosting &a, const SegmentPosting &b) noexcept {
                      if (a.singleScore != b.singleScore) {
                          return a.singleScore > b.singleScore;
                      }
                      return seg.published_ids[a.localIdx] < seg.published_ids[b.localIdx];
                  });
        out->dict.push_back(SegmentTokenDictEntry{tokOff, static_cast<uint32_t>(tok.size()),
                                                  postBegin, postEnd - postBegin});
    }
    return out;
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

CatalogItem WorkshopItemRecord::toCatalogItem() const
{
    CatalogItem item;
    item.id = mapPublishedIdToCatalogId(published_file_id);
    item.workshop_id = std::to_string(published_file_id);
    item.title = title;
    item.path = file_path;
    item.preview_path = preview_path;
    item.file_size = file_size_bytes;
    item.date_modified = static_cast<int64_t>(time_updated);
    item.aspect_ratio = aspect_ratio;
    item.dominant_color = dominant_color;
    item.tag_mask = tag_mask;

    const std::string lowerType = toLowerUtf8(type);
    if (lowerType == "video") {
        item.badge = WallpaperBadge::Video;
    } else if (lowerType == "scene") {
        item.badge = WallpaperBadge::Scene;
    } else if (lowerType == "web") {
        item.badge = WallpaperBadge::Image;
    } else if (lowerType.find("shader") != std::string::npos) {
        item.badge = WallpaperBadge::Shader;
    } else {
        item.badge = WallpaperBadge::Image;
    }
    return item;
}

/* ========================================================================= */
/* In-Memory SoA Tag Bitmap & Lowercase Title Arena                          */
/* ========================================================================= */

struct CatalogIndexManager::Impl {
    static constexpr size_t kBatchInsertRows = 32U;

    mutable std::mutex dbMutex;
    mutable std::mutex stmtCacheMutex;
    // Declared before readPool so cached sqlite3_stmt handles outlive
    // ReadOnlySqlitePool::~ReadOnlySqlitePool().
    mutable std::unordered_map<sqlite3 *, std::unordered_map<uint32_t, sqlite3_stmt *>>
        filterStmtCache;
    mutable ReadOnlySqlitePool readPool;
    ChunkedCatalogSnapshot rcuPool;
    bool hasTrigramFts{false};
    bool hasJsonEach{false};
    std::atomic<size_t> lastReconcileStatCalls{0};
    sqlite3_stmt *stmtUpsertBatch32{nullptr};
    sqlite3_stmt *stmtBeginImmediate{nullptr};
    sqlite3_stmt *stmtCommit{nullptr};
    sqlite3_stmt *stmtRollback{nullptr};

    // Writer-side chunk table; published ChunkedCatalogSegments remain immutable for wait-free
    // readers.
    ChunkedCatalogSnapshot::SegmentList committedSegments;
    std::vector<std::shared_ptr<ChunkedCatalogSegment>> scratchWorking;
    std::vector<size_t> scratchDirty;
    std::unordered_map<uint64_t, size_t> idToGlobalSlot;
    size_t totalCount{0};

    [[nodiscard]] sqlite3_stmt *lookupCachedFilterStmt(sqlite3 *dbHandle, uint32_t shapeKey) const
    {
        std::lock_guard<std::mutex> lock(stmtCacheMutex);
        if (auto dbIt = filterStmtCache.find(dbHandle); dbIt != filterStmtCache.end()) {
            if (auto sIt = dbIt->second.find(shapeKey); sIt != dbIt->second.end()) {
                return sIt->second;
            }
        }
        return nullptr;
    }

    [[nodiscard]] sqlite3_stmt *cacheAndGetFilterStmt(sqlite3 *dbHandle, uint32_t shapeKey,
                                                      std::string_view sql) const
    {
        std::lock_guard<std::mutex> lock(stmtCacheMutex);
        auto &perDb = filterStmtCache[dbHandle];
        if (auto it = perDb.find(shapeKey); it != perDb.end()) {
            return it->second;
        }
        sqlite3_stmt *stmt = nullptr;
#ifdef SQLITE_PREPARE_PERSISTENT
        const int rc = sqlite3_prepare_v3(dbHandle, sql.data(), static_cast<int>(sql.size()),
                                          SQLITE_PREPARE_PERSISTENT, &stmt, nullptr);
#else
        const int rc =
            sqlite3_prepare_v2(dbHandle, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr);
#endif
        if (rc != SQLITE_OK || !stmt) {
            std::fprintf(stderr,
                         "[CatalogIndexManager] Failed to prepare filter stmt (rc=%d): %s\n", rc,
                         dbHandle ? sqlite3_errmsg(dbHandle) : "null db");
            if (stmt) {
                sqlite3_finalize(stmt);
            }
            return nullptr;
        }
        perDb.emplace(shapeKey, stmt);
        return stmt;
    }

    void finalizeCachedFilterStatements()
    {
        std::lock_guard<std::mutex> lock(stmtCacheMutex);
        for (auto &[dbHandle, perDb] : filterStmtCache) {
            (void)dbHandle;
            for (auto &[key, stmt] : perDb) {
                (void)key;
                if (stmt) {
                    sqlite3_finalize(stmt);
                }
            }
        }
        filterStmtCache.clear();
    }

    void finalizeBatchStmt() noexcept
    {
        auto fin = [](sqlite3_stmt *&s) noexcept {
            if (s) {
                sqlite3_finalize(s);
                s = nullptr;
            }
        };
        fin(stmtUpsertBatch32);
        fin(stmtBeginImmediate);
        fin(stmtCommit);
        fin(stmtRollback);
    }

    static bool stepTxStmt(sqlite3 *db, sqlite3_stmt *stmt, const char *ctx)
    {
        if (!stmt) {
            return false;
        }
        const int rc = sqlite3_step(stmt);
        sqlite3_reset(stmt);
        if (rc != SQLITE_DONE) {
            std::fprintf(stderr, "[CatalogIndexManager] %s failed (rc=%d): %s\n", ctx, rc,
                         db ? sqlite3_errmsg(db) : "null db");
            return false;
        }
        return true;
    }

    static void overwriteSlotTagBits(ChunkedCatalogSegment &seg, size_t localIdx, uint64_t oldMask,
                                     uint64_t newMask) noexcept
    {
        const size_t wordIdx = localIdx >> 6;
        const uint64_t bit = 1ULL << (localIdx & 63u);
        uint64_t cleared = oldMask & ~newMask;
        while (cleared != 0ULL) {
            const unsigned b = static_cast<unsigned>(std::countr_zero(cleared));
            seg.tag_bitsets[b][wordIdx] &= ~bit;
            cleared &= (cleared - 1ULL);
        }
        uint64_t added = newMask & ~oldMask;
        while (added != 0ULL) {
            const unsigned b = static_cast<unsigned>(std::countr_zero(added));
            seg.tag_bitsets[b][wordIdx] |= bit;
            added &= (added - 1ULL);
        }
    }

    static void consolidateArena(uint32_t count, std::shared_ptr<const std::string> &base,
                                 std::string &delta,
                                 std::array<uint32_t, ChunkedCatalogSegment::kChunkLen> &offs,
                                 std::array<uint32_t, ChunkedCatalogSegment::kChunkLen> &lens,
                                 uint32_t &deadBytes)
    {
        size_t liveBytes = 0U;
        for (uint32_t i = 0U; i < count; ++i) {
            liveBytes += lens[i];
        }
        auto rebuilt = std::make_shared<std::string>();
        rebuilt->reserve(liveBytes);
        for (uint32_t i = 0U; i < count; ++i) {
            const std::string_view sv =
                ChunkedCatalogSegment::readArenaSlot(base, delta, offs[i], lens[i]);
            const uint32_t newOff = static_cast<uint32_t>(rebuilt->size());
            if (!sv.empty()) {
                rebuilt->append(sv.data(), sv.size());
            }
            offs[i] = newOff;
        }
        base = std::move(rebuilt);
        std::string{}.swap(delta);
        deadBytes = 0U;
    }

    static void writeArenaSlot(std::shared_ptr<const std::string> &base, std::string &delta,
                               std::array<uint32_t, ChunkedCatalogSegment::kChunkLen> &offs,
                               std::array<uint32_t, ChunkedCatalogSegment::kChunkLen> &lens,
                               uint32_t &deadBytes, size_t localIdx, std::string_view value,
                               bool isOverwrite)
    {
        const uint32_t newLen = static_cast<uint32_t>(
            std::min<size_t>(value.size(), std::numeric_limits<uint32_t>::max()));
        if (isOverwrite) {
            const uint32_t oldOff = offs[localIdx];
            const uint32_t oldLen = lens[localIdx];
            if (newLen == oldLen &&
                ChunkedCatalogSegment::readArenaSlot(base, delta, oldOff, oldLen) == value) {
                return;
            }
            if (((oldOff & ChunkedCatalogSegment::kDeltaOffsetBit) != 0U || !base) &&
                newLen <= oldLen) {
                if (newLen > 0U) {
                    std::memcpy(delta.data() + (oldOff & ~ChunkedCatalogSegment::kDeltaOffsetBit),
                                value.data(), newLen);
                }
                lens[localIdx] = newLen;
                deadBytes += (oldLen - newLen);
                return;
            }
            deadBytes += oldLen;
        }
        const uint32_t deltaOff = static_cast<uint32_t>(delta.size());
        if (newLen > 0U) {
            delta.append(value.data(), newLen);
        }
        offs[localIdx] = ChunkedCatalogSegment::kDeltaOffsetBit | deltaOff;
        lens[localIdx] = newLen;
    }

    void beginChunkEdit()
    {
        scratchWorking.assign(committedSegments.size(), nullptr);
        scratchDirty.clear();
    }

    ChunkedCatalogSegment &mutableSegment(size_t segIdx)
    {
        if (segIdx >= scratchWorking.size()) {
            scratchWorking.resize(segIdx + 1U);
        }
        if (!scratchWorking[segIdx]) {
            if (segIdx < committedSegments.size() && committedSegments[segIdx]) {
                scratchWorking[segIdx] =
                    std::make_shared<ChunkedCatalogSegment>(*committedSegments[segIdx]);
            } else {
                scratchWorking[segIdx] = std::make_shared<ChunkedCatalogSegment>();
            }
            scratchDirty.push_back(segIdx);
        }
        return *scratchWorking[segIdx];
    }

    void commitChunkEdit()
    {
        if (scratchDirty.empty()) {
            scratchWorking.clear();
            return;
        }
        const size_t neededSegs =
            (totalCount + ChunkedCatalogSegment::kChunkLen - 1U) / ChunkedCatalogSegment::kChunkLen;
        const size_t prevSize = committedSegments.size();
        if (neededSegs > committedSegments.size()) {
            committedSegments.resize(neededSegs);
        }

        std::vector<ChunkedCatalogSegment *> toRebuild;
        toRebuild.reserve(scratchDirty.size());
        for (size_t segIdx : scratchDirty) {
            if (segIdx < committedSegments.size() && segIdx < scratchWorking.size() &&
                scratchWorking[segIdx]) {
                auto *segPtr = scratchWorking[segIdx].get();
                if (segPtr->count > 0U &&
                    (!segPtr->token_index || !segPtr->base_title_arena ||
                     segPtr->dirty_count >= 256U || segPtr->lower_title_arena.size() > 65536U ||
                     segPtr->lower_tags_arena.size() > 65536U ||
                     segPtr->lower_desc_arena.size() > 65536U)) {
                    toRebuild.push_back(segPtr);
                }
            }
        }

        auto finalizeSeg = [](ChunkedCatalogSegment *segPtr) {
            if (!segPtr->base_title_arena || segPtr->dirty_count >= 256U ||
                segPtr->lower_title_arena.size() > 65536U) {
                consolidateArena(segPtr->count, segPtr->base_title_arena, segPtr->lower_title_arena,
                                 segPtr->title_offsets, segPtr->title_lengths,
                                 segPtr->dead_title_bytes);
            }
            if (!segPtr->base_tags_arena || segPtr->dirty_count >= 256U ||
                segPtr->lower_tags_arena.size() > 65536U) {
                consolidateArena(segPtr->count, segPtr->base_tags_arena, segPtr->lower_tags_arena,
                                 segPtr->tags_offsets, segPtr->tags_lengths,
                                 segPtr->dead_tags_bytes);
            }
            if (!segPtr->base_desc_arena || segPtr->dirty_count >= 256U ||
                segPtr->lower_desc_arena.size() > 65536U) {
                consolidateArena(segPtr->count, segPtr->base_desc_arena, segPtr->lower_desc_arena,
                                 segPtr->desc_offsets, segPtr->desc_lengths,
                                 segPtr->dead_desc_bytes);
            }
            segPtr->token_index = buildSegmentTokenIndex(*segPtr);
            segPtr->dirty_count = 0U;
            segPtr->dirty_bitset.fill(0ULL);
        };

        if (toRebuild.size() >= 4U) {
            const size_t workers =
                std::min<size_t>(std::max(1U, std::thread::hardware_concurrency()),
                                 std::min<size_t>(toRebuild.size(), 4U));
            std::vector<std::thread> pool;
            pool.reserve(workers - 1U);
            const size_t chunk = (toRebuild.size() + workers - 1U) / workers;
            for (size_t t = 1U; t < workers; ++t) {
                const size_t s = t * chunk;
                const size_t e = std::min(toRebuild.size(), s + chunk);
                if (s < e) {
                    pool.emplace_back([&, s, e]() {
                        for (size_t k = s; k < e; ++k) {
                            finalizeSeg(toRebuild[k]);
                        }
                    });
                }
            }
            for (size_t k = 0U; k < std::min(toRebuild.size(), chunk); ++k) {
                finalizeSeg(toRebuild[k]);
            }
            for (auto &th : pool) {
                th.join();
            }
        } else {
            for (ChunkedCatalogSegment *segPtr : toRebuild) {
                finalizeSeg(segPtr);
            }
        }

        for (size_t segIdx : scratchDirty) {
            if (segIdx < committedSegments.size() && segIdx < scratchWorking.size()) {
                committedSegments[segIdx] = std::move(scratchWorking[segIdx]);
            }
        }
        committedSegments.resize(neededSegs);

        if (scratchDirty.size() == 1U && neededSegs == prevSize && scratchDirty[0] < neededSegs) {
            const size_t onlyIdx = scratchDirty[0];
            rcuPool.replace_segment(onlyIdx, committedSegments[onlyIdx]);
        } else {
            rcuPool.publish_segments(committedSegments);
        }
        scratchWorking.clear();
        scratchDirty.clear();
    }

    void clearSoaLocked()
    {
        committedSegments.clear();
        scratchWorking.clear();
        scratchDirty.clear();
        idToGlobalSlot.clear();
        totalCount = 0;
        rcuPool.publish_segments({});
    }

    void upsertSoaLocked(uint64_t pubId, uint64_t tagMask, std::string_view title,
                         std::string_view tagsStr, std::string_view desc)
    {
        const std::string lowerTitle = toLowerUtf8(title);
        const std::string lowerTags = toLowerUtf8(tagsStr);
        const std::string lowerDesc = toLowerUtf8(desc);
        const uint64_t bloom = computeDocBloom64(lowerTitle, lowerTags, lowerDesc);

        auto it = idToGlobalSlot.find(pubId);
        if (it != idToGlobalSlot.end()) {
            const size_t globalSlot = it->second;
            const size_t segIdx = globalSlot / ChunkedCatalogSegment::kChunkLen;
            const size_t localIdx = globalSlot % ChunkedCatalogSegment::kChunkLen;
            ChunkedCatalogSegment &seg = mutableSegment(segIdx);

            overwriteSlotTagBits(seg, localIdx, seg.tag_masks[localIdx], tagMask);
            seg.tag_masks[localIdx] = tagMask;
            seg.doc_bloom[localIdx] = bloom;

            writeArenaSlot(seg.base_title_arena, seg.lower_title_arena, seg.title_offsets,
                           seg.title_lengths, seg.dead_title_bytes, localIdx, lowerTitle, true);
            writeArenaSlot(seg.base_tags_arena, seg.lower_tags_arena, seg.tags_offsets,
                           seg.tags_lengths, seg.dead_tags_bytes, localIdx, lowerTags, true);
            writeArenaSlot(seg.base_desc_arena, seg.lower_desc_arena, seg.desc_offsets,
                           seg.desc_lengths, seg.dead_desc_bytes, localIdx, lowerDesc, true);
            seg.markSlotDirty(localIdx);
            return;
        }

        const size_t globalSlot = totalCount++;
        const size_t segIdx = globalSlot / ChunkedCatalogSegment::kChunkLen;
        const size_t localIdx = globalSlot % ChunkedCatalogSegment::kChunkLen;
        ChunkedCatalogSegment &seg = mutableSegment(segIdx);
        seg.count = static_cast<uint32_t>(localIdx + 1U);
        seg.published_ids[localIdx] = pubId;
        seg.tag_masks[localIdx] = tagMask;
        seg.doc_bloom[localIdx] = bloom;
        overwriteSlotTagBits(seg, localIdx, 0ULL, tagMask);

        writeArenaSlot(seg.base_title_arena, seg.lower_title_arena, seg.title_offsets,
                       seg.title_lengths, seg.dead_title_bytes, localIdx, lowerTitle, false);
        writeArenaSlot(seg.base_tags_arena, seg.lower_tags_arena, seg.tags_offsets,
                       seg.tags_lengths, seg.dead_tags_bytes, localIdx, lowerTags, false);
        writeArenaSlot(seg.base_desc_arena, seg.lower_desc_arena, seg.desc_offsets,
                       seg.desc_lengths, seg.dead_desc_bytes, localIdx, lowerDesc, false);
        seg.markSlotDirty(localIdx);
        idToGlobalSlot.emplace(pubId, globalSlot);
    }

    void removeSoaLocked(uint64_t pubId)
    {
        auto it = idToGlobalSlot.find(pubId);
        if (it == idToGlobalSlot.end() || totalCount == 0U) {
            return;
        }
        const size_t globalSlot = it->second;
        const size_t lastGlobal = totalCount - 1U;
        const size_t lastSegIdx = lastGlobal / ChunkedCatalogSegment::kChunkLen;
        const size_t lastLocalIdx = lastGlobal % ChunkedCatalogSegment::kChunkLen;

        if (globalSlot != lastGlobal) {
            const size_t segIdx = globalSlot / ChunkedCatalogSegment::kChunkLen;
            const size_t localIdx = globalSlot % ChunkedCatalogSegment::kChunkLen;

            // Read moved slot strings from either dirty copy or immutable committed segment before
            // mutating.
            const ChunkedCatalogSegment *srcLast =
                (lastSegIdx < scratchWorking.size() && scratchWorking[lastSegIdx])
                    ? scratchWorking[lastSegIdx].get()
                    : committedSegments[lastSegIdx].get();
            const uint64_t movedId = srcLast->published_ids[lastLocalIdx];
            const uint64_t movedMask = srcLast->tag_masks[lastLocalIdx];
            const uint64_t movedBloom = srcLast->doc_bloom[lastLocalIdx];
            const std::string movedTitle(srcLast->lowerTitleAt(lastLocalIdx));
            const std::string movedTags(srcLast->lowerTagsAt(lastLocalIdx));
            const std::string movedDesc(srcLast->lowerDescAt(lastLocalIdx));

            ChunkedCatalogSegment &dstSeg = mutableSegment(segIdx);
            overwriteSlotTagBits(dstSeg, localIdx, dstSeg.tag_masks[localIdx], movedMask);
            dstSeg.published_ids[localIdx] = movedId;
            dstSeg.tag_masks[localIdx] = movedMask;
            dstSeg.doc_bloom[localIdx] = movedBloom;
            writeArenaSlot(dstSeg.base_title_arena, dstSeg.lower_title_arena, dstSeg.title_offsets,
                           dstSeg.title_lengths, dstSeg.dead_title_bytes, localIdx, movedTitle,
                           true);
            writeArenaSlot(dstSeg.base_tags_arena, dstSeg.lower_tags_arena, dstSeg.tags_offsets,
                           dstSeg.tags_lengths, dstSeg.dead_tags_bytes, localIdx, movedTags, true);
            writeArenaSlot(dstSeg.base_desc_arena, dstSeg.lower_desc_arena, dstSeg.desc_offsets,
                           dstSeg.desc_lengths, dstSeg.dead_desc_bytes, localIdx, movedDesc, true);
            dstSeg.markSlotDirty(localIdx);
            idToGlobalSlot[movedId] = globalSlot;
        }

        ChunkedCatalogSegment &lastSeg = mutableSegment(lastSegIdx);
        overwriteSlotTagBits(lastSeg, lastLocalIdx, lastSeg.tag_masks[lastLocalIdx], 0ULL);
        lastSeg.dead_title_bytes += lastSeg.title_lengths[lastLocalIdx];
        lastSeg.dead_tags_bytes += lastSeg.tags_lengths[lastLocalIdx];
        lastSeg.dead_desc_bytes += lastSeg.desc_lengths[lastLocalIdx];
        lastSeg.markSlotDirty(lastLocalIdx);
        lastSeg.count = static_cast<uint32_t>(lastLocalIdx);
        --totalCount;
        idToGlobalSlot.erase(it);
    }

    bool loadInitialSoaFromDbLocked(sqlite3 *db)
    {
        clearSoaLocked();
        if (!db) {
            return false;
        }
        sqlite3_stmt *stmt = nullptr;
        const char *sql = "SELECT published_file_id, tag_mask, title, tags_string, description "
                          "FROM workshop_items;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            std::fprintf(stderr, "[CatalogIndexManager] Failed to prepare SoA loader: %s\n",
                         sqlite3_errmsg(db));
            return false;
        }
        beginChunkEdit();
        int stepRc = SQLITE_OK;
        while ((stepRc = sqlite3_step(stmt)) == SQLITE_ROW) {
            const uint64_t id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
            const uint64_t mask = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
            const char *t = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
            const char *g = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
            const char *d = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
            upsertSoaLocked(id, mask, t ? t : "", g ? g : "", d ? d : "");
        }
        sqlite3_finalize(stmt);
        if (stepRc != SQLITE_DONE) {
            std::fprintf(stderr, "[CatalogIndexManager] SoA loader step error: %s (rc=%d)\n",
                         sqlite3_errmsg(db), stepRc);
            clearSoaLocked();
            return false;
        }
        commitChunkEdit();
        return true;
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

    const int rc = sqlite3_open_v2(
        m_dbPath.string().c_str(), &m_db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "[CatalogIndexManager] Failed to open SQLite database %s: %s\n",
                     m_dbPath.string().c_str(), m_db ? sqlite3_errmsg(m_db) : sqlite3_errstr(rc));
        if (m_db) {
            sqlite3_close(m_db);
            m_db = nullptr;
        }
        return;
    }

    sqlite3_busy_timeout(m_db, 200);
    if (!initSchema() || !prepareStatements() || !m_impl->loadInitialSoaFromDbLocked(m_db)) {
        std::fprintf(
            stderr, "[CatalogIndexManager] Initialization failed for %s; closing database handle\n",
            m_dbPath.string().c_str());
        finalizeStatements();
        if (m_db) {
            sqlite3_close(m_db);
            m_db = nullptr;
        }
        return;
    }
    m_impl->readPool.setPreCloseCallback(
        [impl = m_impl.get()]() { impl->finalizeCachedFilterStatements(); });
    (void)m_impl->readPool.open(m_dbPath, m_impl->hasTrigramFts);
}

CatalogIndexManager::~CatalogIndexManager()
{
    if (m_impl) {
        // Drain readPool before closing cached filter statements and database handles (AUD-027).
        m_impl->readPool.close();
        m_impl->readPool.setPreCloseCallback({});
        m_impl->finalizeCachedFilterStatements();
        std::lock_guard<std::mutex> lock(m_impl->dbMutex);
        finalizeStatements();
        if (m_db) {
            sqlite3_close_v2(m_db);
            m_db = nullptr;
        }
    }
}

bool CatalogIndexManager::isOpen() const noexcept { return m_db != nullptr; }

size_t CatalogIndexManager::lastReconcileStatCount() const noexcept
{
    return m_impl->lastReconcileStatCalls.load(std::memory_order_relaxed);
}

bool CatalogIndexManager::initSchema()
{
    if (!m_db) {
        return false;
    }

    if (!execSql(m_db, "PRAGMA journal_mode = WAL;", "initSchema.wal") ||
        !execSql(m_db, "PRAGMA synchronous = NORMAL;", "initSchema.sync") ||
        !execSql(m_db, "PRAGMA temp_store = MEMORY;", "initSchema.tempStore") ||
        !execSql(m_db, "PRAGMA foreign_keys = ON;", "initSchema.foreignKeys") ||
        !execSql(m_db, "PRAGMA cache_size = -8192;", "initSchema.cacheSize") ||
        !execSql(m_db, "PRAGMA mmap_size = 67108864;", "initSchema.mmapSize")) {
        return false;
    }

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
    if (!execSql(m_db, sqlTable, "initSchema.createTable")) {
        return false;
    }

    bool hasManifestId = false;
    bool hasFileClock = false;
    bool hasSigVerified = false;
    {
        sqlite3_stmt *infoStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, "PRAGMA table_info(workshop_items);", -1, &infoStmt,
                               nullptr) != SQLITE_OK) {
            std::fprintf(stderr, "[CatalogIndexManager] Failed to inspect schema: %s\n",
                         sqlite3_errmsg(m_db));
            return false;
        }
        int stepRc = SQLITE_OK;
        while ((stepRc = sqlite3_step(infoStmt)) == SQLITE_ROW) {
            const auto *colName = reinterpret_cast<const char *>(sqlite3_column_text(infoStmt, 1));
            if (!colName) {
                continue;
            }
            if (std::strcmp(colName, "manifest_id") == 0) {
                hasManifestId = true;
            } else if (std::strcmp(colName, "file_clock") == 0) {
                hasFileClock = true;
            } else if (std::strcmp(colName, "signature_verified") == 0) {
                hasSigVerified = true;
            }
        }
        sqlite3_finalize(infoStmt);
        if (stepRc != SQLITE_DONE) {
            std::fprintf(stderr, "[CatalogIndexManager] Schema inspection step failed: %s\n",
                         sqlite3_errmsg(m_db));
            return false;
        }
    }

    if (!hasManifestId &&
        !execSql(m_db,
                 "ALTER TABLE workshop_items ADD COLUMN manifest_id INTEGER NOT NULL DEFAULT 0;",
                 "initSchema.addManifestId")) {
        return false;
    }
    if (!hasFileClock &&
        !execSql(m_db,
                 "ALTER TABLE workshop_items ADD COLUMN file_clock INTEGER NOT NULL DEFAULT 0;",
                 "initSchema.addFileClock")) {
        return false;
    }
    if (!hasSigVerified &&
        !execSql(m_db,
                 "ALTER TABLE workshop_items ADD COLUMN signature_verified INTEGER NOT NULL "
                 "DEFAULT 1;",
                 "initSchema.addSigVerified")) {
        return false;
    }

    const char *sqlIndices = R"(
        CREATE INDEX IF NOT EXISTS idx_items_type_tier_subs ON workshop_items(type, compat_tier, subscriptions DESC);
        CREATE INDEX IF NOT EXISTS idx_items_subs           ON workshop_items(subscriptions DESC, published_file_id ASC);
        CREATE INDEX IF NOT EXISTS idx_items_time_updated   ON workshop_items(time_updated DESC, published_file_id ASC);
        CREATE INDEX IF NOT EXISTS idx_items_title_nocase   ON workshop_items(title COLLATE NOCASE ASC);
        DROP INDEX IF EXISTS idx_items_rating;
        DROP INDEX IF EXISTS idx_items_compat_score;
        DROP INDEX IF EXISTS idx_items_file_size;
    )";
    if (!execSql(m_db, sqlIndices, "initSchema.indices")) {
        return false;
    }

    bool rebuildLegacyFts = false;
    {
        sqlite3_stmt *chk = nullptr;
        if (sqlite3_prepare_v2(
                m_db, "SELECT sql FROM sqlite_master WHERE type='table' AND name='workshop_fts';",
                -1, &chk, nullptr) != SQLITE_OK) {
            std::fprintf(stderr, "[CatalogIndexManager] Failed to inspect FTS schema: %s\n",
                         sqlite3_errmsg(m_db));
            return false;
        }
        const int stepRc = sqlite3_step(chk);
        if (stepRc == SQLITE_ROW) {
            const auto *ddl = reinterpret_cast<const char *>(sqlite3_column_text(chk, 0));
            if (ddl && std::strstr(ddl, "columnsize=0") != nullptr) {
                rebuildLegacyFts = true;
            }
        } else if (stepRc != SQLITE_DONE) {
            std::fprintf(stderr, "[CatalogIndexManager] Failed stepping FTS schema query: %s\n",
                         sqlite3_errmsg(m_db));
            sqlite3_finalize(chk);
            return false;
        }
        sqlite3_finalize(chk);
    }
    if (rebuildLegacyFts &&
        !execSql(m_db, "DROP TABLE IF EXISTS workshop_fts;", "initSchema.dropFts")) {
        return false;
    }

    const char *sqlFts = R"(
        CREATE VIRTUAL TABLE IF NOT EXISTS workshop_fts USING fts5 (
            title,
            tags_string,
            description,
            content='workshop_items',
            content_rowid='published_file_id',
            tokenize='unicode61 remove_diacritics 2'
        );
    )";
    if (!execSql(m_db, sqlFts, "initSchema.createFts")) {
        return false;
    }
    if (rebuildLegacyFts &&
        !execSql(m_db, "INSERT INTO workshop_fts(workshop_fts) VALUES('rebuild');",
                 "initSchema.rebuildFts")) {
        return false;
    }

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
    if (!execSql(m_db, sqlTriggers, "initSchema.ftsTriggers")) {
        return false;
    }

    // Substring queries fall back to the lock-free Chunked SoA Myers pass instead of writing
    // heavyweight trigram FTS5 tables/triggers on every bulk upsert.
    m_impl->hasTrigramFts = false;
    (void)execSql(m_db,
                  "DROP TRIGGER IF EXISTS trg_workshop_tri_ai;"
                  "DROP TRIGGER IF EXISTS trg_workshop_tri_ad;"
                  "DROP TRIGGER IF EXISTS trg_workshop_tri_au;"
                  "DROP TABLE IF EXISTS workshop_fts_trigram;",
                  "initSchema.dropTrigram");

    {
        sqlite3_stmt *jsonProbe = nullptr;
        if (sqlite3_prepare_v2(m_db, "SELECT value FROM json_each('[1]');", -1, &jsonProbe,
                               nullptr) == SQLITE_OK &&
            jsonProbe) {
            m_impl->hasJsonEach = (sqlite3_step(jsonProbe) == SQLITE_ROW);
            sqlite3_finalize(jsonProbe);
        }
    }

    return true;
}

bool CatalogIndexManager::prepareStatements()
{
    if (!m_db) {
        return false;
    }

    auto prep = [this](const char *sql, sqlite3_stmt **out, const char *name) -> bool {
        if (sqlite3_prepare_v2(m_db, sql, -1, out, nullptr) != SQLITE_OK || !*out) {
            std::fprintf(stderr, "[CatalogIndexManager] Failed to prepare %s: %s\n", name,
                         sqlite3_errmsg(m_db));
            return false;
        }
        return true;
    };

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
           OR ABS(workshop_items.aspect_ratio - excluded.aspect_ratio) > 1e-4
           OR workshop_items.dominant_color != excluded.dominant_color
           OR workshop_items.tag_mask != excluded.tag_mask
           OR workshop_items.tags_string != excluded.tags_string
           OR workshop_items.description != excluded.description
           OR workshop_items.manifest_id != excluded.manifest_id
           OR workshop_items.signature_verified != excluded.signature_verified;
    )";

    const char *sqlDelete = "DELETE FROM workshop_items WHERE published_file_id = ?;";

    const char *sqlGetById = R"(
        SELECT published_file_id, owner_steam_id, title, type, file_path, preview_path,
               file_size_bytes, time_created, time_updated, subscriptions, favorited,
               views, rating, install_state, storage_backend, compat_tier, compat_score,
               aspect_ratio, dominant_color, tag_mask, tags_string, description,
               manifest_id, file_clock, signature_verified
        FROM workshop_items WHERE published_file_id = ?;
    )";

    const char *sqlGetByPath = R"(
        SELECT published_file_id, owner_steam_id, title, type, file_path, preview_path,
               file_size_bytes, time_created, time_updated, subscriptions, favorited,
               views, rating, install_state, storage_backend, compat_tier, compat_score,
               aspect_ratio, dominant_color, tag_mask, tags_string, description,
               manifest_id, file_clock, signature_verified
        FROM workshop_items WHERE file_path = ?;
    )";

    const char *sqlCount = "SELECT COUNT(*) FROM workshop_items;";

    const char *sqlFts5 = R"(
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

    if (!prep(sqlUpsert, &m_stmtUpsert, "upsert") || !prep(sqlDelete, &m_stmtDelete, "delete") ||
        !prep(sqlGetById, &m_stmtGetById, "getById") ||
        !prep(sqlGetByPath, &m_stmtGetByPath, "getByPath") ||
        !prep(sqlCount, &m_stmtCount, "count") || !prep(sqlFts5, &m_stmtFts5, "fts5")) {
        finalizeStatements();
        return false;
    }

    // Multi-row prepared statement amortizes SQLite VDBE setup on bulk upserts.
    std::string sqlBatch;
    sqlBatch.reserve(2048);
    sqlBatch.append("INSERT INTO workshop_items ("
                    "published_file_id, owner_steam_id, title, type, file_path, preview_path,"
                    "file_size_bytes, time_created, time_updated, subscriptions, favorited,"
                    "views, rating, install_state, storage_backend, compat_tier, compat_score,"
                    "aspect_ratio, dominant_color, tag_mask, tags_string, description,"
                    "manifest_id, file_clock, signature_verified) VALUES ");
    for (size_t r = 0; r < Impl::kBatchInsertRows; ++r) {
        if (r > 0) {
            sqlBatch.push_back(',');
        }
        sqlBatch.append("(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    }
    sqlBatch.append(" ON CONFLICT(published_file_id) DO UPDATE SET "
                    "owner_steam_id=excluded.owner_steam_id,"
                    "title=excluded.title,"
                    "type=excluded.type,"
                    "file_path=excluded.file_path,"
                    "preview_path=excluded.preview_path,"
                    "file_size_bytes=excluded.file_size_bytes,"
                    "time_created=excluded.time_created,"
                    "time_updated=excluded.time_updated,"
                    "subscriptions=excluded.subscriptions,"
                    "favorited=excluded.favorited,"
                    "views=excluded.views,"
                    "rating=excluded.rating,"
                    "install_state=excluded.install_state,"
                    "storage_backend=excluded.storage_backend,"
                    "compat_tier=excluded.compat_tier,"
                    "compat_score=excluded.compat_score,"
                    "aspect_ratio=excluded.aspect_ratio,"
                    "dominant_color=excluded.dominant_color,"
                    "tag_mask=excluded.tag_mask,"
                    "tags_string=excluded.tags_string,"
                    "description=excluded.description,"
                    "manifest_id=excluded.manifest_id,"
                    "file_clock=CASE WHEN excluded.file_clock>workshop_items.file_clock "
                    "THEN excluded.file_clock ELSE workshop_items.file_clock+1 END,"
                    "signature_verified=excluded.signature_verified "
                    "WHERE workshop_items.owner_steam_id!=excluded.owner_steam_id "
                    "OR workshop_items.title!=excluded.title "
                    "OR workshop_items.type!=excluded.type "
                    "OR workshop_items.file_path!=excluded.file_path "
                    "OR IFNULL(workshop_items.preview_path,'')!=IFNULL(excluded.preview_path,'') "
                    "OR workshop_items.file_size_bytes!=excluded.file_size_bytes "
                    "OR workshop_items.time_created!=excluded.time_created "
                    "OR workshop_items.time_updated!=excluded.time_updated "
                    "OR workshop_items.subscriptions!=excluded.subscriptions "
                    "OR workshop_items.favorited!=excluded.favorited "
                    "OR workshop_items.views!=excluded.views "
                    "OR workshop_items.rating!=excluded.rating "
                    "OR workshop_items.install_state!=excluded.install_state "
                    "OR workshop_items.storage_backend!=excluded.storage_backend "
                    "OR workshop_items.compat_tier!=excluded.compat_tier "
                    "OR workshop_items.compat_score!=excluded.compat_score "
                    "OR ABS(workshop_items.aspect_ratio-excluded.aspect_ratio)>1e-4 "
                    "OR workshop_items.dominant_color!=excluded.dominant_color "
                    "OR workshop_items.tag_mask!=excluded.tag_mask "
                    "OR workshop_items.tags_string!=excluded.tags_string "
                    "OR workshop_items.description!=excluded.description "
                    "OR workshop_items.manifest_id!=excluded.manifest_id "
                    "OR workshop_items.signature_verified!=excluded.signature_verified;");
    (void)prep(sqlBatch.c_str(), &m_impl->stmtUpsertBatch32, "upsertBatch32");
    (void)prep("BEGIN IMMEDIATE TRANSACTION;", &m_impl->stmtBeginImmediate, "beginImmediate");
    (void)prep("COMMIT;", &m_impl->stmtCommit, "commitTx");
    (void)prep("ROLLBACK;", &m_impl->stmtRollback, "rollbackTx");

    return true;
}

void CatalogIndexManager::finalizeStatements()
{
    auto finalizeOne = [](sqlite3_stmt *&s) {
        if (s) {
            sqlite3_finalize(s);
            s = nullptr;
        }
    };
    if (m_impl) {
        m_impl->finalizeBatchStmt();
    }
    finalizeOne(m_stmtUpsert);
    finalizeOne(m_stmtDelete);
    finalizeOne(m_stmtGetById);
    finalizeOne(m_stmtGetByPath);
    finalizeOne(m_stmtCount);
    finalizeOne(m_stmtFts5);
    finalizeOne(m_stmtFtsTrigram);
}

namespace {

std::vector<std::string> tokenizeSanitizedFts(std::string_view rawQuery)
{
    if (rawQuery.empty()) {
        return {};
    }

    std::string cleaned;
    cleaned.reserve(rawQuery.size());
    for (char ch : rawQuery) {
        const auto uc = static_cast<unsigned char>(ch);
        if (uc < 0x20u || uc == 0x7Fu) {
            cleaned.push_back(' ');
            continue;
        }
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
        const size_t start = i;
        while (i < cleaned.size() && static_cast<unsigned char>(cleaned[i]) > ' ') {
            ++i;
        }
        if (i > start) {
            tokens.emplace_back(cleaned.data() + start, i - start);
        }
    }
    return tokens;
}

std::string buildFtsPhraseQuery(std::span<const std::string> tokens)
{
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

struct PendingSoaDelta {
    bool isRemove{false};
    size_t itemIdx{0};
    uint64_t removeId{0};
};

bool upsertSingleItemSql(sqlite3 *db, sqlite3_stmt *stmtUpsert, sqlite3_stmt *stmtGetByPath,
                         sqlite3_stmt *stmtDelete, const WorkshopItemRecord &item, size_t itemIdx,
                         std::vector<PendingSoaDelta> &deltas)
{
    std::string lastErr;
    int extRc = SQLITE_OK;
    auto tryUpsert = [&]() -> int {
        sqlite3_reset(stmtUpsert);
        sqlite3_clear_bindings(stmtUpsert);
        if (!bindRecordToStmt(stmtUpsert, item)) {
            sqlite3_reset(stmtUpsert);
            return SQLITE_ERROR;
        }
        const int rc = sqlite3_step(stmtUpsert);
        if (rc != SQLITE_DONE) {
            extRc = db ? sqlite3_extended_errcode(db) : rc;
            lastErr = (db && sqlite3_errmsg(db)) ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
        }
        sqlite3_reset(stmtUpsert);
        return rc;
    };

    int rc = tryUpsert();
    if (rc == SQLITE_DONE) {
        deltas.push_back(PendingSoaDelta{false, itemIdx, 0});
        return true;
    }

    if ((rc == SQLITE_CONSTRAINT || extRc == SQLITE_CONSTRAINT_UNIQUE) && stmtGetByPath &&
        stmtDelete && !item.file_path.empty()) {
        sqlite3_reset(stmtGetByPath);
        sqlite3_clear_bindings(stmtGetByPath);
        if (sqlite3_bind_text(stmtGetByPath, 1, item.file_path.c_str(), -1, SQLITE_TRANSIENT) ==
            SQLITE_OK) {
            if (sqlite3_step(stmtGetByPath) == SQLITE_ROW) {
                const uint64_t oldId =
                    static_cast<uint64_t>(sqlite3_column_int64(stmtGetByPath, 0));
                sqlite3_reset(stmtGetByPath);
                if (oldId != item.published_file_id) {
                    sqlite3_reset(stmtDelete);
                    sqlite3_clear_bindings(stmtDelete);
                    if (sqlite3_bind_int64(stmtDelete, 1, static_cast<sqlite3_int64>(oldId)) ==
                            SQLITE_OK &&
                        sqlite3_step(stmtDelete) == SQLITE_DONE) {
                        sqlite3_reset(stmtDelete);
                        deltas.push_back(PendingSoaDelta{true, 0, oldId});
                        rc = tryUpsert();
                        if (rc == SQLITE_DONE) {
                            deltas.push_back(PendingSoaDelta{false, itemIdx, 0});
                            return true;
                        }
                    } else {
                        sqlite3_reset(stmtDelete);
                    }
                }
            } else {
                sqlite3_reset(stmtGetByPath);
            }
        } else {
            sqlite3_reset(stmtGetByPath);
        }
    }

    std::fprintf(stderr, "[CatalogIndexManager] Upsert failed for item %llu (rc=%d): %s\n",
                 static_cast<unsigned long long>(item.published_file_id), rc,
                 !lastErr.empty() ? lastErr.c_str() : sqlite3_errstr(rc));
    return false;
}

} // namespace

std::string CatalogIndexManager::sanitizeFts5Query(std::string_view rawQuery)
{
    const std::vector<std::string> tokens = tokenizeSanitizedFts(rawQuery);
    return buildFtsPhraseQuery(tokens);
}

bool CatalogIndexManager::upsertItem(const WorkshopItemRecord &item)
{
    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    if (!m_db || !m_stmtUpsert) {
        return false;
    }

    const bool txStarted =
        m_impl->stmtBeginImmediate
            ? Impl::stepTxStmt(m_db, m_impl->stmtBeginImmediate, "upsertItem.begin")
            : execSql(m_db, "BEGIN IMMEDIATE TRANSACTION;", "upsertItem.begin");
    if (!txStarted) {
        return false;
    }

    std::vector<PendingSoaDelta> deltas;
    deltas.reserve(2);
    const bool ok =
        upsertSingleItemSql(m_db, m_stmtUpsert, m_stmtGetByPath, m_stmtDelete, item, 0, deltas);
    if (!ok) {
        rollbackIfInTx(m_db, "upsertItem.rollback");
        return false;
    }

    const bool txCommitted = m_impl->stmtCommit
                                 ? Impl::stepTxStmt(m_db, m_impl->stmtCommit, "upsertItem.commit")
                                 : execSql(m_db, "COMMIT;", "upsertItem.commit");
    if (!txCommitted) {
        rollbackIfInTx(m_db, "upsertItem.commitRollback");
        return false;
    }

    m_impl->beginChunkEdit();
    for (const auto &d : deltas) {
        if (d.isRemove) {
            m_impl->removeSoaLocked(d.removeId);
        } else {
            m_impl->upsertSoaLocked(item.published_file_id, item.tag_mask, item.title,
                                    item.tags_string, item.description);
        }
    }
    m_impl->commitChunkEdit();
    return true;
}

bool CatalogIndexManager::upsertItems(const std::vector<WorkshopItemRecord> &items)
{
    std::unique_lock<std::mutex> lock(m_impl->dbMutex);
    if (!m_db || !m_stmtUpsert) {
        return false;
    }
    if (items.empty()) {
        return true;
    }

    if (!execSql(m_db, "BEGIN IMMEDIATE TRANSACTION;", "upsertItems.begin")) {
        return false;
    }

    std::vector<PendingSoaDelta> deltas;
    deltas.reserve(items.size());

    bool allOk = true;
    size_t i = 0;
    if (m_impl->stmtUpsertBatch32 && items.size() >= Impl::kBatchInsertRows) {
        constexpr size_t kCols = 25;
        sqlite3_stmt *batchStmt = m_impl->stmtUpsertBatch32;
        while (i + Impl::kBatchInsertRows <= items.size() && allOk) {
            sqlite3_reset(batchStmt);
            sqlite3_clear_bindings(batchStmt);
            bool bindOk = true;
            for (size_t r = 0; r < Impl::kBatchInsertRows; ++r) {
                if (!bindRecordAtOffset(batchStmt, static_cast<int>(r * kCols), items[i + r])) {
                    bindOk = false;
                    break;
                }
            }
            if (bindOk) {
                const int rc = sqlite3_step(batchStmt);
                sqlite3_reset(batchStmt);
                if (rc == SQLITE_DONE) {
                    for (size_t r = 0; r < Impl::kBatchInsertRows; ++r) {
                        deltas.push_back(PendingSoaDelta{false, i + r, 0});
                    }
                    i += Impl::kBatchInsertRows;
                    continue;
                }
            } else {
                sqlite3_reset(batchStmt);
            }
            // Fall back to row-by-row conflict resolution for this 32-item slice on constraint
            // clash.
            for (size_t r = 0; r < Impl::kBatchInsertRows; ++r) {
                if (!upsertSingleItemSql(m_db, m_stmtUpsert, m_stmtGetByPath, m_stmtDelete,
                                         items[i + r], i + r, deltas)) {
                    allOk = false;
                    break;
                }
            }
            i += Impl::kBatchInsertRows;
        }
    }

    for (; i < items.size() && allOk; ++i) {
        if (!upsertSingleItemSql(m_db, m_stmtUpsert, m_stmtGetByPath, m_stmtDelete, items[i], i,
                                 deltas)) {
            allOk = false;
            break;
        }
    }

    if (allOk && !execSql(m_db, "COMMIT;", "upsertItems.commit")) {
        allOk = false;
    }

    if (!allOk) {
        rollbackIfInTx(m_db, "upsertItems.rollback");
        return false;
    }

    m_impl->beginChunkEdit();
    for (const auto &d : deltas) {
        if (d.isRemove) {
            m_impl->removeSoaLocked(d.removeId);
        } else {
            const auto &item = items[d.itemIdx];
            m_impl->upsertSoaLocked(item.published_file_id, item.tag_mask, item.title,
                                    item.tags_string, item.description);
        }
    }
    m_impl->commitChunkEdit();
    if (items.size() >= 64U) {
        sqlite3_wal_checkpoint_v2(m_db, nullptr, SQLITE_CHECKPOINT_PASSIVE, nullptr, nullptr);
        const uint64_t sampleId = items.front().published_file_id;
        lock.unlock();
        m_impl->readPool.warmIdleSlots(sampleId);
    }
    return true;
}

bool CatalogIndexManager::removeItem(uint64_t publishedFileId)
{
    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    if (!m_db || !m_stmtDelete) {
        return false;
    }

    sqlite3_reset(m_stmtDelete);
    sqlite3_clear_bindings(m_stmtDelete);
    if (sqlite3_bind_int64(m_stmtDelete, 1, static_cast<sqlite3_int64>(publishedFileId)) !=
        SQLITE_OK) {
        sqlite3_reset(m_stmtDelete);
        return false;
    }

    const int rc = sqlite3_step(m_stmtDelete);
    sqlite3_reset(m_stmtDelete);
    if (rc == SQLITE_DONE) {
        m_impl->beginChunkEdit();
        m_impl->removeSoaLocked(publishedFileId);
        m_impl->commitChunkEdit();
        return true;
    }
    std::fprintf(stderr, "[CatalogIndexManager] Delete failed for item %llu (rc=%d): %s\n",
                 static_cast<unsigned long long>(publishedFileId), rc, sqlite3_errmsg(m_db));
    return false;
}

std::optional<WorkshopItemRecord> CatalogIndexManager::getItem(uint64_t publishedFileId) const
{
    auto execGet = [publishedFileId](sqlite3 *db,
                                     sqlite3_stmt *stmt) -> std::optional<WorkshopItemRecord> {
        if (!stmt) {
            return std::nullopt;
        }
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        if (sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(publishedFileId)) != SQLITE_OK) {
            sqlite3_reset(stmt);
            return std::nullopt;
        }
        std::optional<WorkshopItemRecord> result;
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            result = readRecordFromStmt(stmt);
        } else if (rc != SQLITE_DONE) {
            std::fprintf(stderr, "[CatalogIndexManager] getItem step failed (rc=%d): %s\n", rc,
                         db ? sqlite3_errmsg(db) : "null db");
        }
        sqlite3_reset(stmt);
        return result;
    };

    if (m_impl->readPool.isOpen()) {
        auto lease = m_impl->readPool.acquire();
        if (!lease.valid()) {
            return std::nullopt;
        }
        return execGet(lease.slot().db, lease.slot().stmtGetById);
    }

    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    return (m_db && m_stmtGetById) ? execGet(m_db, m_stmtGetById) : std::nullopt;
}

std::optional<WorkshopItemRecord>
CatalogIndexManager::getItemByPath(const std::string &filePath) const
{
    auto execGet = [&filePath](sqlite3 *db,
                               sqlite3_stmt *stmt) -> std::optional<WorkshopItemRecord> {
        if (!stmt) {
            return std::nullopt;
        }
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        if (sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
            sqlite3_reset(stmt);
            return std::nullopt;
        }
        std::optional<WorkshopItemRecord> result;
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            result = readRecordFromStmt(stmt);
        } else if (rc != SQLITE_DONE) {
            std::fprintf(stderr, "[CatalogIndexManager] getItemByPath step failed (rc=%d): %s\n",
                         rc, db ? sqlite3_errmsg(db) : "null db");
        }
        sqlite3_reset(stmt);
        return result;
    };

    if (m_impl->readPool.isOpen()) {
        auto lease = m_impl->readPool.acquire();
        if (!lease.valid()) {
            return std::nullopt;
        }
        return execGet(lease.slot().db, lease.slot().stmtGetByPath);
    }

    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    return (m_db && m_stmtGetByPath) ? execGet(m_db, m_stmtGetByPath) : std::nullopt;
}

size_t CatalogIndexManager::totalItemCount() const
{
    auto execCount = [](sqlite3 *db, sqlite3_stmt *stmt) -> size_t {
        if (!stmt) {
            return 0;
        }
        sqlite3_reset(stmt);
        size_t count = 0;
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            count = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
        } else if (rc != SQLITE_DONE) {
            std::fprintf(stderr, "[CatalogIndexManager] totalItemCount step failed (rc=%d): %s\n",
                         rc, db ? sqlite3_errmsg(db) : "null db");
        }
        sqlite3_reset(stmt);
        return count;
    };

    if (m_impl->readPool.isOpen()) {
        auto lease = m_impl->readPool.acquire();
        if (!lease.valid()) {
            return 0;
        }
        return execCount(lease.slot().db, lease.slot().stmtCount);
    }

    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    return (m_db && m_stmtCount) ? execCount(m_db, m_stmtCount) : 0;
}

size_t CatalogIndexManager::filterByTagMaskBitmap(uint64_t requiredMask,
                                                  std::vector<uint64_t> *outIds) const
{
    if (outIds) {
        outIds->clear();
    }

    const auto snap = m_impl->rcuPool.acquire_snapshot();
    if (!snap || snap.total_count() == 0U) {
        return 0;
    }

    const auto &segments = snap.segments();
    if (requiredMask == 0ULL) {
        if (outIds) {
            outIds->reserve(snap.total_count());
            for (const auto &seg : segments) {
                if (!seg || seg->count == 0U) {
                    continue;
                }
                outIds->insert(outIds->end(), seg->published_ids.data(),
                               seg->published_ids.data() + seg->count);
            }
        }
        return snap.total_count();
    }

    std::array<unsigned, 64> activeBitIdx{};
    size_t planeCount = 0;
    for (uint64_t m = requiredMask; m != 0ULL; m &= (m - 1ULL)) {
        activeBitIdx[planeCount++] = static_cast<unsigned>(std::countr_zero(m));
    }

    size_t matches = 0;
    for (const auto &seg : segments) {
        if (!seg || seg->count == 0U) {
            continue;
        }
        const uint32_t segCount = seg->count;
        const size_t wordCount = (segCount + 63u) >> 6;
        const unsigned tailBits = static_cast<unsigned>(segCount & 63u);
        const uint64_t tailMask = (tailBits == 0u) ? ~0ULL : ((1ULL << tailBits) - 1ULL);
        const uint64_t *ids = seg->published_ids.data();

        for (size_t w = 0; w < wordCount; ++w) {
            uint64_t bits = seg->tag_bitsets[activeBitIdx[0]][w];
            for (size_t p = 1; p < planeCount; ++p) {
                bits &= seg->tag_bitsets[activeBitIdx[p]][w];
            }
            if (w + 1 == wordCount) {
                bits &= tailMask;
            }
            if (outIds) {
                while (bits != 0ULL) {
                    const unsigned bitIdx = static_cast<unsigned>(std::countr_zero(bits));
                    outIds->push_back(ids[(w << 6) + bitIdx]);
                    bits &= (bits - 1ULL);
                }
            } else {
                matches += static_cast<size_t>(std::popcount(bits));
            }
        }
    }
    return outIds ? outIds->size() : matches;
}

namespace {

constexpr int kMaxQueryLimit = 100000;
constexpr int kMaxQueryOffset = 10000000;
constexpr size_t kMaxReserveRows = 4096;

struct StmtResetGuard {
    sqlite3_stmt *stmt{nullptr};
    ~StmtResetGuard()
    {
        if (stmt) {
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
        }
    }
};

std::string buildFilterQuerySql(bool hasType, bool hasTier, int tagMode, bool hasInstall,
                                CatalogSortOrder sortOrder)
{
    std::ostringstream sql;
    sql << "SELECT published_file_id, owner_steam_id, title, type, file_path, preview_path, "
           "file_size_bytes, time_created, time_updated, subscriptions, favorited, "
           "views, rating, install_state, storage_backend, compat_tier, compat_score, "
           "aspect_ratio, dominant_color, tag_mask, tags_string, description, "
           "manifest_id, file_clock, signature_verified "
           "FROM workshop_items";

    bool firstCond = true;
    auto appendCond = [&](const char *cond) {
        sql << (firstCond ? " WHERE " : " AND ") << cond;
        firstCond = false;
    };

    if (hasType) {
        appendCond("type = ?");
    }
    if (hasTier) {
        appendCond("compat_tier = ?");
    }
    if (tagMode == 1) {
        appendCond("published_file_id IN (SELECT value FROM json_each(?))");
    } else if (tagMode == 2) {
        appendCond("(tag_mask & ?) = ?");
    }
    if (hasInstall) {
        appendCond("(install_state & ?) != 0");
    }

    switch (sortOrder) {
    case CatalogSortOrder::SubscriptionsDesc:
        sql << " ORDER BY subscriptions DESC, published_file_id ASC";
        break;
    case CatalogSortOrder::RatingDesc:
        sql << " ORDER BY rating DESC, published_file_id ASC";
        break;
    case CatalogSortOrder::RecentlyUpdated:
        sql << " ORDER BY time_updated DESC, published_file_id ASC";
        break;
    case CatalogSortOrder::CompatibilityScoreDesc:
        sql << " ORDER BY compat_score DESC, subscriptions DESC, published_file_id ASC";
        break;
    case CatalogSortOrder::TitleAsc:
        sql << " ORDER BY title COLLATE NOCASE ASC, published_file_id ASC";
        break;
    case CatalogSortOrder::FileSizeDesc:
        sql << " ORDER BY file_size_bytes DESC, published_file_id ASC";
        break;
    }

    sql << " LIMIT ? OFFSET ?;";
    return sql.str();
}

} // namespace

std::vector<WorkshopItemRecord> CatalogIndexManager::searchFuzzy(const std::string &query,
                                                                 uint64_t requiredTagMask,
                                                                 int limit) const
{
    const int cappedLimit = std::clamp(limit > 0 ? limit : 50, 1, kMaxQueryLimit);
    const size_t reserveCap = std::min<size_t>(static_cast<size_t>(cappedLimit), kMaxReserveRows);

    if (!m_db) {
        return {};
    }

    const std::string trimmed = trimAsciiWs(query);
    if (trimmed.empty()) {
        return {};
    }

    std::vector<WorkshopItemRecord> results;
    const std::vector<std::string> triTokens = tokenizeSanitizedFts(trimmed);
    bool canUseTrigram = m_impl->hasTrigramFts && requiredTagMask == 0 && !triTokens.empty();
    if (canUseTrigram) {
        // SQLite FTS5 trigram silently drops tokens shorter than 3 codepoints in multi-word
        // queries; fall back to Myers when any token has fewer than 3 codepoints.
        for (const auto &tok : triTokens) {
            if (utf8CodepointCount(tok) < 3) {
                canUseTrigram = false;
                break;
            }
        }
    }

    if (canUseTrigram) {
        const std::string sanitizedTri = buildFtsPhraseQuery(triTokens);
        auto runTrigram = [&](sqlite3 *db, sqlite3_stmt *stmt) {
            if (!stmt || sanitizedTri.empty()) {
                return;
            }
            StmtResetGuard resetGuard{stmt};
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            if (sqlite3_bind_text(stmt, 1, sanitizedTri.c_str(), -1, SQLITE_TRANSIENT) !=
                    SQLITE_OK ||
                sqlite3_bind_int(stmt, 2, cappedLimit) != SQLITE_OK ||
                sqlite3_bind_int(stmt, 3, 0) != SQLITE_OK) {
                return;
            }
            results.reserve(reserveCap);
            int stepRc = SQLITE_OK;
            while ((stepRc = sqlite3_step(stmt)) == SQLITE_ROW) {
                results.push_back(readRecordFromStmt(stmt));
            }
            if (stepRc != SQLITE_DONE) {
                std::fprintf(stderr, "[CatalogIndexManager] Trigram query failed (rc=%d): %s\n",
                             stepRc, db ? sqlite3_errmsg(db) : "null db");
                results.clear();
            }
        };

        if (m_impl->readPool.isOpen()) {
            auto lease = m_impl->readPool.acquire();
            if (!lease.valid()) {
                return {};
            }
            runTrigram(lease.slot().db, lease.slot().stmtTrigram);
        } else {
            std::lock_guard<std::mutex> lock(m_impl->dbMutex);
            runTrigram(m_db, m_stmtFtsTrigram);
        }
        if (!results.empty()) {
            return results;
        }
    }

    const auto snap = m_impl->rcuPool.acquire_snapshot();
    if (!snap || snap.total_count() == 0U) {
        return {};
    }

    struct FuzzyTokenMatcher {
        bool isAscii{true};
        uint32_t m{0};
        uint32_t maxDist{0};
        std::array<uint64_t, 256> peq{};
        PatternCodepointMap cpMap{};
    };

    std::vector<FuzzyTokenMatcher> matchers;
    size_t tokStart = 0;
    while (tokStart < trimmed.size()) {
        while (tokStart < trimmed.size() && static_cast<unsigned char>(trimmed[tokStart]) <= ' ') {
            ++tokStart;
        }
        if (tokStart >= trimmed.size()) {
            break;
        }
        size_t tokEnd = tokStart;
        while (tokEnd < trimmed.size() && static_cast<unsigned char>(trimmed[tokEnd]) > ' ') {
            ++tokEnd;
        }

        const std::string lowerTok =
            toLowerUtf8(std::string_view(trimmed.data() + tokStart, tokEnd - tokStart));
        tokStart = tokEnd;

        std::vector<uint32_t> cps;
        cps.reserve(std::min<size_t>(lowerTok.size(), 64u));
        bool tokenAscii = true;
        size_t pos = 0;
        while (pos < lowerTok.size() && cps.size() < 64u) {
            const uint32_t cp = decodeUtf8Next(lowerTok, pos);
            if (cp >= 0x80u) {
                tokenAscii = false;
            }
            cps.push_back(cp);
        }
        if (cps.empty()) {
            continue;
        }

        FuzzyTokenMatcher tm;
        tm.isAscii = tokenAscii;
        tm.m = static_cast<uint32_t>(cps.size());
        tm.maxDist = (tm.m <= 3) ? 0u : ((tm.m <= 5) ? 1u : std::min<uint32_t>(2u, tm.m / 3u));
        if (tm.isAscii) {
            for (uint32_t i = 0; i < tm.m; ++i) {
                tm.peq[static_cast<unsigned char>(cps[i])] |= (1ULL << i);
            }
        } else {
            tm.cpMap.build(cps);
        }
        matchers.push_back(std::move(tm));
    }

    if (matchers.empty()) {
        return {};
    }

    struct CandidateHit {
        uint32_t dist;
        uint32_t titleLen;
        uint64_t pubId;
    };
    std::vector<CandidateHit> hits;
    hits.reserve(std::min<size_t>(reserveCap * 2u, kMaxReserveRows));

    for (const auto &seg : snap.segments()) {
        if (!seg || seg->count == 0U) {
            continue;
        }
        const uint32_t segCount = seg->count;
        const uint64_t *masks = seg->tag_masks.data();
        const uint64_t *ids = seg->published_ids.data();
        const uint32_t *lens = seg->title_lengths.data();
        for (uint32_t i = 0; i < segCount; ++i) {
            if (requiredTagMask != 0ULL && (masks[i] & requiredTagMask) != requiredTagMask) {
                continue;
            }
            const std::string_view titleView = seg->lowerTitleAt(i);
            uint32_t totalDist = 0;
            bool matchedAll = true;
            for (const auto &tm : matchers) {
                const uint32_t d = tm.isAscii
                                       ? myersSubstringDistance64(titleView, tm.peq, tm.m)
                                       : myersSubstringDistanceCodepoints64(titleView, tm.cpMap);
                if (d > tm.maxDist) {
                    matchedAll = false;
                    break;
                }
                totalDist += d;
            }
            if (matchedAll) {
                hits.push_back(CandidateHit{totalDist, lens[i], ids[i]});
            }
        }
    }

    if (hits.empty()) {
        return {};
    }

    const size_t keep = std::min(hits.size(), static_cast<size_t>(cappedLimit));
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
    auto fetchByIds = [&](sqlite3 *db, sqlite3_stmt *stmt) {
        if (!stmt) {
            return;
        }
        StmtResetGuard resetGuard{stmt};
        for (size_t i = 0; i < keep; ++i) {
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            if (sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(hits[i].pubId)) !=
                SQLITE_OK) {
                continue;
            }
            const int rc = sqlite3_step(stmt);
            if (rc == SQLITE_ROW) {
                WorkshopItemRecord rec = readRecordFromStmt(stmt);
                rec.search_rank = static_cast<double>(hits[i].dist) * 25.0 - 100.0;
                results.push_back(std::move(rec));
            } else if (rc != SQLITE_DONE) {
                std::fprintf(stderr,
                             "[CatalogIndexManager] searchFuzzy lookup failed (rc=%d): %s\n", rc,
                             db ? sqlite3_errmsg(db) : "null db");
            }
        }
    };

    if (m_impl->readPool.isOpen()) {
        auto lease = m_impl->readPool.acquire();
        if (!lease.valid()) {
            return {};
        }
        fetchByIds(lease.slot().db, lease.slot().stmtGetById);
    } else {
        std::lock_guard<std::mutex> lock(m_impl->dbMutex);
        if (m_db && m_stmtGetById) {
            fetchByIds(m_db, m_stmtGetById);
        }
    }
    return results;
}

std::vector<WorkshopItemRecord> CatalogIndexManager::searchFts5Bm25(const std::string &query,
                                                                    int limit, int offset) const
{
    const std::string sanitized = sanitizeFts5Query(query);
    if (sanitized.empty() || !m_db) {
        return {};
    }

    const int cappedLimit = std::clamp(limit > 0 ? limit : 50, 1, kMaxQueryLimit);
    const int safeOffset = std::clamp(offset >= 0 ? offset : 0, 0, kMaxQueryOffset);
    const size_t keepCount = static_cast<size_t>(safeOffset) + static_cast<size_t>(cappedLimit);

    std::vector<WorkshopItemRecord> results;
    results.reserve(std::min<size_t>(static_cast<size_t>(cappedLimit), kMaxReserveRows));

    // Answer bounded ASCII BM25 queries directly over wait-free ChunkedCatalogSnapshot arenas
    // before hydrating winning rowids via ReadOnlySqlitePool.
    const std::vector<std::string> rawTokens = tokenizeSanitizedFts(query);
    bool allAscii = !rawTokens.empty();
    std::vector<std::string> lowerTokens;
    if (allAscii && keepCount <= 1024U) {
        lowerTokens.reserve(rawTokens.size());
        uint64_t qBloomMask = 0ULL;
        for (const auto &tok : rawTokens) {
            for (unsigned char c : tok) {
                if (c >= 0x80u) {
                    allAscii = false;
                    break;
                }
            }
            if (!allAscii) {
                break;
            }
            std::string lowerTok = toLowerUtf8(tok);
            qBloomMask |= prefixTokenBloomMask(lowerTok);
            lowerTokens.push_back(std::move(lowerTok));
        }

        if (allAscii && !lowerTokens.empty()) {
            const auto snap = m_impl->rcuPool.acquire_snapshot();
            if (snap && snap.total_count() > 0U) {
                struct Bm25Hit {
                    double rankScore;
                    uint64_t pubId;
                };
                auto betterThan = [](const Bm25Hit &a, const Bm25Hit &b) noexcept {
                    if (a.rankScore != b.rankScore) {
                        return a.rankScore < b.rankScore;
                    }
                    return a.pubId < b.pubId;
                };

                std::vector<Bm25Hit> topHits;
                topHits.reserve(std::min<size_t>(keepCount, 128U));
                const size_t tokCount = lowerTokens.size();

                auto pushHit = [&](double docScore, uint64_t pubId) {
                    const Bm25Hit cand{-docScore, pubId};
                    if (topHits.size() < keepCount) {
                        topHits.push_back(cand);
                        if (topHits.size() == keepCount) {
                            std::make_heap(topHits.begin(), topHits.end(), betterThan);
                        }
                    } else if (betterThan(cand, topHits.front())) {
                        std::pop_heap(topHits.begin(), topHits.end(), betterThan);
                        topHits.back() = cand;
                        std::push_heap(topHits.begin(), topHits.end(), betterThan);
                    }
                };

                struct SegDocAcc {
                    double scoreAcc;
                    uint32_t tfT;
                    uint32_t tfG;
                    uint32_t tfD;
                    uint32_t matchMask;
                };
                struct TlsBm25Scratch {
                    std::array<uint16_t, ChunkedCatalogSegment::kChunkLen> stamps{};
                    std::array<SegDocAcc, ChunkedCatalogSegment::kChunkLen> docs;
                    std::vector<uint16_t> touched{};
                    uint16_t curStamp{0U};
                };
                thread_local TlsBm25Scratch tlsAcc;

                auto nextStamp = [&]() -> uint16_t {
                    if (tlsAcc.curStamp == std::numeric_limits<uint16_t>::max()) {
                        tlsAcc.stamps.fill(0U);
                        tlsAcc.curStamp = 0U;
                    }
                    return ++tlsAcc.curStamp;
                };

                auto evalSingleDocFromArena = [&](const ChunkedCatalogSegment &seg, uint32_t i) {
                    if ((seg.doc_bloom[i] & qBloomMask) != qBloomMask) {
                        return;
                    }
                    const std::string_view titleView = seg.lowerTitleAt(i);
                    const std::string_view tagsView = seg.lowerTagsAt(i);
                    const std::string_view descView = seg.lowerDescAt(i);

                    double docScore = 0.0;
                    for (size_t t = 0; t < tokCount; ++t) {
                        const bool exactWord = (t + 1U < tokCount);
                        const std::string_view tok = lowerTokens[t];
                        const uint32_t tfTitle = countWordMatches(titleView, tok, exactWord);
                        const uint32_t tfTags = countWordMatches(tagsView, tok, exactWord);
                        const uint32_t tfDesc = countWordMatches(descView, tok, exactWord);
                        if (tfTitle == 0U && tfTags == 0U && tfDesc == 0U) {
                            return;
                        }
                        docScore += 10.0 * bm25TfNorm(tfTitle, titleView.size(), 24.0) +
                                    5.0 * bm25TfNorm(tfTags, tagsView.size(), 16.0) +
                                    1.0 * bm25TfNorm(tfDesc, descView.size(), 64.0);
                    }
                    pushHit(docScore, seg.published_ids[i]);
                };

                auto findRange = [&](const SegmentTokenIndex &ti, std::string_view tok,
                                     bool exactWord) -> std::pair<size_t, size_t> {
                    auto lo = std::lower_bound(
                        ti.dict.begin(), ti.dict.end(), tok,
                        [&](const SegmentTokenDictEntry &de, std::string_view key) {
                            return ti.tokenAt(de) < key;
                        });
                    if (lo == ti.dict.end()) {
                        return {0U, 0U};
                    }
                    const size_t loIdx = static_cast<size_t>(lo - ti.dict.begin());
                    if (exactWord) {
                        if (ti.tokenAt(*lo) == tok) {
                            return {loIdx, loIdx + 1U};
                        }
                        return {0U, 0U};
                    }
                    auto hi = std::partition_point(lo, ti.dict.end(),
                                                   [&](const SegmentTokenDictEntry &de) {
                                                       return ti.tokenAt(de).starts_with(tok);
                                                   });
                    return {loIdx, static_cast<size_t>(hi - ti.dict.begin())};
                };

                if (tokCount == 1U) {
                    struct SegCand {
                        double upperBound;
                        size_t segIdx;
                        size_t loIdx;
                        size_t hiIdx;
                    };
                    const auto &segs = snap.segments();
                    std::vector<SegCand> segCands;
                    segCands.reserve(segs.size());
                    for (size_t segIdx = 0U; segIdx < segs.size(); ++segIdx) {
                        const auto &seg = segs[segIdx];
                        if (!seg || seg->count == 0U) {
                            continue;
                        }
                        const uint32_t segCount = seg->count;
                        if (!seg->token_index) {
                            for (uint32_t i = 0U; i < segCount; ++i) {
                                evalSingleDocFromArena(*seg, i);
                            }
                            continue;
                        }
                        for (uint16_t d = 0U; d < seg->dirty_count; ++d) {
                            const uint16_t dirtyIdx = seg->dirty_slots[d];
                            if (dirtyIdx < segCount) {
                                evalSingleDocFromArena(*seg, dirtyIdx);
                            }
                        }
                        const SegmentTokenIndex &ti = *seg->token_index;
                        const auto [loIdx, hiIdx] = findRange(ti, lowerTokens[0], false);
                        if (loIdx == hiIdx) {
                            continue;
                        }
                        double upperBound = 0.0;
                        for (size_t d = loIdx; d < hiIdx; ++d) {
                            const auto &de = ti.dict[d];
                            if (de.postLen > 0U) {
                                upperBound += ti.postings[de.postBegin].singleScore;
                            }
                        }
                        segCands.push_back(SegCand{upperBound, segIdx, loIdx, hiIdx});
                    }

                    std::sort(segCands.begin(), segCands.end(),
                              [](const SegCand &a, const SegCand &b) noexcept {
                                  return a.upperBound > b.upperBound;
                              });

                    for (const auto &sc : segCands) {
                        if (topHits.size() == keepCount &&
                            sc.upperBound < -topHits.front().rankScore) {
                            break;
                        }
                        const auto &seg = segs[sc.segIdx];
                        const uint32_t segCount = seg->count;
                        const SegmentTokenIndex &ti = *seg->token_index;
                        if (sc.loIdx + 1U == sc.hiIdx) {
                            const auto &de = ti.dict[sc.loIdx];
                            size_t cleanTaken = 0U;
                            const uint32_t endPos = de.postBegin + de.postLen;
                            for (uint32_t p = de.postBegin; p < endPos; ++p) {
                                const auto &post = ti.postings[p];
                                if (topHits.size() == keepCount &&
                                    post.singleScore < -topHits.front().rankScore) {
                                    break;
                                }
                                if (post.localIdx >= segCount || seg->isSlotDirty(post.localIdx)) {
                                    continue;
                                }
                                pushHit(post.singleScore, seg->published_ids[post.localIdx]);
                                if (++cleanTaken >= keepCount) {
                                    break;
                                }
                            }
                        } else {
                            const uint16_t stamp = nextStamp();
                            tlsAcc.touched.clear();
                            for (size_t d = sc.loIdx; d < sc.hiIdx; ++d) {
                                const auto &de = ti.dict[d];
                                const uint32_t endPos = de.postBegin + de.postLen;
                                for (uint32_t p = de.postBegin; p < endPos; ++p) {
                                    const auto &post = ti.postings[p];
                                    if (post.localIdx >= segCount ||
                                        seg->isSlotDirty(post.localIdx)) {
                                        continue;
                                    }
                                    auto &acc = tlsAcc.docs[post.localIdx];
                                    if (tlsAcc.stamps[post.localIdx] != stamp) {
                                        tlsAcc.stamps[post.localIdx] = stamp;
                                        acc.tfT = 0U;
                                        acc.tfG = 0U;
                                        acc.tfD = 0U;
                                        tlsAcc.touched.push_back(post.localIdx);
                                    }
                                    acc.tfT += post.tfTitle;
                                    acc.tfG += post.tfTags;
                                    acc.tfD += post.tfDesc;
                                }
                            }
                            for (uint16_t docIdx : tlsAcc.touched) {
                                const auto &acc = tlsAcc.docs[docIdx];
                                const double docScore =
                                    10.0 * bm25TfNorm(acc.tfT, seg->title_lengths[docIdx], 24.0) +
                                    5.0 * bm25TfNorm(acc.tfG, seg->tags_lengths[docIdx], 16.0) +
                                    1.0 * bm25TfNorm(acc.tfD, seg->desc_lengths[docIdx], 64.0);
                                pushHit(docScore, seg->published_ids[docIdx]);
                            }
                        }
                    }
                } else {
                    for (const auto &seg : snap.segments()) {
                        if (!seg || seg->count == 0U) {
                            continue;
                        }
                        const uint32_t segCount = seg->count;

                        if (!seg->token_index || tokCount > 16U) {
                            for (uint32_t i = 0U; i < segCount; ++i) {
                                evalSingleDocFromArena(*seg, i);
                            }
                            continue;
                        }

                        for (uint16_t d = 0U; d < seg->dirty_count; ++d) {
                            const uint16_t dirtyIdx = seg->dirty_slots[d];
                            if (dirtyIdx < segCount) {
                                evalSingleDocFromArena(*seg, dirtyIdx);
                            }
                        }

                        const SegmentTokenIndex &ti = *seg->token_index;
                        std::array<std::pair<size_t, size_t>, 16> ranges{};
                        bool allPresent = true;
                        size_t anchorToken = 0U;
                        size_t smallestCount = std::numeric_limits<size_t>::max();
                        for (size_t t = 0U; t < tokCount; ++t) {
                            ranges[t] = findRange(ti, lowerTokens[t], t + 1U < tokCount);
                            if (ranges[t].first == ranges[t].second) {
                                allPresent = false;
                                break;
                            }
                            size_t spanPosts = 0U;
                            for (size_t d = ranges[t].first; d < ranges[t].second; ++d) {
                                spanPosts += ti.dict[d].postLen;
                            }
                            if (spanPosts < smallestCount) {
                                smallestCount = spanPosts;
                                anchorToken = t;
                            }
                        }
                        if (!allPresent) {
                            continue;
                        }

                        const uint16_t stamp = nextStamp();
                        tlsAcc.touched.clear();
                        const uint32_t fullMask = (1U << tokCount) - 1U;
                        const bool anchorIsLast = (anchorToken + 1U == tokCount);
                        for (size_t d = ranges[anchorToken].first; d < ranges[anchorToken].second;
                             ++d) {
                            const auto &de = ti.dict[d];
                            const uint32_t endPos = de.postBegin + de.postLen;
                            for (uint32_t p = de.postBegin; p < endPos; ++p) {
                                const auto &post = ti.postings[p];
                                if (post.localIdx >= segCount || seg->isSlotDirty(post.localIdx)) {
                                    continue;
                                }
                                auto &acc = tlsAcc.docs[post.localIdx];
                                if (tlsAcc.stamps[post.localIdx] != stamp) {
                                    tlsAcc.stamps[post.localIdx] = stamp;
                                    acc.scoreAcc = 0.0;
                                    acc.tfT = 0U;
                                    acc.tfG = 0U;
                                    acc.tfD = 0U;
                                    acc.matchMask = 1U << anchorToken;
                                    tlsAcc.touched.push_back(post.localIdx);
                                }
                                if (anchorIsLast) {
                                    acc.tfT += post.tfTitle;
                                    acc.tfG += post.tfTags;
                                    acc.tfD += post.tfDesc;
                                } else {
                                    acc.scoreAcc += post.singleScore;
                                }
                            }
                        }
                        for (size_t t = 0U; t < tokCount; ++t) {
                            if (t == anchorToken) {
                                continue;
                            }
                            const bool isLast = (t + 1U == tokCount);
                            const uint32_t tBit = 1U << t;
                            for (size_t d = ranges[t].first; d < ranges[t].second; ++d) {
                                const auto &de = ti.dict[d];
                                const uint32_t endPos = de.postBegin + de.postLen;
                                for (uint32_t p = de.postBegin; p < endPos; ++p) {
                                    const auto &post = ti.postings[p];
                                    if (post.localIdx >= segCount) {
                                        continue;
                                    }
                                    if (tlsAcc.stamps[post.localIdx] != stamp) {
                                        continue;
                                    }
                                    auto &acc = tlsAcc.docs[post.localIdx];
                                    acc.matchMask |= tBit;
                                    if (isLast) {
                                        acc.tfT += post.tfTitle;
                                        acc.tfG += post.tfTags;
                                        acc.tfD += post.tfDesc;
                                    } else {
                                        acc.scoreAcc += post.singleScore;
                                    }
                                }
                            }
                        }
                        for (uint16_t docIdx : tlsAcc.touched) {
                            const auto &acc = tlsAcc.docs[docIdx];
                            if (acc.matchMask != fullMask) {
                                continue;
                            }
                            double totalScore = acc.scoreAcc;
                            if (acc.tfT != 0U || acc.tfG != 0U || acc.tfD != 0U) {
                                totalScore +=
                                    10.0 * bm25TfNorm(acc.tfT, seg->title_lengths[docIdx], 24.0) +
                                    5.0 * bm25TfNorm(acc.tfG, seg->tags_lengths[docIdx], 16.0) +
                                    1.0 * bm25TfNorm(acc.tfD, seg->desc_lengths[docIdx], 64.0);
                            }
                            pushHit(totalScore, seg->published_ids[docIdx]);
                        }
                    }
                }

                if (!topHits.empty()) {
                    std::sort(topHits.begin(), topHits.end(), betterThan);
                    const size_t startIdx =
                        std::min(static_cast<size_t>(safeOffset), topHits.size());
                    const size_t endIdx =
                        std::min(startIdx + static_cast<size_t>(cappedLimit), topHits.size());
                    if (startIdx >= endIdx) {
                        return {};
                    }

                    auto hydrateHits = [&](sqlite3 *db, sqlite3_stmt *stmtSingle,
                                           sqlite3_stmt *stmtBatch50) {
                        if (stmtBatch50) {
                            StmtResetGuard batchResetGuard{stmtBatch50};
                            std::vector<WorkshopItemRecord> fetched;
                            fetched.reserve(50U);
                            for (size_t pos = startIdx; pos < endIdx; pos += 50U) {
                                const size_t batchN = std::min<size_t>(50U, endIdx - pos);
                                sqlite3_reset(stmtBatch50);
                                sqlite3_clear_bindings(stmtBatch50);
                                bool bindOk = true;
                                for (size_t k = 0U; k < 50U; ++k) {
                                    const int rc =
                                        (k < batchN)
                                            ? sqlite3_bind_int64(stmtBatch50,
                                                                 static_cast<int>(k + 1U),
                                                                 static_cast<sqlite3_int64>(
                                                                     topHits[pos + k].pubId))
                                            : sqlite3_bind_null(stmtBatch50,
                                                                static_cast<int>(k + 1U));
                                    if (rc != SQLITE_OK) {
                                        bindOk = false;
                                        break;
                                    }
                                }
                                if (!bindOk) {
                                    continue;
                                }
                                fetched.clear();
                                int stepRc = SQLITE_OK;
                                while ((stepRc = sqlite3_step(stmtBatch50)) == SQLITE_ROW) {
                                    fetched.push_back(readRecordFromStmt(stmtBatch50));
                                }
                                if (stepRc != SQLITE_DONE) {
                                    std::fprintf(stderr,
                                                 "[CatalogIndexManager] BM25 batch hydrate failed "
                                                 "(rc=%d): %s\n",
                                                 stepRc, db ? sqlite3_errmsg(db) : "null db");
                                    continue;
                                }
                                std::sort(fetched.begin(), fetched.end(),
                                          [](const WorkshopItemRecord &a,
                                             const WorkshopItemRecord &b) noexcept {
                                              return a.published_file_id < b.published_file_id;
                                          });
                                for (size_t k = 0U; k < batchN; ++k) {
                                    const uint64_t targetId = topHits[pos + k].pubId;
                                    auto it = std::lower_bound(
                                        fetched.begin(), fetched.end(), targetId,
                                        [](const WorkshopItemRecord &r, uint64_t id) noexcept {
                                            return r.published_file_id < id;
                                        });
                                    if (it != fetched.end() && it->published_file_id == targetId) {
                                        WorkshopItemRecord rec = std::move(*it);
                                        rec.search_rank = topHits[pos + k].rankScore;
                                        results.push_back(std::move(rec));
                                    }
                                }
                            }
                            return;
                        }
                        if (!stmtSingle) {
                            return;
                        }
                        StmtResetGuard resetGuard{stmtSingle};
                        for (size_t idx = startIdx; idx < endIdx; ++idx) {
                            sqlite3_reset(stmtSingle);
                            sqlite3_clear_bindings(stmtSingle);
                            if (sqlite3_bind_int64(
                                    stmtSingle, 1,
                                    static_cast<sqlite3_int64>(topHits[idx].pubId)) != SQLITE_OK) {
                                continue;
                            }
                            const int rc = sqlite3_step(stmtSingle);
                            if (rc == SQLITE_ROW) {
                                WorkshopItemRecord rec = readRecordFromStmt(stmtSingle);
                                rec.search_rank = topHits[idx].rankScore;
                                results.push_back(std::move(rec));
                            } else if (rc != SQLITE_DONE) {
                                std::fprintf(
                                    stderr,
                                    "[CatalogIndexManager] BM25 hydrate failed (rc=%d): %s\n", rc,
                                    db ? sqlite3_errmsg(db) : "null db");
                            }
                        }
                    };

                    if (m_impl->readPool.isOpen()) {
                        auto lease = m_impl->readPool.acquire();
                        if (!lease.valid()) {
                            return {};
                        }
                        hydrateHits(lease.slot().db, lease.slot().stmtGetById,
                                    lease.slot().stmtGetBatch50);
                    } else {
                        std::lock_guard<std::mutex> lock(m_impl->dbMutex);
                        if (m_db && m_stmtGetById) {
                            hydrateHits(m_db, m_stmtGetById, nullptr);
                        }
                    }
                    if (!results.empty()) {
                        return results;
                    }
                }
            }
        }
    }

    auto runBm25 = [&](sqlite3 *db, sqlite3_stmt *stmt) {
        if (!stmt) {
            return;
        }
        StmtResetGuard resetGuard{stmt};
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        if (sqlite3_bind_text(stmt, 1, sanitized.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_int(stmt, 2, cappedLimit) != SQLITE_OK ||
            sqlite3_bind_int(stmt, 3, safeOffset) != SQLITE_OK) {
            return;
        }
        int stepRc = SQLITE_OK;
        while ((stepRc = sqlite3_step(stmt)) == SQLITE_ROW) {
            results.push_back(readRecordFromStmt(stmt));
        }
        if (stepRc != SQLITE_DONE) {
            std::fprintf(stderr, "[CatalogIndexManager] BM25 query failed (rc=%d): %s\n", stepRc,
                         db ? sqlite3_errmsg(db) : "null db");
            results.clear();
        }
    };

    if (m_impl->readPool.isOpen()) {
        auto lease = m_impl->readPool.acquire();
        if (!lease.valid()) {
            return {};
        }
        runBm25(lease.slot().db, lease.slot().stmtFtsBm25);
    } else {
        std::lock_guard<std::mutex> lock(m_impl->dbMutex);
        if (m_db && m_stmtFts5) {
            runBm25(m_db, m_stmtFts5);
        }
    }

    if (results.empty() && safeOffset == 0) {
        return searchFuzzy(query, 0, cappedLimit);
    }
    return results;
}

std::vector<WorkshopItemRecord>
CatalogIndexManager::queryByFilters(const CatalogFilter &filter) const
{
    if (!m_db) {
        return {};
    }

    const int lim = std::clamp(filter.limit > 0 ? filter.limit : 50, 1, kMaxQueryLimit);
    const int off = std::clamp(filter.offset >= 0 ? filter.offset : 0, 0, kMaxQueryOffset);

    int tagMode = 0;
    std::string jsonIdsBuf;
    if (filter.required_tag_mask != 0) {
        std::vector<uint64_t> matchedIds;
        if (filterByTagMaskBitmap(filter.required_tag_mask, &matchedIds) == 0) {
            return {};
        }
        if (m_impl->hasJsonEach && matchedIds.size() <= 1024u) {
            tagMode = 1;
            jsonIdsBuf = serializeIdsAsJson(matchedIds);
        } else {
            tagMode = 2;
        }
    }

    const bool hasType = !filter.type.empty() && filter.type != "all" && filter.type != "All";
    const bool hasTier = filter.compat_tier > 0;
    const bool hasInstall = filter.install_state_mask != 0;
    const uint32_t sortIdx = static_cast<uint32_t>(filter.sort_order) <= 5u
                                 ? static_cast<uint32_t>(filter.sort_order)
                                 : 0u;
    const uint32_t shapeKey = (hasType ? 1u : 0u) | (hasTier ? 2u : 0u) |
                              (static_cast<uint32_t>(tagMode) << 2) | (hasInstall ? 16u : 0u) |
                              (sortIdx << 5);

    auto executeWithDb = [&](sqlite3 *dbHandle) -> std::vector<WorkshopItemRecord> {
        sqlite3_stmt *stmt = m_impl->lookupCachedFilterStmt(dbHandle, shapeKey);
        if (!stmt) {
            const std::string sqlStr =
                buildFilterQuerySql(hasType, hasTier, tagMode, hasInstall, filter.sort_order);
            stmt = m_impl->cacheAndGetFilterStmt(dbHandle, shapeKey, sqlStr);
        }
        if (!stmt) {
            return {};
        }

        StmtResetGuard resetGuard{stmt};
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);

        int bindIdx = 1;
        bool bindOk = true;
        if (hasType) {
            bindOk = bindOk && sqlite3_bind_text(stmt, bindIdx++, filter.type.c_str(), -1,
                                                 SQLITE_TRANSIENT) == SQLITE_OK;
        }
        if (hasTier) {
            bindOk = bindOk && sqlite3_bind_int(stmt, bindIdx++,
                                                static_cast<int>(filter.compat_tier)) == SQLITE_OK;
        }
        if (tagMode == 1) {
            bindOk = bindOk && sqlite3_bind_text(stmt, bindIdx++, jsonIdsBuf.c_str(),
                                                 static_cast<int>(jsonIdsBuf.size()),
                                                 SQLITE_TRANSIENT) == SQLITE_OK;
        } else if (tagMode == 2) {
            bindOk = bindOk &&
                     sqlite3_bind_int64(stmt, bindIdx++,
                                        static_cast<sqlite3_int64>(filter.required_tag_mask)) ==
                         SQLITE_OK;
            bindOk = bindOk &&
                     sqlite3_bind_int64(stmt, bindIdx++,
                                        static_cast<sqlite3_int64>(filter.required_tag_mask)) ==
                         SQLITE_OK;
        }
        if (hasInstall) {
            bindOk = bindOk &&
                     sqlite3_bind_int64(stmt, bindIdx++,
                                        static_cast<sqlite3_int64>(filter.install_state_mask)) ==
                         SQLITE_OK;
        }
        bindOk = bindOk && sqlite3_bind_int(stmt, bindIdx++, lim) == SQLITE_OK;
        bindOk = bindOk && sqlite3_bind_int(stmt, bindIdx++, off) == SQLITE_OK;
        if (!bindOk) {
            return {};
        }

        std::vector<WorkshopItemRecord> rows;
        rows.reserve(std::min<size_t>(static_cast<size_t>(lim), kMaxReserveRows));
        int stepRc = SQLITE_OK;
        while ((stepRc = sqlite3_step(stmt)) == SQLITE_ROW) {
            rows.push_back(readRecordFromStmt(stmt));
        }
        if (stepRc != SQLITE_DONE) {
            std::fprintf(stderr, "[CatalogIndexManager] Filter query failed (rc=%d): %s\n", stepRc,
                         dbHandle ? sqlite3_errmsg(dbHandle) : "null db");
            rows.clear();
        }
        return rows;
    };

    if (m_impl->readPool.isOpen()) {
        auto lease = m_impl->readPool.acquire();
        if (!lease.valid()) {
            return {};
        }
        return executeWithDb(lease.slot().db);
    }

    std::lock_guard<std::mutex> lock(m_impl->dbMutex);
    return m_db ? executeWithDb(m_db) : std::vector<WorkshopItemRecord>{};
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

using CompatScannerPtr = std::unique_ptr<we_compat_scanner_t, decltype(&we_compat_scanner_destroy)>;
using CompatReportPtr = std::unique_ptr<we_compat_report_t, decltype(&we_compat_report_free)>;

struct ExistingItemMeta {
    uint64_t time_updated{0};
    std::string file_path;
};

std::string dirBasename(const std::filesystem::path &p)
{
    std::string norm = p.lexically_normal().string();
    while (norm.size() > 1 && norm.back() == '/') {
        norm.pop_back();
    }
    return std::filesystem::path(norm).filename().string();
}

bool buildLooseItemRecord(const std::filesystem::path &itemDir, uint64_t publishedFileId,
                          uint32_t storageBackend, const waywal_workshop_item_manifest_t *acfEntry,
                          IoUringStatxBatcher &batcher, size_t &statCounter,
                          WorkshopItemRecord &outRec,
                          we_compat_scanner_t *reusableScanner = nullptr,
                          we_compat_report_t *reusableReport = nullptr)
{
    const std::string baseName = dirBasename(itemDir);
    if (publishedFileId == 0) {
        publishedFileId = parseStrictBase10Uint64(baseName);
        if (publishedFileId == 0) {
            publishedFileId = fnv1a64(baseName);
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
    const bool hasProject = we_project_load_dir(itemDir.string().c_str(), &proj);
    if (hasProject) {
        rec.title = proj.title[0] ? proj.title : baseName;
        rec.type = proj.type[0] ? proj.type : "scene";
        if (proj.preview[0]) {
            rec.preview_path = resolveConfinedPreviewPath(itemDir, proj.preview);
        }
    } else {
        rec.title = baseName;
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

    CompatScannerPtr localScanner{nullptr, &we_compat_scanner_destroy};
    CompatReportPtr localReport{nullptr, &we_compat_report_free};
    we_compat_scanner_t *scanner = reusableScanner;
    we_compat_report_t *report = reusableReport;
    if (!scanner) {
        localScanner.reset(we_compat_scanner_create(true, true));
        scanner = localScanner.get();
    }
    if (!report) {
        localReport.reset(we_compat_report_alloc());
        report = localReport.get();
    }
    if (scanner && report) {
        // we_compat_scan_directory zeroes out_report on entry and treats scanner as read-only.
        if (we_compat_scan_directory(scanner, itemDir.string().c_str(), report)) {
            rec.compat_tier = static_cast<uint32_t>(report->tier);
            rec.compat_score = report->compatibility_score;
            if (rec.title.empty() && report->title[0]) {
                rec.title = report->title;
            }
        }
    }

    if (acfEntry && acfEntry->time_updated > 0 && acfEntry->size_bytes > 0) {
        rec.file_size_bytes = acfEntry->size_bytes;
        rec.time_updated = acfEntry->time_updated;
        rec.manifest_id = acfEntry->manifest_id;
        rec.install_state = acfEntry->state_flags | WAYWAL_UGC_STATE_INSTALLED;
    } else {
        std::vector<std::string> files;
        collectRegularFilesNoStat(itemDir.string(), files, statCounter);
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
    if (!m_db) {
        m_impl->lastReconcileStatCalls.store(0, std::memory_order_relaxed);
        return 0;
    }

    DIR *topDir = ::opendir(workshopDir.c_str());
    if (!topDir) {
        m_impl->lastReconcileStatCalls.store(0, std::memory_order_relaxed);
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

    // 2. Snapshot existing rows scoped to storageBackend and workshopDir in one pass.
    std::string dirPrefix = workshopDir.lexically_normal().string();
    while (dirPrefix.size() > 1 && dirPrefix.back() == '/') {
        dirPrefix.pop_back();
    }
    if (dirPrefix != "/") {
        dirPrefix.push_back('/');
    }

    std::unordered_map<uint64_t, ExistingItemMeta> existingItems;
    std::vector<uint64_t> existingScopedIds;
    {
        auto loadExisting = [&](sqlite3 *dbHandle) -> bool {
            if (!dbHandle) {
                return false;
            }
            sqlite3_stmt *stmt = nullptr;
            const char *sql =
                "SELECT published_file_id, time_updated, file_path FROM workshop_items WHERE "
                "storage_backend = ?;";
            if (sqlite3_prepare_v2(dbHandle, sql, -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
                std::fprintf(stderr,
                             "[CatalogIndexManager] reconcile loadExisting prepare failed: %s\n",
                             sqlite3_errmsg(dbHandle));
                return false;
            }
            if (sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(storageBackend)) !=
                SQLITE_OK) {
                sqlite3_finalize(stmt);
                return false;
            }
            int stepRc = SQLITE_OK;
            while ((stepRc = sqlite3_step(stmt)) == SQLITE_ROW) {
                const uint64_t id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
                const uint64_t ts = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
                const auto *fp = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
                const int fpLen = sqlite3_column_bytes(stmt, 2);
                std::string filePath =
                    (fp && fpLen > 0) ? std::string(fp, static_cast<size_t>(fpLen)) : std::string{};
                existingItems.emplace(id, ExistingItemMeta{ts, filePath});
                if (!filePath.empty()) {
                    const std::string normFp =
                        std::filesystem::path(filePath).lexically_normal().string();
                    if (normFp.starts_with(dirPrefix)) {
                        existingScopedIds.push_back(id);
                    }
                }
            }
            sqlite3_finalize(stmt);
            if (stepRc != SQLITE_DONE) {
                std::fprintf(
                    stderr,
                    "[CatalogIndexManager] reconcile loadExisting step failed (rc=%d): %s\n",
                    stepRc, sqlite3_errmsg(dbHandle));
                return false;
            }
            return true;
        };

        bool loaded = false;
        if (m_impl->readPool.isOpen()) {
            auto lease = m_impl->readPool.acquire();
            if (!lease.valid()) {
                ::closedir(topDir);
                m_impl->lastReconcileStatCalls.store(0, std::memory_order_relaxed);
                return 0;
            }
            loaded = loadExisting(lease.slot().db);
        } else {
            std::lock_guard<std::mutex> lock(m_impl->dbMutex);
            loaded = loadExisting(m_db);
        }
        if (!loaded) {
            ::closedir(topDir);
            m_impl->lastReconcileStatCalls.store(0, std::memory_order_relaxed);
            return 0;
        }
    }

    size_t indexedCount = 0;
    size_t statCalls = 0;
    IoUringStatxBatcher &statxBatcher = threadLocalStatxBatcher();
    std::vector<WorkshopItemRecord> dirtyRecords;
    std::unordered_set<uint64_t> seenIds;
    seenIds.reserve(existingScopedIds.size() + 32);
    // Defer the 246 KB compat report allocation until a loose item actually needs scanning.
    CompatScannerPtr compatScanner{nullptr, &we_compat_scanner_destroy};
    CompatReportPtr compatReport{nullptr, &we_compat_report_free};
    bool compatAllocAttempted = false;

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
            uint64_t id = parseStrictBase10Uint64(ent->d_name);
            if (id == 0) {
                id = fnv1a64(ent->d_name);
            }
            seenIds.insert(id);

            const waywal_workshop_item_manifest_t *acfPtr = nullptr;
            auto mfIt = manifestMap.find(id);
            if (mfIt != manifestMap.end()) {
                acfPtr = &mfIt->second;
            }

            const std::string entryPathStr = entryPath.string();
            // Zero-stat fast path requires identical time_updated and file_path so moved items
            // still re-index even when their manifest timestamp matches.
            if (acfPtr && acfPtr->time_updated > 0) {
                auto exIt = existingItems.find(id);
                if (exIt != existingItems.end() &&
                    exIt->second.time_updated == acfPtr->time_updated &&
                    !exIt->second.file_path.empty() && exIt->second.file_path == entryPathStr) {
                    ++indexedCount;
                    continue;
                }
            }

            if (!compatAllocAttempted) {
                compatScanner.reset(we_compat_scanner_create(true, true));
                compatReport.reset(we_compat_report_alloc());
                compatAllocAttempted = true;
            }

            WorkshopItemRecord rec;
            if (buildLooseItemRecord(entryPath, id, storageBackend, acfPtr, statxBatcher, statCalls,
                                     rec, compatScanner.get(), compatReport.get())) {
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
                const bool sigVerified = wwpkg_verify_signature(pkg, &isTrusted);
                if (man) {
                    const std::string_view manId(man->id, ::strnlen(man->id, sizeof(man->id)));
                    const std::string_view manSlug(man->slug,
                                                   ::strnlen(man->slug, sizeof(man->slug)));
                    const uint64_t id = !manId.empty() ? fnv1a64(manId) : fnv1a64(manSlug);
                    seenIds.insert(id);

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
                const std::string pathStr = entryPath.string();
                const uint64_t id = fnv1a64(pathStr);
                seenIds.insert(id);
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

    std::vector<uint64_t> staleIds;
    for (uint64_t exId : existingScopedIds) {
        if (!seenIds.contains(exId)) {
            staleIds.push_back(exId);
        }
    }
    if (!staleIds.empty()) {
        std::lock_guard<std::mutex> lock(m_impl->dbMutex);
        if (!m_db || !m_stmtDelete) {
            return indexedCount;
        }
        if (!execSql(m_db, "BEGIN IMMEDIATE TRANSACTION;", "reconcile.deleteBegin")) {
            return indexedCount;
        }
        bool deleteOk = true;
        for (uint64_t deadId : staleIds) {
            sqlite3_reset(m_stmtDelete);
            sqlite3_clear_bindings(m_stmtDelete);
            if (sqlite3_bind_int64(m_stmtDelete, 1, static_cast<sqlite3_int64>(deadId)) !=
                    SQLITE_OK ||
                sqlite3_step(m_stmtDelete) != SQLITE_DONE) {
                std::fprintf(stderr, "[CatalogIndexManager] Stale delete failed for %llu: %s\n",
                             static_cast<unsigned long long>(deadId), sqlite3_errmsg(m_db));
                deleteOk = false;
                break;
            }
        }
        sqlite3_reset(m_stmtDelete);
        if (deleteOk && !execSql(m_db, "COMMIT;", "reconcile.deleteCommit")) {
            deleteOk = false;
        }
        if (!deleteOk) {
            rollbackIfInTx(m_db, "reconcile.deleteRollback");
            return indexedCount;
        }
        m_impl->beginChunkEdit();
        for (uint64_t deadId : staleIds) {
            m_impl->removeSoaLocked(deadId);
        }
        m_impl->commitChunkEdit();
    }

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

    IoUringStatxBatcher &batcher = threadLocalStatxBatcher();
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
