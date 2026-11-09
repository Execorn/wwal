// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace wwal::studio {

enum class FsWatchAction {
    ItemCreated,
    ItemModified,
    ItemDeleted,
    ItemRenamed,
    CatalogOverflowReconcile
};

struct FsWatchEvent {
    FsWatchAction action{FsWatchAction::ItemModified};
    uint64_t published_file_id{0};
    std::filesystem::path full_path;
};

using FsWatchCallback = std::function<void(const std::vector<FsWatchEvent> &)>;

class RecursiveFsWatcher {
  public:
    explicit RecursiveFsWatcher(FsWatchCallback callback, uint32_t debounce_ms = 300);
    ~RecursiveFsWatcher();

    RecursiveFsWatcher(const RecursiveFsWatcher &) = delete;
    RecursiveFsWatcher &operator=(const RecursiveFsWatcher &) = delete;

    bool addWatchRoot(const std::filesystem::path &root_path);
    void removeWatchRoot(const std::filesystem::path &root_path);
    void start();
    void stop();

    [[nodiscard]] bool isUsingFanotify() const noexcept { return m_usingFanotify; }
    [[nodiscard]] size_t totalWatches() const noexcept;

  private:
    void workerThreadLoop();
    void processInotifyEvents();
    void processFanotifyEvents();
    void flushDebounceQueue();
    void addWatchRecursive(const std::filesystem::path &path);
    void removeWatchRecursive(int wd);

    FsWatchCallback m_callback;
    uint32_t m_debounceMs;
    bool m_usingFanotify{false};

    int m_epollFd{-1};
    int m_inotifyFd{-1};
    int m_fanotifyFd{-1};
    int m_timerFd{-1};
    int m_stopEventFd{-1};

    std::unordered_map<int, std::filesystem::path> m_wdToPath;
    std::unordered_map<std::string, int> m_pathToWd;
    std::unordered_map<uint64_t, FsWatchEvent> m_debounceBuffer;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wwal::studio

namespace waywal::studio {
using FsWatchAction = ::wwal::studio::FsWatchAction;
using FsWatchEvent = ::wwal::studio::FsWatchEvent;
using FsWatchCallback = ::wwal::studio::FsWatchCallback;
using RecursiveFsWatcher = ::wwal::studio::RecursiveFsWatcher;
} // namespace waywal::studio
