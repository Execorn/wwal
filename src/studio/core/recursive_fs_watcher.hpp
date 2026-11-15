// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
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
    std::filesystem::path old_path{};
};

using FsWatchCallback = std::function<void(const std::vector<FsWatchEvent> &)>;
using FsErrorCallback = std::function<void(const std::string &)>;

class RecursiveFsWatcher {
  public:
    explicit RecursiveFsWatcher(FsWatchCallback callback, uint32_t debounce_ms = 300);
    ~RecursiveFsWatcher();

    RecursiveFsWatcher(const RecursiveFsWatcher &) = delete;
    RecursiveFsWatcher &operator=(const RecursiveFsWatcher &) = delete;

    void setErrorCallback(FsErrorCallback callback);

    bool addWatchRoot(const std::filesystem::path &root_path);
    void removeWatchRoot(const std::filesystem::path &root_path);
    void start();
    void stop();

    [[nodiscard]] bool isUsingFanotify() const noexcept { return false; }
    [[nodiscard]] size_t totalWatches() const noexcept;

  private:
    void workerThreadLoop();
    void processInotifyEvents();
    void flushDebounceQueue();

    FsWatchCallback m_callback;
    uint32_t m_debounceMs;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wwal::studio

namespace waywal::studio {
using FsWatchAction = ::wwal::studio::FsWatchAction;
using FsWatchEvent = ::wwal::studio::FsWatchEvent;
using FsWatchCallback = ::wwal::studio::FsWatchCallback;
using FsErrorCallback = ::wwal::studio::FsErrorCallback;
using RecursiveFsWatcher = ::wwal::studio::RecursiveFsWatcher;
} // namespace waywal::studio
