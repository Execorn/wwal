// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio_footer_bar.hpp"

#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QFrame>

namespace wwal::studio {

namespace {

QFrame *createDivider(QWidget *parent)
{
    auto *divider = new QFrame(parent);
    divider->setFrameShape(QFrame::VLine);
    divider->setFrameShadow(QFrame::Plain);
    divider->setStyleSheet("QFrame { color: #334155; margin-top: 4px; margin-bottom: 4px; }");
    return divider;
}

} // namespace

StudioFooterBar::StudioFooterBar(QWidget *parent) : QWidget(parent)
{
    setFixedHeight(48);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    setupUi();

    m_statusMessageTimer.setSingleShot(true);
    connect(&m_statusMessageTimer, &QTimer::timeout, this,
            &StudioFooterBar::onStatusMessageTimeout);
}

void StudioFooterBar::setupUi()
{
    // Top-level footer styling
    setStyleSheet("StudioFooterBar {"
                  "  background-color: #12141a;"
                  "  border-top: 1px solid #262b36;"
                  "}");

    auto *rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(4, 2, 4, 2);
    rootLayout->setSpacing(6);

    // 1. Left section: Power Governor
    m_powerGovernor = new PowerGovernorWidget(this);
    connect(m_powerGovernor, &PowerGovernorWidget::fpsCapChanged, this,
            &StudioFooterBar::fpsCapChanged);
    connect(m_powerGovernor, &PowerGovernorWidget::batteryPolicyChanged, this,
            &StudioFooterBar::batteryPolicyChanged);
    connect(m_powerGovernor, &PowerGovernorWidget::autoPauseFlagsChanged, this,
            &StudioFooterBar::autoPauseFlagsChanged);
    rootLayout->addWidget(m_powerGovernor, 0);

    rootLayout->addWidget(createDivider(this));

    // 2. Center section: 64-Band Bark-Scale Audio Visualizer & Controls
    m_audioSpectrum = new AudioSpectrumWidget(this);
    connect(m_audioSpectrum, &AudioSpectrumWidget::sensitivityGainChanged, this,
            &StudioFooterBar::audioSensitivityGainChanged);
    connect(m_audioSpectrum, &AudioSpectrumWidget::muteOnAudioToggled, this,
            &StudioFooterBar::muteOnAudioToggled);
    rootLayout->addWidget(m_audioSpectrum, 1); // Expand to fill available center space

    rootLayout->addWidget(createDivider(this));

    // 3. Right section: Streaming Telemetry HUD & DMA-BUF Ring Visualizer
    m_telemetryHud = new TelemetryHudWidget(this);
    connect(m_telemetryHud, &TelemetryHudWidget::outputSelected, this,
            &StudioFooterBar::outputSelected);
    rootLayout->addWidget(m_telemetryHud, 0);

    // Status Message label (initially hidden/empty, floats or displays when active)
    m_statusMessageLabel = new QLabel(this);
    m_statusMessageLabel->setVisible(false);
    m_statusMessageLabel->setStyleSheet("QLabel {"
                                        "  background-color: #0284c7;"
                                        "  color: #ffffff;"
                                        "  border-radius: 3px;"
                                        "  font-size: 10px;"
                                        "  font-weight: 500;"
                                        "  padding: 2px 6px;"
                                        "}");
    rootLayout->addWidget(m_statusMessageLabel);
}

void StudioFooterBar::updateTelemetry(const waywal_telemetry_snapshot_t &snapshot)
{
    if (m_powerGovernor) {
        m_powerGovernor->updateFromTelemetry(snapshot);
    }
    if (m_audioSpectrum) {
        m_audioSpectrum->updateFromTelemetry(snapshot);
    }
    if (m_telemetryHud) {
        m_telemetryHud->updateTelemetry(snapshot);
    }
}

void StudioFooterBar::setStatusMessage(const QString &msg, int timeoutMs)
{
    if (!m_statusMessageLabel) {
        return;
    }
    if (msg.isEmpty()) {
        m_statusMessageLabel->setVisible(false);
        m_statusMessageTimer.stop();
        return;
    }
    m_statusMessageLabel->setText(msg);
    m_statusMessageLabel->setVisible(true);

    if (timeoutMs > 0) {
        m_statusMessageTimer.start(timeoutMs);
    } else {
        m_statusMessageTimer.stop();
    }
}

void StudioFooterBar::onStatusMessageTimeout()
{
    if (m_statusMessageLabel) {
        m_statusMessageLabel->setVisible(false);
    }
}

} // namespace wwal::studio
