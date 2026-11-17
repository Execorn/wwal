// SPDX-License-Identifier: GPL-3.0-or-later
#include "telemetry_hud_widget.hpp"
#include "audio_spectrum_widget.hpp"

#include <QtWidgets/QHBoxLayout>
#include <QtCore/QMetaObject>
#include <QtCore/QSignalBlocker>
#include <QtGui/QFontMetrics>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace wwal::studio {

namespace {

QString extractBoundedUtf8(const char *buf, size_t maxLen)
{
    if (!buf || maxLen == 0) {
        return {};
    }
    const void *nul = std::memchr(buf, '\0', maxLen);
    const size_t len = nul ? static_cast<size_t>(static_cast<const char *>(nul) - buf) : maxLen;
    return QString::fromUtf8(buf, static_cast<int>(len));
}

const char *boStateShortName(waywal_bo_slot_state_t st) noexcept
{
    switch (st) {
    case WAYWAL_BO_STATE_FREE:
        return "FREE";
    case WAYWAL_BO_STATE_RENDERING:
        return "REND";
    case WAYWAL_BO_STATE_COMMITTED:
        return "COMM";
    case WAYWAL_BO_STATE_SCANOUT_LOCKED:
        return "SCAN";
    default:
        return "UNK";
    }
}

} // namespace

float TelemetryHudWidget::readCurrentProcessRssMb() noexcept
{
    FILE *f = std::fopen("/proc/self/status", "r");
    if (!f) {
        return 0.0f;
    }
    char line[256];
    unsigned long vmrssKb = 0;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            if (std::sscanf(line + 6, "%lu", &vmrssKb) == 1) {
                break;
            }
        }
    }
    std::fclose(f);
    return static_cast<float>(vmrssKb) / 1024.0f;
}

TelemetryHudWidget::TelemetryHudWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(44);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

    setupUi();
    updateLabels();
    updateSlotIndicators();

    connect(&m_refreshTimer, &QTimer::timeout, this, &TelemetryHudWidget::onRefreshTimerTick);
}

TelemetryHudWidget::~TelemetryHudWidget()
{
    m_refreshTimer.stop();
    stopRssWorker();
}

void TelemetryHudWidget::setupUi()
{
    setStyleSheet(QStringLiteral("QComboBox#telemetryOutputCombo {"
                                 "  background-color: #181b22;"
                                 "  color: #f1f5f9;"
                                 "  border: 1px solid #2c3240;"
                                 "  border-radius: 4px;"
                                 "  padding: 2px 14px 2px 5px;"
                                 "  min-height: 24px;"
                                 "  font-size: 11px;"
                                 "  font-weight: bold;"
                                 "}"
                                 "QComboBox#telemetryOutputCombo:hover {"
                                 "  border-color: #38bdf8;"
                                 "}"
                                 "QComboBox#telemetryOutputCombo:focus {"
                                 "  border: 2px solid #38bdf8;"
                                 "}"
                                 "QComboBox#telemetryOutputCombo::drop-down {"
                                 "  subcontrol-origin: padding;"
                                 "  subcontrol-position: top right;"
                                 "  width: 13px;"
                                 "  border-left: none;"
                                 "}"
                                 "QComboBox#telemetryOutputCombo QAbstractItemView {"
                                 "  background-color: #12141a;"
                                 "  color: #f1f5f9;"
                                 "  border: 1px solid #2c3240;"
                                 "  selection-background-color: #0369a1;"
                                 "  selection-color: #ffffff;"
                                 "  font-size: 11px;"
                                 "}"));

    auto *rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(2, 2, 2, 2);
    rootLayout->setSpacing(4);

    m_outputCombo = new QComboBox(this);
    m_outputCombo->setObjectName(QStringLiteral("telemetryOutputCombo"));
    m_outputCombo->setMinimumHeight(detail::tokens::MinTargetPx);
    m_outputCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    QFont comboFont = m_outputCombo->font();
    comboFont.setPixelSize(detail::tokens::MinFontPx);
    comboFont.setBold(true);
    m_outputCombo->setFont(comboFont);
    const QFontMetrics comboFm(comboFont);
    m_outputCombo->setMinimumWidth(
        std::max(60, comboFm.horizontalAdvance(QStringLiteral("DP-1")) + 22));
    m_outputCombo->setMaximumWidth(84);
    m_outputCombo->addItem(QStringLiteral("ALL"), QVariant(-1));
    m_outputCombo->setToolTip(QStringLiteral("Selected Wayland output"));
    m_outputCombo->setAccessibleName(QStringLiteral("Selected Wayland output"));
    connect(m_outputCombo, &QComboBox::currentIndexChanged, this,
            &TelemetryHudWidget::onOutputComboChanged);
    rootLayout->addWidget(m_outputCombo);

    m_fpsBadge = new detail::FooterBadgeLabel(
        QStringLiteral("0 FPS"), this, QStringLiteral("144 FPS"), detail::tokens::MinTargetPx, 4);
    m_fpsBadge->setToolTip(QStringLiteral("Display scanout frame rate"));
    m_fpsBadge->setAccessibleName(QStringLiteral("Display scanout frame rate"));
    rootLayout->addWidget(m_fpsBadge);

    m_computeBadge = new detail::FooterBadgeLabel(
        QStringLiteral("0us"), this, QStringLiteral("12.3ms"), detail::tokens::MinTargetPx, 4);
    m_computeBadge->setBadgeColors(QColor::fromRgba(detail::tokens::SurfaceCard),
                                   QColor::fromRgba(detail::tokens::SkyFg),
                                   QColor::fromRgba(detail::tokens::BorderSubtle));
    m_computeBadge->setToolTip(QStringLiteral("GPU compute pass duration"));
    m_computeBadge->setAccessibleName(QStringLiteral("GPU compute pass duration"));
    rootLayout->addWidget(m_computeBadge);

    m_vramBadge =
        new detail::FooterBadgeLabel(QStringLiteral("VRAM:0.0M"), this,
                                     QStringLiteral("VRAM:99.9M"), detail::tokens::MinTargetPx, 5);
    m_vramBadge->setBadgeColors(QColor::fromRgba(detail::tokens::SurfaceCard),
                                QColor::fromRgba(detail::tokens::VioletFg),
                                QColor::fromRgba(detail::tokens::VioletBorder));
    m_vramBadge->setToolTip(QStringLiteral("Daemon GPU VRAM usage"));
    m_vramBadge->setAccessibleName(QStringLiteral("Daemon GPU VRAM usage"));
    rootLayout->addWidget(m_vramBadge);

    m_pssBadge =
        new detail::FooterBadgeLabel(QStringLiteral("PSS:0.0M"), this, QStringLiteral("PSS:99.9M"),
                                     detail::tokens::MinTargetPx, 5);
    m_pssBadge->setBadgeColors(QColor::fromRgba(detail::tokens::SurfaceCard),
                               QColor::fromRgba(detail::tokens::TextSecondary),
                               QColor::fromRgba(detail::tokens::BorderDivider));
    m_pssBadge->setToolTip(QStringLiteral("Daemon proportional memory usage (PSS)"));
    m_pssBadge->setAccessibleName(QStringLiteral("Daemon PSS memory usage"));
    rootLayout->addWidget(m_pssBadge);

    m_studioRssBadge = new detail::FooterBadgeLabel(
        QStringLiteral("RSS:--"), this, QStringLiteral("RSS:999M"), detail::tokens::MinTargetPx, 5);
    m_studioRssBadge->setBadgeColors(QColor::fromRgba(detail::tokens::SurfaceCard),
                                     QColor::fromRgba(detail::tokens::AmberFg),
                                     QColor::fromRgba(detail::tokens::AmberBorder));
    m_studioRssBadge->setToolTip(QStringLiteral("WayWal Studio memory usage (VmRSS)"));
    m_studioRssBadge->setAccessibleName(QStringLiteral("Studio RSS memory usage"));
    rootLayout->addWidget(m_studioRssBadge);

    auto *ringLayout = new QHBoxLayout();
    ringLayout->setSpacing(3);
    ringLayout->setContentsMargins(2, 0, 0, 0);

    for (size_t i = 0; i < DmabufRingSlots; ++i) {
        char nameBuf[16];
        std::snprintf(nameBuf, sizeof(nameBuf), "BO%zu", i);
        auto *slotLabel =
            new detail::FooterBadgeLabel(QString::fromUtf8(nameBuf), this, QStringLiteral("SCAN"),
                                         detail::tokens::MinTargetPx, 4);
        slotLabel->setAccessibleName(QStringLiteral("DMA-BUF slot %1").arg(i));
        m_slotLabels[i] = slotLabel;
        ringLayout->addWidget(slotLabel);
    }
    rootLayout->addLayout(ringLayout);
}

void TelemetryHudWidget::setCompactTier(CompactTier tier)
{
    if (m_compactTier == tier) {
        return;
    }
    m_compactTier = tier;

    const bool showVramPss = (tier == CompactTier::Full);
    const bool showRssAndSlots = (tier != CompactTier::Minimal);
    if (m_vramBadge) {
        m_vramBadge->setVisible(showVramPss);
    }
    if (m_pssBadge) {
        m_pssBadge->setVisible(showVramPss);
    }
    if (m_studioRssBadge) {
        m_studioRssBadge->setVisible(showRssAndSlots);
    }
    for (detail::FooterBadgeLabel *slotLabel : m_slotLabels) {
        if (slotLabel) {
            slotLabel->setVisible(showRssAndSlots);
        }
    }
    updateLabels();
}

void TelemetryHudWidget::updateTelemetry(const waywal_telemetry_snapshot_t &snapshot)
{
    m_gpuActive = (snapshot.gpu_active != 0);
    m_daemonVramMb =
        detail::clampFinite(static_cast<float>(snapshot.total_vram_kib) / 1024.0f, 0.0f, 1e7f);
    m_daemonPssMb = detail::clampFinite(static_cast<float>(snapshot.pss_kib) / 1024.0f, 0.0f, 1e7f);

    const uint32_t clampedOutputs = std::min<uint32_t>(
        snapshot.num_outputs, static_cast<uint32_t>(WAYWAL_MAX_TELEMETRY_OUTPUTS));
    m_numOutputs = clampedOutputs;

    std::array<QString, WAYWAL_MAX_TELEMETRY_OUTPUTS> nextDisplayNames{};
    bool outputsChanged = (clampedOutputs != m_cachedOutputCount);

    for (uint32_t i = 0; i < clampedOutputs; ++i) {
        const QString rawName =
            extractBoundedUtf8(snapshot.outputs[i].name, sizeof(snapshot.outputs[i].name));
        const QByteArray rawUtf8 = rawName.toUtf8();
        const size_t copyLen =
            std::min<size_t>(static_cast<size_t>(rawUtf8.size()), sizeof(m_outputNames[i]) - 1);
        if (copyLen > 0) {
            std::memcpy(m_outputNames[i], rawUtf8.constData(), copyLen);
        }
        m_outputNames[i][copyLen] = '\0';

        const QString trimmed = rawName.trimmed();
        nextDisplayNames[i] = trimmed.isEmpty() ? QStringLiteral("OUT-%1").arg(i) : trimmed;
        if (nextDisplayNames[i] != m_cachedDisplayNames[i]) {
            outputsChanged = true;
        }
    }

    if (outputsChanged && m_outputCombo) {
        const QString prevSelectedName =
            (m_selectedOutputIndex > 0 &&
             m_selectedOutputIndex <= static_cast<int>(m_cachedOutputCount))
                ? m_cachedDisplayNames[static_cast<size_t>(m_selectedOutputIndex - 1)]
                : QString();

        const QSignalBlocker blocker(m_outputCombo);
        if (m_outputCombo->count() == 0) {
            m_outputCombo->addItem(QStringLiteral("ALL"), QVariant(-1));
        } else if (m_outputCombo->itemText(0) != QStringLiteral("ALL")) {
            m_outputCombo->setItemText(0, QStringLiteral("ALL"));
            m_outputCombo->setItemData(0, QVariant(-1));
        }

        for (uint32_t i = 0; i < clampedOutputs; ++i) {
            const int comboIdx = 1 + static_cast<int>(i);
            if (comboIdx < m_outputCombo->count()) {
                if (m_outputCombo->itemText(comboIdx) != nextDisplayNames[i]) {
                    m_outputCombo->setItemText(comboIdx, nextDisplayNames[i]);
                }
                m_outputCombo->setItemData(comboIdx, QVariant(static_cast<int>(i)));
            } else {
                m_outputCombo->addItem(nextDisplayNames[i], QVariant(static_cast<int>(i)));
            }
        }
        while (m_outputCombo->count() > 1 + static_cast<int>(clampedOutputs)) {
            m_outputCombo->removeItem(m_outputCombo->count() - 1);
        }

        m_cachedOutputCount = clampedOutputs;
        m_cachedDisplayNames = nextDisplayNames;
        for (uint32_t i = clampedOutputs; i < WAYWAL_MAX_TELEMETRY_OUTPUTS; ++i) {
            m_outputNames[i][0] = '\0';
        }

        int nextComboIndex = 0;
        if (clampedOutputs > 0 && !prevSelectedName.isEmpty()) {
            for (uint32_t i = 0; i < clampedOutputs; ++i) {
                if (m_cachedDisplayNames[i] == prevSelectedName) {
                    nextComboIndex = 1 + static_cast<int>(i);
                    break;
                }
            }
        }
        m_selectedOutputIndex = nextComboIndex;
        m_outputCombo->setCurrentIndex(m_selectedOutputIndex);
    }

    if (clampedOutputs > 0) {
        if (m_selectedOutputIndex <= 0) {
            float maxFps = 0.0f;
            uint32_t maxRenderUs = 0;
            for (uint32_t i = 0; i < clampedOutputs; ++i) {
                const float f =
                    detail::clampFinite(snapshot.outputs[i].scanout_fps, 0.0f, 10000.0f);
                maxFps = std::max(maxFps, f);
                maxRenderUs = std::max(maxRenderUs, snapshot.outputs[i].last_render_us);
            }
            m_scanoutFps = maxFps;
            m_lastRenderUs = maxRenderUs;
            for (size_t s = 0; s < DmabufRingSlots; ++s) {
                m_boStates[s] =
                    static_cast<waywal_bo_slot_state_t>(snapshot.outputs[0].bo_states[s]);
            }
        } else {
            const int maxValidOut = static_cast<int>(clampedOutputs) - 1;
            const size_t outIdx =
                static_cast<size_t>(std::clamp(m_selectedOutputIndex - 1, 0, maxValidOut));
            const auto &outTele = snapshot.outputs[outIdx];
            m_scanoutFps = detail::clampFinite(outTele.scanout_fps, 0.0f, 10000.0f);
            m_lastRenderUs = outTele.last_render_us;

            for (size_t s = 0; s < DmabufRingSlots; ++s) {
                m_boStates[s] = static_cast<waywal_bo_slot_state_t>(outTele.bo_states[s]);
            }
        }
    } else {
        m_selectedOutputIndex = 0;
        m_scanoutFps = 0.0f;
        m_lastRenderUs = 0;
        m_boStates.fill(WAYWAL_BO_STATE_FREE);
    }

    updateLabels();
    updateSlotIndicators();
}

bool TelemetryHudWidget::isActivelyShown() const
{
    if (!isVisible()) {
        return false;
    }
    if (const QWidget *win = window()) {
        if (win->isMinimized()) {
            return false;
        }
    }
    return true;
}

void TelemetryHudWidget::startRssWorkerIfNeeded()
{
    if (m_rssWorker.joinable()) {
        return;
    }
    m_selfWeak = this;
    QPointer<TelemetryHudWidget> safeSelf = m_selfWeak;
    m_rssWorker = std::jthread([this, safeSelf](std::stop_token st) {
        while (!st.stop_requested()) {
            {
                std::unique_lock<std::mutex> lock(m_rssMutex);
                m_rssCv.wait(lock, st, [this]() { return m_rssSampleRequested; });
                if (st.stop_requested()) {
                    return;
                }
                m_rssSampleRequested = false;
            }

            const float sampledMb = readCurrentProcessRssMb();
            if (st.stop_requested()) {
                return;
            }
            QMetaObject::invokeMethod(
                this,
                [safeSelf, sampledMb]() {
                    if (safeSelf) {
                        safeSelf->applySampledRssMb(sampledMb);
                    }
                },
                Qt::QueuedConnection);
        }
    });
}

void TelemetryHudWidget::stopRssWorker()
{
    if (m_rssWorker.joinable()) {
        m_rssWorker.request_stop();
        m_rssCv.notify_all();
        m_rssWorker.join();
    }
}

void TelemetryHudWidget::scheduleAsyncRssSample()
{
    if (!isActivelyShown()) {
        return;
    }
    startRssWorkerIfNeeded();
    {
        std::lock_guard<std::mutex> lock(m_rssMutex);
        m_rssSampleRequested = true;
    }
    m_rssCv.notify_one();
}

void TelemetryHudWidget::applySampledRssMb(float rssMb)
{
    m_studioRssMb = detail::clampFinite(rssMb, 0.0f, 1e7f);
    m_hasRssSample = true;
    updateRssBadgeText();
}

void TelemetryHudWidget::syncPollingState()
{
    if (isActivelyShown()) {
        scheduleAsyncRssSample();
        if (!m_refreshTimer.isActive()) {
            m_refreshTimer.start(1000);
        }
    } else {
        m_refreshTimer.stop();
    }
}

void TelemetryHudWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    syncPollingState();
}

void TelemetryHudWidget::hideEvent(QHideEvent *event)
{
    m_refreshTimer.stop();
    QWidget::hideEvent(event);
}

void TelemetryHudWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event && event->type() == QEvent::WindowStateChange) {
        syncPollingState();
    }
}

void TelemetryHudWidget::onRefreshTimerTick()
{
    if (!isActivelyShown()) {
        m_refreshTimer.stop();
        return;
    }
    scheduleAsyncRssSample();
}

void TelemetryHudWidget::onOutputComboChanged(int index)
{
    if (index < 0) {
        return;
    }
    m_selectedOutputIndex = index;
    if (index == 0) {
        Q_EMIT outputSelected(QStringLiteral("*"));
        return;
    }
    const int outIdx = index - 1;
    if (outIdx >= 0 && outIdx < static_cast<int>(m_numOutputs)) {
        const QString rawName = QString::fromUtf8(m_outputNames[static_cast<size_t>(outIdx)]);
        Q_EMIT outputSelected(rawName.trimmed().isEmpty()
                                  ? m_cachedDisplayNames[static_cast<size_t>(outIdx)]
                                  : rawName);
    }
}

waywal_bo_slot_state_t TelemetryHudWidget::boSlotState(size_t slot) const noexcept
{
    if (slot < DmabufRingSlots) {
        return m_boStates[slot];
    }
    return WAYWAL_BO_STATE_FREE;
}

void TelemetryHudWidget::updateRssBadgeText()
{
    if (!m_studioRssBadge) {
        return;
    }
    if (!m_hasRssSample) {
        m_studioRssBadge->setBadgeText(QStringLiteral("RSS:--"));
        return;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "RSS:%.0fM", m_studioRssMb);
    m_studioRssBadge->setBadgeText(QString::fromUtf8(buf));
}

void TelemetryHudWidget::updateLabels()
{
    char buf[64];

    if (m_fpsBadge) {
        FpsTone tone = FpsTone::Idle;
        if (m_scanoutFps > 0.1f) {
            std::snprintf(buf, sizeof(buf), "%.0f FPS", m_scanoutFps);
            m_fpsBadge->setBadgeText(QString::fromUtf8(buf));
            if (m_scanoutFps >= 115.0f) {
                tone = FpsTone::High;
            } else if (m_scanoutFps >= 55.0f) {
                tone = FpsTone::Medium;
            } else {
                tone = FpsTone::Low;
            }
        } else {
            m_fpsBadge->setBadgeText(QStringLiteral("0 FPS"));
            tone = FpsTone::Idle;
        }

        if (tone != m_lastFpsTone) {
            m_lastFpsTone = tone;
            switch (tone) {
            case FpsTone::High:
                m_fpsBadge->setBadgeColors(QColor::fromRgba(detail::tokens::EmeraldBg),
                                           QColor::fromRgba(detail::tokens::EmeraldFg),
                                           QColor::fromRgba(detail::tokens::EmeraldBorder));
                break;
            case FpsTone::Medium:
                m_fpsBadge->setBadgeColors(QColor::fromRgba(detail::tokens::SkyBg),
                                           QColor::fromRgba(detail::tokens::SkyFg),
                                           QColor::fromRgba(detail::tokens::SkyBorder));
                break;
            case FpsTone::Low:
                m_fpsBadge->setBadgeColors(QColor::fromRgba(detail::tokens::AmberDeepBg),
                                           QColor::fromRgba(detail::tokens::AmberFg),
                                           QColor::fromRgba(detail::tokens::AmberBorder));
                break;
            case FpsTone::Idle:
            case FpsTone::Unset:
                m_fpsBadge->setBadgeColors(QColor::fromRgba(detail::tokens::SurfaceCard),
                                           QColor::fromRgba(detail::tokens::TextSecondary),
                                           QColor::fromRgba(detail::tokens::BorderSubtle));
                break;
            }
        }
    }

    if (m_computeBadge) {
        if (m_lastRenderUs >= 1000) {
            std::snprintf(buf, sizeof(buf), "%.1fms", static_cast<float>(m_lastRenderUs) / 1000.0f);
        } else {
            std::snprintf(buf, sizeof(buf), "%uus", m_lastRenderUs);
        }
        m_computeBadge->setBadgeText(QString::fromUtf8(buf));
        if (m_compactTier == CompactTier::Full) {
            m_computeBadge->setToolTip(QStringLiteral("GPU compute pass duration"));
        } else {
            m_computeBadge->setToolTip(
                QStringLiteral("GPU compute pass: %1\nVRAM: %2M | PSS: %3M | RSS: %4M\n"
                               "DMA-BUF: %5 / %6 / %7 / %8")
                    .arg(QString::fromUtf8(buf))
                    .arg(m_daemonVramMb, 0, 'f', 1)
                    .arg(m_daemonPssMb, 0, 'f', 1)
                    .arg(m_studioRssMb, 0, 'f', 0)
                    .arg(QString::fromLatin1(boStateShortName(m_boStates[0])))
                    .arg(QString::fromLatin1(boStateShortName(m_boStates[1])))
                    .arg(QString::fromLatin1(boStateShortName(m_boStates[2])))
                    .arg(QString::fromLatin1(boStateShortName(m_boStates[3]))));
        }
    }

    if (m_vramBadge) {
        std::snprintf(buf, sizeof(buf), "VRAM:%.1fM", m_daemonVramMb);
        m_vramBadge->setBadgeText(QString::fromUtf8(buf));
    }

    if (m_pssBadge) {
        std::snprintf(buf, sizeof(buf), "PSS:%.1fM", m_daemonPssMb);
        m_pssBadge->setBadgeText(QString::fromUtf8(buf));
    }

    updateRssBadgeText();
}

void TelemetryHudWidget::updateSlotIndicators()
{
    for (size_t i = 0; i < DmabufRingSlots; ++i) {
        detail::FooterBadgeLabel *label = m_slotLabels[i];
        if (!label) {
            continue;
        }
        const int stateVal = static_cast<int>(m_boStates[i]);
        if (stateVal == m_lastBoSlotStates[i]) {
            continue;
        }
        m_lastBoSlotStates[i] = stateVal;

        switch (m_boStates[i]) {
        case WAYWAL_BO_STATE_FREE:
            label->setBadgeText(QStringLiteral("BO%1").arg(i));
            label->setToolTip(QStringLiteral("DMA-BUF Slot #%1: FREE (Idle in GPU pool)").arg(i));
            label->setBadgeColors(QColor::fromRgba(detail::tokens::SurfaceSlate),
                                  QColor::fromRgba(detail::tokens::TextSecondary),
                                  QColor::fromRgba(detail::tokens::BorderDivider));
            break;

        case WAYWAL_BO_STATE_RENDERING:
            label->setBadgeText(QStringLiteral("REND"));
            label->setToolTip(
                QStringLiteral("DMA-BUF Slot #%1: RENDERING (Active GLES 3.1 compute shader pass)")
                    .arg(i));
            label->setBadgeColors(QColor::fromRgba(detail::tokens::AmberDeepBg),
                                  QColor::fromRgba(detail::tokens::AmberFg),
                                  QColor::fromRgba(detail::tokens::AmberBorder));
            break;

        case WAYWAL_BO_STATE_COMMITTED:
            label->setBadgeText(QStringLiteral("COMM"));
            label->setToolTip(
                QStringLiteral(
                    "DMA-BUF Slot #%1: COMMITTED (Attached to Wayland surface, pending flip)")
                    .arg(i));
            label->setBadgeColors(QColor::fromRgba(detail::tokens::SkyBg),
                                  QColor::fromRgba(detail::tokens::SkyFg),
                                  QColor::fromRgba(detail::tokens::SkyBorder));
            break;

        case WAYWAL_BO_STATE_SCANOUT_LOCKED:
            label->setBadgeText(QStringLiteral("SCAN"));
            label->setToolTip(
                QStringLiteral("DMA-BUF Slot #%1: SCANOUT (Active hardware display scanout lock)")
                    .arg(i));
            label->setBadgeColors(QColor::fromRgba(detail::tokens::EmeraldBg),
                                  QColor::fromRgba(detail::tokens::EmeraldFg),
                                  QColor::fromRgba(detail::tokens::EmeraldBorder));
            break;

        default:
            label->setBadgeText(QStringLiteral("UNK"));
            label->setToolTip(
                QStringLiteral("DMA-BUF Slot #%1: Unknown state (%2)").arg(i).arg(stateVal));
            label->setBadgeColors(QColor::fromRgba(detail::tokens::SurfacePausedSlate),
                                  QColor::fromRgba(detail::tokens::TextPrimary),
                                  QColor::fromRgba(detail::tokens::BorderSlate));
            break;
        }
    }
}

} // namespace wwal::studio
