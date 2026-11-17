// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio_footer_bar.hpp"

#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QFrame>
#include <QtGui/QFontMetrics>

#include <algorithm>

namespace wwal::studio {

namespace {

QFrame *createDivider(QWidget *parent)
{
    auto *divider = new QFrame(parent);
    divider->setObjectName(QStringLiteral("footerDivider"));
    divider->setFrameShape(QFrame::VLine);
    divider->setFrameShadow(QFrame::Plain);
    return divider;
}

QString elideAsciiRight(const QFontMetrics &fm, const QString &text, int maxPx)
{
    if (fm.horizontalAdvance(text) <= maxPx) {
        return text;
    }
    const QString ellipsis = QStringLiteral("...");
    const int ellW = fm.horizontalAdvance(ellipsis);
    if (maxPx <= ellW) {
        return ellipsis;
    }
    int len = static_cast<int>(text.size());
    while (len > 0 && fm.horizontalAdvance(text.left(len)) + ellW > maxPx) {
        --len;
    }
    return text.left(len) + ellipsis;
}

} // namespace

StudioFooterBar::StudioFooterBar(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(48);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    setupUi();

    m_statusMessageTimer.setSingleShot(true);
    connect(&m_statusMessageTimer, &QTimer::timeout, this,
            &StudioFooterBar::onStatusMessageTimeout);
}

QSize StudioFooterBar::minimumSizeHint() const
{
    const int hintH = std::max(48, QWidget::sizeHint().height());
    // Keep minimum width below MainWindow's 860 px minimum so docked layouts never overflow.
    return QSize(720, hintH);
}

void StudioFooterBar::setupUi()
{
    setStyleSheet(QStringLiteral("StudioFooterBar {"
                                 "  background-color: #12141a;"
                                 "  border-top: 1px solid #262b36;"
                                 "}"
                                 "QFrame#footerDivider {"
                                 "  color: #334155;"
                                 "  margin-top: 4px;"
                                 "  margin-bottom: 4px;"
                                 "}"
                                 "QLabel#footerStatusBanner {"
                                 "  background-color: #0369a1;"
                                 "  color: #ffffff;"
                                 "  border-radius: 4px;"
                                 "  font-size: 11px;"
                                 "  font-weight: 600;"
                                 "  padding: 3px 8px;"
                                 "  min-height: 22px;"
                                 "}"));

    auto *rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(6, 2, 6, 2);
    rootLayout->setSpacing(6);

    m_powerGovernor = new PowerGovernorWidget(this);
    connect(m_powerGovernor, &PowerGovernorWidget::fpsCapChanged, this,
            &StudioFooterBar::fpsCapChanged);
    connect(m_powerGovernor, &PowerGovernorWidget::batteryPolicyChanged, this,
            &StudioFooterBar::batteryPolicyChanged);
    connect(m_powerGovernor, &PowerGovernorWidget::autoPauseFlagsChanged, this,
            &StudioFooterBar::autoPauseFlagsChanged);
    rootLayout->addWidget(m_powerGovernor, 0);

    rootLayout->addWidget(createDivider(this));

    m_audioSpectrum = new AudioSpectrumWidget(this);
    connect(m_audioSpectrum, &AudioSpectrumWidget::sensitivityGainChanged, this,
            &StudioFooterBar::audioSensitivityGainChanged);
    connect(m_audioSpectrum, &AudioSpectrumWidget::muteOnAudioToggled, this,
            &StudioFooterBar::muteOnAudioToggled);
    rootLayout->addWidget(m_audioSpectrum, 1);

    rootLayout->addWidget(createDivider(this));

    m_telemetryHud = new TelemetryHudWidget(this);
    connect(m_telemetryHud, &TelemetryHudWidget::outputSelected, this,
            &StudioFooterBar::outputSelected);
    rootLayout->addWidget(m_telemetryHud, 0);

    m_statusMessageLabel = new QLabel(this);
    m_statusMessageLabel->setObjectName(QStringLiteral("footerStatusBanner"));
    m_statusMessageLabel->setVisible(false);
    m_statusMessageLabel->setMaximumWidth(220);
    m_statusMessageLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
}

void StudioFooterBar::updateResponsiveLayout(int totalWidth)
{
    if (m_powerGovernor) {
        m_powerGovernor->setCompactMode(totalWidth < 1320);
    }
    if (m_telemetryHud) {
        if (totalWidth >= 1320) {
            m_telemetryHud->setCompactTier(TelemetryHudWidget::CompactTier::Full);
        } else if (totalWidth >= 1120) {
            m_telemetryHud->setCompactTier(TelemetryHudWidget::CompactTier::Compact);
        } else {
            m_telemetryHud->setCompactTier(TelemetryHudWidget::CompactTier::Minimal);
        }
    }
    if (layout()) {
        layout()->setSpacing(totalWidth < 960 ? 4 : 6);
    }
    if (m_statusMessageLabel && m_statusMessageLabel->isVisible()) {
        const QSize hint = m_statusMessageLabel->sizeHint();
        const int w = std::min(220, std::max(80, hint.width()));
        const int h = std::max(28, hint.height());
        const int x = std::max(6, totalWidth - w - 6);
        const int y = std::max(2, (height() - h) / 2);
        m_statusMessageLabel->setGeometry(x, y, w, h);
        m_statusMessageLabel->raise();
    }
}

void StudioFooterBar::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateResponsiveLayout(width());
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
        updateResponsiveLayout(width());
        return;
    }
    const QFontMetrics fm(m_statusMessageLabel->font());
    m_statusMessageLabel->setText(elideAsciiRight(fm, msg, 196));
    m_statusMessageLabel->setToolTip(msg);
    m_statusMessageLabel->setVisible(true);
    updateResponsiveLayout(width());

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
        updateResponsiveLayout(width());
    }
}

} // namespace wwal::studio
