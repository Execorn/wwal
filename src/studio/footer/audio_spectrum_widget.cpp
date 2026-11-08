// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio_spectrum_widget.hpp"

#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QToolTip>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QLinearGradient>
#include <QtGui/QRadialGradient>
#include <QtCore/QDateTime>

#include <cmath>
#include <algorithm>
#include <cstdio>

namespace wwal::studio {

namespace {

[[maybe_unused]] constexpr float MaxBark = 28.0f; // Schroeder Bark scale limit up to ~20 kHz
constexpr float MinDecayRate = 9.0f;              // 1/s exponential decay constant
constexpr float PeakHoldDurationSec = 0.35f;
constexpr float PeakGravity = 3.5f; // Normalized units / s^2

inline float clampf(float val, float minVal, float maxVal) noexcept
{
    return std::max(minVal, std::min(val, maxVal));
}

} // namespace

float AudioSpectrumWidget::hzToBark(float hz) noexcept
{
    float term1 = 13.0f * std::atan(0.00076f * hz);
    float ratio = hz / 7500.0f;
    float term2 = 3.5f * std::atan(ratio * ratio);
    return term1 + term2;
}

float AudioSpectrumWidget::barkToHz(float bark) noexcept
{
    float z = clampf(bark, 0.0f, 30.0f);
    return 600.0f * std::sinh(z / 6.0f);
}

float AudioSpectrumWidget::bandCenterHz(size_t band) noexcept
{
    if (band >= NumBands) {
        band = NumBands - 1;
    }
    const float maxBark = hzToBark(20000.0f);
    const float zCenter =
        ((static_cast<float>(band) + 0.5f) / static_cast<float>(NumBands)) * maxBark;
    return barkToHz(zCenter);
}

AudioSpectrumWidget::AudioSpectrumWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(40);
    setMinimumWidth(140);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setMouseTracking(true);

    // Precompute center frequencies and subtle ambient idle floor
    for (size_t b = 0; b < NumBands; ++b) {
        m_centerFreqs[b] = bandCenterHz(b);
        const float normIdx = static_cast<float>(b) / static_cast<float>(NumBands - 1);
        const float env = 0.14f + 0.22f * std::exp(-std::pow((normIdx - 0.22f) / 0.18f, 2.0f)) +
                          0.12f * std::exp(-std::pow((normIdx - 0.58f) / 0.20f, 2.0f));
        m_targetBands[b] = env;
        m_currentBands[b] = env;
        m_peakCaps[b] = std::min(1.0f, env + 0.08f);
        m_peakHoldTimers[b] = PeakHoldDurationSec;
        m_peakVelocities[b] = 0.0f;
    }
    m_targetRms = 0.22f;
    m_currentRms = 0.22f;
    m_peakRms = 0.30f;

    setupUi();

    // 60 FPS physics and rendering timer
    connect(&m_physicsTimer, &QTimer::timeout, this, &AudioSpectrumWidget::onPhysicsTick);
    m_elapsedTimer.start();
    m_physicsTimer.start(16); // ~60 Hz
}

void AudioSpectrumWidget::setupUi()
{
    auto *rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(2, 2, 2, 2);
    rootLayout->setSpacing(4);

    // Controls container on the right
    auto *controlsLayout = new QHBoxLayout();
    controlsLayout->setSpacing(3);
    controlsLayout->setContentsMargins(0, 0, 0, 0);

    // Beat indicator badge
    m_beatBadge = new QLabel("BEAT", this);
    m_beatBadge->setFixedSize(34, 22);
    m_beatBadge->setAlignment(Qt::AlignCenter);
    m_beatBadge->setStyleSheet("QLabel {"
                               "  background-color: #1a1e29;"
                               "  color: #64748b;"
                               "  border: 1px solid #334155;"
                               "  border-radius: 4px;"
                               "  font-family: monospace;"
                               "  font-size: 9px;"
                               "  font-weight: bold;"
                               "  padding: 1px 2px;"
                               "}");
    controlsLayout->addWidget(m_beatBadge);

    // Audio status badge (PipeWire / Muted)
    m_statusBadge = new QLabel("PW", this);
    m_statusBadge->setToolTip("PipeWire Audio Stream Status");
    m_statusBadge->setFixedSize(26, 22);
    m_statusBadge->setAlignment(Qt::AlignCenter);
    m_statusBadge->setStyleSheet("QLabel {"
                                 "  background-color: #1a1e29;"
                                 "  color: #10b981;"
                                 "  border: 1px solid #065f46;"
                                 "  border-radius: 4px;"
                                 "  font-family: monospace;"
                                 "  font-size: 9px;"
                                 "  font-weight: bold;"
                                 "  padding: 1px 2px;"
                                 "}");
    controlsLayout->addWidget(m_statusBadge);

    // Sensitivity Gain Slider (0.0x to 5.0x -> slider range 0 to 500)
    m_gainLabel = new QLabel("Gain", this);
    m_gainLabel->setStyleSheet("color: #94a3b8; font-size: 9.5px; font-weight: 500;");
    controlsLayout->addWidget(m_gainLabel);

    m_gainSlider = new QSlider(Qt::Horizontal, this);
    m_gainSlider->setRange(0, 500);
    m_gainSlider->setValue(100); // 1.00x default
    m_gainSlider->setFixedWidth(52);
    m_gainSlider->setToolTip("Master Audio Sensitivity Gain (0.0x - 5.0x)");
    m_gainSlider->setStyleSheet("QSlider::groove:horizontal {"
                                "  height: 4px;"
                                "  background: #334155;"
                                "  border-radius: 2px;"
                                "}"
                                "QSlider::sub-page:horizontal {"
                                "  background: #06b6d4;"
                                "  border-radius: 2px;"
                                "}"
                                "QSlider::handle:horizontal {"
                                "  background: #f8fafc;"
                                "  border: 1px solid #06b6d4;"
                                "  width: 10px;"
                                "  margin-top: -3px;"
                                "  margin-bottom: -3px;"
                                "  border-radius: 5px;"
                                "}"
                                "QSlider::handle:horizontal:hover {"
                                "  background: #38bdf8;"
                                "}");
    connect(m_gainSlider, &QSlider::valueChanged, this, &AudioSpectrumWidget::onGainSliderChanged);
    controlsLayout->addWidget(m_gainSlider);

    m_gainValueLabel = new QLabel("1.0x", this);
    m_gainValueLabel->setFixedWidth(30);
    m_gainValueLabel->setStyleSheet("color: #cbd5e1; font-family: monospace; font-size: 9.5px;");
    controlsLayout->addWidget(m_gainValueLabel);

    // Mute on External Audio Checkbox
    m_muteToggle = new QCheckBox("Mute", this);
    m_muteToggle->setToolTip("Mute wallpaper audio when external application is playing");
    m_muteToggle->setStyleSheet("QCheckBox {"
                                "  color: #94a3b8;"
                                "  font-size: 9.5px;"
                                "  spacing: 3px;"
                                "}"
                                "QCheckBox::indicator {"
                                "  width: 11px;"
                                "  height: 11px;"
                                "  border-radius: 2px;"
                                "  border: 1px solid #475569;"
                                "  background: #1e293b;"
                                "}"
                                "QCheckBox::indicator:checked {"
                                "  background: #06b6d4;"
                                "  border-color: #38bdf8;"
                                "}");
    connect(m_muteToggle, &QCheckBox::toggled, this, &AudioSpectrumWidget::onMuteCheckToggled);
    controlsLayout->addWidget(m_muteToggle);

    rootLayout->addStretch(1);
    rootLayout->addLayout(controlsLayout);
}

void AudioSpectrumWidget::updateFromTelemetry(const waywal_telemetry_snapshot_t &snapshot)
{
    updateAudioData(snapshot.bark_bands, snapshot.rms_level, snapshot.beat_pulse,
                    snapshot.beat_onset != 0, snapshot.audio_active != 0,
                    snapshot.pipewire_active != 0, snapshot.mute_on_audio != 0,
                    snapshot.audio_volume);
}

void AudioSpectrumWidget::updateAudioData(const float bands[NumBands], float rmsLevel,
                                          float beatPulse, bool beatOnset, bool audioActive,
                                          bool pipewireActive, bool muteOnAudio, float audioVolume)
{
    if (bands) {
        for (size_t i = 0; i < NumBands; ++i) {
            m_targetBands[i] = clampf(bands[i], 0.0f, 1.0f);
        }
    }

    m_targetRms = clampf(rmsLevel, 0.0f, 1.0f);
    m_targetBeatPulse = clampf(beatPulse, 0.0f, 1.0f);
    m_audioActive = audioActive;
    m_pipewireActive = pipewireActive;
    m_audioVolume = clampf(audioVolume, 0.0f, 1.0f);

    if (beatOnset) {
        m_beatOnset = true;
        m_beatGlowIntensity = 1.0f;
        m_beatRingRadius = 0.0f;
    }

    if (m_muteOnAudio != muteOnAudio) {
        m_muteOnAudio = muteOnAudio;
        QSignalBlocker blocker(m_muteToggle);
        m_muteToggle->setChecked(muteOnAudio);
    }

    // Update status badge
    if (m_statusBadge) {
        if (!pipewireActive) {
            m_statusBadge->setText("OFF");
            m_statusBadge->setStyleSheet(
                "QLabel { background-color: #1e293b; color: #64748b; border: 1px solid #334155; "
                "border-radius: 4px; font-family: monospace; font-size: 9px; padding: 2px; }");
        } else if (muteOnAudio && audioVolume < 0.05f) {
            m_statusBadge->setText("DUCK");
            m_statusBadge->setStyleSheet(
                "QLabel { background-color: #3b2010; color: #fbbf24; border: 1px solid #b45309; "
                "border-radius: 4px; font-family: monospace; font-size: 9px; padding: 2px; }");
        } else {
            m_statusBadge->setText("PW");
            m_statusBadge->setStyleSheet(
                "QLabel { background-color: #064e3b; color: #34d399; border: 1px solid #059669; "
                "border-radius: 4px; font-family: monospace; font-size: 9px; padding: 2px; }");
        }
    }
}

void AudioSpectrumWidget::setSensitivityGain(float gain)
{
    gain = clampf(gain, 0.0f, 5.0f);
    if (std::abs(m_sensitivityGain - gain) > 0.001f) {
        m_sensitivityGain = gain;
        int sliderVal = static_cast<int>(std::round(gain * 100.0f));
        if (m_gainSlider && m_gainSlider->value() != sliderVal) {
            QSignalBlocker blocker(m_gainSlider);
            m_gainSlider->setValue(sliderVal);
        }
        if (m_gainValueLabel) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.1fx", gain);
            m_gainValueLabel->setText(buf);
        }
        Q_EMIT sensitivityGainChanged(m_sensitivityGain);
    }
}

void AudioSpectrumWidget::setMuteOnAudio(bool enabled)
{
    if (m_muteOnAudio != enabled) {
        m_muteOnAudio = enabled;
        if (m_muteToggle) {
            QSignalBlocker blocker(m_muteToggle);
            m_muteToggle->setChecked(enabled);
        }
        Q_EMIT muteOnAudioToggled(m_muteOnAudio);
    }
}

void AudioSpectrumWidget::onGainSliderChanged(int value)
{
    float newGain = static_cast<float>(value) / 100.0f;
    if (std::abs(m_sensitivityGain - newGain) > 0.001f) {
        m_sensitivityGain = newGain;
        if (m_gainValueLabel) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.1fx", newGain);
            m_gainValueLabel->setText(buf);
        }
        Q_EMIT sensitivityGainChanged(m_sensitivityGain);
    }
}

void AudioSpectrumWidget::onMuteCheckToggled(bool checked)
{
    if (m_muteOnAudio != checked) {
        m_muteOnAudio = checked;
        Q_EMIT muteOnAudioToggled(m_muteOnAudio);
    }
}

void AudioSpectrumWidget::onPhysicsTick()
{
    qint64 elapsedNs = m_elapsedTimer.nsecsElapsed();
    m_elapsedTimer.restart();
    float dt = static_cast<float>(elapsedNs) * 1e-9f;
    dt = clampf(dt, 0.001f, 0.050f); // Guard against giant lag spikes

    stepPhysics(dt);
    update();
}

void AudioSpectrumWidget::stepPhysics(float dt)
{
    const float decayFactor = std::exp(-MinDecayRate * dt);

    for (size_t i = 0; i < NumBands; ++i) {
        float scaledTarget = clampf(m_targetBands[i] * m_sensitivityGain, 0.0f, 1.0f);

        // Exponential decay with fast attack
        if (scaledTarget >= m_currentBands[i]) {
            m_currentBands[i] = scaledTarget;
        } else {
            m_currentBands[i] = m_currentBands[i] * decayFactor;
            if (m_currentBands[i] < 0.001f) {
                m_currentBands[i] = 0.0f;
            }
        }

        // Peak-hold caps with gravity fall
        if (m_currentBands[i] >= m_peakCaps[i]) {
            m_peakCaps[i] = m_currentBands[i];
            m_peakHoldTimers[i] = PeakHoldDurationSec;
            m_peakVelocities[i] = 0.0f;
        } else {
            if (m_peakHoldTimers[i] > 0.0f) {
                m_peakHoldTimers[i] -= dt;
            } else {
                m_peakVelocities[i] += PeakGravity * dt;
                m_peakCaps[i] -= m_peakVelocities[i] * dt;
                if (m_peakCaps[i] < m_currentBands[i]) {
                    m_peakCaps[i] = m_currentBands[i];
                    m_peakVelocities[i] = 0.0f;
                }
            }
        }
        m_peakCaps[i] = clampf(m_peakCaps[i], 0.0f, 1.0f);
    }

    // RMS level decay & peak
    float scaledRms = clampf(m_targetRms * m_sensitivityGain, 0.0f, 1.0f);
    if (scaledRms >= m_currentRms) {
        m_currentRms = scaledRms;
    } else {
        m_currentRms = m_currentRms * decayFactor;
    }

    if (m_currentRms >= m_peakRms) {
        m_peakRms = m_currentRms;
        m_peakRmsTimer = PeakHoldDurationSec;
    } else {
        if (m_peakRmsTimer > 0.0f) {
            m_peakRmsTimer -= dt;
        } else {
            m_peakRms -= PeakGravity * 0.5f * dt;
            if (m_peakRms < m_currentRms) {
                m_peakRms = m_currentRms;
            }
        }
    }

    // Beat pulse decay
    m_currentBeatPulse = m_currentBeatPulse * std::exp(-8.0f * dt);
    if (m_targetBeatPulse > m_currentBeatPulse) {
        m_currentBeatPulse = m_targetBeatPulse;
    }

    if (m_beatGlowIntensity > 0.005f) {
        m_beatGlowIntensity *= std::exp(-10.0f * dt);
        m_beatRingRadius += 35.0f * dt;
    } else {
        m_beatGlowIntensity = 0.0f;
        m_beatOnset = false;
    }

    // Update beat badge style
    if (m_beatBadge) {
        if (m_beatGlowIntensity > 0.15f || m_currentBeatPulse > 0.4f) {
            int alpha = static_cast<int>(
                clampf(std::max(m_beatGlowIntensity, m_currentBeatPulse), 0.0f, 1.0f) * 255.0f);
            char styleBuf[256];
            std::snprintf(styleBuf, sizeof(styleBuf),
                          "QLabel {"
                          "  background-color: rgba(236, 72, 153, %d);"
                          "  color: #ffffff;"
                          "  border: 1px solid #f472b6;"
                          "  border-radius: 4px;"
                          "  font-family: monospace;"
                          "  font-size: 9px;"
                          "  font-weight: bold;"
                          "  padding: 2px;"
                          "}",
                          std::min(alpha, 220));
            m_beatBadge->setStyleSheet(styleBuf);
        } else {
            m_beatBadge->setStyleSheet("QLabel {"
                                       "  background-color: #1a1e29;"
                                       "  color: #64748b;"
                                       "  border: 1px solid #334155;"
                                       "  border-radius: 4px;"
                                       "  font-family: monospace;"
                                       "  font-size: 9px;"
                                       "  font-weight: bold;"
                                       "  padding: 2px;"
                                       "}");
        }
    }
}

void AudioSpectrumWidget::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Calculate canvas bounds for spectrum and RMS meter
    const float widgetWidth = static_cast<float>(width());
    const float widgetHeight = static_cast<float>(height());

    // Allocate space: Spectrum bars occupy available space to the left of the leftmost visible
    // control
    float controlsLeft = widgetWidth - 140.0f;
    if (m_beatBadge && m_beatBadge->isVisible()) {
        controlsLeft = static_cast<float>(m_beatBadge->geometry().left());
    } else if (m_statusBadge && m_statusBadge->isVisible()) {
        controlsLeft = static_cast<float>(m_statusBadge->geometry().left());
    }
    const float availWidth = controlsLeft - 12.0f;
    if (availWidth < 24.0f) {
        return;
    }
    const float topPadding = 4.0f;
    const float bottomPadding = 4.0f;
    const float usableHeight = std::max(10.0f, widgetHeight - topPadding - bottomPadding);

    // RMS meter takes rightmost 6 pixels of the visualizer area
    const float rmsWidth = 6.0f;
    const float spectrumWidth = std::max(16.0f, availWidth - rmsWidth - 10.0f);

    QRectF spectrumRect(6.0f, topPadding, spectrumWidth, usableHeight);
    QRectF rmsRect(6.0f + spectrumWidth + 6.0f, topPadding, rmsWidth, usableHeight);

    // Background panel for visualizer
    QRectF totalBg(4.0f, topPadding - 2.0f, availWidth + 4.0f, usableHeight + 4.0f);
    painter.setPen(QPen(QColor(51, 65, 85, 90), 1.0));
    painter.setBrush(QColor(15, 23, 42, 140));
    painter.drawRoundedRect(totalBg, 4.0, 4.0);

    // Render components
    renderBeatPulse(painter, totalBg);
    renderSpectrumBars(painter, spectrumRect);
    renderRmsMeter(painter, rmsRect);
}

void AudioSpectrumWidget::renderBeatPulse(QPainter &painter, const QRectF &rect)
{
    if (m_beatGlowIntensity <= 0.01f && m_currentBeatPulse <= 0.05f) {
        return;
    }
    float pulseVal = std::max(m_beatGlowIntensity, m_currentBeatPulse);
    painter.save();
    painter.setClipRect(rect);

    // Glowing bass pulse background aura
    QPointF center = rect.center();
    float maxRadius = std::max(rect.width(), rect.height()) * 0.7f;
    QRadialGradient aura(center, maxRadius);
    int alpha = static_cast<int>(clampf(pulseVal * 70.0f, 0.0f, 90.0f));
    aura.setColorAt(0.0, QColor(236, 72, 153, alpha));
    aura.setColorAt(0.5, QColor(139, 92, 246, alpha / 2));
    aura.setColorAt(1.0, QColor(15, 23, 42, 0));

    painter.setPen(Qt::NoPen);
    painter.setBrush(aura);
    painter.drawRect(rect);

    // Expanding beat ripple ring
    if (m_beatRingRadius > 0.0f && m_beatRingRadius < maxRadius) {
        int ringAlpha = static_cast<int>(clampf(
            m_beatGlowIntensity * 120.0f * (1.0f - (m_beatRingRadius / maxRadius)), 0.0f, 255.0f));
        if (ringAlpha > 0) {
            painter.setPen(QPen(QColor(244, 114, 182, ringAlpha), 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(center, m_beatRingRadius, m_beatRingRadius * 0.5f);
        }
    }
    painter.restore();
}

void AudioSpectrumWidget::renderSpectrumBars(QPainter &painter, const QRectF &rect)
{
    const float totalWidth = rect.width();
    const float totalHeight = rect.height();
    const float barSpacing = (totalWidth >= 140.0f) ? 1.0f : 0.5f;
    const float barWidth =
        std::max(0.5f, (totalWidth - (NumBands - 1) * barSpacing) / static_cast<float>(NumBands));

    // Dynamic gradient from cyan/teal to violet/magenta
    QLinearGradient barGradient(rect.left(), rect.bottom(), rect.left(), rect.top());
    barGradient.setColorAt(0.0, QColor(6, 182, 212, 220));  // Cyan-500
    barGradient.setColorAt(0.6, QColor(139, 92, 246, 230)); // Violet-500
    barGradient.setColorAt(1.0, QColor(236, 72, 153, 240)); // Pink-500

    painter.setPen(Qt::NoPen);

    for (size_t i = 0; i < NumBands; ++i) {
        float x = rect.left() + static_cast<float>(i) * (barWidth + barSpacing);
        float normVal = m_currentBands[i];
        float barH = std::max(1.0f, normVal * totalHeight);
        float y = rect.bottom() - barH;

        QRectF barRect(x, y, barWidth, barH);

        // Highlight hovered band
        if (static_cast<int>(i) == m_hoveredBand) {
            painter.setBrush(QColor(248, 250, 252, 255));
        } else {
            painter.setBrush(barGradient);
        }
        painter.drawRoundedRect(barRect, 0.5, 0.5);

        // Render peak cap
        float peakNorm = m_peakCaps[i];
        if (peakNorm > 0.02f) {
            float peakY = static_cast<float>(rect.bottom()) - (peakNorm * totalHeight);
            peakY = std::max(static_cast<float>(rect.top()), peakY);
            QRectF capRect(x, peakY - 1.5f, barWidth, 1.5f);
            painter.setBrush(QColor(255, 255, 255, 230));
            painter.drawRect(capRect);
        }
    }
}

void AudioSpectrumWidget::renderRmsMeter(QPainter &painter, const QRectF &rect)
{
    const float totalHeight = rect.height();

    // Background track
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(30, 41, 59, 180));
    painter.drawRoundedRect(rect, 2.0, 2.0);

    // Active RMS bar
    float rmsH = std::max(0.0f, m_currentRms * totalHeight);
    float y = rect.bottom() - rmsH;
    QRectF fillRect(rect.left(), y, rect.width(), rmsH);

    QLinearGradient rmsGrad(rect.left(), rect.bottom(), rect.left(), rect.top());
    rmsGrad.setColorAt(0.0, QColor(16, 185, 129, 230)); // Green
    rmsGrad.setColorAt(0.7, QColor(234, 179, 8, 230));  // Yellow
    rmsGrad.setColorAt(1.0, QColor(239, 68, 68, 240));  // Red

    painter.setBrush(rmsGrad);
    painter.drawRoundedRect(fillRect, 1.5, 1.5);

    // Peak RMS needle
    if (m_peakRms > 0.02f) {
        float peakY = rect.bottom() - (m_peakRms * totalHeight);
        QRectF peakNeedle(rect.left() - 0.5f, peakY - 1.0f, rect.width() + 1.0f, 2.0f);
        painter.setBrush(QColor(255, 255, 255, 250));
        painter.drawRect(peakNeedle);
    }
}

void AudioSpectrumWidget::mouseMoveEvent(QMouseEvent *event)
{
    const float widgetWidth = static_cast<float>(width());
    float controlsLeft = widgetWidth - 140.0f;
    if (m_beatBadge && m_beatBadge->isVisible()) {
        controlsLeft = static_cast<float>(m_beatBadge->geometry().left());
    } else if (m_statusBadge && m_statusBadge->isVisible()) {
        controlsLeft = static_cast<float>(m_statusBadge->geometry().left());
    }
    const float availWidth = std::max(28.0f, controlsLeft - 12.0f);
    const float rmsWidth = 6.0f;
    const float spectrumWidth = std::max(16.0f, availWidth - rmsWidth - 10.0f);
    const float barSpacing = (spectrumWidth >= 140.0f) ? 1.0f : 0.5f;
    const float barWidth = std::max(0.5f, (spectrumWidth - (NumBands - 1) * barSpacing) /
                                              static_cast<float>(NumBands));

    float mouseX = static_cast<float>(event->pos().x());
    float relativeX = mouseX - 6.0f;

    if (relativeX >= 0.0f && relativeX <= spectrumWidth) {
        int band = static_cast<int>(relativeX / (barWidth + barSpacing));
        if (band >= 0 && band < static_cast<int>(NumBands)) {
            m_hoveredBand = band;
            float freqHz = m_centerFreqs[band];
            float valNorm = m_currentBands[band];
            float dB = (valNorm > 0.0001f) ? (20.0f * std::log10(valNorm)) : -80.0f;

            char tipBuf[128];
            if (freqHz >= 1000.0f) {
                std::snprintf(tipBuf, sizeof(tipBuf), "Band #%d: %.2f kHz\nLevel: %.1f%% (%.1f dB)",
                              band + 1, freqHz / 1000.0f, valNorm * 100.0f, dB);
            } else {
                std::snprintf(tipBuf, sizeof(tipBuf), "Band #%d: %.0f Hz\nLevel: %.1f%% (%.1f dB)",
                              band + 1, freqHz, valNorm * 100.0f, dB);
            }
            QToolTip::showText(event->globalPosition().toPoint(), QString::fromUtf8(tipBuf), this);
            update();
            return;
        }
    }

    if (m_hoveredBand != -1) {
        m_hoveredBand = -1;
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void AudioSpectrumWidget::leaveEvent(QEvent *event)
{
    if (m_hoveredBand != -1) {
        m_hoveredBand = -1;
        update();
    }
    QWidget::leaveEvent(event);
}

void AudioSpectrumWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const bool wide = (width() >= 290);
    if (m_beatBadge)
        m_beatBadge->setVisible(wide);
    if (m_gainLabel)
        m_gainLabel->setVisible(wide);
    if (m_gainValueLabel)
        m_gainValueLabel->setVisible(wide);
}

} // namespace wwal::studio
