// SPDX-License-Identifier: GPL-3.0-or-later
#include "monitor_topology_canvas.hpp"

#include "studio/core/services.hpp"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "waywal/image_loader.h"
#include "waywal/ipc_proto.h"
#pragma GCC diagnostic pop

#include <QCoreApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <unistd.h>

namespace waywal::studio {

namespace {

constexpr int kPreviewMaxDecodeW = 1920;
constexpr int kPreviewMaxDecodeH = 1080;
constexpr qreal kMinTilePreviewPx = 12.0;

[[nodiscard]] qreal resolveRefreshRateHz(const QJsonObject &obj)
{
    auto normalizeHzOrMhz = [](double raw) -> qreal {
        if (!std::isfinite(raw) || raw <= 0.0) {
            return 0.0;
        }
        // Compositor protocols (wlr-randr/Hyprland/Wayland) report millihertz (>= 1000).
        return (raw >= 1000.0) ? (raw / 1000.0) : raw;
    };

    qreal hz = 0.0;
    if (obj.contains(QStringLiteral("refresh_mhz"))) {
        const double mhz = obj.value(QStringLiteral("refresh_mhz")).toDouble(0.0);
        if (std::isfinite(mhz) && mhz > 0.0) {
            hz = mhz / 1000.0;
        }
    } else if (obj.contains(QStringLiteral("refresh"))) {
        hz = normalizeHzOrMhz(obj.value(QStringLiteral("refresh")).toDouble(0.0));
    } else if (obj.contains(QStringLiteral("refresh_rate"))) {
        hz = normalizeHzOrMhz(obj.value(QStringLiteral("refresh_rate")).toDouble(0.0));
    } else if (obj.contains(QStringLiteral("scanout_fps"))) {
        const double fps = obj.value(QStringLiteral("scanout_fps")).toDouble(0.0);
        if (std::isfinite(fps) && fps > 0.0) {
            hz = fps;
        }
    } else if (obj.contains(QStringLiteral("vblank_ns"))) {
        const double ns = obj.value(QStringLiteral("vblank_ns")).toDouble(0.0);
        if (std::isfinite(ns) && ns > 0.0) {
            hz = 1e9 / ns;
        }
    }

    if (!std::isfinite(hz) || hz <= 0.0) {
        return 60.0;
    }
    return std::clamp(hz, 1.0, 1000.0);
}

[[nodiscard]] QString detectActiveDaemonNamespace()
{
    const QStringList args = QCoreApplication::arguments();
    for (int i = 1; i < args.size(); ++i) {
        if ((args[i] == QStringLiteral("--namespace") || args[i] == QStringLiteral("-n")) &&
            i + 1 < args.size()) {
            const QString val = args[i + 1].trimmed();
            if (!val.isEmpty()) {
                return val;
            }
        } else if (args[i].startsWith(QStringLiteral("--namespace="))) {
            const QString val = args[i].mid(12).trimmed();
            if (!val.isEmpty()) {
                return val;
            }
        }
    }

    for (const char *envKey :
         {"WWAL_NAMESPACE", "WAYWAL_NAMESPACE", "AWWW_NAMESPACE", "SWWW_NAMESPACE"}) {
        const char *val = std::getenv(envKey);
        if (val && val[0] != '\0') {
            return QString::fromUtf8(val);
        }
    }
    return QStringLiteral("default");
}

[[nodiscard]] bool hasActiveDaemonSocket(const QString &ns)
{
    const bool hasWaylandDisplay = qEnvironmentVariableIsSet("WAYLAND_DISPLAY") &&
                                   !qEnvironmentVariable("WAYLAND_DISPLAY").trimmed().isEmpty();
    const bool hasExplicitNs =
        (ns != QStringLiteral("default")) || qEnvironmentVariableIsSet("WWAL_NAMESPACE") ||
        qEnvironmentVariableIsSet("WAYWAL_NAMESPACE") ||
        qEnvironmentVariableIsSet("AWWW_NAMESPACE") || qEnvironmentVariableIsSet("SWWW_NAMESPACE");
    if (!hasWaylandDisplay && !hasExplicitNs) {
        return false;
    }

    const QByteArray nsBa = ns.toUtf8();
    path_buf_t sockPath{};
    if (waywal_ipc_resolve_socket_path(&sockPath, nsBa.constData()) &&
        ::access(path_buf_cstr(&sockPath), F_OK) == 0) {
        return true;
    }

    if (ns == QStringLiteral("default") && hasWaylandDisplay) {
        const char *xdg = std::getenv("XDG_RUNTIME_DIR");
        if (!xdg || xdg[0] == '\0') {
            xdg = "/tmp";
        }
        const QByteArray fallback = QByteArray(xdg) + QByteArrayLiteral("/wwald.sock");
        if (::access(fallback.constData(), F_OK) == 0) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] QString findWwalExecutable()
{
    const QString siblingBin = QCoreApplication::applicationDirPath() + QStringLiteral("/wwal");
    if (QFileInfo::exists(siblingBin) && QFileInfo(siblingBin).isExecutable()) {
        return siblingBin;
    }
    const QString pathBin = QStandardPaths::findExecutable(QStringLiteral("wwal"));
    if (!pathBin.isEmpty()) {
        return pathBin;
    }
    const QString localBin = QDir::homePath() + QStringLiteral("/.local/bin/wwal");
    if (QFileInfo::exists(localBin) && QFileInfo(localBin).isExecutable()) {
        return localBin;
    }
    return {};
}

[[nodiscard]] bool decodePreviewImage(const QString &filePath, QImage &outImage,
                                      QSize &outSourceSize)
{
    outImage = QImage();
    outSourceSize = QSize();
    if (filePath.isEmpty() || !QFileInfo::exists(filePath)) {
        return false;
    }

    QImageReader reader(filePath);
    reader.setAutoTransform(true);
    const QSize readerSize = reader.size();
    if (readerSize.isValid() && readerSize.width() > 0 && readerSize.height() > 0) {
        outSourceSize = readerSize;
        if (readerSize.width() > kPreviewMaxDecodeW || readerSize.height() > kPreviewMaxDecodeH) {
            const QSize scaled =
                readerSize.scaled(kPreviewMaxDecodeW, kPreviewMaxDecodeH, Qt::KeepAspectRatio);
            if (scaled.isValid() && scaled.width() > 0 && scaled.height() > 0) {
                reader.setScaledSize(scaled);
            }
        }
    }

    QImage img = reader.read();
    if (!img.isNull()) {
        if (!outSourceSize.isValid()) {
            outSourceSize = img.size();
        }
        if (img.width() > kPreviewMaxDecodeW || img.height() > kPreviewMaxDecodeH) {
            img = img.scaled(kPreviewMaxDecodeW, kPreviewMaxDecodeH, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
        }
        outImage = std::move(img);
        return true;
    }

    // Fallback to libwaywal_core's image_load_rgba (supports QOI/WebP/AVIF/JXL/SVG etc.).
    const QByteArray utf8Path = filePath.toUtf8();
    uint8_t *pixels = nullptr;
    uint32_t w = 0;
    uint32_t h = 0;
    if (image_load_rgba(utf8Path.constData(), &pixels, &w, &h) && pixels && w > 0 && h > 0) {
        const uint64_t rowBytes64 = static_cast<uint64_t>(w) * 4ULL;
        const bool dimsValid = w <= static_cast<uint32_t>(std::numeric_limits<int>::max()) &&
                               h <= static_cast<uint32_t>(std::numeric_limits<int>::max()) &&
                               rowBytes64 <= static_cast<uint64_t>(std::numeric_limits<int>::max());
        if (dimsValid) {
            outSourceSize = QSize(static_cast<int>(w), static_cast<int>(h));
            const QImage raw(pixels, static_cast<int>(w), static_cast<int>(h),
                             static_cast<qsizetype>(rowBytes64), QImage::Format_RGBA8888);
            if (w > static_cast<uint32_t>(kPreviewMaxDecodeW) ||
                h > static_cast<uint32_t>(kPreviewMaxDecodeH)) {
                img = raw.scaled(kPreviewMaxDecodeW, kPreviewMaxDecodeH, Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);
            } else {
                img = raw.copy();
            }
        }
        image_free(pixels);
    } else if (pixels) {
        image_free(pixels);
    }

    if (!img.isNull()) {
        outImage = std::move(img);
        return true;
    }
    return false;
}

[[nodiscard]] QString extractMimeWallpaperPath(const QMimeData *mime)
{
    if (!mime) {
        return {};
    }
    if (mime->hasUrls()) {
        for (const QUrl &url : mime->urls()) {
            const QString local = url.toLocalFile().trimmed();
            if (!local.isEmpty()) {
                return local;
            }
        }
    }
    if (mime->hasFormat(QStringLiteral("application/x-waywal-wallpaper"))) {
        const QString custom =
            QString::fromUtf8(mime->data(QStringLiteral("application/x-waywal-wallpaper")))
                .trimmed();
        if (!custom.isEmpty()) {
            return custom;
        }
    }
    if (mime->hasText()) {
        const QString text = mime->text().trimmed();
        if (!text.isEmpty()) {
            return text;
        }
    }
    return {};
}

} // namespace

MonitorTopologyCanvas::MonitorTopologyCanvas(QWidget *parent) : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAcceptDrops(true);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setAccessibleName(QStringLiteral("Monitor Topology Canvas"));
    setAccessibleDescription(
        QStringLiteral("Interactive multi-monitor layout and wallpaper scaling canvas"));

    refreshFromWayland();
}

MonitorTopologyCanvas::~MonitorTopologyCanvas()
{
    if (m_queryProc) {
        m_queryProc->disconnect(this);
        if (m_queryProc->state() != QProcess::NotRunning) {
            m_queryProc->kill();
            m_queryProc->waitForFinished(200);
        }
        delete m_queryProc;
        m_queryProc.clear();
    }
    stopDecodeWorker();
}

void MonitorTopologyCanvas::stopDecodeWorker()
{
    {
        std::lock_guard<std::mutex> lock(m_decodeMutex);
        m_decodeStop = true;
        m_decodeQueue.clear();
    }
    m_decodeCv.notify_all();
    if (m_decodeThread.joinable()) {
        m_decodeThread.request_stop();
        m_decodeThread.join();
    }
}

void MonitorTopologyCanvas::ensureDecodeWorkerStarted()
{
    if (m_decodeThread.joinable()) {
        return;
    }
    m_decodeStop = false;
    QPointer<MonitorTopologyCanvas> self(this);
    m_decodeThread = std::jthread([this, self](std::stop_token st) {
        for (;;) {
            DecodeTask task;
            {
                std::unique_lock<std::mutex> lock(m_decodeMutex);
                m_decodeCv.wait(lock, st,
                                [this]() { return m_decodeStop || !m_decodeQueue.empty(); });
                if (st.stop_requested() || m_decodeStop) {
                    return;
                }
                if (m_decodeQueue.empty()) {
                    continue;
                }
                task = std::move(m_decodeQueue.front());
                m_decodeQueue.pop_front();
            }

            QImage img;
            QSize srcSize;
            const bool ok = decodePreviewImage(task.filePath, img, srcSize);
            if (!ok || st.stop_requested()) {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(m_decodeMutex);
                if (m_decodeStop) {
                    return;
                }
            }

            QMetaObject::invokeMethod(
                self.data(),
                [self, targetKey = std::move(task.targetKey), gen = task.generation,
                 img = std::move(img), srcSize]() {
                    if (!self) {
                        return;
                    }
                    if (self->m_previewLoadGens.value(targetKey, 0ULL) != gen) {
                        return;
                    }
                    const QPixmap pix = QPixmap::fromImage(img);
                    if (pix.isNull()) {
                        return;
                    }
                    if (targetKey == QStringLiteral("*")) {
                        self->m_spanWallpaperPixmap = pix;
                        self->m_spanWallpaperSourceSize = srcSize;
                    } else if (auto *m = self->findMonitor(targetKey)) {
                        m->wallpaperPixmap = pix;
                        m->wallpaperSourceSize = srcSize;
                    }
                    self->update();
                },
                Qt::QueuedConnection);
        }
    });
}

void MonitorTopologyCanvas::enqueueAsyncWallpaperLoad(const QString &targetKey,
                                                      const QString &filePath)
{
    const uint64_t gen = m_previewLoadGens.value(targetKey, 0ULL);
    ensureDecodeWorkerStarted();
    {
        std::lock_guard<std::mutex> lock(m_decodeMutex);
        if (m_decodeStop) {
            return;
        }
        // Coalesce queued loads targeting the same monitor or span slot to avoid backlog on bursts.
        for (auto &queued : m_decodeQueue) {
            if (queued.targetKey == targetKey) {
                queued.filePath = filePath;
                queued.generation = gen;
                m_decodeCv.notify_one();
                return;
            }
        }
        m_decodeQueue.push_back(DecodeTask{targetKey, filePath, gen});
    }
    m_decodeCv.notify_one();
}

void MonitorTopologyCanvas::refreshFromWayland()
{
    ++m_outputsGeneration;
    const uint64_t reqGen = m_outputsGeneration;

    if (auto *ipc = Services::instance().ipcClient(); ipc && ipc->isConnected()) {
        if (m_monitors.isEmpty()) {
            applyScreensFallback();
        }
        QPointer<MonitorTopologyCanvas> self(this);
        ipc->requestOutputsJson([self, reqGen](const QString &json) {
            if (!self || self->m_outputsGeneration != reqGen || json.isEmpty()) {
                return;
            }
            self->updateOutputsFromJson(json.toUtf8());
        });
        return;
    }

    const QString ns = detectActiveDaemonNamespace();
    if (hasActiveDaemonSocket(ns)) {
        const QString wwalBin = findWwalExecutable();
        if (!wwalBin.isEmpty()) {
            if (m_monitors.isEmpty()) {
                applyScreensFallback();
            }
            if (m_queryProc) {
                m_queryProc->disconnect(this);
                if (m_queryProc->state() != QProcess::NotRunning) {
                    m_queryProc->kill();
                    m_queryProc->waitForFinished(100);
                }
                m_queryProc->deleteLater();
                m_queryProc.clear();
            }

            auto *proc = new QProcess(this);
            m_queryProc = proc;
            QStringList args;
            if (ns != QStringLiteral("default")) {
                args << QStringLiteral("--namespace") << ns;
            }
            args << QStringLiteral("query") << QStringLiteral("--json");

            QPointer<MonitorTopologyCanvas> self(this);
            connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                    [self, proc, reqGen](int exitCode, QProcess::ExitStatus exitStatus) {
                        const QByteArray stdoutData = proc->readAllStandardOutput();
                        if (self && self->m_queryProc == proc) {
                            self->m_queryProc.clear();
                        }
                        proc->deleteLater();
                        if (!self || self->m_outputsGeneration != reqGen) {
                            return;
                        }
                        if (exitStatus == QProcess::NormalExit && exitCode == 0 &&
                            !stdoutData.trimmed().isEmpty()) {
                            self->updateOutputsFromJson(stdoutData);
                        }
                    });
            QTimer::singleShot(2000, proc, [proc]() {
                if (proc->state() != QProcess::NotRunning) {
                    proc->kill();
                }
            });
            proc->start(wwalBin, args);
            return;
        }
    }

    applyScreensFallback();
}

void MonitorTopologyCanvas::applyScreensFallback()
{
    const QList<QScreen *> screens = QGuiApplication::screens();
    if (screens.isEmpty()) {
        return;
    }

    QMap<QString, MonitorInfo> existingSettings;
    for (const auto &m : m_monitors) {
        existingSettings.insert(m.name, m);
    }

    m_monitors.clear();
    QSet<QString> seenNames;

    int screenIdx = 1;
    for (QScreen *screen : screens) {
        if (!screen) {
            continue;
        }
        const QRect geom = screen->geometry();
        const QSize sz = screen->size();
        const int logicalW = geom.width() > 0 ? geom.width() : sz.width();
        const int logicalH = geom.height() > 0 ? geom.height() : sz.height();
        if (logicalW <= 0 || logicalH <= 0) {
            ++screenIdx;
            continue;
        }

        QString candidateName = screen->name().trimmed();
        if (candidateName.isEmpty()) {
            candidateName = QStringLiteral("Display-%1").arg(screenIdx);
        }
        while (seenNames.contains(candidateName)) {
            ++screenIdx;
            candidateName = QStringLiteral("%1-%2").arg(candidateName).arg(screenIdx);
        }
        seenNames.insert(candidateName);
        ++screenIdx;

        MonitorInfo info;
        info.name = candidateName;
        info.model = screen->model();
        info.manufacturer = screen->manufacturer();
        info.geometry = QRect(geom.x(), geom.y(), logicalW, logicalH);

        const qreal rawDpr = screen->devicePixelRatio();
        info.devicePixelRatio =
            (std::isfinite(rawDpr) && rawDpr > 0.0) ? std::clamp(rawDpr, 0.25, 8.0) : 1.0;
        const int physW =
            std::max(1, static_cast<int>(std::lround(logicalW * info.devicePixelRatio)));
        const int physH =
            std::max(1, static_cast<int>(std::lround(logicalH * info.devicePixelRatio)));
        info.physicalResolution = QSize(physW, physH);

        const qreal rawRefresh = screen->refreshRate();
        info.refreshRateHz = (std::isfinite(rawRefresh) && rawRefresh > 0.0)
                                 ? std::clamp(rawRefresh, 1.0, 1000.0)
                                 : 60.0;
        info.isConnected = true;
        info.isPrimary = (screen == QGuiApplication::primaryScreen());

        if (existingSettings.contains(info.name)) {
            const auto &prev = existingSettings[info.name];
            info.assignedWallpaperPath = prev.assignedWallpaperPath;
            info.wallpaperPixmap = prev.wallpaperPixmap;
            info.wallpaperSourceSize = prev.wallpaperSourceSize;
            info.scalingMode = prev.scalingMode;
            info.cropGravity = prev.cropGravity;
        }

        m_monitors.append(info);
    }

    pruneSelectionAndClampFocus();
    recalculateLayoutTransform();
    emit outputsRefreshed(m_monitors.size());
    emit monitorSelectionChanged(selectedMonitorNames());
    emit spanRectsCalculated(calculateSpanRectangles().normalizedRects);
    update();
}

void MonitorTopologyCanvas::updateOutputsFromJson(const QByteArray &jsonBytes)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes, &err);
    if (err.error != QJsonParseError::NoError) {
        return;
    }
    if (doc.isObject()) {
        updateOutputsFromJson(doc.object());
    } else if (doc.isArray()) {
        ++m_outputsGeneration;
        applyOutputsArray(doc.array());
    }
}

void MonitorTopologyCanvas::updateOutputsFromJson(const QJsonObject &rootObj)
{
    QJsonArray outputsArray;
    if (rootObj.contains(QStringLiteral("outputs")) &&
        rootObj.value(QStringLiteral("outputs")).isArray()) {
        outputsArray = rootObj.value(QStringLiteral("outputs")).toArray();
    } else if (rootObj.contains(QStringLiteral("screens")) &&
               rootObj.value(QStringLiteral("screens")).isArray()) {
        outputsArray = rootObj.value(QStringLiteral("screens")).toArray();
    } else {
        return;
    }

    ++m_outputsGeneration;
    applyOutputsArray(outputsArray);
}

void MonitorTopologyCanvas::applyOutputsArray(const QJsonArray &outputsArray)
{
    QMap<QString, MonitorInfo> existingSettings;
    for (const auto &m : m_monitors) {
        existingSettings.insert(m.name, m);
    }

    m_monitors.clear();
    QSet<QString> seenNames;

    for (const auto &val : outputsArray) {
        if (!val.isObject()) {
            continue;
        }
        const QJsonObject obj = val.toObject();
        const QString name = obj.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty() || seenNames.contains(name)) {
            continue;
        }

        int physW = obj.value(QStringLiteral("width")).toInt(0);
        int physH = obj.value(QStringLiteral("height")).toInt(0);
        const int logW = obj.value(QStringLiteral("logical_width")).toInt(0);
        const int logH = obj.value(QStringLiteral("logical_height")).toInt(0);
        if ((physW <= 0 || physH <= 0) && logW > 0 && logH > 0) {
            physW = logW;
            physH = logH;
        }
        if (physW <= 0 || physH <= 0 || physW > 65536 || physH > 65536) {
            continue;
        }

        const int geomW = (logW > 0 && logW <= 65536) ? logW : physW;
        const int geomH = (logH > 0 && logH <= 65536) ? logH : physH;
        const int x = std::clamp(obj.value(QStringLiteral("x")).toInt(0), -65536, 65536);
        const int y = std::clamp(obj.value(QStringLiteral("y")).toInt(0), -65536, 65536);

        seenNames.insert(name);

        MonitorInfo info;
        info.name = name;
        info.model = obj.value(QStringLiteral("model")).toString();
        info.manufacturer = obj.value(QStringLiteral("make")).toString();
        info.geometry = QRect(x, y, geomW, geomH);
        info.physicalResolution = QSize(physW, physH);

        const double rawScale = obj.value(QStringLiteral("scale")).toDouble(1.0);
        info.devicePixelRatio =
            (std::isfinite(rawScale) && rawScale > 0.0) ? std::clamp(rawScale, 0.25, 8.0) : 1.0;
        info.refreshRateHz = resolveRefreshRateHz(obj);
        info.isConnected = true;
        info.isPrimary = obj.value(QStringLiteral("primary")).toBool(false);

        if (existingSettings.contains(info.name)) {
            const auto &prev = existingSettings[info.name];
            info.assignedWallpaperPath = prev.assignedWallpaperPath;
            info.wallpaperPixmap = prev.wallpaperPixmap;
            info.wallpaperSourceSize = prev.wallpaperSourceSize;
            info.scalingMode = prev.scalingMode;
            info.cropGravity = prev.cropGravity;
        }

        m_monitors.append(info);
    }

    pruneSelectionAndClampFocus();
    recalculateLayoutTransform();
    emit outputsRefreshed(m_monitors.size());
    emit monitorSelectionChanged(selectedMonitorNames());
    emit spanRectsCalculated(calculateSpanRectangles().normalizedRects);
    update();
}

void MonitorTopologyCanvas::pruneSelectionAndClampFocus()
{
    QSet<QString> connectedNames;
    connectedNames.reserve(m_monitors.size());
    for (const auto &m : m_monitors) {
        connectedNames.insert(m.name);
    }

    m_selectedMonitors.intersect(connectedNames);
    if (!connectedNames.contains(m_hoveredMonitorName)) {
        m_hoveredMonitorName.clear();
    }
    if (!connectedNames.contains(m_dropTargetMonitorName)) {
        m_dropTargetMonitorName.clear();
    }

    if (m_selectedMonitors.isEmpty() && !m_monitors.isEmpty()) {
        QString fallbackName = m_monitors.first().name;
        for (const auto &m : m_monitors) {
            if (m.isPrimary) {
                fallbackName = m.name;
                break;
            }
        }
        m_selectedMonitors.insert(fallbackName);
    }

    if (m_monitors.isEmpty()) {
        m_focusedMonitorIndex = -1;
    } else if (m_focusedMonitorIndex < 0 || m_focusedMonitorIndex >= m_monitors.size()) {
        m_focusedMonitorIndex = 0;
        for (int i = 0; i < m_monitors.size(); ++i) {
            if (m_selectedMonitors.contains(m_monitors[i].name)) {
                m_focusedMonitorIndex = i;
                break;
            }
        }
    }

    m_dragMode = DragMode::None;
    m_activeDragMonitorIndex = -1;
    m_dragMoved = false;
    m_dragInitialGeometries.clear();
    m_activeSnapGuides.clear();
}

MonitorInfo *MonitorTopologyCanvas::findMonitor(const QString &name)
{
    for (auto &m : m_monitors) {
        if (m.name == name) {
            return &m;
        }
    }
    return nullptr;
}

const MonitorInfo *MonitorTopologyCanvas::findMonitor(const QString &name) const
{
    for (const auto &m : m_monitors) {
        if (m.name == name) {
            return &m;
        }
    }
    return nullptr;
}

QStringList MonitorTopologyCanvas::selectedMonitorNames() const
{
    QStringList names;
    names.reserve(m_selectedMonitors.size());
    for (const auto &m : m_monitors) {
        if (m_selectedMonitors.contains(m.name)) {
            names.append(m.name);
        }
    }
    return names;
}

void MonitorTopologyCanvas::setSelectedMonitors(const QStringList &names)
{
    m_selectedMonitors.clear();
    for (const QString &n : names) {
        if (findMonitor(n)) {
            m_selectedMonitors.insert(n);
        }
    }
    for (int i = 0; i < m_monitors.size(); ++i) {
        if (m_selectedMonitors.contains(m_monitors[i].name)) {
            m_focusedMonitorIndex = i;
            break;
        }
    }
    emit monitorSelectionChanged(selectedMonitorNames());
    update();
}

void MonitorTopologyCanvas::selectMonitor(const QString &name, bool additive)
{
    if (!additive) {
        m_selectedMonitors.clear();
    }

    if (m_selectedMonitors.contains(name)) {
        if (additive) {
            m_selectedMonitors.remove(name);
        }
    } else {
        if (findMonitor(name)) {
            m_selectedMonitors.insert(name);
        }
    }

    for (int i = 0; i < m_monitors.size(); ++i) {
        if (m_monitors[i].name == name) {
            m_focusedMonitorIndex = i;
            break;
        }
    }

    emit monitorSelectionChanged(selectedMonitorNames());
    update();
}

void MonitorTopologyCanvas::selectAllMonitors()
{
    m_selectedMonitors.clear();
    for (const auto &m : m_monitors) {
        m_selectedMonitors.insert(m.name);
    }
    if (!m_monitors.isEmpty() && m_focusedMonitorIndex < 0) {
        m_focusedMonitorIndex = 0;
    }
    emit monitorSelectionChanged(selectedMonitorNames());
    update();
}

void MonitorTopologyCanvas::clearSelection()
{
    m_selectedMonitors.clear();
    emit monitorSelectionChanged(selectedMonitorNames());
    update();
}

void MonitorTopologyCanvas::setDesktopSpanMode(bool enabled)
{
    if (m_spanMode != enabled) {
        m_spanMode = enabled;
        if (enabled) {
            if (m_scalingMode != static_cast<int>(WAYWAL_SCALING_SPAN)) {
                m_nonSpanScalingMode = m_scalingMode;
                m_scalingMode = static_cast<int>(WAYWAL_SCALING_SPAN);
            }
        } else if (m_scalingMode == static_cast<int>(WAYWAL_SCALING_SPAN)) {
            m_scalingMode = (m_nonSpanScalingMode != static_cast<int>(WAYWAL_SCALING_SPAN))
                                ? m_nonSpanScalingMode
                                : static_cast<int>(WAYWAL_SCALING_FILL);
        }
        emit desktopSpanModeToggled(m_spanMode);
        emit spanRectsCalculated(calculateSpanRectangles().normalizedRects);
        update();
    }
}

void MonitorTopologyCanvas::setSnapEnabled(bool enabled)
{
    if (m_snapEnabled != enabled) {
        m_snapEnabled = enabled;
        emit snapToggled(m_snapEnabled);
        update();
    }
}

void MonitorTopologyCanvas::setSnapGridStepPx(int stepPx)
{
    m_snapStepPx = std::clamp(stepPx, 1, 512);
}

void MonitorTopologyCanvas::setScalingMode(int mode)
{
    const int clampedMode = (mode >= static_cast<int>(WAYWAL_SCALING_FILL) &&
                             mode <= static_cast<int>(WAYWAL_SCALING_SPAN))
                                ? mode
                                : static_cast<int>(WAYWAL_SCALING_FILL);
    const bool wantSpan = (clampedMode == static_cast<int>(WAYWAL_SCALING_SPAN));

    if (!wantSpan) {
        m_nonSpanScalingMode = clampedMode;
    }
    m_scalingMode = clampedMode;

    QStringList targets = selectedMonitorNames();
    for (const QString &name : targets) {
        if (auto *m = findMonitor(name)) {
            m->scalingMode = clampedMode;
        }
    }

    if (m_spanMode != wantSpan) {
        m_spanMode = wantSpan;
        emit desktopSpanModeToggled(m_spanMode);
        emit spanRectsCalculated(calculateSpanRectangles().normalizedRects);
    }

    emit scalingModeChanged(targets, clampedMode);
    update();
}

QPointF MonitorTopologyCanvas::cropGravity(const QString &monitorName) const
{
    if (m_spanMode && monitorName.isEmpty()) {
        return m_spanCropGravity;
    }
    if (const auto *m = findMonitor(monitorName)) {
        return m->cropGravity;
    }
    return m_spanMode ? m_spanCropGravity : QPointF(0.5, 0.5);
}

void MonitorTopologyCanvas::setCropGravity(const QString &monitorName, const QPointF &gravity)
{
    const qreal gx = std::isfinite(gravity.x()) ? std::clamp(gravity.x(), 0.0, 1.0) : 0.5;
    const qreal gy = std::isfinite(gravity.y()) ? std::clamp(gravity.y(), 0.0, 1.0) : 0.5;
    const QPointF clamped(gx, gy);
    if (auto *m = findMonitor(monitorName)) {
        m->cropGravity = clamped;
        emit cropGravityChanged({monitorName}, static_cast<float>(clamped.x()),
                                static_cast<float>(clamped.y()));
        update();
    }
}

void MonitorTopologyCanvas::setCropGravityForSelection(const QPointF &gravity)
{
    const qreal gx = std::isfinite(gravity.x()) ? std::clamp(gravity.x(), 0.0, 1.0) : 0.5;
    const qreal gy = std::isfinite(gravity.y()) ? std::clamp(gravity.y(), 0.0, 1.0) : 0.5;
    const QPointF clamped(gx, gy);
    if (m_spanMode) {
        m_spanCropGravity = clamped;
        emit cropGravityChanged(selectedMonitorNames(), static_cast<float>(clamped.x()),
                                static_cast<float>(clamped.y()));
    } else {
        const QStringList targets = selectedMonitorNames();
        for (const QString &name : targets) {
            if (auto *m = findMonitor(name)) {
                m->cropGravity = clamped;
            }
        }
        emit cropGravityChanged(targets, static_cast<float>(clamped.x()),
                                static_cast<float>(clamped.y()));
    }
    update();
}

void MonitorTopologyCanvas::assignWallpaper(const QString &monitorName, const QString &filePath,
                                            const QPixmap &pixmap)
{
    if (auto *m = findMonitor(monitorName)) {
        m_previewLoadGens[monitorName] = m_previewLoadGens.value(monitorName, 0ULL) + 1ULL;
        m->assignedWallpaperPath = filePath;
        if (!pixmap.isNull()) {
            m->wallpaperPixmap = pixmap;
            m->wallpaperSourceSize = pixmap.size();
        } else if (!filePath.isEmpty()) {
            m->wallpaperPixmap = QPixmap();
            m->wallpaperSourceSize = QSize();
            enqueueAsyncWallpaperLoad(monitorName, filePath);
        } else {
            m->wallpaperPixmap = QPixmap();
            m->wallpaperSourceSize = QSize();
        }
        emit wallpaperDropped({monitorName}, filePath);
        update();
    }
}

void MonitorTopologyCanvas::assignWallpaperToSelection(const QString &filePath,
                                                       const QPixmap &pixmap)
{
    if (m_spanMode) {
        const QString spanKey = QStringLiteral("*");
        m_previewLoadGens[spanKey] = m_previewLoadGens.value(spanKey, 0ULL) + 1ULL;
        m_spanWallpaperPath = filePath;
        if (!pixmap.isNull()) {
            m_spanWallpaperPixmap = pixmap;
            m_spanWallpaperSourceSize = pixmap.size();
        } else if (!filePath.isEmpty()) {
            m_spanWallpaperPixmap = QPixmap();
            m_spanWallpaperSourceSize = QSize();
            enqueueAsyncWallpaperLoad(spanKey, filePath);
        } else {
            m_spanWallpaperPixmap = QPixmap();
            m_spanWallpaperSourceSize = QSize();
        }
        emit wallpaperDropped(selectedMonitorNames(), filePath);
    } else {
        const QStringList targets = selectedMonitorNames();
        for (const QString &name : targets) {
            assignWallpaper(name, filePath, pixmap);
        }
    }
    update();
}

DesktopSpanCalculation MonitorTopologyCanvas::calculateSpanRectangles() const
{
    DesktopSpanCalculation calc;
    if (m_monitors.isEmpty()) {
        calc.boundingBox = QRect(0, 0, 1920, 1080);
        return calc;
    }

    int minX = std::numeric_limits<int>::max();
    int minY = std::numeric_limits<int>::max();
    int maxX = std::numeric_limits<int>::min();
    int maxY = std::numeric_limits<int>::min();

    for (const auto &m : m_monitors) {
        minX = std::min(minX, m.geometry.left());
        minY = std::min(minY, m.geometry.top());
        maxX = std::max(maxX, m.geometry.right() + 1);
        maxY = std::max(maxY, m.geometry.bottom() + 1);
    }

    const int spanW = std::max(1, maxX - minX);
    const int spanH = std::max(1, maxY - minY);
    calc.boundingBox = QRect(minX, minY, spanW, spanH);

    for (const auto &m : m_monitors) {
        const qreal u_x = static_cast<qreal>(m.geometry.left() - minX) / spanW;
        const qreal u_y = static_cast<qreal>(m.geometry.top() - minY) / spanH;
        const qreal u_w = static_cast<qreal>(m.geometry.width()) / spanW;
        const qreal u_h = static_cast<qreal>(m.geometry.height()) / spanH;

        calc.normalizedRects.insert(m.name, QRectF(u_x, u_y, u_w, u_h));
    }

    return calc;
}

void MonitorTopologyCanvas::recalculateLayoutTransform()
{
    const DesktopSpanCalculation span = calculateSpanRectangles();
    m_virtualBoundingBox = span.boundingBox;

    const bool compact = (height() < 150);
    const qreal padX = compact ? std::min(m_padding, 12.0) : m_padding;
    const qreal padY = compact ? std::min(m_padding * 0.5, 6.0) : m_padding * (2.0 / 3.0);
    const qreal topReserve = compact ? 0.0 : 44.0;

    const qreal availW = std::max(60.0, static_cast<qreal>(width()) - 2.0 * padX);
    const qreal availH = std::max(36.0, static_cast<qreal>(height()) - 2.0 * padY - topReserve);

    const qreal scaleX = availW / std::max(1, m_virtualBoundingBox.width());
    const qreal scaleY = availH / std::max(1, m_virtualBoundingBox.height());
    m_canvasScale = std::min(scaleX, scaleY);

    const qreal renderW = m_virtualBoundingBox.width() * m_canvasScale;
    const qreal renderH = m_virtualBoundingBox.height() * m_canvasScale;

    m_canvasOffsetX = padX + (availW - renderW) * 0.5 -
                      m_virtualBoundingBox.left() * m_canvasScale + m_panOffset.x();
    m_canvasOffsetY = padY + topReserve + (availH - renderH) * 0.5 -
                      m_virtualBoundingBox.top() * m_canvasScale + m_panOffset.y();
}

QRectF MonitorTopologyCanvas::mapToCanvas(const QRect &compositorRect) const
{
    return QRectF(m_canvasOffsetX + compositorRect.left() * m_canvasScale,
                  m_canvasOffsetY + compositorRect.top() * m_canvasScale,
                  compositorRect.width() * m_canvasScale, compositorRect.height() * m_canvasScale);
}

QRectF MonitorTopologyCanvas::getMonitorCanvasRect(int monitorIndex) const
{
    if (monitorIndex < 0 || monitorIndex >= m_monitors.size()) {
        return QRectF();
    }
    return mapToCanvas(m_monitors[monitorIndex].geometry);
}

int MonitorTopologyCanvas::findMonitorAtCanvasPos(const QPointF &pos) const
{
    for (int i = m_monitors.size() - 1; i >= 0; --i) {
        if (getMonitorCanvasRect(i).contains(pos)) {
            return i;
        }
    }
    return -1;
}

QRectF MonitorTopologyCanvas::getCropGravityHandleRect(int monitorIndex) const
{
    const QRectF mRect = getMonitorCanvasRect(monitorIndex);
    if (mRect.isEmpty() || monitorIndex < 0 || monitorIndex >= m_monitors.size()) {
        return QRectF();
    }

    const auto &m = m_monitors[monitorIndex];
    const qreal hx = mRect.left() + m.cropGravity.x() * mRect.width();
    const qreal hy = mRect.top() + m.cropGravity.y() * mRect.height();
    // 28x28 px minimum hit target satisfies WCAG 2.2 SC 2.5.8 Target Size.
    const qreal r = 14.0;
    return QRectF(hx - r, hy - r, 2.0 * r, 2.0 * r);
}

QRectF MonitorTopologyCanvas::getSpanGravityHandleRect() const
{
    const QRectF spanRect = mapToCanvas(m_virtualBoundingBox);
    const qreal hx = spanRect.left() + m_spanCropGravity.x() * spanRect.width();
    const qreal hy = spanRect.top() + m_spanCropGravity.y() * spanRect.height();
    const qreal r = 14.0;
    return QRectF(hx - r, hy - r, 2.0 * r, 2.0 * r);
}

QRectF MonitorTopologyCanvas::toolbarBarRect() const
{
    if (height() < 150) {
        return QRectF();
    }
    return QRectF(12.0, 6.0, std::max(120.0, static_cast<qreal>(width()) - 24.0), 34.0);
}

QRectF MonitorTopologyCanvas::modePillRect() const
{
    const QRectF bar = toolbarBarRect();
    if (bar.isEmpty()) {
        return QRectF();
    }
    QFont font = this->font();
    font.setPixelSize(11);
    font.setBold(true);
    const QFontMetrics fm(font);
    const QString modeText =
        m_spanMode ? QStringLiteral("MODE: DESKTOP SPAN") : QStringLiteral("MODE: PER-MONITOR");
    const qreal pillW =
        std::max(148.0, std::min(bar.width() - 16.0,
                                 static_cast<qreal>(fm.horizontalAdvance(modeText)) + 24.0));
    constexpr qreal pillH = 28.0;
    return QRectF(bar.right() - pillW - 4.0, bar.top() + (bar.height() - pillH) * 0.5, pillW,
                  pillH);
}

QRectF MonitorTopologyCanvas::snapPillRect() const
{
    const QRectF bar = toolbarBarRect();
    if (bar.isEmpty()) {
        return QRectF();
    }
    const QRectF modeRect = modePillRect();
    QFont font = this->font();
    font.setPixelSize(11);
    font.setBold(true);
    const QFontMetrics fm(font);
    const QString snapText =
        m_snapEnabled ? QStringLiteral("SNAP: ON") : QStringLiteral("SNAP: OFF");
    const qreal pillW = std::max(76.0, static_cast<qreal>(fm.horizontalAdvance(snapText)) + 20.0);
    constexpr qreal pillH = 28.0;
    const qreal rightEdge = modeRect.left() - 8.0;
    if (rightEdge - pillW < bar.left() + 140.0) {
        return QRectF();
    }
    return QRectF(rightEdge - pillW, bar.top() + (bar.height() - pillH) * 0.5, pillW, pillH);
}

void MonitorTopologyCanvas::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    drawGridBackground(painter);
    drawMonitors(painter);

    if (m_spanMode) {
        drawSpanOverlay(painter);
    }

    if (m_dragMode == DragMode::MonitorMove && !m_activeSnapGuides.isEmpty()) {
        painter.save();
        painter.setPen(QPen(QColor(0x38, 0xBD, 0xF8, 200), 1.0, Qt::DashLine));
        for (const SnapGuide &guide : m_activeSnapGuides) {
            if (guide.vertical) {
                const qreal x = m_canvasOffsetX + guide.coord * m_canvasScale;
                painter.drawLine(QPointF(x, 0.0), QPointF(x, static_cast<qreal>(height())));
            } else {
                const qreal y = m_canvasOffsetY + guide.coord * m_canvasScale;
                painter.drawLine(QPointF(0.0, y), QPointF(static_cast<qreal>(width()), y));
            }
        }
        painter.restore();
    }

    if (m_dragMode == DragMode::SelectionBox && m_dragMoved) {
        painter.save();
        const QRectF selBox = QRectF(m_dragStartPos, m_currentMousePos).normalized();
        painter.setPen(QPen(QColor(0x38, 0xBD, 0xF8), 1.5));
        painter.setBrush(QColor(0x38, 0xBD, 0xF8, 36));
        painter.drawRect(selBox);
        painter.restore();
    }

    drawGravityHandles(painter);
    drawToolbarOverlay(painter);

    if (hasFocus()) {
        painter.save();
        painter.setPen(QPen(QColor(0x38, 0xBD, 0xF8), 2.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0), 4.0, 4.0);
        painter.restore();
    }
}

void MonitorTopologyCanvas::drawGridBackground(QPainter &painter)
{
    painter.fillRect(rect(), QColor(0x0B, 0x0E, 0x14));

    painter.setPen(QPen(QColor(0x28, 0x30, 0x42, 140), 1.0));
    constexpr int gridSize = 24;
    for (int x = 0; x < width(); x += gridSize) {
        for (int y = 0; y < height(); y += gridSize) {
            painter.drawPoint(QPointF(x + 0.5, y + 0.5));
        }
    }
}

void MonitorTopologyCanvas::drawMonitors(QPainter &painter)
{
    const DesktopSpanCalculation spanCalc = calculateSpanRectangles();

    for (int i = 0; i < m_monitors.size(); ++i) {
        const auto &m = m_monitors[i];
        const QRectF mRect = getMonitorCanvasRect(i);
        if (mRect.width() < 1.0 || mRect.height() < 1.0) {
            continue;
        }
        const bool isSelected = m_selectedMonitors.contains(m.name);
        const bool isFocusedMonitor = (hasFocus() && i == m_focusedMonitorIndex);
        const bool isHovered = (m.name == m_hoveredMonitorName);
        const bool isDropTarget = (m.name == m_dropTargetMonitorName);

        QPainterPath bezelPath;
        bezelPath.addRoundedRect(mRect, 6.0, 6.0);

        painter.save();
        painter.setClipPath(bezelPath);

        painter.fillRect(mRect, QColor(0x18, 0x1B, 0x22));

        const QPixmap *pix = nullptr;
        if (m_spanMode && !m_spanWallpaperPixmap.isNull()) {
            pix = &m_spanWallpaperPixmap;
        } else if (!m.wallpaperPixmap.isNull()) {
            pix = &m.wallpaperPixmap;
        }

        if (pix && !pix->isNull() && pix->width() > 0 && pix->height() > 0) {
            if (m_spanMode) {
                // Match layer_shell.c: aspect-preserving cover crop over the virtual span box
                // anchored by m_spanCropGravity, then sample each monitor's normalized slice.
                const qreal totW = std::max(1, spanCalc.boundingBox.width());
                const qreal totH = std::max(1, spanCalc.boundingBox.height());
                const qreal pixW = static_cast<qreal>(pix->width());
                const qreal pixH = static_cast<qreal>(pix->height());
                const qreal rSrc = pixW / pixH;
                const qreal rDst = totW / totH;
                qreal cropW = pixW;
                qreal cropH = pixH;
                qreal offX = 0.0;
                qreal offY = 0.0;
                if (rSrc > rDst) {
                    cropH = pixH;
                    cropW = cropH * rDst;
                    offX =
                        std::max(0.0, pixW - cropW) * std::clamp(m_spanCropGravity.x(), 0.0, 1.0);
                } else {
                    cropW = pixW;
                    cropH = cropW / rDst;
                    offY =
                        std::max(0.0, pixH - cropH) * std::clamp(m_spanCropGravity.y(), 0.0, 1.0);
                }
                const QRectF normRect = spanCalc.normalizedRects.value(m.name, QRectF(0, 0, 1, 1));
                const QRectF srcRect(offX + normRect.x() * cropW, offY + normRect.y() * cropH,
                                     normRect.width() * cropW, normRect.height() * cropH);
                painter.drawPixmap(mRect, *pix, srcRect);
            } else {
                const QSize srcPixels =
                    m.wallpaperSourceSize.isValid() ? m.wallpaperSourceSize : pix->size();
                const qreal srcW = std::max(1, srcPixels.width());
                const qreal srcH = std::max(1, srcPixels.height());
                const qreal monW =
                    std::max(1, m.physicalResolution.width() > 0 ? m.physicalResolution.width()
                                                                 : m.geometry.width());
                const qreal monH =
                    std::max(1, m.physicalResolution.height() > 0 ? m.physicalResolution.height()
                                                                  : m.geometry.height());

                if (m.scalingMode == WAYWAL_SCALING_FIT) {
                    const qreal scale =
                        std::min(mRect.width() / pix->width(), mRect.height() / pix->height());
                    const qreal w = pix->width() * scale;
                    const qreal h = pix->height() * scale;
                    const QRectF destRect(mRect.x() + (mRect.width() - w) * 0.5,
                                          mRect.y() + (mRect.height() - h) * 0.5, w, h);
                    painter.fillRect(mRect, Qt::black);
                    painter.drawPixmap(destRect, *pix, pix->rect());
                } else if (m.scalingMode == WAYWAL_SCALING_STRETCH) {
                    painter.drawPixmap(mRect, *pix, pix->rect());
                } else if (m.scalingMode == WAYWAL_SCALING_CENTER) {
                    const qreal w = srcW * (mRect.width() / monW);
                    const qreal h = srcH * (mRect.height() / monH);
                    const QRectF destRect(mRect.x() + (mRect.width() - w) * 0.5,
                                          mRect.y() + (mRect.height() - h) * 0.5, w, h);
                    painter.fillRect(mRect, Qt::black);
                    painter.drawPixmap(destRect, *pix, pix->rect());
                } else if (m.scalingMode == WAYWAL_SCALING_TILE) {
                    const qreal tileW = std::max(kMinTilePreviewPx, srcW * (mRect.width() / monW));
                    const qreal tileH = std::max(kMinTilePreviewPx, srcH * (mRect.height() / monH));
                    painter.fillRect(mRect, Qt::black);
                    for (qreal ty = mRect.top(); ty < mRect.bottom(); ty += tileH) {
                        for (qreal tx = mRect.left(); tx < mRect.right(); tx += tileW) {
                            painter.drawPixmap(QRectF(tx, ty, tileW, tileH), *pix, pix->rect());
                        }
                    }
                } else {
                    const qreal scale =
                        std::max(mRect.width() / pix->width(), mRect.height() / pix->height());
                    const qreal visibleSrcW = mRect.width() / scale;
                    const qreal visibleSrcH = mRect.height() / scale;
                    const qreal maxOffX = std::max(0.0, pix->width() - visibleSrcW);
                    const qreal maxOffY = std::max(0.0, pix->height() - visibleSrcH);
                    const qreal srcX = std::clamp(maxOffX * m.cropGravity.x(), 0.0, maxOffX);
                    const qreal srcY = std::clamp(maxOffY * m.cropGravity.y(), 0.0, maxOffY);

                    const QRectF srcRect(srcX, srcY, visibleSrcW, visibleSrcH);
                    painter.drawPixmap(mRect, *pix, srcRect);
                }
            }
        }

        if (isDropTarget) {
            painter.fillRect(mRect, QColor(0x38, 0xBD, 0xF8, 80));
        }

        const bool compactMonitor = (mRect.height() < 78.0 || mRect.width() < 130.0);
        const QString displayName = m.name.isEmpty() ? QStringLiteral("Display-1") : m.name;
        if (compactMonitor) {
            QFont font = painter.font();
            font.setPixelSize(11);
            font.setBold(true);
            painter.setFont(font);
            const QFontMetrics fm(font);

            const QString resStr =
                QStringLiteral("%1x%2").arg(m.geometry.width()).arg(m.geometry.height());
            const QString combined = (mRect.width() >= 96.0)
                                         ? QStringLiteral("%1 | %2").arg(displayName, resStr)
                                         : resStr;

            const qreal maxBadgeW = std::max(24.0, mRect.width() - 6.0);
            const qreal badgeW =
                std::min(maxBadgeW, static_cast<qreal>(fm.horizontalAdvance(combined) + 10));
            const qreal badgeH = std::max(18.0, static_cast<qreal>(fm.height() + 4));
            const QRectF badgeRect(mRect.left() + 3.0, mRect.top() + 3.0, badgeW, badgeH);

            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0x0B, 0x0E, 0x14, 225));
            painter.drawRoundedRect(badgeRect, 3.0, 3.0);

            painter.setPen(isSelected ? QColor(0x38, 0xBD, 0xF8) : QColor(0xF1, 0xF5, 0xF9));
            const QString elided =
                fm.elidedText(combined, Qt::ElideRight, static_cast<int>(badgeW - 6));
            painter.drawText(badgeRect.adjusted(3, 0, -3, 0), Qt::AlignLeft | Qt::AlignVCenter,
                             elided);
        } else {
            QFont titleFont = painter.font();
            titleFont.setPixelSize(12);
            titleFont.setBold(true);
            const QFontMetrics fmTitle(titleFont);

            QFont subFont = painter.font();
            subFont.setPixelSize(11);
            subFont.setBold(false);
            const QFontMetrics fmSub(subFont);

            const QString title = QStringLiteral("%1 (%2x%3)")
                                      .arg(displayName)
                                      .arg(m.geometry.width())
                                      .arg(m.geometry.height());
            const QString subtitle = QStringLiteral("%1 Hz | %2x")
                                         .arg(QString::number(m.refreshRateHz, 'f', 0))
                                         .arg(QString::number(m.devicePixelRatio, 'f', 1));

            const qreal maxBadgeW = std::max(28.0, mRect.width() - 8.0);
            const qreal badgeW =
                std::min(maxBadgeW, static_cast<qreal>(std::max(fmTitle.horizontalAdvance(title),
                                                                fmSub.horizontalAdvance(subtitle)) +
                                                       14));
            const qreal titleH = static_cast<qreal>(fmTitle.height());
            const qreal subH = static_cast<qreal>(fmSub.height());
            const qreal badgeH = titleH + subH + 8.0;
            const QRectF badgeRect(mRect.left() + 4.0, mRect.top() + 4.0, badgeW, badgeH);

            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0x0B, 0x0E, 0x14, 225));
            painter.drawRoundedRect(badgeRect, 4.0, 4.0);

            painter.setFont(titleFont);
            painter.setPen(isSelected ? QColor(0x38, 0xBD, 0xF8) : QColor(0xF1, 0xF5, 0xF9));
            const QString elidedTitle =
                fmTitle.elidedText(title, Qt::ElideRight, static_cast<int>(badgeW - 10));
            painter.drawText(
                QRectF(badgeRect.left() + 5.0, badgeRect.top() + 3.0, badgeW - 10.0, titleH),
                Qt::AlignLeft | Qt::AlignVCenter, elidedTitle);

            painter.setFont(subFont);
            painter.setPen(QColor(0x94, 0xA3, 0xB8));
            const QString elidedSub =
                fmSub.elidedText(subtitle, Qt::ElideRight, static_cast<int>(badgeW - 10));
            painter.drawText(
                QRectF(badgeRect.left() + 5.0, badgeRect.top() + 4.0 + titleH, badgeW - 10.0, subH),
                Qt::AlignLeft | Qt::AlignVCenter, elidedSub);
        }

        painter.restore();

        QColor strokeColor;
        qreal strokeWidth = 1.5;

        if (isDropTarget) {
            strokeColor = QColor(0x38, 0xBD, 0xF8);
            strokeWidth = 3.0;
        } else if (isSelected) {
            strokeColor = QColor(0x38, 0xBD, 0xF8);
            strokeWidth = 2.0;
        } else if (isHovered) {
            strokeColor = QColor(0x94, 0xA3, 0xB8);
            strokeWidth = 1.6;
        } else {
            strokeColor = QColor(0x33, 0x41, 0x55);
        }

        painter.setPen(QPen(strokeColor, strokeWidth));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(mRect, 6.0, 6.0);

        if (isFocusedMonitor) {
            painter.setPen(QPen(QColor(0x38, 0xBD, 0xF8), 2.0, Qt::DotLine));
            painter.drawRoundedRect(mRect.adjusted(-3.0, -3.0, 3.0, 3.0), 8.0, 8.0);
        }
    }
}

void MonitorTopologyCanvas::drawSpanOverlay(QPainter &painter)
{
    const QRectF spanRect = mapToCanvas(m_virtualBoundingBox);

    const QPen pen(QColor(249, 226, 175, 220), 2.0, Qt::DashLine);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(spanRect.adjusted(-3, -3, 3, 3), 8.0, 8.0);

    if (height() >= 150) {
        QFont font = painter.font();
        font.setPixelSize(11);
        font.setBold(true);
        painter.setFont(font);

        const QString text = QStringLiteral("SPAN: %1 x %2")
                                 .arg(m_virtualBoundingBox.width())
                                 .arg(m_virtualBoundingBox.height());
        const QFontMetrics fm(font);
        const int w = fm.horizontalAdvance(text) + 16;
        const int h = std::max(22, fm.height() + 6);
        const QRectF badgeRect(spanRect.left() + 6, spanRect.bottom() - h - 4, w, h);

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(249, 226, 175, 235));
        painter.drawRoundedRect(badgeRect, 4.0, 4.0);

        painter.setPen(QColor(0x0B, 0x0E, 0x14));
        painter.drawText(badgeRect, Qt::AlignCenter, text);
    }
}

void MonitorTopologyCanvas::drawGravityHandles(QPainter &painter)
{
    const bool compact = (height() < 150);

    if (m_spanMode) {
        const QRectF hRect = getSpanGravityHandleRect();
        const QPointF center = hRect.center();
        const qreal r = compact ? 7.0 : 11.0;

        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);

        painter.setPen(QPen(QColor(249, 226, 175, 240), 1.8));
        painter.setBrush(QColor(249, 226, 175, 60));
        painter.drawEllipse(center, r, r);

        painter.drawLine(QPointF(center.x() - r - 3, center.y()),
                         QPointF(center.x() + r + 3, center.y()));
        painter.drawLine(QPointF(center.x(), center.y() - r - 3),
                         QPointF(center.x(), center.y() + r + 3));

        painter.setBrush(QColor(249, 226, 175));
        painter.drawEllipse(center, 2.5, 2.5);

        if (!compact) {
            const QString readout = QStringLiteral("Span Gravity: (%1, %2)")
                                        .arg(QString::number(m_spanCropGravity.x(), 'f', 2))
                                        .arg(QString::number(m_spanCropGravity.y(), 'f', 2));
            QFont font = painter.font();
            font.setPixelSize(11);
            painter.setFont(font);
            const QFontMetrics fm(font);
            const int bw = fm.horizontalAdvance(readout) + 14;
            const int bh = std::max(22, fm.height() + 6);
            const QRectF rRect(center.x() + 14, center.y() - bh * 0.5, bw, bh);

            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0x0B, 0x0E, 0x14, 230));
            painter.drawRoundedRect(rRect, 4.0, 4.0);
            painter.setPen(QColor(249, 226, 175));
            painter.drawText(rRect, Qt::AlignCenter, readout);
        }

        painter.restore();
    } else {
        for (int i = 0; i < m_monitors.size(); ++i) {
            const auto &m = m_monitors[i];
            if (!m_selectedMonitors.contains(m.name)) {
                continue;
            }

            const QRectF hRect = getCropGravityHandleRect(i);
            const QPointF center = hRect.center();
            const qreal r = compact ? 6.0 : 10.0;

            painter.save();
            painter.setRenderHint(QPainter::Antialiasing, true);

            painter.setPen(QPen(QColor(0x38, 0xBD, 0xF8, 240), 1.8));
            painter.setBrush(QColor(0x38, 0xBD, 0xF8, 70));
            painter.drawEllipse(center, r, r);

            painter.drawLine(QPointF(center.x() - r - 3, center.y()),
                             QPointF(center.x() + r + 3, center.y()));
            painter.drawLine(QPointF(center.x(), center.y() - r - 3),
                             QPointF(center.x(), center.y() + r + 3));

            painter.setBrush(QColor(0xF1, 0xF5, 0xF9));
            painter.drawEllipse(center, 2.5, 2.5);

            if (!compact) {
                const QString readout = QStringLiteral("(%1, %2)")
                                            .arg(QString::number(m.cropGravity.x(), 'f', 2))
                                            .arg(QString::number(m.cropGravity.y(), 'f', 2));
                QFont font = painter.font();
                font.setPixelSize(11);
                painter.setFont(font);
                const QFontMetrics fm(font);
                const int bw = fm.horizontalAdvance(readout) + 12;
                const int bh = std::max(22, fm.height() + 6);
                const QRectF rRect(center.x() + 14, center.y() - bh * 0.5, bw, bh);

                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(0x0B, 0x0E, 0x14, 230));
                painter.drawRoundedRect(rRect, 4.0, 4.0);
                painter.setPen(QColor(0xF1, 0xF5, 0xF9));
                painter.drawText(rRect, Qt::AlignCenter, readout);
            }

            painter.restore();
        }
    }
}

void MonitorTopologyCanvas::drawToolbarOverlay(QPainter &painter)
{
    if (height() < 150) {
        QFont font = painter.font();
        font.setPixelSize(11);
        font.setBold(true);
        painter.setFont(font);

        const QString badgeText =
            QStringLiteral("%1 Display(s) | %2x%3 | %4")
                .arg(m_monitors.size())
                .arg(m_virtualBoundingBox.width())
                .arg(m_virtualBoundingBox.height())
                .arg(m_spanMode ? QStringLiteral("SPAN") : QStringLiteral("PER-MONITOR"));
        const QFontMetrics fm(font);
        const qreal bw = std::min(static_cast<qreal>(width() - 16),
                                  static_cast<qreal>(fm.horizontalAdvance(badgeText) + 16));
        const qreal bh = std::max(22.0, static_cast<qreal>(fm.height() + 6));
        const QRectF pillRect(8.0, 6.0, bw, bh);

        painter.setPen(QPen(QColor(0x33, 0x41, 0x55, 200), 1.0));
        painter.setBrush(QColor(0x14, 0x17, 0x1F, 225));
        painter.drawRoundedRect(pillRect, 5.0, 5.0);

        painter.setPen(m_spanMode ? QColor(249, 226, 175) : QColor(0x94, 0xA3, 0xB8));
        painter.drawText(pillRect.adjusted(6, 0, -6, 0), Qt::AlignLeft | Qt::AlignVCenter,
                         fm.elidedText(badgeText, Qt::ElideRight, static_cast<int>(bw - 12)));
        return;
    }

    const QRectF barRect = toolbarBarRect();
    painter.setPen(QPen(QColor(0x33, 0x41, 0x55), 1.0));
    painter.setBrush(QColor(0x14, 0x17, 0x1F, 230));
    painter.drawRoundedRect(barRect, 6.0, 6.0);

    QFont font = painter.font();
    font.setPixelSize(11);
    font.setBold(true);
    painter.setFont(font);
    const QFontMetrics fm(font);

    const QRectF modeRect = modePillRect();
    const QRectF snapRect = snapPillRect();
    const qreal pillsLeft = snapRect.isEmpty() ? modeRect.left() : snapRect.left();

    const QString statusText = QStringLiteral("Displays: %1 | Canvas: %2 x %3")
                                   .arg(m_monitors.size())
                                   .arg(m_virtualBoundingBox.width())
                                   .arg(m_virtualBoundingBox.height());
    const qreal statusMaxW = std::max(32.0, pillsLeft - barRect.left() - 20.0);
    painter.setPen(QColor(0xF1, 0xF5, 0xF9));
    painter.drawText(QRectF(barRect.left() + 12.0, barRect.top(), statusMaxW, barRect.height()),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     fm.elidedText(statusText, Qt::ElideRight, static_cast<int>(statusMaxW)));

    if (!snapRect.isEmpty()) {
        if (m_snapEnabled) {
            painter.setPen(QPen(QColor(0x38, 0xBD, 0xF8), 1.2));
            painter.setBrush(QColor(0x38, 0xBD, 0xF8, 36));
            painter.drawRoundedRect(snapRect, 5.0, 5.0);
            painter.setPen(QColor(0xF1, 0xF5, 0xF9));
        } else {
            painter.setPen(QPen(QColor(0x47, 0x55, 0x69), 1.0));
            painter.setBrush(QColor(0x18, 0x1B, 0x22));
            painter.drawRoundedRect(snapRect, 5.0, 5.0);
            painter.setPen(QColor(0x94, 0xA3, 0xB8));
        }
        painter.drawText(snapRect, Qt::AlignCenter,
                         m_snapEnabled ? QStringLiteral("SNAP: ON") : QStringLiteral("SNAP: OFF"));
    }

    if (m_spanMode) {
        painter.setPen(QPen(QColor(249, 226, 175), 1.2));
        painter.setBrush(QColor(249, 226, 175, 42));
        painter.drawRoundedRect(modeRect, 5.0, 5.0);
        painter.setPen(QColor(249, 226, 175));
    } else {
        painter.setPen(QPen(QColor(0x38, 0xBD, 0xF8), 1.2));
        painter.setBrush(QColor(0x38, 0xBD, 0xF8, 42));
        painter.drawRoundedRect(modeRect, 5.0, 5.0);
        painter.setPen(QColor(0xF1, 0xF5, 0xF9));
    }
    const QString modeText =
        m_spanMode ? QStringLiteral("MODE: DESKTOP SPAN") : QStringLiteral("MODE: PER-MONITOR");
    painter.drawText(modeRect, Qt::AlignCenter, modeText);
}

void MonitorTopologyCanvas::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);

    if (event->button() == Qt::MiddleButton) {
        m_dragMode = DragMode::Pan;
        m_dragStartPos = event->position();
        m_currentMousePos = event->position();
        m_dragStartPan = m_panOffset;
        setCursor(Qt::ClosedHandCursor);
        return;
    }

    if (event->button() == Qt::LeftButton) {
        m_dragStartPos = event->position();
        m_currentMousePos = event->position();
        m_dragMoved = false;

        const QRectF modeRect = modePillRect();
        if (!modeRect.isEmpty() && modeRect.contains(m_dragStartPos)) {
            setDesktopSpanMode(!m_spanMode);
            return;
        }
        const QRectF snapRect = snapPillRect();
        if (!snapRect.isEmpty() && snapRect.contains(m_dragStartPos)) {
            setSnapEnabled(!m_snapEnabled);
            return;
        }

        if (m_spanMode) {
            const QRectF sHandle = getSpanGravityHandleRect();
            if (sHandle.contains(m_dragStartPos)) {
                m_dragMode = DragMode::SpanCropGravityHandle;
                return;
            }
        } else {
            for (int i = 0; i < m_monitors.size(); ++i) {
                if (m_selectedMonitors.contains(m_monitors[i].name)) {
                    const QRectF hRect = getCropGravityHandleRect(i);
                    if (hRect.contains(m_dragStartPos)) {
                        m_dragMode = DragMode::CropGravityHandle;
                        m_activeDragMonitorIndex = i;
                        return;
                    }
                }
            }
        }

        const int clickedIdx = findMonitorAtCanvasPos(m_dragStartPos);
        if (clickedIdx >= 0) {
            const QString name = m_monitors[clickedIdx].name;
            const bool additive = (event->modifiers() & Qt::ControlModifier) ||
                                  (event->modifiers() & Qt::ShiftModifier);
            m_focusedMonitorIndex = clickedIdx;
            if (additive || !m_selectedMonitors.contains(name)) {
                selectMonitor(name, additive);
            }

            if (m_selectedMonitors.contains(name)) {
                m_dragMode = DragMode::MonitorMove;
                m_activeDragMonitorIndex = clickedIdx;
                m_dragStartScale = std::max(1e-4, m_canvasScale);
                m_dragInitialGeometries.clear();
                for (const auto &m : m_monitors) {
                    if (m_selectedMonitors.contains(m.name)) {
                        m_dragInitialGeometries.insert(m.name, m.geometry);
                    }
                }
            }
        } else {
            if (event->modifiers() & Qt::AltModifier) {
                m_dragMode = DragMode::Pan;
                m_dragStartPan = m_panOffset;
                setCursor(Qt::ClosedHandCursor);
            } else {
                m_dragMode = DragMode::SelectionBox;
            }
        }
    }
}

void MonitorTopologyCanvas::mouseMoveEvent(QMouseEvent *event)
{
    m_currentMousePos = event->position();

    if (m_dragMode == DragMode::Pan) {
        m_panOffset = m_dragStartPan + (m_currentMousePos - m_dragStartPos);
        recalculateLayoutTransform();
        update();
        return;
    }

    if (m_dragMode == DragMode::SelectionBox) {
        if (!m_dragMoved && (m_currentMousePos - m_dragStartPos).manhattanLength() >= 4.0) {
            m_dragMoved = true;
        }
        if (m_dragMoved) {
            const QRectF box = QRectF(m_dragStartPos, m_currentMousePos).normalized();
            QSet<QString> nextSel;
            if (event->modifiers() & Qt::ControlModifier) {
                nextSel = m_selectedMonitors;
            }
            for (int i = 0; i < m_monitors.size(); ++i) {
                if (getMonitorCanvasRect(i).intersects(box)) {
                    nextSel.insert(m_monitors[i].name);
                }
            }
            if (nextSel != m_selectedMonitors) {
                m_selectedMonitors = std::move(nextSel);
                emit monitorSelectionChanged(selectedMonitorNames());
            }
            update();
        }
        return;
    }

    if (m_dragMode == DragMode::MonitorMove && m_activeDragMonitorIndex >= 0 &&
        m_activeDragMonitorIndex < m_monitors.size()) {
        if (!m_dragMoved && (m_currentMousePos - m_dragStartPos).manhattanLength() >= 4.0) {
            m_dragMoved = true;
        }
        if (!m_dragMoved) {
            return;
        }

        const int rawDx = static_cast<int>(
            std::lround((m_currentMousePos.x() - m_dragStartPos.x()) / m_dragStartScale));
        const int rawDy = static_cast<int>(
            std::lround((m_currentMousePos.y() - m_dragStartPos.y()) / m_dragStartScale));

        const QString anchorName = m_monitors[m_activeDragMonitorIndex].name;
        const QRect anchorInit = m_dragInitialGeometries.value(anchorName);
        QRect cand = anchorInit.translated(rawDx, rawDy);

        m_activeSnapGuides.clear();
        const bool snapActive = m_snapEnabled && !(event->modifiers() & Qt::ShiftModifier);
        if (snapActive && anchorInit.isValid()) {
            const int snapTol =
                std::max(16, static_cast<int>(std::lround(10.0 / m_dragStartScale)));
            int bestDistX = snapTol + 1;
            int snappedX = cand.x();
            int guideX = 0;
            bool hasEdgeSnapX = false;

            int bestDistY = snapTol + 1;
            int snappedY = cand.y();
            int guideY = 0;
            bool hasEdgeSnapY = false;

            for (const auto &other : m_monitors) {
                if (m_dragInitialGeometries.contains(other.name)) {
                    continue;
                }
                const QRect og = other.geometry;
                const int oRightEdge = og.right() + 1;
                const int oBottomEdge = og.bottom() + 1;

                const std::pair<int, int> xCandidates[] = {
                    {og.left(), og.left()},
                    {oRightEdge, oRightEdge},
                    {og.left() - cand.width(), og.left()},
                    {oRightEdge - cand.width(), oRightEdge},
                    {og.center().x() - cand.width() / 2, og.center().x()},
                };
                for (const auto &[tx, gx] : xCandidates) {
                    const int d = std::abs(cand.x() - tx);
                    if (d < bestDistX) {
                        bestDistX = d;
                        snappedX = tx;
                        guideX = gx;
                        hasEdgeSnapX = true;
                    }
                }

                const std::pair<int, int> yCandidates[] = {
                    {og.top(), og.top()},
                    {oBottomEdge, oBottomEdge},
                    {og.top() - cand.height(), og.top()},
                    {oBottomEdge - cand.height(), oBottomEdge},
                    {og.center().y() - cand.height() / 2, og.center().y()},
                };
                for (const auto &[ty, gy] : yCandidates) {
                    const int d = std::abs(cand.y() - ty);
                    if (d < bestDistY) {
                        bestDistY = d;
                        snappedY = ty;
                        guideY = gy;
                        hasEdgeSnapY = true;
                    }
                }
            }

            if (hasEdgeSnapX) {
                cand.moveLeft(snappedX);
                m_activeSnapGuides.push_back(SnapGuide{true, guideX});
            } else if (m_snapStepPx > 1) {
                const int gridX = static_cast<int>(
                    std::lround(static_cast<double>(cand.x()) / m_snapStepPx) * m_snapStepPx);
                cand.moveLeft(gridX);
            }

            if (hasEdgeSnapY) {
                cand.moveTop(snappedY);
                m_activeSnapGuides.push_back(SnapGuide{false, guideY});
            } else if (m_snapStepPx > 1) {
                const int gridY = static_cast<int>(
                    std::lround(static_cast<double>(cand.y()) / m_snapStepPx) * m_snapStepPx);
                cand.moveTop(gridY);
            }
        }

        const int finalDx = cand.x() - anchorInit.x();
        const int finalDy = cand.y() - anchorInit.y();
        for (auto &m : m_monitors) {
            if (m_dragInitialGeometries.contains(m.name)) {
                m.geometry = m_dragInitialGeometries.value(m.name).translated(finalDx, finalDy);
            }
        }
        setCursor(Qt::ClosedHandCursor);
        update();
        return;
    }

    if (m_dragMode == DragMode::SpanCropGravityHandle) {
        const QRectF spanRect = mapToCanvas(m_virtualBoundingBox);
        if (spanRect.width() > 1.0 && spanRect.height() > 1.0) {
            const qreal gx =
                std::clamp((m_currentMousePos.x() - spanRect.left()) / spanRect.width(), 0.0, 1.0);
            const qreal gy =
                std::clamp((m_currentMousePos.y() - spanRect.top()) / spanRect.height(), 0.0, 1.0);
            m_spanCropGravity = QPointF(gx, gy);
            emit cropGravityChanged(selectedMonitorNames(), static_cast<float>(gx),
                                    static_cast<float>(gy));
            update();
        }
        return;
    }

    if (m_dragMode == DragMode::CropGravityHandle && m_activeDragMonitorIndex >= 0 &&
        m_activeDragMonitorIndex < m_monitors.size()) {
        const QRectF mRect = getMonitorCanvasRect(m_activeDragMonitorIndex);
        if (mRect.width() > 1.0 && mRect.height() > 1.0) {
            const qreal gx =
                std::clamp((m_currentMousePos.x() - mRect.left()) / mRect.width(), 0.0, 1.0);
            const qreal gy =
                std::clamp((m_currentMousePos.y() - mRect.top()) / mRect.height(), 0.0, 1.0);
            m_monitors[m_activeDragMonitorIndex].cropGravity = QPointF(gx, gy);
            emit cropGravityChanged({m_monitors[m_activeDragMonitorIndex].name},
                                    static_cast<float>(gx), static_cast<float>(gy));
            update();
        }
        return;
    }

    const QRectF modeRect = modePillRect();
    const QRectF snapRect = snapPillRect();
    const bool hoveringPill = (!modeRect.isEmpty() && modeRect.contains(m_currentMousePos)) ||
                              (!snapRect.isEmpty() && snapRect.contains(m_currentMousePos));

    bool hoveringHandle = false;
    if (!hoveringPill) {
        if (m_spanMode) {
            hoveringHandle = getSpanGravityHandleRect().contains(m_currentMousePos);
        } else {
            for (int i = 0; i < m_monitors.size(); ++i) {
                if (m_selectedMonitors.contains(m_monitors[i].name) &&
                    getCropGravityHandleRect(i).contains(m_currentMousePos)) {
                    hoveringHandle = true;
                    break;
                }
            }
        }
    }

    const int hoveredIdx = hoveringPill ? -1 : findMonitorAtCanvasPos(m_currentMousePos);
    const QString newHoveredName = (hoveredIdx >= 0) ? m_monitors[hoveredIdx].name : QString();

    if (hoveringPill || hoveredIdx >= 0) {
        setCursor(Qt::PointingHandCursor);
    } else if (hoveringHandle) {
        setCursor(Qt::SizeAllCursor);
    } else {
        setCursor(Qt::ArrowCursor);
    }

    if (newHoveredName != m_hoveredMonitorName || hoveringHandle != m_isHoveringGravityHandle) {
        m_hoveredMonitorName = newHoveredName;
        m_isHoveringGravityHandle = hoveringHandle;
        update();
    }
}

void MonitorTopologyCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    const DragMode prevMode = m_dragMode;
    const bool moved = m_dragMoved;
    const int dragIdx = m_activeDragMonitorIndex;

    m_dragMode = DragMode::None;
    m_activeDragMonitorIndex = -1;
    m_dragMoved = false;
    m_dragInitialGeometries.clear();
    m_activeSnapGuides.clear();

    if (prevMode == DragMode::MonitorMove) {
        if (moved) {
            recalculateLayoutTransform();
            emit spanRectsCalculated(calculateSpanRectangles().normalizedRects);
            emit monitorLayoutChanged();
        } else if (event && event->button() == Qt::LeftButton &&
                   !(event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) &&
                   dragIdx >= 0 && dragIdx < m_monitors.size() && m_selectedMonitors.size() > 1) {
            selectMonitor(m_monitors[dragIdx].name, false);
        }
    } else if (prevMode == DragMode::SelectionBox && !moved && event &&
               event->button() == Qt::LeftButton && !(event->modifiers() & Qt::ControlModifier)) {
        clearSelection();
    }

    setCursor(Qt::ArrowCursor);
    update();
}

void MonitorTopologyCanvas::wheelEvent(QWheelEvent *event)
{
    const qreal numDegrees = event->angleDelta().y() / 8.0;
    const qreal numSteps = numDegrees / 15.0;
    const qreal factor = std::pow(1.1, numSteps);

    m_padding = std::clamp(m_padding / factor, 16.0, 120.0);
    recalculateLayoutTransform();
    update();
    event->accept();
}

void MonitorTopologyCanvas::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    recalculateLayoutTransform();
}

bool MonitorTopologyCanvas::focusNextPrevChild(bool next)
{
    if (m_monitors.size() > 1) {
        const int idx = m_focusedMonitorIndex;
        if (next && (idx < 0 || idx + 1 < m_monitors.size())) {
            return false;
        }
        if (!next && idx > 0) {
            return false;
        }
    }
    return QWidget::focusNextPrevChild(next);
}

void MonitorTopologyCanvas::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
    if (m_focusedMonitorIndex < 0 && !m_monitors.isEmpty()) {
        m_focusedMonitorIndex = 0;
        for (int i = 0; i < m_monitors.size(); ++i) {
            if (m_selectedMonitors.contains(m_monitors[i].name)) {
                m_focusedMonitorIndex = i;
                break;
            }
        }
    }
    update();
}

void MonitorTopologyCanvas::focusOutEvent(QFocusEvent *event)
{
    QWidget::focusOutEvent(event);
    update();
}

void MonitorTopologyCanvas::nudgeSelectedMonitors(int dx, int dy, bool bypassSnap)
{
    if (m_monitors.isEmpty() || (dx == 0 && dy == 0)) {
        return;
    }

    if (m_selectedMonitors.isEmpty()) {
        const int fallbackIdx =
            (m_focusedMonitorIndex >= 0 && m_focusedMonitorIndex < m_monitors.size())
                ? m_focusedMonitorIndex
                : 0;
        m_selectedMonitors.insert(m_monitors[fallbackIdx].name);
        emit monitorSelectionChanged(selectedMonitorNames());
    }

    bool changed = false;
    for (auto &m : m_monitors) {
        if (!m_selectedMonitors.contains(m.name)) {
            continue;
        }
        QRect next = m.geometry.translated(dx, dy);
        if (m_snapEnabled && !bypassSnap && m_snapStepPx > 1) {
            if (dx != 0) {
                const int snappedX = static_cast<int>(
                    std::lround(static_cast<double>(next.x()) / m_snapStepPx) * m_snapStepPx);
                next.moveLeft(snappedX);
            }
            if (dy != 0) {
                const int snappedY = static_cast<int>(
                    std::lround(static_cast<double>(next.y()) / m_snapStepPx) * m_snapStepPx);
                next.moveTop(snappedY);
            }
        }
        if (next != m.geometry) {
            m.geometry = next;
            changed = true;
        }
    }

    if (changed) {
        recalculateLayoutTransform();
        emit spanRectsCalculated(calculateSpanRectangles().normalizedRects);
        emit monitorLayoutChanged();
        update();
    }
}

void MonitorTopologyCanvas::keyPressEvent(QKeyEvent *event)
{
    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool shift = (mods & Qt::ShiftModifier) != 0;
    const bool ctrl = (mods & Qt::ControlModifier) != 0;
    const bool alt = (mods & Qt::AltModifier) != 0;

    switch (event->key()) {
    case Qt::Key_Tab:
    case Qt::Key_Backtab: {
        if (m_monitors.isEmpty()) {
            QWidget::keyPressEvent(event);
            return;
        }
        const int count = m_monitors.size();
        const int dir = (event->key() == Qt::Key_Backtab || shift) ? -1 : 1;
        if (m_focusedMonitorIndex < 0 || m_focusedMonitorIndex >= count) {
            m_focusedMonitorIndex = (dir > 0) ? 0 : (count - 1);
        } else {
            m_focusedMonitorIndex = (m_focusedMonitorIndex + dir + count) % count;
        }
        selectMonitor(m_monitors[m_focusedMonitorIndex].name, false);
        event->accept();
        return;
    }

    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down: {
        if (m_monitors.isEmpty()) {
            break;
        }
        const int sx = (event->key() == Qt::Key_Left)    ? -1
                       : (event->key() == Qt::Key_Right) ? 1
                                                         : 0;
        const int sy = (event->key() == Qt::Key_Up) ? -1 : (event->key() == Qt::Key_Down) ? 1 : 0;
        if (alt) {
            const qreal delta = shift ? 0.01 : 0.05;
            QPointF base = m_spanCropGravity;
            if (!m_spanMode) {
                const QStringList sel = selectedMonitorNames();
                base = sel.isEmpty() ? QPointF(0.5, 0.5) : cropGravity(sel.first());
            }
            setCropGravityForSelection(QPointF(base.x() + sx * delta, base.y() + sy * delta));
        } else {
            const int step = shift ? 4 : (m_snapEnabled ? m_snapStepPx : 16);
            nudgeSelectedMonitors(sx * step, sy * step, shift);
        }
        event->accept();
        return;
    }

    case Qt::Key_S:
    case Qt::Key_G:
        setSnapEnabled(!m_snapEnabled);
        event->accept();
        return;

    case Qt::Key_M:
        setDesktopSpanMode(!m_spanMode);
        event->accept();
        return;

    case Qt::Key_Space:
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (!m_monitors.isEmpty()) {
            const int idx =
                (m_focusedMonitorIndex >= 0 && m_focusedMonitorIndex < m_monitors.size())
                    ? m_focusedMonitorIndex
                    : 0;
            m_focusedMonitorIndex = idx;
            selectMonitor(m_monitors[idx].name, ctrl);
            event->accept();
            return;
        }
        break;

    case Qt::Key_A:
        if (ctrl) {
            selectAllMonitors();
            event->accept();
            return;
        }
        break;

    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        setCropGravityForSelection(QPointF(0.5, 0.5));
        event->accept();
        return;

    case Qt::Key_Escape:
        if (m_dragMode != DragMode::None) {
            m_dragMode = DragMode::None;
            m_activeDragMonitorIndex = -1;
            m_dragMoved = false;
            m_dragInitialGeometries.clear();
            m_activeSnapGuides.clear();
            setCursor(Qt::ArrowCursor);
            update();
        } else {
            clearSelection();
        }
        event->accept();
        return;

    case Qt::Key_Plus:
    case Qt::Key_Equal:
        m_padding = std::clamp(m_padding / 1.15, 16.0, 120.0);
        recalculateLayoutTransform();
        update();
        event->accept();
        return;

    case Qt::Key_Minus:
        m_padding = std::clamp(m_padding * 1.15, 16.0, 120.0);
        recalculateLayoutTransform();
        update();
        event->accept();
        return;

    case Qt::Key_0:
        m_padding = 40.0;
        m_panOffset = QPointF(0.0, 0.0);
        recalculateLayoutTransform();
        update();
        event->accept();
        return;

    case Qt::Key_R:
        refreshFromWayland();
        event->accept();
        return;

    default:
        break;
    }

    QWidget::keyPressEvent(event);
}

void MonitorTopologyCanvas::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls() ||
        event->mimeData()->hasFormat(QStringLiteral("application/x-waywal-wallpaper")) ||
        event->mimeData()->hasText()) {
        event->acceptProposedAction();
    }
}

void MonitorTopologyCanvas::dragMoveEvent(QDragMoveEvent *event)
{
    const QPointF pos = event->position();
    QString targetName;
    if (m_spanMode && !m_monitors.isEmpty()) {
        const QRectF spanRect = mapToCanvas(m_virtualBoundingBox);
        if (spanRect.contains(pos) || findMonitorAtCanvasPos(pos) >= 0) {
            targetName = m_monitors.first().name;
        }
    } else {
        const int idx = findMonitorAtCanvasPos(pos);
        if (idx >= 0) {
            targetName = m_monitors[idx].name;
        }
    }

    if (targetName != m_dropTargetMonitorName) {
        m_dropTargetMonitorName = targetName;
        update();
    }
    event->acceptProposedAction();
}

void MonitorTopologyCanvas::dragLeaveEvent(QDragLeaveEvent *event)
{
    Q_UNUSED(event);
    m_dropTargetMonitorName.clear();
    update();
}

void MonitorTopologyCanvas::dropEvent(QDropEvent *event)
{
    const QString wallpaperPath = extractMimeWallpaperPath(event->mimeData());
    if (wallpaperPath.isEmpty() || m_monitors.isEmpty()) {
        m_dropTargetMonitorName.clear();
        event->ignore();
        update();
        return;
    }

    if (m_spanMode) {
        assignWallpaperToSelection(wallpaperPath);
        m_dropTargetMonitorName.clear();
        event->acceptProposedAction();
        update();
        return;
    }

    const int idx = findMonitorAtCanvasPos(event->position());
    if (idx >= 0) {
        const QString target = m_monitors[idx].name;
        if (m_selectedMonitors.contains(target) && m_selectedMonitors.size() > 1) {
            assignWallpaperToSelection(wallpaperPath);
        } else {
            assignWallpaper(target, wallpaperPath);
        }
        m_dropTargetMonitorName.clear();
        event->acceptProposedAction();
    } else {
        m_dropTargetMonitorName.clear();
        event->ignore();
    }
    update();
}

} // namespace waywal::studio
