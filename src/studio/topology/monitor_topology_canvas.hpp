// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_MONITOR_TOPOLOGY_CANVAS_HPP
#define WAYWAL_STUDIO_MONITOR_TOPOLOGY_CANVAS_HPP

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QPixmap>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QSet>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

class QProcess;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "waywal/types.h"
#pragma GCC diagnostic pop

namespace waywal::studio {

/**
 * @brief Representation of a connected physical or logical Wayland monitor.
 */
struct MonitorInfo {
    QString name;                ///< e.g. "DP-1", "HDMI-A-1"
    QString model;               ///< Display model name
    QString manufacturer;        ///< Display manufacturer
    QRect geometry;              ///< Compositor coordinates (X, Y, W, H)
    QSize physicalResolution;    ///< Native hardware resolution
    qreal devicePixelRatio{1.0}; ///< Fractional scale factor (e.g. 1.0, 1.25, 1.5)
    qreal refreshRateHz{60.0};   ///< Refresh rate in Hz (e.g. 144.0)
    bool isConnected{true};
    bool isPrimary{false};

    // User assignment & transform settings
    QString assignedWallpaperPath;
    QPixmap wallpaperPixmap;
    QSize wallpaperSourceSize;
    int scalingMode{WAYWAL_SCALING_FILL}; ///< waywal_scaling_mode_t
    QPointF cropGravity{0.5, 0.5};        ///< Normalized focal anchor (gx, gy) in [0.0, 1.0]^2
};

/**
 * @brief Result of desktop span bounding calculation across all monitors.
 */
struct DesktopSpanCalculation {
    QRect boundingBox; ///< Global virtual bounding box in compositor space
    QMap<QString, QRectF>
        normalizedRects; ///< u_span_rect_i = (Xi-Xmin/Wspan, Yi-Ymin/Hspan, Wi/Wspan, Hi/Hspan)
};

/**
 * @brief Interactive 2D proportional layout canvas for multi-monitor Wayland setups.
 *
 * Features:
 * - Displays proportional layout of physical/logical monitors.
 * - Supports clicking / multi-selection (Ctrl/Shift click).
 * - Drag-and-drop wallpaper assignment from virtualized grid.
 * - Per-monitor mode vs. Desktop Span Mode toggle.
 * - Scaling mode selection (fill, fit, center, stretch, tile, span).
 * - Interactive crop gravity handle (gx, gy) with live drag reticle.
 * - Computes normalized span sub-rectangles for zero-seam panoramic wallpapers.
 */
class MonitorTopologyCanvas : public QWidget {
    Q_OBJECT

  public:
    explicit MonitorTopologyCanvas(QWidget *parent = nullptr);
    ~MonitorTopologyCanvas() override;

    // Output discovery & query synchronization
    void refreshFromWayland();
    void updateOutputsFromJson(const QByteArray &jsonBytes);
    void updateOutputsFromJson(const QJsonObject &rootObj);

    // Monitor queries
    const QList<MonitorInfo> &monitors() const { return m_monitors; }
    MonitorInfo *findMonitor(const QString &name);
    const MonitorInfo *findMonitor(const QString &name) const;

    // Selection management
    QStringList selectedMonitorNames() const;
    void setSelectedMonitors(const QStringList &names);
    void selectMonitor(const QString &name, bool additive = false);
    void selectAllMonitors();
    void clearSelection();

    // Mode management
    bool isDesktopSpanMode() const { return m_spanMode; }
    void setDesktopSpanMode(bool enabled);

    // Layout snap & keyboard focus
    bool isSnapEnabled() const { return m_snapEnabled; }
    void setSnapEnabled(bool enabled);
    int snapGridStepPx() const { return m_snapStepPx; }
    void setSnapGridStepPx(int stepPx);
    int focusedMonitorIndex() const { return m_focusedMonitorIndex; }

    // Interactive toolbar pill geometry (single source of truth for paint & hit-testing)
    QRectF modePillRect() const;
    QRectF snapPillRect() const;

    // Scaling mode & crop gravity
    int currentScalingMode() const { return m_scalingMode; }
    void setScalingMode(int mode);

    QPointF cropGravity(const QString &monitorName) const;
    void setCropGravity(const QString &monitorName, const QPointF &gravity);
    void setCropGravityForSelection(const QPointF &gravity);

    // Wallpaper assignment
    void assignWallpaper(const QString &monitorName, const QString &filePath,
                         const QPixmap &pixmap = QPixmap());
    void assignWallpaperToSelection(const QString &filePath, const QPixmap &pixmap = QPixmap());

    // Normalized span sub-rectangle calculation
    DesktopSpanCalculation calculateSpanRectangles() const;

  signals:
    void monitorSelectionChanged(const QStringList &selectedOutputs);
    void wallpaperDropped(const QStringList &targetOutputs, const QString &wallpaperPath);
    void scalingModeChanged(const QStringList &targetOutputs, int mode);
    void cropGravityChanged(const QStringList &targetOutputs, float gx, float gy);
    void desktopSpanModeToggled(bool enabled);
    void spanRectsCalculated(const QMap<QString, QRectF> &normalizedSpanRects);
    void outputsRefreshed(int count);
    void snapToggled(bool enabled);
    void monitorLayoutChanged();

  protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool focusNextPrevChild(bool next) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

    // Drag-and-drop
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

  private:
    void applyOutputsArray(const QJsonArray &outputsArray);
    void applyScreensFallback();
    void pruneSelectionAndClampFocus();
    void recalculateLayoutTransform();
    QRectF mapToCanvas(const QRect &compositorRect) const;
    QRectF getMonitorCanvasRect(int monitorIndex) const;
    int findMonitorAtCanvasPos(const QPointF &pos) const;
    QRectF getCropGravityHandleRect(int monitorIndex) const;
    QRectF getSpanGravityHandleRect() const;
    QRectF toolbarBarRect() const;

    void nudgeSelectedMonitors(int dx, int dy, bool bypassSnap);
    void enqueueAsyncWallpaperLoad(const QString &targetKey, const QString &filePath);
    void ensureDecodeWorkerStarted();
    void stopDecodeWorker();

    void drawGridBackground(QPainter &painter);
    void drawMonitors(QPainter &painter);
    void drawSpanOverlay(QPainter &painter);
    void drawGravityHandles(QPainter &painter);
    void drawToolbarOverlay(QPainter &painter);

    QList<MonitorInfo> m_monitors;
    QSet<QString> m_selectedMonitors;
    bool m_spanMode{false};
    int m_scalingMode{WAYWAL_SCALING_FILL};
    int m_nonSpanScalingMode{WAYWAL_SCALING_FILL};
    bool m_snapEnabled{true};
    int m_snapStepPx{32};
    int m_focusedMonitorIndex{-1};

    // Global desktop span wallpaper & gravity
    QString m_spanWallpaperPath;
    QPixmap m_spanWallpaperPixmap;
    QSize m_spanWallpaperSourceSize;
    QPointF m_spanCropGravity{0.5, 0.5};

    // Canvas view transform
    QRect m_virtualBoundingBox{0, 0, 1920, 1080};
    qreal m_canvasScale{1.0};
    qreal m_canvasOffsetX{0.0};
    qreal m_canvasOffsetY{0.0};
    qreal m_padding{40.0};
    QPointF m_panOffset{0.0, 0.0};

    // Interactive interaction state
    enum class DragMode {
        None,
        CropGravityHandle,
        SpanCropGravityHandle,
        MonitorMove,
        SelectionBox,
        Pan,
    };
    DragMode m_dragMode{DragMode::None};
    int m_activeDragMonitorIndex{-1};
    QPointF m_dragStartPos;
    QPointF m_currentMousePos;
    QPointF m_dragStartPan{0.0, 0.0};
    qreal m_dragStartScale{1.0};
    bool m_dragMoved{false};
    QMap<QString, QRect> m_dragInitialGeometries;
    struct SnapGuide {
        bool vertical{true};
        int coord{0};
    };
    QVector<SnapGuide> m_activeSnapGuides;
    QString m_hoveredMonitorName;
    QString m_dropTargetMonitorName;
    bool m_isHoveringGravityHandle{false};

    // Async output query & preview image decoding state
    uint64_t m_outputsGeneration{0};
    QPointer<QProcess> m_queryProc;
    QMap<QString, uint64_t> m_previewLoadGens;
    struct DecodeTask {
        QString targetKey;
        QString filePath;
        uint64_t generation{0};
    };
    std::mutex m_decodeMutex;
    std::condition_variable_any m_decodeCv;
    std::deque<DecodeTask> m_decodeQueue;
    bool m_decodeStop{false};
    std::jthread m_decodeThread;
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_MONITOR_TOPOLOGY_CANVAS_HPP
