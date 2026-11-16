// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/queue/download_manager.hpp"

#include "waywal/wwpkg.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
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
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_set>

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

bool parseStrictU64(std::string_view text, uint64_t &out)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ||
                             text.back() == '\n')) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return false;
    }

    uint64_t val = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        if (__builtin_mul_overflow(val, 10ULL, &val) ||
            __builtin_add_overflow(val, static_cast<uint64_t>(ch - '0'), &val)) {
            return false;
        }
    }
    out = val;
    return true;
}

bool parseContentRangeHeader(std::string_view val, uint64_t &startOut, uint64_t &endOut,
                             std::optional<uint64_t> &totalOut)
{
    while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
        val.remove_prefix(1);
    }
    while (!val.empty() &&
           (val.back() == ' ' || val.back() == '\t' || val.back() == '\r' || val.back() == '\n')) {
        val.remove_suffix(1);
    }

    constexpr std::string_view kBytesPrefix = "bytes";
    if (val.size() <= kBytesPrefix.size()) {
        return false;
    }
    for (size_t i = 0; i < kBytesPrefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(val[i])) != kBytesPrefix[i]) {
            return false;
        }
    }
    val.remove_prefix(kBytesPrefix.size());
    if (val.empty() || (val.front() != ' ' && val.front() != '\t')) {
        return false;
    }
    while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
        val.remove_prefix(1);
    }

    const size_t slashPos = val.find('/');
    const size_t dashPos = val.find('-');
    if (dashPos == std::string_view::npos || slashPos == std::string_view::npos ||
        dashPos >= slashPos) {
        return false;
    }

    const std::string_view startSv = val.substr(0, dashPos);
    const std::string_view endSv = val.substr(dashPos + 1, slashPos - dashPos - 1);
    const std::string_view totalSv = val.substr(slashPos + 1);

    uint64_t start = 0;
    uint64_t end = 0;
    if (!parseStrictU64(startSv, start) || !parseStrictU64(endSv, end) || end < start) {
        return false;
    }

    if (totalSv == "*") {
        totalOut.reset();
    } else {
        uint64_t total = 0;
        if (!parseStrictU64(totalSv, total) || end >= total) {
            return false;
        }
        totalOut = total;
    }

    startOut = start;
    endOut = end;
    return true;
}

std::string resolveDefaultDownloadDir()
{
    const char *xdgData = std::getenv("XDG_DATA_HOME");
    if (xdgData && xdgData[0] == '/') {
        return (fs::path(xdgData) / "waywal" / "downloads").lexically_normal().string();
    }
    const char *home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return (fs::path(home) / ".local" / "share" / "waywal" / "downloads")
            .lexically_normal()
            .string();
    }
    return "/tmp/waywal/downloads";
}

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

bool mkdirConfinedDir(int rootDirFd, const std::vector<std::string> &components)
{
    int curDirFd = ::dup(rootDirFd);
    if (curDirFd < 0) {
        return false;
    }

    for (const std::string &comp : components) {
        if (::mkdirat(curDirFd, comp.c_str(), 0755) != 0 && errno != EEXIST) {
            ::close(curDirFd);
            return false;
        }
        int nextFd =
            ::openat(curDirFd, comp.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(curDirFd);
        if (nextFd < 0) {
            return false;
        }
        curDirFd = nextFd;
    }

    ::close(curDirFd);
    return true;
}

bool parsePaxExtendedHeader(const uint8_t *data, size_t len, std::optional<std::string> &pathOut,
                            std::optional<uint64_t> &sizeOut)
{
    constexpr size_t kMaxPaxBytes = 64ULL * 1024ULL;
    if (len > kMaxPaxBytes) {
        return false;
    }

    std::string_view sv(reinterpret_cast<const char *>(data), len);
    size_t pos = 0;
    while (pos < sv.size()) {
        const size_t sp = sv.find(' ', pos);
        if (sp == std::string_view::npos || sp == pos) {
            return false;
        }

        uint64_t recLen64 = 0;
        if (!parseStrictU64(sv.substr(pos, sp - pos), recLen64) || recLen64 < 5 ||
            recLen64 > sv.size() - pos) {
            return false;
        }
        const size_t recLen = static_cast<size_t>(recLen64);
        const std::string_view rec = sv.substr(pos, recLen);
        if (rec.back() != '\n') {
            return false;
        }

        const size_t kvStart = (sp - pos) + 1;
        if (kvStart >= rec.size() - 1) {
            return false;
        }
        const std::string_view kv = rec.substr(kvStart, rec.size() - kvStart - 1);
        const size_t eq = kv.find('=');
        if (eq == std::string_view::npos) {
            return false;
        }
        const std::string_view key = kv.substr(0, eq);
        const std::string_view val = kv.substr(eq + 1);
        if (key == "path") {
            if (val.size() > 4096) {
                return false;
            }
            pathOut = std::string(val);
        } else if (key == "size") {
            uint64_t sz = 0;
            if (!parseStrictU64(val, sz)) {
                return false;
            }
            sizeOut = sz;
        }

        pos += recLen;
    }
    return true;
}

bool extractUstarBufferConfined(const uint8_t *tarData, size_t tarLen, const fs::path &destDir)
{
    constexpr size_t kMaxExtractedBytes = 512ULL * 1024ULL * 1024ULL;
    constexpr size_t kMaxExtractedFiles = 4096;
    constexpr size_t kMaxGnuLongBytes = 4096;

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
    size_t dirCount = 0;
    bool ok = true;

    std::optional<std::string> paxGlobalPath;
    std::optional<uint64_t> paxGlobalSize;
    std::optional<std::string> paxNextPath;
    std::optional<uint64_t> paxNextSize;
    std::optional<std::string> gnuLongName;

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
        uint64_t headerSize = 0;
        if (!parseOctalU64(hdr->size, sizeof(hdr->size), headerSize)) {
            ok = false;
            break;
        }

        const char tf = hdr->typeflag;
        if (headerSize > tarLen - offset - 512) {
            ok = false;
            break;
        }
        uint64_t headerRem511 = 0;
        uint64_t headerBlocks = 0;
        if (__builtin_add_overflow(headerSize, 511ULL, &headerRem511)) {
            ok = false;
            break;
        }
        headerBlocks = headerRem511 / 512ULL;
        uint64_t paddedHeaderPayload = 0;
        if (__builtin_mul_overflow(headerBlocks, 512ULL, &paddedHeaderPayload) ||
            paddedHeaderPayload > tarLen - offset - 512) {
            ok = false;
            break;
        }

        const uint8_t *payload = tarData + offset + 512;
        if (tf == 'x' || tf == 'g') {
            std::optional<std::string> &targetPath = (tf == 'x') ? paxNextPath : paxGlobalPath;
            std::optional<uint64_t> &targetSize = (tf == 'x') ? paxNextSize : paxGlobalSize;
            if (!parsePaxExtendedHeader(payload, static_cast<size_t>(headerSize), targetPath,
                                        targetSize)) {
                ok = false;
                break;
            }
            offset += 512 + static_cast<size_t>(paddedHeaderPayload);
            continue;
        }

        if (tf == 'L' || tf == 'K') {
            if (headerSize == 0 || headerSize > kMaxGnuLongBytes) {
                ok = false;
                break;
            }
            const size_t sz = static_cast<size_t>(headerSize);
            size_t strLen = ::strnlen(reinterpret_cast<const char *>(payload), sz);
            if (tf == 'L') {
                gnuLongName = std::string(reinterpret_cast<const char *>(payload), strLen);
            }
            offset += 512 + static_cast<size_t>(paddedHeaderPayload);
            continue;
        }

        std::string rawName;
        if (paxNextPath.has_value()) {
            rawName = *paxNextPath;
        } else if (gnuLongName.has_value()) {
            rawName = *gnuLongName;
        } else if (paxGlobalPath.has_value()) {
            rawName = *paxGlobalPath;
        } else {
            size_t prefixLen = ::strnlen(hdr->prefix, sizeof(hdr->prefix));
            size_t nameLen = ::strnlen(hdr->name, sizeof(hdr->name));
            if (prefixLen > 0) {
                rawName.assign(hdr->prefix, prefixLen);
                rawName.push_back('/');
            }
            rawName.append(hdr->name, nameLen);
        }

        const uint64_t entrySize = paxNextSize.value_or(paxGlobalSize.value_or(headerSize));

        paxNextPath.reset();
        paxNextSize.reset();
        gnuLongName.reset();

        if (entrySize > tarLen - offset - 512) {
            ok = false;
            break;
        }

        if (tf == '5') {
            if (entrySize != 0 || headerSize != 0) {
                ok = false;
                break;
            }
            std::vector<std::string> comps;
            std::string normRel;
            if (!rawName.empty() && rawName != "." && rawName != "./") {
                if (!sanitizeRelativeArchivePath(rawName, comps, normRel) ||
                    !mkdirConfinedDir(rootDirFd, comps)) {
                    ok = false;
                    break;
                }
                ++dirCount;
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

        size_t nextTotalBytes = 0;
        if (++fileCount > kMaxExtractedFiles || entrySize > kMaxExtractedBytes ||
            __builtin_add_overflow(totalBytes, static_cast<size_t>(entrySize), &nextTotalBytes) ||
            nextTotalBytes > kMaxExtractedBytes) {
            ok = false;
            break;
        }
        totalBytes = nextTotalBytes;

        int outFd = openConfinedWriteFd(rootDirFd, comps, normRel);
        if (outFd < 0) {
            ok = false;
            break;
        }

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

        offset += 512 + static_cast<size_t>(paddedHeaderPayload);
    }

    ::close(rootDirFd);
    return ok && (fileCount > 0 || dirCount > 0);
}

bool verifyWwpkgSignatureAndTrust(const wwpkg_package_t *pkg, bool &outIsTrusted)
{
    outIsTrusted = false;
    if (!pkg) {
        return false;
    }

    // Consult $XDG_CONFIG_HOME/waywal/trusted_keys.json when overridden so creator-exported trust
    // stores work seamlessly under custom XDG environments as well as default ~/.config/waywal.
    const char *xdgCfg = ::getenv("XDG_CONFIG_HOME");
    if (xdgCfg && xdgCfg[0] != '\0') {
        std::error_code ec;
        const fs::path xdgTrustPath = fs::path(xdgCfg) / "waywal" / "trusted_keys.json";
        if (fs::exists(xdgTrustPath, ec) && !ec) {
            const wwpkg_trust_level_t lvl =
                wwpkg_verify_trust_with_store(pkg, xdgTrustPath.c_str());
            if (lvl == WWPKG_TRUST_INVALID) {
                return false;
            }
            outIsTrusted = (lvl == WWPKG_TRUST_AUTHOR_TRUSTED || lvl == WWPKG_TRUST_NOTARIZED);
            return true;
        }
    }

    return wwpkg_verify_signature(pkg, &outIsTrusted);
}

enum class VerifyInstallResult {
    Ok,
    CorruptContainer,
    SignatureFailed,
    ExtractFailed,
    FinalizeFailed
};

VerifyInstallResult verifyExtractAndInstallWwpkg(const std::string &partPath,
                                                 const std::string &finalPath,
                                                 const fs::path &extractDir, bool &outIsTrusted)
{
    outIsTrusted = false;

    const int pkgFd = ::open(partPath.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (pkgFd < 0) {
        return VerifyInstallResult::CorruptContainer;
    }

    struct stat openSt{};
    if (::fstat(pkgFd, &openSt) != 0 || !S_ISREG(openSt.st_mode)) {
        ::close(pkgFd);
        return VerifyInstallResult::CorruptContainer;
    }

    wwpkg_package_t *pkg = wwpkg_open_fd(pkgFd);
    if (!pkg) {
        ::close(pkgFd);
        return VerifyInstallResult::CorruptContainer;
    }

    bool isTrusted = false;
    const bool sigValid = verifyWwpkgSignatureAndTrust(pkg, isTrusted);
    if (!sigValid) {
        wwpkg_close(pkg);
        ::close(pkgFd);
        return VerifyInstallResult::SignatureFailed;
    }

    std::error_code ec;
    fs::remove_all(extractDir, ec);
    const bool extractedOk = wwpkg_extract_all(pkg, extractDir.c_str());
    wwpkg_close(pkg);

    if (!extractedOk) {
        ::close(pkgFd);
        fs::remove_all(extractDir, ec);
        return VerifyInstallResult::ExtractFailed;
    }

    // Re-check path inode identity under O_NOFOLLOW before finalizing while pkgFd keeps the inode
    // alive.
    struct stat curSt{};
    if (::lstat(partPath.c_str(), &curSt) != 0 || S_ISLNK(curSt.st_mode) ||
        curSt.st_dev != openSt.st_dev || curSt.st_ino != openSt.st_ino) {
        ::close(pkgFd);
        fs::remove_all(extractDir, ec);
        return VerifyInstallResult::FinalizeFailed;
    }

    std::error_code renameEc;
    fs::rename(partPath, finalPath, renameEc);
    ::close(pkgFd);
    if (renameEc) {
        fs::remove_all(extractDir, ec);
        return VerifyInstallResult::FinalizeFailed;
    }

    outIsTrusted = isTrusted;
    return VerifyInstallResult::Ok;
}

bool envLoopbackHttpAllowed()
{
    const char *env = std::getenv("WAYWAL_ALLOW_LOOPBACK_HTTP");
    return env != nullptr && std::strcmp(env, "1") == 0;
}

bool parseSchemeAndHost(std::string_view url, std::string &schemeOut, std::string &hostOut)
{
    for (char ch : url) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (uch <= 0x20 || uch == 0x7F || ch == '\\') {
            return false;
        }
    }

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

bool isPrivateOrReservedIpv4(uint32_t ip, bool &isLoopbackOut)
{
    isLoopbackOut = ((ip & 0xFF000000u) == 0x7F000000u); // 127.0.0.0/8
    if (isLoopbackOut) {
        return true;
    }
    if ((ip & 0xFF000000u) == 0x00000000u || // 0.0.0.0/8
        (ip & 0xFF000000u) == 0x0A000000u || // 10.0.0.0/8
        (ip & 0xFFC00000u) == 0x64400000u || // 100.64.0.0/10 (CGNAT)
        (ip & 0xFFFF0000u) == 0xA9FE0000u || // 169.254.0.0/16 (Link-local/IMDS)
        (ip & 0xFFF00000u) == 0xAC100000u || // 172.16.0.0/12
        (ip & 0xFFFFFF00u) == 0xC0000000u || // 192.0.0.0/24
        (ip & 0xFFFFFF00u) == 0xC0000200u || // 192.0.2.0/24
        (ip & 0xFFFF0000u) == 0xC0A80000u || // 192.168.0.0/16
        (ip & 0xFFFE0000u) == 0xC6120000u || // 198.18.0.0/15
        (ip & 0xFFFFFF00u) == 0xC6336400u || // 198.51.100.0/24
        (ip & 0xFFFFFF00u) == 0xCB007100u || // 203.0.113.0/24
        (ip & 0xE0000000u) == 0xE0000000u) { // 224.0.0.0/4 + 240.0.0.0/4 + broadcast
        return true;
    }
    return false;
}

bool isObfuscatedNumericIpv4(std::string_view host)
{
    if (host.empty()) {
        return false;
    }
    size_t parts = 0;
    size_t pos = 0;
    while (pos <= host.size()) {
        const size_t dot = host.find('.', pos);
        const std::string_view seg =
            (dot == std::string_view::npos) ? host.substr(pos) : host.substr(pos, dot - pos);
        if (seg.empty()) {
            return false;
        }
        ++parts;
        if (seg.rfind("0x", 0) == 0) {
            if (seg.size() <= 2) {
                return false;
            }
            for (char c : seg.substr(2)) {
                if (!std::isxdigit(static_cast<unsigned char>(c))) {
                    return false;
                }
            }
        } else {
            for (char c : seg) {
                if (!std::isdigit(static_cast<unsigned char>(c))) {
                    return false;
                }
            }
        }
        if (dot == std::string_view::npos) {
            break;
        }
        pos = dot + 1;
    }
    return parts >= 1 && parts <= 4;
}

bool isRestrictedIpv6(const struct in6_addr &addr, bool &isLoopbackOut)
{
    isLoopbackOut = false;
    if (IN6_IS_ADDR_UNSPECIFIED(&addr) || IN6_IS_ADDR_LINKLOCAL(&addr) ||
        IN6_IS_ADDR_MULTICAST(&addr) || (addr.s6_addr[0] & 0xFEu) == 0xFCu) {
        return true;
    }
    if (IN6_IS_ADDR_LOOPBACK(&addr)) {
        isLoopbackOut = true;
        return true;
    }
    if (IN6_IS_ADDR_V4MAPPED(&addr) || IN6_IS_ADDR_V4COMPAT(&addr)) {
        const uint32_t v4 = (static_cast<uint32_t>(addr.s6_addr[12]) << 24) |
                            (static_cast<uint32_t>(addr.s6_addr[13]) << 16) |
                            (static_cast<uint32_t>(addr.s6_addr[14]) << 8) |
                            static_cast<uint32_t>(addr.s6_addr[15]);
        return isPrivateOrReservedIpv4(v4, isLoopbackOut);
    }
    // Block Teredo (2001::/32), 6to4 (2002::/16), and NAT64 well-known prefix (64:ff9b::/96).
    if ((addr.s6_addr[0] == 0x20 && addr.s6_addr[1] == 0x01 && addr.s6_addr[2] == 0x00 &&
         addr.s6_addr[3] == 0x00) ||
        (addr.s6_addr[0] == 0x20 && addr.s6_addr[1] == 0x02) ||
        (addr.s6_addr[0] == 0x00 && addr.s6_addr[1] == 0x64 && addr.s6_addr[2] == 0xff &&
         addr.s6_addr[3] == 0x9b && addr.s6_addr[4] == 0x00 && addr.s6_addr[5] == 0x00 &&
         addr.s6_addr[6] == 0x00 && addr.s6_addr[7] == 0x00 && addr.s6_addr[8] == 0x00 &&
         addr.s6_addr[9] == 0x00 && addr.s6_addr[10] == 0x00 && addr.s6_addr[11] == 0x00)) {
        return true;
    }
    return false;
}

bool endsWithHostSuffix(std::string_view host, std::string_view suffix)
{
    return host.size() > suffix.size() && host.substr(host.size() - suffix.size()) == suffix;
}

bool isAllowedDownloadUrl(std::string_view url, bool allowLoopbackHttp)
{
    std::string scheme;
    std::string host;
    if (!parseSchemeAndHost(url, scheme, host)) {
        return false;
    }
    if (scheme != "https" && !(scheme == "http" && allowLoopbackHttp)) {
        return false;
    }

    std::string_view cleanHost(host);
    while (!cleanHost.empty() && cleanHost.back() == '.') {
        cleanHost.remove_suffix(1);
    }
    if (cleanHost.empty()) {
        return false;
    }

    if (cleanHost.front() == '[' && cleanHost.back() == ']') {
        std::string_view inner = cleanHost.substr(1, cleanHost.size() - 2);
        const size_t pct = inner.find('%');
        if (pct != std::string_view::npos) {
            inner = inner.substr(0, pct);
        }
        std::string innerStr(inner);
        struct in6_addr addr6{};
        if (::inet_pton(AF_INET6, innerStr.c_str(), &addr6) != 1) {
            return false;
        }
        bool isLoopback = false;
        if (isRestrictedIpv6(addr6, isLoopback)) {
            return allowLoopbackHttp && isLoopback;
        }
        return scheme == "https";
    }

    std::string hostStr(cleanHost);
    struct in_addr addr4{};
    if (::inet_pton(AF_INET, hostStr.c_str(), &addr4) == 1) {
        bool isLoopback = false;
        if (isPrivateOrReservedIpv4(ntohl(addr4.s_addr), isLoopback)) {
            return allowLoopbackHttp && isLoopback;
        }
        return scheme == "https";
    }

    if (isObfuscatedNumericIpv4(cleanHost)) {
        return false;
    }

    if (cleanHost == "localhost" || endsWithHostSuffix(cleanHost, ".localhost")) {
        return allowLoopbackHttp;
    }
    if (cleanHost == "local" || endsWithHostSuffix(cleanHost, ".local") ||
        cleanHost == "internal" || endsWithHostSuffix(cleanHost, ".internal")) {
        return false;
    }

    return scheme == "https";
}

bool isAllowedRedirectLocation(std::string_view locRaw, bool allowLoopbackHttp)
{
    while (!locRaw.empty() && (locRaw.front() == ' ' || locRaw.front() == '\t')) {
        locRaw.remove_prefix(1);
    }
    while (!locRaw.empty() && (locRaw.back() == ' ' || locRaw.back() == '\t' ||
                               locRaw.back() == '\r' || locRaw.back() == '\n')) {
        locRaw.remove_suffix(1);
    }
    if (locRaw.empty()) {
        return false;
    }
    for (char ch : locRaw) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (uch <= 0x20 || uch == 0x7F || ch == '\\') {
            return false;
        }
    }

    // Reject protocol-relative network-path redirects (`//host/path`).
    if (locRaw.size() >= 2 && locRaw[0] == '/' && locRaw[1] == '/') {
        return false;
    }
    if (locRaw.front() == '/') {
        return true;
    }
    return isAllowedDownloadUrl(locRaw, allowLoopbackHttp);
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

    enum class Opt : int {
        WRITEDATA = 10001,
        URL = 10002,
        RANGE = 10007,
        POSTFIELDS = 10015,
        USERAGENT = 10018,
        HTTPHEADER = 10023,
        HEADERDATA = 10029,
        PRIVATE = 10103,
        PROTOCOLS_STR = 10318,
        REDIR_PROTOCOLS_STR = 10319,
        WRITEFUNCTION = 20011,
        HEADERFUNCTION = 20079,
        TIMEOUT = 13,
        FOLLOWLOCATION = 52,
        MAXREDIRS = 68,
        CONNECTTIMEOUT = 78,
        HTTP_VERSION = 84,
        NOSIGNAL = 99,
        PIPEWAIT = 237
    };

    enum class Info : int { EFFECTIVE_URL = 1048577, PRIVATE = 1048597, RESPONSE_CODE = 2097154 };

    enum class MultiOpt : int {
        SOCKETDATA = 10002,
        TIMERDATA = 10005,
        PIPELINING = 3,
        MAX_HOST_CONNECTIONS = 7,
        MAX_TOTAL_CONNECTIONS = 13,
        SOCKETFUNCTION = 20001,
        TIMERFUNCTION = 20004
    };

    using socket_callback_fn = int (*)(CURL *easy, curl_socket_t s, int what, void *userp,
                                       void *socketp);
    using timer_callback_fn = int (*)(CURLM *multi, long timeout_ms, void *userp);

    using global_init_fn = CURLcode (*)(long);
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
    global_init_fn global_init{nullptr};
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

    static constexpr long VAL_GLOBAL_DEFAULT = 3L;
    static constexpr long VAL_HTTP_VERSION_2TLS = 4;
    static constexpr size_t WRITEFUNC_PAUSE = 0x10000001u;
    static constexpr int PAUSE_RECV = (1 << 0);
    static constexpr int PAUSE_CONT = 0;
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

    bool globalInitOk{false};

    template <typename Arg> CURLcode setOpt(CURL *easy, Opt opt, Arg arg) const
    {
        return easy_setopt(easy, static_cast<int>(opt), arg);
    }

    template <typename Arg> CURLcode getInfo(CURL *easy, Info info, Arg arg) const
    {
        return easy_getinfo(easy, static_cast<int>(info), arg);
    }

    template <typename Arg> CURLMcode multiSetOpt(CURLM *multi, MultiOpt opt, Arg arg) const
    {
        return multi_setopt(multi, static_cast<int>(opt), arg);
    }

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
            sym(binding.global_init, "curl_global_init");
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

            if (binding.global_init) {
                binding.globalInitOk = (binding.global_init(VAL_GLOBAL_DEFAULT) == 0);
            } else {
                binding.globalInitOk = true;
            }
        });
        return binding;
    }

    [[nodiscard]] bool available() const noexcept
    {
        return globalInitOk && easy_init && easy_cleanup && easy_setopt && easy_getinfo &&
               easy_pause && multi_init && multi_cleanup && multi_add_handle &&
               multi_remove_handle && multi_socket_action && multi_info_read && multi_setopt;
    }
};

/* ========================================================================= */
/* Internal Task & Transfer Structures                                       */
/* ========================================================================= */

struct Task {
    DownloadRequest request;
    ProgressCallback callback;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> immediate_counted{false};
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
    bool is_trusted{false};

    SpeedEstimator speed_estimator;

    void notifyProgress()
    {
        ProgressCallback cb;
        DownloadProgress prog;
        {
            std::lock_guard<std::mutex> lk(progress_mutex);
            if (!callback) {
                return;
            }
            cb = callback;
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
            prog.is_trusted = is_trusted;
        }
        cb(prog);
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
    std::atomic<bool> preempted{false};
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
    std::atomic<bool> m_reactorAlive{false};

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
    std::atomic<uint32_t> m_pendingImmediateCount{0};

    std::mutex m_partMutex;
    std::unordered_map<std::string, uint64_t> m_partOwners;

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
    std::mutex m_wakeMutex;
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
        uint64_t content_length{0};
        bool has_content_length{false};
        bool has_authoritative_length{false};
        bool has_content_range{false};
        bool content_range_wildcard{false};
        uint64_t content_range_start{0};
        uint64_t content_range_end{0};
        std::optional<uint64_t> content_range_total;
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
        std::string protocol_error_msg;
        bool recv_paused{false};
        uint64_t paused_epoch{0};
        std::chrono::steady_clock::time_point throttle_until{};

        void resetResponseHeaders(long statusCode)
        {
            last_http_code = statusCode;
            retry_after_sec = -1;
            expected_total = 0;
            content_length = 0;
            has_content_length = false;
            has_authoritative_length = false;
            has_content_range = false;
            content_range_wildcard = false;
            content_range_start = 0;
            content_range_end = 0;
            content_range_total.reset();
            protocol_error = false;
            protocol_error_msg.clear();
        }
    };

    bool tryClaimPartPath(const std::string &partPath, uint64_t ownerSeq)
    {
        if (partPath.empty()) {
            return true;
        }
        std::lock_guard<std::mutex> lk(m_partMutex);
        auto [it, inserted] = m_partOwners.emplace(partPath, ownerSeq);
        return inserted || it->second == ownerSeq;
    }

    void releasePartClaim(const std::string &partPath, uint64_t ownerSeq)
    {
        if (partPath.empty()) {
            return;
        }
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(m_partMutex);
            auto it = m_partOwners.find(partPath);
            if (it != m_partOwners.end() && it->second == ownerSeq) {
                m_partOwners.erase(it);
                changed = true;
            }
        }
        if (changed) {
            wakeReactor();
        }
    }

    [[nodiscard]] bool shouldPreemptTask(const std::shared_ptr<Task> &task) const noexcept
    {
        if (!task || task->request.priority > TaskPriority::BackgroundUpdate) {
            return false;
        }
        return m_preemptActiveTask.load(std::memory_order_acquire) ||
               m_pendingImmediateCount.load(std::memory_order_acquire) > 0;
    }

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
        std::lock_guard<std::mutex> lk(m_wakeMutex);
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
        {
            ProgressCallback droppedCb;
            std::lock_guard<std::mutex> lk(task->progress_mutex);
            droppedCb = std::move(task->callback);
        }

        m_inFlightTasks.fetch_sub(1, std::memory_order_acq_rel);
        std::lock_guard<std::mutex> qlk(m_queueMutex);
        if (m_allTasks.size() > 64) {
            std::erase_if(m_allTasks, [](const std::shared_ptr<Task> &t) {
                const TaskState st = t->state.load(std::memory_order_relaxed);
                return st == TaskState::Installed || st == TaskState::Failed ||
                       st == TaskState::Cancelled;
            });
        }
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
                m_downloadDir = resolveDefaultDownloadDir();
            }
            std::error_code ec;
            fs::create_directories(m_downloadDir, ec);
        }

        m_pendingImmediateCount.store(0, std::memory_order_release);
        m_preemptActiveTask.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> plk(m_partMutex);
            m_partOwners.clear();
        }

        m_tokenBucket.resetForStart();
        m_steamworks.initialize(431960);
        (void)CurlBinding::instance();

        m_epollFd = ::epoll_create1(EPOLL_CLOEXEC);
        m_timerFd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        const int wakeFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        {
            std::lock_guard<std::mutex> lk(m_wakeMutex);
            m_wakeEventFd = wakeFd;
        }
        if (m_epollFd < 0 || m_timerFd < 0 || wakeFd < 0) {
            closeReactorFds();
            return false;
        }

        epoll_event evTimer{};
        evTimer.events = EPOLLIN;
        evTimer.data.fd = m_timerFd;
        ::epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_timerFd, &evTimer);

        epoll_event evWake{};
        evWake.events = EPOLLIN;
        evWake.data.fd = wakeFd;
        ::epoll_ctl(m_epollFd, EPOLL_CTL_ADD, wakeFd, &evWake);

        m_running.store(true, std::memory_order_release);
        m_reactorAlive.store(true, std::memory_order_release);
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
        {
            std::lock_guard<std::mutex> lk(m_wakeMutex);
            if (m_wakeEventFd >= 0) {
                ::close(m_wakeEventFd);
                m_wakeEventFd = -1;
            }
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

        // Join the arbitration producer before the HTTP reactor consumer so any
        // late Backend B fallback submission wakes deterministically.
        if (m_arbThread.joinable()) {
            m_arbThread.join();
        }
        wakeReactor();
        if (m_reactorThread.joinable()) {
            m_reactorThread.join();
        }
        m_verifyCv.notify_all();
        for (auto &vt : m_verifyThreads) {
            if (vt.joinable()) {
                vt.join();
            }
        }
        m_verifyThreads.clear();

        closeReactorFds();
        m_steamworks.shutdown();

        m_pendingImmediateCount.store(0, std::memory_order_release);
        m_preemptActiveTask.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> plk(m_partMutex);
            m_partOwners.clear();
        }

        std::lock_guard<std::mutex> qlk(m_queueMutex);
        m_allTasks.clear();
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

            bool enqueued = false;
            {
                std::lock_guard<std::mutex> hlk(m_httpMutex);
                if (m_running.load(std::memory_order_acquire) &&
                    !task->cancelled.load(std::memory_order_relaxed) &&
                    m_reactorAlive.load(std::memory_order_acquire)) {
                    if (job->fast_lane) {
                        m_httpFastQueue.push(job);
                    } else {
                        m_httpBulkQueue.push(job);
                    }
                    enqueued = true;
                }
            }
            if (!enqueued) {
                finishHttpJobOutcome(job, false, "Download engine not running",
                                     task->cancelled.load(std::memory_order_relaxed) ||
                                         !m_running.load(std::memory_order_acquire));
                return;
            }
            wakeReactor();
            return;
        }

        bool pushed = false;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_running.load(std::memory_order_acquire) &&
                !task->cancelled.load(std::memory_order_acquire)) {
                if (request.priority == TaskPriority::Immediate) {
                    task->immediate_counted.store(true, std::memory_order_release);
                    m_pendingImmediateCount.fetch_add(1, std::memory_order_acq_rel);
                    const int activePrio = m_currentActivePriority.load(std::memory_order_acquire);
                    if (activePrio > 0 &&
                        activePrio <= static_cast<int>(TaskPriority::BackgroundUpdate)) {
                        m_preemptActiveTask.store(true, std::memory_order_release);
                    }
                }
                m_arbQueue.push(task);
                m_queueCv.notify_all();
                pushed = true;
            }
        }
        if (!pushed) {
            markTaskTerminal(task, TaskState::Failed, "Download engine not running");
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
                    if (t->immediate_counted.exchange(false, std::memory_order_acq_rel)) {
                        m_pendingImmediateCount.fetch_sub(1, std::memory_order_acq_rel);
                    }
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
                    if (t->immediate_counted.exchange(false, std::memory_order_acq_rel)) {
                        m_pendingImmediateCount.fetch_sub(1, std::memory_order_acq_rel);
                    }
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
                    slot->resetResponseHeaders(code);
                }
            }
            return total;
        }

        if (startsWithCaseInsensitive(line, "Location:")) {
            std::string_view loc = line.substr(9);
            if (!isAllowedRedirectLocation(loc, slot->allow_loopback_http)) {
                slot->ssrf_rejected = true;
                return 0;
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

        if (startsWithCaseInsensitive(line, "Content-Range:")) {
            if (slot->last_http_code == 206) {
                if (slot->has_content_range) {
                    slot->protocol_error = true;
                    slot->protocol_error_msg = "Duplicate HTTP 206 Content-Range header";
                    return 0;
                }
                std::string_view val = line.substr(14);
                uint64_t crStart = 0;
                uint64_t crEnd = 0;
                std::optional<uint64_t> crTotal;
                if (!parseContentRangeHeader(val, crStart, crEnd, crTotal) ||
                    crStart != slot->initial_offset) {
                    slot->protocol_error = true;
                    slot->protocol_error_msg =
                        "Malformed or mismatched HTTP 206 Content-Range header";
                    return 0;
                }

                uint64_t rangeSpan = 0;
                uint64_t rangeEndPlusOne = 0;
                if (__builtin_add_overflow(crEnd - crStart, uint64_t{1}, &rangeSpan) ||
                    __builtin_add_overflow(crStart, rangeSpan, &rangeEndPlusOne)) {
                    slot->protocol_error = true;
                    slot->protocol_error_msg = "HTTP 206 Content-Range integer overflow";
                    return 0;
                }
                if (slot->has_content_length && slot->content_length != rangeSpan) {
                    slot->protocol_error = true;
                    slot->protocol_error_msg =
                        "HTTP 206 Content-Length does not match Content-Range span";
                    return 0;
                }

                slot->has_content_range = true;
                slot->content_range_start = crStart;
                slot->content_range_end = crEnd;
                slot->content_range_total = crTotal;
                slot->expected_total = crTotal.value_or(rangeEndPlusOne);
                slot->has_authoritative_length = true;
                {
                    std::lock_guard<std::mutex> lk(slot->job->task->progress_mutex);
                    slot->job->task->bytes_total = slot->expected_total;
                }
            }
            return total;
        }

        if (startsWithCaseInsensitive(line, "Content-Length:")) {
            if (slot->last_http_code == 200 || slot->last_http_code == 206) {
                std::string_view val = line.substr(15);
                uint64_t contentLen = 0;
                if (!parseStrictU64(val, contentLen)) {
                    slot->protocol_error = true;
                    slot->protocol_error_msg = "Malformed or overflowing Content-Length header";
                    return 0;
                }
                if (slot->has_content_length && slot->content_length != contentLen) {
                    slot->protocol_error = true;
                    slot->protocol_error_msg = "Conflicting Content-Length headers";
                    return 0;
                }

                uint64_t computedTotal = contentLen;
                if (slot->last_http_code == 206) {
                    if (__builtin_add_overflow(slot->initial_offset, contentLen, &computedTotal)) {
                        slot->protocol_error = true;
                        slot->protocol_error_msg = "HTTP 206 offset + Content-Length overflow";
                        return 0;
                    }
                    if (slot->has_content_range) {
                        uint64_t rangeSpan = 0;
                        if (__builtin_add_overflow(slot->content_range_end -
                                                       slot->content_range_start,
                                                   uint64_t{1}, &rangeSpan) ||
                            rangeSpan != contentLen) {
                            slot->protocol_error = true;
                            slot->protocol_error_msg =
                                "HTTP 206 Content-Length does not match Content-Range span";
                            return 0;
                        }
                        if (slot->content_range_total.has_value()) {
                            computedTotal = *slot->content_range_total;
                        }
                    }
                }

                slot->content_length = contentLen;
                slot->has_content_length = true;
                slot->expected_total = computedTotal;
                slot->has_authoritative_length = true;
                {
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
            slot->job->preempted.load(std::memory_order_acquire) ||
            !slot->self->m_running.load(std::memory_order_relaxed)) {
            return 0;
        }

        const auto &curl = CurlBinding::instance();
        long httpCode = slot->last_http_code;
        if (httpCode == 0) {
            curl.getInfo(slot->easy, CurlBinding::Info::RESPONSE_CODE, &httpCode);
            slot->last_http_code = httpCode;
        }

        const char *effectiveUrl = nullptr;
        if (curl.getInfo(slot->easy, CurlBinding::Info::EFFECTIVE_URL, &effectiveUrl) == 0 &&
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

        if (httpCode == 206) {
            if (slot->initial_offset == 0) {
                slot->protocol_error = true;
                slot->protocol_error_msg = "Unexpected HTTP 206 response without Range request";
                return 0;
            }
            if (!slot->has_content_range) {
                slot->protocol_error = true;
                slot->protocol_error_msg = "HTTP 206 response missing valid Content-Range header";
                return 0;
            }
            uint64_t rangeSpan = (slot->content_range_end - slot->content_range_start) + 1;
            uint64_t alreadyReceived = slot->current_offset - slot->initial_offset;
            uint64_t nextReceived = 0;
            if (__builtin_add_overflow(alreadyReceived, static_cast<uint64_t>(bytes),
                                       &nextReceived) ||
                nextReceived > rangeSpan) {
                slot->protocol_error = true;
                slot->protocol_error_msg = "HTTP 206 payload exceeded declared Content-Range span";
                return 0;
            }
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
        if (std::isfinite(hooks.drop_at_percent) && hooks.drop_at_percent > 0.0f &&
            hooks.drop_at_percent <= 1.0f && hooks.simulated_drop_count == 0 &&
            slot->expected_total > 0) {
            const double threshold = static_cast<double>(slot->expected_total) *
                                     static_cast<double>(hooks.drop_at_percent);
            if (std::isfinite(threshold) && threshold >= 0.0 &&
                static_cast<double>(slot->current_offset) >= threshold) {
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

        uint64_t nextOffset = 0;
        if (__builtin_add_overflow(slot->current_offset, static_cast<uint64_t>(writtenAll),
                                   &nextOffset)) {
            slot->protocol_error = true;
            slot->protocol_error_msg = "HTTP payload offset overflow";
            return 0;
        }
        slot->current_offset = nextOffset;
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

        const uint64_t seqId = job->task->sequence_id;
        if (wasCancelled) {
            releasePartClaim(job->part_path, seqId);
            markTaskTerminal(job->task, TaskState::Cancelled,
                             errMsg.empty() ? "Download cancelled by user" : errMsg);
            return;
        }

        if (!ok) {
            releasePartClaim(job->part_path, seqId);
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
            releasePartClaim(job->part_path, seqId);
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

        if (job->preempted.load(std::memory_order_acquire)) {
            finishHttpJobOutcome(job, false, "Preempted by immediate task");
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

        bool optOk = true;
        auto setChecked = [&](CurlBinding::Opt opt, auto value) {
            if (curl.setOpt(easy, opt, value) != 0) {
                optOk = false;
            }
        };

        setChecked(CurlBinding::Opt::URL, job->url.c_str());
        setChecked(CurlBinding::Opt::PROTOCOLS_STR, allowedProtocols);
        setChecked(CurlBinding::Opt::REDIR_PROTOCOLS_STR, allowedProtocols);
        setChecked(CurlBinding::Opt::FOLLOWLOCATION, 1L);
        setChecked(CurlBinding::Opt::MAXREDIRS, 5L);
        setChecked(CurlBinding::Opt::HTTP_VERSION, CurlBinding::VAL_HTTP_VERSION_2TLS);
        // Coalesce bulk HTTPS transfers onto multiplexed HTTP/2 streams, while keeping
        // the dedicated Immediate fast lane and cleartext loopback HTTP free of pipe-wait blocking.
        setChecked(CurlBinding::Opt::PIPEWAIT, (isHttps && !useFastLaneSlot) ? 1L : 0L);
        setChecked(CurlBinding::Opt::NOSIGNAL, 1L);
        setChecked(CurlBinding::Opt::CONNECTTIMEOUT, 15L);
        setChecked(CurlBinding::Opt::TIMEOUT, 300L);
        setChecked(CurlBinding::Opt::USERAGENT, "WayWal-Studio/3.0");
        setChecked(CurlBinding::Opt::HEADERFUNCTION, &Impl::onCurlHeader);
        setChecked(CurlBinding::Opt::HEADERDATA, slot.get());
        setChecked(CurlBinding::Opt::WRITEFUNCTION, &Impl::onCurlWrite);
        setChecked(CurlBinding::Opt::WRITEDATA, slot.get());

        if (offset > 0) {
            slot->range_header = std::to_string(offset) + "-";
            setChecked(CurlBinding::Opt::RANGE, slot->range_header.c_str());
        }

        if (!optOk) {
            ::close(fd);
            curl.easy_cleanup(easy);
            finishHttpJobOutcome(job, false, "libcurl rejected required HTTPS/transfer option");
            return false;
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
        std::vector<std::shared_ptr<HttpTransferJob>> readyRetries;
        while (!m_retryHeap.empty() && m_retryHeap.top().deadline <= now) {
            readyRetries.push_back(m_retryHeap.top().job);
            m_retryHeap.pop();
        }
        if (!readyRetries.empty()) {
            std::vector<std::shared_ptr<HttpTransferJob>> cancelledRetries;
            {
                std::lock_guard<std::mutex> hlk(m_httpMutex);
                for (auto &readyJob : readyRetries) {
                    if (!m_running.load(std::memory_order_acquire) ||
                        readyJob->task->cancelled.load(std::memory_order_relaxed) ||
                        readyJob->preempted.load(std::memory_order_acquire)) {
                        cancelledRetries.push_back(std::move(readyJob));
                    } else if (readyJob->fast_lane) {
                        m_httpFastQueue.push(std::move(readyJob));
                    } else {
                        m_httpBulkQueue.push(std::move(readyJob));
                    }
                }
            }
            for (auto &cj : cancelledRetries) {
                const bool isPreempt = cj->preempted.load(std::memory_order_acquire) &&
                                       !cj->task->cancelled.load(std::memory_order_relaxed);
                finishHttpJobOutcome(cj, false,
                                     isPreempt ? "Preempted by immediate task"
                                               : "Download cancelled before retry",
                                     !isPreempt);
            }
        }

        std::vector<std::shared_ptr<HttpTransferJob>> deferred;
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
            if (nextJob->task->cancelled.load(std::memory_order_relaxed) ||
                nextJob->preempted.load(std::memory_order_acquire)) {
                const bool isPreempt = nextJob->preempted.load(std::memory_order_acquire) &&
                                       !nextJob->task->cancelled.load(std::memory_order_relaxed);
                finishHttpJobOutcome(nextJob, false,
                                     isPreempt ? "Preempted by immediate task"
                                               : "Download cancelled in queue",
                                     !isPreempt);
                continue;
            }
            if (!tryClaimPartPath(nextJob->part_path, nextJob->task->sequence_id)) {
                deferred.push_back(std::move(nextJob));
                continue;
            }
            launchHttpSlot(nextJob, useFastSlot);
        }

        if (!deferred.empty()) {
            std::lock_guard<std::mutex> hlk(m_httpMutex);
            for (auto &job : deferred) {
                if (job->fast_lane) {
                    m_httpFastQueue.push(std::move(job));
                } else {
                    m_httpBulkQueue.push(std::move(job));
                }
            }
        }
    }

    void cancelActiveFlaggedSlots()
    {
        const auto &curl = CurlBinding::instance();
        std::vector<CurlBinding::CURL *> toCancel;
        for (const auto &[easy, slot] : m_activeSlots) {
            if (slot->job->task->cancelled.load(std::memory_order_relaxed) ||
                slot->job->preempted.load(std::memory_order_acquire)) {
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
            const bool isPreempt = slot->job->preempted.load(std::memory_order_acquire) &&
                                   !slot->job->task->cancelled.load(std::memory_order_relaxed);
            finishHttpJobOutcome(slot->job, false,
                                 isPreempt ? "Preempted by immediate task"
                                           : "Download cancelled by user",
                                 !isPreempt);
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
                curl.getInfo(easy, CurlBinding::Info::RESPONSE_CODE, &httpCode);
            }
            const char *effUrl = nullptr;
            if (curl.getInfo(easy, CurlBinding::Info::EFFECTIVE_URL, &effUrl) == 0 && effUrl) {
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
            if (job->preempted.load(std::memory_order_acquire)) {
                finishHttpJobOutcome(job, false, "Preempted by immediate task");
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
                if (slot->initial_offset == 0) {
                    ::unlink(job->part_path.c_str());
                }
                finishHttpJobOutcome(job, false,
                                     slot->protocol_error_msg.empty()
                                         ? "Malformed HTTP protocol framing"
                                         : slot->protocol_error_msg);
                continue;
            }

            if (httpCode == 206 && (!slot->has_content_range || slot->initial_offset == 0)) {
                if (slot->initial_offset == 0) {
                    ::unlink(job->part_path.c_str());
                }
                finishHttpJobOutcome(job, false,
                                     "HTTP 206 response missing valid Content-Range header");
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

            const bool validSuccessCode =
                (httpCode == 200) ||
                (httpCode == 206 && slot->initial_offset > 0 && slot->has_content_range);
            const bool rangeSpanComplete =
                !(httpCode == 206 && slot->has_content_length &&
                  (slot->current_offset - slot->initial_offset) != slot->content_length);
            const bool completeBody =
                (!slot->has_authoritative_length || slot->current_offset == slot->expected_total) &&
                (slot->expected_total == 0 || slot->current_offset >= slot->expected_total) &&
                rangeSpanComplete;
            if (curlResult == 0 && !slot->drop_triggered && validSuccessCode && completeBody) {
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
            std::string reason;
            if ((httpCode == 200 || httpCode == 206) && (!completeBody || curlResult == 18)) {
                reason = "Unexpected EOF / Content-Length mismatch";
            } else if (httpCode > 0) {
                reason = "HTTP transfer failed with status " + std::to_string(httpCode);
            } else {
                reason = "HTTP network transfer failed after retries";
            }
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
                curl.multiSetOpt(m_multi, CurlBinding::MultiOpt::PIPELINING,
                                 CurlBinding::VAL_PIPE_MULTIPLEX);
                curl.multiSetOpt(m_multi, CurlBinding::MultiOpt::MAX_HOST_CONNECTIONS, 6L);
                curl.multiSetOpt(m_multi, CurlBinding::MultiOpt::MAX_TOTAL_CONNECTIONS, 8L);
                curl.multiSetOpt(m_multi, CurlBinding::MultiOpt::SOCKETFUNCTION,
                                 &Impl::onCurlSocket);
                curl.multiSetOpt(m_multi, CurlBinding::MultiOpt::SOCKETDATA, this);
                curl.multiSetOpt(m_multi, CurlBinding::MultiOpt::TIMERFUNCTION, &Impl::onCurlTimer);
                curl.multiSetOpt(m_multi, CurlBinding::MultiOpt::TIMERDATA, this);
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
                    if (!m_socketEvents.contains(fd)) {
                        continue;
                    }
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
            m_reactorAlive.store(false, std::memory_order_release);
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

            const uint64_t ownerSeq = job->task->sequence_id;
            if (job->task->cancelled.load(std::memory_order_relaxed) ||
                !m_running.load(std::memory_order_acquire)) {
                ::unlink(job->part_path.c_str());
                releasePartClaim(job->part_path, ownerSeq);
                markTaskTerminal(job->task, TaskState::Cancelled, "Verification cancelled");
                continue;
            }

            job->task->state.store(TaskState::Verifying, std::memory_order_relaxed);
            job->task->notifyProgress();

            std::string stem = sanitizePackageFilenameStem(job->task->request.openhub_package_id);
            fs::path extractDir = fs::path(currentDownloadDir()) / "extracted" / stem;
            bool isTrusted = false;
            const VerifyInstallResult res = verifyExtractAndInstallWwpkg(
                job->part_path, job->final_path, extractDir, isTrusted);
            releasePartClaim(job->part_path, ownerSeq);

            if (res != VerifyInstallResult::Ok) {
                ::unlink(job->part_path.c_str());
                const char *errMsg = "Invalid or corrupt .wwpkg container";
                if (res == VerifyInstallResult::SignatureFailed) {
                    errMsg = "Cryptographic Ed25519 or Merkle verification failed";
                } else if (res == VerifyInstallResult::ExtractFailed) {
                    errMsg = "Confined .wwpkg extraction failed";
                } else if (res == VerifyInstallResult::FinalizeFailed) {
                    errMsg = "Failed to move verified .wwpkg into destination";
                }
                markTaskTerminal(job->task, TaskState::Failed, errMsg);
                continue;
            }

            {
                std::lock_guard<std::mutex> lk(job->task->progress_mutex);
                job->task->installed_path = job->final_path;
                job->task->extracted_dir = extractDir.string();
                job->task->is_trusted = isTrusted;
            }

            job->task->state.store(TaskState::Extracting, std::memory_order_relaxed);
            job->task->notifyProgress();
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

            if (currentTask->immediate_counted.exchange(false, std::memory_order_acq_rel)) {
                m_pendingImmediateCount.fetch_sub(1, std::memory_order_acq_rel);
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
                auto t = m_arbQueue.top();
                m_arbQueue.pop();
                if (t && t->immediate_counted.exchange(false, std::memory_order_acq_rel)) {
                    m_pendingImmediateCount.fetch_sub(1, std::memory_order_acq_rel);
                }
                remaining.push_back(std::move(t));
            }
        }
        m_pendingImmediateCount.store(0, std::memory_order_release);
        for (auto &t : remaining) {
            markTaskTerminal(t, TaskState::Cancelled, "Shutdown in progress");
        }
    }

    void executeArbitratedTask(const std::shared_ptr<Task> &task)
    {
        m_currentActivePriority.store(static_cast<int>(task->request.priority),
                                      std::memory_order_release);
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
                m_running.load(std::memory_order_acquire) && !shouldPreemptTask(task)) {
                if (!hooks.force_backend_failure_b) {
                    std::string bErr;
                    if (executeBackendB(task, &bErr)) {
                        success = true;
                    } else {
                        failureReason += bErr.empty() ? "Backend B failed; "
                                                      : ("Backend B failed (" + bErr + "); ");
                    }
                } else {
                    failureReason += "Backend B skipped; ";
                }
            }

            if (!success && !task->cancelled.load(std::memory_order_relaxed) &&
                m_running.load(std::memory_order_acquire) && !shouldPreemptTask(task)) {
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
            std::string bErr;
            if (executeBackendB(task, &bErr)) {
                success = true;
            } else {
                failureReason = bErr.empty() ? "Open Hub download failed" : bErr;
            }
        }

        if (task->request.priority == TaskPriority::Immediate &&
            m_pendingImmediateCount.load(std::memory_order_acquire) == 0) {
            m_preemptActiveTask.store(false, std::memory_order_release);
        }

        if (task->cancelled.load(std::memory_order_relaxed)) {
            m_currentActivePriority.store(0, std::memory_order_release);
            markTaskTerminal(task, TaskState::Cancelled, "Download cancelled by user");
            return;
        }

        if (shouldPreemptTask(task) && !success && m_running.load(std::memory_order_acquire)) {
            task->state.store(TaskState::Queued, std::memory_order_relaxed);
            task->notifyProgress();
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_arbQueue.push(task);
                m_queueCv.notify_all();
            }
            if (m_pendingImmediateCount.load(std::memory_order_acquire) == 0) {
                m_preemptActiveTask.store(false, std::memory_order_release);
            }
            m_currentActivePriority.store(0, std::memory_order_release);
            return;
        }

        m_currentActivePriority.store(0, std::memory_order_release);
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
               !task->cancelled.load(std::memory_order_relaxed) && !shouldPreemptTask(task)) {
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
            if (!curl.globalInitOk || !curl.easy_init || !curl.easy_perform || !curl.easy_setopt ||
                !curl.easy_getinfo || !curl.easy_cleanup) {
                return false;
            }

            CurlBinding::CURL *easy = curl.easy_init();
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

            bool optOk = true;
            auto setChecked = [&](CurlBinding::Opt opt, auto value) {
                if (curl.setOpt(easy, opt, value) != 0) {
                    optOk = false;
                }
            };
            setChecked(CurlBinding::Opt::URL, postUrl.c_str());
            setChecked(CurlBinding::Opt::PROTOCOLS_STR, "https");
            setChecked(CurlBinding::Opt::REDIR_PROTOCOLS_STR, "https");
            setChecked(CurlBinding::Opt::POSTFIELDS, postData.c_str());
            setChecked(CurlBinding::Opt::WRITEFUNCTION, writeCallback);
            setChecked(CurlBinding::Opt::WRITEDATA, &responseString);
            setChecked(CurlBinding::Opt::TIMEOUT, 15L);
            setChecked(CurlBinding::Opt::NOSIGNAL, 1L);

            if (!optOk) {
                curl.easy_cleanup(easy);
                return false;
            }

            int res = curl.easy_perform(easy);
            long httpCode = 0;
            curl.getInfo(easy, CurlBinding::Info::RESPONSE_CODE, &httpCode);
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

    bool executeBackendB(const std::shared_ptr<Task> &task, std::string *outErr = nullptr)
    {
        if (shouldPreemptTask(task)) {
            return false;
        }

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

        if (downloadUrl.empty() || shouldPreemptTask(task)) {
            return false;
        }

        auto urlHasWwpkgSuffix = [](std::string_view url) {
            const size_t q = url.find_first_of("?#");
            std::string_view pathPart = (q == std::string_view::npos) ? url : url.substr(0, q);
            constexpr std::string_view kExt = ".wwpkg";
            if (pathPart.size() < kExt.size()) {
                return false;
            }
            std::string_view tail = pathPart.substr(pathPart.size() - kExt.size());
            for (size_t i = 0; i < kExt.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(tail[i])) != kExt[i]) {
                    return false;
                }
            }
            return true;
        };

        const std::string dlDir = currentDownloadDir();
        const std::string stem =
            (task->request.kind == DownloadRequest::SourceKind::SteamWorkshop)
                ? std::to_string(task->request.steam_published_file_id)
                : sanitizePackageFilenameStem(task->request.openhub_package_id);
        const bool expectWwpkg =
            (task->request.kind == DownloadRequest::SourceKind::OpenHubPackage) ||
            urlHasWwpkgSuffix(downloadUrl);
        const std::string ext = expectWwpkg ? ".wwpkg" : ".bin";
        const std::string partPath = dlDir + "/" + stem + ext + ".part";
        const std::string finalPath = dlDir + "/" + stem + ext;

        while (!tryClaimPartPath(partPath, task->sequence_id)) {
            if (!m_running.load(std::memory_order_acquire) ||
                task->cancelled.load(std::memory_order_relaxed) || shouldPreemptTask(task)) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        struct PartClaimGuard {
            Impl *self;
            std::string path;
            uint64_t seq;
            ~PartClaimGuard() { self->releasePartClaim(path, seq); }
        } claimGuard{this, partPath, task->sequence_id};

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

            bool enqueued = false;
            {
                std::lock_guard<std::mutex> hlk(m_httpMutex);
                if (m_running.load(std::memory_order_acquire) &&
                    !task->cancelled.load(std::memory_order_relaxed) &&
                    m_reactorAlive.load(std::memory_order_acquire)) {
                    if (job->fast_lane) {
                        m_httpFastQueue.push(job);
                    } else {
                        m_httpBulkQueue.push(job);
                    }
                    enqueued = true;
                }
            }
            if (!enqueued) {
                return false;
            }
            wakeReactor();

            std::unique_lock<std::mutex> slk(*job->sync_mtx);
            while (!*job->sync_done && m_reactorAlive.load(std::memory_order_acquire)) {
                if (shouldPreemptTask(task) || task->cancelled.load(std::memory_order_relaxed) ||
                    !m_running.load(std::memory_order_acquire)) {
                    if (!task->cancelled.load(std::memory_order_relaxed) &&
                        shouldPreemptTask(task)) {
                        job->preempted.store(true, std::memory_order_release);
                    }
                    wakeReactor();
                    job->sync_cv->wait(slk, [&] {
                        return *job->sync_done || !m_reactorAlive.load(std::memory_order_acquire);
                    });
                    break;
                }
                job->sync_cv->wait_for(slk, std::chrono::milliseconds(25));
            }
            if (!*job->sync_ok || task->cancelled.load(std::memory_order_relaxed) ||
                job->preempted.load(std::memory_order_acquire)) {
                if (outErr && !job->sync_err->empty()) {
                    *outErr = *job->sync_err;
                }
                return false;
            }

            const int pkgFd = ::open(partPath.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (pkgFd < 0) {
                return false;
            }
            struct stat openSt{};
            if (::fstat(pkgFd, &openSt) != 0 || !S_ISREG(openSt.st_mode)) {
                ::close(pkgFd);
                ::unlink(partPath.c_str());
                return false;
            }

            auto verifySameInode = [&]() {
                struct stat curSt{};
                return ::lstat(partPath.c_str(), &curSt) == 0 && !S_ISLNK(curSt.st_mode) &&
                       curSt.st_dev == openSt.st_dev && curSt.st_ino == openSt.st_ino;
            };

            if (wwpkg_package_t *pkg = wwpkg_open_fd(pkgFd)) {
                bool isTrusted = false;
                const bool sigValid = verifyWwpkgSignatureAndTrust(pkg, isTrusted);
                if (!sigValid) {
                    wwpkg_close(pkg);
                    ::close(pkgFd);
                    ::unlink(partPath.c_str());
                    if (outErr) {
                        *outErr = "Cryptographic Ed25519 or Merkle verification failed";
                    }
                    return false;
                }

                fs::path extractDir = fs::path(dlDir) / "extracted" / stem;
                std::error_code ec;
                fs::remove_all(extractDir, ec);
                const bool extractedOk = wwpkg_extract_all(pkg, extractDir.c_str());
                wwpkg_close(pkg);

                if (!extractedOk || !verifySameInode()) {
                    ::close(pkgFd);
                    fs::remove_all(extractDir, ec);
                    ::unlink(partPath.c_str());
                    if (outErr) {
                        *outErr = "Confined .wwpkg extraction failed";
                    }
                    return false;
                }

                const std::string wwpkgFinal = dlDir + "/" + stem + ".wwpkg";
                std::error_code renameEc;
                fs::rename(partPath, wwpkgFinal, renameEc);
                ::close(pkgFd);
                if (renameEc) {
                    fs::remove_all(extractDir, ec);
                    ::unlink(partPath.c_str());
                    return false;
                }

                std::lock_guard<std::mutex> lk(task->progress_mutex);
                task->installed_path = wwpkgFinal;
                task->extracted_dir = extractDir.string();
                task->is_trusted = isTrusted;
                return true;
            }

            if (expectWwpkg) {
                ::close(pkgFd);
                ::unlink(partPath.c_str());
                if (outErr) {
                    *outErr = "Invalid or corrupt .wwpkg container";
                }
                return false;
            }

            if (openSt.st_size >= 512 &&
                openSt.st_size <= static_cast<off_t>(128ULL * 1024ULL * 1024ULL)) {
                std::array<uint8_t, 512> hdr{};
                if (::pread(pkgFd, hdr.data(), hdr.size(), 0) == 512 &&
                    std::memcmp(hdr.data() + 257, "ustar", 5) == 0) {
                    const size_t tarSize = static_cast<size_t>(openSt.st_size);
                    std::vector<uint8_t> tarBuf(tarSize);
                    size_t got = 0;
                    bool readOk = true;
                    while (got < tarSize) {
                        ssize_t r = ::pread(pkgFd, tarBuf.data() + got, tarSize - got,
                                            static_cast<off_t>(got));
                        if (r < 0) {
                            if (errno == EINTR) {
                                continue;
                            }
                            readOk = false;
                            break;
                        }
                        if (r == 0) {
                            readOk = false;
                            break;
                        }
                        got += static_cast<size_t>(r);
                    }
                    fs::path extractDir = fs::path(dlDir) / "extracted" / stem;
                    std::error_code ec;
                    fs::remove_all(extractDir, ec);
                    const bool okTar = readOk &&
                                       extractUstarBufferConfined(tarBuf.data(), got, extractDir) &&
                                       verifySameInode();
                    const std::string tarFinal = dlDir + "/" + stem + ".tar";
                    std::error_code renameEc;
                    if (okTar) {
                        fs::rename(partPath, tarFinal, renameEc);
                    }
                    ::close(pkgFd);
                    if (!okTar || renameEc) {
                        fs::remove_all(extractDir, ec);
                        ::unlink(partPath.c_str());
                        if (outErr) {
                            *outErr = "Confined tar archive extraction failed";
                        }
                        return false;
                    }
                    std::lock_guard<std::mutex> lk(task->progress_mutex);
                    task->installed_path = tarFinal;
                    task->extracted_dir = extractDir.string();
                    return true;
                }
            }

            const bool sameInode = verifySameInode();
            std::error_code ec;
            if (sameInode) {
                fs::rename(partPath, finalPath, ec);
            }
            ::close(pkgFd);
            if (sameInode && !ec) {
                std::lock_guard<std::mutex> lk(task->progress_mutex);
                task->installed_path = finalPath;
                return true;
            }
            ::unlink(partPath.c_str());
            return false;
        }

        task->state.store(TaskState::Downloading, std::memory_order_relaxed);

        bool downloadSuccess = false;
        constexpr int kMaxRetries = 3;

        for (int attempt = 0;
             attempt < kMaxRetries && !task->cancelled.load(std::memory_order_relaxed) &&
             m_running.load(std::memory_order_relaxed) && !shouldPreemptTask(task);
             ++attempt) {
            uint64_t offset = 0;
            struct stat st{};
            if (::stat(partPath.c_str(), &st) == 0 && st.st_size > 0) {
                offset = static_cast<uint64_t>(st.st_size);
            }

            int fd = ::open(partPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
            if (fd < 0) {
                return false;
            }
            if (offset > 0 && ::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) {
                ::close(fd);
                return false;
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

            if (task->cancelled.load(std::memory_order_relaxed) || shouldPreemptTask(task)) {
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
            if (shouldPreemptTask(task)) {
                return false;
            }

            const auto hooks = snapshotHooks();
            if (std::isfinite(hooks.drop_at_percent) && hooks.drop_at_percent > 0.0f &&
                hooks.drop_at_percent <= 1.0f && hooks.simulated_drop_count == 0) {
                const double threshold =
                    static_cast<double>(totalSize) * static_cast<double>(hooks.drop_at_percent);
                if (std::isfinite(threshold) && threshold >= 0.0 &&
                    static_cast<double>(offset) >= threshold) {
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
        if (shouldPreemptTask(task)) {
            return false;
        }

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
            (void)::setpgid(0, 0);
            ::close(pipefd[0]);
            ::dup2(pipefd[1], STDOUT_FILENO);
            ::dup2(pipefd[1], STDERR_FILENO);
            ::close(pipefd[1]);

            ::execlp(steamCmdPath.c_str(), steamCmdPath.c_str(), "+login", "anonymous",
                     "+workshop_download_item", "431960", idStr.c_str(), "validate", "+quit",
                     nullptr);
            ::_exit(127);
        }

        (void)::setpgid(pid, pid);
        ::close(pipefd[1]);

        {
            std::lock_guard<std::mutex> lk(task->progress_mutex);
            task->state.store(TaskState::Downloading, std::memory_order_relaxed);
            task->speed_estimator.reset(0);
        }

        std::string lineBuffer;
        char readBuf[1024];

        auto parseFloatPrefix = [](std::string_view sv, float &out, size_t &consumed) noexcept {
            if (sv.empty()) {
                return false;
            }
            const char *first = sv.data();
            const char *last = first + sv.size();
            auto [ptr, ec] = std::from_chars(first, last, out);
            if (ec != std::errc{} || !std::isfinite(out) || ptr == first) {
                return false;
            }
            consumed = static_cast<size_t>(ptr - first);
            return true;
        };
        auto parseU64Prefix = [](std::string_view sv, uint64_t &out, size_t &consumed) noexcept {
            if (sv.empty()) {
                return false;
            }
            const char *first = sv.data();
            const char *last = first + sv.size();
            auto [ptr, ec] = std::from_chars(first, last, out, 10);
            if (ec != std::errc{} || ptr == first) {
                return false;
            }
            consumed = static_cast<size_t>(ptr - first);
            return true;
        };
        auto skipSpaces = [](std::string_view sv, size_t &pos) noexcept {
            while (pos < sv.size() && (sv[pos] == ' ' || sv[pos] == '\t')) {
                ++pos;
            }
        };

        auto handleLine = [&](const std::string &line) {
            if (line.empty()) {
                return;
            }
            std::string_view sv(line);
            const size_t progPos = sv.find("progress:");
            if (progPos != std::string_view::npos) {
                size_t p = progPos + 9;
                skipSpaces(sv, p);
                float pct = 0.0f;
                size_t len = 0;
                if (parseFloatPrefix(sv.substr(p), pct, len) && p + len < sv.size() &&
                    sv[p + len] == '%' && std::isfinite(pct) && pct >= 0.0f && pct <= 100.0f) {
                    p += len + 1;
                    skipSpaces(sv, p);
                    if (sv.find("Update state (0x") != std::string_view::npos &&
                        sv.find("downloading, progress:") != std::string_view::npos &&
                        p < sv.size() && sv[p] == '(') {
                        ++p;
                        skipSpaces(sv, p);
                        uint64_t cur = 0;
                        uint64_t tot = 0;
                        if (parseU64Prefix(sv.substr(p), cur, len)) {
                            p += len;
                            skipSpaces(sv, p);
                            if (p < sv.size() && sv[p] == '/') {
                                ++p;
                                skipSpaces(sv, p);
                                if (parseU64Prefix(sv.substr(p), tot, len)) {
                                    {
                                        std::lock_guard<std::mutex> lk(task->progress_mutex);
                                        task->bytes_downloaded = cur;
                                        task->bytes_total = tot;
                                        task->progress_pct = pct;
                                        task->speed_estimator.update(
                                            cur, tot, task->speed_bytes_per_sec, task->eta_seconds);
                                    }
                                    task->notifyProgress();
                                    return;
                                }
                            }
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lk(task->progress_mutex);
                        task->progress_pct = pct;
                    }
                    task->notifyProgress();
                    return;
                }
            }
            if (sv.find("Success.") != std::string_view::npos) {
                const size_t dlPos = sv.find("Downloaded item");
                if (dlPos != std::string_view::npos) {
                    const size_t q1 = sv.find('"', dlPos);
                    const size_t q2 = (q1 != std::string_view::npos) ? sv.find('"', q1 + 1)
                                                                     : std::string_view::npos;
                    if (q1 != std::string_view::npos && q2 != std::string_view::npos &&
                        q2 > q1 + 1) {
                        std::lock_guard<std::mutex> lk(task->progress_mutex);
                        task->installed_path = std::string(sv.substr(q1 + 1, q2 - q1 - 1));
                    }
                }
            }
        };

        constexpr size_t kMaxLineBytes = 16384;
        constexpr size_t kMaxStdoutBytes = 16ULL * 1024ULL * 1024ULL;
        size_t totalStdoutBytes = 0;
        bool dropLongLine = false;
        bool sawEof = false;
        auto drainStdout = [&]() {
            while (!sawEof) {
                ssize_t n = ::read(pipefd[0], readBuf, sizeof(readBuf));
                if (n > 0) {
                    if (totalStdoutBytes >= kMaxStdoutBytes) {
                        lineBuffer.clear();
                        continue;
                    }
                    totalStdoutBytes += static_cast<size_t>(n);
                    for (ssize_t i = 0; i < n; ++i) {
                        char c = readBuf[i];
                        if (c == '\n' || c == '\r') {
                            if (!dropLongLine && !lineBuffer.empty()) {
                                handleLine(lineBuffer);
                            }
                            lineBuffer.clear();
                            dropLongLine = false;
                        } else if (!dropLongLine) {
                            if (lineBuffer.size() < kMaxLineBytes) {
                                lineBuffer.push_back(c);
                            } else {
                                dropLongLine = true;
                                lineBuffer.clear();
                            }
                        }
                    }
                } else if (n == 0) {
                    sawEof = true;
                    if (!dropLongLine && !lineBuffer.empty()) {
                        handleLine(lineBuffer);
                    }
                    lineBuffer.clear();
                    dropLongLine = false;
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
               !task->cancelled.load(std::memory_order_relaxed) && !shouldPreemptTask(task)) {
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
            if (::kill(-pid, SIGTERM) != 0) {
                (void)::kill(pid, SIGTERM);
            }
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
            if (::kill(-pid, SIGKILL) != 0) {
                (void)::kill(pid, SIGKILL);
            }
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

        if (!reaped &&
            (!m_running.load(std::memory_order_relaxed) ||
             task->cancelled.load(std::memory_order_relaxed) || shouldPreemptTask(task))) {
            terminateAndReap();
            ::close(pipefd[0]);
            return false;
        }

        if (!reaped) {
            for (int i = 0;
                 i < 12 && m_running.load(std::memory_order_relaxed) &&
                 !task->cancelled.load(std::memory_order_relaxed) && !shouldPreemptTask(task);
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
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
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

DownloadManager::DownloadManager(DownloadManager &&other) noexcept : m_impl(std::move(other.m_impl))
{
    if (m_impl) {
        m_impl->q_ptr = this;
    }
}

DownloadManager &DownloadManager::operator=(DownloadManager &&other) noexcept
{
    if (this != &other) {
        m_impl = std::move(other.m_impl);
        if (m_impl) {
            m_impl->q_ptr = this;
        }
    }
    return *this;
}

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
