// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/core/recursive_fs_watcher.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

namespace wwal::studio {

namespace {

constexpr size_t kMaxWatchDepth = 2;
constexpr size_t kMaxPendingMoves = 1024;
constexpr uint64_t kOverflowToken = 1;
constexpr std::chrono::milliseconds kHardDebounceCap{1000};

std::filesystem::path normalizeLexicalPath(const std::filesystem::path &raw)
{
    if (raw.empty()) {
        return {};
    }
    std::error_code ec;
    std::filesystem::path absPath = std::filesystem::absolute(raw, ec);
    std::filesystem::path norm = (ec ? raw : absPath).lexically_normal();
    std::string s = norm.native();
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
    if (s.size() != norm.native().size()) {
        norm = std::filesystem::path(std::move(s));
    }
    return norm;
}

bool isPathWithinOrEqual(const std::filesystem::path &candidate, const std::filesystem::path &base)
{
    if (candidate.empty() || base.empty()) {
        return false;
    }
    std::filesystem::path normCand = normalizeLexicalPath(candidate);
    std::filesystem::path normBase = normalizeLexicalPath(base);
    std::filesystem::path rel = normCand.lexically_relative(normBase);
    if (rel.empty()) {
        return false;
    }
    return (*rel.begin()) != "..";
}

std::optional<size_t> computeRelativeDepth(const std::filesystem::path &candidate,
                                           const std::filesystem::path &root)
{
    std::filesystem::path normCand = normalizeLexicalPath(candidate);
    std::filesystem::path normRoot = normalizeLexicalPath(root);
    std::filesystem::path rel = normCand.lexically_relative(normRoot);
    if (rel.empty() || (*rel.begin()) == "..") {
        return std::nullopt;
    }
    if (rel == ".") {
        return 0;
    }
    size_t depth = 0;
    for (const auto &part : rel) {
        if (!part.empty() && part != ".") {
            ++depth;
        }
    }
    return depth;
}

bool parsePositiveUint64(std::string_view text, uint64_t &outId)
{
    if (text.empty()) {
        return false;
    }
    uint64_t val = 0;
    for (char ch : text) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
        uint64_t digit = static_cast<uint64_t>(ch - '0');
        if (val > (UINT64_MAX - digit) / 10ULL) {
            return false;
        }
        val = val * 10ULL + digit;
    }
    if (val == 0) {
        return false;
    }
    outId = val;
    return true;
}

uint64_t extractPublishedFileId(const std::filesystem::path &fullPath,
                                const std::vector<std::filesystem::path> &watchRoots)
{
    const std::filesystem::path normFull = normalizeLexicalPath(fullPath);

    for (const auto &root : watchRoots) {
        std::filesystem::path rel = normFull.lexically_relative(root);
        if (rel.empty() || rel == "." || (*rel.begin()) == "..") {
            continue;
        }

        auto it = rel.begin();
        std::string firstComp = it->string();
        if (firstComp == "431960") {
            ++it;
            if (it == rel.end()) {
                return 0;
            }
            firstComp = it->string();
        }

        uint64_t parsed = 0;
        if (parsePositiveUint64(firstComp, parsed)) {
            return parsed;
        }

        for (const auto &part : rel) {
            std::string partStr = part.string();
            if (partStr == "431960" || partStr.empty()) {
                continue;
            }
            if (parsePositiveUint64(partStr, parsed)) {
                return parsed;
            }
        }

        if (!firstComp.empty()) {
            return std::hash<std::string>{}(firstComp);
        }
    }

    for (auto it = normFull.end(); it != normFull.begin();) {
        --it;
        std::string comp = it->string();
        if (comp == "431960" || comp == "content" || comp == "workshop" || comp == "steamapps" ||
            comp.empty()) {
            continue;
        }
        uint64_t parsed = 0;
        if (parsePositiveUint64(comp, parsed)) {
            return parsed;
        }
    }

    uint64_t stemId = 0;
    if (parsePositiveUint64(normFull.stem().string(), stemId)) {
        return stemId;
    }

    return 0;
}

std::filesystem::path resolveItemRootDirectory(const std::filesystem::path &fullPath,
                                               const std::vector<std::filesystem::path> &watchRoots,
                                               uint64_t publishedFileId)
{
    const std::filesystem::path normFull = normalizeLexicalPath(fullPath);

    for (const auto &root : watchRoots) {
        std::filesystem::path rel = normFull.lexically_relative(root);
        if (rel.empty() || rel == "." || (*rel.begin()) == "..") {
            continue;
        }

        auto it = rel.begin();
        std::string firstComp = it->string();
        if (firstComp == "431960") {
            ++it;
            if (it != rel.end()) {
                return normalizeLexicalPath(root / "431960" / it->string());
            }
            return normalizeLexicalPath(root / "431960");
        }
        return normalizeLexicalPath(root / firstComp);
    }

    if (publishedFileId > 0) {
        std::string idStr = std::to_string(publishedFileId);
        std::filesystem::path cur = normFull;
        while (!cur.empty() && cur != cur.parent_path()) {
            if (cur.filename() == idStr) {
                return cur;
            }
            cur = cur.parent_path();
        }
    }

    return normFull;
}

std::optional<std::filesystem::path>
findOwningRoot(const std::filesystem::path &candidate,
               const std::vector<std::filesystem::path> &watchRoots)
{
    for (const auto &root : watchRoots) {
        if (isPathWithinOrEqual(candidate, root)) {
            return root;
        }
    }
    return std::nullopt;
}

enum class DeadlineKind : uint8_t { DebounceItem, PendingMove, Overflow };

struct DeadlineNode {
    std::chrono::steady_clock::time_point deadline;
    uint64_t token{0};
};

struct TokenMeta {
    DeadlineKind kind{DeadlineKind::DebounceItem};
    std::string debounceKey;
    uint32_t moveCookie{0};
    size_t heapIndex{0};
};

class IndexedDeadlineHeap {
  public:
    [[nodiscard]] bool empty() const noexcept { return m_heap.empty(); }

    [[nodiscard]] std::chrono::steady_clock::time_point topDeadline() const noexcept
    {
        return m_heap.front().deadline;
    }

    [[nodiscard]] const DeadlineNode &topNode() const noexcept { return m_heap.front(); }

    [[nodiscard]] const TokenMeta *findMeta(uint64_t token) const noexcept
    {
        auto it = m_meta.find(token);
        return it != m_meta.end() ? &it->second : nullptr;
    }

    void upsert(uint64_t token, DeadlineKind kind, std::chrono::steady_clock::time_point deadline,
                std::string debounceKey = {}, uint32_t moveCookie = 0)
    {
        auto it = m_meta.find(token);
        if (it != m_meta.end()) {
            const size_t idx = it->second.heapIndex;
            m_heap[idx].deadline = deadline;
            it->second.kind = kind;
            it->second.debounceKey = std::move(debounceKey);
            it->second.moveCookie = moveCookie;
            siftUp(idx);
            siftDown(it->second.heapIndex);
            return;
        }

        const size_t idx = m_heap.size();
        m_heap.push_back(DeadlineNode{.deadline = deadline, .token = token});
        m_meta.emplace(token, TokenMeta{.kind = kind,
                                        .debounceKey = std::move(debounceKey),
                                        .moveCookie = moveCookie,
                                        .heapIndex = idx});
        siftUp(idx);
    }

    void erase(uint64_t token)
    {
        auto it = m_meta.find(token);
        if (it == m_meta.end()) {
            return;
        }
        const size_t idx = it->second.heapIndex;
        const size_t lastIdx = m_heap.size() - 1;
        if (idx != lastIdx) {
            m_heap[idx] = m_heap[lastIdx];
            m_meta[m_heap[idx].token].heapIndex = idx;
        }
        m_heap.pop_back();
        m_meta.erase(it);
        if (idx < m_heap.size()) {
            siftUp(idx);
            auto movedIt = m_meta.find(m_heap[idx].token);
            if (movedIt != m_meta.end()) {
                siftDown(movedIt->second.heapIndex);
            }
        }
    }

    void clear() noexcept
    {
        m_heap.clear();
        m_meta.clear();
    }

  private:
    void swapNodes(size_t a, size_t b) noexcept
    {
        std::swap(m_heap[a], m_heap[b]);
        m_meta[m_heap[a].token].heapIndex = a;
        m_meta[m_heap[b].token].heapIndex = b;
    }

    void siftUp(size_t idx) noexcept
    {
        while (idx > 0) {
            const size_t parent = (idx - 1) / 2;
            if (m_heap[idx].deadline < m_heap[parent].deadline) {
                swapNodes(idx, parent);
                idx = parent;
            } else {
                break;
            }
        }
    }

    void siftDown(size_t idx) noexcept
    {
        const size_t n = m_heap.size();
        while (true) {
            const size_t left = 2 * idx + 1;
            const size_t right = 2 * idx + 2;
            size_t smallest = idx;
            if (left < n && m_heap[left].deadline < m_heap[smallest].deadline) {
                smallest = left;
            }
            if (right < n && m_heap[right].deadline < m_heap[smallest].deadline) {
                smallest = right;
            }
            if (smallest == idx) {
                break;
            }
            swapNodes(idx, smallest);
            idx = smallest;
        }
    }

    std::vector<DeadlineNode> m_heap;
    std::unordered_map<uint64_t, TokenMeta> m_meta;
};

struct WatchEntry {
    std::filesystem::path path;
    dev_t dev{0};
    ino_t ino{0};
};

struct DebounceEntry {
    FsWatchEvent event;
    std::chrono::steady_clock::time_point firstSeen;
    std::chrono::steady_clock::time_point lastSeen;
    uint64_t token{0};
};

struct PendingMove {
    uint32_t cookie{0};
    bool isDir{false};
    std::filesystem::path oldPath;
    std::filesystem::path oldItemRoot;
    uint64_t oldPublishedId{0};
    std::chrono::steady_clock::time_point firstSeen;
    std::chrono::steady_clock::time_point expiresAt;
    uint64_t token{0};
};

} // namespace

struct RecursiveFsWatcher::Impl {
    explicit Impl(uint32_t debounceMs) : debounceDuration(debounceMs) {}

    std::chrono::milliseconds debounceDuration;
    std::mutex lifecycleMutex;
    mutable std::mutex mutex;

    std::jthread workerThread;
    std::atomic<bool> isRunning{false};
    std::atomic<std::thread::id> workerThreadId{};

    int epollFd{-1};
    int inotifyFd{-1};
    int timerFd{-1};
    int stopEventFd{-1};

    FsErrorCallback errorCallback;
    std::vector<std::filesystem::path> watchRoots;
    std::unordered_map<int, WatchEntry> wdToEntry;
    std::unordered_map<std::string, int> pathToWd;

    std::unordered_map<std::string, DebounceEntry> debounceBuffer;
    std::unordered_map<uint32_t, PendingMove> pendingMoves;
    IndexedDeadlineHeap deadlineHeap;

    bool overflowPending{false};
    std::chrono::steady_clock::time_point overflowFirstSeen{};
    std::chrono::steady_clock::time_point overflowLastSeen{};
    uint64_t nextToken{kOverflowToken + 1};

    [[nodiscard]] std::chrono::steady_clock::time_point
    computeDeadline(std::chrono::steady_clock::time_point firstSeen,
                    std::chrono::steady_clock::time_point lastSeen) const noexcept
    {
        return std::min(lastSeen + debounceDuration, firstSeen + kHardDebounceCap);
    }

    [[nodiscard]] std::chrono::milliseconds cookieTtl() const noexcept
    {
        const uint32_t rawMs = static_cast<uint32_t>(debounceDuration.count()) / 2U;
        return std::chrono::milliseconds(std::clamp<uint32_t>(rawMs, 15U, 50U));
    }

    bool ensureCoreFdsLocked()
    {
        if (epollFd < 0) {
            epollFd = ::epoll_create1(EPOLL_CLOEXEC);
            if (epollFd < 0) {
                return false;
            }
        }

        if (timerFd < 0) {
            timerFd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
            if (timerFd < 0) {
                return false;
            }
            epoll_event evTimer{};
            evTimer.events = EPOLLIN;
            evTimer.data.fd = timerFd;
            ::epoll_ctl(epollFd, EPOLL_CTL_ADD, timerFd, &evTimer);
        }

        if (stopEventFd < 0) {
            stopEventFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (stopEventFd < 0) {
                return false;
            }
            epoll_event evStop{};
            evStop.events = EPOLLIN;
            evStop.data.fd = stopEventFd;
            ::epoll_ctl(epollFd, EPOLL_CTL_ADD, stopEventFd, &evStop);
        }

        if (inotifyFd < 0) {
            inotifyFd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
            if (inotifyFd < 0) {
                return false;
            }
            epoll_event evIno{};
            evIno.events = EPOLLIN;
            evIno.data.fd = inotifyFd;
            ::epoll_ctl(epollFd, EPOLL_CTL_ADD, inotifyFd, &evIno);
        }

        return true;
    }

    void bindWatchLocked(int wd, const std::filesystem::path &normPath, dev_t dev, ino_t ino)
    {
        const std::string pathStr = normPath.string();
        // Remove any previous wd mapped to this path before IN_IGNORED arrives for replaced dirs.
        if (auto oldWdIt = pathToWd.find(pathStr);
            oldWdIt != pathToWd.end() && oldWdIt->second != wd) {
            wdToEntry.erase(oldWdIt->second);
        }
        // Remove any stale path previously associated with a kernel-recycled wd.
        if (auto oldPathIt = wdToEntry.find(wd);
            oldPathIt != wdToEntry.end() && oldPathIt->second.path != normPath) {
            pathToWd.erase(oldPathIt->second.path.string());
        }

        wdToEntry[wd] = WatchEntry{.path = normPath, .dev = dev, .ino = ino};
        pathToWd[pathStr] = wd;
    }

    void unbindWatchLocked(int wd)
    {
        auto it = wdToEntry.find(wd);
        if (it == wdToEntry.end()) {
            return;
        }
        const std::string pathStr = it->second.path.string();
        if (auto pIt = pathToWd.find(pathStr); pIt != pathToWd.end() && pIt->second == wd) {
            pathToWd.erase(pIt);
        }
        wdToEntry.erase(it);
    }

    void removeWatchLocked(int wd)
    {
        if (wd < 0 || inotifyFd < 0) {
            return;
        }
        ::inotify_rm_watch(inotifyFd, wd);
        unbindWatchLocked(wd);
    }

    void removeWatchesUnderPrefixLocked(const std::filesystem::path &prefix,
                                        bool checkOrphanedByRemainingRoots = false)
    {
        std::vector<int> wds;
        for (const auto &[wd, entry] : wdToEntry) {
            if (isPathWithinOrEqual(entry.path, prefix) &&
                (!checkOrphanedByRemainingRoots ||
                 !findOwningRoot(entry.path, watchRoots).has_value())) {
                wds.push_back(wd);
            }
        }
        for (int wd : wds) {
            removeWatchLocked(wd);
        }
    }

    void markOverflowPendingLocked(std::chrono::steady_clock::time_point now)
    {
        if (!overflowPending) {
            overflowPending = true;
            overflowFirstSeen = now;
        }
        overflowLastSeen = now;
        deadlineHeap.upsert(kOverflowToken, DeadlineKind::Overflow,
                            computeDeadline(overflowFirstSeen, overflowLastSeen));
    }

    void addWatchRecursiveLocked(const std::filesystem::path &path,
                                 const std::filesystem::path &rootPath, size_t maxDepth,
                                 std::vector<std::string> &errorsOut)
    {
        if (inotifyFd < 0) {
            return;
        }

        const std::filesystem::path normPath = normalizeLexicalPath(path);
        const auto depthOpt = computeRelativeDepth(normPath, rootPath);
        if (!depthOpt.has_value() || *depthOpt > maxDepth) {
            return;
        }

        std::error_code ec;
        const std::filesystem::file_status st = std::filesystem::symlink_status(normPath, ec);
        if (ec || !std::filesystem::exists(st) || std::filesystem::is_symlink(st) ||
            !std::filesystem::is_directory(st)) {
            return;
        }

        struct stat stBuf{};
        if (::lstat(normPath.c_str(), &stBuf) != 0 || !S_ISDIR(stBuf.st_mode)) {
            return;
        }

        // Omit IN_MODIFY so multi-hundred-MB writes do not flood inotify before IN_CLOSE_WRITE.
        constexpr uint32_t mask = IN_CREATE | IN_DELETE | IN_ATTRIB | IN_MOVED_FROM | IN_MOVED_TO |
                                  IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_DONT_FOLLOW |
                                  IN_ONLYDIR;
        const int wd = ::inotify_add_watch(inotifyFd, normPath.c_str(), mask);
        if (wd < 0) {
            if (errno == ENOSPC) {
                std::string msg =
                    "[RecursiveFsWatcher] inotify_add_watch returned ENOSPC (table "
                    "exhaustion) for " +
                    normPath.string() +
                    ". Recommended fix: sudo sysctl fs.inotify.max_user_watches=524288";
                std::fprintf(stderr, "%s\n", msg.c_str());
                errorsOut.push_back(std::move(msg));
                markOverflowPendingLocked(std::chrono::steady_clock::now());
            }
            return;
        }

        bindWatchLocked(wd, normPath, stBuf.st_dev, stBuf.st_ino);

        if (*depthOpt >= maxDepth) {
            return;
        }

        for (const auto &entry : std::filesystem::directory_iterator(
                 normPath, std::filesystem::directory_options::skip_permission_denied, ec)) {
            std::error_code entryEc;
            const auto entryStatus = entry.symlink_status(entryEc);
            if (!entryEc && std::filesystem::is_directory(entryStatus) &&
                !std::filesystem::is_symlink(entryStatus)) {
                addWatchRecursiveLocked(entry.path(), rootPath, maxDepth, errorsOut);
            }
        }
    }

    void upsertDebouncedEventLocked(const std::string &key, FsWatchEvent event,
                                    std::chrono::steady_clock::time_point firstSeen,
                                    std::chrono::steady_clock::time_point lastSeen)
    {
        auto bIt = debounceBuffer.find(key);
        if (bIt != debounceBuffer.end()) {
            if (bIt->second.event.action == FsWatchAction::ItemCreated &&
                event.action == FsWatchAction::ItemModified) {
                // Keep ItemCreated across initial asset extraction bursts.
            } else {
                bIt->second.event = std::move(event);
            }
            bIt->second.firstSeen = std::min(bIt->second.firstSeen, firstSeen);
            bIt->second.lastSeen = std::max(bIt->second.lastSeen, lastSeen);
            deadlineHeap.upsert(bIt->second.token, DeadlineKind::DebounceItem,
                                computeDeadline(bIt->second.firstSeen, bIt->second.lastSeen), key);
            return;
        }

        const uint64_t token = nextToken++;
        debounceBuffer.emplace(key, DebounceEntry{.event = std::move(event),
                                                  .firstSeen = firstSeen,
                                                  .lastSeen = lastSeen,
                                                  .token = token});
        deadlineHeap.upsert(token, DeadlineKind::DebounceItem, computeDeadline(firstSeen, lastSeen),
                            key);
    }

    void finalizePendingMoveAsUnpairedLocked(PendingMove pm)
    {
        if (pm.isDir) {
            removeWatchesUnderPrefixLocked(pm.oldPath);
        }
        std::error_code existsEc;
        const FsWatchAction action =
            (pm.oldPath == pm.oldItemRoot || !std::filesystem::exists(pm.oldItemRoot, existsEc))
                ? FsWatchAction::ItemDeleted
                : FsWatchAction::ItemModified;
        const std::string key = "item:" + pm.oldItemRoot.string();
        upsertDebouncedEventLocked(key,
                                   FsWatchEvent{.action = action,
                                                .published_file_id = pm.oldPublishedId,
                                                .full_path = pm.oldItemRoot},
                                   pm.firstSeen, pm.firstSeen);
    }

    void recordPendingMoveLocked(uint32_t cookie, bool isDir, const std::filesystem::path &oldPath,
                                 std::chrono::steady_clock::time_point now)
    {
        if (auto dup = pendingMoves.find(cookie); dup != pendingMoves.end()) {
            PendingMove oldPm = std::move(dup->second);
            deadlineHeap.erase(oldPm.token);
            pendingMoves.erase(dup);
            finalizePendingMoveAsUnpairedLocked(std::move(oldPm));
        }

        if (pendingMoves.size() >= kMaxPendingMoves) {
            auto oldestIt = std::min_element(pendingMoves.begin(), pendingMoves.end(),
                                             [](const auto &a, const auto &b) {
                                                 return a.second.expiresAt < b.second.expiresAt;
                                             });
            if (oldestIt != pendingMoves.end()) {
                PendingMove evicted = std::move(oldestIt->second);
                deadlineHeap.erase(evicted.token);
                pendingMoves.erase(oldestIt);
                finalizePendingMoveAsUnpairedLocked(std::move(evicted));
            }
        }

        const uint64_t oldId = extractPublishedFileId(oldPath, watchRoots);
        const std::filesystem::path oldItemRoot =
            resolveItemRootDirectory(oldPath, watchRoots, oldId);
        const auto expiresAt = now + cookieTtl();
        const uint64_t token = nextToken++;

        pendingMoves.emplace(cookie, PendingMove{.cookie = cookie,
                                                 .isDir = isDir,
                                                 .oldPath = oldPath,
                                                 .oldItemRoot = oldItemRoot,
                                                 .oldPublishedId = oldId,
                                                 .firstSeen = now,
                                                 .expiresAt = expiresAt,
                                                 .token = token});
        deadlineHeap.upsert(token, DeadlineKind::PendingMove, expiresAt, {}, cookie);
    }

    bool hasPendingMovePathLocked(const std::filesystem::path &path) const
    {
        for (const auto &[cookie, pm] : pendingMoves) {
            if (pm.oldPath == path || isPathWithinOrEqual(path, pm.oldPath)) {
                return true;
            }
        }
        return false;
    }

    void migrateWatchPrefixesLocked(const std::filesystem::path &oldPrefix,
                                    const std::filesystem::path &newPrefix,
                                    std::vector<std::string> &errorsOut)
    {
        std::vector<int> affectedWds;
        for (const auto &[wd, entry] : wdToEntry) {
            if (entry.path == oldPrefix || isPathWithinOrEqual(entry.path, oldPrefix)) {
                affectedWds.push_back(wd);
            }
        }

        for (int wd : affectedWds) {
            auto wIt = wdToEntry.find(wd);
            if (wIt == wdToEntry.end()) {
                continue;
            }
            const std::filesystem::path oldPath = wIt->second.path;
            const std::filesystem::path rel = oldPath.lexically_relative(oldPrefix);
            const std::filesystem::path newPath =
                (rel.empty() || rel == ".") ? newPrefix : normalizeLexicalPath(newPrefix / rel);

            const auto owner = findOwningRoot(newPath, watchRoots);
            const auto depth =
                owner ? computeRelativeDepth(newPath, *owner) : std::optional<size_t>{};
            if (!depth.has_value() || *depth > kMaxWatchDepth) {
                removeWatchLocked(wd);
                continue;
            }

            if (auto pIt = pathToWd.find(oldPath.string());
                pIt != pathToWd.end() && pIt->second == wd) {
                pathToWd.erase(pIt);
            }
            const dev_t dev = wIt->second.dev;
            const ino_t ino = wIt->second.ino;
            bindWatchLocked(wd, newPath, dev, ino);
        }

        if (const auto owner = findOwningRoot(newPrefix, watchRoots)) {
            addWatchRecursiveLocked(newPrefix, *owner, kMaxWatchDepth, errorsOut);
        }
    }

    void pruneStaleWatchesLocked()
    {
        std::vector<int> staleWds;
        for (const auto &[wd, entry] : wdToEntry) {
            if (!findOwningRoot(entry.path, watchRoots).has_value()) {
                staleWds.push_back(wd);
                continue;
            }
            struct stat st{};
            if (::lstat(entry.path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
                st.st_dev != entry.dev || st.st_ino != entry.ino) {
                staleWds.push_back(wd);
            }
        }
        for (int wd : staleWds) {
            removeWatchLocked(wd);
        }
    }

    void rearmTimerLocked(std::chrono::steady_clock::time_point now) const
    {
        if (timerFd < 0) {
            return;
        }
        struct itimerspec ts{};
        if (deadlineHeap.empty()) {
            ::timerfd_settime(timerFd, 0, &ts, nullptr);
            return;
        }

        const auto earliest = deadlineHeap.topDeadline();
        int64_t diffNs = 1;
        if (earliest > now) {
            diffNs = std::chrono::duration_cast<std::chrono::nanoseconds>(earliest - now).count();
            if (diffNs <= 0) {
                diffNs = 1;
            }
        }

        ts.it_value.tv_sec = diffNs / 1'000'000'000LL;
        ts.it_value.tv_nsec = diffNs % 1'000'000'000LL;
        if (ts.it_value.tv_sec == 0 && ts.it_value.tv_nsec == 0) {
            ts.it_value.tv_nsec = 1;
        }
        ::timerfd_settime(timerFd, 0, &ts, nullptr);
    }

    void clearPendingEventsLocked()
    {
        debounceBuffer.clear();
        pendingMoves.clear();
        deadlineHeap.clear();
        overflowPending = false;
    }

    void closeCoreFdsAndClearRuntimeLocked()
    {
        if (timerFd >= 0) {
            ::close(timerFd);
            timerFd = -1;
        }
        if (stopEventFd >= 0) {
            ::close(stopEventFd);
            stopEventFd = -1;
        }
        if (inotifyFd >= 0) {
            ::close(inotifyFd);
            inotifyFd = -1;
        }
        if (epollFd >= 0) {
            ::close(epollFd);
            epollFd = -1;
        }

        wdToEntry.clear();
        pathToWd.clear();
        clearPendingEventsLocked();
    }
};

RecursiveFsWatcher::RecursiveFsWatcher(FsWatchCallback callback, uint32_t debounce_ms)
    : m_callback(std::move(callback)), m_impl(std::make_unique<Impl>(debounce_ms))
{
}

RecursiveFsWatcher::~RecursiveFsWatcher() { stop(); }

void RecursiveFsWatcher::setErrorCallback(FsErrorCallback callback)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->errorCallback = std::move(callback);
}

bool RecursiveFsWatcher::addWatchRoot(const std::filesystem::path &root_path)
{
    if (root_path.empty()) {
        return false;
    }

    const std::filesystem::path normRoot = normalizeLexicalPath(root_path);
    std::error_code ec;
    const std::filesystem::file_status st = std::filesystem::symlink_status(normRoot, ec);
    if (ec || !std::filesystem::exists(st) || std::filesystem::is_symlink(st) ||
        !std::filesystem::is_directory(st)) {
        return false;
    }

    std::vector<std::string> errorsOut;
    FsErrorCallback errorCb;
    bool rootAddedNow = false;
    bool watchAttached = false;

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!m_impl->ensureCoreFdsLocked()) {
            return false;
        }

        if (std::find(m_impl->watchRoots.begin(), m_impl->watchRoots.end(), normRoot) ==
            m_impl->watchRoots.end()) {
            m_impl->watchRoots.push_back(normRoot);
            rootAddedNow = true;
        }

        m_impl->addWatchRecursiveLocked(normRoot, normRoot, kMaxWatchDepth, errorsOut);
        watchAttached = m_impl->pathToWd.contains(normRoot.string());
        if (!watchAttached && rootAddedNow) {
            auto it = std::find(m_impl->watchRoots.begin(), m_impl->watchRoots.end(), normRoot);
            if (it != m_impl->watchRoots.end()) {
                m_impl->watchRoots.erase(it);
            }
        }
        if (m_impl->isRunning.load(std::memory_order_acquire) && !m_impl->deadlineHeap.empty()) {
            m_impl->rearmTimerLocked(std::chrono::steady_clock::now());
        }
        errorCb = m_impl->errorCallback;
    }

    if (errorCb) {
        for (const auto &err : errorsOut) {
            errorCb(err);
        }
    }
    return watchAttached;
}

void RecursiveFsWatcher::removeWatchRoot(const std::filesystem::path &root_path)
{
    const std::filesystem::path normRoot = normalizeLexicalPath(root_path);
    std::lock_guard<std::mutex> lock(m_impl->mutex);

    auto it = std::find(m_impl->watchRoots.begin(), m_impl->watchRoots.end(), normRoot);
    if (it != m_impl->watchRoots.end()) {
        m_impl->watchRoots.erase(it);
    }

    if (m_impl->inotifyFd >= 0) {
        m_impl->removeWatchesUnderPrefixLocked(normRoot, true);
    }

    for (auto bIt = m_impl->debounceBuffer.begin(); bIt != m_impl->debounceBuffer.end();) {
        const auto &ev = bIt->second.event;
        const bool fullOrphaned = isPathWithinOrEqual(ev.full_path, normRoot) &&
                                  !findOwningRoot(ev.full_path, m_impl->watchRoots).has_value();
        const bool oldOrphaned = !ev.old_path.empty() &&
                                 isPathWithinOrEqual(ev.old_path, normRoot) &&
                                 !findOwningRoot(ev.old_path, m_impl->watchRoots).has_value();
        if (fullOrphaned || oldOrphaned) {
            m_impl->deadlineHeap.erase(bIt->second.token);
            bIt = m_impl->debounceBuffer.erase(bIt);
        } else {
            ++bIt;
        }
    }

    for (auto mIt = m_impl->pendingMoves.begin(); mIt != m_impl->pendingMoves.end();) {
        const bool moveOrphaned =
            isPathWithinOrEqual(mIt->second.oldPath, normRoot) &&
            !findOwningRoot(mIt->second.oldPath, m_impl->watchRoots).has_value();
        if (moveOrphaned) {
            m_impl->deadlineHeap.erase(mIt->second.token);
            mIt = m_impl->pendingMoves.erase(mIt);
        } else {
            ++mIt;
        }
    }

    if (m_impl->watchRoots.empty() && m_impl->overflowPending) {
        m_impl->overflowPending = false;
        m_impl->deadlineHeap.erase(kOverflowToken);
    }

    if (m_impl->timerFd >= 0) {
        m_impl->rearmTimerLocked(std::chrono::steady_clock::now());
    }
}

void RecursiveFsWatcher::start()
{
    std::lock_guard<std::mutex> lifecycleLock(m_impl->lifecycleMutex);
    if (m_impl->isRunning.load(std::memory_order_acquire)) {
        return;
    }

    if (m_impl->workerThread.joinable()) {
        m_impl->workerThread.join();
        m_impl->workerThreadId.store(std::thread::id{}, std::memory_order_release);
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->closeCoreFdsAndClearRuntimeLocked();
    }

    std::vector<std::string> errorsOut;
    FsErrorCallback errorCb;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!m_impl->ensureCoreFdsLocked()) {
            return;
        }
        if (m_impl->wdToEntry.empty() && !m_impl->watchRoots.empty()) {
            for (const auto &root : m_impl->watchRoots) {
                m_impl->addWatchRecursiveLocked(root, root, kMaxWatchDepth, errorsOut);
            }
        }
        if (!m_impl->deadlineHeap.empty()) {
            m_impl->rearmTimerLocked(std::chrono::steady_clock::now());
        }
        errorCb = m_impl->errorCallback;
    }

    if (errorCb) {
        for (const auto &err : errorsOut) {
            errorCb(err);
        }
    }

    m_impl->isRunning.store(true, std::memory_order_release);
    m_impl->workerThread = std::jthread([this](std::stop_token st) {
        (void)st;
        workerThreadLoop();
    });
}

void RecursiveFsWatcher::stop()
{
    // Avoid joining or locking lifecycleMutex if stop() is called from inside m_callback on the
    // worker thread, because an external stop() may already hold lifecycleMutex waiting on join().
    if (std::this_thread::get_id() == m_impl->workerThreadId.load(std::memory_order_acquire)) {
        m_impl->isRunning.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->clearPendingEventsLocked();
        if (m_impl->stopEventFd >= 0) {
            uint64_t val = 1;
            const ssize_t s = ::write(m_impl->stopEventFd, &val, sizeof(val));
            (void)s;
        }
        return;
    }

    std::lock_guard<std::mutex> lifecycleLock(m_impl->lifecycleMutex);
    m_impl->isRunning.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->stopEventFd >= 0) {
            uint64_t val = 1;
            const ssize_t s = ::write(m_impl->stopEventFd, &val, sizeof(val));
            (void)s;
        }
    }

    if (m_impl->workerThread.joinable()) {
        m_impl->workerThread.request_stop();
        m_impl->workerThread.join();
    }
    m_impl->workerThreadId.store(std::thread::id{}, std::memory_order_release);

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->closeCoreFdsAndClearRuntimeLocked();
}

size_t RecursiveFsWatcher::totalWatches() const noexcept
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->wdToEntry.size();
}

void RecursiveFsWatcher::workerThreadLoop()
{
    m_impl->workerThreadId.store(std::this_thread::get_id(), std::memory_order_release);

    int epollFd = -1;
    int stopFd = -1;
    int timerFd = -1;
    int inotifyFd = -1;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        epollFd = m_impl->epollFd;
        stopFd = m_impl->stopEventFd;
        timerFd = m_impl->timerFd;
        inotifyFd = m_impl->inotifyFd;
    }

    constexpr int maxEvents = 64;
    epoll_event events[maxEvents];

    while (m_impl->isRunning.load(std::memory_order_acquire)) {
        const int nfds = ::epoll_wait(epollFd, events, maxEvents, -1);
        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == stopFd) {
                uint64_t val = 0;
                while (::read(stopFd, &val, sizeof(val)) > 0) {
                }
                return;
            }
        }

        if (!m_impl->isRunning.load(std::memory_order_acquire)) {
            return;
        }

        for (int i = 0; i < nfds; ++i) {
            if (!m_impl->isRunning.load(std::memory_order_acquire)) {
                return;
            }
            const int fd = events[i].data.fd;
            if (fd == timerFd) {
                flushDebounceQueue();
            } else if (fd == inotifyFd) {
                processInotifyEvents();
            }
        }
    }
}

void RecursiveFsWatcher::processInotifyEvents()
{
    int inoFd = -1;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        inoFd = m_impl->inotifyFd;
    }
    if (inoFd < 0) {
        return;
    }

    alignas(struct inotify_event) char buffer[16384];
    std::vector<std::string> errorsOut;
    FsErrorCallback errorCb;

    constexpr int kMaxReadsPerWake = 64;
    for (int readIter = 0;
         readIter < kMaxReadsPerWake && m_impl->isRunning.load(std::memory_order_acquire);
         ++readIter) {
        const ssize_t bytesRead = ::read(inoFd, buffer, sizeof(buffer));
        if (bytesRead < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (bytesRead == 0) {
            break;
        }

        std::lock_guard<std::mutex> lock(m_impl->mutex);
        const auto now = std::chrono::steady_clock::now();
        bool touchedDeadlines = false;
        ssize_t offset = 0;

        while (offset + static_cast<ssize_t>(sizeof(struct inotify_event)) <= bytesRead) {
            const auto *event = reinterpret_cast<const struct inotify_event *>(&buffer[offset]);
            const ssize_t remaining = bytesRead - offset - static_cast<ssize_t>(sizeof(*event));
            if (event->len > static_cast<uint32_t>(remaining)) {
                break;
            }
            const ssize_t step =
                static_cast<ssize_t>(sizeof(*event)) + static_cast<ssize_t>(event->len);

            if (event->mask & IN_Q_OVERFLOW) {
                for (const auto &[cookie, pm] : m_impl->pendingMoves) {
                    (void)cookie;
                    m_impl->deadlineHeap.erase(pm.token);
                }
                m_impl->pendingMoves.clear();
                m_impl->pruneStaleWatchesLocked();
                for (const auto &root : m_impl->watchRoots) {
                    m_impl->addWatchRecursiveLocked(root, root, kMaxWatchDepth, errorsOut);
                }
                m_impl->markOverflowPendingLocked(now);
                touchedDeadlines = true;
                offset += step;
                continue;
            }

            if (event->mask & IN_IGNORED) {
                m_impl->unbindWatchLocked(event->wd);
                offset += step;
                continue;
            }

            if (event->wd < 0) {
                offset += step;
                continue;
            }

            const auto wIt = m_impl->wdToEntry.find(event->wd);
            if (wIt == m_impl->wdToEntry.end()) {
                offset += step;
                continue;
            }
            const std::filesystem::path dirPath = wIt->second.path;
            const dev_t dirDev = wIt->second.dev;
            const ino_t dirIno = wIt->second.ino;

            size_t nameLen = 0;
            if (event->len > 0) {
                nameLen = ::strnlen(event->name, event->len);
            }
            const std::filesystem::path fullPath =
                (nameLen > 0)
                    ? normalizeLexicalPath(dirPath / std::string_view(event->name, nameLen))
                    : dirPath;

            if (event->mask & IN_DELETE_SELF) {
                m_impl->removeWatchesUnderPrefixLocked(dirPath);
                offset += step;
                continue;
            }

            if (event->mask & IN_MOVE_SELF) {
                const bool isRoot = std::find(m_impl->watchRoots.begin(), m_impl->watchRoots.end(),
                                              dirPath) != m_impl->watchRoots.end();
                struct stat st{};
                const bool pathMatchesWatch =
                    (::lstat(dirPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode) &&
                     st.st_dev == dirDev && st.st_ino == dirIno);
                if (isRoot || (!pathMatchesWatch && !m_impl->hasPendingMovePathLocked(dirPath))) {
                    m_impl->removeWatchesUnderPrefixLocked(dirPath);
                }
                offset += step;
                continue;
            }

            if ((event->mask & IN_MOVED_FROM) && event->cookie != 0) {
                m_impl->recordPendingMoveLocked(event->cookie, (event->mask & IN_ISDIR) != 0,
                                                fullPath, now);
                touchedDeadlines = true;
                offset += step;
                continue;
            }

            if ((event->mask & IN_MOVED_TO) && event->cookie != 0) {
                auto pmIt = m_impl->pendingMoves.find(event->cookie);
                if (pmIt != m_impl->pendingMoves.end()) {
                    PendingMove pm = std::move(pmIt->second);
                    m_impl->deadlineHeap.erase(pm.token);
                    m_impl->pendingMoves.erase(pmIt);

                    if (pm.isDir || (event->mask & IN_ISDIR)) {
                        m_impl->migrateWatchPrefixesLocked(pm.oldPath, fullPath, errorsOut);
                    }

                    const uint64_t newId = extractPublishedFileId(fullPath, m_impl->watchRoots);
                    const std::filesystem::path newItemRoot =
                        resolveItemRootDirectory(fullPath, m_impl->watchRoots, newId);

                    if (pm.oldPath == pm.oldItemRoot && pm.oldItemRoot != newItemRoot) {
                        const std::string oldItemKey = "item:" + pm.oldItemRoot.string();
                        if (auto oldBufIt = m_impl->debounceBuffer.find(oldItemKey);
                            oldBufIt != m_impl->debounceBuffer.end()) {
                            m_impl->deadlineHeap.erase(oldBufIt->second.token);
                            m_impl->debounceBuffer.erase(oldBufIt);
                        }
                    }

                    // Preserve original source path across chained A -> B -> C renames.
                    std::filesystem::path origOldPath = pm.oldPath;
                    const std::string prevRenameKey = "rename:" + pm.oldPath.string();
                    if (auto prevIt = m_impl->debounceBuffer.find(prevRenameKey);
                        prevIt != m_impl->debounceBuffer.end() &&
                        prevIt->second.event.action == FsWatchAction::ItemRenamed &&
                        !prevIt->second.event.old_path.empty()) {
                        origOldPath = prevIt->second.event.old_path;
                        m_impl->deadlineHeap.erase(prevIt->second.token);
                        m_impl->debounceBuffer.erase(prevIt);
                    }

                    const std::string renameKey = "rename:" + fullPath.string();
                    m_impl->upsertDebouncedEventLocked(
                        renameKey,
                        FsWatchEvent{.action = FsWatchAction::ItemRenamed,
                                     .published_file_id = newId,
                                     .full_path = fullPath,
                                     .old_path = std::move(origOldPath)},
                        pm.firstSeen, now);
                    touchedDeadlines = true;
                    offset += step;
                    continue;
                }
            }

            if (event->mask & IN_ISDIR) {
                if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
                    if (const auto owner = findOwningRoot(fullPath, m_impl->watchRoots)) {
                        m_impl->addWatchRecursiveLocked(fullPath, *owner, kMaxWatchDepth,
                                                        errorsOut);
                    }

                    // Catch files created inside the new directory before watch attachment.
                    std::error_code ec;
                    for (std::filesystem::recursive_directory_iterator
                             rit(fullPath,
                                 std::filesystem::directory_options::skip_permission_denied, ec),
                         rend;
                         !ec && rit != rend; rit.increment(ec)) {
                        if (rit.depth() > static_cast<int>(kMaxWatchDepth)) {
                            rit.disable_recursion_pending();
                            continue;
                        }
                        const auto childStatus = rit->symlink_status(ec);
                        if (ec || std::filesystem::is_symlink(childStatus)) {
                            rit.disable_recursion_pending();
                            continue;
                        }
                        const uint64_t childId =
                            extractPublishedFileId(rit->path(), m_impl->watchRoots);
                        const std::filesystem::path itemDir =
                            resolveItemRootDirectory(rit->path(), m_impl->watchRoots, childId);
                        const std::string childKey = "item:" + itemDir.string();
                        m_impl->upsertDebouncedEventLocked(
                            childKey,
                            FsWatchEvent{.action = FsWatchAction::ItemCreated,
                                         .published_file_id = childId,
                                         .full_path = itemDir},
                            now, now);
                        touchedDeadlines = true;
                    }
                } else if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
                    m_impl->removeWatchesUnderPrefixLocked(fullPath);
                }
            }

            const uint64_t id = extractPublishedFileId(fullPath, m_impl->watchRoots);
            const std::filesystem::path itemRoot =
                resolveItemRootDirectory(fullPath, m_impl->watchRoots, id);

            // Ignore bare structural events on watch root or intermediate "431960" folder.
            bool isContainerOnly = false;
            for (const auto &r : m_impl->watchRoots) {
                if (fullPath == r || fullPath == (r / "431960")) {
                    isContainerOnly = true;
                    break;
                }
            }

            if (!isContainerOnly) {
                FsWatchAction action = FsWatchAction::ItemModified;
                if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
                    action = (fullPath == itemRoot) ? FsWatchAction::ItemCreated
                                                    : FsWatchAction::ItemModified;
                } else if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
                    // Only emit ItemDeleted when the item root directory itself disappears.
                    std::error_code existsEc;
                    if (fullPath == itemRoot || !std::filesystem::exists(itemRoot, existsEc)) {
                        action = FsWatchAction::ItemDeleted;
                    } else {
                        action = FsWatchAction::ItemModified;
                    }
                } else if (event->mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB)) {
                    action = FsWatchAction::ItemModified;
                }

                const std::string itemKey = "item:" + itemRoot.string();
                m_impl->upsertDebouncedEventLocked(
                    itemKey,
                    FsWatchEvent{.action = action, .published_file_id = id, .full_path = itemRoot},
                    now, now);
                touchedDeadlines = true;
            }

            offset += step;
        }

        if (touchedDeadlines) {
            m_impl->rearmTimerLocked(now);
        }
        errorCb = m_impl->errorCallback;
    }

    if (errorCb) {
        for (const auto &err : errorsOut) {
            errorCb(err);
        }
    }
}

void RecursiveFsWatcher::flushDebounceQueue()
{
    std::vector<FsWatchEvent> readyEvents;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->timerFd >= 0) {
            uint64_t expirations = 0;
            while (::read(m_impl->timerFd, &expirations, sizeof(expirations)) > 0) {
            }
        }

        if (!m_impl->isRunning.load(std::memory_order_acquire)) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        while (!m_impl->deadlineHeap.empty() && m_impl->deadlineHeap.topDeadline() <= now) {
            const uint64_t token = m_impl->deadlineHeap.topNode().token;
            const TokenMeta *metaPtr = m_impl->deadlineHeap.findMeta(token);
            if (!metaPtr) {
                m_impl->deadlineHeap.erase(token);
                continue;
            }

            const DeadlineKind kind = metaPtr->kind;
            const std::string debounceKey = metaPtr->debounceKey;
            const uint32_t moveCookie = metaPtr->moveCookie;
            m_impl->deadlineHeap.erase(token);

            if (kind == DeadlineKind::Overflow) {
                if (m_impl->overflowPending) {
                    readyEvents.push_back(
                        FsWatchEvent{.action = FsWatchAction::CatalogOverflowReconcile,
                                     .published_file_id = 0,
                                     .full_path = {}});
                    m_impl->overflowPending = false;
                }
            } else if (kind == DeadlineKind::PendingMove) {
                auto pmIt = m_impl->pendingMoves.find(moveCookie);
                if (pmIt != m_impl->pendingMoves.end()) {
                    PendingMove pm = std::move(pmIt->second);
                    m_impl->pendingMoves.erase(pmIt);
                    m_impl->finalizePendingMoveAsUnpairedLocked(std::move(pm));
                }
            } else {
                auto bIt = m_impl->debounceBuffer.find(debounceKey);
                if (bIt != m_impl->debounceBuffer.end()) {
                    readyEvents.push_back(std::move(bIt->second.event));
                    m_impl->debounceBuffer.erase(bIt);
                }
            }
        }

        m_impl->rearmTimerLocked(now);
    }

    if (!readyEvents.empty() && m_callback && m_impl->isRunning.load(std::memory_order_acquire)) {
        m_callback(readyEvents);
    }
}

} // namespace wwal::studio
