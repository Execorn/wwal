// SPDX-License-Identifier: GPL-3.0-or-later
#include "power_governor_widget.hpp"

#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>
#include <QtCore/QSignalBlocker>

#include <cstdio>

namespace wwal::studio {

PowerGovernorWidget::PowerGovernorWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(44);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    setupUi();
    updateStatusBadges();
}

void PowerGovernorWidget::setupUi()
{
    auto *rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(2, 2, 2, 2);
    rootLayout->setSpacing(4);

    // Common stylesheet for compact dark controls
    const QString comboStyle = "QComboBox {"
                               "  background-color: #1e293b;"
                               "  color: #f1f5f9;"
                               "  border: 1px solid #334155;"
                               "  border-radius: 4px;"
                               "  padding: 1px 3px;"
                               "  font-size: 9.5px;"
                               "  font-weight: 500;"
                               "}"
                               "QComboBox:hover {"
                               "  border-color: #06b6d4;"
                               "}"
                               "QComboBox::drop-down {"
                               "  subcontrol-origin: padding;"
                               "  subcontrol-position: top right;"
                               "  width: 12px;"
                               "  border-left: none;"
                               "}"
                               "QComboBox QAbstractItemView {"
                               "  background-color: #0f172a;"
                               "  color: #f1f5f9;"
                               "  border: 1px solid #334155;"
                               "  selection-background-color: #0284c7;"
                               "  selection-color: #ffffff;"
                               "  font-size: 10px;"
                               "}";

    const QString checkStyle = "QCheckBox {"
                               "  color: #94a3b8;"
                               "  font-size: 9.5px;"
                               "  spacing: 2px;"
                               "}"
                               "QCheckBox:hover {"
                               "  color: #f1f5f9;"
                               "}"
                               "QCheckBox::indicator {"
                               "  width: 11px;"
                               "  height: 11px;"
                               "  border-radius: 2px;"
                               "  border: 1px solid #475569;"
                               "  background: #1e293b;"
                               "}"
                               "QCheckBox::indicator:checked {"
                               "  background: #0284c7;"
                               "  border-color: #38bdf8;"
                               "}";

    // Power source badge (AC vs Battery)
    m_powerBadge = new QLabel("⚡ AC", this);
    m_powerBadge->setToolTip("Live Power Source");
    m_powerBadge->setAlignment(Qt::AlignCenter);
    m_powerBadge->setFixedSize(40, 22);
    m_powerBadge->setStyleSheet("QLabel {"
                                "  background-color: #064e3b;"
                                "  color: #34d399;"
                                "  border: 1px solid #059669;"
                                "  border-radius: 4px;"
                                "  font-family: monospace;"
                                "  font-size: 9.5px;"
                                "  font-weight: bold;"
                                "  padding: 1px 2px;"
                                "}");
    rootLayout->addWidget(m_powerBadge);

    // Pause status badge
    m_pauseStatusBadge = new QLabel("ACTIVE", this);
    m_pauseStatusBadge->setToolTip("Live Wallpaper Daemon Render State");
    m_pauseStatusBadge->setAlignment(Qt::AlignCenter);
    m_pauseStatusBadge->setFixedSize(48, 22);
    m_pauseStatusBadge->setStyleSheet("QLabel {"
                                      "  background-color: #0c4a6e;"
                                      "  color: #38bdf8;"
                                      "  border: 1px solid #0284c7;"
                                      "  border-radius: 4px;"
                                      "  font-family: monospace;"
                                      "  font-size: 9px;"
                                      "  font-weight: bold;"
                                      "  padding: 1px 2px;"
                                      "}");
    rootLayout->addWidget(m_pauseStatusBadge);

    // FPS Cap section
    auto *fpsLayout = new QHBoxLayout();
    fpsLayout->setSpacing(2);
    auto *fpsLabel = new QLabel("FPS:", this);
    fpsLabel->setStyleSheet("color: #64748b; font-size: 9.5px; font-weight: 600;");
    fpsLayout->addWidget(fpsLabel);

    m_fpsCombo = new QComboBox(this);
    m_fpsCombo->setFixedSize(54, 22);
    m_fpsCombo->setStyleSheet(comboStyle);
    m_fpsCombo->setToolTip("Target Render FPS Cap (or VSync for monitor refresh)");
    m_fpsCombo->addItem("VSync", QVariant(0u));
    m_fpsCombo->addItem("24", QVariant(24u));
    m_fpsCombo->addItem("30", QVariant(30u));
    m_fpsCombo->addItem("60", QVariant(60u));
    m_fpsCombo->addItem("120", QVariant(120u));
    m_fpsCombo->addItem("144", QVariant(144u));
    m_fpsCombo->setCurrentIndex(0); // VSync default
    connect(m_fpsCombo, &QComboBox::currentIndexChanged, this,
            &PowerGovernorWidget::onFpsComboChanged);
    fpsLayout->addWidget(m_fpsCombo);
    rootLayout->addLayout(fpsLayout);

    // Battery Policy section
    auto *batLayout = new QHBoxLayout();
    batLayout->setSpacing(2);
    auto *batLabel = new QLabel("Bat:", this);
    batLabel->setStyleSheet("color: #64748b; font-size: 9.5px; font-weight: 600;");
    batLayout->addWidget(batLabel);

    m_batteryPolicyCombo = new QComboBox(this);
    m_batteryPolicyCombo->setFixedSize(56, 22);
    m_batteryPolicyCombo->setStyleSheet(comboStyle);
    m_batteryPolicyCombo->setToolTip("Autonomous Battery Power Governor Policy");
    m_batteryPolicyCombo->addItem("Ignore", QVariant(static_cast<uint32_t>(WAYWAL_BATTERY_IGNORE)));
    m_batteryPolicyCombo->addItem("30 FPS",
                                  QVariant(static_cast<uint32_t>(WAYWAL_BATTERY_REDUCE_FPS)));
    m_batteryPolicyCombo->addItem("Pause",
                                  QVariant(static_cast<uint32_t>(WAYWAL_BATTERY_PAUSE_VIDEO)));
    m_batteryPolicyCombo->setCurrentIndex(0);
    connect(m_batteryPolicyCombo, &QComboBox::currentIndexChanged, this,
            &PowerGovernorWidget::onBatteryPolicyComboChanged);
    batLayout->addWidget(m_batteryPolicyCombo);
    rootLayout->addLayout(batLayout);

    // Auto-Pause check boxes
    auto *pauseLayout = new QHBoxLayout();
    pauseLayout->setSpacing(4);

    m_pauseFullscreenCheck = new QCheckBox("FS", this);
    m_pauseFullscreenCheck->setStyleSheet(checkStyle);
    m_pauseFullscreenCheck->setToolTip(
        "Auto-pause wallpaper rendering when a window is fullscreened");
    m_pauseFullscreenCheck->setChecked(m_pauseFullscreen);
    connect(m_pauseFullscreenCheck, &QCheckBox::toggled, this,
            &PowerGovernorWidget::onAutoPauseCheckChanged);
    pauseLayout->addWidget(m_pauseFullscreenCheck);

    m_pauseMaximizedCheck = new QCheckBox("Max", this);
    m_pauseMaximizedCheck->setStyleSheet(checkStyle);
    m_pauseMaximizedCheck->setToolTip("Auto-pause wallpaper rendering when windows are maximized");
    m_pauseMaximizedCheck->setChecked(m_pauseMaximized);
    connect(m_pauseMaximizedCheck, &QCheckBox::toggled, this,
            &PowerGovernorWidget::onAutoPauseCheckChanged);
    pauseLayout->addWidget(m_pauseMaximizedCheck);

    m_pauseOccludedCheck = new QCheckBox("Occl", this);
    m_pauseOccludedCheck->setStyleSheet(checkStyle);
    m_pauseOccludedCheck->setToolTip("Auto-pause rendering on workspaces without visible monitors");
    m_pauseOccludedCheck->setChecked(m_pauseOccluded);
    connect(m_pauseOccludedCheck, &QCheckBox::toggled, this,
            &PowerGovernorWidget::onAutoPauseCheckChanged);
    pauseLayout->addWidget(m_pauseOccludedCheck);

    rootLayout->addLayout(pauseLayout);
}

void PowerGovernorWidget::updateFromTelemetry(const waywal_telemetry_snapshot_t &snapshot)
{
    bool onBatteryChanged = (m_onBattery != (snapshot.on_battery != 0));
    m_onBattery = (snapshot.on_battery != 0);

    auto policy = static_cast<waywal_battery_policy_t>(snapshot.battery_policy);
    if (m_batteryPolicy != policy) {
        m_batteryPolicy = policy;
        if (m_batteryPolicyCombo) {
            QSignalBlocker blocker(m_batteryPolicyCombo);
            int idx = m_batteryPolicyCombo->findData(QVariant(static_cast<uint32_t>(policy)));
            if (idx >= 0) {
                m_batteryPolicyCombo->setCurrentIndex(idx);
            }
        }
    }

    m_globalPauseMask = snapshot.global_pause_mask;

    if (onBatteryChanged || true) {
        updateStatusBadges();
    }
}

void PowerGovernorWidget::updateStatusBadges()
{
    if (m_powerBadge) {
        if (m_onBattery) {
            m_powerBadge->setText("🔋 BAT");
            m_powerBadge->setStyleSheet("QLabel {"
                                        "  background-color: #713f12;"
                                        "  color: #fde047;"
                                        "  border: 1px solid #ca8a04;"
                                        "  border-radius: 4px;"
                                        "  font-family: monospace;"
                                        "  font-size: 10px;"
                                        "  font-weight: bold;"
                                        "  padding: 2px 4px;"
                                        "}");
        } else {
            m_powerBadge->setText("⚡ AC");
            m_powerBadge->setStyleSheet("QLabel {"
                                        "  background-color: #064e3b;"
                                        "  color: #34d399;"
                                        "  border: 1px solid #059669;"
                                        "  border-radius: 4px;"
                                        "  font-family: monospace;"
                                        "  font-size: 10px;"
                                        "  font-weight: bold;"
                                        "  padding: 2px 4px;"
                                        "}");
        }
    }

    if (m_pauseStatusBadge) {
        if (m_globalPauseMask == 0) {
            m_pauseStatusBadge->setText("ACTIVE");
            m_pauseStatusBadge->setStyleSheet("QLabel {"
                                              "  background-color: #064e3b;"
                                              "  color: #34d399;"
                                              "  border: 1px solid #059669;"
                                              "  border-radius: 4px;"
                                              "  font-family: monospace;"
                                              "  font-size: 9px;"
                                              "  font-weight: bold;"
                                              "  padding: 2px 4px;"
                                              "}");
        } else if (m_globalPauseMask & WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN) {
            m_pauseStatusBadge->setText("PAUSE:FS");
            m_pauseStatusBadge->setStyleSheet("QLabel {"
                                              "  background-color: #450a0a;"
                                              "  color: #f87171;"
                                              "  border: 1px solid #dc2626;"
                                              "  border-radius: 4px;"
                                              "  font-family: monospace;"
                                              "  font-size: 9px;"
                                              "  font-weight: bold;"
                                              "  padding: 2px 4px;"
                                              "}");
        } else if (m_globalPauseMask & WAYWAL_PAUSE_TOPLEVEL_MAXIMIZED) {
            m_pauseStatusBadge->setText("PAUSE:MAX");
            m_pauseStatusBadge->setStyleSheet("QLabel {"
                                              "  background-color: #450a0a;"
                                              "  color: #f87171;"
                                              "  border: 1px solid #dc2626;"
                                              "  border-radius: 4px;"
                                              "  font-family: monospace;"
                                              "  font-size: 9px;"
                                              "  font-weight: bold;"
                                              "  padding: 2px 4px;"
                                              "}");
        } else if (m_globalPauseMask & WAYWAL_PAUSE_WORKSPACE_OCCLUDED) {
            m_pauseStatusBadge->setText("PAUSE:OCCL");
            m_pauseStatusBadge->setStyleSheet("QLabel {"
                                              "  background-color: #3b2010;"
                                              "  color: #fbbf24;"
                                              "  border: 1px solid #b45309;"
                                              "  border-radius: 4px;"
                                              "  font-family: monospace;"
                                              "  font-size: 9px;"
                                              "  font-weight: bold;"
                                              "  padding: 2px 4px;"
                                              "}");
        } else if (m_globalPauseMask & WAYWAL_PAUSE_BATTERY_SAVER) {
            m_pauseStatusBadge->setText("PAUSE:BAT");
            m_pauseStatusBadge->setStyleSheet("QLabel {"
                                              "  background-color: #3b2010;"
                                              "  color: #fbbf24;"
                                              "  border: 1px solid #b45309;"
                                              "  border-radius: 4px;"
                                              "  font-family: monospace;"
                                              "  font-size: 9px;"
                                              "  font-weight: bold;"
                                              "  padding: 2px 4px;"
                                              "}");
        } else {
            m_pauseStatusBadge->setText("PAUSED");
            m_pauseStatusBadge->setStyleSheet("QLabel {"
                                              "  background-color: #334155;"
                                              "  color: #94a3b8;"
                                              "  border: 1px solid #475569;"
                                              "  border-radius: 4px;"
                                              "  font-family: monospace;"
                                              "  font-size: 9px;"
                                              "  font-weight: bold;"
                                              "  padding: 2px 4px;"
                                              "}");
        }
    }
}

void PowerGovernorWidget::setFpsCap(uint32_t fps)
{
    if (m_fpsCap != fps) {
        m_fpsCap = fps;
        if (m_fpsCombo) {
            QSignalBlocker blocker(m_fpsCombo);
            int idx = m_fpsCombo->findData(QVariant(fps));
            if (idx >= 0) {
                m_fpsCombo->setCurrentIndex(idx);
            }
        }
        Q_EMIT fpsCapChanged(m_fpsCap);
    }
}

void PowerGovernorWidget::setBatteryPolicy(waywal_battery_policy_t policy)
{
    if (m_batteryPolicy != policy) {
        m_batteryPolicy = policy;
        if (m_batteryPolicyCombo) {
            QSignalBlocker blocker(m_batteryPolicyCombo);
            int idx = m_batteryPolicyCombo->findData(QVariant(static_cast<uint32_t>(policy)));
            if (idx >= 0) {
                m_batteryPolicyCombo->setCurrentIndex(idx);
            }
        }
        Q_EMIT batteryPolicyChanged(m_batteryPolicy);
    }
}

void PowerGovernorWidget::setAutoPausePolicies(bool pauseFullscreen, bool pauseMaximized,
                                               bool pauseOccluded)
{
    bool changed = (m_pauseFullscreen != pauseFullscreen) || (m_pauseMaximized != pauseMaximized) ||
                   (m_pauseOccluded != pauseOccluded);

    m_pauseFullscreen = pauseFullscreen;
    m_pauseMaximized = pauseMaximized;
    m_pauseOccluded = pauseOccluded;

    if (m_pauseFullscreenCheck) {
        QSignalBlocker blocker(m_pauseFullscreenCheck);
        m_pauseFullscreenCheck->setChecked(pauseFullscreen);
    }
    if (m_pauseMaximizedCheck) {
        QSignalBlocker blocker(m_pauseMaximizedCheck);
        m_pauseMaximizedCheck->setChecked(pauseMaximized);
    }
    if (m_pauseOccludedCheck) {
        QSignalBlocker blocker(m_pauseOccludedCheck);
        m_pauseOccludedCheck->setChecked(pauseOccluded);
    }

    if (changed) {
        Q_EMIT autoPauseFlagsChanged(m_pauseFullscreen, m_pauseMaximized, m_pauseOccluded);
    }
}

void PowerGovernorWidget::onFpsComboChanged(int index)
{
    if (index >= 0 && m_fpsCombo) {
        uint32_t fps = m_fpsCombo->itemData(index).toUInt();
        if (m_fpsCap != fps) {
            m_fpsCap = fps;
            Q_EMIT fpsCapChanged(m_fpsCap);
        }
    }
}

void PowerGovernorWidget::onBatteryPolicyComboChanged(int index)
{
    if (index >= 0 && m_batteryPolicyCombo) {
        auto policy =
            static_cast<waywal_battery_policy_t>(m_batteryPolicyCombo->itemData(index).toUInt());
        if (m_batteryPolicy != policy) {
            m_batteryPolicy = policy;
            Q_EMIT batteryPolicyChanged(m_batteryPolicy);
        }
    }
}

void PowerGovernorWidget::onAutoPauseCheckChanged()
{
    bool fs = m_pauseFullscreenCheck ? m_pauseFullscreenCheck->isChecked() : false;
    bool max = m_pauseMaximizedCheck ? m_pauseMaximizedCheck->isChecked() : false;
    bool occl = m_pauseOccludedCheck ? m_pauseOccludedCheck->isChecked() : false;

    if (m_pauseFullscreen != fs || m_pauseMaximized != max || m_pauseOccluded != occl) {
        m_pauseFullscreen = fs;
        m_pauseMaximized = max;
        m_pauseOccluded = occl;
        Q_EMIT autoPauseFlagsChanged(m_pauseFullscreen, m_pauseMaximized, m_pauseOccluded);
    }
}

} // namespace wwal::studio
