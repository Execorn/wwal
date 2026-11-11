// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/queue/download_manager.hpp"

#include "waywal/wwpkg.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#else
struct open_how {
    uint64_t flags;
    uint64_t mode;
    uint64_t resolve;
};
#define RESOLVE_NO_MAGICLINKS 0x02ULL
#define RESOLVE_NO_SYMLINKS   0x04ULL
#define RESOLVE_BENEATH       0x08ULL
#endif

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QVariant>

namespace wwal::studio {

namespace fs = std::filesystem;

namespace {

/* ========================================================================= */
/* Runtime Zstd Stream Loader for Confined Package Extraction                */
/* ========================================================================= */

struct ZstdStreamApi {
    using ZSTD_DStream = void;
    struct InBuffer {
        const void *src;
        size_t size;
        size_t pos;
    };
    struct OutBuffer {
        void *dst;
        size_t size;
        size_t pos;
    };

    ZSTD_DStream *(*createDStream)(){nullptr};
    size_t (*freeDStream)(ZSTD_DStream *){nullptr};
    size_t (*initDStream)(ZSTD_DStream *){nullptr};
    size_t (*decompressStream)(ZSTD_DStream *, OutBuffer *, InBuffer *){nullptr};
    unsigned (*isError)(size_t){nullptr};
    bool available{false};

    static const ZstdStreamApi &instance()
    {
        static ZstdStreamApi api;
        static std::once_flag once;
        std::call_once(once, []() {
            void *h = ::dlopen("libzstd.so.1", RTLD_LAZY | RTLD_LOCAL | RTLD_NODELETE);
            if (!h) {
                h = ::dlopen("libzstd.so", RTLD_LAZY | RTLD_LOCAL | RTLD_NODELETE);
            }
            if (!h) {
                return;
            }
            api.createDStream =
                reinterpret_cast<decltype(api.createDStream)>(::dlsym(h, "ZSTD_createDStream"));
            api.freeDStream =
                reinterpret_cast<decltype(api.freeDStream)>(::dlsym(h, "ZSTD_freeDStream"));
            api.initDStream =
                reinterpret_cast<decltype(api.initDStream)>(::dlsym(h, "ZSTD_initDStream"));
            api.decompressStream = reinterpret_cast<decltype(api.decompressStream)>(
                ::dlsym(h, "ZSTD_decompressStream"));
            api.isError = reinterpret_cast<decltype(api.isError)>(::dlsym(h, "ZSTD_isError"));
            api.available = (api.createDStream && api.freeDStream && api.initDStream &&
                             api.decompressStream && api.isError);
        });
        return api;
    }
};

struct UstarHeader {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};
static_assert(sizeof(UstarHeader) == 512);

bool parseOctalU64(const char *field, size_t len, uint64_t &out)
{
    out = 0;
    size_t i = 0;
    while (i < len && (field[i] == ' ' || field[i] == '\0')) {
        ++i;
    }
    if (i == len) {
        return true;
    }
    for (; i < len && field[i] != '\0' && field[i] != ' '; ++i) {
        if (field[i] < '0' || field[i] > '7') {
            return false;
        }
        uint64_t digit = static_cast<uint64_t>(field[i] - '0');
        if (out > (UINT64_MAX >> 3)) {
            return false;
        }
        out = (out << 3) | digit;
    }
    return true;
}

bool sanitizeRelativeArchivePath(std::string_view raw, std::vector<std::string> &components,
                                 std::string &normalized)
{
    components.clear();
    normalized.clear();
    if (raw.empty() || raw.front() == '/' || raw.find('\\') != std::string_view::npos ||
        raw.find('\0') != std::string_view::npos) {
        return false;
    }

    size_t pos = 0;
    while (pos < raw.size()) {
        size_t slash = raw.find('/', pos);
        std::string_view part =
            (slash == std::string_view::npos) ? raw.substr(pos) : raw.substr(pos, slash - pos);
        if (part.empty() || part == ".") {
            if (slash == std::string_view::npos) {
                break;
            }
            pos = slash + 1;
            continue;
        }
        if (part == "..") {
            return false;
        }
        components.emplace_back(part);
        if (slash == std::string_view::npos) {
            break;
        }
        pos = slash + 1;
    }

    if (components.empty()) {
        return false;
    }

    for (size_t i = 0; i < components.size(); ++i) {
        if (i > 0) {
            normalized.push_back('/');
        }
        normalized += components[i];
    }
    return true;
}

int openConfinedWriteFd(int rootDirFd, const std::vector<std::string> &components,
                        const std::string &normalizedRel)
{
    int curDirFd = ::dup(rootDirFd);
    if (curDirFd < 0) {
        return -1;
    }

    for (size_t i = 0; i + 1 < components.size(); ++i) {
        const char *comp = components[i].c_str();
        if (::mkdirat(curDirFd, comp, 0755) != 0 && errno != EEXIST) {
            ::close(curDirFd);
            return -1;
        }
        int nextFd = ::openat(curDirFd, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(curDirFd);
        if (nextFd < 0) {
            return -1;
        }
        curDirFd = nextFd;
    }

#ifdef SYS_openat2
    open_how how{};
    how.flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW;
    how.mode = 0644;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
    long fd2 = ::syscall(SYS_openat2, rootDirFd, normalizedRel.c_str(), &how, sizeof(how));
    if (fd2 >= 0) {
        ::close(curDirFd);
        return static_cast<int>(fd2);
    }
    if (errno != ENOSYS && errno != EINVAL) {
        ::close(curDirFd);
        return -1;
    }
#endif

    int outFd = ::openat(curDirFd, components.back().c_str(),
                         O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    ::close(curDirFd);
    return outFd;
}

bool extractUstarBufferConfined(const uint8_t *tarData, size_t tarLen, const fs::path &destDir)
{
    constexpr size_t kMaxExtractedBytes = 512ULL * 1024ULL * 1024ULL;
    constexpr size_t kMaxExtractedFiles = 4096;

    std::error_code ec;
    fs::create_directories(destDir, ec);
    if (ec) {
        return false;
    }

    int rootDirFd = ::open(destDir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (rootDirFd < 0) {
        return false;
    }

    size_t offset = 0;
    size_t totalBytes = 0;
    size_t fileCount = 0;
    bool ok = true;

    while (offset + 512 <= tarLen) {
        const uint8_t *blk = tarData + offset;
        bool allZero = true;
        for (size_t i = 0; i < 512; ++i) {
            if (blk[i] != 0) {
                allZero = false;
                break;
            }
        }
        if (allZero) {
            break;
        }

        const auto *hdr = reinterpret_cast<const UstarHeader *>(blk);
        uint64_t entrySize = 0;
        if (!parseOctalU64(hdr->size, sizeof(hdr->size), entrySize)) {
            ok = false;
            break;
        }

        if (entrySize > tarLen - offset - 512) {
            ok = false;
            break;
        }

        std::string rawName;
        size_t prefixLen = ::strnlen(hdr->prefix, sizeof(hdr->prefix));
        size_t nameLen = ::strnlen(hdr->name, sizeof(hdr->name));
        if (prefixLen > 0) {
            rawName.assign(hdr->prefix, prefixLen);
            rawName.push_back('/');
        }
        rawName.append(hdr->name, nameLen);

        char tf = hdr->typeflag;
        if (tf == '5') {
            if (entrySize != 0) {
                ok = false;
                break;
            }
            offset += 512;
            continue;
        }

        if (tf != '0' && tf != '\0') {
            // Reject symlinks, hardlinks, FIFOs, and device nodes inside archives.
            ok = false;
            break;
        }

        std::vector<std::string> comps;
        std::string normRel;
        if (!sanitizeRelativeArchivePath(rawName, comps, normRel)) {
            ok = false;
            break;
        }

        if (++fileCount > kMaxExtractedFiles || totalBytes + entrySize > kMaxExtractedBytes) {
            ok = false;
            break;
        }
        totalBytes += static_cast<size_t>(entrySize);

        int outFd = openConfinedWriteFd(rootDirFd, comps, normRel);
        if (outFd < 0) {
            ok = false;
            break;
        }

        const uint8_t *payload = tarData + offset + 512;
        size_t writtenTotal = 0;
        while (writtenTotal < entrySize) {
            ssize_t w = ::write(outFd, payload + writtenTotal,
                                static_cast<size_t>(entrySize) - writtenTotal);
            if (w < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ok = false;
                break;
            }
            writtenTotal += static_cast<size_t>(w);
        }
        ::close(outFd);
        if (!ok) {
            break;
        }

        uint64_t padded = ((entrySize + 511ULL) / 512ULL) * 512ULL;
        if (padded > tarLen - offset - 512) {
            ok = false;
            break;
        }
        offset += 512 + static_cast<size_t>(padded);
    }

    ::close(rootDirFd);
    return ok && fileCount > 0;
}

bool extractVerifiedWwpkgAll(wwpkg_package_t *pkg, const std::string &pkgPath,
                             const fs::path &destDir)
{
    using ExtractAllFn = bool (*)(wwpkg_package_t *, const char *);
    static ExtractAllFn dynamicExtractFn =
        reinterpret_cast<ExtractAllFn>(::dlsym(RTLD_DEFAULT, "wwpkg_extract_all"));
    if (dynamicExtractFn) {
        std::error_code ec;
        fs::remove_all(destDir, ec);
        fs::create_directories(destDir, ec);
        if (!ec && dynamicExtractFn(pkg, destDir.c_str())) {
            return true;
        }
    }

    std::error_code ec;
    fs::remove_all(destDir, ec);

    std::ifstream ifs(pkgPath, std::ios::binary | std::ios::ate);
    if (!ifs) {
        return false;
    }
    auto endPos = ifs.tellg();
    if (endPos <= 0 || endPos > static_cast<std::streamoff>(256ULL * 1024ULL * 1024ULL)) {
        return false;
    }
    size_t fileLen = static_cast<size_t>(endPos);
    ifs.seekg(0, std::ios::beg);
    std::vector<uint8_t> raw(fileLen);
    if (!ifs.read(reinterpret_cast<char *>(raw.data()), static_cast<std::streamsize>(fileLen))) {
        return false;
    }

    // Advance past any outer Zstd skippable frames (0x184D2A50..0x184D2A5F).
    size_t payloadOff = 0;
    while (payloadOff + 8 <= raw.size()) {
        uint32_t magic = static_cast<uint32_t>(raw[payloadOff]) |
                         (static_cast<uint32_t>(raw[payloadOff + 1]) << 8) |
                         (static_cast<uint32_t>(raw[payloadOff + 2]) << 16) |
                         (static_cast<uint32_t>(raw[payloadOff + 3]) << 24);
        if ((magic & 0xFFFFFFF0u) != 0x184D2A50u) {
            break;
        }
        uint32_t frameLen = static_cast<uint32_t>(raw[payloadOff + 4]) |
                            (static_cast<uint32_t>(raw[payloadOff + 5]) << 8) |
                            (static_cast<uint32_t>(raw[payloadOff + 6]) << 16) |
                            (static_cast<uint32_t>(raw[payloadOff + 7]) << 24);
        if (frameLen > raw.size() - payloadOff - 8) {
            return false;
        }
        payloadOff += 8 + static_cast<size_t>(frameLen);
    }

    if (payloadOff + 512 <= raw.size() &&
        std::memcmp(raw.data() + payloadOff + 257, "ustar", 5) == 0) {
        return extractUstarBufferConfined(raw.data() + payloadOff, raw.size() - payloadOff,
                                          destDir);
    }

    const auto &zstd = ZstdStreamApi::instance();
    if (!zstd.available || payloadOff + 4 > raw.size()) {
        return false;
    }

    constexpr size_t kMaxDecompressed = 512ULL * 1024ULL * 1024ULL;
    void *dstream = zstd.createDStream();
    if (!dstream) {
        return false;
    }
    zstd.initDStream(dstream);

    std::vector<uint8_t> tarBuf;
    tarBuf.reserve(std::min<size_t>(raw.size() * 4, 4 * 1024 * 1024));
    std::array<uint8_t, 65536> outChunk{};

    ZstdStreamApi::InBuffer inBuf{raw.data() + payloadOff, raw.size() - payloadOff, 0};
    bool streamOk = true;
    while (inBuf.pos < inBuf.size) {
        ZstdStreamApi::OutBuffer outBuf{outChunk.data(), outChunk.size(), 0};
        size_t rc = zstd.decompressStream(dstream, &outBuf, &inBuf);
        if (zstd.isError(rc)) {
            streamOk = false;
            break;
        }
        if (tarBuf.size() + outBuf.pos > kMaxDecompressed) {
            streamOk = false;
            break;
        }
        tarBuf.insert(tarBuf.end(), outChunk.data(), outChunk.data() + outBuf.pos);
        if (rc == 0 && inBuf.pos < inBuf.size) {
            // Re-init for concatenated Zstd frames (e.g. dual-frame metadata + payload archives).
            zstd.initDStream(dstream);
        }
    }
    zstd.freeDStream(dstream);

    if (!streamOk || tarBuf.size() < 512) {
        return false;
    }

    return extractUstarBufferConfined(tarBuf.data(), tarBuf.size(), destDir);
}

bool envLoopbackHttpAllowed()
{
    const char *env = std::getenv("WAYWAL_ALLOW_LOOPBACK_HTTP");
    return env != nullptr && std::strcmp(env, "1") == 0;
}

bool parseSchemeAndHost(std::string_view url, std::string &schemeOut, std::string &hostOut)
{
    auto pos = url.find("://");
    if (pos == std::string_view::npos || pos == 0) {
        return false;
    }
    schemeOut.assign(url.substr(0, pos));
    for (char &ch : schemeOut) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    std::string_view rest = url.substr(pos + 3);
    size_t authEnd = rest.find_first_of("/?#");
    std::string_view authority =
        (authEnd == std::string_view::npos) ? rest : rest.substr(0, authEnd);
    auto atPos = authority.rfind('@');
    if (atPos != std::string_view::npos) {
        authority = authority.substr(atPos + 1);
    }
    if (authority.empty()) {
        return false;
    }

    if (authority.front() == '[') {
        auto closeBracket = authority.find(']');
        if (closeBracket == std::string_view::npos) {
            return false;
        }
        hostOut.assign(authority.substr(0, closeBracket + 1));
    } else {
        auto colon = authority.find(':');
        hostOut.assign((colon == std::string_view::npos) ? authority : authority.substr(0, colon));
    }
    for (char &ch : hostOut) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return !hostOut.empty();
}

bool isLoopbackHost(std::string_view host)
{
    return host == "127.0.0.1" || host == "localhost" || host == "[::1]";
}

bool isAllowedDownloadUrl(std::string_view url, bool allowLoopbackHttp)
{
    std::string scheme;
    std::string host;
    if (!parseSchemeAndHost(url, scheme, host)) {
        return false;
    }
    if (scheme == "https") {
        return true;
    }
    if (scheme == "http" && allowLoopbackHttp && isLoopbackHost(host)) {
        return true;
    }
    return false;
}

std::string sanitizePackageFilenameStem(std::string id)
{
    for (char &ch : id) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' ||
              ch == '.')) {
            ch = '_';
        }
    }
    while (!id.empty() && id.front() == '.') {
        id.erase(id.begin());
    }
    if (id.empty()) {
        id = "package";
    }
    return id;
}

} // namespace

/* ========================================================================= */
/* Token Bucket Bandwidth Governor                                           */
/* ========================================================================= */

class TokenBucket {
  public:
    TokenBucket() = default;

    void setLimit(uint64_t bytes_per_sec)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_rate_limit = bytes_per_sec;
        ++m_epoch;
        m_last_refill = std::chrono::steady_clock::now();
        if (m_rate_limit > 0) {
            uint64_t tenth = static_cast<uint64_t>(m_rate_limit * 0.1);
            uint64_t burst_cap =
                std::max<uint64_t>(65536ULL, std::min<uint64_t>(m_rate_limit, tenth));
            m_tokens = std::min(m_tokens, burst_cap);
        }
        m_cv.notify_all();
    }

    [[nodiscard]] uint64_t limit() const noexcept
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_rate_limit;
    }

    [[nodiscard]] uint64_t epoch() const noexcept
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_epoch;
    }

    void pause()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_paused = true;
        ++m_epoch;
    }

    void resume()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_paused = false;
        ++m_epoch;
        m_last_refill = std::chrono::steady_clock::now();
        if (m_rate_limit > 0) {
            uint64_t tenth = static_cast<uint64_t>(m_rate_limit * 0.1);
            uint64_t burst_cap =
                std::max<uint64_t>(65536ULL, std::min<uint64_t>(m_rate_limit, tenth));
            m_tokens = std::min(m_tokens, burst_cap);
        }
        m_cv.notify_all();
    }

    [[nodiscard]] bool isPaused() const noexcept
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_paused;
    }

    void wakeAll() { m_cv.notify_all(); }

    void shutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shutdown = true;
        m_cv.notify_all();
    }

    void resetForStart()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shutdown = false;
    }

    // Non-blocking check for the single-threaded epoll reactor so write callbacks never sleep.
    bool tryConsume(uint64_t bytes, std::chrono::steady_clock::time_point &outNextReady,
                    bool &outPaused, uint64_t &outEpoch)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        outEpoch = m_epoch;
        if (m_paused) {
            outPaused = true;
            outNextReady = std::chrono::steady_clock::time_point::max();
            return false;
        }
        outPaused = false;
        if (bytes == 0 || m_rate_limit == 0) {
            return true;
        }

        refillLocked(bytes);
        if (m_tokens >= bytes) {
            m_tokens -= bytes;
            return true;
        }

        uint64_t deficit = bytes - m_tokens;
        double wait_sec = static_cast<double>(deficit) / static_cast<double>(m_rate_limit);
        auto wait_dur = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(std::max(0.0005, wait_sec)));
        outNextReady = std::chrono::steady_clock::now() + wait_dur;
        return false;
    }

    bool consume(uint64_t bytes, const std::atomic<bool> &cancel_flag)
    {
        if (bytes == 0) {
            return true;
        }

        std::unique_lock<std::mutex> lock(m_mutex);
        while (bytes > 0) {
            if (m_shutdown || cancel_flag.load(std::memory_order_relaxed)) {
                return false;
            }

            if (m_paused) {
                m_cv.wait(lock, [&] {
                    return !m_paused || m_shutdown || cancel_flag.load(std::memory_order_relaxed);
                });
                if (m_shutdown || cancel_flag.load(std::memory_order_relaxed)) {
                    return false;
                }
            }

            if (m_rate_limit == 0) {
                return true;
            }

            refillLocked(65536ULL);

            if (m_tokens >= bytes) {
                m_tokens -= bytes;
                return true;
            }

            uint64_t available = m_tokens;
            bytes -= available;
            m_tokens = 0;

            double wait_sec = static_cast<double>(bytes) / static_cast<double>(m_rate_limit);
            auto wait_dur = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::duration<double>(wait_sec));

            m_cv.wait_for(lock, wait_dur, [&] {
                return m_paused || m_shutdown || m_rate_limit == 0 ||
                       cancel_flag.load(std::memory_order_relaxed);
            });
        }
        return true;
    }

  private:
    void refillLocked(uint64_t min_burst)
    {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - m_last_refill).count();
        if (elapsed > 0.0 && m_rate_limit > 0) {
            uint64_t delta_tokens = static_cast<uint64_t>(elapsed * m_rate_limit);
            if (delta_tokens > 0) {
                uint64_t tenth = static_cast<uint64_t>(m_rate_limit * 0.1);
                uint64_t burst_cap = std::max<uint64_t>(
                    min_burst,
                    std::max<uint64_t>(65536ULL, std::min<uint64_t>(m_rate_limit, tenth)));
                m_tokens = std::min(m_tokens + delta_tokens, burst_cap);
                auto consumed_time =
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(static_cast<double>(delta_tokens) /
                                                      static_cast<double>(m_rate_limit)));
                m_last_refill = std::min(now, m_last_refill + consumed_time);
            }
        }
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    uint64_t m_rate_limit{0};
    uint64_t m_tokens{0};
    uint64_t m_epoch{1};
    std::chrono::steady_clock::time_point m_last_refill{std::chrono::steady_clock::now()};
    bool m_paused{false};
    bool m_shutdown{false};
};

/* ========================================================================= */
/* EWMA Speed and ETA Estimator                                              */
/* ========================================================================= */

class SpeedEstimator {
  public:
    void reset(uint64_t initial_bytes = 0)
    {
        m_smoothed_speed = 0.0;
        m_last_time = std::chrono::steady_clock::now();
        m_last_bytes = initial_bytes;
    }

    void update(uint64_t current_bytes, uint64_t total_bytes, float &out_speed, uint32_t &out_eta)
    {
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - m_last_time).count();
        if (dt >= 0.05) {
            uint64_t delta_bytes =
                (current_bytes >= m_last_bytes) ? (current_bytes - m_last_bytes) : 0;
            double inst_speed = static_cast<double>(delta_bytes) / dt;
            if (m_smoothed_speed <= 0.0) {
                m_smoothed_speed = inst_speed;
            } else {
                m_smoothed_speed = 0.2 * inst_speed + 0.8 * m_smoothed_speed;
            }
            m_last_time = now;
            m_last_bytes = current_bytes;
        }

        out_speed = static_cast<float>(m_smoothed_speed);
        if (m_smoothed_speed > 10.0 && total_bytes > current_bytes) {
            out_eta = static_cast<uint32_t>((total_bytes - current_bytes) / m_smoothed_speed);
        } else {
            out_eta = 0;
        }
    }

  private:
    double m_smoothed_speed{0.0};
    std::chrono::steady_clock::time_point m_last_time{std::chrono::steady_clock::now()};
    uint64_t m_last_bytes{0};
};

/* ========================================================================= */
/* Backend A: Steamworks SDK Lazy Dynamic Loader                             */
/* ========================================================================= */

class SteamworksBackend {
  public:
    SteamworksBackend() = default;
    ~SteamworksBackend() { shutdown(); }

    bool initialize(uint32_t appId = 431960)
    {
        if (m_initialized) {
            return true;
        }

        const char *home = std::getenv("HOME");
        std::vector<std::string> candidates;
        if (home) {
            candidates.push_back(std::string(home) +
                                 "/.local/share/Steam/ubuntu12_32/steam-runtime/usr/lib/"
                                 "x86_64-linux-gnu/libsteam_api.so");
            candidates.push_back(
                std::string(home) +
                "/.steam/steam/ubuntu12_32/steam-runtime/usr/lib/x86_64-linux-gnu/libsteam_api.so");
            candidates.push_back(std::string(home) +
                                 "/.var/app/com.valvesoftware.Steam/.local/share/Steam/ubuntu12_32/"
                                 "steam-runtime/usr/lib/x86_64-linux-gnu/libsteam_api.so");
        }
        candidates.push_back("libsteam_api.so");

        for (const auto &path : candidates) {
            m_libHandle = ::dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL | RTLD_NODELETE);
            if (m_libHandle) {
                break;
            }
        }

        if (!m_libHandle) {
            return false;
        }

        pfn_SteamAPI_InitFlat =
            reinterpret_cast<SteamAPI_InitFlat_fn>(::dlsym(m_libHandle, "SteamAPI_InitFlat"));
        pfn_SteamAPI_Shutdown =
            reinterpret_cast<SteamAPI_Shutdown_fn>(::dlsym(m_libHandle, "SteamAPI_Shutdown"));
        pfn_SteamAPI_IsSteamRunning = reinterpret_cast<SteamAPI_IsSteamRunning_fn>(
            ::dlsym(m_libHandle, "SteamAPI_IsSteamRunning"));
        pfn_SteamAPI_RunCallbacks = reinterpret_cast<SteamAPI_RunCallbacks_fn>(
            ::dlsym(m_libHandle, "SteamAPI_RunCallbacks"));
        pfn_SteamAPI_SteamUGC_v016 = reinterpret_cast<SteamAPI_SteamUGC_v016_fn>(
            ::dlsym(m_libHandle, "SteamAPI_SteamUGC_v016"));
        pfn_SteamAPI_ISteamUGC_SubscribeItem =
            reinterpret_cast<SteamAPI_ISteamUGC_SubscribeItem_fn>(
                ::dlsym(m_libHandle, "SteamAPI_ISteamUGC_SubscribeItem"));
        pfn_SteamAPI_ISteamUGC_DownloadItem = reinterpret_cast<SteamAPI_ISteamUGC_DownloadItem_fn>(
            ::dlsym(m_libHandle, "SteamAPI_ISteamUGC_DownloadItem"));
        pfn_SteamAPI_ISteamUGC_GetItemDownloadInfo =
            reinterpret_cast<SteamAPI_ISteamUGC_GetItemDownloadInfo_fn>(
                ::dlsym(m_libHandle, "SteamAPI_ISteamUGC_GetItemDownloadInfo"));
        pfn_SteamAPI_ISteamUGC_GetItemInstallInfo =
            reinterpret_cast<SteamAPI_ISteamUGC_GetItemInstallInfo_fn>(
                ::dlsym(m_libHandle, "SteamAPI_ISteamUGC_GetItemInstallInfo"));
        pfn_SteamAPI_ISteamUGC_GetItemState = reinterpret_cast<SteamAPI_ISteamUGC_GetItemState_fn>(
            ::dlsym(m_libHandle, "SteamAPI_ISteamUGC_GetItemState"));

        if (!pfn_SteamAPI_InitFlat || !pfn_SteamAPI_IsSteamRunning ||
            !pfn_SteamAPI_IsSteamRunning()) {
            shutdown();
            return false;
        }

        std::string appIdStr = std::to_string(appId);
        ::setenv("SteamAppId", appIdStr.c_str(), 0);

        // SteamAPI_InitFlat returns ESteamAPIInitResult where k_ESteamAPIInitResult_OK == 0.
        char err[1024] = {0};
        if (pfn_SteamAPI_InitFlat(err) != 0) {
            shutdown();
            return false;
        }

        if (pfn_SteamAPI_SteamUGC_v016) {
            m_ugc = pfn_SteamAPI_SteamUGC_v016();
        }

        if (!m_ugc) {
            shutdown();
            return false;
        }

        m_initialized = true;
        return true;
    }

    void shutdown()
    {
        if (m_initialized && pfn_SteamAPI_Shutdown) {
            pfn_SteamAPI_Shutdown();
        }
        m_libHandle = nullptr;
        m_ugc = nullptr;
        m_initialized = false;
    }

    [[nodiscard]] bool isRunning() const noexcept
    {
        if (!m_initialized || !pfn_SteamAPI_IsSteamRunning) {
            return false;
        }
        return pfn_SteamAPI_IsSteamRunning();
    }

    void runCallbacks()
    {
        if (m_initialized && pfn_SteamAPI_RunCallbacks) {
            pfn_SteamAPI_RunCallbacks();
        }
    }

    bool subscribeItem(uint64_t publishedFileId)
    {
        if (!m_initialized || !m_ugc || !pfn_SteamAPI_ISteamUGC_SubscribeItem) {
            return false;
        }
        pfn_SteamAPI_ISteamUGC_SubscribeItem(m_ugc, publishedFileId);
        return true;
    }

    bool downloadItem(uint64_t publishedFileId, bool highPriority = true)
    {
        if (!m_initialized || !m_ugc || !pfn_SteamAPI_ISteamUGC_DownloadItem) {
            return false;
        }
        return pfn_SteamAPI_ISteamUGC_DownloadItem(m_ugc, publishedFileId, highPriority);
    }

    bool getItemDownloadInfo(uint64_t publishedFileId, uint64_t *punBytesDownloaded,
                             uint64_t *punBytesTotal)
    {
        if (!m_initialized || !m_ugc || !pfn_SteamAPI_ISteamUGC_GetItemDownloadInfo) {
            return false;
        }
        return pfn_SteamAPI_ISteamUGC_GetItemDownloadInfo(m_ugc, publishedFileId,
                                                          punBytesDownloaded, punBytesTotal);
    }

    uint32_t getItemState(uint64_t publishedFileId)
    {
        if (!m_initialized || !m_ugc || !pfn_SteamAPI_ISteamUGC_GetItemState) {
            return 0;
        }
        return pfn_SteamAPI_ISteamUGC_GetItemState(m_ugc, publishedFileId);
    }

    bool getItemInstallInfo(uint64_t publishedFileId, uint64_t *punSizeOnDisk, char *pchFolder,
                            uint32_t cchFolderSize, uint32_t *punTimeStamp)
    {
        if (!m_initialized || !m_ugc || !pfn_SteamAPI_ISteamUGC_GetItemInstallInfo) {
            return false;
        }
        return pfn_SteamAPI_ISteamUGC_GetItemInstallInfo(m_ugc, publishedFileId, punSizeOnDisk,
                                                         pchFolder, cchFolderSize, punTimeStamp);
    }

  private:
    using SteamAPI_InitFlat_fn = int (*)(char *pszErr);
    using SteamAPI_Shutdown_fn = void (*)();
    using SteamAPI_IsSteamRunning_fn = bool (*)();
    using SteamAPI_RunCallbacks_fn = void (*)();
    using SteamAPI_SteamUGC_v016_fn = void *(*)();
    using SteamAPI_ISteamUGC_SubscribeItem_fn = void (*)(void *self, uint64_t nPublishedFileId);
    using SteamAPI_ISteamUGC_DownloadItem_fn = bool (*)(void *self, uint64_t nPublishedFileId,
                                                        bool bHighPriority);
    using SteamAPI_ISteamUGC_GetItemDownloadInfo_fn = bool (*)(void *self,
                                                               uint64_t nPublishedFileId,
                                                               uint64_t *punBytesDownloaded,
                                                               uint64_t *punBytesTotal);
    using SteamAPI_ISteamUGC_GetItemInstallInfo_fn = bool (*)(void *self, uint64_t nPublishedFileId,
                                                              uint64_t *punSizeOnDisk,
                                                              char *pchFolder,
                                                              uint32_t cchFolderSize,
                                                              uint32_t *punTimeStamp);
    using SteamAPI_ISteamUGC_GetItemState_fn = uint32_t (*)(void *self, uint64_t nPublishedFileId);

    void *m_libHandle{nullptr};
    void *m_ugc{nullptr};
    bool m_initialized{false};

    SteamAPI_InitFlat_fn pfn_SteamAPI_InitFlat{nullptr};
    SteamAPI_Shutdown_fn pfn_SteamAPI_Shutdown{nullptr};
    SteamAPI_IsSteamRunning_fn pfn_SteamAPI_IsSteamRunning{nullptr};
    SteamAPI_RunCallbacks_fn pfn_SteamAPI_RunCallbacks{nullptr};
    SteamAPI_SteamUGC_v016_fn pfn_SteamAPI_SteamUGC_v016{nullptr};
    SteamAPI_ISteamUGC_SubscribeItem_fn pfn_SteamAPI_ISteamUGC_SubscribeItem{nullptr};
    SteamAPI_ISteamUGC_DownloadItem_fn pfn_SteamAPI_ISteamUGC_DownloadItem{nullptr};
    SteamAPI_ISteamUGC_GetItemDownloadInfo_fn pfn_SteamAPI_ISteamUGC_GetItemDownloadInfo{nullptr};
    SteamAPI_ISteamUGC_GetItemInstallInfo_fn pfn_SteamAPI_ISteamUGC_GetItemInstallInfo{nullptr};
    SteamAPI_ISteamUGC_GetItemState_fn pfn_SteamAPI_ISteamUGC_GetItemState{nullptr};
};

/* ========================================================================= */
/* Curl Dynamic Binding Helper (easy + multi_socket_action ABI)              */
/* ========================================================================= */

struct CurlBinding {
    using CURL = void;
    using CURLM = void;
    using CURLcode = int;
    using CURLMcode = int;
    using curl_socket_t = int;

    struct curl_slist {
        char *data;
        struct curl_slist *next;
    };

    struct CurlMsg {
        int msg;
        CURL *easy_handle;
        union {
            void *whatever;
            CURLcode result;
        } data;
    };

    using socket_callback_fn = int (*)(CURL *easy, curl_socket_t s, int what, void *userp,
                                       void *socketp);
    using timer_callback_fn = int (*)(CURLM *multi, long timeout_ms, void *userp);

    using easy_init_fn = CURL *(*)();
    using easy_cleanup_fn = void (*)(CURL *);
    using easy_setopt_fn = CURLcode (*)(CURL *, int, ...);
    using easy_perform_fn = CURLcode (*)(CURL *);
    using easy_getinfo_fn = CURLcode (*)(CURL *, int, ...);
    using easy_pause_fn = CURLcode (*)(CURL *, int);
    using easy_strerror_fn = const char *(*)(CURLcode);
    using slist_append_fn = struct curl_slist *(*)(struct curl_slist *, const char *);
    using slist_free_all_fn = void (*)(struct curl_slist *);

    using multi_init_fn = CURLM *(*)();
    using multi_cleanup_fn = CURLMcode (*)(CURLM *);
    using multi_add_handle_fn = CURLMcode (*)(CURLM *, CURL *);
    using multi_remove_handle_fn = CURLMcode (*)(CURLM *, CURL *);
    using multi_socket_action_fn = CURLMcode (*)(CURLM *, curl_socket_t, int, int *);
    using multi_info_read_fn = CurlMsg *(*)(CURLM *, int *);
    using multi_setopt_fn = CURLMcode (*)(CURLM *, int, ...);

    void *handle{nullptr};
    easy_init_fn easy_init{nullptr};
    easy_cleanup_fn easy_cleanup{nullptr};
    easy_setopt_fn easy_setopt{nullptr};
    easy_perform_fn easy_perform{nullptr};
    easy_getinfo_fn easy_getinfo{nullptr};
    easy_pause_fn easy_pause{nullptr};
    easy_strerror_fn easy_strerror{nullptr};
    slist_append_fn slist_append{nullptr};
    slist_free_all_fn slist_free_all{nullptr};

    multi_init_fn multi_init{nullptr};
    multi_cleanup_fn multi_cleanup{nullptr};
    multi_add_handle_fn multi_add_handle{nullptr};
    multi_remove_handle_fn multi_remove_handle{nullptr};
    multi_socket_action_fn multi_socket_action{nullptr};
    multi_info_read_fn multi_info_read{nullptr};
    multi_setopt_fn multi_setopt{nullptr};

    // Frozen libcurl option & info identifiers.
    static constexpr int OPT_WRITEDATA = 10001;
    static constexpr int OPT_URL = 10002;
    static constexpr int OPT_RANGE = 10007;
    static constexpr int OPT_POSTFIELDS = 10015;
    static constexpr int OPT_USERAGENT = 10018;
    static constexpr int OPT_HTTPHEADER = 10023;
    static constexpr int OPT_HEADERDATA = 10029;
    static constexpr int OPT_PRIVATE = 10103;
    static constexpr int OPT_PROTOCOLS_STR = 10318;
    static constexpr int OPT_REDIR_PROTOCOLS_STR = 10319;
    static constexpr int OPT_WRITEFUNCTION = 20011;
    static constexpr int OPT_HEADERFUNCTION = 20079;
    static constexpr int OPT_TIMEOUT = 13;
    static constexpr int OPT_FOLLOWLOCATION = 52;
    static constexpr int OPT_MAXREDIRS = 68;
    static constexpr int OPT_CONNECTTIMEOUT = 78;
    static constexpr int OPT_HTTP_VERSION = 84;
    static constexpr int OPT_NOSIGNAL = 99;
    static constexpr int OPT_PIPEWAIT = 237;

    static constexpr long VAL_HTTP_VERSION_2TLS = 4;
    static constexpr size_t WRITEFUNC_PAUSE = 0x10000001u;
    static constexpr int PAUSE_RECV = (1 << 0);
    static constexpr int PAUSE_CONT = 0;

    static constexpr int INFO_EFFECTIVE_URL = 1048577;
    static constexpr int INFO_PRIVATE = 1048597;
    static constexpr int INFO_RESPONSE_CODE = 2097154;

    static constexpr int MOPT_SOCKETDATA = 10002;
    static constexpr int MOPT_TIMERDATA = 10005;
    static constexpr int MOPT_PIPELINING = 3;
    static constexpr int MOPT_MAX_HOST_CONNECTIONS = 7;
    static constexpr int MOPT_MAX_TOTAL_CONNECTIONS = 13;
    static constexpr int MOPT_SOCKETFUNCTION = 20001;
    static constexpr int MOPT_TIMERFUNCTION = 20004;

    static constexpr long VAL_PIPE_MULTIPLEX = 2;

    static constexpr int POLL_IN = 1;
    static constexpr int POLL_OUT = 2;
    static constexpr int POLL_INOUT = 3;
    static constexpr int POLL_REMOVE = 4;

    static constexpr int CSELECT_IN = 0x01;
    static constexpr int CSELECT_OUT = 0x02;
    static constexpr int CSELECT_ERR = 0x04;

    static constexpr curl_socket_t SOCKET_TIMEOUT = -1;
    static constexpr int MSG_DONE = 1;

    static const CurlBinding &instance()
    {
        static CurlBinding binding;
        static std::once_flag once;
        std::call_once(once, []() {
            binding.handle = ::dlopen("libcurl.so.4", RTLD_LAZY | RTLD_LOCAL | RTLD_NODELETE);
            if (!binding.handle) {
                binding.handle = ::dlopen("libcurl.so", RTLD_LAZY | RTLD_LOCAL | RTLD_NODELETE);
            }
            if (!binding.handle) {
                return;
            }
            auto sym = [&](auto &dst, const char *name) {
                dst = reinterpret_cast<std::decay_t<decltype(dst)>>(::dlsym(binding.handle, name));
            };
            sym(binding.easy_init, "curl_easy_init");
            sym(binding.easy_cleanup, "curl_easy_cleanup");
            sym(binding.easy_setopt, "curl_easy_setopt");
            sym(binding.easy_perform, "curl_easy_perform");
            sym(binding.easy_getinfo, "curl_easy_getinfo");
            sym(binding.easy_pause, "curl_easy_pause");
            sym(binding.easy_strerror, "curl_easy_strerror");
            sym(binding.slist_append, "curl_slist_append");
            sym(binding.slist_free_all, "curl_slist_free_all");
            sym(binding.multi_init, "curl_multi_init");
            sym(binding.multi_cleanup, "curl_multi_cleanup");
            sym(binding.multi_add_handle, "curl_multi_add_handle");
            sym(binding.multi_remove_handle, "curl_multi_remove_handle");
            sym(binding.multi_socket_action, "curl_multi_socket_action");
            sym(binding.multi_info_read, "curl_multi_info_read");
            sym(binding.multi_setopt, "curl_multi_setopt");
        });
        return binding;
    }

    [[nodiscard]] bool available() const noexcept
    {
        return easy_init && easy_cleanup && easy_setopt && easy_getinfo && easy_pause &&
               multi_init && multi_cleanup && multi_add_handle && multi_remove_handle &&
               multi_socket_action && multi_info_read && multi_setopt;
    }
};

/* ========================================================================= */
/* Internal Task & Transfer Structures                                       */
/* ========================================================================= */

struct Task {
    DownloadRequest request;
    ProgressCallback callback;
    std::atomic<bool> cancelled{false};
    std::atomic<TaskState> state{TaskState::Queued};
    uint64_t sequence_id{0};

    std::mutex progress_mutex;
    uint64_t bytes_downloaded{0};
    uint64_t bytes_total{0};
    float progress_pct{0.0f};
    float speed_bytes_per_sec{0.0f};
    uint32_t eta_seconds{0};
    std::string error_message;
    std::string installed_path;
    std::string extracted_dir;

    SpeedEstimator speed_estimator;

    void notifyProgress()
    {
        if (!callback) {
            return;
        }
        DownloadProgress prog;
        {
            std::lock_guard<std::mutex> lk(progress_mutex);
            prog.published_file_id = request.steam_published_file_id;
            prog.openhub_package_id = request.openhub_package_id;
            prog.state = state.load(std::memory_order_relaxed);
            prog.bytes_downloaded = bytes_downloaded;
            prog.bytes_total = bytes_total;
            prog.progress_pct = progress_pct;
            prog.speed_bytes_per_sec = speed_bytes_per_sec;
            prog.eta_seconds = eta_seconds;
            prog.error_message = error_message;
            prog.installed_path = installed_path;
            prog.extracted_dir = extracted_dir;
        }
        callback(prog);
    }
};

struct TaskComparator {
    bool operator()(const std::shared_ptr<Task> &a, const std::shared_ptr<Task> &b) const
    {
        if (static_cast<int>(a->request.priority) != static_cast<int>(b->request.priority)) {
            return static_cast<int>(a->request.priority) < static_cast<int>(b->request.priority);
        }
        return a->sequence_id > b->sequence_id;
    }
};

struct HttpTransferJob {
    std::shared_ptr<Task> task;
    std::string url;
    std::string part_path;
    std::string final_path;
    uint64_t estimated_size{0};
    bool is_wwpkg{false};
    bool fast_lane{false};
    int attempt{0};
    bool retried_416{false};

    // Optional synchronous promise for SteamWorkshop Backend B arbitration fallback.
    std::shared_ptr<std::mutex> sync_mtx;
    std::shared_ptr<std::condition_variable> sync_cv;
    std::shared_ptr<bool> sync_done;
    std::shared_ptr<bool> sync_ok;
    std::shared_ptr<std::string> sync_err;
};

struct HttpJobComparator {
    bool operator()(const std::shared_ptr<HttpTransferJob> &a,
                    const std::shared_ptr<HttpTransferJob> &b) const
    {
        int pa = static_cast<int>(a->task->request.priority);
        int pb = static_cast<int>(b->task->request.priority);
        if (pa != pb) {
            return pa < pb;
        }
        return a->task->sequence_id > b->task->sequence_id;
    }
};

struct ScheduledRetry {
    std::chrono::steady_clock::time_point deadline;
    std::shared_ptr<HttpTransferJob> job;

    bool operator>(const ScheduledRetry &other) const { return deadline > other.deadline; }
};

/* ========================================================================= */
/* DownloadManager::Impl                                                     */
/* ========================================================================= */

struct DownloadManager::Impl {
    static constexpr size_t kMaxBulkHttpLanes = 4;
    static constexpr size_t kVerifyPoolThreads = 2;

    explicit Impl(DownloadManager *q) : q_ptr(q) {}
    ~Impl() { shutdown(); }

    DownloadManager *q_ptr{nullptr};
    std::atomic<bool> m_running{false};

    TokenBucket m_tokenBucket;
    SteamworksBackend m_steamworks;

    mutable std::mutex m_hooksMutex;
    DownloadManager::SimulationHooks m_simHooks;

    mutable std::mutex m_dirMutex;
    std::string m_downloadDir;

    std::mutex m_rngMutex;
    std::mt19937 m_rng{std::random_device{}()};

    // Total non-terminal tasks across queues, HTTP reactor, arbitration workers, and verifiers.
    std::atomic<size_t> m_inFlightTasks{0};
    std::atomic<size_t> m_activeTransfers{0};
    std::atomic<uint64_t> m_sequenceCounter{0};

    mutable std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::condition_variable m_idleCv;
    std::priority_queue<std::shared_ptr<Task>, std::vector<std::shared_ptr<Task>>, TaskComparator>
        m_arbQueue;
    std::vector<std::shared_ptr<Task>> m_allTasks;

    std::atomic<int> m_currentActivePriority{0};
    std::atomic<bool> m_preemptActiveTask{false};

    // HTTP/2 curl_multi + epoll 4+1 lane reactor state.
    std::mutex m_httpMutex;
    std::priority_queue<std::shared_ptr<HttpTransferJob>,
                        std::vector<std::shared_ptr<HttpTransferJob>>, HttpJobComparator>
        m_httpFastQueue;
    std::priority_queue<std::shared_ptr<HttpTransferJob>,
                        std::vector<std::shared_ptr<HttpTransferJob>>, HttpJobComparator>
        m_httpBulkQueue;

    int m_epollFd{-1};
    int m_timerFd{-1};
    int m_wakeEventFd{-1};
    CurlBinding::CURLM *m_multi{nullptr};
    std::unordered_map<int, uint32_t> m_socketEvents;
    std::optional<std::chrono::steady_clock::time_point> m_curlTimerDeadline;

    struct ActiveHttpSlot {
        Impl *self{nullptr};
        std::shared_ptr<HttpTransferJob> job;
        CurlBinding::CURL *easy{nullptr};
        int fd{-1};
        uint64_t initial_offset{0};
        uint64_t current_offset{0};
        uint64_t expected_total{0};
        long last_http_code{0};
        int retry_after_sec{-1};
        size_t discarded_error_bytes{0};
        std::string range_header;
        bool allow_loopback_http{false};
        bool fast_lane_slot{false};
        bool truncated_for_200{false};
        bool drop_triggered{false};
        bool ssrf_rejected{false};
        bool protocol_error{false};
        bool recv_paused{false};
        uint64_t paused_epoch{0};
        std::chrono::steady_clock::time_point throttle_until{};
    };

    std::unordered_map<CurlBinding::CURL *, std::unique_ptr<ActiveHttpSlot>> m_activeSlots;
    size_t m_activeBulkLanes{0};
    bool m_activeFastLane{false};
    std::priority_queue<ScheduledRetry, std::vector<ScheduledRetry>, std::greater<ScheduledRetry>>
        m_retryHeap;

    // Bounded 2-thread cryptographic verification & extraction pool.
    std::mutex m_verifyMutex;
    std::condition_variable m_verifyCv;
    std::queue<std::shared_ptr<HttpTransferJob>> m_verifyQueue;

    std::thread m_reactorThread;
    std::thread m_arbThread;
    std::vector<std::thread> m_verifyThreads;

    [[nodiscard]] DownloadManager::SimulationHooks snapshotHooks() const
    {
        std::lock_guard<std::mutex> lk(m_hooksMutex);
        return m_simHooks;
    }

    void recordDropCount()
    {
        std::lock_guard<std::mutex> lk(m_hooksMutex);
        m_simHooks.simulated_drop_count++;
    }

    uint32_t recordRateLimitAndGetCount()
    {
        std::lock_guard<std::mutex> lk(m_hooksMutex);
        return ++m_simHooks.simulated_429_count;
    }

    void recordTruncation()
    {
        std::lock_guard<std::mutex> lk(m_hooksMutex);
        m_simHooks.simulated_truncation_occurred = true;
    }

    [[nodiscard]] std::string currentDownloadDir() const
    {
        std::lock_guard<std::mutex> lk(m_dirMutex);
        return m_downloadDir;
    }

    double calculateFullJitterBackoff(int attempt, double base_sec = 0.2, double max_sec = 10.0)
    {
        double exp_backoff = base_sec * std::pow(2.0, std::min(attempt, 10));
        double ceiling = std::min(max_sec, exp_backoff);
        std::lock_guard<std::mutex> lk(m_rngMutex);
        std::uniform_real_distribution<double> dist(0.0, ceiling);
        return dist(m_rng);
    }

    void wakeReactor() noexcept
    {
        if (m_wakeEventFd >= 0) {
            uint64_t one = 1;
            ssize_t r = ::write(m_wakeEventFd, &one, sizeof(one));
            (void)r;
        }
    }

    void markTaskTerminal(const std::shared_ptr<Task> &task, TaskState finalState,
                          const std::string &errMsg = {})
    {
        {
            std::lock_guard<std::mutex> lk(task->progress_mutex);
            if (!errMsg.empty()) {
                task->error_message = errMsg;
            }
            if (finalState == TaskState::Installed) {
                task->progress_pct = 100.0f;
                if (task->bytes_total > 0) {
                    task->bytes_downloaded = task->bytes_total;
                } else {
                    task->bytes_total = task->bytes_downloaded;
                }
                task->speed_bytes_per_sec = 0.0f;
                task->eta_seconds = 0;
            }
            task->state.store(finalState, std::memory_order_relaxed);
        }
        task->notifyProgress();

        m_inFlightTasks.fetch_sub(1, std::memory_order_acq_rel);
        std::lock_guard<std::mutex> qlk(m_queueMutex);
        m_idleCv.notify_all();
    }

    bool initialize()
    {
        if (m_running.load(std::memory_order_acquire)) {
            return true;
        }

        {
            std::lock_guard<std::mutex> lk(m_dirMutex);
            if (m_downloadDir.empty()) {
                const char *home = std::getenv("HOME");
                if (home) {
                    m_downloadDir = std::string(home) + "/.local/share/waywal/downloads";
                } else {
                    m_downloadDir = "/tmp/waywal/downloads";
                }
            }
            std::error_code ec;
            fs::create_directories(m_downloadDir, ec);
        }

        m_tokenBucket.resetForStart();
        m_steamworks.initialize(431960);
        (void)CurlBinding::instance();

        m_epollFd = ::epoll_create1(EPOLL_CLOEXEC);
        m_timerFd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        m_wakeEventFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (m_epollFd < 0 || m_timerFd < 0 || m_wakeEventFd < 0) {
            closeReactorFds();
            return false;
        }

        epoll_event evTimer{};
        evTimer.events = EPOLLIN;
        evTimer.data.fd = m_timerFd;
        ::epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_timerFd, &evTimer);

        epoll_event evWake{};
        evWake.events = EPOLLIN;
        evWake.data.fd = m_wakeEventFd;
        ::epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_wakeEventFd, &evWake);

        m_running.store(true, std::memory_order_release);
        m_reactorThread = std::thread(&Impl::httpReactorLoop, this);
        m_arbThread = std::thread(&Impl::arbWorkerLoop, this);
        m_verifyThreads.reserve(kVerifyPoolThreads);
        for (size_t i = 0; i < kVerifyPoolThreads; ++i) {
            m_verifyThreads.emplace_back(&Impl::verifyWorkerLoop, this);
        }
        return true;
    }

    void closeReactorFds()
    {
        if (m_wakeEventFd >= 0) {
            ::close(m_wakeEventFd);
            m_wakeEventFd = -1;
        }
        if (m_timerFd >= 0) {
            ::close(m_timerFd);
            m_timerFd = -1;
        }
        if (m_epollFd >= 0) {
            ::close(m_epollFd);
            m_epollFd = -1;
        }
    }

    void shutdown()
    {
        if (!m_running.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        m_tokenBucket.shutdown();

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            for (auto &t : m_allTasks) {
                t->cancelled.store(true, std::memory_order_relaxed);
            }
            m_queueCv.notify_all();
        }
        m_verifyCv.notify_all();
        wakeReactor();

        if (m_reactorThread.joinable()) {
            m_reactorThread.join();
        }
        if (m_arbThread.joinable()) {
            m_arbThread.join();
        }
        for (auto &vt : m_verifyThreads) {
            if (vt.joinable()) {
                vt.join();
            }
        }
        m_verifyThreads.clear();

        closeReactorFds();
        m_steamworks.shutdown();

        std::lock_guard<std::mutex> qlk(m_queueMutex);
        m_idleCv.notify_all();
    }

    void enqueueTask(const DownloadRequest &request, ProgressCallback callback)
    {
        auto task = std::make_shared<Task>();
        task->request = request;
        task->callback = std::move(callback);
        task->sequence_id = ++m_sequenceCounter;
        task->state.store(TaskState::Queued, std::memory_order_relaxed);

        m_inFlightTasks.fetch_add(1, std::memory_order_acq_rel);
        task->notifyProgress();

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_allTasks.push_back(task);
        }

        const auto hooks = snapshotHooks();
        bool isMockUrl = (request.openhub_download_url.rfind("mock://", 0) == 0) ||
                         (request.openhub_download_url.rfind("test://", 0) == 0);

        if (request.kind == DownloadRequest::SourceKind::OpenHubPackage && !hooks.dry_run_mode &&
            !isMockUrl) {
            auto job = std::make_shared<HttpTransferJob>();
            job->task = task;
            job->url = request.openhub_download_url;
            std::string stem = sanitizePackageFilenameStem(request.openhub_package_id);
            std::string dlDir = currentDownloadDir();
            job->part_path = dlDir + "/" + stem + ".wwpkg.part";
            job->final_path = dlDir + "/" + stem + ".wwpkg";
            job->is_wwpkg = true;
            job->fast_lane = (request.priority == TaskPriority::Immediate);

            {
                std::lock_guard<std::mutex> hlk(m_httpMutex);
                if (job->fast_lane) {
                    m_httpFastQueue.push(std::move(job));
                } else {
                    m_httpBulkQueue.push(std::move(job));
                }
            }
            wakeReactor();
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_arbQueue.push(task);
            if (request.priority == TaskPriority::Immediate) {
                int activePrio = m_currentActivePriority.load(std::memory_order_relaxed);
                if (activePrio > 0 &&
                    activePrio <= static_cast<int>(TaskPriority::BackgroundUpdate)) {
                    m_preemptActiveTask.store(true, std::memory_order_relaxed);
                }
            }
            m_queueCv.notify_all();
        }
    }

    void cancelTask(uint64_t publishedFileId)
    {
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            for (auto &t : m_allTasks) {
                if (t->request.kind == DownloadRequest::SourceKind::SteamWorkshop &&
                    t->request.steam_published_file_id == publishedFileId) {
                    t->cancelled.store(true, std::memory_order_relaxed);
                }
            }
            m_queueCv.notify_all();
        }
        m_tokenBucket.wakeAll();
        wakeReactor();
    }

    void cancelOpenHubTask(const std::string &packageIdOrCid)
    {
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            for (auto &t : m_allTasks) {
                if (t->request.kind == DownloadRequest::SourceKind::OpenHubPackage &&
                    t->request.openhub_package_id == packageIdOrCid) {
                    t->cancelled.store(true, std::memory_order_relaxed);
                }
            }
            m_queueCv.notify_all();
        }
        m_tokenBucket.wakeAll();
        wakeReactor();
    }

    void waitUntilIdle()
    {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        m_idleCv.wait(lock, [this] {
            return m_inFlightTasks.load(std::memory_order_acquire) == 0 &&
                   m_activeTransfers.load(std::memory_order_relaxed) == 0;
        });
    }

    [[nodiscard]] size_t queuedTasks() const noexcept
    {
        size_t total = 0;
        {
            std::lock_guard<std::mutex> lk(m_queueMutex);
            total += m_arbQueue.size();
        }
        {
            std::lock_guard<std::mutex> hlk(const_cast<std::mutex &>(m_httpMutex));
            total += m_httpFastQueue.size() + m_httpBulkQueue.size();
        }
        return total;
    }

    /* --------------------------------------------------------------------- */
    /* HTTP/2 Multiplexed curl_multi_socket_action + epoll 4+1 Lane Reactor  */
    /* --------------------------------------------------------------------- */

    static int onCurlSocket(CurlBinding::CURL * /*easy*/, CurlBinding::curl_socket_t s, int what,
                            void *userp, void * /*socketp*/)
    {
        auto *self = static_cast<Impl *>(userp);
        if (!self || self->m_epollFd < 0) {
            return 0;
        }

        if (what == CurlBinding::POLL_REMOVE) {
            auto it = self->m_socketEvents.find(s);
            if (it != self->m_socketEvents.end()) {
                ::epoll_ctl(self->m_epollFd, EPOLL_CTL_DEL, s, nullptr);
                self->m_socketEvents.erase(it);
            }
            return 0;
        }

        uint32_t evMask = 0;
        if (what & CurlBinding::POLL_IN) {
            evMask |= EPOLLIN;
        }
        if (what & CurlBinding::POLL_OUT) {
            evMask |= EPOLLOUT;
        }

        epoll_event ev{};
        ev.events = evMask;
        ev.data.fd = s;

        auto it = self->m_socketEvents.find(s);
        if (it == self->m_socketEvents.end()) {
            if (::epoll_ctl(self->m_epollFd, EPOLL_CTL_ADD, s, &ev) == 0 ||
                (errno == EEXIST && ::epoll_ctl(self->m_epollFd, EPOLL_CTL_MOD, s, &ev) == 0)) {
                self->m_socketEvents[s] = evMask;
            }
        } else {
            if (::epoll_ctl(self->m_epollFd, EPOLL_CTL_MOD, s, &ev) == 0 ||
                (errno == ENOENT && ::epoll_ctl(self->m_epollFd, EPOLL_CTL_ADD, s, &ev) == 0)) {
                self->m_socketEvents[s] = evMask;
            }
        }
        return 0;
    }

    void armReactorTimer()
    {
        if (m_timerFd < 0) {
            return;
        }

        std::optional<std::chrono::steady_clock::time_point> earliest = m_curlTimerDeadline;
        auto consider = [&](std::chrono::steady_clock::time_point tp) {
            if (!earliest.has_value() || tp < *earliest) {
                earliest = tp;
            }
        };

        if (!m_retryHeap.empty()) {
            consider(m_retryHeap.top().deadline);
        }

        if (!m_tokenBucket.isPaused()) {
            const uint64_t curEpoch = m_tokenBucket.epoch();
            const uint64_t curLimit = m_tokenBucket.limit();
            auto now = std::chrono::steady_clock::now();
            for (const auto &[easy, slot] : m_activeSlots) {
                (void)easy;
                if (!slot->recv_paused) {
                    continue;
                }
                if (slot->fast_lane_slot || curLimit == 0 || slot->paused_epoch != curEpoch) {
                    consider(now);
                } else if (slot->throttle_until != std::chrono::steady_clock::time_point::max()) {
                    consider(slot->throttle_until);
                }
            }
        }

        itimerspec its{};
        if (earliest.has_value()) {
            auto now = std::chrono::steady_clock::now();
            if (*earliest <= now) {
                its.it_value.tv_sec = 0;
                its.it_value.tv_nsec = 1;
            } else {
                auto ns =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(*earliest - now).count();
                its.it_value.tv_sec = static_cast<time_t>(ns / 1000000000LL);
                its.it_value.tv_nsec = std::max<long>(1L, static_cast<long>(ns % 1000000000LL));
            }
        }
        ::timerfd_settime(m_timerFd, 0, &its, nullptr);
    }

    static int onCurlTimer(CurlBinding::CURLM * /*multi*/, long timeout_ms, void *userp)
    {
        auto *self = static_cast<Impl *>(userp);
        if (!self || self->m_timerFd < 0) {
            return 0;
        }

        if (timeout_ms < 0) {
            self->m_curlTimerDeadline.reset();
        } else if (timeout_ms == 0) {
            self->m_curlTimerDeadline = std::chrono::steady_clock::now();
        } else {
            self->m_curlTimerDeadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        }
        self->armReactorTimer();
        return 0;
    }

    static size_t onCurlHeader(char *buffer, size_t size, size_t nitems, void *userdata)
    {
        size_t total = size * nitems;
        auto *slot = static_cast<ActiveHttpSlot *>(userdata);
        if (!slot || total == 0) {
            return total;
        }

        std::string_view line(buffer, total);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            return total;
        }

        auto startsWithCaseInsensitive = [](std::string_view str, std::string_view prefix) {
            if (str.size() < prefix.size()) {
                return false;
            }
            for (size_t i = 0; i < prefix.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(str[i])) !=
                    std::tolower(static_cast<unsigned char>(prefix[i]))) {
                    return false;
                }
            }
            return true;
        };

        if (startsWithCaseInsensitive(line, "HTTP/")) {
            auto sp = line.find(' ');
            if (sp != std::string_view::npos) {
                while (sp < line.size() && line[sp] == ' ') {
                    ++sp;
                }
                long code = 0;
                while (sp < line.size() && std::isdigit(static_cast<unsigned char>(line[sp]))) {
                    code = code * 10 + (line[sp] - '0');
                    ++sp;
                }
                if (code > 0) {
                    slot->last_http_code = code;
                    slot->retry_after_sec = -1;
                }
            }
            return total;
        }

        if (startsWithCaseInsensitive(line, "Location:")) {
            std::string_view loc = line.substr(9);
            while (!loc.empty() && (loc.front() == ' ' || loc.front() == '\t')) {
                loc.remove_prefix(1);
            }
            if (!loc.empty() && loc.front() != '/') {
                if (!isAllowedDownloadUrl(loc, slot->allow_loopback_http)) {
                    slot->ssrf_rejected = true;
                    return 0;
                }
            }
            return total;
        }

        if (startsWithCaseInsensitive(line, "Retry-After:")) {
            std::string_view val = line.substr(12);
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
                val.remove_prefix(1);
            }
            int secs = 0;
            bool anyDigit = false;
            for (char ch : val) {
                if (std::isdigit(static_cast<unsigned char>(ch))) {
                    anyDigit = true;
                    secs = std::min(3600, secs * 10 + (ch - '0'));
                } else {
                    break;
                }
            }
            if (anyDigit) {
                slot->retry_after_sec = std::min(secs, 30);
            }
            return total;
        }

        if (startsWithCaseInsensitive(line, "Content-Length:")) {
            if (slot->last_http_code == 200 || slot->last_http_code == 206) {
                std::string_view val = line.substr(15);
                while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
                    val.remove_prefix(1);
                }
                uint64_t contentLen = 0;
                bool ok = false;
                for (char ch : val) {
                    if (std::isdigit(static_cast<unsigned char>(ch))) {
                        ok = true;
                        contentLen = contentLen * 10 + static_cast<uint64_t>(ch - '0');
                    } else {
                        break;
                    }
                }
                if (ok) {
                    uint64_t base = (slot->last_http_code == 206) ? slot->initial_offset : 0;
                    slot->expected_total = base + contentLen;
                    std::lock_guard<std::mutex> lk(slot->job->task->progress_mutex);
                    slot->job->task->bytes_total = slot->expected_total;
                }
            }
        }
        return total;
    }

    static size_t onCurlWrite(char *ptr, size_t size, size_t nmemb, void *userdata)
    {
        size_t bytes = size * nmemb;
        if (bytes == 0) {
            return 0;
        }
        auto *slot = static_cast<ActiveHttpSlot *>(userdata);
        if (!slot || !slot->self) {
            return 0;
        }

        if (slot->job->task->cancelled.load(std::memory_order_relaxed) ||
            !slot->self->m_running.load(std::memory_order_relaxed)) {
            return 0;
        }

        const auto &curl = CurlBinding::instance();
        long httpCode = slot->last_http_code;
        if (httpCode == 0) {
            curl.easy_getinfo(slot->easy, CurlBinding::INFO_RESPONSE_CODE, &httpCode);
            slot->last_http_code = httpCode;
        }

        const char *effectiveUrl = nullptr;
        if (curl.easy_getinfo(slot->easy, CurlBinding::INFO_EFFECTIVE_URL, &effectiveUrl) == 0 &&
            effectiveUrl != nullptr) {
            if (!isAllowedDownloadUrl(effectiveUrl, slot->allow_loopback_http)) {
                slot->ssrf_rejected = true;
                return 0;
            }
        }

        // Never write error or redirect bodies into the .part file.
        if (httpCode != 200 && httpCode != 206) {
            slot->discarded_error_bytes += bytes;
            return (slot->discarded_error_bytes <= 65536) ? bytes : 0;
        }

        if (httpCode == 206 && slot->initial_offset == 0) {
            slot->protocol_error = true;
            return 0;
        }

        if (httpCode == 200 && slot->initial_offset > 0 && !slot->truncated_for_200) {
            if (::ftruncate(slot->fd, 0) != 0 || ::lseek(slot->fd, 0, SEEK_SET) < 0) {
                return 0;
            }
            slot->current_offset = 0;
            slot->truncated_for_200 = true;
            slot->self->recordTruncation();
            {
                std::lock_guard<std::mutex> lk(slot->job->task->progress_mutex);
                slot->job->task->bytes_downloaded = 0;
            }
        }

        const auto hooks = slot->self->snapshotHooks();
        if (hooks.drop_at_percent > 0.0f && hooks.simulated_drop_count == 0 &&
            slot->expected_total > 0) {
            if (slot->current_offset >=
                static_cast<uint64_t>(slot->expected_total * hooks.drop_at_percent)) {
                slot->self->recordDropCount();
                slot->drop_triggered = true;
                return 0;
            }
        }

        if (slot->self->m_tokenBucket.isPaused()) {
            slot->recv_paused = true;
            slot->paused_epoch = slot->self->m_tokenBucket.epoch();
            slot->throttle_until = std::chrono::steady_clock::time_point::max();
            return CurlBinding::WRITEFUNC_PAUSE;
        }

        // Rate-limit bulk lanes non-blockingly so the epoll reactor and Immediate fast lane
        // remain responsive while tokens refill.
        if (!slot->fast_lane_slot && slot->self->m_tokenBucket.limit() > 0) {
            std::chrono::steady_clock::time_point nextReady{};
            bool paused = false;
            uint64_t curEpoch = 0;
            if (!slot->self->m_tokenBucket.tryConsume(bytes, nextReady, paused, curEpoch)) {
                slot->recv_paused = true;
                slot->paused_epoch = curEpoch;
                slot->throttle_until =
                    paused ? std::chrono::steady_clock::time_point::max() : nextReady;
                slot->self->armReactorTimer();
                return CurlBinding::WRITEFUNC_PAUSE;
            }
        }

        size_t writtenAll = 0;
        while (writtenAll < bytes) {
            ssize_t w = ::write(slot->fd, ptr + writtenAll, bytes - writtenAll);
            if (w < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return 0;
            }
            writtenAll += static_cast<size_t>(w);
        }

        slot->current_offset += writtenAll;
        {
            std::lock_guard<std::mutex> lk(slot->job->task->progress_mutex);
            slot->job->task->bytes_downloaded = slot->current_offset;
            if (slot->expected_total > 0) {
                slot->job->task->bytes_total = std::max(slot->expected_total, slot->current_offset);
                slot->job->task->progress_pct = static_cast<float>((slot->current_offset * 100.0) /
                                                                   slot->job->task->bytes_total);
            }
            slot->job->task->speed_estimator.update(
                slot->current_offset, slot->job->task->bytes_total,
                slot->job->task->speed_bytes_per_sec, slot->job->task->eta_seconds);
        }
        slot->job->task->notifyProgress();
        return writtenAll;
    }

    void finishHttpJobOutcome(const std::shared_ptr<HttpTransferJob> &job, bool ok,
                              const std::string &errMsg, bool wasCancelled = false)
    {
        if (job->sync_done) {
            {
                std::lock_guard<std::mutex> slk(*job->sync_mtx);
                *job->sync_ok = ok;
                *job->sync_err = errMsg;
                *job->sync_done = true;
            }
            job->sync_cv->notify_all();
            return;
        }

        if (wasCancelled) {
            markTaskTerminal(job->task, TaskState::Cancelled,
                             errMsg.empty() ? "Download cancelled by user" : errMsg);
            return;
        }

        if (!ok) {
            markTaskTerminal(job->task, TaskState::Failed,
                             errMsg.empty() ? "HTTP download failed" : errMsg);
            return;
        }

        if (job->is_wwpkg) {
            {
                std::lock_guard<std::mutex> vlk(m_verifyMutex);
                m_verifyQueue.push(job);
            }
            m_verifyCv.notify_one();
        } else {
            std::error_code ec;
            fs::rename(job->part_path, job->final_path, ec);
            if (ec) {
                markTaskTerminal(job->task, TaskState::Failed,
                                 "Failed to finalize downloaded file");
                return;
            }
            {
                std::lock_guard<std::mutex> lk(job->task->progress_mutex);
                job->task->installed_path = job->final_path;
            }
            job->task->state.store(TaskState::Verifying, std::memory_order_relaxed);
            job->task->notifyProgress();
            job->task->state.store(TaskState::Extracting, std::memory_order_relaxed);
            job->task->notifyProgress();
            job->task->state.store(TaskState::Indexing, std::memory_order_relaxed);
            job->task->notifyProgress();
            markTaskTerminal(job->task, TaskState::Installed);
        }
    }

    void releaseSlotLane(ActiveHttpSlot *slot)
    {
        if (slot->fast_lane_slot) {
            m_activeFastLane = false;
        } else if (m_activeBulkLanes > 0) {
            --m_activeBulkLanes;
        }
        m_activeTransfers.fetch_sub(1, std::memory_order_relaxed);
    }

    bool launchHttpSlot(const std::shared_ptr<HttpTransferJob> &job, bool useFastLaneSlot)
    {
        const auto &curl = CurlBinding::instance();
        if (!m_multi || !curl.available()) {
            finishHttpJobOutcome(job, false, "libcurl multi engine unavailable");
            return false;
        }

        if (job->task->cancelled.load(std::memory_order_relaxed)) {
            finishHttpJobOutcome(job, false, "Cancelled before transfer", true);
            return false;
        }

        const auto hooks = snapshotHooks();
        const bool allowLoopback = hooks.allow_loopback_http || envLoopbackHttpAllowed();
        if (!isAllowedDownloadUrl(job->url, allowLoopback)) {
            finishHttpJobOutcome(job, false, "Rejected disallowed URL scheme or host");
            return false;
        }

        uint64_t offset = 0;
        struct stat st{};
        if (::stat(job->part_path.c_str(), &st) == 0 && st.st_size > 0) {
            offset = static_cast<uint64_t>(st.st_size);
        }

        int fd = ::open(job->part_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (fd < 0) {
            finishHttpJobOutcome(job, false, "Failed to open staging .part file");
            return false;
        }
        if (offset > 0 && ::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) {
            ::close(fd);
            finishHttpJobOutcome(job, false, "Failed to seek staging .part file");
            return false;
        }

        CurlBinding::CURL *easy = curl.easy_init();
        if (!easy) {
            ::close(fd);
            finishHttpJobOutcome(job, false, "curl_easy_init failed");
            return false;
        }

        auto slot = std::make_unique<ActiveHttpSlot>();
        slot->self = this;
        slot->job = job;
        slot->easy = easy;
        slot->fd = fd;
        slot->initial_offset = offset;
        slot->current_offset = offset;
        slot->expected_total = std::max(job->estimated_size, offset);
        slot->allow_loopback_http = allowLoopback;
        slot->fast_lane_slot = useFastLaneSlot;

        {
            std::lock_guard<std::mutex> lk(job->task->progress_mutex);
            job->task->state.store(TaskState::Downloading, std::memory_order_relaxed);
            job->task->bytes_downloaded = offset;
            job->task->bytes_total = slot->expected_total;
            job->task->speed_estimator.reset(offset);
        }
        job->task->notifyProgress();

        const bool isHttps =
            job->url.size() >= 8 &&
            std::equal(job->url.begin(), job->url.begin() + 8, "https://", [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == b;
            });
        const char *allowedProtocols = allowLoopback ? "https,http" : "https";
        curl.easy_setopt(easy, CurlBinding::OPT_URL, job->url.c_str());
        curl.easy_setopt(easy, CurlBinding::OPT_PROTOCOLS_STR, allowedProtocols);
        curl.easy_setopt(easy, CurlBinding::OPT_REDIR_PROTOCOLS_STR, allowedProtocols);
        curl.easy_setopt(easy, CurlBinding::OPT_FOLLOWLOCATION, 1L);
        curl.easy_setopt(easy, CurlBinding::OPT_MAXREDIRS, 5L);
        curl.easy_setopt(easy, CurlBinding::OPT_HTTP_VERSION, CurlBinding::VAL_HTTP_VERSION_2TLS);
        // Coalesce bulk HTTPS transfers onto multiplexed HTTP/2 streams, while keeping
        // the dedicated Immediate fast lane and cleartext loopback HTTP free of pipe-wait blocking.
        curl.easy_setopt(easy, CurlBinding::OPT_PIPEWAIT, (isHttps && !useFastLaneSlot) ? 1L : 0L);
        curl.easy_setopt(easy, CurlBinding::OPT_NOSIGNAL, 1L);
        curl.easy_setopt(easy, CurlBinding::OPT_CONNECTTIMEOUT, 15L);
        curl.easy_setopt(easy, CurlBinding::OPT_TIMEOUT, 300L);
        curl.easy_setopt(easy, CurlBinding::OPT_USERAGENT, "WayWal-Studio/3.0");
        curl.easy_setopt(easy, CurlBinding::OPT_HEADERFUNCTION, &Impl::onCurlHeader);
        curl.easy_setopt(easy, CurlBinding::OPT_HEADERDATA, slot.get());
        curl.easy_setopt(easy, CurlBinding::OPT_WRITEFUNCTION, &Impl::onCurlWrite);
        curl.easy_setopt(easy, CurlBinding::OPT_WRITEDATA, slot.get());

        if (offset > 0) {
            slot->range_header = std::to_string(offset) + "-";
            curl.easy_setopt(easy, CurlBinding::OPT_RANGE, slot->range_header.c_str());
        }

        if (useFastLaneSlot) {
            m_activeFastLane = true;
        } else {
            ++m_activeBulkLanes;
        }
        m_activeTransfers.fetch_add(1, std::memory_order_relaxed);

        ActiveHttpSlot *rawSlot = slot.get();
        m_activeSlots.emplace(easy, std::move(slot));

        if (curl.multi_add_handle(m_multi, easy) != 0) {
            ::close(rawSlot->fd);
            curl.easy_cleanup(easy);
            releaseSlotLane(rawSlot);
            m_activeSlots.erase(easy);
            finishHttpJobOutcome(job, false, "curl_multi_add_handle failed");
            return false;
        }

        int stillRunning = 0;
        curl.multi_socket_action(m_multi, CurlBinding::SOCKET_TIMEOUT, 0, &stillRunning);
        return true;
    }

    void dispatchQueuedHttpJobs()
    {
        auto now = std::chrono::steady_clock::now();
        while (!m_retryHeap.empty() && m_retryHeap.top().deadline <= now) {
            auto readyJob = m_retryHeap.top().job;
            m_retryHeap.pop();
            std::lock_guard<std::mutex> hlk(m_httpMutex);
            if (readyJob->fast_lane) {
                m_httpFastQueue.push(std::move(readyJob));
            } else {
                m_httpBulkQueue.push(std::move(readyJob));
            }
        }

        while (true) {
            std::shared_ptr<HttpTransferJob> nextJob;
            bool useFastSlot = false;
            {
                std::lock_guard<std::mutex> hlk(m_httpMutex);
                if (!m_httpFastQueue.empty() && !m_activeFastLane) {
                    nextJob = m_httpFastQueue.top();
                    m_httpFastQueue.pop();
                    useFastSlot = true;
                } else if (!m_httpFastQueue.empty() && m_activeBulkLanes < kMaxBulkHttpLanes) {
                    nextJob = m_httpFastQueue.top();
                    m_httpFastQueue.pop();
                    useFastSlot = false;
                } else if (!m_httpBulkQueue.empty() && m_activeBulkLanes < kMaxBulkHttpLanes) {
                    nextJob = m_httpBulkQueue.top();
                    m_httpBulkQueue.pop();
                    useFastSlot = false;
                }
            }
            if (!nextJob) {
                break;
            }
            launchHttpSlot(nextJob, useFastSlot);
        }
    }

    void cancelActiveFlaggedSlots()
    {
        const auto &curl = CurlBinding::instance();
        std::vector<CurlBinding::CURL *> toCancel;
        for (const auto &[easy, slot] : m_activeSlots) {
            if (slot->job->task->cancelled.load(std::memory_order_relaxed)) {
                toCancel.push_back(easy);
            }
        }
        for (auto *easy : toCancel) {
            auto it = m_activeSlots.find(easy);
            if (it == m_activeSlots.end()) {
                continue;
            }
            std::unique_ptr<ActiveHttpSlot> slot = std::move(it->second);
            m_activeSlots.erase(it);

            curl.multi_remove_handle(m_multi, easy);
            curl.easy_cleanup(easy);
            if (slot->fd >= 0) {
                ::close(slot->fd);
                slot->fd = -1;
            }
            releaseSlotLane(slot.get());
            finishHttpJobOutcome(slot->job, false, "Download cancelled by user", true);
        }
    }

    void reapCompletedMultiTransfers()
    {
        const auto &curl = CurlBinding::instance();
        if (!m_multi) {
            return;
        }

        int msgsLeft = 0;
        while (CurlBinding::CurlMsg *msg = curl.multi_info_read(m_multi, &msgsLeft)) {
            if (msg->msg != CurlBinding::MSG_DONE) {
                continue;
            }

            CurlBinding::CURL *easy = msg->easy_handle;
            int curlResult = msg->data.result;

            auto it = m_activeSlots.find(easy);
            if (it == m_activeSlots.end()) {
                continue;
            }
            std::unique_ptr<ActiveHttpSlot> slot = std::move(it->second);
            m_activeSlots.erase(it);

            long httpCode = slot->last_http_code;
            if (httpCode == 0) {
                curl.easy_getinfo(easy, CurlBinding::INFO_RESPONSE_CODE, &httpCode);
            }
            const char *effUrl = nullptr;
            if (curl.easy_getinfo(easy, CurlBinding::INFO_EFFECTIVE_URL, &effUrl) == 0 && effUrl) {
                if (!isAllowedDownloadUrl(effUrl, slot->allow_loopback_http)) {
                    slot->ssrf_rejected = true;
                }
            }

            curl.multi_remove_handle(m_multi, easy);
            curl.easy_cleanup(easy);

            if (slot->fd >= 0) {
                ::close(slot->fd);
                slot->fd = -1;
            }
            releaseSlotLane(slot.get());

            auto job = slot->job;
            if (job->task->cancelled.load(std::memory_order_relaxed)) {
                finishHttpJobOutcome(job, false, "Download cancelled by user", true);
                continue;
            }

            if (slot->ssrf_rejected) {
                if (slot->initial_offset == 0) {
                    ::unlink(job->part_path.c_str());
                }
                finishHttpJobOutcome(job, false,
                                     "Redirect or target protocol blocked by SSRF policy");
                continue;
            }

            if (slot->protocol_error) {
                ::unlink(job->part_path.c_str());
                finishHttpJobOutcome(job, false,
                                     "Unexpected HTTP 206 response without Range request");
                continue;
            }

            // Stale .part recovery on HTTP 416 Range Not Satisfiable: truncate once and retry from
            // 0.
            if (httpCode == 416 && !job->retried_416) {
                int truncFd = ::open(job->part_path.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
                if (truncFd >= 0) {
                    ::close(truncFd);
                } else {
                    ::unlink(job->part_path.c_str());
                }
                job->retried_416 = true;
                m_retryHeap.push(ScheduledRetry{std::chrono::steady_clock::now(), job});
                continue;
            }

            // Rate limit / transient overload: honor Retry-After + full-jitter backoff.
            constexpr int kMaxHttpAttempts = 5;
            if ((httpCode == 429 || httpCode == 503) && job->attempt + 1 < kMaxHttpAttempts) {
                recordRateLimitAndGetCount();
                if (slot->initial_offset == 0 && slot->current_offset == 0) {
                    ::unlink(job->part_path.c_str());
                }
                double delaySec = calculateFullJitterBackoff(job->attempt, 0.05, 2.0);
                if (slot->retry_after_sec >= 0) {
                    delaySec = std::max(delaySec, static_cast<double>(slot->retry_after_sec));
                }
                job->attempt++;
                auto deadline = std::chrono::steady_clock::now() +
                                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                    std::chrono::duration<double>(delaySec));
                m_retryHeap.push(ScheduledRetry{deadline, job});
                continue;
            }

            // Permanent client errors (e.g. 403, 404, unrecovered 416): fail immediately, discard
            // HTML.
            if (httpCode >= 400 && httpCode < 500) {
                if (slot->initial_offset == 0 && slot->current_offset == 0) {
                    ::unlink(job->part_path.c_str());
                }
                finishHttpJobOutcome(job, false, "HTTP client error " + std::to_string(httpCode));
                continue;
            }

            bool validSuccessCode =
                (httpCode == 200) || (httpCode == 206 && slot->initial_offset > 0);
            if (curlResult == 0 && !slot->drop_triggered && validSuccessCode) {
                finishHttpJobOutcome(job, true, {});
                continue;
            }

            // Mid-stream network drop or 5xx server error: retry with byte-range resumption.
            if (job->attempt + 1 < kMaxHttpAttempts) {
                double delaySec = slot->drop_triggered
                                      ? 0.01
                                      : calculateFullJitterBackoff(job->attempt, 0.05, 1.5);
                job->attempt++;
                auto deadline = std::chrono::steady_clock::now() +
                                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                    std::chrono::duration<double>(delaySec));
                m_retryHeap.push(ScheduledRetry{deadline, job});
                continue;
            }

            if (slot->current_offset == 0) {
                ::unlink(job->part_path.c_str());
            }
            std::string reason =
                (httpCode > 0) ? ("HTTP transfer failed with status " + std::to_string(httpCode))
                               : "HTTP network transfer failed after retries";
            finishHttpJobOutcome(job, false, reason);
        }
    }

    void resumeReadyPausedSlots()
    {
        if (!m_multi || m_tokenBucket.isPaused()) {
            return;
        }

        const auto &curl = CurlBinding::instance();
        const uint64_t curEpoch = m_tokenBucket.epoch();
        const uint64_t curLimit = m_tokenBucket.limit();
        const auto now = std::chrono::steady_clock::now();

        std::vector<CurlBinding::CURL *> toResume;
        for (const auto &[easy, slot] : m_activeSlots) {
            if (!slot->recv_paused) {
                continue;
            }
            if (slot->fast_lane_slot || curLimit == 0 || slot->paused_epoch != curEpoch ||
                now >= slot->throttle_until) {
                toResume.push_back(easy);
            }
        }

        bool anyUnpaused = false;
        for (auto *easy : toResume) {
            auto it = m_activeSlots.find(easy);
            if (it == m_activeSlots.end() ||
                it->second->job->task->cancelled.load(std::memory_order_relaxed)) {
                continue;
            }
            it->second->recv_paused = false;
            it->second->throttle_until = {};
            curl.easy_pause(easy, CurlBinding::PAUSE_CONT);
            anyUnpaused = true;
        }

        if (anyUnpaused) {
            int stillRunning = 0;
            curl.multi_socket_action(m_multi, CurlBinding::SOCKET_TIMEOUT, 0, &stillRunning);
            reapCompletedMultiTransfers();
        }
    }

    void httpReactorLoop()
    {
        m_curlTimerDeadline.reset();
        const auto &curl = CurlBinding::instance();
        if (curl.available()) {
            m_multi = curl.multi_init();
            if (m_multi) {
                curl.multi_setopt(m_multi, CurlBinding::MOPT_PIPELINING,
                                  CurlBinding::VAL_PIPE_MULTIPLEX);
                curl.multi_setopt(m_multi, CurlBinding::MOPT_MAX_HOST_CONNECTIONS, 6L);
                curl.multi_setopt(m_multi, CurlBinding::MOPT_MAX_TOTAL_CONNECTIONS, 8L);
                curl.multi_setopt(m_multi, CurlBinding::MOPT_SOCKETFUNCTION, &Impl::onCurlSocket);
                curl.multi_setopt(m_multi, CurlBinding::MOPT_SOCKETDATA, this);
                curl.multi_setopt(m_multi, CurlBinding::MOPT_TIMERFUNCTION, &Impl::onCurlTimer);
                curl.multi_setopt(m_multi, CurlBinding::MOPT_TIMERDATA, this);
            }
        }

        std::array<epoll_event, 32> events{};

        while (m_running.load(std::memory_order_acquire)) {
            cancelActiveFlaggedSlots();
            resumeReadyPausedSlots();
            dispatchQueuedHttpJobs();
            reapCompletedMultiTransfers();
            armReactorTimer();

            int nfds = ::epoll_wait(m_epollFd, events.data(), static_cast<int>(events.size()), -1);
            if (!m_running.load(std::memory_order_acquire)) {
                break;
            }
            if (nfds < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }

            int stillRunning = 0;
            for (int i = 0; i < nfds; ++i) {
                int fd = events[static_cast<size_t>(i)].data.fd;
                uint32_t revents = events[static_cast<size_t>(i)].events;

                if (fd == m_wakeEventFd) {
                    uint64_t val = 0;
                    while (::read(m_wakeEventFd, &val, sizeof(val)) > 0) {
                    }
                    continue;
                }

                if (fd == m_timerFd) {
                    uint64_t expirations = 0;
                    while (::read(m_timerFd, &expirations, sizeof(expirations)) > 0) {
                    }
                    if (m_multi && (!m_curlTimerDeadline.has_value() ||
                                    std::chrono::steady_clock::now() >= *m_curlTimerDeadline)) {
                        m_curlTimerDeadline.reset();
                        curl.multi_socket_action(m_multi, CurlBinding::SOCKET_TIMEOUT, 0,
                                                 &stillRunning);
                    }
                    continue;
                }

                if (m_multi) {
                    int action = 0;
                    if (revents & EPOLLIN) {
                        action |= CurlBinding::CSELECT_IN;
                    }
                    if (revents & EPOLLOUT) {
                        action |= CurlBinding::CSELECT_OUT;
                    }
                    if (revents & (EPOLLERR | EPOLLHUP)) {
                        action |= CurlBinding::CSELECT_ERR;
                    }
                    curl.multi_socket_action(m_multi, fd, action, &stillRunning);
                }
            }

            reapCompletedMultiTransfers();
        }

        // Clean shutdown: abort active slots, queued HTTP jobs, and pending retries.
        m_curlTimerDeadline.reset();
        if (m_multi) {
            for (auto &[easy, slot] : m_activeSlots) {
                curl.multi_remove_handle(m_multi, easy);
                curl.easy_cleanup(easy);
                if (slot->fd >= 0) {
                    ::close(slot->fd);
                    slot->fd = -1;
                }
                releaseSlotLane(slot.get());
                finishHttpJobOutcome(slot->job, false, "Shutdown in progress", true);
            }
            m_activeSlots.clear();
            m_socketEvents.clear();
            curl.multi_cleanup(m_multi);
            m_multi = nullptr;
        }

        while (!m_retryHeap.empty()) {
            auto job = m_retryHeap.top().job;
            m_retryHeap.pop();
            finishHttpJobOutcome(job, false, "Shutdown in progress", true);
        }

        std::vector<std::shared_ptr<HttpTransferJob>> drained;
        {
            std::lock_guard<std::mutex> hlk(m_httpMutex);
            while (!m_httpFastQueue.empty()) {
                drained.push_back(m_httpFastQueue.top());
                m_httpFastQueue.pop();
            }
            while (!m_httpBulkQueue.empty()) {
                drained.push_back(m_httpBulkQueue.top());
                m_httpBulkQueue.pop();
            }
        }
        for (auto &job : drained) {
            finishHttpJobOutcome(job, false, "Shutdown in progress", true);
        }
    }

    /* --------------------------------------------------------------------- */
    /* Bounded 2-Thread Cryptographic Verification & Extraction Pool         */
    /* --------------------------------------------------------------------- */

    void verifyWorkerLoop()
    {
        while (true) {
            std::shared_ptr<HttpTransferJob> job;
            {
                std::unique_lock<std::mutex> lk(m_verifyMutex);
                m_verifyCv.wait(lk, [this] {
                    return !m_running.load(std::memory_order_acquire) || !m_verifyQueue.empty();
                });
                if (m_verifyQueue.empty()) {
                    if (!m_running.load(std::memory_order_acquire)) {
                        break;
                    }
                    continue;
                }
                job = m_verifyQueue.front();
                m_verifyQueue.pop();
            }

            if (!job) {
                continue;
            }

            if (job->task->cancelled.load(std::memory_order_relaxed) ||
                !m_running.load(std::memory_order_acquire)) {
                ::unlink(job->part_path.c_str());
                markTaskTerminal(job->task, TaskState::Cancelled, "Verification cancelled");
                continue;
            }

            job->task->state.store(TaskState::Verifying, std::memory_order_relaxed);
            job->task->notifyProgress();

            wwpkg_package_t *pkg = wwpkg_open(job->part_path.c_str());
            if (!pkg) {
                ::unlink(job->part_path.c_str());
                markTaskTerminal(job->task, TaskState::Failed,
                                 "Invalid or corrupt .wwpkg container");
                continue;
            }

            bool isTrusted = false;
            bool sigValid = wwpkg_verify_signature(pkg, &isTrusted);
            if (!sigValid) {
                wwpkg_close(pkg);
                ::unlink(job->part_path.c_str());
                markTaskTerminal(job->task, TaskState::Failed,
                                 "Cryptographic Ed25519 or Merkle verification failed");
                continue;
            }

            job->task->state.store(TaskState::Extracting, std::memory_order_relaxed);
            job->task->notifyProgress();

            std::string stem = sanitizePackageFilenameStem(job->task->request.openhub_package_id);
            fs::path extractDir = fs::path(currentDownloadDir()) / "extracted" / stem;
            bool extractedOk = extractVerifiedWwpkgAll(pkg, job->part_path, extractDir);
            wwpkg_close(pkg);

            if (!extractedOk) {
                std::error_code ec;
                fs::remove_all(extractDir, ec);
                ::unlink(job->part_path.c_str());
                markTaskTerminal(job->task, TaskState::Failed, "Confined .wwpkg extraction failed");
                continue;
            }

            std::error_code renameEc;
            fs::rename(job->part_path, job->final_path, renameEc);
            if (renameEc) {
                std::error_code ec;
                fs::remove_all(extractDir, ec);
                ::unlink(job->part_path.c_str());
                markTaskTerminal(job->task, TaskState::Failed,
                                 "Failed to move verified .wwpkg into destination");
                continue;
            }

            {
                std::lock_guard<std::mutex> lk(job->task->progress_mutex);
                job->task->installed_path = job->final_path;
                job->task->extracted_dir = extractDir.string();
            }

            job->task->state.store(TaskState::Indexing, std::memory_order_relaxed);
            job->task->notifyProgress();

            markTaskTerminal(job->task, TaskState::Installed);
        }
    }

    /* --------------------------------------------------------------------- */
    /* Steam Workshop 3-Backend Arbitration Worker                           */
    /* --------------------------------------------------------------------- */

    void arbWorkerLoop()
    {
        while (m_running.load(std::memory_order_acquire)) {
            std::shared_ptr<Task> currentTask;
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_queueCv.wait(lock, [this] {
                    return !m_running.load(std::memory_order_acquire) || !m_arbQueue.empty();
                });

                if (m_arbQueue.empty()) {
                    if (!m_running.load(std::memory_order_acquire)) {
                        break;
                    }
                    continue;
                }

                currentTask = m_arbQueue.top();
                m_arbQueue.pop();
            }

            if (!currentTask) {
                continue;
            }

            if (currentTask->cancelled.load(std::memory_order_relaxed)) {
                markTaskTerminal(currentTask, TaskState::Cancelled, "Cancelled before execution");
                continue;
            }

            m_activeTransfers.fetch_add(1, std::memory_order_relaxed);
            executeArbitratedTask(currentTask);
            m_activeTransfers.fetch_sub(1, std::memory_order_relaxed);

            std::lock_guard<std::mutex> qlk(m_queueMutex);
            m_idleCv.notify_all();
        }

        // Drain any remaining queued tasks on shutdown so m_inFlightTasks reaches 0.
        std::vector<std::shared_ptr<Task>> remaining;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            while (!m_arbQueue.empty()) {
                remaining.push_back(m_arbQueue.top());
                m_arbQueue.pop();
            }
        }
        for (auto &t : remaining) {
            markTaskTerminal(t, TaskState::Cancelled, "Shutdown in progress");
        }
    }

    void executeArbitratedTask(const std::shared_ptr<Task> &task)
    {
        m_currentActivePriority.store(static_cast<int>(task->request.priority),
                                      std::memory_order_relaxed);
        task->state.store(TaskState::Probing, std::memory_order_relaxed);
        task->notifyProgress();

        bool success = false;
        std::string failureReason;
        const auto hooks = snapshotHooks();

        if (task->request.kind == DownloadRequest::SourceKind::SteamWorkshop) {
            if (!hooks.force_backend_failure_a && m_steamworks.isRunning()) {
                if (executeBackendA(task)) {
                    success = true;
                } else {
                    failureReason += "Backend A failed; ";
                }
            } else {
                failureReason += "Backend A unavailable; ";
            }

            if (!success && !task->cancelled.load(std::memory_order_relaxed) &&
                m_running.load(std::memory_order_acquire)) {
                if (!hooks.force_backend_failure_b) {
                    if (executeBackendB(task)) {
                        success = true;
                    } else {
                        failureReason += "Backend B failed; ";
                    }
                } else {
                    failureReason += "Backend B skipped; ";
                }
            }

            if (!success && !task->cancelled.load(std::memory_order_relaxed) &&
                m_running.load(std::memory_order_acquire) &&
                !(m_preemptActiveTask.load(std::memory_order_relaxed) &&
                  task->request.priority <= TaskPriority::BackgroundUpdate)) {
                if (!hooks.force_backend_failure_c) {
                    if (executeBackendC(task)) {
                        success = true;
                    } else {
                        failureReason += "Backend C failed; ";
                    }
                } else {
                    failureReason += "Backend C skipped; ";
                }
            }
        } else {
            if (executeBackendB(task)) {
                success = true;
            } else {
                failureReason = "Open Hub synthetic download failed";
            }
        }

        if (task->cancelled.load(std::memory_order_relaxed)) {
            m_currentActivePriority.store(0, std::memory_order_relaxed);
            markTaskTerminal(task, TaskState::Cancelled, "Download cancelled by user");
            return;
        }

        if (m_preemptActiveTask.load(std::memory_order_relaxed) &&
            task->request.priority <= TaskPriority::BackgroundUpdate && !success) {
            task->state.store(TaskState::Queued, std::memory_order_relaxed);
            task->notifyProgress();
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_arbQueue.push(task);
                m_queueCv.notify_all();
            }
            m_preemptActiveTask.store(false, std::memory_order_relaxed);
            m_currentActivePriority.store(0, std::memory_order_relaxed);
            return;
        }

        m_currentActivePriority.store(0, std::memory_order_relaxed);
        if (success) {
            task->state.store(TaskState::Verifying, std::memory_order_relaxed);
            task->notifyProgress();
            task->state.store(TaskState::Extracting, std::memory_order_relaxed);
            task->notifyProgress();
            task->state.store(TaskState::Indexing, std::memory_order_relaxed);
            task->notifyProgress();
            markTaskTerminal(task, TaskState::Installed);
        } else {
            markTaskTerminal(task, TaskState::Failed,
                             failureReason.empty() ? "All backends exhausted" : failureReason);
        }
    }

    /* --------------------------------------------------------------------- */
    /* Backend A Execution (Steamworks SDK with Stall Timeout)               */
    /* --------------------------------------------------------------------- */

    bool executeBackendA(const std::shared_ptr<Task> &task)
    {
        uint64_t publishedFileId = task->request.steam_published_file_id;
        if (!m_steamworks.subscribeItem(publishedFileId)) {
            return false;
        }
        if (!m_steamworks.downloadItem(publishedFileId, true)) {
            return false;
        }

        {
            std::lock_guard<std::mutex> lk(task->progress_mutex);
            task->state.store(TaskState::Downloading, std::memory_order_relaxed);
            task->speed_estimator.reset(0);
        }

        const auto hooks = snapshotHooks();
        const auto stallLimit = std::chrono::milliseconds(
            hooks.steamworks_stall_timeout_ms > 0 ? hooks.steamworks_stall_timeout_ms : 30000);
        const auto maxOverall = std::chrono::minutes(10);

        const auto startTime = std::chrono::steady_clock::now();
        auto lastProgressTime = startTime;
        uint64_t lastBytes = 0;

        while (m_running.load(std::memory_order_relaxed) &&
               !task->cancelled.load(std::memory_order_relaxed)) {
            if (!m_steamworks.isRunning()) {
                return false;
            }

            m_steamworks.runCallbacks();

            uint64_t bytesDownloaded = 0;
            uint64_t bytesTotal = 0;
            m_steamworks.getItemDownloadInfo(publishedFileId, &bytesDownloaded, &bytesTotal);
            uint32_t state = m_steamworks.getItemState(publishedFileId);

            auto now = std::chrono::steady_clock::now();
            if (bytesDownloaded > lastBytes) {
                lastBytes = bytesDownloaded;
                lastProgressTime = now;
            }

            if (bytesTotal > 0) {
                {
                    std::lock_guard<std::mutex> lk(task->progress_mutex);
                    task->bytes_downloaded = bytesDownloaded;
                    task->bytes_total = bytesTotal;
                    task->progress_pct = static_cast<float>(bytesDownloaded * 100.0 / bytesTotal);
                    task->speed_estimator.update(bytesDownloaded, bytesTotal,
                                                 task->speed_bytes_per_sec, task->eta_seconds);
                }
                task->notifyProgress();
            }

            // k_EItemStateInstalled == 4, k_EItemStateNeedsUpdate == 8
            if ((state & 4u) != 0 && (state & 8u) == 0) {
                uint64_t sizeOnDisk = 0;
                char folder[1024] = {0};
                uint32_t timestamp = 0;
                if (m_steamworks.getItemInstallInfo(publishedFileId, &sizeOnDisk, folder,
                                                    sizeof(folder), &timestamp)) {
                    std::lock_guard<std::mutex> lk(task->progress_mutex);
                    task->installed_path = folder;
                }
                return true;
            }

            if ((now - lastProgressTime) > stallLimit || (now - startTime) > maxOverall) {
                return false;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        return false;
    }

    /* --------------------------------------------------------------------- */
    /* Backend B Execution (Steam Web API + HTTP/2 Reactor Dispatch)         */
    /* --------------------------------------------------------------------- */

    bool querySteamWebMetadata(uint64_t publishedFileId, std::string &out_file_url,
                               uint64_t &out_file_size)
    {
        const std::string postUrl =
            "https://api.steampowered.com/ISteamRemoteStorage/GetPublishedFileDetails/v1/";
        const std::string postData =
            "itemcount=1&publishedfileids%5B0%5D=" + std::to_string(publishedFileId);

        const auto initialHooks = snapshotHooks();
        int max_retries = std::max(4, initialHooks.simulate_rate_limit_count + 1);

        for (int retry = 0; retry < max_retries && m_running.load(std::memory_order_relaxed);
             ++retry) {
            const auto hooks = snapshotHooks();
            if (hooks.simulate_rate_limit_count > 0 &&
                hooks.simulated_429_count <
                    static_cast<uint32_t>(hooks.simulate_rate_limit_count)) {
                recordRateLimitAndGetCount();
                double backoff = calculateFullJitterBackoff(retry, 0.02, 0.2);
                std::this_thread::sleep_for(std::chrono::duration<double>(backoff));
                continue;
            }

            if (hooks.dry_run_mode) {
                out_file_url = "mock://steam_workshop_" + std::to_string(publishedFileId);
                out_file_size = hooks.mock_total_size > 0 ? hooks.mock_total_size : 1048576;
                return true;
            }

            const auto &curl = CurlBinding::instance();
            if (!curl.easy_init || !curl.easy_perform) {
                return false;
            }

            void *easy = curl.easy_init();
            if (!easy) {
                return false;
            }

            std::string responseString;
            auto writeCallback =
                +[](char *ptr, size_t size, size_t nmemb, void *userdata) -> size_t {
                auto *buf = static_cast<std::string *>(userdata);
                size_t total = size * nmemb;
                if (buf->size() + total > 4 * 1024 * 1024) {
                    return 0;
                }
                buf->append(ptr, total);
                return total;
            };

            curl.easy_setopt(easy, CurlBinding::OPT_URL, postUrl.c_str());
            curl.easy_setopt(easy, CurlBinding::OPT_PROTOCOLS_STR, "https");
            curl.easy_setopt(easy, CurlBinding::OPT_REDIR_PROTOCOLS_STR, "https");
            curl.easy_setopt(easy, CurlBinding::OPT_POSTFIELDS, postData.c_str());
            curl.easy_setopt(easy, CurlBinding::OPT_WRITEFUNCTION, writeCallback);
            curl.easy_setopt(easy, CurlBinding::OPT_WRITEDATA, &responseString);
            curl.easy_setopt(easy, CurlBinding::OPT_TIMEOUT, 15L);
            curl.easy_setopt(easy, CurlBinding::OPT_NOSIGNAL, 1L);

            int res = curl.easy_perform(easy);
            long httpCode = 0;
            curl.easy_getinfo(easy, CurlBinding::INFO_RESPONSE_CODE, &httpCode);
            curl.easy_cleanup(easy);

            if (res == 0 && (httpCode == 429 || httpCode == 503)) {
                recordRateLimitAndGetCount();
                double backoff = calculateFullJitterBackoff(retry);
                std::this_thread::sleep_for(std::chrono::duration<double>(backoff));
                continue;
            }

            if (res == 0 && httpCode == 200) {
                QByteArray jsonBytes(responseString.data(),
                                     static_cast<int>(responseString.size()));
                QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
                if (!doc.isNull() && doc.isObject()) {
                    QJsonObject root = doc.object();
                    QJsonObject resp = root.value("response").toObject();
                    QJsonArray details = resp.value("publishedfiledetails").toArray();
                    if (!details.isEmpty()) {
                        QJsonObject item = details.at(0).toObject();
                        if (item.value("result").toInt(0) == 1) {
                            out_file_url = item.value("file_url").toString().toStdString();
                            out_file_size = item.value("file_size").toVariant().toULongLong();
                            return !out_file_url.empty();
                        }
                    }
                }
                return false;
            }

            if (httpCode >= 400 && httpCode < 500) {
                return false;
            }

            double backoff = calculateFullJitterBackoff(retry, 0.05, 1.0);
            std::this_thread::sleep_for(std::chrono::duration<double>(backoff));
        }

        return false;
    }

    bool executeBackendB(const std::shared_ptr<Task> &task)
    {
        std::string downloadUrl;
        uint64_t estimatedSize = 0;

        if (task->request.kind == DownloadRequest::SourceKind::SteamWorkshop) {
            if (!task->request.openhub_download_url.empty()) {
                downloadUrl = task->request.openhub_download_url;
            } else if (!querySteamWebMetadata(task->request.steam_published_file_id, downloadUrl,
                                              estimatedSize)) {
                return false;
            }
        } else {
            downloadUrl = task->request.openhub_download_url;
        }

        if (downloadUrl.empty()) {
            return false;
        }

        std::string dlDir = currentDownloadDir();
        std::string filename;
        if (task->request.kind == DownloadRequest::SourceKind::SteamWorkshop) {
            filename = std::to_string(task->request.steam_published_file_id) + ".bin";
        } else {
            filename = sanitizePackageFilenameStem(task->request.openhub_package_id) + ".wwpkg";
        }

        std::string partPath = dlDir + "/" + filename + ".part";
        std::string finalPath = dlDir + "/" + filename;

        const auto hooks = snapshotHooks();
        bool isMock = hooks.dry_run_mode || (downloadUrl.rfind("mock://", 0) == 0) ||
                      (downloadUrl.rfind("test://", 0) == 0);

        if (!isMock) {
            auto job = std::make_shared<HttpTransferJob>();
            job->task = task;
            job->url = downloadUrl;
            job->part_path = partPath;
            job->final_path = finalPath;
            job->estimated_size = estimatedSize;
            job->is_wwpkg = false;
            job->fast_lane = (task->request.priority == TaskPriority::Immediate);
            job->sync_mtx = std::make_shared<std::mutex>();
            job->sync_cv = std::make_shared<std::condition_variable>();
            job->sync_done = std::make_shared<bool>(false);
            job->sync_ok = std::make_shared<bool>(false);
            job->sync_err = std::make_shared<std::string>();

            {
                std::lock_guard<std::mutex> hlk(m_httpMutex);
                if (job->fast_lane) {
                    m_httpFastQueue.push(job);
                } else {
                    m_httpBulkQueue.push(job);
                }
            }
            wakeReactor();

            std::unique_lock<std::mutex> slk(*job->sync_mtx);
            job->sync_cv->wait(slk, [&] { return *job->sync_done; });
            if (*job->sync_ok && !task->cancelled.load(std::memory_order_relaxed)) {
                std::error_code ec;
                fs::rename(partPath, finalPath, ec);
                if (!ec) {
                    std::lock_guard<std::mutex> lk(task->progress_mutex);
                    task->installed_path = finalPath;
                    return true;
                }
            }
            return false;
        }

        task->state.store(TaskState::Downloading, std::memory_order_relaxed);

        bool downloadSuccess = false;
        constexpr int kMaxRetries = 3;

        for (int attempt = 0;
             attempt < kMaxRetries && !task->cancelled.load(std::memory_order_relaxed) &&
             m_running.load(std::memory_order_relaxed);
             ++attempt) {
            uint64_t offset = 0;
            struct stat st{};
            if (::stat(partPath.c_str(), &st) == 0 && st.st_size > 0) {
                offset = static_cast<uint64_t>(st.st_size);
            }

            int fd = ::open(partPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
            if (fd < 0) {
                return false;
            }
            if (offset > 0) {
                ::lseek(fd, static_cast<off_t>(offset), SEEK_SET);
            }

            {
                std::lock_guard<std::mutex> lk(task->progress_mutex);
                task->bytes_downloaded = offset;
                task->bytes_total = std::max(estimatedSize, offset);
                task->speed_estimator.reset(offset);
            }
            task->notifyProgress();

            bool chunkSuccess = executeMockStream(task, fd, offset, estimatedSize);
            ::close(fd);

            if (chunkSuccess) {
                downloadSuccess = true;
                break;
            }

            if (task->cancelled.load(std::memory_order_relaxed) ||
                (m_preemptActiveTask.load(std::memory_order_relaxed) &&
                 task->request.priority <= TaskPriority::BackgroundUpdate)) {
                break;
            }

            if (attempt + 1 < kMaxRetries) {
                double backoff = calculateFullJitterBackoff(attempt, 0.02, 0.2);
                std::this_thread::sleep_for(std::chrono::duration<double>(backoff));
            }
        }

        if (downloadSuccess && !task->cancelled.load(std::memory_order_relaxed)) {
            std::error_code ec;
            fs::rename(partPath, finalPath, ec);
            if (!ec) {
                std::lock_guard<std::mutex> lk(task->progress_mutex);
                task->installed_path = finalPath;
                return true;
            }
        }
        return false;
    }

    bool executeMockStream(const std::shared_ptr<Task> &task, int fd, uint64_t &offset,
                           uint64_t estimatedSize)
    {
        const auto initialHooks = snapshotHooks();
        uint64_t totalSize = initialHooks.mock_total_size > 0
                                 ? initialHooks.mock_total_size
                                 : (estimatedSize > 0 ? estimatedSize : 1048576);
        {
            std::lock_guard<std::mutex> lk(task->progress_mutex);
            task->bytes_total = totalSize;
        }

        if (offset > 0 && initialHooks.simulate_http_200_on_resume) {
            if (::ftruncate(fd, 0) != 0 || ::lseek(fd, 0, SEEK_SET) < 0) {
                return false;
            }
            offset = 0;
            {
                std::lock_guard<std::mutex> lk(task->progress_mutex);
                task->bytes_downloaded = 0;
            }
            recordTruncation();
        }

        constexpr size_t kChunkSize = 65536;
        std::vector<uint8_t> chunk(kChunkSize);

        while (offset < totalSize && !task->cancelled.load(std::memory_order_relaxed) &&
               m_running.load(std::memory_order_relaxed)) {
            if (m_preemptActiveTask.load(std::memory_order_relaxed) &&
                task->request.priority <= TaskPriority::BackgroundUpdate) {
                return false;
            }

            const auto hooks = snapshotHooks();
            if (hooks.drop_at_percent > 0.0f && hooks.simulated_drop_count == 0) {
                if (offset >= static_cast<uint64_t>(totalSize * hooks.drop_at_percent)) {
                    recordDropCount();
                    return false;
                }
            }

            size_t bytesToWrite = std::min<uint64_t>(kChunkSize, totalSize - offset);
            if (!hooks.mock_data.empty()) {
                for (size_t i = 0; i < bytesToWrite; ++i) {
                    chunk[i] = static_cast<uint8_t>(
                        hooks.mock_data[(offset + i) % hooks.mock_data.size()]);
                }
            } else {
                for (size_t i = 0; i < bytesToWrite; ++i) {
                    chunk[i] = static_cast<uint8_t>((offset + i) & 0xFF);
                }
            }

            if (!m_tokenBucket.consume(bytesToWrite, task->cancelled)) {
                return false;
            }

            ssize_t written = ::write(fd, chunk.data(), bytesToWrite);
            if (written <= 0) {
                return false;
            }

            offset += static_cast<uint64_t>(written);
            {
                std::lock_guard<std::mutex> lk(task->progress_mutex);
                task->bytes_downloaded = offset;
                task->progress_pct = static_cast<float>(offset * 100.0 / totalSize);
                task->speed_estimator.update(offset, totalSize, task->speed_bytes_per_sec,
                                             task->eta_seconds);
            }
            task->notifyProgress();
        }

        return offset >= totalSize;
    }

    /* --------------------------------------------------------------------- */
    /* Backend C Execution (Headless steamcmd Subprocess Runner)             */
    /* --------------------------------------------------------------------- */

    std::string findSteamCmd()
    {
        const auto hooks = snapshotHooks();
        if (!hooks.steamcmd_override_path.empty() &&
            ::access(hooks.steamcmd_override_path.c_str(), X_OK) == 0) {
            return hooks.steamcmd_override_path;
        }

        const char *envOverride = std::getenv("WAYWAL_STEAMCMD_BIN");
        if (envOverride && envOverride[0] != '\0' && ::access(envOverride, X_OK) == 0) {
            return envOverride;
        }

        const char *home = std::getenv("HOME");
        std::vector<std::string> candidates;
        if (home) {
            candidates.push_back(std::string(home) + "/.local/share/Steam/steamcmd.sh");
            candidates.push_back(std::string(home) + "/.steam/steam/steamcmd.sh");
            candidates.push_back(std::string(home) + "/steamcmd/steamcmd.sh");
        }
        candidates.push_back("/usr/bin/steamcmd");
        candidates.push_back("/usr/games/steamcmd");
        candidates.push_back("/usr/local/bin/steamcmd");

        for (const auto &path : candidates) {
            if (::access(path.c_str(), X_OK) == 0) {
                return path;
            }
        }

        const char *envPath = std::getenv("PATH");
        if (envPath) {
            std::istringstream iss(envPath);
            std::string dir;
            while (std::getline(iss, dir, ':')) {
                std::string full = dir + "/steamcmd";
                if (::access(full.c_str(), X_OK) == 0) {
                    return full;
                }
                full = dir + "/steamcmd.sh";
                if (::access(full.c_str(), X_OK) == 0) {
                    return full;
                }
            }
        }
        return "";
    }

    bool executeBackendC(const std::shared_ptr<Task> &task)
    {
        std::string steamCmdPath = findSteamCmd();
        if (steamCmdPath.empty()) {
            return false;
        }

        uint64_t publishedFileId = task->request.steam_published_file_id;
        std::string idStr = std::to_string(publishedFileId);

        int pipefd[2]{-1, -1};
        if (::pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) != 0) {
            return false;
        }

        pid_t pid = ::fork();
        if (pid < 0) {
            ::close(pipefd[0]);
            ::close(pipefd[1]);
            return false;
        }

        if (pid == 0) {
            ::close(pipefd[0]);
            ::dup2(pipefd[1], STDOUT_FILENO);
            ::dup2(pipefd[1], STDERR_FILENO);
            ::close(pipefd[1]);

            ::execlp(steamCmdPath.c_str(), steamCmdPath.c_str(), "+login", "anonymous",
                     "+workshop_download_item", "431960", idStr.c_str(), "validate", "+quit",
                     nullptr);
            ::_exit(127);
        }

        ::close(pipefd[1]);

        {
            std::lock_guard<std::mutex> lk(task->progress_mutex);
            task->state.store(TaskState::Downloading, std::memory_order_relaxed);
            task->speed_estimator.reset(0);
        }

        std::string lineBuffer;
        char readBuf[1024];

        std::regex reProgressBytes(
            R"(Update state \(0x[0-9a-fA-F]+\) downloading, progress:\s*([0-9.]+)%\s*\(([0-9]+)\s*\/\s*([0-9]+)\))");
        std::regex reProgressPct(R"(progress:\s*([0-9.]+)%)");
        std::regex reSuccess(R"rx(Success\.\s*Downloaded item\s*([0-9]+)\s*to\s*"([^"]+)")rx");

        auto handleLine = [&](const std::string &line) {
            if (line.empty()) {
                return;
            }
            std::smatch match;
            if (std::regex_search(line, match, reProgressBytes)) {
                float pct = std::stof(match[1].str());
                uint64_t cur = std::stoull(match[2].str());
                uint64_t tot = std::stoull(match[3].str());
                {
                    std::lock_guard<std::mutex> lk(task->progress_mutex);
                    task->bytes_downloaded = cur;
                    task->bytes_total = tot;
                    task->progress_pct = pct;
                    task->speed_estimator.update(cur, tot, task->speed_bytes_per_sec,
                                                 task->eta_seconds);
                }
                task->notifyProgress();
            } else if (std::regex_search(line, match, reProgressPct)) {
                float pct = std::stof(match[1].str());
                {
                    std::lock_guard<std::mutex> lk(task->progress_mutex);
                    task->progress_pct = pct;
                }
                task->notifyProgress();
            } else if (std::regex_search(line, match, reSuccess)) {
                std::lock_guard<std::mutex> lk(task->progress_mutex);
                task->installed_path = match[2].str();
            }
        };

        bool sawEof = false;
        auto drainStdout = [&]() {
            while (!sawEof) {
                ssize_t n = ::read(pipefd[0], readBuf, sizeof(readBuf));
                if (n > 0) {
                    for (ssize_t i = 0; i < n; ++i) {
                        char c = readBuf[i];
                        if (c == '\n' || c == '\r') {
                            if (!lineBuffer.empty()) {
                                handleLine(lineBuffer);
                                lineBuffer.clear();
                            }
                        } else {
                            lineBuffer.push_back(c);
                        }
                    }
                } else if (n == 0) {
                    sawEof = true;
                    if (!lineBuffer.empty()) {
                        handleLine(lineBuffer);
                        lineBuffer.clear();
                    }
                } else {
                    if (errno == EINTR) {
                        continue;
                    }
                    break;
                }
            }
        };

        bool reaped = false;
        int finalStatus = -1;

        while (m_running.load(std::memory_order_relaxed) &&
               !task->cancelled.load(std::memory_order_relaxed)) {
            struct pollfd pfd{};
            pfd.fd = pipefd[0];
            pfd.events = POLLIN;

            int pollRet = ::poll(&pfd, 1, 50);
            if (pollRet > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
                drainStdout();
            }

            int status = 0;
            pid_t waited = ::waitpid(pid, &status, WNOHANG);
            if (waited == pid) {
                reaped = true;
                finalStatus = status;
                drainStdout();
                break;
            }
            if (waited < 0 && errno != EINTR) {
                break;
            }
            if (sawEof) {
                break;
            }
        }

        auto terminateAndReap = [&]() {
            if (reaped) {
                return;
            }
            ::kill(pid, SIGTERM);
            for (int i = 0; i < 12; ++i) {
                int status = 0;
                pid_t w = ::waitpid(pid, &status, WNOHANG);
                if (w == pid) {
                    reaped = true;
                    finalStatus = status;
                    return;
                }
                if (w < 0 && errno != EINTR) {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            ::kill(pid, SIGKILL);
            int status = 0;
            while (true) {
                pid_t w = ::waitpid(pid, &status, 0);
                if (w == pid) {
                    reaped = true;
                    finalStatus = status;
                    break;
                }
                if (w < 0 && errno != EINTR) {
                    break;
                }
            }
        };

        if (!reaped && (!m_running.load(std::memory_order_relaxed) ||
                        task->cancelled.load(std::memory_order_relaxed))) {
            terminateAndReap();
            ::close(pipefd[0]);
            return false;
        }

        if (!reaped) {
            for (int i = 0; i < 80 && m_running.load(std::memory_order_relaxed) &&
                            !task->cancelled.load(std::memory_order_relaxed);
                 ++i) {
                int status = 0;
                pid_t w = ::waitpid(pid, &status, WNOHANG);
                if (w == pid) {
                    reaped = true;
                    finalStatus = status;
                    break;
                }
                if (w < 0 && errno != EINTR) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            if (!reaped) {
                terminateAndReap();
                ::close(pipefd[0]);
                return false;
            }
        }

        drainStdout();
        ::close(pipefd[0]);

        return reaped && WIFEXITED(finalStatus) && (WEXITSTATUS(finalStatus) == 0);
    }
};

/* ========================================================================= */
/* DownloadManager Public API                                                */
/* ========================================================================= */

DownloadManager::DownloadManager() : m_impl(std::make_unique<Impl>(this)) {}

DownloadManager::~DownloadManager() = default;

DownloadManager::DownloadManager(DownloadManager &&) noexcept = default;
DownloadManager &DownloadManager::operator=(DownloadManager &&) noexcept = default;

bool DownloadManager::initialize() { return m_impl->initialize(); }

void DownloadManager::shutdown() { m_impl->shutdown(); }

void DownloadManager::enqueue(uint64_t published_file_id, TaskPriority priority,
                              ProgressCallback callback)
{
    DownloadRequest req;
    req.kind = DownloadRequest::SourceKind::SteamWorkshop;
    req.steam_published_file_id = published_file_id;
    req.priority = priority;
    m_impl->enqueueTask(req, std::move(callback));
}

void DownloadManager::enqueue(const std::string &package_id_or_cid, const std::string &download_url,
                              TaskPriority priority, ProgressCallback callback)
{
    DownloadRequest req;
    req.kind = DownloadRequest::SourceKind::OpenHubPackage;
    req.openhub_package_id = package_id_or_cid;
    req.openhub_download_url = download_url;
    req.priority = priority;
    m_impl->enqueueTask(req, std::move(callback));
}

void DownloadManager::enqueue(const DownloadRequest &request, ProgressCallback callback)
{
    m_impl->enqueueTask(request, std::move(callback));
}

void DownloadManager::cancel(uint64_t published_file_id) { m_impl->cancelTask(published_file_id); }

void DownloadManager::cancelOpenHub(const std::string &package_id_or_cid)
{
    m_impl->cancelOpenHubTask(package_id_or_cid);
}

void DownloadManager::pause()
{
    m_impl->m_tokenBucket.pause();
    m_impl->wakeReactor();
}

void DownloadManager::resume()
{
    m_impl->m_tokenBucket.resume();
    m_impl->wakeReactor();
}

void DownloadManager::setBandwidthLimit(uint64_t bytes_per_sec)
{
    m_impl->m_tokenBucket.setLimit(bytes_per_sec);
    m_impl->wakeReactor();
}

uint64_t DownloadManager::bandwidthLimit() const noexcept { return m_impl->m_tokenBucket.limit(); }

bool DownloadManager::isSteamClientRunning() const noexcept
{
    return m_impl->m_steamworks.isRunning();
}

size_t DownloadManager::activeTransfers() const noexcept
{
    return m_impl->m_activeTransfers.load(std::memory_order_relaxed);
}

void DownloadManager::setSimulationHooks(const SimulationHooks &hooks)
{
    std::lock_guard<std::mutex> lk(m_impl->m_hooksMutex);
    m_impl->m_simHooks = hooks;
}

DownloadManager::SimulationHooks DownloadManager::simulationHooks() const
{
    return m_impl->snapshotHooks();
}

void DownloadManager::resetSimulationStats()
{
    std::lock_guard<std::mutex> lk(m_impl->m_hooksMutex);
    m_impl->m_simHooks.simulated_drop_count = 0;
    m_impl->m_simHooks.simulated_429_count = 0;
    m_impl->m_simHooks.simulated_truncation_occurred = false;
}

void DownloadManager::setDownloadDirectory(const std::string &dir)
{
    {
        std::lock_guard<std::mutex> lk(m_impl->m_dirMutex);
        m_impl->m_downloadDir = dir;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
}

std::string DownloadManager::downloadDirectory() const { return m_impl->currentDownloadDir(); }

size_t DownloadManager::queuedTasks() const noexcept { return m_impl->queuedTasks(); }

bool DownloadManager::isPaused() const noexcept { return m_impl->m_tokenBucket.isPaused(); }

void DownloadManager::waitUntilIdle() { m_impl->waitUntilIdle(); }

} // namespace wwal::studio
