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
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
    QString backdrop;
    QString layerName;
    QString batteryPolicy;
    QString depthMapPath;
    QString customShaderGlsl;
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
    uint32_t filterMode{WAYWAL_FILTER_LANCZOS3};
    float cropGravityX{0.5f};
    float cropGravityY{0.5f};
    bool hasCropGravity{false};
    float depthStrength{1.0f};
    uint32_t syncMode{0};
    uint32_t staggerDelayMs{0};
    bool tenBit{false};
    bool invertY{false};
    bool posYInverted{false};
    bool noCache{false};
    bool extractTheme{false};
    bool noTheme{false};
    bool hasThemeMode{false};
    bool themeLightMode{false};
    bool isSolidColor{false};
    uint8_t color[4]{0, 0, 0, 255};
    bool hasSeedColor{false};
    uint32_t seedColor{0};
    bool hasSourceColorIndex{false};
    uint8_t sourceColorIndex{0};
    bool hasAutoDayNight{false};
    bool autoDayNight{false};
    bool hasSolarCoords{false};
    double solarLatitude{0.0};
    double solarLongitude{0.0};
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
        m_hasIpcOverride = false;
        m_ipcOverride = nullptr;
        if (m_ipcClient) {
            m_ipcClient->stop();
        }
        m_pendingExports.clear();
    }

    [[nodiscard]] CommandBus *commandBus() const noexcept { return m_commandBus.get(); }
    [[nodiscard]] IpcClient *ipcClient() const noexcept { return m_ipcClient.get(); }
    [[nodiscard]] IIpcClientService *activeIpcService() const noexcept
    {
        return m_hasIpcOverride ? m_ipcOverride : Services::instance().ipcClient();
    }
    void setIpcServiceOverride(IIpcClientService *svc) noexcept
    {
        m_ipcOverride = svc;
        m_hasIpcOverride = true;
    }
    void clearIpcServiceOverride() noexcept
    {
        m_ipcOverride = nullptr;
        m_hasIpcOverride = false;
    }
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

    enum class MediaDispatchKind {
        Image,
        Video,
        Shader,
        Workshop,
    };

    [[nodiscard]] static QString batteryPolicyCliName(uint8_t policy)
    {
        switch (policy) {
        case 1:
            return QStringLiteral("reduce-fps");
        case 2:
            return QStringLiteral("pause-video");
        default:
            return QStringLiteral("ignore");
        }
    }

    [[nodiscard]] static QStringList parseTargetOutputsCsv(const QString &targetOutputsCsv)
    {
        QStringList outputs;
        const QStringList parts = targetOutputsCsv.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &raw : parts) {
            const QString trimmed = raw.trimmed();
            if (trimmed.isEmpty()) {
                continue;
            }
            if (trimmed == QStringLiteral("*") ||
                trimmed.compare(QStringLiteral("All Displays"), Qt::CaseInsensitive) == 0) {
                return {};
            }
            outputs.push_back(trimmed);
        }
        return outputs;
    }

    [[nodiscard]] static MediaDispatchKind classifyMediaPath(const QString &path)
    {
        const QFileInfo fi(path);
        const QString fileName = fi.fileName().toLower();
        const QString lowerPath = path.toLower();
        if (fi.isDir() || fileName == QStringLiteral("scene.json") ||
            fileName == QStringLiteral("project.json")) {
            return MediaDispatchKind::Workshop;
        }
        if (lowerPath.endsWith(QStringLiteral(".frag")) ||
            lowerPath.endsWith(QStringLiteral(".glsl")) ||
            lowerPath.endsWith(QStringLiteral(".shader")) ||
            lowerPath.endsWith(QStringLiteral(".comp")) ||
            lowerPath.endsWith(QStringLiteral(".hlsl"))) {
            return MediaDispatchKind::Shader;
        }
        if (lowerPath.endsWith(QStringLiteral(".mp4")) ||
            lowerPath.endsWith(QStringLiteral(".mkv")) ||
            lowerPath.endsWith(QStringLiteral(".webm")) ||
            lowerPath.endsWith(QStringLiteral(".mov")) ||
            lowerPath.endsWith(QStringLiteral(".avi")) ||
            lowerPath.endsWith(QStringLiteral(".m4v"))) {
            return MediaDispatchKind::Video;
        }
        return MediaDispatchKind::Image;
    }

    [[nodiscard]] static bool requiresCliFallbackMediaPath(const QString &path)
    {
        // IpcClient::setWallpaper encodes images via WAYWAL_REQ_SET_IMAGE; video/shader/workshop paths require CLI.
        return classifyMediaPath(path) != MediaDispatchKind::Image;
    }

    [[nodiscard]] static WallpaperApplyOptions
    toWallpaperApplyOptions(const WallpaperApplyRequest &req)
    {
        WallpaperApplyOptions opts;
        opts.filepath = req.path;
        opts.targetOutputs = parseTargetOutputsCsv(req.targetOutputsCsv);
        opts.customShaderGlsl = req.customShaderGlsl;
        opts.depthMapPath = req.depthMapPath;
        opts.backdrop = req.backdrop;
        opts.layerName = req.layerName;
        opts.batteryPolicy = req.batteryPolicy;
        opts.scaling_mode = req.scalingMode <= WAYWAL_SCALING_SPAN
                                ? req.scalingMode
                                : static_cast<uint32_t>(WAYWAL_SCALING_FILL);
        opts.filter_mode = req.filterMode;
        opts.transition_type = req.transitionType;
        opts.transition_duration_ms = std::clamp(req.transitionDurationMs, 0u, 3600000u);
        opts.transition_fps = std::clamp(req.transitionFps, 15u, 360u);
        opts.transition_angle_rad = req.angleDeg * 0.017453292519943295f;
        opts.transition_wave_freq = req.waveFreq;
        opts.transition_wave_amp = req.waveAmp;
        opts.transition_center_x = std::clamp(req.centerX, 0.0f, 1.0f);
        opts.transition_center_y = std::clamp(req.centerY, 0.0f, 1.0f);
        opts.bezier = req.bezier;
        opts.has_custom_bezier = req.hasCustomBezier;
        opts.crop_gravity_x = std::clamp(req.cropGravityX, 0.0f, 1.0f);
        opts.crop_gravity_y = std::clamp(req.cropGravityY, 0.0f, 1.0f);
        opts.has_crop_gravity = req.hasCropGravity;
        opts.depth_strength = (std::isfinite(req.depthStrength) && req.depthStrength > 0.0f)
                                  ? req.depthStrength
                                  : 1.0f;
        opts.sync_mode = req.syncMode;
        opts.stagger_delay_ms = req.staggerDelayMs;
        opts.ten_bit = req.tenBit;
        opts.invert_y = req.invertY;
        opts.pos_y_inverted = req.posYInverted;
        opts.no_cache = req.noCache;
        opts.extract_theme = req.extractTheme;
        opts.no_theme = req.noTheme;
        opts.has_theme_mode = req.hasThemeMode;
        opts.theme_light_mode = req.themeLightMode;
        opts.is_solid_color = req.isSolidColor;
        std::memcpy(opts.color, req.color, sizeof(opts.color));
        opts.has_seed_color = req.hasSeedColor;
        opts.seed_color = req.seedColor;
        opts.has_source_color_index = req.hasSourceColorIndex;
        opts.source_color_index = req.sourceColorIndex;
        opts.has_auto_day_night = req.hasAutoDayNight;
        opts.auto_day_night = req.autoDayNight;
        opts.has_solar_coords = req.hasSolarCoords;
        opts.solar_latitude = std::isfinite(req.solarLatitude) ? req.solarLatitude : 0.0;
        opts.solar_longitude = std::isfinite(req.solarLongitude) ? req.solarLongitude : 0.0;
        return opts;
    }

    [[nodiscard]] static waywal_img_metadata_t
    buildImgMetadata(const WallpaperApplyRequest &req)
    {
        return toWallpaperApplyOptions(req).toMetadata();
    }

    [[nodiscard]] static WallpaperApplyRequest
    requestFromMetadata(const QString &namespaceStr, const QString &targetOutputsCsv,
                        const waywal_img_metadata_t &meta, const QString &path)
    {
        WallpaperApplyRequest req;
        req.namespaceStr = namespaceStr;
        req.path = path.isEmpty() ? QString::fromUtf8(meta.filepath) : path;
        req.targetOutputsCsv = targetOutputsCsv;
        req.backdrop = QString::fromUtf8(meta.backdrop);
        req.layerName = QString::fromUtf8(meta.layer_name);
        req.batteryPolicy = QString::fromUtf8(meta.battery_policy);
        req.depthMapPath = QString::fromUtf8(meta.depth_map_path);
        req.transitionType = meta.transition_type;
        req.transitionDurationMs = meta.transition_duration_ms;
        req.transitionFps = meta.transition_fps > 0u ? meta.transition_fps : 144u;
        req.angleDeg = meta.transition_angle_rad / 0.017453292519943295f;
        req.waveFreq = meta.transition_wave_freq;
        req.waveAmp = meta.transition_wave_amp;
        req.centerX = meta.transition_center_x;
        req.centerY = meta.transition_center_y;
        // waywal_img_metadata_t is #pragma pack(1); copy bezier bytes to avoid misaligned ref binding.
        std::memcpy(&req.bezier,
                    reinterpret_cast<const char *>(&meta) +
                        offsetof(waywal_img_metadata_t, bezier),
                    sizeof(req.bezier));
        req.hasCustomBezier = meta.has_custom_bezier;
        req.scalingMode = meta.scaling_mode;
        req.filterMode = meta.filter_mode;
        req.cropGravityX = meta.crop_gravity_x;
        req.cropGravityY = meta.crop_gravity_y;
        req.hasCropGravity = (meta.has_crop_gravity != 0u);
        req.depthStrength = meta.depth_strength;
        req.syncMode = meta.sync_mode;
        req.staggerDelayMs = meta.stagger_delay_ms;
        req.tenBit = meta.ten_bit || (meta.pixel_format == WAYWAL_FORMAT_XRGB2101010);
        req.invertY = (meta.invert_y != 0u);
        req.posYInverted = (meta.pos_y_inverted != 0u);
        req.noCache = (meta.no_cache != 0u);
        req.extractTheme = meta.extract_theme;
        req.noTheme = meta.no_theme;
        req.hasThemeMode = (meta.has_theme_mode != 0u);
        req.themeLightMode = meta.theme_light_mode;
        req.isSolidColor = (meta.is_solid_color != 0u);
        std::memcpy(req.color, meta.color, sizeof(req.color));
        req.hasSeedColor = (meta.has_seed_color != 0u);
        req.seedColor = meta.seed_color;
        req.hasSourceColorIndex = (meta.has_source_color_index != 0u);
        req.sourceColorIndex = meta.source_color_index;
        req.hasAutoDayNight = (meta.has_auto_day_night != 0u);
        req.autoDayNight = (meta.auto_day_night != 0u);
        req.hasSolarCoords = (meta.has_solar_coords != 0u);
        req.solarLatitude = meta.solar_latitude;
        req.solarLongitude = meta.solar_longitude;
        return req;
    }

    [[nodiscard]] static QStringList buildWallpaperApplyArgs(const WallpaperApplyRequest &req)
    {
        QStringList args;
        if (!req.namespaceStr.isEmpty() && req.namespaceStr != QStringLiteral("default")) {
            args << QStringLiteral("--namespace") << req.namespaceStr;
        }

        const QFileInfo fi(req.path);
        QString effectiveTarget = req.path;

        switch (classifyMediaPath(req.path)) {
        case MediaDispatchKind::Workshop:
            args << QStringLiteral("workshop");
            if (!fi.isDir()) {
                effectiveTarget = fi.absolutePath();
            }
            break;
        case MediaDispatchKind::Shader:
            args << QStringLiteral("shader");
            break;
        case MediaDispatchKind::Video:
            args << QStringLiteral("video");
            break;
        case MediaDispatchKind::Image:
            args << QStringLiteral("img");
            break;
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
    IIpcClientService *m_ipcOverride{nullptr};
    bool m_hasIpcOverride{false};
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
