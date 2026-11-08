// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WWAL_STUDIO_FOOTER_POWER_GOVERNOR_WIDGET_HPP
#define WWAL_STUDIO_FOOTER_POWER_GOVERNOR_WIDGET_HPP

#include <QtWidgets/QWidget>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLabel>

#include <cstdint>

#include "waywal/ipc_proto.h"
#include "waywal/power_governor.h"

namespace wwal::studio {

/**
 * @brief Power Governor, Battery Policy & Auto-Pause Control Widget.
 *
 * Features:
 * - FPS cap selector (24, 30, 60, 120, 144, VSync).
 * - Battery Policy selector (Ignore, Reduce FPS to 30, Pause Video/Shaders).
 * - Auto-Pause policies (Fullscreen, Maximized, Occluded Workspace).
 * - Live battery and AC power status badge.
 * - Live daemon global pause reason monitor.
 */
class PowerGovernorWidget : public QWidget {
    Q_OBJECT

public:
    explicit PowerGovernorWidget(QWidget *parent = nullptr);
    ~PowerGovernorWidget() override = default;

    /**
     * @brief Update power governor state from daemon telemetry snapshot.
     */
    void updateFromTelemetry(const waywal_telemetry_snapshot_t &snapshot);

    [[nodiscard]] uint32_t fpsCap() const noexcept { return m_fpsCap; }
    [[nodiscard]] waywal_battery_policy_t batteryPolicy() const noexcept { return m_batteryPolicy; }
    [[nodiscard]] bool pauseFullscreen() const noexcept { return m_pauseFullscreen; }
    [[nodiscard]] bool pauseMaximized() const noexcept { return m_pauseMaximized; }
    [[nodiscard]] bool pauseOccluded() const noexcept { return m_pauseOccluded; }
    [[nodiscard]] bool isOnBattery() const noexcept { return m_onBattery; }
    [[nodiscard]] uint16_t globalPauseMask() const noexcept { return m_globalPauseMask; }

public Q_SLOTS:
    void setFpsCap(uint32_t fps);
    void setBatteryPolicy(waywal_battery_policy_t policy);
    void setAutoPausePolicies(bool pauseFullscreen, bool pauseMaximized, bool pauseOccluded);

Q_SIGNALS:
    void fpsCapChanged(uint32_t fps);
    void batteryPolicyChanged(waywal_battery_policy_t policy);
    void autoPauseFlagsChanged(bool pauseFullscreen, bool pauseMaximized, bool pauseOccluded);

private Q_SLOTS:
    void onFpsComboChanged(int index);
    void onBatteryPolicyComboChanged(int index);
    void onAutoPauseCheckChanged();

private:
    void setupUi();
    void updateStatusBadges();

    // Internal state
    uint32_t m_fpsCap{0}; // 0 = VSync
    waywal_battery_policy_t m_batteryPolicy{WAYWAL_BATTERY_IGNORE};
    bool m_pauseFullscreen{true};
    bool m_pauseMaximized{false};
    bool m_pauseOccluded{true};
    bool m_onBattery{false};
    uint16_t m_globalPauseMask{0};

    // UI Widgets
    QComboBox *m_fpsCombo{nullptr};
    QComboBox *m_batteryPolicyCombo{nullptr};
    QCheckBox *m_pauseFullscreenCheck{nullptr};
    QCheckBox *m_pauseMaximizedCheck{nullptr};
    QCheckBox *m_pauseOccludedCheck{nullptr};
    QLabel *m_powerBadge{nullptr};
    QLabel *m_pauseStatusBadge{nullptr};
};

} // namespace wwal::studio

#endif // WWAL_STUDIO_FOOTER_POWER_GOVERNOR_WIDGET_HPP
