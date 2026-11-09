// SPDX-License-Identifier: GPL-3.0-or-later
#include "recursive_fs_watcher.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/fanotify.h>
#include <sys/inotify.h>
#include <sys/timerfd.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>

namespace wwal::studio {

namespace {

uint64_t extractPublishedFileId(const std::filesystem::path &fullPath,
                                const std::vector<std::filesystem::path> &watchRoots)
{
    std::error_code ec;

    // Check relative to any known watch roots first
    for (const auto &root : watchRoots) {
        auto rel = std::filesystem::relative(fullPath, root, ec);
        if (ec || rel.empty() || rel == ".") {
            continue;
        }

        auto it = rel.begin();
        std::string firstComp = it->string();
        if (firstComp == "431960" && ++it != rel.end()) {
            firstComp = it->string();
        }

        // Check if first component is all digits (Steam Workshop ID convention)
        if (!firstComp.empty()) {
            bool allDigits = true;
            for (char c : firstComp) {
                if (!std::isdigit(static_cast<unsigned char>(c))) {
                    allDigits = false;
                    break;
                }
            }
            if (allDigits) {
                try {
                    uint64_t id = std::stoull(firstComp);
                    if (id > 0)
                        return id;
                } catch (...) {
                }
            }
        }

        // Check if any subsequent component is all digits
        for (const auto &part : rel) {
            std::string partStr = part.string();
            if (partStr == "431960" || partStr.empty())
                continue;
            bool digits = true;
            for (char c : partStr) {
                if (!std::isdigit(static_cast<unsigned char>(c))) {
                    digits = false;
                    break;
                }
            }
            if (digits) {
                try {
                    uint64_t id = std::stoull(partStr);
                    if (id > 0)
                        return id;
                } catch (...) {
                }
            }
        }

        // Non-numeric item folder under root: hash top-level relative component
        if (!firstComp.empty()) {
            return std::hash<std::string>{}(firstComp);
        }
    }

    // Fallback: check backwards from end of path
    for (auto it = fullPath.end(); it != fullPath.begin();) {
        --it;
        std::string comp = it->string();
        if (comp == "431960" || comp == "content" || comp == "workshop" || comp == "steamapps") {
            continue;
        }
        if (comp.empty())
            continue;

        bool allDigits = true;
        for (char c : comp) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                allDigits = false;
                break;
            }
        }
        if (allDigits) {
            try {
                uint64_t id = std::stoull(comp);
                if (id > 0)
                    return id;
            } catch (...) {
            }
        }
    }

    std::string stem = fullPath.stem().string();
    if (!stem.empty()) {
        bool allDigits = true;
        for (char c : stem) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                allDigits = false;
                break;
            }
        }
        if (allDigits) {
            try {
                return std::stoull(stem);
            } catch (...) {
            }
        }
    }

    return 0;
}

std::filesystem::path resolveItemDirectory(const std::filesystem::path &fullPath,
                                           const std::vector<std::filesystem::path> &watchRoots,
                                           uint64_t publishedFileId)
{
    std::error_code ec;
    for (const auto &root : watchRoots) {
        auto rel = std::filesystem::relative(fullPath, root, ec);
        if (ec || rel.empty() || rel == ".")
            continue;

        auto it = rel.begin();
        std::string firstComp = it->string();
        if (firstComp == "431960" && ++it != rel.end()) {
            std::filesystem::path itemDir = root / "431960" / it->string();
            if (std::filesystem::exists(itemDir, ec))
                return itemDir;
        } else {
            std::filesystem::path itemDir = root / firstComp;
            if (std::filesystem::exists(itemDir, ec))
                return itemDir;
        }
    }

    // Try finding directory component matching publishedFileId string
    if (publishedFileId > 0) {
        std::string idStr = std::to_string(publishedFileId);
        std::filesystem::path cur = fullPath;
        while (!cur.empty() && cur != cur.parent_path()) {
            if (cur.filename() == idStr) {
                return cur;
            }
            cur = cur.parent_path();
        }
    }

    return fullPath;
}

} // namespace

struct RecursiveFsWatcher::Impl {
    std::thread workerThread;
    std::atomic<bool> isRunning{false};
    std::mutex mutex;
    std::vector<std::filesystem::path> watchRoots;

    struct DebounceTiming {
        std::chrono::steady_clock::time_point firstSeen;
        std::chrono::steady_clock::time_point lastSeen;
    };
    std::unordered_map<uint64_t, DebounceTiming> itemTiming;
};

RecursiveFsWatcher::RecursiveFsWatcher(FsWatchCallback callback, uint32_t debounce_ms)
    : m_callback(std::move(callback)), m_debounceMs(debounce_ms), m_impl(std::make_unique<Impl>())
{
}

RecursiveFsWatcher::~RecursiveFsWatcher() { stop(); }

bool RecursiveFsWatcher::addWatchRoot(const std::filesystem::path &root_path)
{
    std::error_code ec;
    if (!std::filesystem::exists(root_path, ec)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_impl->mutex);

    // Initialize core epoll, timerfd, and stop eventfd descriptors if not already created
    if (m_epollFd < 0) {
        m_epollFd = epoll_create1(EPOLL_CLOEXEC);
        if (m_epollFd < 0)
            return false;

        m_timerFd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (m_timerFd >= 0) {
            epoll_event evTimer{};
            evTimer.events = EPOLLIN;
            evTimer.data.fd = m_timerFd;
            epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_timerFd, &evTimer);
        }

        m_stopEventFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (m_stopEventFd >= 0) {
            epoll_event evStop{};
            evStop.events = EPOLLIN;
            evStop.data.fd = m_stopEventFd;
            epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_stopEventFd, &evStop);
        }

        // Dual-Probe fanotify capability check
        int fanFd =
            fanotify_init(FAN_CLASS_NOTIF | FAN_REPORT_FID, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fanFd >= 0) {
            // Test mark on root path
            int markRc = fanotify_mark(fanFd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM,
                                       FAN_CREATE | FAN_DELETE | FAN_MODIFY | FAN_MOVE | FAN_ONDIR,
                                       AT_FDCWD, root_path.c_str());
            if (markRc == 0) {
                m_fanotifyFd = fanFd;
                m_usingFanotify = true;
                epoll_event evFan{};
                evFan.events = EPOLLIN;
                evFan.data.fd = m_fanotifyFd;
                epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_fanotifyFd, &evFan);
            } else {
                // fanotify_mark failed (EPERM, ENOSYS, etc.) - standard unprivileged Linux session
                close(fanFd);
                m_fanotifyFd = -1;
                m_usingFanotify = false;
            }
        } else {
            m_usingFanotify = false;
        }

        // Fallback: engage recursive inotify engine
        if (!m_usingFanotify) {
            m_inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
            if (m_inotifyFd >= 0) {
                epoll_event evIno{};
                evIno.events = EPOLLIN;
                evIno.data.fd = m_inotifyFd;
                epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_inotifyFd, &evIno);
            }
        }
    } else if (m_usingFanotify && m_fanotifyFd >= 0) {
        // Mark additional watch root with fanotify
        int markRc = fanotify_mark(m_fanotifyFd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM,
                                   FAN_CREATE | FAN_DELETE | FAN_MODIFY | FAN_MOVE | FAN_ONDIR,
                                   AT_FDCWD, root_path.c_str());
        if (markRc != 0) {
            // Degrade to recursive inotify if marking fails
            close(m_fanotifyFd);
            m_fanotifyFd = -1;
            m_usingFanotify = false;
            if (m_inotifyFd < 0) {
                m_inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
                if (m_inotifyFd >= 0) {
                    epoll_event evIno{};
                    evIno.events = EPOLLIN;
                    evIno.data.fd = m_inotifyFd;
                    epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_inotifyFd, &evIno);
                }
            }
            for (const auto &r : m_impl->watchRoots) {
                addWatchRecursive(r);
            }
        }
    }

    m_impl->watchRoots.push_back(root_path);

    if (!m_usingFanotify && m_inotifyFd >= 0) {
        addWatchRecursive(root_path);
    }

    return true;
}

void RecursiveFsWatcher::removeWatchRoot(const std::filesystem::path &root_path)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);

    auto it = std::find(m_impl->watchRoots.begin(), m_impl->watchRoots.end(), root_path);
    if (it != m_impl->watchRoots.end()) {
        m_impl->watchRoots.erase(it);
    }

    if (m_usingFanotify && m_fanotifyFd >= 0) {
        fanotify_mark(m_fanotifyFd, FAN_MARK_REMOVE | FAN_MARK_FILESYSTEM,
                      FAN_CREATE | FAN_DELETE | FAN_MODIFY | FAN_MOVE | FAN_ONDIR, AT_FDCWD,
                      root_path.c_str());
    } else if (m_inotifyFd >= 0) {
        std::string rootStr = root_path.string();
        std::vector<int> wdsToRemove;
        for (const auto &[wd, path] : m_wdToPath) {
            std::string pStr = path.string();
            if (pStr.rfind(rootStr, 0) == 0) {
                wdsToRemove.push_back(wd);
            }
        }
        for (int wd : wdsToRemove) {
            removeWatchRecursive(wd);
        }
    }
}

void RecursiveFsWatcher::start()
{
    if (m_impl->isRunning.load(std::memory_order_acquire)) {
        return;
    }
    m_impl->isRunning.store(true, std::memory_order_release);
    m_impl->workerThread = std::thread(&RecursiveFsWatcher::workerThreadLoop, this);
}

void RecursiveFsWatcher::stop()
{
    if (m_impl->isRunning.exchange(false, std::memory_order_acq_rel)) {
        if (m_stopEventFd >= 0) {
            uint64_t val = 1;
            ssize_t s = write(m_stopEventFd, &val, sizeof(val));
            (void)s;
        }
        if (m_impl->workerThread.joinable()) {
            m_impl->workerThread.join();
        }
    }

    // Flush any pending debounce queue before shutting down descriptors
    flushDebounceQueue();

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_timerFd >= 0) {
        close(m_timerFd);
        m_timerFd = -1;
    }
    if (m_stopEventFd >= 0) {
        close(m_stopEventFd);
        m_stopEventFd = -1;
    }
    if (m_inotifyFd >= 0) {
        close(m_inotifyFd);
        m_inotifyFd = -1;
    }
    if (m_fanotifyFd >= 0) {
        close(m_fanotifyFd);
        m_fanotifyFd = -1;
    }
    if (m_epollFd >= 0) {
        close(m_epollFd);
        m_epollFd = -1;
    }

    m_wdToPath.clear();
    m_pathToWd.clear();
    m_debounceBuffer.clear();
    m_impl->itemTiming.clear();
    m_usingFanotify = false;
}

size_t RecursiveFsWatcher::totalWatches() const noexcept
{
    if (m_usingFanotify) {
        return m_impl ? m_impl->watchRoots.size() : 1;
    }
    return m_wdToPath.size();
}

void RecursiveFsWatcher::addWatchRecursive(const std::filesystem::path &path)
{
    if (m_inotifyFd < 0)
        return;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || !std::filesystem::is_directory(path, ec)) {
        return;
    }

    constexpr uint32_t mask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO |
                              IN_CLOSE_WRITE | IN_DONT_FOLLOW | IN_ONLYDIR;
    int wd = inotify_add_watch(m_inotifyFd, path.c_str(), mask);
    if (wd < 0) {
        if (errno == ENOSPC) {
            std::fprintf(
                stderr,
                "[RecursiveFsWatcher] inotify_add_watch returned ENOSPC (table exhaustion) for %s. "
                "Recommended fix: sudo sysctl fs.inotify.max_user_watches=524288\n",
                path.c_str());
        }
        return;
    }

    m_wdToPath[wd] = path;
    m_pathToWd[path.string()] = wd;

    // Traverse subdirectories recursively
    for (const auto &entry : std::filesystem::directory_iterator(
             path, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (entry.is_directory(ec) && !entry.is_symlink(ec)) {
            addWatchRecursive(entry.path());
        }
    }
}

void RecursiveFsWatcher::removeWatchRecursive(int wd)
{
    if (wd < 0 || m_inotifyFd < 0)
        return;
    inotify_rm_watch(m_inotifyFd, wd);
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

    while (m_impl->isRunning.load(std::memory_order_relaxed)) {
        int nfds = epoll_wait(m_epollFd, events, maxEvents, -1);
        if (nfds < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            if (fd == m_stopEventFd) {
                uint64_t val = 0;
                ssize_t s = read(m_stopEventFd, &val, sizeof(val));
                (void)s;
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

    while ((bytesRead = read(m_inotifyFd, buffer, sizeof(buffer))) > 0) {
        ssize_t offset = 0;
        while (offset < bytesRead) {
            auto *event = reinterpret_cast<struct inotify_event *>(&buffer[offset]);

            // Handle kernel queue overflow
            if (event->mask & IN_Q_OVERFLOW) {
                std::lock_guard<std::mutex> lock(m_impl->mutex);
                auto now = std::chrono::steady_clock::now();
                FsWatchEvent ovEvt{.action = FsWatchAction::CatalogOverflowReconcile,
                                   .published_file_id = 0,
                                   .full_path = ""};
                m_debounceBuffer[0] = ovEvt;
                m_impl->itemTiming[0] = {now, now};

                // Arm timer
                struct itimerspec ts{};
                ts.it_value.tv_sec = 0;
                ts.it_value.tv_nsec = 1;
                timerfd_settime(m_timerFd, 0, &ts, nullptr);

                offset += sizeof(struct inotify_event) + event->len;
                continue;
            }

            if (event->wd >= 0) {
                std::filesystem::path dirPath;
                {
                    std::lock_guard<std::mutex> lock(m_impl->mutex);
                    auto it = m_wdToPath.find(event->wd);
                    if (it != m_wdToPath.end()) {
                        dirPath = it->second;
                    }
                }

                if (!dirPath.empty()) {
                    std::filesystem::path fullPath =
                        (event->len > 0) ? (dirPath / event->name) : dirPath;

                    // Recursive inotify + Catch-Up Iterator
                    if (event->mask & IN_ISDIR) {
                        if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
                            {
                                std::lock_guard<std::mutex> lock(m_impl->mutex);
                                addWatchRecursive(fullPath);
                            }

                            // Catch-Up Iterator: immediately iterate newly created directory
                            // to capture files created inside race window before the watch was
                            // attached
                            std::error_code ec;
                            for (const auto &entry : std::filesystem::recursive_directory_iterator(
                                     fullPath,
                                     std::filesystem::directory_options::skip_permission_denied,
                                     ec)) {
                                uint64_t childId =
                                    extractPublishedFileId(entry.path(), m_impl->watchRoots);
                                std::filesystem::path itemDir =
                                    resolveItemDirectory(entry.path(), m_impl->watchRoots, childId);

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
                            auto pit = m_pathToWd.find(fullPath.string());
                            if (pit != m_pathToWd.end()) {
                                removeWatchRecursive(pit->second);
                            }
                        }
                    }

                    // Determine event action
                    FsWatchAction action = FsWatchAction::ItemModified;
                    if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
                        action = FsWatchAction::ItemCreated;
                    } else if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
                        action = FsWatchAction::ItemDeleted;
                    } else if (event->mask & (IN_MODIFY | IN_CLOSE_WRITE)) {
                        action = FsWatchAction::ItemModified;
                    }

                    uint64_t id = extractPublishedFileId(fullPath, m_impl->watchRoots);
                    std::filesystem::path itemDir =
                        resolveItemDirectory(fullPath, m_impl->watchRoots, id);

                    {
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
                                // Retain ItemCreated status for brand-new items receiving write
                                // bursts
                            } else {
                                bIt->second.action = action;
                                bIt->second.full_path = itemDir;
                            }
                        } else {
                            m_debounceBuffer[id] = FsWatchEvent{
                                .action = action, .published_file_id = id, .full_path = itemDir};
                        }

                        // Re-arm debounce timer for sliding quiet window / flush deadline ceiling
                        auto earliest = std::chrono::steady_clock::time_point::max();
                        for (const auto &[itemId, timing] : m_impl->itemTiming) {
                            auto quietDeadline =
                                timing.lastSeen + std::chrono::milliseconds(m_debounceMs);
                            auto hardDeadline = timing.firstSeen + std::chrono::milliseconds(1000);
                            auto deadline = std::min(quietDeadline, hardDeadline);
                            if (deadline < earliest)
                                earliest = deadline;
                        }

                        int64_t diffNs = 1;
                        if (earliest > now) {
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
                        timerfd_settime(m_timerFd, 0, &ts, nullptr);
                    }
                }
            }

            offset += sizeof(struct inotify_event) + event->len;
        }
    }
}

void RecursiveFsWatcher::processFanotifyEvents()
{
    alignas(struct fanotify_event_metadata) char buffer[4096];
    ssize_t bytesRead = 0;

    while ((bytesRead = read(m_fanotifyFd, buffer, sizeof(buffer))) > 0) {
        auto *meta = reinterpret_cast<struct fanotify_event_metadata *>(buffer);
        while (FAN_EVENT_OK(meta, bytesRead)) {
            if (meta->mask & FAN_Q_OVERFLOW) {
                std::lock_guard<std::mutex> lock(m_impl->mutex);
                auto now = std::chrono::steady_clock::now();
                m_debounceBuffer[0] =
                    FsWatchEvent{.action = FsWatchAction::CatalogOverflowReconcile,
                                 .published_file_id = 0,
                                 .full_path = ""};
                m_impl->itemTiming[0] = {now, now};
            } else {
                FsWatchAction action = FsWatchAction::ItemModified;
                if (meta->mask & FAN_CREATE) {
                    action = FsWatchAction::ItemCreated;
                } else if (meta->mask & FAN_DELETE) {
                    action = FsWatchAction::ItemDeleted;
                } else if (meta->mask & (FAN_MODIFY | FAN_MOVE)) {
                    action = FsWatchAction::ItemModified;
                }

                // In privileged mode with FAN_REPORT_FID, trigger catalog update
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

        // Arm debounce timer
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        struct itimerspec ts{};
        ts.it_value.tv_sec = m_debounceMs / 1000;
        ts.it_value.tv_nsec = (m_debounceMs % 1000) * 1'000'000LL;
        if (ts.it_value.tv_sec == 0 && ts.it_value.tv_nsec == 0)
            ts.it_value.tv_nsec = 1;
        timerfd_settime(m_timerFd, 0, &ts, nullptr);
    }
}

void RecursiveFsWatcher::flushDebounceQueue()
{
    uint64_t expirations = 0;
    if (m_timerFd >= 0) {
        ssize_t s = read(m_timerFd, &expirations, sizeof(expirations));
        (void)s;
    }

    std::vector<FsWatchEvent> readyEvents;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        auto now = std::chrono::steady_clock::now();

        for (auto it = m_debounceBuffer.begin(); it != m_debounceBuffer.end();) {
            uint64_t id = it->first;
            auto tIt = m_impl->itemTiming.find(id);
            bool readyToFlush = false;

            if (tIt != m_impl->itemTiming.end()) {
                auto quietDeadline = tIt->second.lastSeen + std::chrono::milliseconds(m_debounceMs);
                auto hardDeadline = tIt->second.firstSeen + std::chrono::milliseconds(1000);
                if (now >= quietDeadline || now >= hardDeadline) {
                    readyToFlush = true;
                }
            } else {
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

        // Re-arm timer for remaining items or disarm if empty
        if (m_debounceBuffer.empty() || m_timerFd < 0) {
            struct itimerspec disarm{};
            if (m_timerFd >= 0) {
                timerfd_settime(m_timerFd, 0, &disarm, nullptr);
            }
        } else {
            auto earliest = std::chrono::steady_clock::time_point::max();
            for (const auto &[id, timing] : m_impl->itemTiming) {
                auto quietDeadline = timing.lastSeen + std::chrono::milliseconds(m_debounceMs);
                auto hardDeadline = timing.firstSeen + std::chrono::milliseconds(1000);
                auto deadline = std::min(quietDeadline, hardDeadline);
                if (deadline < earliest)
                    earliest = deadline;
            }
            int64_t diffNs = 1;
            if (earliest > now) {
                diffNs =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(earliest - now).count();
            }
            struct itimerspec ts{};
            ts.it_value.tv_sec = diffNs / 1'000'000'000LL;
            ts.it_value.tv_nsec = diffNs % 1'000'000'000LL;
            if (ts.it_value.tv_sec == 0 && ts.it_value.tv_nsec == 0) {
                ts.it_value.tv_nsec = 1;
            }
            timerfd_settime(m_timerFd, 0, &ts, nullptr);
        }
    }

    if (!readyEvents.empty() && m_callback) {
        m_callback(readyEvents);
    }
}

} // namespace wwal::studio
