// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WWAL_STUDIO_FOOTER_AUDIO_SPECTRUM_WIDGET_HPP
#define WWAL_STUDIO_FOOTER_AUDIO_SPECTRUM_WIDGET_HPP

#include <QtWidgets/QWidget>
#include <QtWidgets/QSlider>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLabel>
#include <QtCore/QTimer>
#include <QtCore/QElapsedTimer>
#include <QtCore/QRectF>
#include <QtGui/QBrush>
#include <QtGui/QColor>
#include <QtGui/QFocusEvent>
#include <QtGui/QFontMetrics>
#include <QtGui/QHideEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QLinearGradient>
#include <QtGui/QMouseEvent>
#include <QtGui/QPaintEvent>
#include <QtGui/QPainter>
#include <QtGui/QPen>
#include <QtGui/QRadialGradient>
#include <QtGui/QShowEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "waywal/ipc_proto.h"

namespace wwal::studio {

namespace detail {

namespace tokens {
inline constexpr QRgb SurfaceFooter = 0xff12141au;
inline constexpr QRgb SurfaceCard = 0xff181b22u;
inline constexpr QRgb SurfaceBadgeIdle = 0xff1a1e29u;
inline constexpr QRgb SurfaceSlate = 0xff1e293bu;
inline constexpr QRgb SurfacePausedSlate = 0xff334155u;
inline constexpr QRgb BorderSubtle = 0xff2c3240u;
inline constexpr QRgb BorderSlate = 0xff475569u;
inline constexpr QRgb BorderDivider = 0xff334155u;
inline constexpr QRgb TextPrimary = 0xfff1f5f9u;
inline constexpr QRgb TextSecondary = 0xff94a3b8u;
inline constexpr QRgb FocusRing = 0xff38bdf8u;
inline constexpr QRgb EmeraldBg = 0xff064e3bu;
inline constexpr QRgb EmeraldFg = 0xff34d399u;
inline constexpr QRgb EmeraldBorder = 0xff059669u;
inline constexpr QRgb SkyBg = 0xff082f49u;
inline constexpr QRgb SkyFg = 0xff38bdf8u;
inline constexpr QRgb SkyBorder = 0xff0284c7u;
inline constexpr QRgb AmberBg = 0xff3b2010u;
inline constexpr QRgb AmberDeepBg = 0xff451a03u;
inline constexpr QRgb AmberFg = 0xfffbbf24u;
inline constexpr QRgb AmberBorder = 0xffb45309u;
inline constexpr QRgb YellowBg = 0xff713f12u;
inline constexpr QRgb YellowFg = 0xfffde047u;
inline constexpr QRgb YellowBorder = 0xffca8a04u;
inline constexpr QRgb CrimsonBg = 0xff450a0au;
inline constexpr QRgb CrimsonFg = 0xfff87171u;
inline constexpr QRgb CrimsonBorder = 0xffdc2626u;
inline constexpr QRgb VioletFg = 0xffc084fcu;
inline constexpr QRgb VioletBorder = 0xff7e22ceu;
inline constexpr QRgb StatusBannerBg = 0xff0369a1u;
inline constexpr int MinFontPx = 11;
inline constexpr int MinTargetPx = 28;
} // namespace tokens

[[nodiscard]] inline float clampFinite(float val, float minVal, float maxVal) noexcept
{
    if (!std::isfinite(val)) {
        return minVal;
    }
    return std::clamp(val, minVal, maxVal);
}

/**
 * @brief Custom-painted QLabel pill badge avoiding dynamic setStyleSheet() CSS re-parsing.
 */
class FooterBadgeLabel final : public QLabel {
  public:
    explicit FooterBadgeLabel(const QString &text, QWidget *parent = nullptr,
                              QString minSampleText = QString(),
                              int minHeight = tokens::MinTargetPx, int padX = 6)
        : QLabel(text, parent),
          m_minSampleText(minSampleText.isEmpty() ? text : std::move(minSampleText)),
          m_minHeight(minHeight), m_padX(padX)
    {
        setAlignment(Qt::AlignCenter);
        QFont f = font();
        f.setFamily(QStringLiteral("monospace"));
        f.setStyleHint(QFont::Monospace);
        f.setPixelSize(tokens::MinFontPx);
        f.setBold(true);
        setFont(f);
        setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
        setMinimumHeight(m_minHeight);
    }

    void setBadgeColors(const QColor &bg, const QColor &fg, const QColor &border)
    {
        if (m_bg == bg && m_fg == fg && m_border == border) {
            return;
        }
        m_bg = bg;
        m_fg = fg;
        m_border = border;
        update();
    }

    void setBadgeText(const QString &newText)
    {
        if (text() == newText) {
            return;
        }
        const int oldAdvance = fontMetrics().horizontalAdvance(text());
        const int newAdvance = fontMetrics().horizontalAdvance(newText);
        setText(newText);
        if (newAdvance != oldAdvance) {
            updateGeometry();
        }
        update();
    }

    [[nodiscard]] QSize sizeHint() const override
    {
        const QFontMetrics fm(font());
        const int textW =
            std::max(fm.horizontalAdvance(text()), fm.horizontalAdvance(m_minSampleText));
        const int w = textW + m_padX * 2 + 4;
        const int h = std::max(m_minHeight, fm.height() + 8);
        return QSize(w, h);
    }

    [[nodiscard]] QSize minimumSizeHint() const override { return sizeHint(); }

  protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const qreal dpr = std::max(1.0, devicePixelRatioF());
        const qreal penW = 1.0;
        const qreal half = 0.5 / dpr;
        const QRectF frame = QRectF(rect()).adjusted(half, half, -half, -half);

        p.setPen(QPen(m_border, penW));
        p.setBrush(m_bg);
        p.drawRoundedRect(frame, 4.0, 4.0);

        p.setPen(m_fg);
        p.setFont(font());
        p.drawText(rect().adjusted(m_padX, 0, -m_padX, 0), alignment(), text());
    }

  private:
    QColor m_bg{QColor::fromRgba(tokens::SurfaceCard)};
    QColor m_fg{QColor::fromRgba(tokens::TextSecondary)};
    QColor m_border{QColor::fromRgba(tokens::BorderSubtle)};
    QString m_minSampleText;
    int m_minHeight{tokens::MinTargetPx};
    int m_padX{6};
};

} // namespace detail

/**
 * @brief 64-Band Bark-Scale Audio FFT Spectrum Visualizer & Audio Control Widget.
 *
 * Features:
 * - 64-band Schroeder/Zwicker Bark-scale FFT spectrum (20 Hz - 20 kHz) with demand-driven paint.
 * - Smooth exponential decay physics with instantaneous attack.
 * - Dynamic peak-hold gravity caps with configurable hold timers and gravity fall.
 * - Live RMS level meter with peak envelope follower.
 * - Beat onset pulse indicator with animated expanding radial glow.
 * - Master Audio Sensitivity Gain slider (0.0x - 5.0x).
 * - "Mute on External Audio" (mute_on_audio) toggle.
 * - Tooltip and keyboard inspection of individual Bark bands (center frequency in Hz, magnitude,
 * dB).
 */
class AudioSpectrumWidget : public QWidget {
    Q_OBJECT

  public:
    static constexpr size_t NumBands = 64;

    explicit AudioSpectrumWidget(QWidget *parent = nullptr);
    ~AudioSpectrumWidget() override = default;

    /**
     * @brief Update audio spectrum and beat telemetry from daemon snapshot.
     */
    void updateFromTelemetry(const waywal_telemetry_snapshot_t &snapshot);

    /**
     * @brief Direct update of raw audio data.
     */
    void updateAudioData(const float bands[NumBands], float rmsLevel, float beatPulse,
                         bool beatOnset, bool audioActive, bool pipewireActive, bool muteOnAudio,
                         float audioVolume);

    [[nodiscard]] float sensitivityGain() const noexcept { return m_sensitivityGain; }
    [[nodiscard]] bool muteOnAudio() const noexcept { return m_muteOnAudio; }
    [[nodiscard]] bool isPipewireActive() const noexcept { return m_pipewireActive; }
    [[nodiscard]] bool isAudioActive() const noexcept { return m_audioActive; }

    /**
     * @brief Calculate center frequency in Hz for a given Bark band [0..63].
     */
    static float bandCenterHz(size_t band) noexcept;

    /**
     * @brief Traunmueller-Schroeder Bark frequency transformation: Hz -> Bark.
     */
    static float hzToBark(float hz) noexcept;

    /**
     * @brief Inverse Bark frequency transformation: Bark -> Hz.
     */
    static float barkToHz(float bark) noexcept;

  public Q_SLOTS:
    void setSensitivityGain(float gain);
    void setMuteOnAudio(bool enabled);

  Q_SIGNALS:
    void sensitivityGainChanged(float gain);
    void muteOnAudioToggled(bool enabled);

  protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

  private Q_SLOTS:
    void onPhysicsTick();
    void onGainSliderChanged(int value);
    void onMuteCheckToggled(bool checked);

  private:
    enum class StatusTone {
        Unset = 0,
        Off,
        Ducked,
        Active,
    };

    struct VisualizerLayout {
        bool valid{false};
        QRectF totalBg;
        QRectF spectrumRect;
        QRectF rmsRect;
        qreal barSpacing{1.0};
        qreal barWidth{1.0};
        qreal barPitch{2.0};
    };

    void setupUi();
    void initPaintResources();
    [[nodiscard]] bool hasActivePhysics() const noexcept;
    void wakePhysicsTimer();
    void stepPhysics(float dt);
    [[nodiscard]] bool computeVisualizerGeometry(VisualizerLayout &out) const;
    void updateBeatBadgeVisual();
    void showBandTooltip(int band, const QPoint &globalPos);
    void renderSpectrumBars(QPainter &painter, const VisualizerLayout &layout);
    void renderRmsMeter(QPainter &painter, const QRectF &rect);
    void renderBeatPulse(QPainter &painter, const QRectF &rect);

    // Audio frequency bands state (normalized 0.0 - 1.0)
    std::array<float, NumBands> m_targetBands{};
    std::array<float, NumBands> m_currentBands{};
    std::array<float, NumBands> m_peakCaps{};
    std::array<float, NumBands> m_peakHoldTimers{};
    std::array<float, NumBands> m_peakVelocities{};
    std::array<float, NumBands> m_centerFreqs{};

    // RMS level & peak state
    float m_targetRms{0.0f};
    float m_currentRms{0.0f};
    float m_peakRms{0.0f};
    float m_peakRmsTimer{0.0f};

    // Beat pulse state
    float m_targetBeatPulse{0.0f};
    float m_currentBeatPulse{0.0f};
    bool m_beatOnset{false};
    float m_beatGlowIntensity{0.0f};
    float m_beatRingRadius{0.0f};

    // Controls and settings
    float m_sensitivityGain{1.0f};
    bool m_muteOnAudio{false};
    bool m_audioActive{false};
    bool m_pipewireActive{false};
    float m_audioVolume{1.0f};

    // Mouse / keyboard hover tracking for tooltips
    int m_hoveredBand{-1};
    int m_lastBeatAlphaBucket{-1};
    StatusTone m_lastStatusTone{StatusTone::Unset};

    // Hoisted paint resources
    QPen m_bgPanelPen;
    QBrush m_bgPanelBrush;
    QPen m_focusRingPen;
    QBrush m_hoverBarBrush;
    QBrush m_peakCapBrush;
    QBrush m_silentTickBrush;
    QBrush m_rmsTrackBrush;
    QBrush m_rmsPeakBrush;
    QLinearGradient m_barGradient;
    QLinearGradient m_rmsGradient;
    QRadialGradient m_beatAuraGradient;
    QPen m_beatRingPen;
    QRectF m_cachedBarGradRect;
    QRectF m_cachedRmsGradRect;
    QRectF m_cachedAuraRect;
    int m_cachedAuraAlpha{-1};

    // Timers & clock
    QTimer m_physicsTimer;
    QElapsedTimer m_elapsedTimer;

    // UI widgets
    QLabel *m_gainLabel{nullptr};
    QSlider *m_gainSlider{nullptr};
    QLabel *m_gainValueLabel{nullptr};
    QCheckBox *m_muteToggle{nullptr};
    detail::FooterBadgeLabel *m_beatBadge{nullptr};
    detail::FooterBadgeLabel *m_statusBadge{nullptr};
};

} // namespace wwal::studio

#endif // WWAL_STUDIO_FOOTER_AUDIO_SPECTRUM_WIDGET_HPP
