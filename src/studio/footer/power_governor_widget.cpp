// SPDX-License-Identifier: GPL-3.0-or-later
#include "power_governor_widget.hpp"
#include "audio_spectrum_widget.hpp"

#include <QtWidgets/QHBoxLayout>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStringList>
#include <QtGui/QFontMetrics>

#include <algorithm>
#include <array>

namespace wwal::studio {

namespace {

constexpr int kCustomFpsRole = Qt::UserRole + 1;

struct PauseReasonMeta {
    uint16_t flag;
    const char *shortTag;
    const char *description;
};

constexpr std::array<PauseReasonMeta, 7> kPauseReasons = {{
    {WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN, "FS", "Fullscreen Window Active"},
    {WAYWAL_PAUSE_TOPLEVEL_MAXIMIZED, "MAX", "Maximized Window Active"},
    {WAYWAL_PAUSE_WORKSPACE_OCCLUDED, "OCCL", "Workspace Occluded"},
    {WAYWAL_PAUSE_BATTERY_SAVER, "BAT", "Battery Saver Policy"},
    {WAYWAL_PAUSE_DPMS_OR_UNMAPPED, "DPMS", "Display Off / Unmapped (DPMS)"},
    {WAYWAL_PAUSE_SESSION_IDLE, "IDLE", "Session Idle"},
    {WAYWAL_PAUSE_USER_MANUAL, "USER", "Manual User Pause"},
}};

int computeComboMinWidth(const QComboBox *combo)
{
    if (!combo) {
        return 64;
    }
    const QFontMetrics fm(combo->font());
    int maxTextW = 0;
    for (int i = 0; i < combo->count(); ++i) {
        maxTextW = std::max(maxTextW, fm.horizontalAdvance(combo->itemText(i)));
    }
    // Reserve space for horizontal padding, frame border, and dropdown arrow indicator.
    return maxTextW + 22;
}

} // namespace

PowerGovernorWidget::PowerGovernorWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(44);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    setupUi();
    updateStatusBadges();
}

void PowerGovernorWidget::setupUi()
{
    setStyleSheet(QStringLiteral("QLabel#govPrefixLabel {"
                                 "  color: #94a3b8;"
                                 "  font-size: 11px;"
                                 "  font-weight: 600;"
                                 "}"
                                 "QComboBox#govCombo {"
                                 "  background-color: #181b22;"
                                 "  color: #f1f5f9;"
                                 "  border: 1px solid #2c3240;"
                                 "  border-radius: 4px;"
                                 "  padding: 2px 14px 2px 5px;"
                                 "  min-height: 24px;"
                                 "  font-size: 11px;"
                                 "  font-weight: 500;"
                                 "}"
                                 "QComboBox#govCombo:hover {"
                                 "  border-color: #38bdf8;"
                                 "}"
                                 "QComboBox#govCombo:focus {"
                                 "  border: 2px solid #38bdf8;"
                                 "}"
                                 "QComboBox#govCombo::drop-down {"
                                 "  subcontrol-origin: padding;"
                                 "  subcontrol-position: top right;"
                                 "  width: 13px;"
                                 "  border-left: none;"
                                 "}"
                                 "QComboBox#govCombo QAbstractItemView {"
                                 "  background-color: #12141a;"
                                 "  color: #f1f5f9;"
                                 "  border: 1px solid #2c3240;"
                                 "  selection-background-color: #0369a1;"
                                 "  selection-color: #ffffff;"
                                 "  font-size: 11px;"
                                 "}"
                                 "QCheckBox#govCheck {"
                                 "  color: #94a3b8;"
                                 "  font-size: 11px;"
                                 "  spacing: 2px;"
                                 "  min-height: 24px;"
                                 "  padding: 0px 2px;"
                                 "  border: 2px solid transparent;"
                                 "  border-radius: 4px;"
                                 "}"
                                 "QCheckBox#govCheck:hover, QCheckBox#govCheck:checked {"
                                 "  color: #f1f5f9;"
                                 "}"
                                 "QCheckBox#govCheck:focus {"
                                 "  border: 2px solid #38bdf8;"
                                 "}"
                                 "QCheckBox#govCheck::indicator {"
                                 "  width: 14px;"
                                 "  height: 14px;"
                                 "  border-radius: 3px;"
                                 "  border: 1px solid #64748b;"
                                 "  background: #181b22;"
                                 "}"
                                 "QCheckBox#govCheck::indicator:checked {"
                                 "  background: #0284c7;"
                                 "  border-color: #38bdf8;"
                                 "}"));

    auto *rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(2, 2, 2, 2);
    rootLayout->setSpacing(4);

    m_powerBadge = new detail::FooterBadgeLabel(QStringLiteral("AC"), this, QStringLiteral("BAT"),
                                                detail::tokens::MinTargetPx, 4);
    m_powerBadge->setToolTip(QStringLiteral("Power source: AC Mains"));
    m_powerBadge->setAccessibleName(QStringLiteral("Power source"));
    rootLayout->addWidget(m_powerBadge);

    m_pauseStatusBadge =
        new detail::FooterBadgeLabel(QStringLiteral("ACTIVE"), this, QStringLiteral("PAUSE:OCCL"),
                                     detail::tokens::MinTargetPx, 4);
    m_pauseStatusBadge->setToolTip(QStringLiteral("Daemon render state: ACTIVE"));
    m_pauseStatusBadge->setAccessibleName(QStringLiteral("Daemon pause state"));
    rootLayout->addWidget(m_pauseStatusBadge);

    auto *fpsLayout = new QHBoxLayout();
    fpsLayout->setSpacing(3);
    fpsLayout->setContentsMargins(0, 0, 0, 0);

    m_fpsLabel = new QLabel(QStringLiteral("FPS:"), this);
    m_fpsLabel->setObjectName(QStringLiteral("govPrefixLabel"));
    fpsLayout->addWidget(m_fpsLabel);

    m_fpsCombo = new QComboBox(this);
    m_fpsCombo->setObjectName(QStringLiteral("govCombo"));
    m_fpsCombo->setMinimumHeight(detail::tokens::MinTargetPx);
    m_fpsCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    QFont comboFont = m_fpsCombo->font();
    comboFont.setPixelSize(detail::tokens::MinFontPx);
    m_fpsCombo->setFont(comboFont);
    m_fpsCombo->setToolTip(QStringLiteral("Frame rate cap (or VSync for monitor refresh)"));
    m_fpsCombo->setAccessibleName(QStringLiteral("Frame rate cap"));
    m_fpsCombo->addItem(QStringLiteral("VSync"), QVariant(0u));
    m_fpsCombo->addItem(QStringLiteral("24"), QVariant(24u));
    m_fpsCombo->addItem(QStringLiteral("30"), QVariant(30u));
    m_fpsCombo->addItem(QStringLiteral("60"), QVariant(60u));
    m_fpsCombo->addItem(QStringLiteral("120"), QVariant(120u));
    m_fpsCombo->addItem(QStringLiteral("144"), QVariant(144u));
    m_fpsCombo->setMinimumWidth(computeComboMinWidth(m_fpsCombo));
    m_fpsCombo->setMaximumWidth(std::max(72, computeComboMinWidth(m_fpsCombo) + 8));
    m_fpsCombo->setCurrentIndex(0);
    connect(m_fpsCombo, &QComboBox::currentIndexChanged, this,
            &PowerGovernorWidget::onFpsComboChanged);
    fpsLayout->addWidget(m_fpsCombo);
    rootLayout->addLayout(fpsLayout);

    auto *batLayout = new QHBoxLayout();
    batLayout->setSpacing(3);
    batLayout->setContentsMargins(0, 0, 0, 0);

    m_batLabel = new QLabel(QStringLiteral("Bat:"), this);
    m_batLabel->setObjectName(QStringLiteral("govPrefixLabel"));
    batLayout->addWidget(m_batLabel);

    m_batteryPolicyCombo = new QComboBox(this);
    m_batteryPolicyCombo->setObjectName(QStringLiteral("govCombo"));
    m_batteryPolicyCombo->setMinimumHeight(detail::tokens::MinTargetPx);
    m_batteryPolicyCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_batteryPolicyCombo->setFont(comboFont);
    m_batteryPolicyCombo->setToolTip(QStringLiteral("Behavior when running on battery power"));
    m_batteryPolicyCombo->setAccessibleName(QStringLiteral("Battery policy"));
    m_batteryPolicyCombo->addItem(QStringLiteral("Ignore"),
                                  QVariant(static_cast<uint32_t>(WAYWAL_BATTERY_IGNORE)));
    m_batteryPolicyCombo->addItem(QStringLiteral("30 FPS"),
                                  QVariant(static_cast<uint32_t>(WAYWAL_BATTERY_REDUCE_FPS)));
    m_batteryPolicyCombo->addItem(QStringLiteral("Pause"),
                                  QVariant(static_cast<uint32_t>(WAYWAL_BATTERY_PAUSE_VIDEO)));
    m_batteryPolicyCombo->setMinimumWidth(computeComboMinWidth(m_batteryPolicyCombo));
    m_batteryPolicyCombo->setMaximumWidth(
        std::max(76, computeComboMinWidth(m_batteryPolicyCombo) + 8));
    m_batteryPolicyCombo->setCurrentIndex(0);
    connect(m_batteryPolicyCombo, &QComboBox::currentIndexChanged, this,
            &PowerGovernorWidget::onBatteryPolicyComboChanged);
    batLayout->addWidget(m_batteryPolicyCombo);
    rootLayout->addLayout(batLayout);

    auto *pauseLayout = new QHBoxLayout();
    pauseLayout->setSpacing(2);
    pauseLayout->setContentsMargins(0, 0, 0, 0);

    m_pauseFullscreenCheck = new QCheckBox(QStringLiteral("FS"), this);
    m_pauseFullscreenCheck->setObjectName(QStringLiteral("govCheck"));
    m_pauseFullscreenCheck->setMinimumSize(detail::tokens::MinTargetPx,
                                           detail::tokens::MinTargetPx);
    m_pauseFullscreenCheck->setToolTip(
        QStringLiteral("Pause rendering when a window is fullscreen"));
    m_pauseFullscreenCheck->setAccessibleName(QStringLiteral("Auto-pause on fullscreen"));
    m_pauseFullscreenCheck->setChecked(m_pauseFullscreen);
    connect(m_pauseFullscreenCheck, &QCheckBox::toggled, this,
            &PowerGovernorWidget::onAutoPauseCheckChanged);
    pauseLayout->addWidget(m_pauseFullscreenCheck);

    m_pauseMaximizedCheck = new QCheckBox(QStringLiteral("Max"), this);
    m_pauseMaximizedCheck->setObjectName(QStringLiteral("govCheck"));
    m_pauseMaximizedCheck->setMinimumSize(detail::tokens::MinTargetPx, detail::tokens::MinTargetPx);
    m_pauseMaximizedCheck->setToolTip(QStringLiteral("Pause rendering when windows are maximized"));
    m_pauseMaximizedCheck->setAccessibleName(QStringLiteral("Auto-pause on maximized window"));
    m_pauseMaximizedCheck->setChecked(m_pauseMaximized);
    connect(m_pauseMaximizedCheck, &QCheckBox::toggled, this,
            &PowerGovernorWidget::onAutoPauseCheckChanged);
    pauseLayout->addWidget(m_pauseMaximizedCheck);

    m_pauseOccludedCheck = new QCheckBox(QStringLiteral("Occl"), this);
    m_pauseOccludedCheck->setObjectName(QStringLiteral("govCheck"));
    m_pauseOccludedCheck->setMinimumSize(detail::tokens::MinTargetPx, detail::tokens::MinTargetPx);
    m_pauseOccludedCheck->setToolTip(QStringLiteral("Pause rendering on inactive workspaces"));
    m_pauseOccludedCheck->setAccessibleName(QStringLiteral("Auto-pause on occluded workspace"));
    m_pauseOccludedCheck->setChecked(m_pauseOccluded);
    connect(m_pauseOccludedCheck, &QCheckBox::toggled, this,
            &PowerGovernorWidget::onAutoPauseCheckChanged);
    pauseLayout->addWidget(m_pauseOccludedCheck);

    rootLayout->addLayout(pauseLayout);
}

void PowerGovernorWidget::setCompactMode(bool compact)
{
    if (m_compactMode == compact) {
        return;
    }
    m_compactMode = compact;
    if (m_fpsLabel) {
        m_fpsLabel->setVisible(!compact);
    }
    if (m_batLabel) {
        m_batLabel->setVisible(!compact);
    }
    if (layout()) {
        layout()->setContentsMargins(compact ? 0 : 2, 2, compact ? 0 : 2, 2);
        layout()->setSpacing(compact ? 2 : 4);
    }
}

void PowerGovernorWidget::updateFromTelemetry(const waywal_telemetry_snapshot_t &snapshot)
{
    const bool onBatteryChanged = (m_onBattery != (snapshot.on_battery != 0));
    m_onBattery = (snapshot.on_battery != 0);

    const auto policy = static_cast<waywal_battery_policy_t>(snapshot.battery_policy);
    if (m_batteryPolicy != policy) {
        m_batteryPolicy = policy;
        if (m_batteryPolicyCombo) {
            const QSignalBlocker blocker(m_batteryPolicyCombo);
            const int idx = m_batteryPolicyCombo->findData(QVariant(static_cast<uint32_t>(policy)));
            if (idx >= 0) {
                m_batteryPolicyCombo->setCurrentIndex(idx);
            }
        }
    }

    const bool pauseMaskChanged = (m_globalPauseMask != snapshot.global_pause_mask);
    m_globalPauseMask = snapshot.global_pause_mask;

    if (onBatteryChanged || pauseMaskChanged || m_firstTelemetrySync) {
        m_firstTelemetrySync = false;
        updateStatusBadges();
    }
}

void PowerGovernorWidget::updateStatusBadges()
{
    if (m_powerBadge) {
        if (m_onBattery) {
            m_powerBadge->setBadgeText(QStringLiteral("BAT"));
            m_powerBadge->setBadgeColors(QColor::fromRgba(detail::tokens::YellowBg),
                                         QColor::fromRgba(detail::tokens::YellowFg),
                                         QColor::fromRgba(detail::tokens::YellowBorder));
            m_powerBadge->setToolTip(QStringLiteral("Power source: Battery (DC)"));
            m_powerBadge->setAccessibleName(QStringLiteral("Power source: Battery"));
        } else {
            m_powerBadge->setBadgeText(QStringLiteral("AC"));
            m_powerBadge->setBadgeColors(QColor::fromRgba(detail::tokens::EmeraldBg),
                                         QColor::fromRgba(detail::tokens::EmeraldFg),
                                         QColor::fromRgba(detail::tokens::EmeraldBorder));
            m_powerBadge->setToolTip(QStringLiteral("Power source: AC Mains"));
            m_powerBadge->setAccessibleName(QStringLiteral("Power source: AC Mains"));
        }
    }

    if (m_pauseStatusBadge) {
        if (m_globalPauseMask == 0) {
            m_pauseStatusBadge->setBadgeText(QStringLiteral("ACTIVE"));
            m_pauseStatusBadge->setBadgeColors(QColor::fromRgba(detail::tokens::EmeraldBg),
                                               QColor::fromRgba(detail::tokens::EmeraldFg),
                                               QColor::fromRgba(detail::tokens::EmeraldBorder));
            m_pauseStatusBadge->setToolTip(
                QStringLiteral("Daemon render state: ACTIVE (no active pause reasons)"));
            m_pauseStatusBadge->setAccessibleName(QStringLiteral("Daemon render state: Active"));
            return;
        }

        QStringList shortTags;
        QStringList descriptions;
        uint16_t remaining = m_globalPauseMask;
        for (const PauseReasonMeta &meta : kPauseReasons) {
            if ((remaining & meta.flag) != 0u) {
                shortTags.push_back(QString::fromLatin1(meta.shortTag));
                descriptions.push_back(QStringLiteral("- ") +
                                       QString::fromLatin1(meta.description));
                remaining = static_cast<uint16_t>(remaining & ~meta.flag);
            }
        }
        if (remaining != 0u) {
            shortTags.push_back(QStringLiteral("EXT"));
            descriptions.push_back(
                QStringLiteral("- External Pause Reason (0x%1)").arg(remaining, 0, 16));
        }

        QString badgeText;
        if (shortTags.size() == 1) {
            badgeText = QStringLiteral("PAUSE:%1").arg(shortTags[0]);
        } else if (shortTags.size() == 2) {
            badgeText = QStringLiteral("PAUSE:%1+%2").arg(shortTags[0], shortTags[1]);
        } else {
            badgeText = QStringLiteral("PAUSE:%1+%2").arg(shortTags[0]).arg(shortTags.size() - 1);
        }

        const QString fullTip =
            QStringLiteral("Daemon render paused:\n") + descriptions.join(QLatin1Char('\n'));
        m_pauseStatusBadge->setBadgeText(badgeText);
        m_pauseStatusBadge->setToolTip(fullTip);
        m_pauseStatusBadge->setAccessibleName(
            QStringLiteral("Daemon render paused: %1").arg(shortTags.join(QStringLiteral(", "))));

        if ((m_globalPauseMask &
             (WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN | WAYWAL_PAUSE_TOPLEVEL_MAXIMIZED)) != 0u) {
            m_pauseStatusBadge->setBadgeColors(QColor::fromRgba(detail::tokens::CrimsonBg),
                                               QColor::fromRgba(detail::tokens::CrimsonFg),
                                               QColor::fromRgba(detail::tokens::CrimsonBorder));
        } else if ((m_globalPauseMask &
                    (WAYWAL_PAUSE_WORKSPACE_OCCLUDED | WAYWAL_PAUSE_BATTERY_SAVER)) != 0u) {
            m_pauseStatusBadge->setBadgeColors(QColor::fromRgba(detail::tokens::AmberBg),
                                               QColor::fromRgba(detail::tokens::AmberFg),
                                               QColor::fromRgba(detail::tokens::AmberBorder));
        } else {
            m_pauseStatusBadge->setBadgeColors(QColor::fromRgba(detail::tokens::SurfacePausedSlate),
                                               QColor::fromRgba(detail::tokens::TextPrimary),
                                               QColor::fromRgba(detail::tokens::BorderSlate));
        }
    }
}

void PowerGovernorWidget::syncFpsComboSelection(uint32_t fps)
{
    if (!m_fpsCombo) {
        return;
    }
    const QSignalBlocker blocker(m_fpsCombo);

    // Remove any previous non-preset entry before locating or adding the active cap.
    for (int i = m_fpsCombo->count() - 1; i >= 0; --i) {
        if (m_fpsCombo->itemData(i, kCustomFpsRole).toBool()) {
            m_fpsCombo->removeItem(i);
        }
    }

    int idx = m_fpsCombo->findData(QVariant(fps));
    if (idx < 0) {
        m_fpsCombo->addItem(QString::number(fps), QVariant(fps));
        idx = m_fpsCombo->count() - 1;
        m_fpsCombo->setItemData(idx, true, kCustomFpsRole);
        m_fpsCombo->setToolTip(
            QStringLiteral("Frame rate cap: %1 FPS (custom non-preset)").arg(fps));
    } else {
        m_fpsCombo->setToolTip(QStringLiteral("Frame rate cap (or VSync for monitor refresh)"));
    }
    m_fpsCombo->setCurrentIndex(idx);
    const int minW = computeComboMinWidth(m_fpsCombo);
    m_fpsCombo->setMinimumWidth(minW);
    m_fpsCombo->setMaximumWidth(std::max(72, minW + 8));
}

void PowerGovernorWidget::setFpsCap(uint32_t fps)
{
    syncFpsComboSelection(fps);
    if (m_fpsCap != fps) {
        m_fpsCap = fps;
        Q_EMIT fpsCapChanged(m_fpsCap);
    }
}

void PowerGovernorWidget::setBatteryPolicy(waywal_battery_policy_t policy)
{
    if (m_batteryPolicy != policy) {
        m_batteryPolicy = policy;
        if (m_batteryPolicyCombo) {
            const QSignalBlocker blocker(m_batteryPolicyCombo);
            const int idx = m_batteryPolicyCombo->findData(QVariant(static_cast<uint32_t>(policy)));
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
    const bool changed = (m_pauseFullscreen != pauseFullscreen) ||
                         (m_pauseMaximized != pauseMaximized) || (m_pauseOccluded != pauseOccluded);

    m_pauseFullscreen = pauseFullscreen;
    m_pauseMaximized = pauseMaximized;
    m_pauseOccluded = pauseOccluded;

    if (m_pauseFullscreenCheck) {
        const QSignalBlocker blocker(m_pauseFullscreenCheck);
        m_pauseFullscreenCheck->setChecked(pauseFullscreen);
    }
    if (m_pauseMaximizedCheck) {
        const QSignalBlocker blocker(m_pauseMaximizedCheck);
        m_pauseMaximizedCheck->setChecked(pauseMaximized);
    }
    if (m_pauseOccludedCheck) {
        const QSignalBlocker blocker(m_pauseOccludedCheck);
        m_pauseOccludedCheck->setChecked(pauseOccluded);
    }

    if (changed) {
        Q_EMIT autoPauseFlagsChanged(m_pauseFullscreen, m_pauseMaximized, m_pauseOccluded);
    }
}

void PowerGovernorWidget::onFpsComboChanged(int index)
{
    if (index >= 0 && m_fpsCombo) {
        const QVariant data = m_fpsCombo->itemData(index);
        if (!data.isValid()) {
            return;
        }
        const uint32_t fps = data.toUInt();
        if (m_fpsCap != fps) {
            m_fpsCap = fps;
            Q_EMIT fpsCapChanged(m_fpsCap);
        }
    }
}

void PowerGovernorWidget::onBatteryPolicyComboChanged(int index)
{
    if (index >= 0 && m_batteryPolicyCombo) {
        const QVariant data = m_batteryPolicyCombo->itemData(index);
        if (!data.isValid()) {
            return;
        }
        const auto policy = static_cast<waywal_battery_policy_t>(data.toUInt());
        if (m_batteryPolicy != policy) {
            m_batteryPolicy = policy;
            Q_EMIT batteryPolicyChanged(m_batteryPolicy);
        }
    }
}

void PowerGovernorWidget::onAutoPauseCheckChanged()
{
    const bool fs = m_pauseFullscreenCheck ? m_pauseFullscreenCheck->isChecked() : false;
    const bool max = m_pauseMaximizedCheck ? m_pauseMaximizedCheck->isChecked() : false;
    const bool occl = m_pauseOccludedCheck ? m_pauseOccludedCheck->isChecked() : false;

    if (m_pauseFullscreen != fs || m_pauseMaximized != max || m_pauseOccluded != occl) {
        m_pauseFullscreen = fs;
        m_pauseMaximized = max;
        m_pauseOccluded = occl;
        Q_EMIT autoPauseFlagsChanged(m_pauseFullscreen, m_pauseMaximized, m_pauseOccluded);
    }
}

} // namespace wwal::studio
