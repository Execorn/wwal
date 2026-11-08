// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_CUBIC_BEZIER_EDITOR_HPP
#define WAYWAL_STUDIO_CUBIC_BEZIER_EDITOR_HPP

#include <QWidget>
#include <QPointF>
#include <QString>
#include <QTimer>
#include <QElapsedTimer>
#include <functional>
#include <memory>
#include <vector>
#include <chrono>

#include "studio/core/command_bus.hpp"
#include "waywal/bezier.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "waywal/types.h"
#pragma GCC diagnostic pop

class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QSlider;
class QPushButton;
class QLabel;

namespace waywal::studio {

/**
 * @brief Helper alias calling waywal_cubic_bezier_solve_t directly from libwaywal_core.a.
 */
inline float bezier_solve_t(const bezier_curve_t *curve, float progress)
{
    return waywal_cubic_bezier_solve_t(curve, progress);
}

/**
 * @brief Complete GPU transition configuration including easing curve and shader parameters.
 */
struct TransitionConfig {
    uint32_t type{WAYWAL_TRANSITION_FADE}; ///< Transition type (0..21)
    uint32_t durationMs{1000};             ///< Duration in milliseconds (100..10000)
    uint32_t fps{144};                     ///< Easing evaluation framerate (15..360)
    float angleDeg{0.0f};                  ///< Direction angle in degrees (0..360)
    float waveFreq{10.0f};                 ///< Wave frequency (0.1..50.0)
    float waveAmp{0.1f};                   ///< Wave amplitude (0.0..1.0)
    float centerX{0.5f};                   ///< Radial center X in [0.0, 1.0]
    float centerY{0.5f};                   ///< Radial center Y in [0.0, 1.0]
    bezier_curve_t bezier{{0.25f}, {0.1f}, {0.25f}, {1.0f}}; ///< Cubic bezier curve
    bool hasCustomBezier{true};                              ///< Custom curve enabled flag
};

class BezierCurveCanvas;
class EasingRunnerTrack;

/**
 * @brief Interactive Cubic Bézier curve editor and GPU transition control panel.
 *
 * Features:
 * - Direct evaluation via waywal_cubic_bezier_solve_t() and bezier_eval().
 * - High-precision anti-aliased [0, 1] x [0, 1] curve canvas.
 * - Draggable tangent handles P1(p1x, p1y) and P2(p2x, p2y) with strict p1x/p2x clamping in [0, 1].
 * - Real-time animated easing preview dot & physical motion runner track.
 * - Presets: Linear, Ease, Ease-In, Ease-Out, Ease-In-Out, Bounce.
 * - Complete GPU transition parameter controls for all 18 GPU transition types.
 * - CommandBus undo/redo integration with micro-drag coalescing.
 */
class CubicBezierEditorWidget : public QWidget {
    Q_OBJECT

  public:
    explicit CubicBezierEditorWidget(QWidget *parent = nullptr);
    ~CubicBezierEditorWidget() override;

    // Curve access
    bezier_curve_t curve() const { return m_config.bezier; }
    void setCurve(const bezier_curve_t &curve, bool dispatchCommand = false);
    void setCurve(float p1x, float p1y, float p2x, float p2y, bool dispatchCommand = false);

    // Preset application
    void applyPreset(const QString &name);

    // Transition configuration access
    const TransitionConfig &transitionConfig() const { return m_config; }
    void setTransitionConfig(const TransitionConfig &config);

    // CommandBus integration
    void setCommandBus(CommandBus *bus) { m_commandBus = bus; }
    CommandBus *commandBus() const { return m_commandBus; }

  signals:
    void curveChanged(float p1x, float p1y, float p2x, float p2y);
    void transitionConfigChanged(const TransitionConfig &config);
    void presetApplied(const QString &presetName);

  private slots:
    void onCanvasCurveChanged(float p1x, float p1y, float p2x, float p2y, bool isInteractive);
    void onTransitionTypeChanged(int index);
    void onDurationChanged(int val);
    void onFpsChanged(int val);
    void onAngleChanged(double val);
    void onWaveFreqChanged(double val);
    void onWaveAmpChanged(double val);
    void onCenterChanged();
    void onSpinBoxesCurveChanged();

  private:
    void setupUi();
    void updateUiFromConfig();

    TransitionConfig m_config;
    CommandBus *m_commandBus{nullptr};

    // Sub-widgets
    BezierCurveCanvas *m_canvas{nullptr};
    EasingRunnerTrack *m_runnerTrack{nullptr};

    // Preset buttons
    QPushButton *m_btnLinear{nullptr};
    QPushButton *m_btnEase{nullptr};
    QPushButton *m_btnEaseIn{nullptr};
    QPushButton *m_btnEaseOut{nullptr};
    QPushButton *m_btnEaseInOut{nullptr};
    QPushButton *m_btnBounce{nullptr};

    // Numeric curve inputs
    QDoubleSpinBox *m_spinP1x{nullptr};
    QDoubleSpinBox *m_spinP1y{nullptr};
    QDoubleSpinBox *m_spinP2x{nullptr};
    QDoubleSpinBox *m_spinP2y{nullptr};
    QLabel *m_cssReadoutLabel{nullptr};

    // Transition controls
    QComboBox *m_comboTransitionType{nullptr};
    QSpinBox *m_spinDuration{nullptr};
    QSlider *m_sliderDuration{nullptr};
    QSpinBox *m_spinFps{nullptr};
    QDoubleSpinBox *m_spinAngle{nullptr};
    QSlider *m_sliderAngle{nullptr};
    QDoubleSpinBox *m_spinWaveFreq{nullptr};
    QDoubleSpinBox *m_spinWaveAmp{nullptr};
    QDoubleSpinBox *m_spinCenterX{nullptr};
    QDoubleSpinBox *m_spinCenterY{nullptr};

    bool m_updatingUi{false};
};

/**
 * @brief The anti-aliased 2D canvas displaying the curve, handles, and animated dot.
 */
class BezierCurveCanvas : public QWidget {
    Q_OBJECT

  public:
    explicit BezierCurveCanvas(QWidget *parent = nullptr);
    ~BezierCurveCanvas() override;

    bezier_curve_t curve() const { return m_curve; }
    void setCurve(const bezier_curve_t &curve);

    void setDurationMs(uint32_t ms);
    void setAnimationFps(uint32_t fps);

  signals:
    void curveModified(float p1x, float p1y, float p2x, float p2y, bool isInteractive);
    void animationTick(float normalizedTime, float easedProgress);

  protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

  private:
    QPointF mapToCanvas(float x, float y) const;
    QPointF mapFromCanvas(const QPointF &pt) const;
    QRectF getHandleRect(const QPointF &canvasPt) const;

    bezier_curve_t m_curve{{0.25f}, {0.1f}, {0.25f}, {1.0f}};

    // Canvas view bounds (allowing overshoot Y in [-0.5, 1.5])
    const float m_minX{0.0f};
    const float m_maxX{1.0f};
    const float m_minY{-0.5f};
    const float m_maxY{1.5f};

    // Drag interaction
    enum class DragTarget { None, HandleP1, HandleP2 };
    DragTarget m_dragTarget{DragTarget::None};
    bezier_curve_t m_dragStartCurve;

    // Real-time animation
    QTimer m_animTimer;
    QElapsedTimer m_elapsedTimer;
    uint32_t m_durationMs{1000};
    float m_animProgress{0.0f};
    float m_easedProgress{0.0f};
};

/**
 * @brief Horizontal runner track illustrating physical easing velocity.
 */
class EasingRunnerTrack : public QWidget {
    Q_OBJECT

  public:
    explicit EasingRunnerTrack(QWidget *parent = nullptr);
    ~EasingRunnerTrack() override;

  public slots:
    void updateProgress(float normalizedTime, float easedProgress);

  protected:
    void paintEvent(QPaintEvent *event) override;

  private:
    float m_normalizedTime{0.0f};
    float m_easedProgress{0.0f};
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_CUBIC_BEZIER_EDITOR_HPP
