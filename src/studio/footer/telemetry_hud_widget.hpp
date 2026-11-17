// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WWAL_STUDIO_FOOTER_TELEMETRY_HUD_WIDGET_HPP
#define WWAL_STUDIO_FOOTER_TELEMETRY_HUD_WIDGET_HPP

#include <QtWidgets/QWidget>
#include <QtWidgets/QLabel>
#include <QtWidgets/QComboBox>
#include <QtCore/QEvent>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QHideEvent>
#include <QtGui/QShowEvent>

#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "waywal/ipc_proto.h"

namespace wwal::studio {

namespace detail {
class FooterBadgeLabel;
} // namespace detail

/**
 * @brief Streaming Telemetry HUD & 4-Slot DMA-BUF Ring Buffer Visualizer.
 *
 * Features:
 * - Real-time wp_presentation hardware scanout FPS.
 * - GPU compute pass duration in microseconds (us) / milliseconds (ms).
 * - Daemon VRAM and PSS footprint in Megabytes.
 * - Studio live RSS in Megabytes asynchronously sampled from /proc/self/status (VmRSS) when
 * visible.
 * - Visual 4-slot DMA-BUF ring indicator (bo[0..3]) color-coded by state:
 *   Free, Compute Rendering, Wayland Committed, Hardware Scanout Locked.
 * - Diff-cached multi-output selector preserving the "ALL" option across hotplugs.
 */
class TelemetryHudWidget : public QWidget {
    Q_OBJECT

  public:
    enum class CompactTier {
        Full = 0,
        Compact,
        Minimal,
    };

    static constexpr size_t DmabufRingSlots = 4;

    explicit TelemetryHudWidget(QWidget *parent = nullptr);
    ~TelemetryHudWidget() override;

    /**
     * @brief Update telemetry metrics from daemon snapshot.
     */
    void updateTelemetry(const waywal_telemetry_snapshot_t &snapshot);

    /**
     * @brief Adjust displayed badge density for narrow footer widths.
     */
    void setCompactTier(CompactTier tier);

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

  protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void changeEvent(QEvent *event) override;

  private Q_SLOTS:
    void onRefreshTimerTick();
    void onOutputComboChanged(int index);

  private:
    enum class FpsTone {
        Unset = 0,
        Idle,
        Low,
        Medium,
        High,
    };

    void setupUi();
    void updateLabels();
    void updateSlotIndicators();
    void updateRssBadgeText();
    [[nodiscard]] bool isActivelyShown() const;
    void startRssWorkerIfNeeded();
    void stopRssWorker();
    void scheduleAsyncRssSample();
    void applySampledRssMb(float rssMb);
    void syncPollingState();

    // Telemetry data
    float m_scanoutFps{0.0f};
    uint32_t m_lastRenderUs{0};
    float m_daemonVramMb{0.0f};
    float m_daemonPssMb{0.0f};
    float m_studioRssMb{0.0f};
    bool m_hasRssSample{false};
    bool m_gpuActive{true};
    CompactTier m_compactTier{CompactTier::Full};
    FpsTone m_lastFpsTone{FpsTone::Unset};

    // 4-Slot DMA-BUF ring buffer states
    std::array<waywal_bo_slot_state_t, DmabufRingSlots> m_boStates{
        WAYWAL_BO_STATE_FREE, WAYWAL_BO_STATE_FREE, WAYWAL_BO_STATE_FREE, WAYWAL_BO_STATE_FREE};
    std::array<int, DmabufRingSlots> m_lastBoSlotStates{-1, -1, -1, -1};

    // Output tracking
    uint32_t m_numOutputs{0};
    uint32_t m_cachedOutputCount{0};
    int m_selectedOutputIndex{0};
    std::array<char[64], WAYWAL_MAX_TELEMETRY_OUTPUTS> m_outputNames{};
    std::array<QString, WAYWAL_MAX_TELEMETRY_OUTPUTS> m_cachedDisplayNames{};

    // Internal 1 Hz timer and async worker for studio RSS refresh
    QTimer m_refreshTimer;
    std::mutex m_rssMutex;
    std::condition_variable_any m_rssCv;
    bool m_rssSampleRequested{false};
    QPointer<TelemetryHudWidget> m_selfWeak;
    std::jthread m_rssWorker;

    // UI Widgets
    QComboBox *m_outputCombo{nullptr};
    detail::FooterBadgeLabel *m_fpsBadge{nullptr};
    detail::FooterBadgeLabel *m_computeBadge{nullptr};
    detail::FooterBadgeLabel *m_vramBadge{nullptr};
    detail::FooterBadgeLabel *m_pssBadge{nullptr};
    detail::FooterBadgeLabel *m_studioRssBadge{nullptr};

    // 4 DMA-BUF slot widgets
    std::array<detail::FooterBadgeLabel *, DmabufRingSlots> m_slotLabels{nullptr, nullptr, nullptr,
                                                                         nullptr};
};

} // namespace wwal::studio

#endif // WWAL_STUDIO_FOOTER_TELEMETRY_HUD_WIDGET_HPP
