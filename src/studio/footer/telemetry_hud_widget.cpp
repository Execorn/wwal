// SPDX-License-Identifier: GPL-3.0-or-later
#include "telemetry_hud_widget.hpp"

#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>
#include <QtCore/QSignalBlocker>

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace wwal::studio {

float TelemetryHudWidget::readCurrentProcessRssMb() noexcept {
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

TelemetryHudWidget::TelemetryHudWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(44);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

    // Initial reading of local RSS
    m_studioRssMb = readCurrentProcessRssMb();

    setupUi();
    updateLabels();
    updateSlotIndicators();

    // 1 Hz timer to keep Studio RSS updated
    connect(&m_refreshTimer, &QTimer::timeout, this, &TelemetryHudWidget::onRefreshTimerTick);
    m_refreshTimer.start(1000);
}

void TelemetryHudWidget::setupUi() {
    auto *rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(2, 2, 2, 2);
    rootLayout->setSpacing(3);

    const QString badgeBaseStyle =
        "QLabel {"
        "  background-color: #0f172a;"
        "  border: 1px solid #1e293b;"
        "  border-radius: 4px;"
        "  font-family: monospace;"
        "  font-size: 9px;"
        "  font-weight: 600;"
        "  padding: 2px 3px;"
        "}";

    // Output selector combo box
    m_outputCombo = new QComboBox(this);
    m_outputCombo->setFixedSize(46, 22);
    m_outputCombo->setStyleSheet(
        "QComboBox {"
        "  background-color: #1e293b;"
        "  color: #f1f5f9;"
        "  border: 1px solid #334155;"
        "  border-radius: 4px;"
        "  padding: 1px 3px;"
        "  font-size: 9px;"
        "  font-weight: bold;"
        "}"
        "QComboBox:hover {"
        "  border-color: #06b6d4;"
        "}"
        "QComboBox::drop-down {"
        "  subcontrol-origin: padding;"
        "  subcontrol-position: top right;"
        "  width: 11px;"
        "  border-left: none;"
        "}"
        "QComboBox QAbstractItemView {"
        "  background-color: #0f172a;"
        "  color: #f1f5f9;"
        "  border: 1px solid #334155;"
        "  selection-background-color: #0284c7;"
        "  selection-color: #ffffff;"
        "}"
    );
    m_outputCombo->addItem("ALL");
    m_outputCombo->setToolTip("Active Wayland Output Display");
    connect(m_outputCombo, &QComboBox::currentIndexChanged, this, &TelemetryHudWidget::onOutputComboChanged);
    rootLayout->addWidget(m_outputCombo);

    // Presentation Scanout FPS Badge
    m_fpsBadge = new QLabel("-- FPS", this);
    m_fpsBadge->setFixedHeight(22);
    m_fpsBadge->setAlignment(Qt::AlignCenter);
    m_fpsBadge->setStyleSheet(badgeBaseStyle + "QLabel { color: #34d399; border-color: #065f46; }");
    m_fpsBadge->setToolTip("wp_presentation Hardware Scanout Refresh Rate");
    rootLayout->addWidget(m_fpsBadge);

    // GPU Compute Time Badge
    m_computeBadge = new QLabel("-- µs", this);
    m_computeBadge->setFixedHeight(22);
    m_computeBadge->setAlignment(Qt::AlignCenter);
    m_computeBadge->setStyleSheet(badgeBaseStyle + "QLabel { color: #38bdf8; border-color: #0369a1; }");
    m_computeBadge->setToolTip("GPU Compute Shader Pass Duration");
    rootLayout->addWidget(m_computeBadge);

    // Daemon VRAM Footprint Badge
    m_vramBadge = new QLabel("VRAM: --", this);
    m_vramBadge->setFixedHeight(22);
    m_vramBadge->setAlignment(Qt::AlignCenter);
    m_vramBadge->setStyleSheet(badgeBaseStyle + "QLabel { color: #c084fc; border-color: #7e22ce; }");
    m_vramBadge->setToolTip("Daemon GPU VRAM Footprint");
    rootLayout->addWidget(m_vramBadge);

    // Daemon PSS Badge
    m_pssBadge = new QLabel("PSS: --", this);
    m_pssBadge->setFixedHeight(22);
    m_pssBadge->setAlignment(Qt::AlignCenter);
    m_pssBadge->setStyleSheet(badgeBaseStyle + "QLabel { color: #94a3b8; border-color: #334155; }");
    m_pssBadge->setToolTip("Daemon Proportional Set Size (PSS)");
    rootLayout->addWidget(m_pssBadge);

    // Studio RSS Badge
    m_studioRssBadge = new QLabel("Studio: --", this);
    m_studioRssBadge->setFixedHeight(22);
    m_studioRssBadge->setAlignment(Qt::AlignCenter);
    m_studioRssBadge->setStyleSheet(badgeBaseStyle + "QLabel { color: #fbbf24; border-color: #b45309; }");
    m_studioRssBadge->setToolTip("wwal-studio Resident Set Size (VmRSS from /proc/self/status)");
    rootLayout->addWidget(m_studioRssBadge);

    // 4-Slot DMA-BUF Ring Buffer Visualizer layout
    auto *ringLayout = new QHBoxLayout();
    ringLayout->setSpacing(2);
    ringLayout->setContentsMargins(1, 0, 0, 0);

    for (size_t i = 0; i < DmabufRingSlots; ++i) {
        char nameBuf[16];
        std::snprintf(nameBuf, sizeof(nameBuf), "BO%zu", i);
        auto *slotLabel = new QLabel(nameBuf, this);
        slotLabel->setAlignment(Qt::AlignCenter);
        slotLabel->setFixedSize(22, 18);
        slotLabel->setStyleSheet(
            "QLabel {"
            "  background-color: #1e293b;"
            "  color: #64748b;"
            "  border: 1px solid #334155;"
            "  border-radius: 3px;"
            "  font-family: monospace;"
            "  font-size: 8px;"
            "  font-weight: bold;"
            "}"
        );
        m_slotLabels[i] = slotLabel;
        ringLayout->addWidget(slotLabel);
    }
    rootLayout->addLayout(ringLayout);
}

void TelemetryHudWidget::updateTelemetry(const waywal_telemetry_snapshot_t &snapshot) {
    m_gpuActive = (snapshot.gpu_active != 0);
    m_daemonVramMb = static_cast<float>(snapshot.total_vram_kib) / 1024.0f;
    m_daemonPssMb = static_cast<float>(snapshot.pss_kib) / 1024.0f;
    m_numOutputs = snapshot.num_outputs;

    // Check outputs
    if (m_numOutputs > 0) {
        bool outputsChanged = false;
        if (m_outputCombo->count() != static_cast<int>(m_numOutputs)) {
            outputsChanged = true;
        } else {
            for (uint32_t i = 0; i < m_numOutputs && i < WAYWAL_MAX_TELEMETRY_OUTPUTS; ++i) {
                if (std::strcmp(m_outputNames[i], snapshot.outputs[i].name) != 0) {
                    outputsChanged = true;
                    break;
                }
            }
        }

        if (outputsChanged) {
            QSignalBlocker blocker(m_outputCombo);
            m_outputCombo->clear();
            for (uint32_t i = 0; i < m_numOutputs && i < WAYWAL_MAX_TELEMETRY_OUTPUTS; ++i) {
                std::strncpy(m_outputNames[i], snapshot.outputs[i].name, sizeof(m_outputNames[i]) - 1);
                m_outputNames[i][sizeof(m_outputNames[i]) - 1] = '\0';
                m_outputCombo->addItem(m_outputNames[i]);
            }
            if (m_selectedOutputIndex >= static_cast<int>(m_numOutputs)) {
                m_selectedOutputIndex = 0;
            }
            m_outputCombo->setCurrentIndex(m_selectedOutputIndex);
        }

        // Extract selected output metrics
        size_t outIdx = static_cast<size_t>(std::clamp(m_selectedOutputIndex, 0, static_cast<int>(m_numOutputs - 1)));
        const auto &outTele = snapshot.outputs[outIdx];
        m_scanoutFps = outTele.scanout_fps;
        m_lastRenderUs = outTele.last_render_us;

        for (size_t s = 0; s < DmabufRingSlots; ++s) {
            m_boStates[s] = static_cast<waywal_bo_slot_state_t>(outTele.bo_states[s]);
        }
    } else {
        m_scanoutFps = 0.0f;
        m_lastRenderUs = 0;
        for (size_t s = 0; s < DmabufRingSlots; ++s) {
            m_boStates[s] = WAYWAL_BO_STATE_FREE;
        }
    }

    updateLabels();
    updateSlotIndicators();
}

void TelemetryHudWidget::onRefreshTimerTick() {
    m_studioRssMb = readCurrentProcessRssMb();
    if (m_studioRssBadge) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "RSS:%.0fM", m_studioRssMb);
        m_studioRssBadge->setText(buf);
    }
}

void TelemetryHudWidget::onOutputComboChanged(int index) {
    if (index >= 0) {
        m_selectedOutputIndex = index;
        if (index < static_cast<int>(m_numOutputs)) {
            Q_EMIT outputSelected(QString::fromUtf8(m_outputNames[index]));
        }
    }
}

waywal_bo_slot_state_t TelemetryHudWidget::boSlotState(size_t slot) const noexcept {
    if (slot < DmabufRingSlots) {
        return m_boStates[slot];
    }
    return WAYWAL_BO_STATE_FREE;
}

void TelemetryHudWidget::updateLabels() {
    char buf[64];

    // FPS badge
    if (m_fpsBadge) {
        if (m_scanoutFps > 0.1f) {
            std::snprintf(buf, sizeof(buf), "%.0f FPS", m_scanoutFps);
            m_fpsBadge->setText(buf);
            if (m_scanoutFps >= 115.0f) {
                m_fpsBadge->setStyleSheet("QLabel { background-color: #064e3b; color: #34d399; border: 1px solid #059669; border-radius: 4px; font-family: monospace; font-size: 9.5px; font-weight: 600; padding: 2px 4px; }");
            } else if (m_scanoutFps >= 55.0f) {
                m_fpsBadge->setStyleSheet("QLabel { background-color: #0c4a6e; color: #38bdf8; border: 1px solid #0284c7; border-radius: 4px; font-family: monospace; font-size: 9.5px; font-weight: 600; padding: 2px 4px; }");
            } else {
                m_fpsBadge->setStyleSheet("QLabel { background-color: #451a03; color: #fbbf24; border: 1px solid #d97706; border-radius: 4px; font-family: monospace; font-size: 9.5px; font-weight: 600; padding: 2px 4px; }");
            }
        } else {
            m_fpsBadge->setText("0 FPS");
            m_fpsBadge->setStyleSheet("QLabel { background-color: #1e293b; color: #64748b; border: 1px solid #334155; border-radius: 4px; font-family: monospace; font-size: 9.5px; font-weight: 600; padding: 2px 4px; }");
        }
    }

    // Compute pass duration
    if (m_computeBadge) {
        if (m_lastRenderUs >= 1000) {
            std::snprintf(buf, sizeof(buf), "%.1fms", static_cast<float>(m_lastRenderUs) / 1000.0f);
        } else {
            std::snprintf(buf, sizeof(buf), "%uµs", m_lastRenderUs);
        }
        m_computeBadge->setText(buf);
    }

    // VRAM
    if (m_vramBadge) {
        std::snprintf(buf, sizeof(buf), "VRAM:%.1fM", m_daemonVramMb);
        m_vramBadge->setText(buf);
    }

    // PSS
    if (m_pssBadge) {
        std::snprintf(buf, sizeof(buf), "PSS:%.1fM", m_daemonPssMb);
        m_pssBadge->setText(buf);
    }

    // Studio RSS
    if (m_studioRssBadge) {
        std::snprintf(buf, sizeof(buf), "RSS:%.0fM", m_studioRssMb);
        m_studioRssBadge->setText(buf);
    }
}

void TelemetryHudWidget::updateSlotIndicators() {
    for (size_t i = 0; i < DmabufRingSlots; ++i) {
        auto *label = m_slotLabels[i];
        if (!label) {
            continue;
        }

        switch (m_boStates[i]) {
            case WAYWAL_BO_STATE_FREE:
                label->setText(QString("BO%1").arg(i));
                label->setToolTip(QString("DMA-BUF Slot #%1: FREE (Idle in GPU pool)").arg(i));
                label->setStyleSheet(
                    "QLabel {"
                    "  background-color: #1e293b;"
                    "  color: #64748b;"
                    "  border: 1px solid #334155;"
                    "  border-radius: 3px;"
                    "  font-family: monospace;"
                    "  font-size: 8px;"
                    "  font-weight: bold;"
                    "}"
                );
                break;

            case WAYWAL_BO_STATE_RENDERING:
                label->setText("REND");
                label->setToolTip(QString("DMA-BUF Slot #%1: RENDERING (Active GLES 3.1 compute shader pass)").arg(i));
                label->setStyleSheet(
                    "QLabel {"
                    "  background-color: #451a03;"
                    "  color: #fbbf24;"
                    "  border: 1px solid #d97706;"
                    "  border-radius: 3px;"
                    "  font-family: monospace;"
                    "  font-size: 8px;"
                    "  font-weight: bold;"
                    "}"
                );
                break;

            case WAYWAL_BO_STATE_COMMITTED:
                label->setText("COMM");
                label->setToolTip(QString("DMA-BUF Slot #%1: COMMITTED (Attached to Wayland surface, pending flip)").arg(i));
                label->setStyleSheet(
                    "QLabel {"
                    "  background-color: #082f49;"
                    "  color: #38bdf8;"
                    "  border: 1px solid #0284c7;"
                    "  border-radius: 3px;"
                    "  font-family: monospace;"
                    "  font-size: 8px;"
                    "  font-weight: bold;"
                    "}"
                );
                break;

            case WAYWAL_BO_STATE_SCANOUT_LOCKED:
                label->setText("SCAN");
                label->setToolTip(QString("DMA-BUF Slot #%1: SCANOUT (Active hardware display scanout lock)").arg(i));
                label->setStyleSheet(
                    "QLabel {"
                    "  background-color: #064e3b;"
                    "  color: #34d399;"
                    "  border: 1px solid #059669;"
                    "  border-radius: 3px;"
                    "  font-family: monospace;"
                    "  font-size: 8px;"
                    "  font-weight: bold;"
                    "}"
                );
                break;
        }
    }
}

} // namespace wwal::studio
