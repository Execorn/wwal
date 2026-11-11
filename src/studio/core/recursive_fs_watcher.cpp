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
#include <sys/fanotify.h>
#include <sys/inotify.h>
#include <sys/timerfd.h>
#include <thread>
#include <unistd.h>

namespace wwal::studio {

namespace {

std::filesystem::path normalizeLexicalPath(const std::filesystem::path &raw)
{
    std::error_code ec;
    std::filesystem::path absPath = std::filesystem::absolute(raw, ec);
    std::filesystem::path norm = (ec ? raw : absPath).lexically_normal();
    if (norm.has_filename() && norm.filename().empty() && norm != norm.root_path()) {
        norm = norm.parent_path();
    }
    return norm;
}

bool isPathWithinOrEqual(const std::filesystem::path &candidate, const std::filesystem::path &base)
{
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

} // namespace

struct RecursiveFsWatcher::Impl {
    std::thread workerThread;
    std::atomic<bool> isRunning{false};
    mutable std::mutex mutex;
    std::vector<std::filesystem::path> watchRoots;

    struct DebounceTiming {
        std::chrono::steady_clock::time_point firstSeen;
        std::chrono::steady_clock::time_point lastSeen;
    };
    std::unordered_map<uint64_t, DebounceTiming> itemTiming;
    DebounceTiming overflowTiming{};
};

RecursiveFsWatcher::RecursiveFsWatcher(FsWatchCallback callback, uint32_t debounce_ms)
    : m_callback(std::move(callback)), m_debounceMs(debounce_ms), m_impl(std::make_unique<Impl>())
{
}

RecursiveFsWatcher::~RecursiveFsWatcher() { stop(); }

bool RecursiveFsWatcher::ensureCoreFdsLocked()
{
    if (m_epollFd < 0) {
        m_epollFd = ::epoll_create1(EPOLL_CLOEXEC);
        if (m_epollFd < 0) {
            return false;
        }
    }

    if (m_timerFd < 0) {
        m_timerFd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (m_timerFd < 0) {
            return false;
        }
        epoll_event evTimer{};
        evTimer.events = EPOLLIN;
        evTimer.data.fd = m_timerFd;
        ::epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_timerFd, &evTimer);
    }

    if (m_stopEventFd < 0) {
        m_stopEventFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (m_stopEventFd < 0) {
            return false;
        }
        epoll_event evStop{};
        evStop.events = EPOLLIN;
        evStop.data.fd = m_stopEventFd;
        ::epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_stopEventFd, &evStop);
    }

    // Use directory-scoped inotify (depth <= 2) rather than whole-filesystem fanotify marks.
    m_usingFanotify = false;
    if (m_inotifyFd < 0) {
        m_inotifyFd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (m_inotifyFd < 0) {
            return false;
        }
        epoll_event evIno{};
        evIno.events = EPOLLIN;
        evIno.data.fd = m_inotifyFd;
        ::epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_inotifyFd, &evIno);
    }

    return true;
}

bool RecursiveFsWatcher::addWatchRoot(const std::filesystem::path &root_path)
{
    std::error_code ec;
    if (!std::filesystem::exists(root_path, ec)) {
        return false;
    }

    const std::filesystem::path normRoot = normalizeLexicalPath(root_path);
    std::lock_guard<std::mutex> lock(m_impl->mutex);

    if (!ensureCoreFdsLocked()) {
        return false;
    }

    if (std::find(m_impl->watchRoots.begin(), m_impl->watchRoots.end(), normRoot) ==
        m_impl->watchRoots.end()) {
        m_impl->watchRoots.push_back(normRoot);
    }

    addWatchRecursiveLocked(normRoot, normRoot, 2);
    return true;
}

void RecursiveFsWatcher::removeWatchRoot(const std::filesystem::path &root_path)
{
    const std::filesystem::path normRoot = normalizeLexicalPath(root_path);
    std::lock_guard<std::mutex> lock(m_impl->mutex);

    auto it = std::find(m_impl->watchRoots.begin(), m_impl->watchRoots.end(), normRoot);
    if (it != m_impl->watchRoots.end()) {
        m_impl->watchRoots.erase(it);
    }

    if (m_inotifyFd >= 0) {
        std::vector<int> wdsToRemove;
        for (const auto &[wd, watchedPath] : m_wdToPath) {
            if (isPathWithinOrEqual(watchedPath, normRoot)) {
                wdsToRemove.push_back(wd);
            }
        }
        for (int wd : wdsToRemove) {
            removeWatchLocked(wd);
        }
    }
}

void RecursiveFsWatcher::start()
{
    if (m_impl->isRunning.load(std::memory_order_acquire)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!ensureCoreFdsLocked()) {
            return;
        }
        if (m_wdToPath.empty() && !m_impl->watchRoots.empty()) {
            for (const auto &root : m_impl->watchRoots) {
                addWatchRecursiveLocked(root, root, 2);
            }
        }
    }

    m_impl->isRunning.store(true, std::memory_order_release);
    m_impl->workerThread = std::thread(&RecursiveFsWatcher::workerThreadLoop, this);
}

void RecursiveFsWatcher::stop()
{
    if (m_impl->isRunning.exchange(false, std::memory_order_acq_rel)) {
        int stopFd = -1;
        {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            stopFd = m_stopEventFd;
        }
        if (stopFd >= 0) {
            uint64_t val = 1;
            ssize_t s = ::write(stopFd, &val, sizeof(val));
            (void)s;
        }
        if (m_impl->workerThread.joinable()) {
            m_impl->workerThread.join();
        }
    }

    flushDebounceQueue();

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_timerFd >= 0) {
        ::close(m_timerFd);
        m_timerFd = -1;
    }
    if (m_stopEventFd >= 0) {
        ::close(m_stopEventFd);
        m_stopEventFd = -1;
    }
    if (m_inotifyFd >= 0) {
        ::close(m_inotifyFd);
        m_inotifyFd = -1;
    }
    if (m_fanotifyFd >= 0) {
        ::close(m_fanotifyFd);
        m_fanotifyFd = -1;
    }
    if (m_epollFd >= 0) {
        ::close(m_epollFd);
        m_epollFd = -1;
    }

    m_wdToPath.clear();
    m_pathToWd.clear();
    m_debounceBuffer.clear();
    m_overflowPending = false;
    m_impl->itemTiming.clear();
    m_usingFanotify = false;
}

size_t RecursiveFsWatcher::totalWatches() const noexcept
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_usingFanotify) {
        return m_impl->watchRoots.size();
    }
    return m_wdToPath.size();
}

void RecursiveFsWatcher::addWatchRecursiveLocked(const std::filesystem::path &path,
                                                 const std::filesystem::path &rootPath,
                                                 size_t maxDepth)
{
    if (m_inotifyFd < 0) {
        return;
    }

    const std::filesystem::path normPath = normalizeLexicalPath(path);
    auto depthOpt = computeRelativeDepth(normPath, rootPath);
    if (!depthOpt.has_value() || *depthOpt > maxDepth) {
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(normPath, ec) || !std::filesystem::is_directory(normPath, ec) ||
        std::filesystem::is_symlink(normPath, ec)) {
        return;
    }

    constexpr uint32_t mask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO |
                              IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_DONT_FOLLOW |
                              IN_ONLYDIR;
    int wd = ::inotify_add_watch(m_inotifyFd, normPath.c_str(), mask);
    if (wd < 0) {
        if (errno == ENOSPC) {
            std::fprintf(
                stderr,
                "[RecursiveFsWatcher] inotify_add_watch returned ENOSPC (table exhaustion) for %s. "
                "Recommended fix: sudo sysctl fs.inotify.max_user_watches=524288\n",
                normPath.c_str());
        }
        return;
    }

    m_wdToPath[wd] = normPath;
    m_pathToWd[normPath.string()] = wd;

    if (*depthOpt >= maxDepth) {
        return;
    }

    for (const auto &entry : std::filesystem::directory_iterator(
             normPath, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (entry.is_directory(ec) && !entry.is_symlink(ec)) {
            addWatchRecursiveLocked(entry.path(), rootPath, maxDepth);
        }
    }
}

void RecursiveFsWatcher::removeWatchLocked(int wd)
{
    if (wd < 0 || m_inotifyFd < 0) {
        return;
    }
    ::inotify_rm_watch(m_inotifyFd, wd);
    auto it = m_wdToPath.find(wd);
    if (it != m_wdToPath.end()) {
        m_pathToWd.erase(it->second.string());
        m_wdToPath.erase(it);
    }
}

void RecursiveFsWatcher::workerThreadLoop()
{
    constexpr int maxEvents = 64;
    epoll_event events[maxEvents];

    while (m_impl->isRunning.load(std::memory_order_acquire)) {
        int nfds = ::epoll_wait(m_epollFd, events, maxEvents, -1);
        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            if (fd == m_stopEventFd) {
                uint64_t val = 0;
                while (::read(m_stopEventFd, &val, sizeof(val)) > 0) {
                }
                return;
            }
            if (fd == m_timerFd) {
                flushDebounceQueue();
            } else if (fd == m_inotifyFd) {
                processInotifyEvents();
            } else if (fd == m_fanotifyFd) {
                processFanotifyEvents();
            }
        }
    }
}

void RecursiveFsWatcher::processInotifyEvents()
{
    alignas(struct inotify_event) char buffer[8192];
    ssize_t bytesRead = 0;

    while ((bytesRead = ::read(m_inotifyFd, buffer, sizeof(buffer))) > 0) {
        ssize_t offset = 0;
        while (offset < bytesRead) {
            auto *event = reinterpret_cast<struct inotify_event *>(&buffer[offset]);
            const ssize_t step = static_cast<ssize_t>(sizeof(struct inotify_event)) + event->len;

            if (event->mask & IN_Q_OVERFLOW) {
                std::lock_guard<std::mutex> lock(m_impl->mutex);
                auto now = std::chrono::steady_clock::now();
                if (!m_overflowPending) {
                    m_overflowPending = true;
                    m_impl->overflowTiming = {now, now};
                } else {
                    m_impl->overflowTiming.lastSeen = now;
                }

                struct itimerspec ts{};
                ts.it_value.tv_sec = 0;
                ts.it_value.tv_nsec = 1;
                ::timerfd_settime(m_timerFd, 0, &ts, nullptr);

                offset += step;
                continue;
            }

            if (event->mask & IN_IGNORED) {
                std::lock_guard<std::mutex> lock(m_impl->mutex);
                auto it = m_wdToPath.find(event->wd);
                if (it != m_wdToPath.end()) {
                    m_pathToWd.erase(it->second.string());
                    m_wdToPath.erase(it);
                }
                offset += step;
                continue;
            }

            if (event->wd >= 0) {
                std::filesystem::path dirPath;
                std::vector<std::filesystem::path> rootsSnapshot;
                {
                    std::lock_guard<std::mutex> lock(m_impl->mutex);
                    auto it = m_wdToPath.find(event->wd);
                    if (it != m_wdToPath.end()) {
                        dirPath = it->second;
                    }
                    rootsSnapshot = m_impl->watchRoots;
                }

                if (!dirPath.empty()) {
                    std::filesystem::path fullPath =
                        (event->len > 0 && event->name[0] != '\0')
                            ? normalizeLexicalPath(dirPath / event->name)
                            : dirPath;

                    if (event->mask & IN_ISDIR) {
                        if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
                            {
                                std::lock_guard<std::mutex> lock(m_impl->mutex);
                                if (auto owner = findOwningRoot(fullPath, m_impl->watchRoots)) {
                                    addWatchRecursiveLocked(fullPath, *owner, 2);
                                }
                            }

                            // Catch files created inside the new directory before watch attachment.
                            std::error_code ec;
                            for (std::filesystem::recursive_directory_iterator
                                     rit(fullPath,
                                         std::filesystem::directory_options::skip_permission_denied,
                                         ec),
                                 rend;
                                 !ec && rit != rend; rit.increment(ec)) {
                                if (rit.depth() > 2) {
                                    rit.disable_recursion_pending();
                                    continue;
                                }
                                uint64_t childId =
                                    extractPublishedFileId(rit->path(), rootsSnapshot);
                                std::filesystem::path itemDir =
                                    resolveItemRootDirectory(rit->path(), rootsSnapshot, childId);

                                std::lock_guard<std::mutex> lock(m_impl->mutex);
                                auto now = std::chrono::steady_clock::now();
                                if (!m_impl->itemTiming.contains(childId)) {
                                    m_impl->itemTiming[childId] = {now, now};
                                } else {
                                    m_impl->itemTiming[childId].lastSeen = now;
                                }
                                m_debounceBuffer[childId] =
                                    FsWatchEvent{.action = FsWatchAction::ItemCreated,
                                                 .published_file_id = childId,
                                                 .full_path = itemDir};
                            }
                        } else if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
                            std::lock_guard<std::mutex> lock(m_impl->mutex);
                            std::vector<int> childWds;
                            for (const auto &[wd, watchedPath] : m_wdToPath) {
                                if (isPathWithinOrEqual(watchedPath, fullPath)) {
                                    childWds.push_back(wd);
                                }
                            }
                            for (int cwd : childWds) {
                                removeWatchLocked(cwd);
                            }
                        }
                    }

                    uint64_t id = extractPublishedFileId(fullPath, rootsSnapshot);
                    std::filesystem::path itemRoot =
                        resolveItemRootDirectory(fullPath, rootsSnapshot, id);

                    // Ignore bare structural events on watch root or intermediate "431960" folder.
                    bool isContainerOnly = false;
                    for (const auto &r : rootsSnapshot) {
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
                        } else if (event->mask &
                                   (IN_DELETE | IN_MOVED_FROM | IN_DELETE_SELF | IN_MOVE_SELF)) {
                            // Only emit ItemDeleted when the item root directory itself disappears;
                            // unlinking temporary child files inside an existing item directory is
                            // an ItemModified event.
                            std::error_code existsEc;
                            if (fullPath == itemRoot ||
                                !std::filesystem::exists(itemRoot, existsEc)) {
                                action = FsWatchAction::ItemDeleted;
                            } else {
                                action = FsWatchAction::ItemModified;
                            }
                        } else if (event->mask & (IN_MODIFY | IN_CLOSE_WRITE)) {
                            action = FsWatchAction::ItemModified;
                        }

                        std::lock_guard<std::mutex> lock(m_impl->mutex);
                        auto now = std::chrono::steady_clock::now();
                        if (!m_impl->itemTiming.contains(id)) {
                            m_impl->itemTiming[id] = {now, now};
                        } else {
                            m_impl->itemTiming[id].lastSeen = now;
                        }

                        auto bIt = m_debounceBuffer.find(id);
                        if (bIt != m_debounceBuffer.end()) {
                            if (bIt->second.action == FsWatchAction::ItemCreated &&
                                action == FsWatchAction::ItemModified) {
                                // Keep ItemCreated across initial asset extraction bursts.
                            } else {
                                bIt->second.action = action;
                                bIt->second.full_path = itemRoot;
                            }
                        } else {
                            m_debounceBuffer[id] = FsWatchEvent{
                                .action = action, .published_file_id = id, .full_path = itemRoot};
                        }

                        auto earliest = std::chrono::steady_clock::time_point::max();
                        if (m_overflowPending) {
                            auto qd = m_impl->overflowTiming.lastSeen +
                                      std::chrono::milliseconds(m_debounceMs);
                            auto hd =
                                m_impl->overflowTiming.firstSeen + std::chrono::milliseconds(1000);
                            earliest = std::min(earliest, std::min(qd, hd));
                        }
                        for (const auto &[itemId, timing] : m_impl->itemTiming) {
                            auto quietDeadline =
                                timing.lastSeen + std::chrono::milliseconds(m_debounceMs);
                            auto hardDeadline = timing.firstSeen + std::chrono::milliseconds(1000);
                            earliest = std::min(earliest, std::min(quietDeadline, hardDeadline));
                        }

                        int64_t diffNs = 1;
                        if (earliest < std::chrono::steady_clock::time_point::max() &&
                            earliest > now) {
                            diffNs =
                                std::chrono::duration_cast<std::chrono::nanoseconds>(earliest - now)
                                    .count();
                        }
                        struct itimerspec ts{};
                        ts.it_value.tv_sec = diffNs / 1'000'000'000LL;
                        ts.it_value.tv_nsec = diffNs % 1'000'000'000LL;
                        if (ts.it_value.tv_sec == 0 && ts.it_value.tv_nsec == 0) {
                            ts.it_value.tv_nsec = 1;
                        }
                        ::timerfd_settime(m_timerFd, 0, &ts, nullptr);
                    }
                }
            }

            offset += step;
        }
    }
}

void RecursiveFsWatcher::processFanotifyEvents()
{
    alignas(struct fanotify_event_metadata) char buffer[4096];
    ssize_t bytesRead = 0;

    while ((bytesRead = ::read(m_fanotifyFd, buffer, sizeof(buffer))) > 0) {
        auto *meta = reinterpret_cast<struct fanotify_event_metadata *>(buffer);
        while (FAN_EVENT_OK(meta, bytesRead)) {
            if (meta->fd >= 0) {
                ::close(meta->fd);
                meta->fd = FAN_NOFD;
            }

            if (meta->mask & FAN_Q_OVERFLOW) {
                std::lock_guard<std::mutex> lock(m_impl->mutex);
                auto now = std::chrono::steady_clock::now();
                if (!m_overflowPending) {
                    m_overflowPending = true;
                    m_impl->overflowTiming = {now, now};
                } else {
                    m_impl->overflowTiming.lastSeen = now;
                }
            } else {
                FsWatchAction action = FsWatchAction::ItemModified;
                if (meta->mask & FAN_CREATE) {
                    action = FsWatchAction::ItemCreated;
                } else if (meta->mask & FAN_DELETE) {
                    action = FsWatchAction::ItemDeleted;
                }

                std::lock_guard<std::mutex> lock(m_impl->mutex);
                auto now = std::chrono::steady_clock::now();
                uint64_t id = 0;
                if (!m_impl->itemTiming.contains(id)) {
                    m_impl->itemTiming[id] = {now, now};
                } else {
                    m_impl->itemTiming[id].lastSeen = now;
                }
                m_debounceBuffer[id] = FsWatchEvent{
                    .action = action,
                    .published_file_id = id,
                    .full_path = m_impl->watchRoots.empty() ? "" : m_impl->watchRoots.front()};
            }

            meta = FAN_EVENT_NEXT(meta, bytesRead);
        }

        std::lock_guard<std::mutex> lock(m_impl->mutex);
        struct itimerspec ts{};
        ts.it_value.tv_sec = m_debounceMs / 1000;
        ts.it_value.tv_nsec = (m_debounceMs % 1000) * 1'000'000LL;
        if (ts.it_value.tv_sec == 0 && ts.it_value.tv_nsec == 0) {
            ts.it_value.tv_nsec = 1;
        }
        ::timerfd_settime(m_timerFd, 0, &ts, nullptr);
    }
}

void RecursiveFsWatcher::flushDebounceQueue()
{
    uint64_t expirations = 0;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_timerFd >= 0) {
            while (::read(m_timerFd, &expirations, sizeof(expirations)) > 0) {
            }
        }
    }

    std::vector<FsWatchEvent> readyEvents;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        const bool forceFlushAll = !m_impl->isRunning.load(std::memory_order_acquire);
        auto now = std::chrono::steady_clock::now();

        if (m_overflowPending) {
            auto qd = m_impl->overflowTiming.lastSeen + std::chrono::milliseconds(m_debounceMs);
            auto hd = m_impl->overflowTiming.firstSeen + std::chrono::milliseconds(1000);
            if (forceFlushAll || now >= qd || now >= hd) {
                readyEvents.push_back(
                    FsWatchEvent{.action = FsWatchAction::CatalogOverflowReconcile,
                                 .published_file_id = 0,
                                 .full_path = {}});
                m_overflowPending = false;
            }
        }

        for (auto it = m_debounceBuffer.begin(); it != m_debounceBuffer.end();) {
            uint64_t id = it->first;
            auto tIt = m_impl->itemTiming.find(id);
            bool readyToFlush = forceFlushAll;

            if (!readyToFlush && tIt != m_impl->itemTiming.end()) {
                auto quietDeadline = tIt->second.lastSeen + std::chrono::milliseconds(m_debounceMs);
                auto hardDeadline = tIt->second.firstSeen + std::chrono::milliseconds(1000);
                if (now >= quietDeadline || now >= hardDeadline) {
                    readyToFlush = true;
                }
            } else if (tIt == m_impl->itemTiming.end()) {
                readyToFlush = true;
            }

            if (readyToFlush) {
                readyEvents.push_back(std::move(it->second));
                if (tIt != m_impl->itemTiming.end()) {
                    m_impl->itemTiming.erase(tIt);
                }
                it = m_debounceBuffer.erase(it);
            } else {
                ++it;
            }
        }

        if ((m_debounceBuffer.empty() && !m_overflowPending) || m_timerFd < 0) {
            struct itimerspec disarm{};
            if (m_timerFd >= 0) {
                ::timerfd_settime(m_timerFd, 0, &disarm, nullptr);
            }
        } else {
            auto earliest = std::chrono::steady_clock::time_point::max();
            if (m_overflowPending) {
                auto qd = m_impl->overflowTiming.lastSeen + std::chrono::milliseconds(m_debounceMs);
                auto hd = m_impl->overflowTiming.firstSeen + std::chrono::milliseconds(1000);
                earliest = std::min(earliest, std::min(qd, hd));
            }
            for (const auto &[id, timing] : m_impl->itemTiming) {
                auto quietDeadline = timing.lastSeen + std::chrono::milliseconds(m_debounceMs);
                auto hardDeadline = timing.firstSeen + std::chrono::milliseconds(1000);
                earliest = std::min(earliest, std::min(quietDeadline, hardDeadline));
            }
            int64_t diffNs = 1;
            if (earliest < std::chrono::steady_clock::time_point::max() && earliest > now) {
                diffNs =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(earliest - now).count();
            }
            struct itimerspec ts{};
            ts.it_value.tv_sec = diffNs / 1'000'000'000LL;
            ts.it_value.tv_nsec = diffNs % 1'000'000'000LL;
            if (ts.it_value.tv_sec == 0 && ts.it_value.tv_nsec == 0) {
                ts.it_value.tv_nsec = 1;
            }
            ::timerfd_settime(m_timerFd, 0, &ts, nullptr);
        }
    }

    if (!readyEvents.empty() && m_callback) {
        m_callback(readyEvents);
    }
}

} // namespace wwal::studio
