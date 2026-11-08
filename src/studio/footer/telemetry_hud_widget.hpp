// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WWAL_STUDIO_FOOTER_TELEMETRY_HUD_WIDGET_HPP
#define WWAL_STUDIO_FOOTER_TELEMETRY_HUD_WIDGET_HPP

#include <QtWidgets/QWidget>
#include <QtWidgets/QLabel>
#include <QtWidgets/QComboBox>
#include <QtCore/QTimer>
#include <QtGui/QPaintEvent>

#include <array>
#include <cstdint>

#include "waywal/ipc_proto.h"

namespace wwal::studio {

/**
 * @brief Streaming Telemetry HUD & 4-Slot DMA-BUF Ring Buffer Visualizer.
 *
 * Features:
 * - Real-time wp_presentation hardware scanout FPS.
 * - GPU compute pass duration in microseconds (µs).
 * - Daemon VRAM and PSS footprint in Megabytes.
 * - Studio live RSS in Megabytes directly parsed from /proc/self/status (VmRSS).
 * - Visual 4-slot DMA-BUF ring indicator (bo[0..3]) color-coded by state:
 *   Free, Compute Rendering, Wayland Committed, Hardware Scanout Locked.
 * - Multi-output selector for multi-monitor setups.
 */
class TelemetryHudWidget : public QWidget {
    Q_OBJECT

public:
    static constexpr size_t DmabufRingSlots = 4;

    explicit TelemetryHudWidget(QWidget *parent = nullptr);
    ~TelemetryHudWidget() override = default;

    /**
     * @brief Update telemetry metrics from daemon snapshot.
     */
    void updateTelemetry(const waywal_telemetry_snapshot_t &snapshot);

    /**
     * @brief Live resident set size (VmRSS) of wwal-studio process in Megabytes.
     */
    static float readCurrentProcessRssMb() noexcept;

    [[nodiscard]] float scanoutFps() const noexcept { return m_scanoutFps; }
    [[nodiscard]] uint32_t lastRenderUs() const noexcept { return m_lastRenderUs; }
    [[nodiscard]] float daemonVramMb() const noexcept { return m_daemonVramMb; }
    [[nodiscard]] float daemonPssMb() const noexcept { return m_daemonPssMb; }
    [[nodiscard]] float studioRssMb() const noexcept { return m_studioRssMb; }
    [[nodiscard]] waywal_bo_slot_state_t boSlotState(size_t slot) const noexcept;

Q_SIGNALS:
    void outputSelected(const QString &outputName);

private Q_SLOTS:
    void onRefreshTimerTick();
    void onOutputComboChanged(int index);

private:
    void setupUi();
    void updateLabels();
    void updateSlotIndicators();

    // Telemetry data
    float m_scanoutFps{0.0f};
    uint32_t m_lastRenderUs{0};
    float m_daemonVramMb{0.0f};
    float m_daemonPssMb{0.0f};
    float m_studioRssMb{0.0f};
    bool m_gpuActive{true};

    // 4-Slot DMA-BUF ring buffer states
    std::array<waywal_bo_slot_state_t, DmabufRingSlots> m_boStates{
        WAYWAL_BO_STATE_FREE,
        WAYWAL_BO_STATE_FREE,
        WAYWAL_BO_STATE_FREE,
        WAYWAL_BO_STATE_FREE
    };

    // Output tracking
    uint32_t m_numOutputs{0};
    int m_selectedOutputIndex{0};
    std::array<char[64], WAYWAL_MAX_TELEMETRY_OUTPUTS> m_outputNames{};

    // Internal 1 Hz timer for studio RSS refresh
    QTimer m_refreshTimer;

    // UI Widgets
    QComboBox *m_outputCombo{nullptr};
    QLabel *m_fpsBadge{nullptr};
    QLabel *m_computeBadge{nullptr};
    QLabel *m_vramBadge{nullptr};
    QLabel *m_pssBadge{nullptr};
    QLabel *m_studioRssBadge{nullptr};

    // 4 DMA-BUF slot widgets
    std::array<QLabel*, DmabufRingSlots> m_slotLabels{nullptr, nullptr, nullptr, nullptr};
};

} // namespace wwal::studio

#endif // WWAL_STUDIO_FOOTER_TELEMETRY_HUD_WIDGET_HPP
