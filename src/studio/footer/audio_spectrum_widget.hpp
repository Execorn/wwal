// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WWAL_STUDIO_FOOTER_AUDIO_SPECTRUM_WIDGET_HPP
#define WWAL_STUDIO_FOOTER_AUDIO_SPECTRUM_WIDGET_HPP

#include <QtWidgets/QWidget>
#include <QtWidgets/QSlider>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLabel>
#include <QtCore/QTimer>
#include <QtCore/QElapsedTimer>
#include <QtGui/QPaintEvent>
#include <QtGui/QMouseEvent>

#include <array>
#include <cstdint>

#include "waywal/ipc_proto.h"

namespace wwal::studio {

/**
 * @brief 64-Band Bark-Scale Audio FFT Spectrum Visualizer & Audio Control Widget.
 *
 * Features:
 * - 64-band Schroeder/Zwicker Bark-scale FFT spectrum (20 Hz - 20 kHz) rendered at 60 FPS.
 * - Smooth exponential decay physics with instantaneous attack.
 * - Dynamic peak-hold gravity caps with configurable hold timers and gravity fall.
 * - Live RMS level meter with peak envelope follower.
 * - Beat onset pulse indicator with animated expanding radial glow.
 * - Master Audio Sensitivity Gain slider (0.0x - 5.0x).
 * - "Mute on External Audio" (mute_on_audio) toggle.
 * - Tooltip inspection of individual Bark bands (center frequency in Hz, magnitude, dB).
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
     * @brief Traunmüller-Schroeder Bark frequency transformation: Hz -> Bark.
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

  private Q_SLOTS:
    void onPhysicsTick();
    void onGainSliderChanged(int value);
    void onMuteCheckToggled(bool checked);

  private:
    void setupUi();
    void stepPhysics(float dt);
    void renderSpectrumBars(class QPainter &painter, const QRectF &rect);
    void renderRmsMeter(class QPainter &painter, const QRectF &rect);
    void renderBeatPulse(class QPainter &painter, const QRectF &rect);

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

    // Mouse hover tracking for tooltips
    int m_hoveredBand{-1};

    // Timers & clock
    QTimer m_physicsTimer;
    QElapsedTimer m_elapsedTimer;

    // UI widgets
    QLabel *m_gainLabel{nullptr};
    QSlider *m_gainSlider{nullptr};
    QLabel *m_gainValueLabel{nullptr};
    QCheckBox *m_muteToggle{nullptr};
    QLabel *m_beatBadge{nullptr};
    QLabel *m_statusBadge{nullptr};
};

} // namespace wwal::studio

#endif // WWAL_STUDIO_FOOTER_AUDIO_SPECTRUM_WIDGET_HPP
