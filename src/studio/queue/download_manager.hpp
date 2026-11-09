// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <functional>
#include <vector>

namespace wwal::studio {

enum class TaskPriority { Prefetch = 1, BackgroundUpdate = 10, Interactive = 50, Immediate = 100 };

enum class TaskState {
    Queued,
    Probing,
    Downloading,
    Verifying,
    Extracting,
    Indexing,
    Installed,
    Failed
};

struct DownloadProgress {
    uint64_t published_file_id{0};
    std::string openhub_package_id; // UUID or CID for Open Hub packages
    TaskState state{TaskState::Queued};
    uint64_t bytes_downloaded{0};
    uint64_t bytes_total{0};
    float progress_pct{0.0f};
    float speed_bytes_per_sec{0.0f};
    uint32_t eta_seconds{0};
    std::string error_message;
};

using ProgressCallback = std::function<void(const DownloadProgress &)>;

struct DownloadRequest {
    enum class SourceKind { SteamWorkshop, OpenHubPackage };
    SourceKind kind{SourceKind::SteamWorkshop};
    uint64_t steam_published_file_id{0};
    std::string openhub_package_id; // UUID string or sha256:... CID
    std::string openhub_download_url;
    TaskPriority priority{TaskPriority::Interactive};
};

class DownloadManager {
  public:
    DownloadManager();
    ~DownloadManager();

    DownloadManager(const DownloadManager &) = delete;
    DownloadManager &operator=(const DownloadManager &) = delete;
    DownloadManager(DownloadManager &&) noexcept;
    DownloadManager &operator=(DownloadManager &&) noexcept;

    bool initialize();
    void shutdown();

    // Enqueue overloads for Steam Workshop and WayWal Open Hub
    void enqueue(uint64_t published_file_id, TaskPriority priority, ProgressCallback callback);
    void enqueue(const std::string &package_id_or_cid, const std::string &download_url,
                 TaskPriority priority, ProgressCallback callback);
    void enqueue(const DownloadRequest &request, ProgressCallback callback);

    void cancel(uint64_t published_file_id);
    void cancelOpenHub(const std::string &package_id_or_cid);
    void pause();
    void resume();

    void setBandwidthLimit(uint64_t bytes_per_sec);
    [[nodiscard]] uint64_t bandwidthLimit() const noexcept;

    [[nodiscard]] bool isSteamClientRunning() const noexcept;
    [[nodiscard]] size_t activeTransfers() const noexcept;

    // Simulation / testing hooks
    struct SimulationHooks {
        float drop_at_percent{0.0f};               // 0.0 = disabled, e.g. 0.5 = drop at 50%
        bool simulate_http_200_on_resume{false};   // respond 200 instead of 206
        int simulate_rate_limit_count{0};          // return 429 N times
        bool force_backend_failure_a{false};       // force Backend A to fail
        bool force_backend_failure_b{false};       // force Backend B to fail
        bool force_backend_failure_c{false};       // force Backend C to fail
        bool dry_run_mode{false};                  // stream synthetic data without real network
        std::string mock_data;                     // mock data payload
        uint64_t mock_total_size{0};               // synthetic content length
        uint32_t simulated_drop_count{0};          // telemetry: count of drops executed
        uint32_t simulated_429_count{0};           // telemetry: count of 429 retries
        bool simulated_truncation_occurred{false}; // telemetry: whether truncation guard triggered
    };

    void setSimulationHooks(const SimulationHooks &hooks);
    [[nodiscard]] SimulationHooks simulationHooks() const;
    void resetSimulationStats();

    void setDownloadDirectory(const std::string &dir);
    [[nodiscard]] std::string downloadDirectory() const;

    [[nodiscard]] size_t queuedTasks() const noexcept;
    [[nodiscard]] bool isPaused() const noexcept;
    void waitUntilIdle();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wwal::studio
