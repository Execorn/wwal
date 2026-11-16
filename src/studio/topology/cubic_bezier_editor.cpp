// SPDX-License-Identifier: GPL-3.0-or-later
#include "cubic_bezier_editor.hpp"

#include <QClipboard>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFontMetrics>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace waywal::studio {

/* ========================================================================= */
/* BezierCurveCanvas Implementation                                         */
/* ========================================================================= */

BezierCurveCanvas::BezierCurveCanvas(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(100, 60);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    m_curve = bezier_create(0.25f, 0.1f, 0.25f, 1.0f);
    m_dragStartCurve = m_curve;

    connect(&m_animTimer, &QTimer::timeout, this, [this]() {
        if (!isVisible() || (window() && !window()->isActiveWindow() && !m_hovered && !hasFocus() &&
                             m_dragTarget == DragTarget::None)) {
            m_animTimer.stop();
            return;
        }
        if (!m_hovered && !hasFocus() && m_dragTarget == DragTarget::None &&
            m_activityTimer.isValid() &&
            m_activityTimer.elapsed() > static_cast<qint64>(m_durationMs) + 1200) {
            m_animTimer.stop();
            return;
        }

        qint64 elapsed = m_elapsedTimer.elapsed();
        qint64 totalLoop = static_cast<qint64>(m_durationMs) + 300;
        qint64 currentInLoop = elapsed % totalLoop;

        if (currentInLoop <= m_durationMs) {
            m_animProgress = static_cast<float>(currentInLoop) / static_cast<float>(m_durationMs);
        } else {
            m_animProgress = 1.0f;
        }

        m_easedProgress = bezier_eval(&m_curve, m_animProgress);
        emit animationTick(m_animProgress, m_easedProgress);
        update();
    });

    setAnimationFps(144);
    m_elapsedTimer.start();
    m_activityTimer.start();

    class CanvasLifecycleFilter final : public QObject {
      public:
        explicit CanvasLifecycleFilter(BezierCurveCanvas *owner) : QObject(owner), m_owner(owner) {}

      protected:
        bool eventFilter(QObject * /*watched*/, QEvent *event) override
        {
            if (!m_owner) {
                return false;
            }
            switch (event->type()) {
            case QEvent::Hide:
            case QEvent::WindowDeactivate:
                m_owner->m_hovered = false;
                m_owner->m_animTimer.stop();
                break;
            case QEvent::Show:
            case QEvent::FocusIn:
            case QEvent::WindowActivate:
                m_owner->wakeAnimationPreview();
                break;
            case QEvent::Enter:
                m_owner->m_hovered = true;
                m_owner->wakeAnimationPreview();
                break;
            case QEvent::Leave:
                m_owner->m_hovered = false;
                m_owner->syncAnimationState();
                break;
            case QEvent::FocusOut:
                m_owner->syncAnimationState();
                break;
            default:
                break;
            }
            return false;
        }

      private:
        BezierCurveCanvas *m_owner{nullptr};
    };

    installEventFilter(new CanvasLifecycleFilter(this));
    if (isVisible()) {
        wakeAnimationPreview();
    }
}

BezierCurveCanvas::~BezierCurveCanvas() = default;

void BezierCurveCanvas::wakeAnimationPreview()
{
    m_elapsedTimer.restart();
    m_activityTimer.restart();
    syncAnimationState();
}

void BezierCurveCanvas::syncAnimationState()
{
    const bool activeWin = !window() || window()->isActiveWindow();
    const bool shouldAnimate =
        isVisible() && (m_hovered || hasFocus() || m_dragTarget != DragTarget::None ||
                        (activeWin && m_activityTimer.isValid() &&
                         m_activityTimer.elapsed() <= static_cast<qint64>(m_durationMs) + 1200));
    if (shouldAnimate) {
        if (!m_animTimer.isActive()) {
            m_animTimer.start();
        }
    } else {
        m_animTimer.stop();
    }
}

void BezierCurveCanvas::rebuildCurvePathIfNeeded()
{
    if (!m_curvePathDirty && m_cachedSize == size()) {
        return;
    }
    m_cachedSize = size();
    m_curvePathDirty = false;
    QPainterPath path;
    constexpr int numSamples = 200;
    for (int i = 0; i <= numSamples; ++i) {
        const float x = static_cast<float>(i) / static_cast<float>(numSamples);
        const float y = bezier_eval(&m_curve, x);
        const QPointF pt = mapToCanvas(x, y);
        if (i == 0) {
            path.moveTo(pt);
        } else {
            path.lineTo(pt);
        }
    }
    m_cachedCurvePath = std::move(path);
}

void BezierCurveCanvas::setCurve(const bezier_curve_t &curve)
{
    m_curve = bezier_create(curve.p1x, curve.p1y, curve.p2x, curve.p2y);
    m_curvePathDirty = true;
    m_easedProgress = bezier_eval(&m_curve, m_animProgress);
    wakeAnimationPreview();
    update();
}

void BezierCurveCanvas::setDurationMs(uint32_t ms)
{
    m_durationMs = std::max(100u, ms);
    wakeAnimationPreview();
}

void BezierCurveCanvas::setAnimationFps(uint32_t fps)
{
    uint32_t safeFps = std::clamp(fps, 15u, 360u);
    int intervalMs = std::max(1, static_cast<int>(1000.0f / static_cast<float>(safeFps)));
    m_animTimer.setInterval(intervalMs);
}

QPointF BezierCurveCanvas::mapToCanvas(float x, float y) const
{
    const qreal padX = std::clamp(static_cast<qreal>(width()) * 0.10, 12.0, 24.0);
    const qreal padY = std::clamp(static_cast<qreal>(height()) * 0.08, 6.0, 18.0);
    const qreal availW = std::max(16.0, static_cast<qreal>(width()) - 2.0 * padX);
    const qreal availH = std::max(16.0, static_cast<qreal>(height()) - 2.0 * padY);

    qreal px = padX + (static_cast<qreal>(x) - m_minX) / (m_maxX - m_minX) * availW;
    qreal py = padY + (m_maxY - static_cast<qreal>(y)) / (m_maxY - m_minY) * availH;
    return QPointF(px, py);
}

QPointF BezierCurveCanvas::mapFromCanvas(const QPointF &pt) const
{
    const qreal padX = std::clamp(static_cast<qreal>(width()) * 0.10, 12.0, 24.0);
    const qreal padY = std::clamp(static_cast<qreal>(height()) * 0.08, 6.0, 18.0);
    const qreal availW = std::max(16.0, static_cast<qreal>(width()) - 2.0 * padX);
    const qreal availH = std::max(16.0, static_cast<qreal>(height()) - 2.0 * padY);

    qreal x = m_minX + (pt.x() - padX) / availW * (m_maxX - m_minX);
    qreal y = m_maxY - (pt.y() - padY) / availH * (m_maxY - m_minY);
    return QPointF(x, y);
}

QRectF BezierCurveCanvas::getHandleRect(const QPointF &canvasPt) const
{
    const qreal r = 12.0;
    return QRectF(canvasPt.x() - r, canvasPt.y() - r, 2.0 * r, 2.0 * r);
}

void BezierCurveCanvas::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    // Canvas background
    painter.fillRect(rect(), QColor(24, 24, 37));

    // Unit square [0, 1] x [0, 1]
    QPointF p00 = mapToCanvas(0.0f, 0.0f);
    QPointF p11 = mapToCanvas(1.0f, 1.0f);
    QRectF unitBox(p00.x(), p11.y(), p11.x() - p00.x(), p00.y() - p11.y());

    painter.fillRect(unitBox, QColor(30, 30, 46));
    painter.setPen(QPen(QColor(49, 50, 68), 1.0, Qt::DashLine));
    painter.drawRect(unitBox);

    // Grid lines at 0.25, 0.5, 0.75
    painter.setPen(QPen(QColor(49, 50, 68, 140), 1.0, Qt::DotLine));
    for (float g = 0.25f; g < 1.0f; g += 0.25f) {
        QPointF h0 = mapToCanvas(0.0f, g);
        QPointF h1 = mapToCanvas(1.0f, g);
        painter.drawLine(h0, h1);

        QPointF v0 = mapToCanvas(g, 0.0f);
        QPointF v1 = mapToCanvas(g, 1.0f);
        painter.drawLine(v0, v1);
    }

    // Baseline y=0 and y=1 solid reference lines
    painter.setPen(QPen(QColor(88, 91, 112), 1.0));
    painter.drawLine(mapToCanvas(-0.05f, 0.0f), mapToCanvas(1.05f, 0.0f));
    painter.drawLine(mapToCanvas(-0.05f, 1.0f), mapToCanvas(1.05f, 1.0f));

    // Diagonal linear curve reference (0,0) -> (1,1)
    painter.setPen(QPen(QColor(69, 71, 90, 180), 1.0, Qt::DashLine));
    painter.drawLine(p00, p11);

    // Tangent handle stems
    QPointF ptP1 = mapToCanvas(m_curve.p1x, m_curve.p1y);
    QPointF ptP2 = mapToCanvas(m_curve.p2x, m_curve.p2y);

    // P1 stem (Blue)
    painter.setPen(QPen(QColor(137, 180, 250, 200), 1.5, Qt::SolidLine));
    painter.drawLine(p00, ptP1);

    // P2 stem (Coral/Pink)
    painter.setPen(QPen(QColor(243, 139, 168, 200), 1.5, Qt::SolidLine));
    painter.drawLine(p11, ptP2);

    // Cached curve stroke (rebuilt only on resize or curve change)
    rebuildCurvePathIfNeeded();
    QLinearGradient grad(p00, p11);
    grad.setColorAt(0.0, QColor(137, 180, 250)); // Sky blue
    grad.setColorAt(1.0, QColor(166, 227, 161)); // Spring green

    painter.setPen(QPen(QBrush(grad), 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(m_cachedCurvePath);

    // Animated live easing dot moving along the curve
    QPointF animPt = mapToCanvas(m_animProgress, m_easedProgress);

    // Outer glow
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(137, 180, 250, 80));
    painter.drawEllipse(animPt, 8.0, 8.0);

    // Inner bright core
    painter.setBrush(QColor(255, 255, 255));
    painter.drawEllipse(animPt, 3.0, 3.0);

    // Tangent handle P1 circle
    painter.setPen(QPen(hasFocus() && m_selectedHandle == DragTarget::HandleP1
                            ? QColor(249, 226, 175)
                            : QColor(255, 255, 255),
                        hasFocus() && m_selectedHandle == DragTarget::HandleP1 ? 2.4 : 1.8));
    painter.setBrush(QColor(137, 180, 250));
    painter.drawEllipse(ptP1, 5.5, 5.5);

    // Tangent handle P2 circle
    painter.setPen(QPen(hasFocus() && m_selectedHandle == DragTarget::HandleP2
                            ? QColor(249, 226, 175)
                            : QColor(255, 255, 255),
                        hasFocus() && m_selectedHandle == DragTarget::HandleP2 ? 2.4 : 1.8));
    painter.setBrush(QColor(243, 139, 168));
    painter.drawEllipse(ptP2, 5.5, 5.5);

    if (hasFocus()) {
        painter.setPen(QPen(QColor(137, 180, 250, 180), 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect().adjusted(1, 1, -2, -2));
    }

    // Labels for control points (smart offset away from the curve path and reference lines)
    QFont font = painter.font();
    font.setPixelSize(9);
    font.setBold(true);
    painter.setFont(font);

    painter.setPen(QColor(137, 180, 250));
    const qreal p1Lx =
        std::clamp((m_curve.p1y <= m_curve.p1x + 0.1f) ? (ptP1.x() + 8.0) : (ptP1.x() - 20.0), 4.0,
                   static_cast<qreal>(width() - 22));
    const qreal p1Ly =
        std::clamp((m_curve.p1y <= m_curve.p1x + 0.1f) ? (ptP1.y() + 1.0) : (ptP1.y() - 13.0), 2.0,
                   static_cast<qreal>(height() - 13));
    painter.drawText(QRectF(p1Lx, p1Ly, 22, 12), Qt::AlignLeft | Qt::AlignVCenter, "P1");

    painter.setPen(QColor(243, 139, 168));
    const qreal p2Lx = std::clamp(
        (ptP2.x() >= 28.0 && m_curve.p2y >= m_curve.p2x) ? (ptP2.x() - 19.0) : (ptP2.x() + 8.0),
        4.0, static_cast<qreal>(width() - 22));
    const qreal p2Ly = std::clamp((m_curve.p2y >= 0.85f) ? (ptP2.y() - 14.0) : (ptP2.y() - 6.0),
                                  1.0, static_cast<qreal>(height() - 13));
    painter.drawText(QRectF(p2Lx, p2Ly, 22, 12), Qt::AlignLeft | Qt::AlignVCenter, "P2");
}

void BezierCurveCanvas::resizeEvent(QResizeEvent *event)
{
    m_curvePathDirty = true;
    QWidget::resizeEvent(event);
}

void BezierCurveCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        QPointF ptP1 = mapToCanvas(m_curve.p1x, m_curve.p1y);
        QPointF ptP2 = mapToCanvas(m_curve.p2x, m_curve.p2y);

        if (getHandleRect(ptP1).contains(event->position())) {
            m_dragTarget = DragTarget::HandleP1;
            m_selectedHandle = DragTarget::HandleP1;
            m_dragStartCurve = m_curve;
            wakeAnimationPreview();
        } else if (getHandleRect(ptP2).contains(event->position())) {
            m_dragTarget = DragTarget::HandleP2;
            m_selectedHandle = DragTarget::HandleP2;
            m_dragStartCurve = m_curve;
            wakeAnimationPreview();
        }
    }
}

void BezierCurveCanvas::mouseMoveEvent(QMouseEvent *event)
{
    QPointF canvasPt = event->position();

    if (m_dragTarget == DragTarget::HandleP1) {
        QPointF curvePt = mapFromCanvas(canvasPt);
        float p1x = std::clamp(static_cast<float>(curvePt.x()), 0.0f, 1.0f);
        float p1y = std::clamp(static_cast<float>(curvePt.y()), m_minY, m_maxY);
        m_curve.p1x = p1x;
        m_curve.p1y = p1y;
        m_curvePathDirty = true;
        wakeAnimationPreview();
        emit curveModified(m_curve.p1x, m_curve.p1y, m_curve.p2x, m_curve.p2y, true);
        update();
        return;
    }

    if (m_dragTarget == DragTarget::HandleP2) {
        QPointF curvePt = mapFromCanvas(canvasPt);
        float p2x = std::clamp(static_cast<float>(curvePt.x()), 0.0f, 1.0f);
        float p2y = std::clamp(static_cast<float>(curvePt.y()), m_minY, m_maxY);
        m_curve.p2x = p2x;
        m_curve.p2y = p2y;
        m_curvePathDirty = true;
        wakeAnimationPreview();
        emit curveModified(m_curve.p1x, m_curve.p1y, m_curve.p2x, m_curve.p2y, true);
        update();
        return;
    }

    // Hover cursor update
    QPointF ptP1 = mapToCanvas(m_curve.p1x, m_curve.p1y);
    QPointF ptP2 = mapToCanvas(m_curve.p2x, m_curve.p2y);
    if (getHandleRect(ptP1).contains(canvasPt) || getHandleRect(ptP2).contains(canvasPt)) {
        setCursor(Qt::PointingHandCursor);
    } else {
        setCursor(Qt::ArrowCursor);
    }
}

void BezierCurveCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragTarget != DragTarget::None) {
        m_dragTarget = DragTarget::None;
        wakeAnimationPreview();
        emit curveModified(m_curve.p1x, m_curve.p1y, m_curve.p2x, m_curve.p2y, false);
    }
}

void BezierCurveCanvas::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Space) {
        m_selectedHandle = (m_selectedHandle == DragTarget::HandleP1) ? DragTarget::HandleP2
                                                                      : DragTarget::HandleP1;
        wakeAnimationPreview();
        update();
        return;
    }

    const float step = (event->modifiers() & Qt::ShiftModifier) ? 0.005f : 0.02f;
    float dx = 0.0f;
    float dy = 0.0f;
    switch (event->key()) {
    case Qt::Key_Left:
        dx = -step;
        break;
    case Qt::Key_Right:
        dx = step;
        break;
    case Qt::Key_Up:
        dy = step;
        break;
    case Qt::Key_Down:
        dy = -step;
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }

    if (m_selectedHandle == DragTarget::HandleP1) {
        m_curve.p1x = std::clamp(m_curve.p1x + dx, 0.0f, 1.0f);
        m_curve.p1y = std::clamp(m_curve.p1y + dy, m_minY, m_maxY);
    } else {
        m_curve.p2x = std::clamp(m_curve.p2x + dx, 0.0f, 1.0f);
        m_curve.p2y = std::clamp(m_curve.p2y + dy, m_minY, m_maxY);
    }
    m_curvePathDirty = true;
    wakeAnimationPreview();
    emit curveModified(m_curve.p1x, m_curve.p1y, m_curve.p2x, m_curve.p2y, false);
    update();
}

/* ========================================================================= */
/* EasingRunnerTrack Implementation                                          */
/* ========================================================================= */

EasingRunnerTrack::EasingRunnerTrack(QWidget *parent) : QWidget(parent)
{
    setFixedHeight(14);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

EasingRunnerTrack::~EasingRunnerTrack() = default;

void EasingRunnerTrack::updateProgress(float normalizedTime, float easedProgress)
{
    m_normalizedTime = normalizedTime;
    m_easedProgress = easedProgress;
    update();
}

void EasingRunnerTrack::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QRectF trackRect = rect().adjusted(4, 2, -4, -2);

    // Track trench background
    painter.setPen(QPen(QColor(49, 50, 68), 1.0));
    painter.setBrush(QColor(17, 17, 27));
    painter.drawRoundedRect(trackRect, 4.0, 4.0);

    // Runner block position
    const qreal runnerW = 16.0;
    qreal availW = std::max(1.0, trackRect.width() - runnerW);
    qreal rx =
        trackRect.left() + std::clamp(static_cast<qreal>(m_easedProgress), 0.0, 1.0) * availW;
    QRectF runnerRect(rx, trackRect.top() + 2, runnerW, trackRect.height() - 4);

    // Glow effect
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(137, 180, 250, 90));
    painter.drawRoundedRect(runnerRect.adjusted(-2, -2, 2, 2), 4.0, 4.0);

    // Runner core
    painter.setBrush(QColor(137, 180, 250));
    painter.drawRoundedRect(runnerRect, 3.0, 3.0);
}

/* ========================================================================= */
/* CubicBezierEditorWidget Implementation                                    */
/* ========================================================================= */

CubicBezierEditorWidget::CubicBezierEditorWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(200, 115);
    setupUi();
    updateUiFromConfig();
}

CubicBezierEditorWidget::~CubicBezierEditorWidget() = default;

void CubicBezierEditorWidget::setupUi()
{
    auto *rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(6, 4, 6, 4);
    rootLayout->setSpacing(8);

    // Left Column: Curve Canvas + Live Runner + Quick Presets
    auto *leftCol = new QVBoxLayout();
    leftCol->setContentsMargins(0, 0, 0, 0);
    leftCol->setSpacing(4);

    m_canvas = new BezierCurveCanvas(this);
    leftCol->addWidget(m_canvas, 1);

    m_runnerTrack = new EasingRunnerTrack(this);
    leftCol->addWidget(m_runnerTrack);

    connect(m_canvas, &BezierCurveCanvas::curveModified, this,
            &CubicBezierEditorWidget::onCanvasCurveChanged);
    connect(m_canvas, &BezierCurveCanvas::animationTick, m_runnerTrack,
            &EasingRunnerTrack::updateProgress);

    // Preset buttons in a 2x3 grid
    auto *presetsGrid = new QGridLayout();
    presetsGrid->setContentsMargins(0, 0, 0, 0);
    presetsGrid->setSpacing(4);

    auto makePresetBtn = [this](const QString &text) {
        auto *btn = new QPushButton(text, this);
        btn->setFixedHeight(22);
        btn->setStyleSheet(
            "QPushButton { background: #20242e; color: #cbd5e1; border: 1px solid #2c3240; "
            "border-radius: 4px; padding: 1px 6px; font-weight: 600; font-size: 9.5px; }"
            "QPushButton:hover { background: #2a303c; color: #38bdf8; border-color: #3b4252; }"
            "QPushButton:pressed { background: #181b22; }");
        return btn;
    };

    m_btnLinear = makePresetBtn("Linear");
    m_btnEase = makePresetBtn("Ease");
    m_btnEaseIn = makePresetBtn("Ease-In");
    m_btnEaseOut = makePresetBtn("Ease-Out");
    m_btnEaseInOut = makePresetBtn("In-Out");
    m_btnBounce = makePresetBtn("Bounce");

    presetsGrid->addWidget(m_btnLinear, 0, 0);
    presetsGrid->addWidget(m_btnEase, 0, 1);
    presetsGrid->addWidget(m_btnEaseIn, 0, 2);
    presetsGrid->addWidget(m_btnEaseOut, 1, 0);
    presetsGrid->addWidget(m_btnEaseInOut, 1, 1);
    presetsGrid->addWidget(m_btnBounce, 1, 2);
    leftCol->addLayout(presetsGrid);

    auto *savedRow = new QHBoxLayout();
    savedRow->setContentsMargins(0, 0, 0, 0);
    savedRow->setSpacing(4);
    m_savedPresetCombo = new QComboBox(this);
    m_savedPresetCombo->setFixedHeight(20);
    m_savedPresetCombo->addItem(tr("Saved Presets..."));
    m_savedPresetCombo->setStyleSheet(
        "QComboBox { background: #181b22; color: #cbd5e1; border: 1px solid #2c3240; "
        "border-radius: 3px; padding: 0px 4px; font-size: 9px; }");
    m_btnSavePreset = makePresetBtn(tr("Save Preset"));
    m_btnSavePreset->setFixedHeight(20);
    savedRow->addWidget(m_savedPresetCombo, 1);
    savedRow->addWidget(m_btnSavePreset);
    leftCol->addLayout(savedRow);

    rootLayout->addLayout(leftCol, 4);

    connect(m_btnLinear, &QPushButton::clicked, this, [this]() { applyPreset("Linear"); });
    connect(m_btnEase, &QPushButton::clicked, this, [this]() { applyPreset("Ease"); });
    connect(m_btnEaseIn, &QPushButton::clicked, this, [this]() { applyPreset("Ease-In"); });
    connect(m_btnEaseOut, &QPushButton::clicked, this, [this]() { applyPreset("Ease-Out"); });
    connect(m_btnEaseInOut, &QPushButton::clicked, this, [this]() { applyPreset("Ease-In-Out"); });
    connect(m_btnBounce, &QPushButton::clicked, this, [this]() { applyPreset("Bounce"); });
    connect(m_btnSavePreset, &QPushButton::clicked, this,
            &CubicBezierEditorWidget::onSaveCustomPresetClicked);
    connect(m_savedPresetCombo, QOverload<int>::of(&QComboBox::activated), this,
            &CubicBezierEditorWidget::onSavedPresetActivated);

    // Right Column: Scrollable Curve Parameters & GPU Transition Settings
    auto *rightScroll = new QScrollArea(this);
    rightScroll->setWidgetResizable(true);
    rightScroll->setFrameShape(QFrame::NoFrame);
    rightScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    rightScroll->setStyleSheet("QScrollArea { background: transparent; border: none; }");

    auto *rightContainer = new QWidget(rightScroll);
    auto *rightLayout = new QVBoxLayout(rightContainer);
    rightLayout->setContentsMargins(0, 0, 4, 0);
    rightLayout->setSpacing(6);

    const QString groupStyle = QStringLiteral(
        "QGroupBox { font-weight: 600; font-size: 9.5px; color: #cbd5e1; border: 1px solid "
        "#262b36; "
        "border-radius: 5px; margin-top: 6px; padding-top: 4px; } "
        "QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; left: 6px; "
        "padding: 0 4px; }");
    const QString labelStyle = QStringLiteral("font-size: 9.5px; color: #94a3b8; padding: 0px;");

    auto makeParamLabel = [&labelStyle](const QString &text, QWidget *parentWidget) {
        auto *lbl = new QLabel(text, parentWidget);
        lbl->setStyleSheet(labelStyle);
        return lbl;
    };

    // Numeric control point spinboxes & CSS formula
    auto *coordsGroup = new QGroupBox("Curve Points", rightContainer);
    coordsGroup->setStyleSheet(groupStyle);
    auto *coordsLayout = new QGridLayout(coordsGroup);
    coordsLayout->setContentsMargins(6, 4, 6, 4);
    coordsLayout->setSpacing(4);

    auto makeDoubleSpin = [](QWidget *parentWidget, double min, double max, double step) {
        auto *spin = new QDoubleSpinBox(parentWidget);
        spin->setRange(min, max);
        spin->setSingleStep(step);
        spin->setDecimals(3);
        spin->setKeyboardTracking(false);
        spin->setFixedHeight(20);
        spin->setMinimumWidth(52);
        spin->setStyleSheet(
            "QDoubleSpinBox { background: #181b22; color: #e2e8f0; border: 1px solid #2c3240; "
            "border-radius: 3px; padding: 0px 3px; font-size: 9px; }");
        return spin;
    };

    m_spinP1x = makeDoubleSpin(coordsGroup, 0.0, 1.0, 0.02);
    m_spinP1y = makeDoubleSpin(coordsGroup, -0.75, 1.75, 0.05);
    m_spinP2x = makeDoubleSpin(coordsGroup, 0.0, 1.0, 0.02);
    m_spinP2y = makeDoubleSpin(coordsGroup, -0.75, 1.75, 0.05);

    coordsLayout->addWidget(makeParamLabel("P1.x:", coordsGroup), 0, 0);
    coordsLayout->addWidget(m_spinP1x, 0, 1);
    coordsLayout->addWidget(makeParamLabel("P1.y:", coordsGroup), 0, 2);
    coordsLayout->addWidget(m_spinP1y, 0, 3);

    coordsLayout->addWidget(makeParamLabel("P2.x:", coordsGroup), 1, 0);
    coordsLayout->addWidget(m_spinP2x, 1, 1);
    coordsLayout->addWidget(makeParamLabel("P2.y:", coordsGroup), 1, 2);
    coordsLayout->addWidget(m_spinP2y, 1, 3);

    m_cssReadoutLabel = new QLabel(coordsGroup);
    m_cssReadoutLabel->setMinimumWidth(80);
    m_cssReadoutLabel->setFixedHeight(16);
    m_cssReadoutLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_cssReadoutLabel->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                               Qt::TextSelectableByKeyboard);
    m_cssReadoutLabel->setStyleSheet(
        "color: #38bdf8; font-family: monospace; font-size: 9px; padding: 0px;");
    coordsLayout->addWidget(m_cssReadoutLabel, 2, 0, 1, 4);

    auto *exportRow = new QHBoxLayout();
    exportRow->setContentsMargins(0, 0, 0, 0);
    exportRow->setSpacing(4);
    m_btnCopyCss = makePresetBtn(tr("Copy CSS"));
    m_btnCopyCss->setFixedHeight(19);
    m_btnExportJson = makePresetBtn(tr("Export JSON"));
    m_btnExportJson->setFixedHeight(19);
    exportRow->addWidget(m_btnCopyCss);
    exportRow->addWidget(m_btnExportJson);
    coordsLayout->addLayout(exportRow, 3, 0, 1, 4);

    connect(m_btnCopyCss, &QPushButton::clicked, this, &CubicBezierEditorWidget::onCopyCssClicked);
    connect(m_btnExportJson, &QPushButton::clicked, this,
            &CubicBezierEditorWidget::onExportJsonClicked);

    rightLayout->addWidget(coordsGroup);

    connect(m_spinP1x, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            &CubicBezierEditorWidget::onSpinBoxesCurveChanged);
    connect(m_spinP1y, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            &CubicBezierEditorWidget::onSpinBoxesCurveChanged);
    connect(m_spinP2x, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            &CubicBezierEditorWidget::onSpinBoxesCurveChanged);
    connect(m_spinP2y, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            &CubicBezierEditorWidget::onSpinBoxesCurveChanged);

    // Transition Settings panel
    auto *transGroup = new QGroupBox("Transition Settings", rightContainer);
    transGroup->setStyleSheet(groupStyle);
    auto *transLayout = new QGridLayout(transGroup);
    transLayout->setContentsMargins(6, 4, 6, 4);
    transLayout->setSpacing(4);

    m_comboTransitionType = new QComboBox(transGroup);
    m_comboTransitionType->setFixedHeight(20);
    m_comboTransitionType->setMinimumWidth(80);
    m_comboTransitionType->setStyleSheet(
        "QComboBox { background: #181b22; color: #e2e8f0; border: 1px solid #2c3240; "
        "border-radius: 3px; padding: 0px 4px; font-size: 9px; } QComboBox::drop-down { border: "
        "none; }");

    const struct {
        int type;
        const char *name;
    } transitions[] = {{WAYWAL_TRANSITION_NONE, "None (Instant Cut)"},
                       {WAYWAL_TRANSITION_SIMPLE, "Simple Cut"},
                       {WAYWAL_TRANSITION_FADE, "Fade (Crossfade)"},
                       {WAYWAL_TRANSITION_WIPE, "Directional Wipe"},
                       {WAYWAL_TRANSITION_GROW, "Grow / Expand"},
                       {WAYWAL_TRANSITION_OUTER, "Outer Reveal"},
                       {WAYWAL_TRANSITION_WAVE, "Wave Dissolve"},
                       {WAYWAL_TRANSITION_NOISE, "Perlin Noise"},
                       {WAYWAL_TRANSITION_CROSSZOOM, "Cross Zoom"},
                       {WAYWAL_TRANSITION_SLIDE, "Smooth Slide"},
                       {WAYWAL_TRANSITION_GLITCH, "Digital Glitch"},
                       {WAYWAL_TRANSITION_BURN, "Color Burn"},
                       {WAYWAL_TRANSITION_RIPPLE, "Water Ripple"},
                       {WAYWAL_TRANSITION_PIXELATE, "Pixelate"},
                       {WAYWAL_TRANSITION_DOOM, "Doom Melt"},
                       {WAYWAL_TRANSITION_SWIRL, "Vortex Swirl"},
                       {WAYWAL_TRANSITION_CUBE, "3D Cube Flip"},
                       {WAYWAL_TRANSITION_LUMA, "Luma Mask"},
                       {WAYWAL_TRANSITION_LIGHT_LEAK, "Light Leak"},
                       {WAYWAL_TRANSITION_PAGE_CURL, "Page Curl"},
                       {WAYWAL_TRANSITION_CUSTOM, "Custom Shader"},
                       {WAYWAL_TRANSITION_RANDOM, "Random"}};

    for (const auto &t : transitions) {
        m_comboTransitionType->addItem(t.name, t.type);
    }
    transLayout->addWidget(makeParamLabel("Effect:", transGroup), 0, 0);
    transLayout->addWidget(m_comboTransitionType, 0, 1, 1, 3);

    // Row 1: Duration Spin + Slider
    m_spinDuration = new QSpinBox(transGroup);
    m_spinDuration->setRange(100, 10000);
    m_spinDuration->setSingleStep(50);
    m_spinDuration->setSuffix(" ms");
    m_spinDuration->setFixedHeight(20);
    m_spinDuration->setMinimumWidth(52);
    m_spinDuration->setStyleSheet(
        "QSpinBox { background: #181b22; color: #e2e8f0; border: 1px solid #2c3240; border-radius: "
        "3px; padding: 0px 3px; font-size: 9px; }");

    m_sliderDuration = new QSlider(Qt::Horizontal, transGroup);
    m_sliderDuration->setFixedHeight(16);
    m_sliderDuration->setRange(100, 10000);
    m_sliderDuration->setMinimumWidth(40);

    transLayout->addWidget(makeParamLabel("Duration:", transGroup), 1, 0);
    transLayout->addWidget(m_spinDuration, 1, 1);
    transLayout->addWidget(m_sliderDuration, 1, 2, 1, 2);

    // Row 2: Angle Spin + Slider (0.1 deg slider resolution to preserve decimals)
    m_spinAngle = makeDoubleSpin(transGroup, 0.0, 360.0, 5.0);
    m_spinAngle->setSuffix(" deg");
    m_sliderAngle = new QSlider(Qt::Horizontal, transGroup);
    m_sliderAngle->setFixedHeight(16);
    m_sliderAngle->setRange(0, 3600);
    m_sliderAngle->setMinimumWidth(40);

    transLayout->addWidget(makeParamLabel("Angle:", transGroup), 2, 0);
    transLayout->addWidget(m_spinAngle, 2, 1);
    transLayout->addWidget(m_sliderAngle, 2, 2, 1, 2);

    // Row 3: FPS & Wave Freq
    m_spinFps = new QSpinBox(transGroup);
    m_spinFps->setRange(15, 360);
    m_spinFps->setValue(144);
    m_spinFps->setSuffix(" Hz");
    m_spinFps->setFixedHeight(20);
    m_spinFps->setMinimumWidth(52);
    m_spinFps->setStyleSheet("QSpinBox { background: #181b22; color: #e2e8f0; border: 1px solid "
                             "#2c3240; border-radius: 3px; padding: 0px 3px; font-size: 9px; }");

    m_spinWaveFreq = makeDoubleSpin(transGroup, 0.1, 50.0, 0.5);
    m_spinWaveAmp = makeDoubleSpin(transGroup, 0.0, 1.0, 0.02);

    transLayout->addWidget(makeParamLabel("FPS:", transGroup), 3, 0);
    transLayout->addWidget(m_spinFps, 3, 1);
    transLayout->addWidget(makeParamLabel("Freq:", transGroup), 3, 2);
    transLayout->addWidget(m_spinWaveFreq, 3, 3);

    // Row 4: Wave Amp & Center X/Y
    m_spinCenterX = makeDoubleSpin(transGroup, 0.0, 1.0, 0.05);
    m_spinCenterY = makeDoubleSpin(transGroup, 0.0, 1.0, 0.05);

    transLayout->addWidget(makeParamLabel("Amp:", transGroup), 4, 0);
    transLayout->addWidget(m_spinWaveAmp, 4, 1);
    transLayout->addWidget(makeParamLabel("Center X:", transGroup), 4, 2);
    transLayout->addWidget(m_spinCenterX, 4, 3);

    transLayout->addWidget(makeParamLabel("Center Y:", transGroup), 5, 0);
    transLayout->addWidget(m_spinCenterY, 5, 1);

    rightLayout->addWidget(transGroup);
    rightLayout->addStretch(1);

    rightScroll->setWidget(rightContainer);
    rootLayout->addWidget(rightScroll, 7);

    // Connect transition controls
    connect(m_comboTransitionType, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &CubicBezierEditorWidget::onTransitionTypeChanged);
    connect(m_spinDuration, QOverload<int>::of(&QSpinBox::valueChanged), this,
            &CubicBezierEditorWidget::onDurationChanged);
    connect(m_sliderDuration, &QSlider::valueChanged, m_spinDuration, &QSpinBox::setValue);
    connect(m_spinFps, QOverload<int>::of(&QSpinBox::valueChanged), this,
            &CubicBezierEditorWidget::onFpsChanged);
    connect(m_spinAngle, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            &CubicBezierEditorWidget::onAngleChanged);
    connect(m_sliderAngle, &QSlider::valueChanged, this,
            [this](int val) { m_spinAngle->setValue(static_cast<double>(val) / 10.0); });
    connect(m_spinWaveFreq, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            &CubicBezierEditorWidget::onWaveFreqChanged);
    connect(m_spinWaveAmp, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            &CubicBezierEditorWidget::onWaveAmpChanged);
    connect(m_spinCenterX, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            &CubicBezierEditorWidget::onCenterChanged);
    connect(m_spinCenterY, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            &CubicBezierEditorWidget::onCenterChanged);
}

void CubicBezierEditorWidget::updateUiFromConfig()
{
    m_updatingUi = true;

    const QSignalBlocker blockP1x(m_spinP1x);
    const QSignalBlocker blockP1y(m_spinP1y);
    const QSignalBlocker blockP2x(m_spinP2x);
    const QSignalBlocker blockP2y(m_spinP2y);
    const QSignalBlocker blockType(m_comboTransitionType);
    const QSignalBlocker blockDurSpin(m_spinDuration);
    const QSignalBlocker blockDurSlider(m_sliderDuration);
    const QSignalBlocker blockFps(m_spinFps);
    const QSignalBlocker blockAngSpin(m_spinAngle);
    const QSignalBlocker blockAngSlider(m_sliderAngle);
    const QSignalBlocker blockFreq(m_spinWaveFreq);
    const QSignalBlocker blockAmp(m_spinWaveAmp);
    const QSignalBlocker blockCx(m_spinCenterX);
    const QSignalBlocker blockCy(m_spinCenterY);

    m_canvas->setCurve(m_config.bezier);
    m_canvas->setDurationMs(m_config.durationMs);
    m_canvas->setAnimationFps(m_config.fps);

    m_spinP1x->setValue(m_config.bezier.p1x);
    m_spinP1y->setValue(m_config.bezier.p1y);
    m_spinP2x->setValue(m_config.bezier.p2x);
    m_spinP2y->setValue(m_config.bezier.p2y);

    m_cssReadoutLabel->setText(QStringLiteral("CSS: %1").arg(cssTimingFunction()));

    int idx = m_comboTransitionType->findData(m_config.type);
    if (idx >= 0) {
        m_comboTransitionType->setCurrentIndex(idx);
    }

    m_spinDuration->setValue(m_config.durationMs);
    m_sliderDuration->setValue(m_config.durationMs);
    m_spinFps->setValue(m_config.fps);
    m_spinAngle->setValue(m_config.angleDeg);
    m_sliderAngle->setValue(static_cast<int>(std::lround(m_config.angleDeg * 10.0f)));
    m_spinWaveFreq->setValue(m_config.waveFreq);
    m_spinWaveAmp->setValue(m_config.waveAmp);
    m_spinCenterX->setValue(m_config.centerX);
    m_spinCenterY->setValue(m_config.centerY);

    m_updatingUi = false;
}

QString CubicBezierEditorWidget::cssTimingFunction() const
{
    return QStringLiteral("cubic-bezier(%1, %2, %3, %4)")
        .arg(QString::number(m_config.bezier.p1x, 'f', 3))
        .arg(QString::number(m_config.bezier.p1y, 'f', 3))
        .arg(QString::number(m_config.bezier.p2x, 'f', 3))
        .arg(QString::number(m_config.bezier.p2y, 'f', 3));
}

QString CubicBezierEditorWidget::exportTransitionJson() const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("transition_type"), static_cast<int>(m_config.type));
    obj.insert(QStringLiteral("duration_ms"), static_cast<int>(m_config.durationMs));
    obj.insert(QStringLiteral("fps"), static_cast<int>(m_config.fps));
    obj.insert(QStringLiteral("angle_deg"), static_cast<double>(m_config.angleDeg));
    obj.insert(QStringLiteral("wave_freq"), static_cast<double>(m_config.waveFreq));
    obj.insert(QStringLiteral("wave_amp"), static_cast<double>(m_config.waveAmp));
    obj.insert(QStringLiteral("center_x"), static_cast<double>(m_config.centerX));
    obj.insert(QStringLiteral("center_y"), static_cast<double>(m_config.centerY));
    obj.insert(QStringLiteral("css"), cssTimingFunction());
    QJsonObject bz;
    bz.insert(QStringLiteral("p1x"), static_cast<double>(m_config.bezier.p1x));
    bz.insert(QStringLiteral("p1y"), static_cast<double>(m_config.bezier.p1y));
    bz.insert(QStringLiteral("p2x"), static_cast<double>(m_config.bezier.p2x));
    bz.insert(QStringLiteral("p2y"), static_cast<double>(m_config.bezier.p2y));
    obj.insert(QStringLiteral("bezier"), bz);
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void CubicBezierEditorWidget::applyCurveState(const bezier_curve_t &curve)
{
    m_config.bezier = bezier_create(curve.p1x, curve.p1y, curve.p2x, curve.p2y);
    m_config.hasCustomBezier = true;
    updateUiFromConfig();
    emit curveChanged(m_config.bezier.p1x, m_config.bezier.p1y, m_config.bezier.p2x,
                      m_config.bezier.p2y);
    emit transitionConfigChanged(m_config);
}

void CubicBezierEditorWidget::setCurve(const bezier_curve_t &curve, bool dispatchCommand)
{
    const bezier_curve_t oldCurve = m_config.bezier;
    const bezier_curve_t nextCurve = bezier_create(curve.p1x, curve.p1y, curve.p2x, curve.p2y);

    if (dispatchCommand && m_commandBus) {
        QPointer<CubicBezierEditorWidget> self(this);
        auto cmd = std::make_unique<SetBezierCurveCommand>(
            "all", oldCurve, nextCurve, [self](const QString &output, const bezier_curve_t &c) {
                (void)output;
                if (!self) {
                    return;
                }
                self->applyCurveState(c);
            });
        m_commandBus->execute(std::move(cmd));
        return;
    }

    applyCurveState(nextCurve);
}

void CubicBezierEditorWidget::setCurve(float p1x, float p1y, float p2x, float p2y,
                                       bool dispatchCommand)
{
    setCurve(bezier_create(p1x, p1y, p2x, p2y), dispatchCommand);
}

void CubicBezierEditorWidget::applyPreset(const QString &name)
{
    bezier_curve_t targetCurve = BEZIER_DEFAULT;
    if (name.compare("Linear", Qt::CaseInsensitive) == 0) {
        targetCurve = BEZIER_LINEAR;
    } else if (name.compare("Ease", Qt::CaseInsensitive) == 0) {
        targetCurve = BEZIER_DEFAULT;
    } else if (name.compare("Ease-In", Qt::CaseInsensitive) == 0) {
        targetCurve = BEZIER_EASE_IN;
    } else if (name.compare("Ease-Out", Qt::CaseInsensitive) == 0) {
        targetCurve = BEZIER_EASE_OUT;
    } else if (name.compare("Ease-In-Out", Qt::CaseInsensitive) == 0) {
        targetCurve = BEZIER_EASE_IN_OUT;
    } else if (name.compare("Bounce", Qt::CaseInsensitive) == 0) {
        // Classic elastic bounce / overshoot curve
        targetCurve = bezier_create(0.68f, -0.55f, 0.265f, 1.55f);
    }

    setCurve(targetCurve, true);
    emit presetApplied(name);
}

void CubicBezierEditorWidget::setTransitionConfig(const TransitionConfig &config)
{
    m_config = config;
    updateUiFromConfig();
    emit transitionConfigChanged(m_config);
}

void CubicBezierEditorWidget::onCanvasCurveChanged(float p1x, float p1y, float p2x, float p2y,
                                                   bool isInteractive)
{
    if (!m_hasPendingDragBaseline) {
        m_dragStartCurve = m_config.bezier;
        m_hasPendingDragBaseline = true;
    }

    const bezier_curve_t nextCurve = bezier_create(p1x, p1y, p2x, p2y);
    if (isInteractive) {
        m_config.bezier = nextCurve;
        m_config.hasCustomBezier = true;

        m_updatingUi = true;
        const QSignalBlocker blockP1x(m_spinP1x);
        const QSignalBlocker blockP1y(m_spinP1y);
        const QSignalBlocker blockP2x(m_spinP2x);
        const QSignalBlocker blockP2y(m_spinP2y);
        m_spinP1x->setValue(nextCurve.p1x);
        m_spinP1y->setValue(nextCurve.p1y);
        m_spinP2x->setValue(nextCurve.p2x);
        m_spinP2y->setValue(nextCurve.p2y);
        m_cssReadoutLabel->setText(QStringLiteral("CSS: %1").arg(cssTimingFunction()));
        m_updatingUi = false;

        emit curveChanged(nextCurve.p1x, nextCurve.p1y, nextCurve.p2x, nextCurve.p2y);
        return;
    }

    const bezier_curve_t baseline = m_dragStartCurve;
    m_hasPendingDragBaseline = false;

    if (m_commandBus) {
        QPointer<CubicBezierEditorWidget> self(this);
        auto cmd = std::make_unique<SetBezierCurveCommand>(
            "all", baseline, nextCurve, [self](const QString &output, const bezier_curve_t &c) {
                (void)output;
                if (!self) {
                    return;
                }
                self->applyCurveState(c);
            });
        m_commandBus->execute(std::move(cmd));
    } else {
        applyCurveState(nextCurve);
    }
}

void CubicBezierEditorWidget::onCopyCssClicked()
{
    if (QClipboard *cb = QGuiApplication::clipboard()) {
        cb->setText(cssTimingFunction());
    }
}

void CubicBezierEditorWidget::onExportJsonClicked()
{
    if (QClipboard *cb = QGuiApplication::clipboard()) {
        cb->setText(exportTransitionJson());
    }
}

void CubicBezierEditorWidget::onSaveCustomPresetClicked()
{
    const QString name =
        QStringLiteral("Custom %1 (%2)").arg(m_customPresets.size() + 1).arg(cssTimingFunction());
    m_customPresets.emplace_back(name, m_config.bezier);
    m_savedPresetCombo->addItem(name, static_cast<int>(m_customPresets.size() - 1));
    m_savedPresetCombo->setCurrentIndex(m_savedPresetCombo->count() - 1);
}

void CubicBezierEditorWidget::onSavedPresetActivated(int index)
{
    if (index <= 0) {
        return;
    }
    const int customIdx = m_savedPresetCombo->itemData(index).toInt();
    if (customIdx < 0 || customIdx >= static_cast<int>(m_customPresets.size())) {
        return;
    }
    setCurve(m_customPresets[static_cast<size_t>(customIdx)].second, true);
    emit presetApplied(m_customPresets[static_cast<size_t>(customIdx)].first);
}

void CubicBezierEditorWidget::onSpinBoxesCurveChanged()
{
    if (m_updatingUi)
        return;

    float p1x = static_cast<float>(m_spinP1x->value());
    float p1y = static_cast<float>(m_spinP1y->value());
    float p2x = static_cast<float>(m_spinP2x->value());
    float p2y = static_cast<float>(m_spinP2y->value());

    setCurve(p1x, p1y, p2x, p2y, true);
}

void CubicBezierEditorWidget::onTransitionTypeChanged(int index)
{
    if (m_updatingUi)
        return;
    m_config.type = static_cast<uint32_t>(m_comboTransitionType->itemData(index).toInt());
    emit transitionConfigChanged(m_config);
}

void CubicBezierEditorWidget::onDurationChanged(int val)
{
    if (m_updatingUi)
        return;
    m_config.durationMs = static_cast<uint32_t>(val);
    {
        const QSignalBlocker blocker(m_sliderDuration);
        m_sliderDuration->setValue(val);
    }
    m_canvas->setDurationMs(m_config.durationMs);
    emit transitionConfigChanged(m_config);
}

void CubicBezierEditorWidget::onFpsChanged(int val)
{
    if (m_updatingUi)
        return;
    m_config.fps = static_cast<uint32_t>(val);
    m_canvas->setAnimationFps(m_config.fps);
    emit transitionConfigChanged(m_config);
}

void CubicBezierEditorWidget::onAngleChanged(double val)
{
    if (m_updatingUi)
        return;
    m_config.angleDeg = static_cast<float>(val);
    {
        const QSignalBlocker blocker(m_sliderAngle);
        m_sliderAngle->setValue(static_cast<int>(std::lround(val * 10.0)));
    }
    emit transitionConfigChanged(m_config);
}

void CubicBezierEditorWidget::onWaveFreqChanged(double val)
{
    if (m_updatingUi)
        return;
    m_config.waveFreq = static_cast<float>(val);
    emit transitionConfigChanged(m_config);
}

void CubicBezierEditorWidget::onWaveAmpChanged(double val)
{
    if (m_updatingUi)
        return;
    m_config.waveAmp = static_cast<float>(val);
    emit transitionConfigChanged(m_config);
}

void CubicBezierEditorWidget::onCenterChanged()
{
    if (m_updatingUi)
        return;
    m_config.centerX = static_cast<float>(m_spinCenterX->value());
    m_config.centerY = static_cast<float>(m_spinCenterY->value());
    emit transitionConfigChanged(m_config);
}

} // namespace waywal::studio
