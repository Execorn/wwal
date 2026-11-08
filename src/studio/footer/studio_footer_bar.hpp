// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WWAL_STUDIO_FOOTER_STUDIO_FOOTER_BAR_HPP
#define WWAL_STUDIO_FOOTER_STUDIO_FOOTER_BAR_HPP

#include <QtWidgets/QWidget>
#include <QtWidgets/QLabel>
#include <QtCore/QTimer>

#include "audio_spectrum_widget.hpp"
#include "power_governor_widget.hpp"
#include "telemetry_hud_widget.hpp"

namespace wwal::studio {

/**
 * @brief Unified Status & Telemetry Footer Bar for wwal-studio MainWindow.
 *
 * Features:
 * - Integrates AudioSpectrumWidget (64-band Bark-scale FFT, beat pulse, RMS meter).
 * - Integrates PowerGovernorWidget (FPS cap, battery policy, auto-pause flags, power status).
 * - Integrates TelemetryHudWidget (wp_presentation scanout FPS, compute duration, VRAM/PSS/RSS, 4-slot DMA-BUF ring).
 * - Exposes unified updateTelemetry() entry point and signal forwarding for MainWindow and CommandBus.
 */
class StudioFooterBar : public QWidget {
    Q_OBJECT

public:
    explicit StudioFooterBar(QWidget *parent = nullptr);
    ~StudioFooterBar() override = default;

    /**
     * @brief Ingest a live telemetry snapshot and distribute to all footer subsystems.
     */
    void updateTelemetry(const waywal_telemetry_snapshot_t &snapshot);

    /**
     * @brief Display a transient status message in the footer.
     */
    void setStatusMessage(const QString &msg, int timeoutMs = 3000);

    [[nodiscard]] AudioSpectrumWidget *audioSpectrum() const noexcept { return m_audioSpectrum; }
    [[nodiscard]] PowerGovernorWidget *powerGovernor() const noexcept { return m_powerGovernor; }
    [[nodiscard]] TelemetryHudWidget *telemetryHud() const noexcept { return m_telemetryHud; }

Q_SIGNALS:
    // Forwarded audio signals
    void audioSensitivityGainChanged(float gain);
    void muteOnAudioToggled(bool enabled);

    // Forwarded power governor signals
    void fpsCapChanged(uint32_t fps);
    void batteryPolicyChanged(waywal_battery_policy_t policy);
    void autoPauseFlagsChanged(bool pauseFullscreen, bool pauseMaximized, bool pauseOccluded);

    // Forwarded telemetry selection
    void outputSelected(const QString &outputName);

private Q_SLOTS:
    void onStatusMessageTimeout();

private:
    void setupUi();

    // Sub-components
    PowerGovernorWidget *m_powerGovernor{nullptr};
    AudioSpectrumWidget *m_audioSpectrum{nullptr};
    TelemetryHudWidget *m_telemetryHud{nullptr};
    QLabel *m_statusMessageLabel{nullptr};

    QTimer m_statusMessageTimer;
};

} // namespace wwal::studio

#endif // WWAL_STUDIO_FOOTER_STUDIO_FOOTER_BAR_HPP
