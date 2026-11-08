// SPDX-License-Identifier: GPL-3.0-or-later
#include "monitor_topology_canvas.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QGuiApplication>
#include <QScreen>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileInfo>
#include <QImageReader>
#include <QFontMetrics>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace waywal::studio {

MonitorTopologyCanvas::MonitorTopologyCanvas(QWidget *parent) : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAcceptDrops(true);
    setAttribute(Qt::WA_OpaquePaintEvent, false);

    // Initial query of connected Wayland outputs from Qt platform integration
    refreshFromWayland();
}

MonitorTopologyCanvas::~MonitorTopologyCanvas() = default;

void MonitorTopologyCanvas::refreshFromWayland()
{
    const QList<QScreen *> screens = QGuiApplication::screens();
    if (screens.isEmpty()) {
        return;
    }

    // Preserve existing settings for matching monitors
    QMap<QString, MonitorInfo> existingSettings;
    for (const auto &m : m_monitors) {
        existingSettings.insert(m.name, m);
    }

    m_monitors.clear();

    int screenIdx = 1;
    for (QScreen *screen : screens) {
        if (!screen)
            continue;

        MonitorInfo info;
        info.name = screen->name().trimmed().isEmpty() ? QStringLiteral("Display-%1").arg(screenIdx)
                                                       : screen->name();
        ++screenIdx;
        info.model = screen->model();
        info.manufacturer = screen->manufacturer();
        info.geometry = screen->geometry();
        info.physicalResolution = screen->size();
        info.devicePixelRatio = screen->devicePixelRatio();
        info.refreshRateHz = screen->refreshRate();
        info.isConnected = true;
        info.isPrimary = (screen == QGuiApplication::primaryScreen());

        // Restore customized assignments if previously set
        if (existingSettings.contains(info.name)) {
            const auto &prev = existingSettings[info.name];
            info.assignedWallpaperPath = prev.assignedWallpaperPath;
            info.wallpaperPixmap = prev.wallpaperPixmap;
            info.scalingMode = prev.scalingMode;
            info.cropGravity = prev.cropGravity;
        }

        m_monitors.append(info);
    }

    // Automatically select the primary monitor if selection is empty
    if (m_selectedMonitors.isEmpty() && !m_monitors.isEmpty()) {
        m_selectedMonitors.insert(m_monitors.first().name);
    }

    recalculateLayoutTransform();
    emit outputsRefreshed(m_monitors.size());
    emit monitorSelectionChanged(selectedMonitorNames());
    emit spanRectsCalculated(calculateSpanRectangles().normalizedRects);
    update();
}

void MonitorTopologyCanvas::updateOutputsFromJson(const QByteArray &jsonBytes)
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(jsonBytes, &err);
    if (err.error == QJsonParseError::NoError && doc.isObject()) {
        updateOutputsFromJson(doc.object());
    }
}

void MonitorTopologyCanvas::updateOutputsFromJson(const QJsonObject &rootObj)
{
    QJsonArray outputsArray;
    if (rootObj.contains("outputs") && rootObj["outputs"].isArray()) {
        outputsArray = rootObj["outputs"].toArray();
    } else if (rootObj.contains("screens") && rootObj["screens"].isArray()) {
        outputsArray = rootObj["screens"].toArray();
    }

    if (outputsArray.isEmpty()) {
        return;
    }

    QMap<QString, MonitorInfo> existingSettings;
    for (const auto &m : m_monitors) {
        existingSettings.insert(m.name, m);
    }

    m_monitors.clear();

    for (const auto &val : outputsArray) {
        if (!val.isObject())
            continue;
        QJsonObject obj = val.toObject();

        MonitorInfo info;
        info.name = obj.value("name").toString();
        info.model = obj.value("model").toString();
        info.manufacturer = obj.value("make").toString();

        int x = obj.value("x").toInt(0);
        int y = obj.value("y").toInt(0);
        int w = obj.value("width").toInt(1920);
        int h = obj.value("height").toInt(1080);
        info.geometry = QRect(x, y, w, h);
        info.physicalResolution = QSize(w, h);

        info.devicePixelRatio = obj.value("scale").toDouble(1.0);
        if (obj.contains("refresh_mhz")) {
            info.refreshRateHz = obj.value("refresh_mhz").toDouble(60000.0) / 1000.0;
        } else if (obj.contains("refresh_rate")) {
            info.refreshRateHz = obj.value("refresh_rate").toDouble(60.0);
        }

        info.isConnected = true;
        info.isPrimary = obj.value("primary").toBool(false);

        if (existingSettings.contains(info.name)) {
            const auto &prev = existingSettings[info.name];
            info.assignedWallpaperPath = prev.assignedWallpaperPath;
            info.wallpaperPixmap = prev.wallpaperPixmap;
            info.scalingMode = prev.scalingMode;
            info.cropGravity = prev.cropGravity;
        }

        m_monitors.append(info);
    }

    if (m_selectedMonitors.isEmpty() && !m_monitors.isEmpty()) {
        m_selectedMonitors.insert(m_monitors.first().name);
    }

    recalculateLayoutTransform();
    emit outputsRefreshed(m_monitors.size());
    emit monitorSelectionChanged(selectedMonitorNames());
    emit spanRectsCalculated(calculateSpanRectangles().normalizedRects);
    update();
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
    return m_selectedMonitors.values();
}

void MonitorTopologyCanvas::setSelectedMonitors(const QStringList &names)
{
    m_selectedMonitors.clear();
    for (const QString &n : names) {
        if (findMonitor(n)) {
            m_selectedMonitors.insert(n);
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

    emit monitorSelectionChanged(selectedMonitorNames());
    update();
}

void MonitorTopologyCanvas::selectAllMonitors()
{
    m_selectedMonitors.clear();
    for (const auto &m : m_monitors) {
        m_selectedMonitors.insert(m.name);
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
        emit desktopSpanModeToggled(m_spanMode);
        emit spanRectsCalculated(calculateSpanRectangles().normalizedRects);
        update();
    }
}

void MonitorTopologyCanvas::setScalingMode(int mode)
{
    m_scalingMode = mode;
    QStringList targets = selectedMonitorNames();
    for (const QString &name : targets) {
        if (auto *m = findMonitor(name)) {
            m->scalingMode = mode;
        }
    }
    emit scalingModeChanged(targets, mode);
    update();
}

QPointF MonitorTopologyCanvas::cropGravity(const QString &monitorName) const
{
    if (const auto *m = findMonitor(monitorName)) {
        return m->cropGravity;
    }
    return QPointF(0.5, 0.5);
}

void MonitorTopologyCanvas::setCropGravity(const QString &monitorName, const QPointF &gravity)
{
    QPointF clamped(std::clamp(gravity.x(), 0.0, 1.0), std::clamp(gravity.y(), 0.0, 1.0));
    if (auto *m = findMonitor(monitorName)) {
        m->cropGravity = clamped;
        emit cropGravityChanged({monitorName}, static_cast<float>(clamped.x()),
                                static_cast<float>(clamped.y()));
        update();
    }
}

void MonitorTopologyCanvas::setCropGravityForSelection(const QPointF &gravity)
{
    QPointF clamped(std::clamp(gravity.x(), 0.0, 1.0), std::clamp(gravity.y(), 0.0, 1.0));
    if (m_spanMode) {
        m_spanCropGravity = clamped;
        emit cropGravityChanged(m_selectedMonitors.values(), static_cast<float>(clamped.x()),
                                static_cast<float>(clamped.y()));
    } else {
        QStringList targets = selectedMonitorNames();
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
        m->assignedWallpaperPath = filePath;
        if (!pixmap.isNull()) {
            m->wallpaperPixmap = pixmap;
        } else if (!filePath.isEmpty()) {
            m->wallpaperPixmap.load(filePath);
        } else {
            m->wallpaperPixmap = QPixmap();
        }
        emit wallpaperDropped({monitorName}, filePath);
        update();
    }
}

void MonitorTopologyCanvas::assignWallpaperToSelection(const QString &filePath,
                                                       const QPixmap &pixmap)
{
    if (m_spanMode) {
        m_spanWallpaperPath = filePath;
        if (!pixmap.isNull()) {
            m_spanWallpaperPixmap = pixmap;
        } else if (!filePath.isEmpty()) {
            m_spanWallpaperPixmap.load(filePath);
        } else {
            m_spanWallpaperPixmap = QPixmap();
        }
        emit wallpaperDropped(selectedMonitorNames(), filePath);
    } else {
        QStringList targets = selectedMonitorNames();
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

    int spanW = std::max(1, maxX - minX);
    int spanH = std::max(1, maxY - minY);
    calc.boundingBox = QRect(minX, minY, spanW, spanH);

    for (const auto &m : m_monitors) {
        qreal u_x = static_cast<qreal>(m.geometry.left() - minX) / spanW;
        qreal u_y = static_cast<qreal>(m.geometry.top() - minY) / spanH;
        qreal u_w = static_cast<qreal>(m.geometry.width()) / spanW;
        qreal u_h = static_cast<qreal>(m.geometry.height()) / spanH;

        calc.normalizedRects.insert(m.name, QRectF(u_x, u_y, u_w, u_h));
    }

    return calc;
}

void MonitorTopologyCanvas::recalculateLayoutTransform()
{
    DesktopSpanCalculation span = calculateSpanRectangles();
    m_virtualBoundingBox = span.boundingBox;

    const bool compact = (height() < 150);
    const qreal padX = compact ? 12.0 : 24.0;
    const qreal padY = compact ? 6.0 : 16.0;
    const qreal topReserve = compact ? 0.0 : 38.0;

    qreal availW = std::max(60.0, static_cast<qreal>(width()) - 2.0 * padX);
    qreal availH = std::max(36.0, static_cast<qreal>(height()) - 2.0 * padY - topReserve);

    qreal scaleX = availW / std::max(1, m_virtualBoundingBox.width());
    qreal scaleY = availH / std::max(1, m_virtualBoundingBox.height());
    m_canvasScale = std::min(scaleX, scaleY);

    qreal renderW = m_virtualBoundingBox.width() * m_canvasScale;
    qreal renderH = m_virtualBoundingBox.height() * m_canvasScale;

    m_canvasOffsetX = padX + (availW - renderW) * 0.5 - m_virtualBoundingBox.left() * m_canvasScale;
    m_canvasOffsetY =
        padY + topReserve + (availH - renderH) * 0.5 - m_virtualBoundingBox.top() * m_canvasScale;
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
    QRectF mRect = getMonitorCanvasRect(monitorIndex);
    if (mRect.isEmpty() || monitorIndex < 0 || monitorIndex >= m_monitors.size()) {
        return QRectF();
    }

    const auto &m = m_monitors[monitorIndex];
    qreal hx = mRect.left() + m.cropGravity.x() * mRect.width();
    qreal hy = mRect.top() + m.cropGravity.y() * mRect.height();
    const qreal r = 12.0;
    return QRectF(hx - r, hy - r, 2.0 * r, 2.0 * r);
}

QRectF MonitorTopologyCanvas::getSpanGravityHandleRect() const
{
    QRectF spanRect = mapToCanvas(m_virtualBoundingBox);
    qreal hx = spanRect.left() + m_spanCropGravity.x() * spanRect.width();
    qreal hy = spanRect.top() + m_spanCropGravity.y() * spanRect.height();
    const qreal r = 14.0;
    return QRectF(hx - r, hy - r, 2.0 * r, 2.0 * r);
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

    drawGravityHandles(painter);
    drawToolbarOverlay(painter);
}

void MonitorTopologyCanvas::drawGridBackground(QPainter &painter)
{
    // Dark modern canvas background (#11111b)
    painter.fillRect(rect(), QColor(17, 17, 27));

    // Subtle isometric/orthogonal dot grid
    painter.setPen(QPen(QColor(49, 50, 68, 120), 1.0));
    const int gridSize = 24;
    for (int x = 0; x < width(); x += gridSize) {
        for (int y = 0; y < height(); y += gridSize) {
            painter.drawPoint(x, y);
        }
    }
}

void MonitorTopologyCanvas::drawMonitors(QPainter &painter)
{
    DesktopSpanCalculation spanCalc = calculateSpanRectangles();

    for (int i = 0; i < m_monitors.size(); ++i) {
        const auto &m = m_monitors[i];
        QRectF mRect = getMonitorCanvasRect(i);
        bool isSelected = m_selectedMonitors.contains(m.name);
        bool isHovered = (m.name == m_hoveredMonitorName);
        bool isDropTarget = (m.name == m_dropTargetMonitorName);

        // Monitor bezel rounded rect
        QPainterPath bezelPath;
        bezelPath.addRoundedRect(mRect, 6.0, 6.0);

        painter.save();
        painter.setClipPath(bezelPath);

        // Background monitor surface
        painter.fillRect(mRect, QColor(30, 30, 46));

        // Draw assigned wallpaper preview if available
        const QPixmap *pix = nullptr;
        if (m_spanMode && !m_spanWallpaperPixmap.isNull()) {
            pix = &m_spanWallpaperPixmap;
        } else if (!m.wallpaperPixmap.isNull()) {
            pix = &m.wallpaperPixmap;
        }

        if (pix && !pix->isNull()) {
            if (m_spanMode) {
                QRectF normRect = spanCalc.normalizedRects.value(m.name, QRectF(0, 0, 1, 1));
                QRectF srcRect(normRect.x() * pix->width(), normRect.y() * pix->height(),
                               normRect.width() * pix->width(), normRect.height() * pix->height());
                painter.drawPixmap(mRect, *pix, srcRect);
            } else {
                if (m.scalingMode == WAYWAL_SCALING_FIT) {
                    qreal scale =
                        std::min(mRect.width() / pix->width(), mRect.height() / pix->height());
                    qreal w = pix->width() * scale;
                    qreal h = pix->height() * scale;
                    QRectF destRect(mRect.x() + (mRect.width() - w) * 0.5,
                                    mRect.y() + (mRect.height() - h) * 0.5, w, h);
                    painter.fillRect(mRect, Qt::black);
                    painter.drawPixmap(destRect, *pix, pix->rect());
                } else if (m.scalingMode == WAYWAL_SCALING_STRETCH) {
                    painter.drawPixmap(mRect, *pix, pix->rect());
                } else if (m.scalingMode == WAYWAL_SCALING_CENTER) {
                    qreal w = pix->width();
                    qreal h = pix->height();
                    QRectF destRect(mRect.x() + (mRect.width() - w) * 0.5,
                                    mRect.y() + (mRect.height() - h) * 0.5, w, h);
                    painter.fillRect(mRect, Qt::black);
                    painter.drawPixmap(destRect, *pix, pix->rect());
                } else {
                    qreal scale =
                        std::max(mRect.width() / pix->width(), mRect.height() / pix->height());
                    qreal visibleSrcW = mRect.width() / scale;
                    qreal visibleSrcH = mRect.height() / scale;
                    qreal maxOffX = pix->width() - visibleSrcW;
                    qreal maxOffY = pix->height() - visibleSrcH;
                    qreal srcX = std::clamp(maxOffX * m.cropGravity.x(), 0.0, maxOffX);
                    qreal srcY = std::clamp(maxOffY * m.cropGravity.y(), 0.0, maxOffY);

                    QRectF srcRect(srcX, srcY, visibleSrcW, visibleSrcH);
                    painter.drawPixmap(mRect, *pix, srcRect);
                }
            }
        }

        if (isDropTarget) {
            painter.fillRect(mRect, QColor(56, 189, 248, 80));
        }

        // Header / Identity Badge inside monitor (clipped to bezelPath)
        const bool compactMonitor = (mRect.height() < 78.0 || mRect.width() < 130.0);
        if (compactMonitor) {
            QFont font = painter.font();
            font.setPixelSize(8);
            font.setBold(true);
            painter.setFont(font);
            QFontMetrics fm(font);

            const QString displayName = m.name.isEmpty() ? QStringLiteral("Display-1") : m.name;
            const QString resStr =
                QStringLiteral("%1x%2").arg(m.geometry.width()).arg(m.geometry.height());
            const QString combined = (mRect.width() >= 96.0)
                                         ? QStringLiteral("%1 · %2").arg(displayName, resStr)
                                         : resStr;

            const qreal maxBadgeW = std::max(20.0, mRect.width() - 6.0);
            const qreal badgeW =
                std::min(maxBadgeW, static_cast<qreal>(fm.horizontalAdvance(combined) + 8));
            const qreal badgeH = 13.0;
            const QRectF badgeRect(mRect.left() + 3.0, mRect.top() + 2.0, badgeW, badgeH);

            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(17, 17, 27, 210));
            painter.drawRoundedRect(badgeRect, 3.0, 3.0);

            painter.setPen(isSelected ? QColor(137, 180, 250) : QColor(205, 214, 244));
            const QString elided =
                fm.elidedText(combined, Qt::ElideRight, static_cast<int>(badgeW - 6));
            painter.drawText(badgeRect.adjusted(3, 0, -3, 0), Qt::AlignLeft | Qt::AlignVCenter,
                             elided);
        } else {
            QFont font = painter.font();
            font.setPixelSize(11);
            font.setBold(true);
            painter.setFont(font);

            const QString displayName = m.name.isEmpty() ? QStringLiteral("Display-1") : m.name;
            QString title = QString("%1 (%2x%3)")
                                .arg(displayName)
                                .arg(m.geometry.width())
                                .arg(m.geometry.height());
            QString subtitle = QString("%1 Hz · %2x")
                                   .arg(QString::number(m.refreshRateHz, 'f', 0))
                                   .arg(QString::number(m.devicePixelRatio, 'f', 1));

            QFontMetrics fm(font);
            qreal maxBadgeW = std::max(24.0, mRect.width() - 8.0);
            qreal badgeW =
                std::min(maxBadgeW, static_cast<qreal>(std::max(fm.horizontalAdvance(title),
                                                                fm.horizontalAdvance(subtitle)) +
                                                       12));
            qreal badgeH = 34.0;
            QRectF badgeRect(mRect.left() + 4.0, mRect.top() + 4.0, badgeW, badgeH);

            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(17, 17, 27, 200));
            painter.drawRoundedRect(badgeRect, 4.0, 4.0);

            painter.setPen(isSelected ? QColor(137, 180, 250) : QColor(205, 214, 244));
            QString elidedTitle =
                fm.elidedText(title, Qt::ElideRight, static_cast<int>(badgeW - 8));
            painter.drawText(badgeRect.adjusted(4, 2, -4, -badgeH * 0.45),
                             Qt::AlignLeft | Qt::AlignVCenter, elidedTitle);

            font.setPixelSize(9);
            font.setBold(false);
            painter.setFont(font);
            QFontMetrics fmSub(font);
            painter.setPen(QColor(166, 173, 200));
            QString elidedSub =
                fmSub.elidedText(subtitle, Qt::ElideRight, static_cast<int>(badgeW - 8));
            painter.drawText(badgeRect.adjusted(4, badgeH * 0.48, -4, -2),
                             Qt::AlignLeft | Qt::AlignVCenter, elidedSub);
        }

        painter.restore(); // End clip

        // Outline / Bezel border
        QColor strokeColor;
        qreal strokeWidth = 1.5;

        if (isDropTarget) {
            strokeColor = QColor(56, 189, 248);
            strokeWidth = 3.0;
        } else if (isSelected) {
            strokeColor = QColor(137, 180, 250);
            strokeWidth = 2.0;
        } else if (isHovered) {
            strokeColor = QColor(88, 91, 112);
            strokeWidth = 1.6;
        } else {
            strokeColor = QColor(49, 50, 68);
        }

        painter.setPen(QPen(strokeColor, strokeWidth));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(mRect, 6.0, 6.0);
    }
}

void MonitorTopologyCanvas::drawSpanOverlay(QPainter &painter)
{
    QRectF spanRect = mapToCanvas(m_virtualBoundingBox);

    QPen pen(QColor(249, 226, 175, 220), 2.0, Qt::DashLine);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(spanRect.adjusted(-3, -3, 3, 3), 8.0, 8.0);

    if (height() >= 150) {
        QFont font = painter.font();
        font.setPixelSize(10);
        font.setBold(true);
        painter.setFont(font);

        QString text = QString("SPAN: %1 x %2")
                           .arg(m_virtualBoundingBox.width())
                           .arg(m_virtualBoundingBox.height());
        QFontMetrics fm(font);
        int w = fm.horizontalAdvance(text) + 16;
        QRectF badgeRect(spanRect.left() + 6, spanRect.bottom() - 22, w, 18);

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(249, 226, 175, 230));
        painter.drawRoundedRect(badgeRect, 4.0, 4.0);

        painter.setPen(QColor(17, 17, 27));
        painter.drawText(badgeRect, Qt::AlignCenter, text);
    }
}

void MonitorTopologyCanvas::drawGravityHandles(QPainter &painter)
{
    const bool compact = (height() < 150);

    if (m_spanMode) {
        QRectF hRect = getSpanGravityHandleRect();
        QPointF center = hRect.center();
        qreal r = compact ? 7.0 : 11.0;

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
            QString readout = QString("Span Gravity: (%1, %2)")
                                  .arg(QString::number(m_spanCropGravity.x(), 'f', 2))
                                  .arg(QString::number(m_spanCropGravity.y(), 'f', 2));
            QFont font = painter.font();
            font.setPixelSize(10);
            painter.setFont(font);
            QFontMetrics fm(font);
            int bw = fm.horizontalAdvance(readout) + 12;
            QRectF rRect(center.x() + 14, center.y() - 10, bw, 20);

            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(17, 17, 27, 220));
            painter.drawRoundedRect(rRect, 4.0, 4.0);
            painter.setPen(QColor(249, 226, 175));
            painter.drawText(rRect, Qt::AlignCenter, readout);
        }

        painter.restore();
    } else {
        for (int i = 0; i < m_monitors.size(); ++i) {
            const auto &m = m_monitors[i];
            if (!m_selectedMonitors.contains(m.name))
                continue;

            QRectF hRect = getCropGravityHandleRect(i);
            QPointF center = hRect.center();
            qreal r = compact ? 6.0 : 10.0;

            painter.save();
            painter.setRenderHint(QPainter::Antialiasing, true);

            painter.setPen(QPen(QColor(137, 180, 250, 240), 1.8));
            painter.setBrush(QColor(137, 180, 250, 70));
            painter.drawEllipse(center, r, r);

            painter.drawLine(QPointF(center.x() - r - 3, center.y()),
                             QPointF(center.x() + r + 3, center.y()));
            painter.drawLine(QPointF(center.x(), center.y() - r - 3),
                             QPointF(center.x(), center.y() + r + 3));

            painter.setBrush(QColor(255, 255, 255));
            painter.drawEllipse(center, 2.5, 2.5);

            if (!compact) {
                QString readout = QString("(%1, %2)")
                                      .arg(QString::number(m.cropGravity.x(), 'f', 2))
                                      .arg(QString::number(m.cropGravity.y(), 'f', 2));
                QFont font = painter.font();
                font.setPixelSize(10);
                painter.setFont(font);
                QFontMetrics fm(font);
                int bw = fm.horizontalAdvance(readout) + 10;
                QRectF rRect(center.x() + 14, center.y() - 10, bw, 20);

                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(17, 17, 27, 220));
                painter.drawRoundedRect(rRect, 4.0, 4.0);
                painter.setPen(QColor(137, 180, 250));
                painter.drawText(rRect, Qt::AlignCenter, readout);
            }

            painter.restore();
        }
    }
}

void MonitorTopologyCanvas::drawToolbarOverlay(QPainter &painter)
{
    if (height() < 150) {
        // Compact corner badge in Top-Bar mode so monitors have full vertical height
        QFont font = painter.font();
        font.setPixelSize(10);
        font.setBold(true);
        painter.setFont(font);

        QString badgeText =
            QString("%1 Display(s) · %2x%3 · %4")
                .arg(m_monitors.size())
                .arg(m_virtualBoundingBox.width())
                .arg(m_virtualBoundingBox.height())
                .arg(m_spanMode ? QStringLiteral("SPAN") : QStringLiteral("PER-MONITOR"));
        QFontMetrics fm(font);
        qreal bw = std::min(static_cast<qreal>(width() - 16),
                            static_cast<qreal>(fm.horizontalAdvance(badgeText) + 16));
        QRectF pillRect(8.0, 6.0, bw, 20.0);

        painter.setPen(QPen(QColor(49, 50, 68, 180), 1.0));
        painter.setBrush(QColor(24, 24, 37, 210));
        painter.drawRoundedRect(pillRect, 5.0, 5.0);

        painter.setPen(m_spanMode ? QColor(249, 226, 175) : QColor(166, 173, 200));
        painter.drawText(pillRect.adjusted(6, 0, -6, 0), Qt::AlignLeft | Qt::AlignVCenter,
                         fm.elidedText(badgeText, Qt::ElideRight, static_cast<int>(bw - 12)));
        return;
    }

    // Expanded Multi-Monitor perspective mode: full top control bar
    QRectF barRect(12, 8, width() - 24, 30);
    painter.setPen(QPen(QColor(49, 50, 68), 1.0));
    painter.setBrush(QColor(24, 24, 37, 220));
    painter.drawRoundedRect(barRect, 6.0, 6.0);

    QFont font = painter.font();
    font.setPixelSize(11);
    font.setBold(true);
    painter.setFont(font);

    QString statusText = QString("Wayland Topology: %1 Display(s) · Bounding Canvas: %2 x %3")
                             .arg(m_monitors.size())
                             .arg(m_virtualBoundingBox.width())
                             .arg(m_virtualBoundingBox.height());
    painter.setPen(QColor(205, 214, 244));
    painter.drawText(barRect.adjusted(12, 0, -180, 0), Qt::AlignLeft | Qt::AlignVCenter,
                     statusText);

    QRectF pillRect(barRect.right() - 164, barRect.top() + 4, 156, 22);
    if (m_spanMode) {
        painter.setPen(QPen(QColor(249, 226, 175), 1.0));
        painter.setBrush(QColor(249, 226, 175, 40));
    } else {
        painter.setPen(QPen(QColor(137, 180, 250), 1.0));
        painter.setBrush(QColor(137, 180, 250, 40));
    }
    painter.drawRoundedRect(pillRect, 5.0, 5.0);

    painter.setPen(m_spanMode ? QColor(249, 226, 175) : QColor(137, 180, 250));
    QString modeText = m_spanMode ? "MODE: DESKTOP SPAN" : "MODE: PER-MONITOR";
    painter.drawText(pillRect, Qt::AlignCenter, modeText);
}

void MonitorTopologyCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragStartPos = event->position();

        // Check if clicked the Mode toggle pill in top toolbar (when in expanded mode)
        if (height() >= 150) {
            QRectF pillRect(width() - 12 - 164, 12, 156, 22);
            if (pillRect.contains(m_dragStartPos)) {
                setDesktopSpanMode(!m_spanMode);
                return;
            }
        }

        // 1. Check if clicked Crop Gravity handle
        if (m_spanMode) {
            QRectF sHandle = getSpanGravityHandleRect();
            if (sHandle.contains(m_dragStartPos)) {
                m_dragMode = DragMode::SpanCropGravityHandle;
                return;
            }
        } else {
            for (int i = 0; i < m_monitors.size(); ++i) {
                if (m_selectedMonitors.contains(m_monitors[i].name)) {
                    QRectF hRect = getCropGravityHandleRect(i);
                    if (hRect.contains(m_dragStartPos)) {
                        m_dragMode = DragMode::CropGravityHandle;
                        m_activeDragMonitorIndex = i;
                        return;
                    }
                }
            }
        }

        // 2. Check if clicked a monitor
        int clickedIdx = findMonitorAtCanvasPos(m_dragStartPos);
        if (clickedIdx >= 0) {
            const QString &name = m_monitors[clickedIdx].name;
            bool additive = (event->modifiers() & Qt::ControlModifier) ||
                            (event->modifiers() & Qt::ShiftModifier);
            selectMonitor(name, additive);
            m_activeDragMonitorIndex = clickedIdx;
        } else {
            // Clicked empty background
            if (!(event->modifiers() & Qt::ControlModifier)) {
                clearSelection();
            }
        }
    }
}

void MonitorTopologyCanvas::mouseMoveEvent(QMouseEvent *event)
{
    m_currentMousePos = event->position();

    if (m_dragMode == DragMode::SpanCropGravityHandle) {
        QRectF spanRect = mapToCanvas(m_virtualBoundingBox);
        if (spanRect.width() > 1.0 && spanRect.height() > 1.0) {
            qreal gx =
                std::clamp((m_currentMousePos.x() - spanRect.left()) / spanRect.width(), 0.0, 1.0);
            qreal gy =
                std::clamp((m_currentMousePos.y() - spanRect.top()) / spanRect.height(), 0.0, 1.0);
            m_spanCropGravity = QPointF(gx, gy);
            emit cropGravityChanged(m_selectedMonitors.values(), static_cast<float>(gx),
                                    static_cast<float>(gy));
            update();
        }
        return;
    }

    if (m_dragMode == DragMode::CropGravityHandle && m_activeDragMonitorIndex >= 0) {
        QRectF mRect = getMonitorCanvasRect(m_activeDragMonitorIndex);
        if (mRect.width() > 1.0 && mRect.height() > 1.0) {
            qreal gx = std::clamp((m_currentMousePos.x() - mRect.left()) / mRect.width(), 0.0, 1.0);
            qreal gy = std::clamp((m_currentMousePos.y() - mRect.top()) / mRect.height(), 0.0, 1.0);
            m_monitors[m_activeDragMonitorIndex].cropGravity = QPointF(gx, gy);
            emit cropGravityChanged({m_monitors[m_activeDragMonitorIndex].name},
                                    static_cast<float>(gx), static_cast<float>(gy));
            update();
        }
        return;
    }

    // Hover detection for cursor update
    bool hoveringHandle = false;
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

    int hoveredIdx = findMonitorAtCanvasPos(m_currentMousePos);
    QString newHoveredName = (hoveredIdx >= 0) ? m_monitors[hoveredIdx].name : QString();

    if (hoveringHandle) {
        setCursor(Qt::SizeAllCursor);
    } else if (hoveredIdx >= 0) {
        setCursor(Qt::PointingHandCursor);
    } else {
        setCursor(Qt::ArrowCursor);
    }

    if (newHoveredName != m_hoveredMonitorName) {
        m_hoveredMonitorName = newHoveredName;
        update();
    }
}

void MonitorTopologyCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
    m_dragMode = DragMode::None;
    m_activeDragMonitorIndex = -1;
    update();
}

void MonitorTopologyCanvas::wheelEvent(QWheelEvent *event)
{
    // Smooth zoom in/out with mouse wheel
    qreal numDegrees = event->angleDelta().y() / 8.0;
    qreal numSteps = numDegrees / 15.0;
    qreal factor = std::pow(1.1, numSteps);

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

void MonitorTopologyCanvas::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls() ||
        event->mimeData()->hasFormat("application/x-waywal-wallpaper") ||
        event->mimeData()->hasText()) {
        event->acceptProposedAction();
    }
}

void MonitorTopologyCanvas::dragMoveEvent(QDragMoveEvent *event)
{
    QPointF pos = event->position();
    int idx = findMonitorAtCanvasPos(pos);
    QString targetName = (idx >= 0) ? m_monitors[idx].name : QString();

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
    QString wallpaperPath;
    if (event->mimeData()->hasUrls() && !event->mimeData()->urls().isEmpty()) {
        wallpaperPath = event->mimeData()->urls().first().toLocalFile();
    } else if (event->mimeData()->hasText()) {
        wallpaperPath = event->mimeData()->text().trimmed();
    }

    if (wallpaperPath.isEmpty()) {
        event->ignore();
        return;
    }

    QPixmap pix;
    pix.load(wallpaperPath);

    if (m_spanMode) {
        // Desktop Span Mode: assign panoramic wallpaper to all monitors
        assignWallpaperToSelection(wallpaperPath, pix);
    } else {
        int idx = findMonitorAtCanvasPos(event->position());
        if (idx >= 0) {
            QString target = m_monitors[idx].name;
            if (m_selectedMonitors.contains(target) && m_selectedMonitors.size() > 1) {
                // If the target is part of a multi-selection, assign to all selected monitors
                assignWallpaperToSelection(wallpaperPath, pix);
            } else {
                assignWallpaper(target, wallpaperPath, pix);
            }
        }
    }

    m_dropTargetMonitorName.clear();
    event->acceptProposedAction();
    update();
}

} // namespace waywal::studio
