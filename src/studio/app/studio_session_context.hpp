// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_APP_STUDIO_SESSION_CONTEXT_HPP
#define WAYWAL_STUDIO_APP_STUDIO_SESSION_CONTEXT_HPP

#include "studio/core/catalog_index_manager.hpp"
#include "studio/core/command_bus.hpp"
#include "studio/core/ipc_client.hpp"
#include "studio/core/preset_store.hpp"
#include "studio/core/recursive_fs_watcher.hpp"
#include "studio/core/services.hpp"
#include "studio/queue/download_manager.hpp"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "waywal/ipc_proto.h"
#pragma GCC diagnostic pop
#include "waywal/we_vdf.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLatin1Char>
#include <QLocale>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace waywal::studio {

/**
 * @brief Complete wallpaper application payload dispatched to the CLI/daemon.
 */
struct WallpaperApplyRequest {
    QString namespaceStr;
    QString path;
    QString targetOutputsCsv;
    uint32_t transitionType{WAYWAL_TRANSITION_FADE};
    uint32_t transitionDurationMs{1000};
    uint32_t transitionFps{144};
    float angleDeg{0.0f};
    float waveFreq{10.0f};
    float waveAmp{0.1f};
    float centerX{0.5f};
    float centerY{0.5f};
    bezier_curve_t bezier{{0.25f}, {0.1f}, {0.25f}, {1.0f}};
    bool hasCustomBezier{true};
    uint32_t scalingMode{WAYWAL_SCALING_FILL};
    float cropGravityX{0.5f};
    float cropGravityY{0.5f};
    bool hasCropGravity{false};
    bool hasThemeMode{false};
    bool themeLightMode{false};
    bool hasSourceColorIndex{false};
    uint8_t sourceColorIndex{0};
};

/**
 * @brief FIFO state entry pairing each scene package export IPC request with its staged output.
 */
struct PendingSceneExport {
    QString stagePath;
    QString destPath;
    QString targetOutput;
    bool isPkgFile{false};
};

/**
 * @brief Non-Qt-parented owner for Studio session services, async IO workers, and state.
 *
 * Isolating core services from MainWindow's QObject child tree prevents ~QMainWindow() from
 * tearing down CommandBus, IpcClient, or PresetStore while child dock widgets still invoke them
 * during widget teardown (AUD-053), and keeps heavy session plumbing out of MainWindow (AUD-420).
 */
class StudioSessionContext {
  public:
    static constexpr qint64 kMaxBoundedTextFileBytes = 2LL * 1024LL * 1024LL;

    StudioSessionContext() = default;
    ~StudioSessionContext() { shutdownAsyncWorkers(); }

    StudioSessionContext(const StudioSessionContext &) = delete;
    StudioSessionContext &operator=(const StudioSessionContext &) = delete;

    void initializeCoreServices(const QString &namespaceStr, const QString &socketPath,
                                bool interactiveMode)
    {
        // Construct Qt services with parent = nullptr so QObject parent destruction never races
        // against child panels flushing pending commands or readbacks in ~MainWindow().
        m_commandBus = std::make_unique<CommandBus>(1000, nullptr);
        m_presetStore = std::make_unique<PresetStore>(QString(), nullptr);
        m_ipcClient = std::make_unique<IpcClient>(namespaceStr, socketPath, nullptr, false);
        Services::instance().setIpcClient(m_ipcClient.get());
        if (interactiveMode) {
            m_ipcClient->start();
        }

        m_catalogIndexMgr = std::make_shared<CatalogIndexManager>();
        m_downloadMgr = std::make_shared<DownloadManager>();
        if (interactiveMode) {
            (void)m_downloadMgr->initialize();
        }
    }

    void startWatchersAndReconcile(const std::function<void()> &onCatalogUpdated)
    {
        if (!onCatalogUpdated) {
            return;
        }

        // Watch both active XDG hub locations ($XDG_DATA_HOME/waywal/hub and wwal/hub) alongside
        // discovered Steam Workshop libraries so non-default XDG roots are never missed (AUD-182).
        m_fsWatcher = std::make_unique<RecursiveFsWatcher>(
            [onCatalogUpdated](const std::vector<FsWatchEvent> &events) {
                if (!events.empty()) {
                    onCatalogUpdated();
                }
            });

        std::vector<std::pair<std::filesystem::path, uint32_t>> scanDirs;
        waywal_steam_library_t libs[32];
        const size_t libCount = waywal_steam_discover_libraries(libs, 32);
        scanDirs.reserve(libCount + 2);

        for (size_t i = 0; i < libCount; ++i) {
            std::filesystem::path wsDir =
                std::filesystem::path(libs[i].path) / "steamapps" / "workshop" / "content" / "431960";
            std::error_code fsEc;
            if (std::filesystem::exists(wsDir, fsEc) && !fsEc) {
                m_fsWatcher->addWatchRoot(wsDir);
                scanDirs.emplace_back(std::move(wsDir), 0u);
            }
        }

        for (const auto &hubPath : resolveXdgHubDirectories()) {
            std::error_code dirEc;
            std::filesystem::create_directories(hubPath, dirEc);
            if (std::filesystem::exists(hubPath, dirEc) && !dirEc) {
                m_fsWatcher->addWatchRoot(hubPath);
                scanDirs.emplace_back(hubPath, 2u);
            }
        }
        m_fsWatcher->start();

        if (m_catalogIndexMgr && !scanDirs.empty()) {
            auto indexMgr = m_catalogIndexMgr;
            m_reconcileThread =
                std::jthread([indexMgr = std::move(indexMgr), scanDirs = std::move(scanDirs),
                              onCatalogUpdated](std::stop_token st) {
                    // Check stop_token between library directories rather than calling monolithic
                    // reconcileAllLibraries(), bounding teardown join latency (AUD-183).
                    for (const auto &[dir, backend] : scanDirs) {
                        if (st.stop_requested()) {
                            return;
                        }
                        (void)indexMgr->reconcileDirectory(dir, backend);
                    }
                    if (!st.stop_requested()) {
                        onCatalogUpdated();
                    }
                });
        }
    }

    void shutdownAsyncWorkers() noexcept
    {
        if (m_fsWatcher) {
            m_fsWatcher->stop();
            m_fsWatcher.reset();
        }
        if (m_reconcileThread.joinable()) {
            m_reconcileThread.request_stop();
            m_reconcileThread.join();
        }
        if (m_downloadMgr) {
            m_downloadMgr->shutdown();
        }
        if (Services::instance().ipcClient() == m_ipcClient.get()) {
            Services::instance().setIpcClient(nullptr);
        }
        if (m_ipcClient) {
            m_ipcClient->stop();
        }
        m_pendingExports.clear();
    }

    [[nodiscard]] CommandBus *commandBus() const noexcept { return m_commandBus.get(); }
    [[nodiscard]] IpcClient *ipcClient() const noexcept { return m_ipcClient.get(); }
    [[nodiscard]] PresetStore *presetStore() const noexcept { return m_presetStore.get(); }
    [[nodiscard]] const std::shared_ptr<CatalogIndexManager> &catalogIndexManager() const noexcept
    {
        return m_catalogIndexMgr;
    }
    [[nodiscard]] const std::shared_ptr<DownloadManager> &downloadManager() const noexcept
    {
        return m_downloadMgr;
    }
    [[nodiscard]] RecursiveFsWatcher *fsWatcher() const noexcept { return m_fsWatcher.get(); }

    void enqueuePendingExport(PendingSceneExport entry)
    {
        m_pendingExports.push_back(std::move(entry));
    }

    bool popPendingExport(PendingSceneExport &outEntry)
    {
        if (m_pendingExports.empty()) {
            return false;
        }
        outEntry = std::move(m_pendingExports.front());
        m_pendingExports.pop_front();
        return true;
    }

    void discardLastPendingExport()
    {
        if (!m_pendingExports.empty()) {
            m_pendingExports.pop_back();
        }
    }

    void setLastAppliedRequest(WallpaperApplyRequest req) { m_lastAppliedReq = std::move(req); }
    [[nodiscard]] const WallpaperApplyRequest &lastAppliedRequest() const noexcept
    {
        return m_lastAppliedReq;
    }

    [[nodiscard]] float sampleAudioTelemetryDeltaSec(bool hasActiveAudio)
    {
        if (!hasActiveAudio) {
            m_telemetryTimer.invalidate();
            return 1.0f / 60.0f;
        }
        if (!m_telemetryTimer.isValid()) {
            m_telemetryTimer.start();
            return 1.0f / 60.0f;
        }
        const qint64 elapsedNs = m_telemetryTimer.restart();
        const float dt = static_cast<float>(elapsedNs) * 1.0e-9f;
        return std::clamp(dt, 1.0e-4f, 0.25f);
    }

    [[nodiscard]] static std::vector<std::filesystem::path> resolveXdgHubDirectories()
    {
        std::filesystem::path dataRoot;
        const QByteArray xdgData = qgetenv("XDG_DATA_HOME");
        if (!xdgData.isEmpty()) {
            dataRoot = std::filesystem::path(xdgData.toStdString());
        } else {
            const QString genericData =
                QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
            if (!genericData.isEmpty()) {
                dataRoot = std::filesystem::path(genericData.toStdString());
            } else {
                dataRoot =
                    std::filesystem::path(QDir::homePath().toStdString()) / ".local" / "share";
            }
        }
        return {dataRoot / "waywal" / "hub", dataRoot / "wwal" / "hub"};
    }

    [[nodiscard]] static bool readBoundedUtf8File(const QString &filePath, QString &outText,
                                                  QString &outError,
                                                  qint64 maxBytes = kMaxBoundedTextFileBytes)
    {
        outText.clear();
        outError.clear();

        const QFileInfo fi(filePath);
        if (!fi.exists() || !fi.isFile()) {
            outError = QStringLiteral("File does not exist: %1").arg(filePath);
            return false;
        }
        if (fi.size() > maxBytes) {
            outError = QStringLiteral("File exceeds %1 MiB limit (%2 bytes): %3")
                           .arg(maxBytes / (1024 * 1024))
                           .arg(fi.size())
                           .arg(fi.fileName());
            return false;
        }

        QFile f(filePath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            outError = QStringLiteral("Failed to open file: %1").arg(fi.fileName());
            return false;
        }

        const QByteArray data = f.read(maxBytes + 1);
        if (data.size() > maxBytes) {
            outError = QStringLiteral("File exceeded %1 MiB limit while reading: %2")
                           .arg(maxBytes / (1024 * 1024))
                           .arg(fi.fileName());
            return false;
        }
        outText = QString::fromUtf8(data);
        return true;
    }

    [[nodiscard]] static QString scalingModeCliName(uint32_t mode)
    {
        switch (mode) {
        case WAYWAL_SCALING_FILL:
            return QStringLiteral("fill");
        case WAYWAL_SCALING_FIT:
            return QStringLiteral("fit");
        case WAYWAL_SCALING_STRETCH:
            return QStringLiteral("stretch");
        case WAYWAL_SCALING_CENTER:
            return QStringLiteral("center");
        case WAYWAL_SCALING_TILE:
            return QStringLiteral("tile");
        case WAYWAL_SCALING_SPAN:
            return QStringLiteral("span");
        default:
            return QStringLiteral("fill");
        }
    }

    [[nodiscard]] static QString transitionTypeCliName(uint32_t type)
    {
        static constexpr const char *kNames[] = {
            "none",      "simple",     "fade",   "wipe",   "grow",      "outer",
            "wave",      "noise",      "crosszoom", "slide", "glitch",   "burn",
            "ripple",    "pixelate",   "doom",   "swirl",  "cube",      "luma",
            "light_leak", "page_curl", "custom", "random"};
        if (type < std::size(kNames)) {
            return QString::fromLatin1(kNames[type]);
        }
        return QStringLiteral("fade");
    }

    [[nodiscard]] static waywal_img_metadata_t
    buildImgMetadata(const WallpaperApplyRequest &req) noexcept
    {
        waywal_img_metadata_t meta{};
        meta.transition_type = req.transitionType;
        meta.transition_duration_ms = std::clamp(req.transitionDurationMs, 0u, 3600000u);
        meta.transition_fps = std::clamp(req.transitionFps, 15u, 360u);
        meta.transition_angle_rad = req.angleDeg * 0.017453292519943295f;
        meta.transition_wave_freq = req.waveFreq;
        meta.transition_wave_amp = req.waveAmp;
        meta.transition_center_x = std::clamp(req.centerX, 0.0f, 1.0f);
        meta.transition_center_y = std::clamp(req.centerY, 0.0f, 1.0f);
        meta.bezier = req.bezier;
        meta.has_custom_bezier = req.hasCustomBezier;
        meta.scaling_mode = req.scalingMode <= WAYWAL_SCALING_SPAN
                                ? req.scalingMode
                                : static_cast<uint32_t>(WAYWAL_SCALING_FILL);
        meta.crop_gravity_x = std::clamp(req.cropGravityX, 0.0f, 1.0f);
        meta.crop_gravity_y = std::clamp(req.cropGravityY, 0.0f, 1.0f);
        meta.has_crop_gravity = req.hasCropGravity ? 1u : 0u;
        meta.has_theme_mode = req.hasThemeMode ? 1u : 0u;
        meta.theme_light_mode = req.themeLightMode;
        meta.has_source_color_index = req.hasSourceColorIndex ? 1u : 0u;
        meta.source_color_index = req.sourceColorIndex;

        const QByteArray pathUtf8 = req.path.toUtf8();
        const size_t copyLen = std::min<size_t>(static_cast<size_t>(pathUtf8.size()),
                                                sizeof(meta.filepath) - 1u);
        if (copyLen > 0) {
            std::copy_n(pathUtf8.constData(), copyLen, meta.filepath);
        }
        meta.filepath[copyLen] = '\0';
        return meta;
    }

    [[nodiscard]] static WallpaperApplyRequest
    requestFromMetadata(const QString &namespaceStr, const QString &targetOutputsCsv,
                        const waywal_img_metadata_t &meta, const QString &path)
    {
        WallpaperApplyRequest req;
        req.namespaceStr = namespaceStr;
        req.path = path.isEmpty() ? QString::fromUtf8(meta.filepath) : path;
        req.targetOutputsCsv = targetOutputsCsv;
        req.transitionType = meta.transition_type;
        req.transitionDurationMs = meta.transition_duration_ms;
        req.transitionFps = meta.transition_fps > 0u ? meta.transition_fps : 144u;
        req.angleDeg = meta.transition_angle_rad / 0.017453292519943295f;
        req.waveFreq = meta.transition_wave_freq;
        req.waveAmp = meta.transition_wave_amp;
        req.centerX = meta.transition_center_x;
        req.centerY = meta.transition_center_y;
        req.bezier = meta.bezier;
        req.hasCustomBezier = meta.has_custom_bezier;
        req.scalingMode = meta.scaling_mode;
        req.cropGravityX = meta.crop_gravity_x;
        req.cropGravityY = meta.crop_gravity_y;
        req.hasCropGravity = (meta.has_crop_gravity != 0u);
        req.hasThemeMode = (meta.has_theme_mode != 0u);
        req.themeLightMode = meta.theme_light_mode;
        req.hasSourceColorIndex = (meta.has_source_color_index != 0u);
        req.sourceColorIndex = meta.source_color_index;
        return req;
    }

    [[nodiscard]] static QStringList buildWallpaperApplyArgs(const WallpaperApplyRequest &req)
    {
        QStringList args;
        if (!req.namespaceStr.isEmpty() && req.namespaceStr != QStringLiteral("default")) {
            args << QStringLiteral("--namespace") << req.namespaceStr;
        }

        const QFileInfo fi(req.path);
        const QString fileName = fi.fileName().toLower();
        const QString lowerPath = req.path.toLower();
        QString effectiveTarget = req.path;

        if (fi.isDir() || fileName == QStringLiteral("scene.json") ||
            fileName == QStringLiteral("project.json")) {
            args << QStringLiteral("workshop");
            if (!fi.isDir()) {
                effectiveTarget = fi.absolutePath();
            }
        } else if (lowerPath.endsWith(QStringLiteral(".frag")) ||
                   lowerPath.endsWith(QStringLiteral(".glsl")) ||
                   lowerPath.endsWith(QStringLiteral(".shader")) ||
                   lowerPath.endsWith(QStringLiteral(".comp")) ||
                   lowerPath.endsWith(QStringLiteral(".hlsl"))) {
            args << QStringLiteral("shader");
        } else if (lowerPath.endsWith(QStringLiteral(".mp4")) ||
                   lowerPath.endsWith(QStringLiteral(".mkv")) ||
                   lowerPath.endsWith(QStringLiteral(".webm")) ||
                   lowerPath.endsWith(QStringLiteral(".mov")) ||
                   lowerPath.endsWith(QStringLiteral(".avi")) ||
                   lowerPath.endsWith(QStringLiteral(".m4v"))) {
            args << QStringLiteral("video");
        } else {
            args << QStringLiteral("img");
        }

        args << effectiveTarget;

        const QLocale cLocale = QLocale::c();
        args << QStringLiteral("--transition-type") << transitionTypeCliName(req.transitionType);
        args << QStringLiteral("--transition-duration")
             << cLocale.toString(static_cast<double>(req.transitionDurationMs) / 1000.0, 'f', 3);
        args << QStringLiteral("--transition-fps")
             << QString::number(std::clamp(req.transitionFps, 15u, 360u));
        args << QStringLiteral("--transition-angle")
             << cLocale.toString(static_cast<double>(req.angleDeg), 'f', 2);
        args << QStringLiteral("--transition-wave")
             << (cLocale.toString(static_cast<double>(req.waveFreq), 'f', 2) + QLatin1Char(',') +
                 cLocale.toString(static_cast<double>(req.waveAmp), 'f', 3));
        args << QStringLiteral("--transition-pos")
             << (cLocale.toString(static_cast<double>(std::clamp(req.centerX, 0.0f, 1.0f)), 'f',
                                  3) +
                 QLatin1Char(',') +
                 cLocale.toString(static_cast<double>(std::clamp(req.centerY, 0.0f, 1.0f)), 'f',
                                  3));

        if (req.hasCustomBezier) {
            const float x1 = req.bezier.p1x;
            const float y1 = req.bezier.p1y;
            const float x2 = req.bezier.p2x;
            const float y2 = req.bezier.p2y;
            args << QStringLiteral("--transition-bezier")
                 << (cLocale.toString(x1, 'f', 3) + QLatin1Char(',') + cLocale.toString(y1, 'f', 3) +
                     QLatin1Char(',') + cLocale.toString(x2, 'f', 3) + QLatin1Char(',') +
                     cLocale.toString(y2, 'f', 3));
        }

        args << QStringLiteral("--scaling-mode") << scalingModeCliName(req.scalingMode);
        if (req.hasCropGravity) {
            args << QStringLiteral("--crop-gravity")
                 << (cLocale.toString(static_cast<double>(std::clamp(req.cropGravityX, 0.0f, 1.0f)),
                                      'f', 3) +
                     QLatin1Char(',') +
                     cLocale.toString(static_cast<double>(std::clamp(req.cropGravityY, 0.0f, 1.0f)),
                                      'f', 3));
        }

        const QString trimmedOut = req.targetOutputsCsv.trimmed();
        if (!trimmedOut.isEmpty() && trimmedOut != QStringLiteral("*")) {
            args << QStringLiteral("--outputs") << trimmedOut;
        }

        if (req.hasThemeMode) {
            args << QStringLiteral("--theme-mode")
                 << (req.themeLightMode ? QStringLiteral("light") : QStringLiteral("dark"));
        }
        if (req.hasSourceColorIndex) {
            args << QStringLiteral("--source-color-index")
                 << QString::number(std::min<uint8_t>(req.sourceColorIndex, 3u));
        }

        return args;
    }

    static bool dispatchWwalCli(const QStringList &args)
    {
        QString wwalBin = QStandardPaths::findExecutable(QStringLiteral("wwal"));
        if (wwalBin.isEmpty()) {
            const QString localBin = QDir::homePath() + QStringLiteral("/.local/bin/wwal");
            if (QFileInfo::exists(localBin)) {
                wwalBin = localBin;
            }
        }
        if (wwalBin.isEmpty()) {
            return false;
        }
        return QProcess::startDetached(wwalBin, args);
    }

  private:
    std::unique_ptr<CommandBus> m_commandBus;
    std::unique_ptr<PresetStore> m_presetStore;
    std::unique_ptr<IpcClient> m_ipcClient;
    std::shared_ptr<CatalogIndexManager> m_catalogIndexMgr;
    std::shared_ptr<DownloadManager> m_downloadMgr;
    std::unique_ptr<RecursiveFsWatcher> m_fsWatcher;
    std::jthread m_reconcileThread;
    std::deque<PendingSceneExport> m_pendingExports;
    WallpaperApplyRequest m_lastAppliedReq;
    QElapsedTimer m_telemetryTimer;
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_APP_STUDIO_SESSION_CONTEXT_HPP
