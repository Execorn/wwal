// SPDX-License-Identifier: GPL-3.0-or-later
#include "queue/download_manager.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QVariant>

namespace wwal::studio {

namespace fs = std::filesystem;

/* ========================================================================= */
/* Token Bucket Bandwidth Governor (§5.6.3)                                  */
/* ========================================================================= */

class TokenBucket {
  public:
    TokenBucket() = default;

    void setLimit(uint64_t bytes_per_sec)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_rate_limit = bytes_per_sec;
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

    void pause()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_paused = true;
    }

    void resume()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_paused = false;
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

    void shutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shutdown = true;
        m_cv.notify_all();
    }

    bool consume(uint64_t bytes, const std::atomic<bool> &cancel_flag)
    {
        if (bytes == 0)
            return true;

        std::unique_lock<std::mutex> lock(m_mutex);
        while (bytes > 0) {
            if (m_shutdown || cancel_flag.load(std::memory_order_relaxed)) {
                return false;
            }

            if (m_paused) {
                m_cv.wait(lock, [&] {
                    return !m_paused || m_shutdown || cancel_flag.load(std::memory_order_relaxed);
                });
                if (m_shutdown || cancel_flag.load(std::memory_order_relaxed))
                    return false;
            }

            if (m_rate_limit == 0) {
                return true; // Unlimited
            }

            refillLocked();

            if (m_tokens >= bytes) {
                m_tokens -= bytes;
                return true;
            }

            // Consume whatever tokens are available and wait for the rest
            uint64_t available = m_tokens;
            bytes -= available;
            m_tokens = 0;

            double wait_sec = static_cast<double>(bytes) / static_cast<double>(m_rate_limit);
            auto wait_dur = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::duration<double>(wait_sec));

            m_cv.wait_for(lock, wait_dur, [&] {
                return m_paused || m_shutdown || cancel_flag.load(std::memory_order_relaxed);
            });
        }
        return true;
    }

  private:
    void refillLocked()
    {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - m_last_refill).count();
        if (elapsed > 0.0) {
            uint64_t delta_tokens = static_cast<uint64_t>(elapsed * m_rate_limit);
            if (delta_tokens > 0) {
                uint64_t tenth = static_cast<uint64_t>(m_rate_limit * 0.1);
                uint64_t burst_cap =
                    std::max<uint64_t>(65536ULL, std::min<uint64_t>(m_rate_limit, tenth));
                m_tokens = std::min(m_tokens + delta_tokens, burst_cap);
                m_last_refill = now;
            }
        }
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    uint64_t m_rate_limit{0}; // 0 = unlimited
    uint64_t m_tokens{0};
    std::chrono::steady_clock::time_point m_last_refill{std::chrono::steady_clock::now()};
    bool m_paused{false};
    bool m_shutdown{false};
};

/* ========================================================================= */
/* EWMA Speed and ETA Estimator (§5.6.3, alpha = 0.2)                        */
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
        if (dt >= 0.05) { // Sample every 50ms
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
/* Backend A: Steamworks SDK Lazy Dynamic Loader (§5.6.2, §4.1)              */
/* ========================================================================= */

class SteamworksBackend {
  public:
    SteamworksBackend() = default;
    ~SteamworksBackend() { shutdown(); }

    bool initialize(uint32_t appId = 431960)
    {
        if (m_initialized)
            return true;

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
            m_libHandle = ::dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
            if (m_libHandle)
                break;
        }

        if (!m_libHandle) {
            return false;
        }

        pfn_SteamAPI_InitFlat = (SteamAPI_InitFlat_fn)::dlsym(m_libHandle, "SteamAPI_InitFlat");
        pfn_SteamAPI_Shutdown = (SteamAPI_Shutdown_fn)::dlsym(m_libHandle, "SteamAPI_Shutdown");
        pfn_SteamAPI_IsSteamRunning =
            (SteamAPI_IsSteamRunning_fn)::dlsym(m_libHandle, "SteamAPI_IsSteamRunning");
        pfn_SteamAPI_RunCallbacks =
            (SteamAPI_RunCallbacks_fn)::dlsym(m_libHandle, "SteamAPI_RunCallbacks");
        pfn_SteamAPI_SteamUGC_v016 =
            (SteamAPI_SteamUGC_v016_fn)::dlsym(m_libHandle, "SteamAPI_SteamUGC_v016");
        pfn_SteamAPI_ISteamUGC_SubscribeItem = (SteamAPI_ISteamUGC_SubscribeItem_fn)::dlsym(
            m_libHandle, "SteamAPI_ISteamUGC_SubscribeItem");
        pfn_SteamAPI_ISteamUGC_DownloadItem = (SteamAPI_ISteamUGC_DownloadItem_fn)::dlsym(
            m_libHandle, "SteamAPI_ISteamUGC_DownloadItem");
        pfn_SteamAPI_ISteamUGC_GetItemDownloadInfo =
            (SteamAPI_ISteamUGC_GetItemDownloadInfo_fn)::dlsym(
                m_libHandle, "SteamAPI_ISteamUGC_GetItemDownloadInfo");
        pfn_SteamAPI_ISteamUGC_GetItemInstallInfo =
            (SteamAPI_ISteamUGC_GetItemInstallInfo_fn)::dlsym(
                m_libHandle, "SteamAPI_ISteamUGC_GetItemInstallInfo");
        pfn_SteamAPI_ISteamUGC_GetItemState = (SteamAPI_ISteamUGC_GetItemState_fn)::dlsym(
            m_libHandle, "SteamAPI_ISteamUGC_GetItemState");

        if (!pfn_SteamAPI_InitFlat || !pfn_SteamAPI_IsSteamRunning ||
            !pfn_SteamAPI_IsSteamRunning()) {
            shutdown();
            return false;
        }

        // Set SteamAppId environment variable prior to calling SteamAPI_InitFlat
        std::string appIdStr = std::to_string(appId);
        ::setenv("SteamAppId", appIdStr.c_str(), 0);

        char err[1024] = {0};
        if (!pfn_SteamAPI_InitFlat(err)) {
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
        if (m_libHandle) {
            ::dlclose(m_libHandle);
            m_libHandle = nullptr;
        }
        m_ugc = nullptr;
        m_initialized = false;
    }

    [[nodiscard]] bool isRunning() const noexcept
    {
        if (!m_initialized || !pfn_SteamAPI_IsSteamRunning)
            return false;
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
        if (!m_initialized || !m_ugc || !pfn_SteamAPI_ISteamUGC_SubscribeItem)
            return false;
        pfn_SteamAPI_ISteamUGC_SubscribeItem(m_ugc, publishedFileId);
        return true;
    }

    bool downloadItem(uint64_t publishedFileId, bool highPriority = true)
    {
        if (!m_initialized || !m_ugc || !pfn_SteamAPI_ISteamUGC_DownloadItem)
            return false;
        return pfn_SteamAPI_ISteamUGC_DownloadItem(m_ugc, publishedFileId, highPriority);
    }

    bool getItemDownloadInfo(uint64_t publishedFileId, uint64_t *punBytesDownloaded,
                             uint64_t *punBytesTotal)
    {
        if (!m_initialized || !m_ugc || !pfn_SteamAPI_ISteamUGC_GetItemDownloadInfo)
            return false;
        return pfn_SteamAPI_ISteamUGC_GetItemDownloadInfo(m_ugc, publishedFileId,
                                                          punBytesDownloaded, punBytesTotal);
    }

    uint32_t getItemState(uint64_t publishedFileId)
    {
        if (!m_initialized || !m_ugc || !pfn_SteamAPI_ISteamUGC_GetItemState)
            return 0;
        return pfn_SteamAPI_ISteamUGC_GetItemState(m_ugc, publishedFileId);
    }

    bool getItemInstallInfo(uint64_t publishedFileId, uint64_t *punSizeOnDisk, char *pchFolder,
                            uint32_t cchFolderSize, uint32_t *punTimeStamp)
    {
        if (!m_initialized || !m_ugc || !pfn_SteamAPI_ISteamUGC_GetItemInstallInfo)
            return false;
        return pfn_SteamAPI_ISteamUGC_GetItemInstallInfo(m_ugc, publishedFileId, punSizeOnDisk,
                                                         pchFolder, cchFolderSize, punTimeStamp);
    }

  private:
    using SteamAPI_InitFlat_fn = bool (*)(char *pszErr);
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
/* Curl Dynamic Binding Helper (§5.6.2, Invariant 3)                         */
/* ========================================================================= */

struct CurlBinding {
    void *handle{nullptr};

    using CURL = void;
    using CURLcode = int;
    struct curl_slist {
        char *data;
        struct curl_slist *next;
    };

    using easy_init_fn = CURL *(*)();
    using easy_cleanup_fn = void (*)(CURL *);
    using easy_setopt_fn = CURLcode (*)(CURL *, int, ...);
    using easy_perform_fn = CURLcode (*)(CURL *);
    using easy_getinfo_fn = CURLcode (*)(CURL *, int, ...);
    using easy_strerror_fn = const char *(*)(CURLcode);
    using slist_append_fn = struct curl_slist *(*)(struct curl_slist *, const char *);
    using slist_free_all_fn = void (*)(struct curl_slist *);

    easy_init_fn easy_init{nullptr};
    easy_cleanup_fn easy_cleanup{nullptr};
    easy_setopt_fn easy_setopt{nullptr};
    easy_perform_fn easy_perform{nullptr};
    easy_getinfo_fn easy_getinfo{nullptr};
    easy_strerror_fn easy_strerror{nullptr};
    slist_append_fn slist_append{nullptr};
    slist_free_all_fn slist_free_all{nullptr};

    // Standard libcurl ABI constants
    static constexpr int OPT_URL = 10002;
    static constexpr int OPT_WRITEFUNCTION = 20011;
    static constexpr int OPT_WRITEDATA = 10001;
    static constexpr int OPT_POSTFIELDS = 10015;
    static constexpr int OPT_POSTFIELDSIZE = 10060;
    static constexpr int OPT_HTTPHEADER = 10023;
    static constexpr int OPT_FOLLOWLOCATION = 52;
    static constexpr int OPT_MAXREDIRS = 68;
    static constexpr int OPT_TIMEOUT = 13;
    static constexpr int OPT_CONNECTTIMEOUT = 78;
    static constexpr int OPT_RANGE = 10007;
    static constexpr int OPT_USERAGENT = 10018;
    static constexpr int OPT_NOSIGNAL = 99;
    static constexpr int OPT_FAILONERROR = 45;
    static constexpr int OPT_HTTP_VERSION = 84;
    static constexpr long VAL_HTTP_VERSION_2_0 = 3;
    static constexpr int INFO_RESPONSE_CODE = 0x200000 + 2;

    bool load()
    {
        if (handle)
            return true;
        handle = ::dlopen("libcurl.so.4", RTLD_LAZY | RTLD_LOCAL);
        if (!handle)
            handle = ::dlopen("libcurl.so", RTLD_LAZY | RTLD_LOCAL);
        if (!handle)
            return false;

        *(void **)&easy_init = ::dlsym(handle, "curl_easy_init");
        *(void **)&easy_cleanup = ::dlsym(handle, "curl_easy_cleanup");
        *(void **)&easy_setopt = ::dlsym(handle, "curl_easy_setopt");
        *(void **)&easy_perform = ::dlsym(handle, "curl_easy_perform");
        *(void **)&easy_getinfo = ::dlsym(handle, "curl_easy_getinfo");
        *(void **)&easy_strerror = ::dlsym(handle, "curl_easy_strerror");
        *(void **)&slist_append = ::dlsym(handle, "curl_slist_append");
        *(void **)&slist_free_all = ::dlsym(handle, "curl_slist_free_all");

        return (easy_init && easy_cleanup && easy_setopt && easy_perform);
    }

    void unload()
    {
        if (handle) {
            ::dlclose(handle);
            handle = nullptr;
        }
    }
};

/* ========================================================================= */
/* Internal Task Representation                                              */
/* ========================================================================= */

struct Task {
    DownloadRequest request;
    ProgressCallback callback;
    std::atomic<bool> cancelled{false};
    std::atomic<TaskState> state{TaskState::Queued};
    uint64_t sequence_id{0};

    uint64_t bytes_downloaded{0};
    uint64_t bytes_total{0};
    float progress_pct{0.0f};
    float speed_bytes_per_sec{0.0f};
    uint32_t eta_seconds{0};
    std::string error_message;
    std::string installed_path;

    SpeedEstimator speed_estimator;

    void notifyProgress()
    {
        if (!callback)
            return;
        DownloadProgress prog;
        prog.published_file_id = request.steam_published_file_id;
        prog.openhub_package_id = request.openhub_package_id;
        prog.state = state.load(std::memory_order_relaxed);
        prog.bytes_downloaded = bytes_downloaded;
        prog.bytes_total = bytes_total;
        prog.progress_pct = progress_pct;
        prog.speed_bytes_per_sec = speed_bytes_per_sec;
        prog.eta_seconds = eta_seconds;
        prog.error_message = error_message;
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

/* ========================================================================= */
/* DownloadManager::Impl Implementation                                      */
/* ========================================================================= */

struct DownloadManager::Impl {
    explicit Impl(DownloadManager *q) : q_ptr(q) {}
    ~Impl() { shutdown(); }

    DownloadManager *q_ptr{nullptr};

    std::atomic<bool> m_running{false};
    TokenBucket m_tokenBucket;
    SteamworksBackend m_steamworks;
    CurlBinding m_curl;
    DownloadManager::SimulationHooks m_simHooks;

    std::string m_downloadDir;
    std::mt19937 m_rng{std::random_device{}()};

    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::priority_queue<std::shared_ptr<Task>, std::vector<std::shared_ptr<Task>>, TaskComparator>
        m_priorityQueue;
    std::vector<std::shared_ptr<Task>> m_allTasks;

    std::atomic<size_t> m_activeTransfers{0};
    std::atomic<uint64_t> m_sequenceCounter{0};
    std::condition_variable m_idleCv;

    std::atomic<int> m_currentActivePriority{0};
    std::atomic<bool> m_preemptActiveTask{false};

    std::thread m_workerThread;
    std::thread m_immediateWorkerThread;

    bool initialize()
    {
        if (m_running)
            return true;

        // Default download directory: ~/.local/share/waywal/downloads
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

        // Attempt lazy load of libsteam_api.so
        m_steamworks.initialize(431960);

        // Attempt lazy load of libcurl.so.4
        m_curl.load();

        m_running = true;
        m_workerThread = std::thread(&Impl::workerLoop, this);
        return true;
    }

    void shutdown()
    {
        if (!m_running)
            return;
        m_running = false;

        m_tokenBucket.shutdown();

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            for (auto &t : m_allTasks) {
                t->cancelled.store(true, std::memory_order_relaxed);
            }
            m_queueCv.notify_all();
        }

        if (m_workerThread.joinable()) {
            m_workerThread.join();
        }
        if (m_immediateWorkerThread.joinable()) {
            m_immediateWorkerThread.join();
        }

        m_steamworks.shutdown();
        m_curl.unload();
        m_idleCv.notify_all();
    }

    void enqueueTask(const DownloadRequest &request, ProgressCallback callback)
    {
        auto task = std::make_shared<Task>();
        task->request = request;
        task->callback = std::move(callback);
        task->sequence_id = ++m_sequenceCounter;
        task->state.store(TaskState::Queued, std::memory_order_relaxed);

        task->notifyProgress();

        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_priorityQueue.push(task);
        m_allTasks.push_back(task);

        if (request.priority == TaskPriority::Immediate) {
            int activePrio = m_currentActivePriority.load(std::memory_order_relaxed);
            if (activePrio > 0 && activePrio <= static_cast<int>(TaskPriority::BackgroundUpdate)) {
                m_preemptActiveTask.store(true, std::memory_order_relaxed);
            }
        }

        m_queueCv.notify_all();
    }

    void cancelTask(uint64_t publishedFileId)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        for (auto &t : m_allTasks) {
            if (t->request.kind == DownloadRequest::SourceKind::SteamWorkshop &&
                t->request.steam_published_file_id == publishedFileId) {
                t->cancelled.store(true, std::memory_order_relaxed);
            }
        }
    }

    void cancelOpenHubTask(const std::string &packageIdOrCid)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        for (auto &t : m_allTasks) {
            if (t->request.kind == DownloadRequest::SourceKind::OpenHubPackage &&
                t->request.openhub_package_id == packageIdOrCid) {
                t->cancelled.store(true, std::memory_order_relaxed);
            }
        }
    }

    void waitUntilIdle()
    {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        m_idleCv.wait(lock, [this] {
            return m_priorityQueue.empty() &&
                   m_activeTransfers.load(std::memory_order_relaxed) == 0;
        });
    }

    [[nodiscard]] size_t queuedTasks() const noexcept
    {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(m_queueMutex));
        return m_priorityQueue.size();
    }

    /* --------------------------------------------------------------------- */
    /* Worker Thread Loop                                                    */
    /* --------------------------------------------------------------------- */

    void workerLoop()
    {
        while (m_running.load(std::memory_order_relaxed)) {
            std::shared_ptr<Task> currentTask;
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_queueCv.wait(lock, [this] {
                    return !m_running.load(std::memory_order_relaxed) || !m_priorityQueue.empty();
                });

                if (!m_running.load(std::memory_order_relaxed))
                    break;

                currentTask = m_priorityQueue.top();
                m_priorityQueue.pop();
            }

            if (!currentTask)
                continue;

            if (currentTask->cancelled.load(std::memory_order_relaxed)) {
                currentTask->state.store(TaskState::Failed, std::memory_order_relaxed);
                currentTask->error_message = "Cancelled before execution";
                currentTask->notifyProgress();
                continue;
            }

            m_activeTransfers.fetch_add(1, std::memory_order_relaxed);
            executeTask(currentTask);
            m_activeTransfers.fetch_sub(1, std::memory_order_relaxed);

            m_idleCv.notify_all();
        }
    }

    /* --------------------------------------------------------------------- */
    /* Task Execution: 3-Tier Arbitration                                    */
    /* --------------------------------------------------------------------- */

    void executeTask(const std::shared_ptr<Task> &task)
    {
        m_currentActivePriority.store(static_cast<int>(task->request.priority),
                                      std::memory_order_relaxed);
        task->state.store(TaskState::Probing, std::memory_order_relaxed);
        task->notifyProgress();

        bool success = false;
        std::string failureReason;

        if (task->request.kind == DownloadRequest::SourceKind::SteamWorkshop) {
            // Tier 1: Backend A (Steam client via lazy dlopen)
            bool tryBackendA = true;
            if (m_simHooks.force_backend_failure_a) {
                tryBackendA = false;
            }

            if (tryBackendA && m_steamworks.isRunning()) {
                if (executeBackendA(task)) {
                    success = true;
                } else {
                    failureReason += "Backend A failed; ";
                }
            } else {
                failureReason += "Backend A unavailable; ";
            }

            // Tier 2: Backend B (Steam Web API + HTTP/2 Direct Worker)
            if (!success && !task->cancelled.load(std::memory_order_relaxed)) {
                bool tryBackendB = true;
                if (m_simHooks.force_backend_failure_b) {
                    tryBackendB = false;
                }

                if (tryBackendB) {
                    if (executeBackendB(task)) {
                        success = true;
                    } else {
                        failureReason += "Backend B failed; ";
                    }
                } else {
                    failureReason += "Backend B skipped (simulated failure); ";
                }
            }

            // Tier 3: Backend C (Headless steamcmd runner)
            if (!success && !task->cancelled.load(std::memory_order_relaxed)) {
                bool tryBackendC = true;
                if (m_simHooks.force_backend_failure_c) {
                    tryBackendC = false;
                }

                if (tryBackendC) {
                    if (executeBackendC(task)) {
                        success = true;
                    } else {
                        failureReason += "Backend C failed; ";
                    }
                } else {
                    failureReason += "Backend C skipped (simulated failure); ";
                }
            }
        } else {
            // Open Hub Package: Direct HTTP/2 package download (Backend B logic)
            if (executeBackendB(task)) {
                success = true;
            } else {
                failureReason = "Open Hub download failed";
            }
        }

        if (task->cancelled.load(std::memory_order_relaxed)) {
            task->state.store(TaskState::Failed, std::memory_order_relaxed);
            task->error_message = "Download cancelled by user";
            task->notifyProgress();
            m_currentActivePriority.store(0, std::memory_order_relaxed);
            return;
        }

        if (m_preemptActiveTask.load(std::memory_order_relaxed) &&
            task->request.priority <= TaskPriority::BackgroundUpdate) {
            // Task was preempted by an Immediate task
            task->state.store(TaskState::Queued, std::memory_order_relaxed);
            task->notifyProgress();
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_priorityQueue.push(task);
            }
            m_preemptActiveTask.store(false, std::memory_order_relaxed);
            m_currentActivePriority.store(0, std::memory_order_relaxed);
            return;
        }

        if (success) {
            // State: Verifying -> Extracting -> Indexing -> Installed
            task->state.store(TaskState::Verifying, std::memory_order_relaxed);
            task->notifyProgress();

            task->state.store(TaskState::Extracting, std::memory_order_relaxed);
            task->notifyProgress();

            task->state.store(TaskState::Indexing, std::memory_order_relaxed);
            task->notifyProgress();

            task->state.store(TaskState::Installed, std::memory_order_relaxed);
            task->progress_pct = 100.0f;
            task->bytes_downloaded = task->bytes_total;
            task->speed_bytes_per_sec = 0.0f;
            task->eta_seconds = 0;
            task->notifyProgress();
        } else {
            task->state.store(TaskState::Failed, std::memory_order_relaxed);
            task->error_message = failureReason.empty() ? "All backends exhausted" : failureReason;
            task->notifyProgress();
        }
        m_currentActivePriority.store(0, std::memory_order_relaxed);
    }

    /* --------------------------------------------------------------------- */
    /* Backend A Execution (Steamworks SDK Flat C ABI)                       */
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

        task->state.store(TaskState::Downloading, std::memory_order_relaxed);
        task->speed_estimator.reset(0);

        while (m_running.load(std::memory_order_relaxed) &&
               !task->cancelled.load(std::memory_order_relaxed)) {
            m_steamworks.runCallbacks();

            uint64_t bytesDownloaded = 0;
            uint64_t bytesTotal = 0;
            m_steamworks.getItemDownloadInfo(publishedFileId, &bytesDownloaded, &bytesTotal);

            uint32_t state = m_steamworks.getItemState(publishedFileId);

            if (bytesTotal > 0) {
                task->bytes_downloaded = bytesDownloaded;
                task->bytes_total = bytesTotal;
                task->progress_pct = static_cast<float>(bytesDownloaded * 100.0 / bytesTotal);
                task->speed_estimator.update(bytesDownloaded, bytesTotal, task->speed_bytes_per_sec,
                                             task->eta_seconds);
                task->notifyProgress();
            }

            // k_EItemStateInstalled = 4
            if ((state & 4) != 0) {
                uint64_t sizeOnDisk = 0;
                char folder[1024] = {0};
                uint32_t timestamp = 0;
                if (m_steamworks.getItemInstallInfo(publishedFileId, &sizeOnDisk, folder,
                                                    sizeof(folder), &timestamp)) {
                    task->installed_path = folder;
                }
                return true;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        return false;
    }

    /* --------------------------------------------------------------------- */
    /* Backend B Execution (Steam Web API + HTTP/2 Direct Worker Queue)      */
    /* --------------------------------------------------------------------- */

    double calculateFullJitterBackoff(int attempt, double base_sec = 0.5, double max_sec = 30.0)
    {
        double exp_backoff = base_sec * std::pow(2.0, attempt);
        double ceiling = std::min(max_sec, exp_backoff);
        std::uniform_real_distribution<double> dist(0.0, ceiling);
        return dist(m_rng);
    }

    bool querySteamWebMetadata(uint64_t publishedFileId, std::string &out_file_url,
                               uint64_t &out_file_size)
    {
        std::string postUrl =
            "https://api.steampowered.com/ISteamRemoteStorage/GetPublishedFileDetails/v1/";
        std::string postData =
            "itemcount=1&publishedfileids%5B0%5D=" + std::to_string(publishedFileId);

        int max_retries = std::max(4, m_simHooks.simulate_rate_limit_count + 1);
        for (int retry = 0; retry < max_retries; ++retry) {
            // Simulated 429 rate limiting check
            if (m_simHooks.simulate_rate_limit_count > 0 &&
                m_simHooks.simulated_429_count <
                    static_cast<uint32_t>(m_simHooks.simulate_rate_limit_count)) {
                m_simHooks.simulated_429_count++;
                double backoff = calculateFullJitterBackoff(retry, 0.02, 0.2);
                std::this_thread::sleep_for(std::chrono::duration<double>(backoff));
                continue;
            }

            if (m_simHooks.dry_run_mode) {
                out_file_url = "mock://steam_workshop_" + std::to_string(publishedFileId);
                out_file_size =
                    m_simHooks.mock_total_size > 0 ? m_simHooks.mock_total_size : 1048576;
                return true;
            }

            if (!m_curl.handle && !m_curl.load()) {
                return false;
            }

            void *curl = m_curl.easy_init();
            if (!curl)
                return false;

            std::string responseString;
            auto writeCallback =
                +[](char *ptr, size_t size, size_t nmemb, void *userdata) -> size_t {
                auto *buf = reinterpret_cast<std::string *>(userdata);
                buf->append(ptr, size * nmemb);
                return size * nmemb;
            };

            m_curl.easy_setopt(curl, CurlBinding::OPT_URL, postUrl.c_str());
            m_curl.easy_setopt(curl, CurlBinding::OPT_POSTFIELDS, postData.c_str());
            m_curl.easy_setopt(curl, CurlBinding::OPT_WRITEFUNCTION, writeCallback);
            m_curl.easy_setopt(curl, CurlBinding::OPT_WRITEDATA, &responseString);
            m_curl.easy_setopt(curl, CurlBinding::OPT_TIMEOUT, 15L);
            m_curl.easy_setopt(curl, CurlBinding::OPT_NOSIGNAL, 1L);

            int res = m_curl.easy_perform(curl);
            long httpCode = 0;
            m_curl.easy_getinfo(curl, CurlBinding::INFO_RESPONSE_CODE, &httpCode);
            m_curl.easy_cleanup(curl);

            if (res == 0 && (httpCode == 429 || httpCode == 503)) {
                m_simHooks.simulated_429_count++;
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
                        int result = item.value("result").toInt(0);
                        if (result == 1) {
                            out_file_url = item.value("file_url").toString().toStdString();
                            out_file_size = item.value("file_size").toVariant().toULongLong();
                            return (!out_file_url.empty());
                        }
                    }
                }
            }

            double backoff = calculateFullJitterBackoff(retry);
            std::this_thread::sleep_for(std::chrono::duration<double>(backoff));
        }

        return false;
    }

    bool executeBackendB(const std::shared_ptr<Task> &task)
    {
        std::string downloadUrl;
        uint64_t estimatedSize = 0;

        if (task->request.kind == DownloadRequest::SourceKind::SteamWorkshop) {
            if (!querySteamWebMetadata(task->request.steam_published_file_id, downloadUrl,
                                       estimatedSize)) {
                return false;
            }
        } else {
            downloadUrl = task->request.openhub_download_url;
        }

        if (downloadUrl.empty()) {
            return false;
        }

        task->state.store(TaskState::Downloading, std::memory_order_relaxed);

        std::string filename;
        if (task->request.kind == DownloadRequest::SourceKind::SteamWorkshop) {
            filename = std::to_string(task->request.steam_published_file_id) + ".bin";
        } else {
            std::string id = task->request.openhub_package_id;
            std::replace(id.begin(), id.end(), ':', '_');
            std::replace(id.begin(), id.end(), '/', '_');
            filename = id + ".wwpkg";
        }

        std::string partPath = m_downloadDir + "/" + filename + ".part";
        std::string finalPath = m_downloadDir + "/" + filename;

        bool downloadSuccess = false;
        constexpr int MAX_RETRIES = 3;

        for (int attempt = 0;
             attempt < MAX_RETRIES && !task->cancelled.load(std::memory_order_relaxed); ++attempt) {
            uint64_t offset = 0;
            struct stat st{};
            if (::stat(partPath.c_str(), &st) == 0 && st.st_size > 0) {
                offset = static_cast<uint64_t>(st.st_size);
            }

            int fd = ::open(partPath.c_str(), O_RDWR | O_CREAT, 0644);
            if (fd < 0) {
                return false;
            }

            if (offset > 0) {
                ::lseek(fd, static_cast<off_t>(offset), SEEK_SET);
            }

            task->bytes_downloaded = offset;
            task->bytes_total = std::max(estimatedSize, offset);
            task->speed_estimator.reset(offset);
            task->notifyProgress();

            bool isMock = m_simHooks.dry_run_mode || (downloadUrl.rfind("mock://", 0) == 0) ||
                          (downloadUrl.rfind("test://", 0) == 0);

            bool chunkSuccess = false;
            if (isMock) {
                chunkSuccess = executeMockStream(task, fd, offset, estimatedSize);
            } else {
                chunkSuccess = executeCurlHttpStream(task, fd, downloadUrl, offset, estimatedSize);
            }

            ::close(fd);

            if (chunkSuccess) {
                downloadSuccess = true;
                break;
            }

            if (task->cancelled.load(std::memory_order_relaxed)) {
                break;
            }

            if (m_preemptActiveTask.load(std::memory_order_relaxed) &&
                task->request.priority <= TaskPriority::BackgroundUpdate) {
                break;
            }

            if (attempt + 1 < MAX_RETRIES) {
                double backoff = calculateFullJitterBackoff(attempt, 0.05, 1.0);
                std::this_thread::sleep_for(std::chrono::duration<double>(backoff));
            }
        }

        if (downloadSuccess && !task->cancelled.load(std::memory_order_relaxed)) {
            std::error_code ec;
            fs::rename(partPath, finalPath, ec);
            if (!ec) {
                task->installed_path = finalPath;
                return true;
            }
        }

        return downloadSuccess;
    }

    bool executeMockStream(const std::shared_ptr<Task> &task, int fd, uint64_t &offset,
                           uint64_t estimatedSize)
    {
        uint64_t totalSize = m_simHooks.mock_total_size > 0
                                 ? m_simHooks.mock_total_size
                                 : (estimatedSize > 0 ? estimatedSize : 1048576);
        task->bytes_total = totalSize;

        // Critical Truncation Guard Simulation
        if (offset > 0) {
            if (m_simHooks.simulate_http_200_on_resume) {
                // Server responded HTTP 200 OK instead of 206 Partial Content!
                ::ftruncate(fd, 0);
                ::lseek(fd, 0, SEEK_SET);
                offset = 0;
                task->bytes_downloaded = 0;
                m_simHooks.simulated_truncation_occurred = true;
            }
        }

        constexpr size_t CHUNK_SIZE = 65536;
        std::vector<uint8_t> chunk(CHUNK_SIZE);

        while (offset < totalSize && !task->cancelled.load(std::memory_order_relaxed) &&
               m_running.load(std::memory_order_relaxed)) {

            // Check preemption
            if (m_preemptActiveTask.load(std::memory_order_relaxed) &&
                task->request.priority <= TaskPriority::BackgroundUpdate) {
                return false;
            }

            // Simulated connection drop check
            if (m_simHooks.drop_at_percent > 0.0f && m_simHooks.simulated_drop_count == 0) {
                if (offset >= static_cast<uint64_t>(totalSize * m_simHooks.drop_at_percent)) {
                    m_simHooks.simulated_drop_count++;
                    return false; // Connection dropped!
                }
            }

            size_t bytesToWrite = std::min<uint64_t>(CHUNK_SIZE, totalSize - offset);

            // Populate deterministic content pattern
            if (!m_simHooks.mock_data.empty()) {
                for (size_t i = 0; i < bytesToWrite; ++i) {
                    chunk[i] = static_cast<uint8_t>(
                        m_simHooks.mock_data[(offset + i) % m_simHooks.mock_data.size()]);
                }
            } else {
                for (size_t i = 0; i < bytesToWrite; ++i) {
                    chunk[i] = static_cast<uint8_t>((offset + i) & 0xFF);
                }
            }

            // Apply Token Bucket pacing
            if (!m_tokenBucket.consume(bytesToWrite, task->cancelled)) {
                return false;
            }

            ssize_t written = ::write(fd, chunk.data(), bytesToWrite);
            if (written <= 0)
                return false;

            offset += written;
            task->bytes_downloaded = offset;
            task->progress_pct = static_cast<float>(offset * 100.0 / totalSize);
            task->speed_estimator.update(offset, totalSize, task->speed_bytes_per_sec,
                                         task->eta_seconds);
            task->notifyProgress();
        }

        return (offset >= totalSize);
    }

    struct CurlContext {
        Impl *self{nullptr};
        std::shared_ptr<Task> task;
        int fd{-1};
        void *curl{nullptr};
        uint64_t *pOffset{nullptr};
        uint64_t totalSize{0};
        bool headersParsed{false};
        bool dropTriggered{false};
    };

    bool executeCurlHttpStream(const std::shared_ptr<Task> &task, int fd, const std::string &url,
                               uint64_t &offset, uint64_t estimatedSize)
    {
        if (!m_curl.handle && !m_curl.load()) {
            return false;
        }

        void *curl = m_curl.easy_init();
        if (!curl)
            return false;

        CurlContext ctx;
        ctx.self = this;
        ctx.curl = curl;
        ctx.task = task;
        ctx.fd = fd;
        ctx.pOffset = &offset;
        ctx.totalSize = estimatedSize;

        auto writeCallback = +[](char *ptr, size_t size, size_t nmemb, void *userdata) -> size_t {
            auto *c = reinterpret_cast<CurlContext *>(userdata);
            size_t bytes = size * nmemb;
            if (bytes == 0)
                return 0;

            if (c->task->cancelled.load(std::memory_order_relaxed)) {
                return 0; // Abort transfer
            }

            if (c->self->m_preemptActiveTask.load(std::memory_order_relaxed) &&
                c->task->request.priority <= TaskPriority::BackgroundUpdate) {
                return 0; // Abort transfer for preemption
            }

            // Inspect HTTP status code upon receiving the first body chunk
            if (!c->headersParsed) {
                long httpCode = 0;
                c->self->m_curl.easy_getinfo(c->curl, CurlBinding::INFO_RESPONSE_CODE, &httpCode);
                c->headersParsed = true;

                // Critical Truncation Guard:
                // If resuming from OFFSET > 0 and server returns HTTP 200 OK instead of 206 Partial
                // Content, immediately truncate to 0 before writing chunks to prevent corrupt
                // concatenated archives!
                if (*(c->pOffset) > 0 && httpCode == 200) {
                    ::ftruncate(c->fd, 0);
                    ::lseek(c->fd, 0, SEEK_SET);
                    *(c->pOffset) = 0;
                    c->task->bytes_downloaded = 0;
                    c->self->m_simHooks.simulated_truncation_occurred = true;
                }
            }

            // Check simulation drop hook
            if (c->self->m_simHooks.drop_at_percent > 0.0f &&
                c->self->m_simHooks.simulated_drop_count == 0 && c->totalSize > 0) {
                if (*(c->pOffset) >=
                    static_cast<uint64_t>(c->totalSize * c->self->m_simHooks.drop_at_percent)) {
                    c->self->m_simHooks.simulated_drop_count++;
                    c->dropTriggered = true;
                    return 0; // Trigger transfer error
                }
            }

            // Bandwidth Governor Token Bucket Throttling
            if (!c->self->m_tokenBucket.consume(bytes, c->task->cancelled)) {
                return 0;
            }

            ssize_t written = ::write(c->fd, ptr, bytes);
            if (written > 0) {
                *(c->pOffset) += written;
                c->task->bytes_downloaded = *(c->pOffset);
                if (c->totalSize > 0) {
                    c->task->progress_pct =
                        static_cast<float>((*(c->pOffset) * 100.0) / c->totalSize);
                }
                c->task->speed_estimator.update(*(c->pOffset), c->totalSize,
                                                c->task->speed_bytes_per_sec, c->task->eta_seconds);
                c->task->notifyProgress();
                return static_cast<size_t>(written);
            }
            return 0;
        };

        m_curl.easy_setopt(curl, CurlBinding::OPT_URL, url.c_str());
        m_curl.easy_setopt(curl, CurlBinding::OPT_FOLLOWLOCATION, 1L);
        m_curl.easy_setopt(curl, CurlBinding::OPT_MAXREDIRS, 10L);
        m_curl.easy_setopt(curl, CurlBinding::OPT_WRITEFUNCTION, writeCallback);
        m_curl.easy_setopt(curl, CurlBinding::OPT_WRITEDATA, &ctx);
        m_curl.easy_setopt(curl, CurlBinding::OPT_NOSIGNAL, 1L);
        m_curl.easy_setopt(curl, CurlBinding::OPT_TIMEOUT, 300L);
        m_curl.easy_setopt(curl, CurlBinding::OPT_CONNECTTIMEOUT, 15L);

        // Byte-range resumption
        std::string rangeHeader;
        if (offset > 0) {
            rangeHeader = std::to_string(offset) + "-";
            m_curl.easy_setopt(curl, CurlBinding::OPT_RANGE, rangeHeader.c_str());
        }

        int res = m_curl.easy_perform(curl);
        m_curl.easy_cleanup(curl);

        return (res == 0 && !ctx.dropTriggered);
    }

    /* --------------------------------------------------------------------- */
    /* Backend C Execution (Headless steamcmd Subprocess Runner)             */
    /* --------------------------------------------------------------------- */

    std::string findSteamCmd()
    {
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
                if (::access(full.c_str(), X_OK) == 0)
                    return full;
                full = dir + "/steamcmd.sh";
                if (::access(full.c_str(), X_OK) == 0)
                    return full;
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

        int pipefd[2];
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
            // Child process
            ::close(pipefd[0]);
            ::dup2(pipefd[1], STDOUT_FILENO);
            ::dup2(pipefd[1], STDERR_FILENO);
            ::close(pipefd[1]);

            ::execlp(steamCmdPath.c_str(), steamCmdPath.c_str(), "+login", "anonymous",
                     "+workshop_download_item", "431960", idStr.c_str(), "validate", "+quit",
                     nullptr);
            ::_exit(127);
        }

        // Parent process
        ::close(pipefd[1]);

        task->state.store(TaskState::Downloading, std::memory_order_relaxed);
        task->speed_estimator.reset(0);

        std::string lineBuffer;
        char readBuf[1024];
        bool downloadSuccess = false;

        std::regex reProgressBytes(
            R"(Update state \(0x[0-9a-fA-F]+\) downloading, progress:\s*([0-9.]+)%\s*\(([0-9]+)\s*\/\s*([0-9]+)\))");
        std::regex reProgressPct(R"(progress:\s*([0-9.]+)%)");
        std::regex reSuccess(R"rx(Success\.\s*Downloaded item\s*([0-9]+)\s*to\s*"([^"]+)")rx");

        while (m_running.load(std::memory_order_relaxed) &&
               !task->cancelled.load(std::memory_order_relaxed)) {

            struct pollfd pfd{};
            pfd.fd = pipefd[0];
            pfd.events = POLLIN;

            int pollRet = ::poll(&pfd, 1, 100);
            if (pollRet > 0 && (pfd.revents & POLLIN)) {
                ssize_t bytesRead = ::read(pipefd[0], readBuf, sizeof(readBuf));
                if (bytesRead > 0) {
                    for (ssize_t i = 0; i < bytesRead; ++i) {
                        char c = readBuf[i];
                        if (c == '\n' || c == '\r') {
                            if (!lineBuffer.empty()) {
                                std::smatch match;
                                if (std::regex_search(lineBuffer, match, reProgressBytes)) {
                                    float pct = std::stof(match[1].str());
                                    uint64_t cur = std::stoull(match[2].str());
                                    uint64_t tot = std::stoull(match[3].str());
                                    task->bytes_downloaded = cur;
                                    task->bytes_total = tot;
                                    task->progress_pct = pct;
                                    task->speed_estimator.update(
                                        cur, tot, task->speed_bytes_per_sec, task->eta_seconds);
                                    task->notifyProgress();
                                } else if (std::regex_search(lineBuffer, match, reProgressPct)) {
                                    float pct = std::stof(match[1].str());
                                    task->progress_pct = pct;
                                    task->notifyProgress();
                                } else if (std::regex_search(lineBuffer, match, reSuccess)) {
                                    task->installed_path = match[2].str();
                                    downloadSuccess = true;
                                }
                                lineBuffer.clear();
                            }
                        } else {
                            lineBuffer.push_back(c);
                        }
                    }
                } else if (bytesRead == 0) {
                    break; // EOF
                }
            }

            int status = 0;
            pid_t waited = ::waitpid(pid, &status, WNOHANG);
            if (waited > 0) {
                if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                    downloadSuccess = true;
                }
                break;
            }
        }

        ::close(pipefd[0]);

        if (task->cancelled.load(std::memory_order_relaxed)) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
            return false;
        }

        int status = 0;
        ::waitpid(pid, &status, 0);
        return (downloadSuccess || (WIFEXITED(status) && WEXITSTATUS(status) == 0));
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

void DownloadManager::pause() { m_impl->m_tokenBucket.pause(); }

void DownloadManager::resume() { m_impl->m_tokenBucket.resume(); }

void DownloadManager::setBandwidthLimit(uint64_t bytes_per_sec)
{
    m_impl->m_tokenBucket.setLimit(bytes_per_sec);
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
    m_impl->m_simHooks = hooks;
}

DownloadManager::SimulationHooks DownloadManager::simulationHooks() const
{
    return m_impl->m_simHooks;
}

void DownloadManager::resetSimulationStats()
{
    m_impl->m_simHooks.simulated_drop_count = 0;
    m_impl->m_simHooks.simulated_429_count = 0;
    m_impl->m_simHooks.simulated_truncation_occurred = false;
}

void DownloadManager::setDownloadDirectory(const std::string &dir)
{
    m_impl->m_downloadDir = dir;
    std::error_code ec;
    fs::create_directories(dir, ec);
}

std::string DownloadManager::downloadDirectory() const { return m_impl->m_downloadDir; }

size_t DownloadManager::queuedTasks() const noexcept { return m_impl->queuedTasks(); }

bool DownloadManager::isPaused() const noexcept { return m_impl->m_tokenBucket.isPaused(); }

void DownloadManager::waitUntilIdle() { m_impl->waitUntilIdle(); }

} // namespace wwal::studio
