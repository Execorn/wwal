// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio_spectrum_widget.hpp"

#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QToolTip>
#include <QtCore/QSignalBlocker>
#include <QtCore/QVarLengthArray>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace wwal::studio {

namespace {

constexpr float MinDecayRate = 9.0f; // 1/s exponential decay constant
constexpr float PeakHoldDurationSec = 0.35f;
constexpr float PeakGravity = 3.5f; // Normalized units / s^2
constexpr float IdleEpsilon = 1e-4f;

} // namespace

float AudioSpectrumWidget::hzToBark(float hz) noexcept
{
    if (!std::isfinite(hz) || hz <= 0.0f) {
        return 0.0f;
    }
    const float term1 = 13.0f * std::atan(0.00076f * hz);
    const float ratio = hz / 7500.0f;
    const float term2 = 3.5f * std::atan(ratio * ratio);
    return term1 + term2;
}

float AudioSpectrumWidget::barkToHz(float bark) noexcept
{
    const float z = detail::clampFinite(bark, 0.0f, 30.0f);
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
    setMinimumHeight(44);
    setMinimumWidth(140);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(QStringLiteral("Audio FFT Spectrum Visualizer"));
    setAccessibleDescription(QStringLiteral(
        "64-band Bark-scale audio spectrum; use Left and Right arrows to inspect bands"));

    for (size_t b = 0; b < NumBands; ++b) {
        m_centerFreqs[b] = bandCenterHz(b);
    }

    initPaintResources();
    setupUi();

    connect(&m_physicsTimer, &QTimer::timeout, this, &AudioSpectrumWidget::onPhysicsTick);
    m_elapsedTimer.start();
}

void AudioSpectrumWidget::initPaintResources()
{
    m_bgPanelPen = QPen(QColor(51, 65, 85, 130), 1.0);
    m_bgPanelBrush = QBrush(QColor(15, 23, 42, 160));
    m_focusRingPen = QPen(QColor::fromRgba(detail::tokens::FocusRing), 2.0);
    m_focusRingPen.setJoinStyle(Qt::RoundJoin);

    m_hoverBarBrush = QBrush(QColor::fromRgba(detail::tokens::TextPrimary));
    m_peakCapBrush = QBrush(QColor(248, 250, 252, 235));
    m_silentTickBrush = QBrush(QColor(51, 65, 85, 140));
    m_rmsTrackBrush = QBrush(QColor(30, 41, 59, 190));
    m_rmsPeakBrush = QBrush(QColor(248, 250, 252, 250));

    m_barGradient = QLinearGradient(0.0, 1.0, 0.0, 0.0);
    m_barGradient.setColorAt(0.0, QColor(6, 182, 212, 225));
    m_barGradient.setColorAt(0.6, QColor(139, 92, 246, 235));
    m_barGradient.setColorAt(1.0, QColor(236, 72, 153, 245));

    m_rmsGradient = QLinearGradient(0.0, 1.0, 0.0, 0.0);
    m_rmsGradient.setColorAt(0.0, QColor(16, 185, 129, 235));
    m_rmsGradient.setColorAt(0.7, QColor(234, 179, 8, 235));
    m_rmsGradient.setColorAt(1.0, QColor(239, 68, 68, 245));

    m_beatRingPen = QPen(QColor(244, 114, 182, 0), 1.5);
}

void AudioSpectrumWidget::setupUi()
{
    setStyleSheet(
        QStringLiteral("QLabel#audioGainLabel {"
                       "  color: #94a3b8;"
                       "  font-size: 11px;"
                       "  font-weight: 600;"
                       "}"
                       "QLabel#audioGainValue {"
                       "  color: #f1f5f9;"
                       "  font-family: monospace;"
                       "  font-size: 11px;"
                       "  font-weight: 600;"
                       "}"
                       "QSlider#audioGainSlider {"
                       "  min-height: 28px;"
                       "  padding: 2px;"
                       "  border: 2px solid transparent;"
                       "  border-radius: 4px;"
                       "}"
                       "QSlider#audioGainSlider:focus {"
                       "  border: 2px solid #38bdf8;"
                       "}"
                       "QSlider#audioGainSlider::groove:horizontal {"
                       "  height: 4px;"
                       "  background: #334155;"
                       "  border-radius: 2px;"
                       "}"
                       "QSlider#audioGainSlider::sub-page:horizontal {"
                       "  background: #0284c7;"
                       "  border-radius: 2px;"
                       "}"
                       "QSlider#audioGainSlider::handle:horizontal {"
                       "  background: #f8fafc;"
                       "  border: 1px solid #38bdf8;"
                       "  width: 12px;"
                       "  margin-top: -4px;"
                       "  margin-bottom: -4px;"
                       "  border-radius: 6px;"
                       "}"
                       "QSlider#audioGainSlider::handle:horizontal:hover {"
                       "  background: #38bdf8;"
                       "}"
                       "QCheckBox#audioMuteToggle {"
                       "  color: #94a3b8;"
                       "  font-size: 11px;"
                       "  spacing: 3px;"
                       "  min-height: 28px;"
                       "  padding: 0px 2px;"
                       "  border: 2px solid transparent;"
                       "  border-radius: 4px;"
                       "}"
                       "QCheckBox#audioMuteToggle:hover, QCheckBox#audioMuteToggle:checked {"
                       "  color: #f1f5f9;"
                       "}"
                       "QCheckBox#audioMuteToggle:focus {"
                       "  border: 2px solid #38bdf8;"
                       "}"
                       "QCheckBox#audioMuteToggle::indicator {"
                       "  width: 14px;"
                       "  height: 14px;"
                       "  border-radius: 3px;"
                       "  border: 1px solid #64748b;"
                       "  background: #181b22;"
                       "}"
                       "QCheckBox#audioMuteToggle::indicator:checked {"
                       "  background: #0284c7;"
                       "  border-color: #38bdf8;"
                       "}"));

    auto *rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(2, 2, 2, 2);
    rootLayout->setSpacing(4);

    auto *controlsLayout = new QHBoxLayout();
    controlsLayout->setSpacing(4);
    controlsLayout->setContentsMargins(0, 0, 0, 0);

    m_beatBadge = new detail::FooterBadgeLabel(QStringLiteral("BEAT"), this, QStringLiteral("BEAT"),
                                               detail::tokens::MinTargetPx, 5);
    m_beatBadge->setToolTip(QStringLiteral("Audio beat onset indicator"));
    m_beatBadge->setAccessibleName(QStringLiteral("Beat onset status"));
    controlsLayout->addWidget(m_beatBadge);
    updateBeatBadgeVisual();

    m_statusBadge = new detail::FooterBadgeLabel(
        QStringLiteral("OFF"), this, QStringLiteral("DUCK"), detail::tokens::MinTargetPx, 4);
    m_statusBadge->setToolTip(QStringLiteral("PipeWire audio stream status"));
    m_statusBadge->setAccessibleName(QStringLiteral("PipeWire audio status"));
    m_statusBadge->setBadgeColors(QColor::fromRgba(detail::tokens::SurfaceSlate),
                                  QColor::fromRgba(detail::tokens::TextSecondary),
                                  QColor::fromRgba(detail::tokens::BorderDivider));
    m_lastStatusTone = StatusTone::Off;
    controlsLayout->addWidget(m_statusBadge);

    m_gainLabel = new QLabel(QStringLiteral("Gain"), this);
    m_gainLabel->setObjectName(QStringLiteral("audioGainLabel"));
    controlsLayout->addWidget(m_gainLabel);

    m_gainSlider = new QSlider(Qt::Horizontal, this);
    m_gainSlider->setObjectName(QStringLiteral("audioGainSlider"));
    m_gainSlider->setRange(0, 500);
    m_gainSlider->setValue(100);
    m_gainSlider->setFixedSize(48, detail::tokens::MinTargetPx);
    m_gainSlider->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_gainSlider->setToolTip(QStringLiteral("Master Audio Sensitivity Gain (0.0x - 5.0x)"));
    m_gainSlider->setAccessibleName(QStringLiteral("Audio sensitivity gain"));
    connect(m_gainSlider, &QSlider::valueChanged, this, &AudioSpectrumWidget::onGainSliderChanged);
    controlsLayout->addWidget(m_gainSlider);

    m_gainValueLabel = new QLabel(QStringLiteral("1.0x"), this);
    m_gainValueLabel->setObjectName(QStringLiteral("audioGainValue"));
    const QFontMetrics valFm(m_gainValueLabel->font());
    m_gainValueLabel->setMinimumWidth(
        std::max(34, valFm.horizontalAdvance(QStringLiteral("5.0x")) + 4));
    controlsLayout->addWidget(m_gainValueLabel);

    m_muteToggle = new QCheckBox(QStringLiteral("Mute"), this);
    m_muteToggle->setObjectName(QStringLiteral("audioMuteToggle"));
    m_muteToggle->setMinimumSize(detail::tokens::MinTargetPx, detail::tokens::MinTargetPx);
    m_muteToggle->setToolTip(
        QStringLiteral("Mute wallpaper audio when external application is playing"));
    m_muteToggle->setAccessibleName(QStringLiteral("Mute wallpaper audio on external audio"));
    connect(m_muteToggle, &QCheckBox::toggled, this, &AudioSpectrumWidget::onMuteCheckToggled);
    controlsLayout->addWidget(m_muteToggle);

    rootLayout->addStretch(1);
    rootLayout->addLayout(controlsLayout);
}

bool AudioSpectrumWidget::hasActivePhysics() const noexcept
{
    if (m_beatGlowIntensity > IdleEpsilon || m_currentBeatPulse > IdleEpsilon) {
        if (m_beatGlowIntensity > IdleEpsilon ||
            std::abs(m_currentBeatPulse - m_targetBeatPulse) > IdleEpsilon) {
            return true;
        }
    } else if (m_targetBeatPulse > IdleEpsilon) {
        return true;
    }

    const float scaledRms = detail::clampFinite(m_targetRms * m_sensitivityGain, 0.0f, 1.0f);
    if (std::abs(m_currentRms - scaledRms) > IdleEpsilon ||
        m_peakRms > m_currentRms + IdleEpsilon) {
        return true;
    }

    for (size_t i = 0; i < NumBands; ++i) {
        const float scaledTarget =
            detail::clampFinite(m_targetBands[i] * m_sensitivityGain, 0.0f, 1.0f);
        if (std::abs(m_currentBands[i] - scaledTarget) > IdleEpsilon ||
            m_peakCaps[i] > m_currentBands[i] + IdleEpsilon) {
            return true;
        }
    }
    return false;
}

void AudioSpectrumWidget::wakePhysicsTimer()
{
    if (!isVisible()) {
        if (m_physicsTimer.isActive()) {
            m_physicsTimer.stop();
        }
        return;
    }
    if (hasActivePhysics()) {
        if (!m_physicsTimer.isActive()) {
            m_elapsedTimer.restart();
            m_physicsTimer.start(16);
        }
    } else if (m_physicsTimer.isActive()) {
        m_physicsTimer.stop();
    }
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
    bool targetsChanged = false;
    if (bands) {
        for (size_t i = 0; i < NumBands; ++i) {
            const float clamped = detail::clampFinite(bands[i], 0.0f, 1.0f);
            if (std::abs(m_targetBands[i] - clamped) > 1e-5f) {
                m_targetBands[i] = clamped;
                targetsChanged = true;
            }
        }
    }

    const float clampedRms = detail::clampFinite(rmsLevel, 0.0f, 1.0f);
    if (std::abs(m_targetRms - clampedRms) > 1e-5f) {
        m_targetRms = clampedRms;
        targetsChanged = true;
    }

    const float clampedBeat = detail::clampFinite(beatPulse, 0.0f, 1.0f);
    if (std::abs(m_targetBeatPulse - clampedBeat) > 1e-5f) {
        m_targetBeatPulse = clampedBeat;
        targetsChanged = true;
    }

    m_audioActive = audioActive;
    m_pipewireActive = pipewireActive;
    m_audioVolume = detail::clampFinite(audioVolume, 0.0f, 1.0f);

    if (beatOnset) {
        m_beatOnset = true;
        m_beatGlowIntensity = 1.0f;
        m_beatRingRadius = 0.0f;
        targetsChanged = true;
    }

    if (m_muteOnAudio != muteOnAudio) {
        m_muteOnAudio = muteOnAudio;
        if (m_muteToggle) {
            const QSignalBlocker blocker(m_muteToggle);
            m_muteToggle->setChecked(muteOnAudio);
        }
    }

    if (m_statusBadge) {
        StatusTone nextTone = StatusTone::Active;
        if (!pipewireActive) {
            nextTone = StatusTone::Off;
        } else if (muteOnAudio && m_audioVolume < 0.05f) {
            nextTone = StatusTone::Ducked;
        }

        if (nextTone != m_lastStatusTone) {
            m_lastStatusTone = nextTone;
            switch (nextTone) {
            case StatusTone::Off:
                m_statusBadge->setBadgeText(QStringLiteral("OFF"));
                m_statusBadge->setBadgeColors(QColor::fromRgba(detail::tokens::SurfaceSlate),
                                              QColor::fromRgba(detail::tokens::TextSecondary),
                                              QColor::fromRgba(detail::tokens::BorderDivider));
                m_statusBadge->setToolTip(QStringLiteral("PipeWire audio stream: Inactive"));
                break;
            case StatusTone::Ducked:
                m_statusBadge->setBadgeText(QStringLiteral("DUCK"));
                m_statusBadge->setBadgeColors(QColor::fromRgba(detail::tokens::AmberBg),
                                              QColor::fromRgba(detail::tokens::AmberFg),
                                              QColor::fromRgba(detail::tokens::AmberBorder));
                m_statusBadge->setToolTip(
                    QStringLiteral("PipeWire audio stream: Ducked by external audio"));
                break;
            case StatusTone::Active:
            case StatusTone::Unset:
                m_statusBadge->setBadgeText(QStringLiteral("PW"));
                m_statusBadge->setBadgeColors(QColor::fromRgba(detail::tokens::EmeraldBg),
                                              QColor::fromRgba(detail::tokens::EmeraldFg),
                                              QColor::fromRgba(detail::tokens::EmeraldBorder));
                m_statusBadge->setToolTip(QStringLiteral("PipeWire audio stream: Active"));
                break;
            }
        }
    }

    // Apply instantaneous attack right away so synchronous callers see updated peaks immediately.
    stepPhysics(0.0f);
    wakePhysicsTimer();

    if (isVisible() && targetsChanged && !m_physicsTimer.isActive()) {
        update();
    }
}

void AudioSpectrumWidget::setSensitivityGain(float gain)
{
    gain = detail::clampFinite(gain, 0.0f, 5.0f);
    if (std::abs(m_sensitivityGain - gain) > 0.001f) {
        m_sensitivityGain = gain;
        const int sliderVal = static_cast<int>(std::lround(gain * 100.0f));
        if (m_gainSlider && m_gainSlider->value() != sliderVal) {
            const QSignalBlocker blocker(m_gainSlider);
            m_gainSlider->setValue(sliderVal);
        }
        if (m_gainValueLabel) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.1fx", gain);
            m_gainValueLabel->setText(QString::fromUtf8(buf));
        }
        stepPhysics(0.0f);
        wakePhysicsTimer();
        if (isVisible() && !m_physicsTimer.isActive()) {
            update();
        }
        Q_EMIT sensitivityGainChanged(m_sensitivityGain);
    }
}

void AudioSpectrumWidget::setMuteOnAudio(bool enabled)
{
    if (m_muteOnAudio != enabled) {
        m_muteOnAudio = enabled;
        if (m_muteToggle) {
            const QSignalBlocker blocker(m_muteToggle);
            m_muteToggle->setChecked(enabled);
        }
        Q_EMIT muteOnAudioToggled(m_muteOnAudio);
    }
}

void AudioSpectrumWidget::onGainSliderChanged(int value)
{
    const float newGain = detail::clampFinite(static_cast<float>(value) / 100.0f, 0.0f, 5.0f);
    if (std::abs(m_sensitivityGain - newGain) > 0.001f) {
        m_sensitivityGain = newGain;
        if (m_gainValueLabel) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.1fx", newGain);
            m_gainValueLabel->setText(QString::fromUtf8(buf));
        }
        stepPhysics(0.0f);
        wakePhysicsTimer();
        if (isVisible() && !m_physicsTimer.isActive()) {
            update();
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
    if (!isVisible()) {
        m_physicsTimer.stop();
        return;
    }

    const qint64 elapsedNs = m_elapsedTimer.nsecsElapsed();
    m_elapsedTimer.restart();
    const float dt = detail::clampFinite(static_cast<float>(elapsedNs) * 1e-9f, 0.001f, 0.050f);

    stepPhysics(dt);
    update();

    if (!hasActivePhysics()) {
        m_physicsTimer.stop();
    }
}

void AudioSpectrumWidget::stepPhysics(float dt)
{
    const float decayFactor = (dt > 0.0f) ? std::exp(-MinDecayRate * dt) : 1.0f;

    for (size_t i = 0; i < NumBands; ++i) {
        const float scaledTarget =
            detail::clampFinite(m_targetBands[i] * m_sensitivityGain, 0.0f, 1.0f);

        if (scaledTarget >= m_currentBands[i]) {
            m_currentBands[i] = scaledTarget;
        } else if (dt > 0.0f) {
            m_currentBands[i] = scaledTarget + (m_currentBands[i] - scaledTarget) * decayFactor;
            if (std::abs(m_currentBands[i] - scaledTarget) < IdleEpsilon) {
                m_currentBands[i] = scaledTarget;
            }
        }

        if (m_currentBands[i] >= m_peakCaps[i]) {
            m_peakCaps[i] = m_currentBands[i];
            m_peakHoldTimers[i] = PeakHoldDurationSec;
            m_peakVelocities[i] = 0.0f;
        } else if (dt > 0.0f) {
            if (m_peakHoldTimers[i] > 0.0f) {
                m_peakHoldTimers[i] -= dt;
            } else {
                m_peakVelocities[i] += PeakGravity * dt;
                m_peakCaps[i] -= m_peakVelocities[i] * dt;
                if (m_peakCaps[i] <= m_currentBands[i] + IdleEpsilon) {
                    m_peakCaps[i] = m_currentBands[i];
                    m_peakVelocities[i] = 0.0f;
                }
            }
        }
        m_peakCaps[i] = detail::clampFinite(m_peakCaps[i], 0.0f, 1.0f);
    }

    const float scaledRms = detail::clampFinite(m_targetRms * m_sensitivityGain, 0.0f, 1.0f);
    if (scaledRms >= m_currentRms) {
        m_currentRms = scaledRms;
    } else if (dt > 0.0f) {
        m_currentRms = scaledRms + (m_currentRms - scaledRms) * decayFactor;
        if (std::abs(m_currentRms - scaledRms) < IdleEpsilon) {
            m_currentRms = scaledRms;
        }
    }

    if (m_currentRms >= m_peakRms) {
        m_peakRms = m_currentRms;
        m_peakRmsTimer = PeakHoldDurationSec;
    } else if (dt > 0.0f) {
        if (m_peakRmsTimer > 0.0f) {
            m_peakRmsTimer -= dt;
        } else {
            m_peakRms -= PeakGravity * 0.5f * dt;
            if (m_peakRms <= m_currentRms + IdleEpsilon) {
                m_peakRms = m_currentRms;
            }
        }
    }

    if (m_targetBeatPulse >= m_currentBeatPulse) {
        m_currentBeatPulse = m_targetBeatPulse;
    } else if (dt > 0.0f) {
        m_currentBeatPulse =
            m_targetBeatPulse + (m_currentBeatPulse - m_targetBeatPulse) * std::exp(-8.0f * dt);
        if (std::abs(m_currentBeatPulse - m_targetBeatPulse) < IdleEpsilon) {
            m_currentBeatPulse = m_targetBeatPulse;
        }
    }

    if (dt > 0.0f) {
        if (m_beatGlowIntensity > 0.005f) {
            m_beatGlowIntensity *= std::exp(-10.0f * dt);
            m_beatRingRadius += 35.0f * dt;
        } else {
            m_beatGlowIntensity = 0.0f;
            m_beatOnset = false;
        }
    }

    updateBeatBadgeVisual();
}

void AudioSpectrumWidget::updateBeatBadgeVisual()
{
    if (!m_beatBadge) {
        return;
    }

    const bool active = (m_beatGlowIntensity > 0.15f || m_currentBeatPulse > 0.4f);
    const int rawAlpha =
        active ? std::min(220, static_cast<int>(detail::clampFinite(std::max(m_beatGlowIntensity,
                                                                             m_currentBeatPulse),
                                                                    0.0f, 1.0f) *
                                                255.0f))
               : 0;
    const int bucket = active ? std::max(1, (rawAlpha + 7) / 8) : 0;
    if (bucket == m_lastBeatAlphaBucket) {
        return;
    }
    m_lastBeatAlphaBucket = bucket;

    if (active) {
        const int alpha = std::min(230, bucket * 8);
        m_beatBadge->setBadgeColors(QColor(157, 23, 77, alpha),
                                    QColor::fromRgba(detail::tokens::TextPrimary),
                                    QColor(244, 114, 182));
    } else {
        m_beatBadge->setBadgeColors(QColor::fromRgba(detail::tokens::SurfaceBadgeIdle),
                                    QColor::fromRgba(detail::tokens::TextSecondary),
                                    QColor::fromRgba(detail::tokens::BorderDivider));
    }
}

bool AudioSpectrumWidget::computeVisualizerGeometry(VisualizerLayout &out) const
{
    out.valid = false;
    const qreal widgetWidth = static_cast<qreal>(width());
    const qreal widgetHeight = static_cast<qreal>(height());
    if (widgetWidth <= 0.0 || widgetHeight <= 0.0) {
        return false;
    }

    // Use the leftmost visible control widget so hidden optional badges never shrink the plot area.
    qreal controlsLeft = widgetWidth;
    const std::array<const QWidget *, 6> controls = {m_beatBadge,  m_statusBadge,    m_gainLabel,
                                                     m_gainSlider, m_gainValueLabel, m_muteToggle};
    bool foundControl = false;
    for (const QWidget *ctrl : controls) {
        if (ctrl && ctrl->isVisible()) {
            controlsLeft = std::min(controlsLeft, static_cast<qreal>(ctrl->geometry().left()));
            foundControl = true;
        }
    }
    if (!foundControl) {
        controlsLeft = std::max(0.0, widgetWidth - 140.0);
    }

    const qreal availWidth = controlsLeft - 12.0;
    if (availWidth < 24.0) {
        return false;
    }

    const qreal topPadding = 4.0;
    const qreal bottomPadding = 4.0;
    const qreal usableHeight = std::max(10.0, widgetHeight - topPadding - bottomPadding);
    const qreal rmsWidth = 6.0;
    const qreal spectrumWidth = std::max(16.0, availWidth - rmsWidth - 10.0);

    out.spectrumRect = QRectF(6.0, topPadding, spectrumWidth, usableHeight);
    out.rmsRect = QRectF(6.0 + spectrumWidth + 6.0, topPadding, rmsWidth, usableHeight);
    out.totalBg = QRectF(4.0, topPadding - 2.0, availWidth + 4.0, usableHeight + 4.0);
    out.barSpacing = (spectrumWidth >= 140.0) ? 1.0 : 0.5;
    out.barWidth = std::max(0.5, (spectrumWidth - (NumBands - 1) * out.barSpacing) /
                                     static_cast<qreal>(NumBands));
    out.barPitch = out.barWidth + out.barSpacing;
    out.valid = true;
    return true;
}

void AudioSpectrumWidget::paintEvent(QPaintEvent * /*event*/)
{
    if (!isVisible()) {
        return;
    }

    VisualizerLayout layout;
    if (!computeVisualizerGeometry(layout)) {
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const qreal dpr = std::max(1.0, devicePixelRatioF());
    const qreal halfPx = 0.5 / dpr;
    const QRectF snappedBg = layout.totalBg.adjusted(halfPx, halfPx, -halfPx, -halfPx);

    painter.setPen(m_bgPanelPen);
    painter.setBrush(m_bgPanelBrush);
    painter.drawRoundedRect(snappedBg, 4.0, 4.0);

    renderBeatPulse(painter, layout.totalBg);
    renderSpectrumBars(painter, layout);
    renderRmsMeter(painter, layout.rmsRect);

    if (hasFocus()) {
        painter.setPen(m_focusRingPen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(snappedBg.adjusted(1.0, 1.0, -1.0, -1.0), 4.0, 4.0);
    }
}

void AudioSpectrumWidget::renderBeatPulse(QPainter &painter, const QRectF &rect)
{
    if (m_beatGlowIntensity <= 0.01f && m_currentBeatPulse <= 0.05f) {
        return;
    }
    const float pulseVal = std::max(m_beatGlowIntensity, m_currentBeatPulse);
    painter.save();
    painter.setClipRect(rect);

    const QPointF center = rect.center();
    const qreal maxRadius = std::max(rect.width(), rect.height()) * 0.7;
    const int alpha = static_cast<int>(detail::clampFinite(pulseVal * 70.0f, 0.0f, 90.0f));
    if (m_cachedAuraRect != rect || m_cachedAuraAlpha != alpha) {
        m_beatAuraGradient.setCenter(center);
        m_beatAuraGradient.setFocalPoint(center);
        m_beatAuraGradient.setRadius(maxRadius);
        m_beatAuraGradient.setColorAt(0.0, QColor(236, 72, 153, alpha));
        m_beatAuraGradient.setColorAt(0.5, QColor(139, 92, 246, alpha / 2));
        m_beatAuraGradient.setColorAt(1.0, QColor(15, 23, 42, 0));
        m_cachedAuraRect = rect;
        m_cachedAuraAlpha = alpha;
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(m_beatAuraGradient);
    painter.drawRect(rect);

    if (m_beatRingRadius > 0.0f && m_beatRingRadius < static_cast<float>(maxRadius)) {
        const int ringAlpha = static_cast<int>(
            detail::clampFinite(m_beatGlowIntensity * 120.0f *
                                    (1.0f - (m_beatRingRadius / static_cast<float>(maxRadius))),
                                0.0f, 255.0f));
        if (ringAlpha > 0) {
            QColor ringColor = m_beatRingPen.color();
            ringColor.setAlpha(ringAlpha);
            m_beatRingPen.setColor(ringColor);
            painter.setPen(m_beatRingPen);
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(center, m_beatRingRadius, m_beatRingRadius * 0.5);
        }
    }
    painter.restore();
}

void AudioSpectrumWidget::renderSpectrumBars(QPainter &painter, const VisualizerLayout &layout)
{
    const QRectF &rect = layout.spectrumRect;
    const qreal totalHeight = rect.height();

    if (m_cachedBarGradRect != rect) {
        m_barGradient.setStart(rect.left(), rect.bottom());
        m_barGradient.setFinalStop(rect.left(), rect.top());
        m_cachedBarGradRect = rect;
    }

    QVarLengthArray<QRectF, NumBands> activeBars;
    QVarLengthArray<QRectF, NumBands> silentTicks;
    QVarLengthArray<QRectF, NumBands> peakCaps;
    QRectF hoveredRect;
    bool hasHovered = false;

    for (size_t i = 0; i < NumBands; ++i) {
        const qreal x = rect.left() + static_cast<qreal>(i) * layout.barPitch;
        const float normVal = m_currentBands[i];
        const qreal barH = std::max(1.0, static_cast<qreal>(normVal) * totalHeight);
        const qreal y = rect.bottom() - barH;
        const QRectF barRect(x, y, layout.barWidth, barH);

        if (static_cast<int>(i) == m_hoveredBand) {
            hoveredRect = barRect;
            hasHovered = true;
        } else if (normVal <= IdleEpsilon) {
            silentTicks.append(barRect);
        } else {
            activeBars.append(barRect);
        }

        const float peakNorm = m_peakCaps[i];
        if (peakNorm > 0.02f) {
            qreal peakY = rect.bottom() - (static_cast<qreal>(peakNorm) * totalHeight);
            peakY = std::max(rect.top(), peakY);
            peakCaps.append(QRectF(x, peakY - 1.5, layout.barWidth, 1.5));
        }
    }

    painter.setPen(Qt::NoPen);
    if (!silentTicks.isEmpty()) {
        painter.setBrush(m_silentTickBrush);
        painter.drawRects(silentTicks.constData(), silentTicks.size());
    }
    if (!activeBars.isEmpty()) {
        painter.setBrush(m_barGradient);
        painter.drawRects(activeBars.constData(), activeBars.size());
    }
    if (hasHovered) {
        painter.setBrush(m_hoverBarBrush);
        painter.drawRect(hoveredRect);
    }
    if (!peakCaps.isEmpty()) {
        painter.setBrush(m_peakCapBrush);
        painter.drawRects(peakCaps.constData(), peakCaps.size());
    }
}

void AudioSpectrumWidget::renderRmsMeter(QPainter &painter, const QRectF &rect)
{
    const qreal totalHeight = rect.height();

    painter.setPen(Qt::NoPen);
    painter.setBrush(m_rmsTrackBrush);
    painter.drawRoundedRect(rect, 2.0, 2.0);

    if (m_currentRms > IdleEpsilon) {
        if (m_cachedRmsGradRect != rect) {
            m_rmsGradient.setStart(rect.left(), rect.bottom());
            m_rmsGradient.setFinalStop(rect.left(), rect.top());
            m_cachedRmsGradRect = rect;
        }
        const qreal rmsH = std::max(1.0, static_cast<qreal>(m_currentRms) * totalHeight);
        const QRectF fillRect(rect.left(), rect.bottom() - rmsH, rect.width(), rmsH);
        painter.setBrush(m_rmsGradient);
        painter.drawRoundedRect(fillRect, 1.5, 1.5);
    }

    if (m_peakRms > 0.02f) {
        const qreal peakY =
            std::max(rect.top(), rect.bottom() - (static_cast<qreal>(m_peakRms) * totalHeight));
        const QRectF peakNeedle(rect.left() - 0.5, peakY - 1.0, rect.width() + 1.0, 2.0);
        painter.setBrush(m_rmsPeakBrush);
        painter.drawRect(peakNeedle);
    }
}

void AudioSpectrumWidget::showBandTooltip(int band, const QPoint &globalPos)
{
    if (band < 0 || band >= static_cast<int>(NumBands)) {
        return;
    }
    const float freqHz = m_centerFreqs[static_cast<size_t>(band)];
    const float valNorm = m_currentBands[static_cast<size_t>(band)];
    const float dB = (valNorm > 0.0001f) ? (20.0f * std::log10(valNorm)) : -80.0f;

    char tipBuf[128];
    if (freqHz >= 1000.0f) {
        std::snprintf(tipBuf, sizeof(tipBuf), "Band #%d: %.2f kHz\nLevel: %.1f%% (%.1f dB)",
                      band + 1, freqHz / 1000.0f, valNorm * 100.0f, dB);
    } else {
        std::snprintf(tipBuf, sizeof(tipBuf), "Band #%d: %.0f Hz\nLevel: %.1f%% (%.1f dB)",
                      band + 1, freqHz, valNorm * 100.0f, dB);
    }
    QToolTip::showText(globalPos, QString::fromUtf8(tipBuf), this);
}

void AudioSpectrumWidget::mouseMoveEvent(QMouseEvent *event)
{
    VisualizerLayout layout;
    if (computeVisualizerGeometry(layout)) {
        const QPointF pos = event->position();
        if (pos.x() >= layout.spectrumRect.left() && pos.x() <= layout.spectrumRect.right() &&
            pos.y() >= layout.totalBg.top() && pos.y() <= layout.totalBg.bottom() &&
            layout.barPitch > 0.0) {
            const qreal relativeX = pos.x() - layout.spectrumRect.left();
            const int band = std::clamp(static_cast<int>(relativeX / layout.barPitch), 0,
                                        static_cast<int>(NumBands) - 1);
            if (m_hoveredBand != band) {
                m_hoveredBand = band;
                update();
            }
            showBandTooltip(band, event->globalPosition().toPoint());
            return;
        }
    }

    if (m_hoveredBand != -1) {
        m_hoveredBand = -1;
        QToolTip::hideText();
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void AudioSpectrumWidget::leaveEvent(QEvent *event)
{
    if (m_hoveredBand != -1) {
        m_hoveredBand = -1;
        QToolTip::hideText();
        update();
    }
    QWidget::leaveEvent(event);
}

void AudioSpectrumWidget::keyPressEvent(QKeyEvent *event)
{
    VisualizerLayout layout;
    if (!computeVisualizerGeometry(layout)) {
        QWidget::keyPressEvent(event);
        return;
    }

    int nextBand = m_hoveredBand;
    switch (event->key()) {
    case Qt::Key_Right:
        nextBand =
            (m_hoveredBand < 0) ? 0 : std::min(static_cast<int>(NumBands) - 1, m_hoveredBand + 1);
        break;
    case Qt::Key_Left:
        nextBand = (m_hoveredBand < 0) ? 0 : std::max(0, m_hoveredBand - 1);
        break;
    case Qt::Key_Home:
        nextBand = 0;
        break;
    case Qt::Key_End:
        nextBand = static_cast<int>(NumBands) - 1;
        break;
    case Qt::Key_Escape:
        if (m_hoveredBand != -1) {
            m_hoveredBand = -1;
            QToolTip::hideText();
            update();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
        return;
    default:
        QWidget::keyPressEvent(event);
        return;
    }

    m_hoveredBand = nextBand;
    const qreal barCenterX =
        layout.spectrumRect.left() + (static_cast<qreal>(nextBand) + 0.5) * layout.barPitch;
    const QPoint localAnchor(static_cast<int>(std::lround(barCenterX)),
                             static_cast<int>(std::lround(layout.spectrumRect.center().y())));
    showBandTooltip(nextBand, mapToGlobal(localAnchor));
    update();
    event->accept();
}

void AudioSpectrumWidget::focusOutEvent(QFocusEvent *event)
{
    if (m_hoveredBand != -1) {
        m_hoveredBand = -1;
        QToolTip::hideText();
    }
    update();
    QWidget::focusOutEvent(event);
}

void AudioSpectrumWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const int w = width();
    const bool showBeat = (w >= 320);
    const bool showGainDetails = (w >= 390);
    if (m_beatBadge && m_beatBadge->isVisible() != showBeat) {
        m_beatBadge->setVisible(showBeat);
    }
    if (m_gainLabel && m_gainLabel->isVisible() != showGainDetails) {
        m_gainLabel->setVisible(showGainDetails);
    }
    if (m_gainValueLabel && m_gainValueLabel->isVisible() != showGainDetails) {
        m_gainValueLabel->setVisible(showGainDetails);
    }
}

void AudioSpectrumWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    wakePhysicsTimer();
}

void AudioSpectrumWidget::hideEvent(QHideEvent *event)
{
    m_physicsTimer.stop();
    QWidget::hideEvent(event);
}

} // namespace wwal::studio
