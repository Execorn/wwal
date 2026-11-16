// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/audio/audio_binding_wizard.hpp"
#include "waywal/we_particle.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPointer>
#include <QScopedValueRollback>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>

namespace waywal::studio {

namespace {

constexpr QColor kBgCanvas(0x0b, 0x0c, 0x10);
constexpr QColor kBorderSubtle(0x2a, 0x2f, 0x3d);
constexpr QColor kTextSecondary(0x94, 0xa3, 0xb8);
constexpr QColor kAccentSky(0x38, 0xbd, 0xf8);
constexpr QColor kSemanticWarn(0xf5, 0x9e, 0x0b);
constexpr QColor kBarInactive(0x64, 0x74, 0x8b);

constexpr int kMaxEffectPassSubIndex = 15; /* Matches WE_MAX_EFFECT_PASSES - 1 in we_scene.h */
constexpr int kMaxEmitterSubIndex = static_cast<int>(WE_PARTICLE_MAX_EMITTERS) - 1;
constexpr int kMaxOperatorSubIndex = static_cast<int>(WE_PARTICLE_MAX_OPERATORS) - 1;
constexpr int kMinControlSizePx = 28;
constexpr int kMinFontSizePx = 11;

[[nodiscard]] float sanitizeFinite(float val, float lo, float hi, float fallback) noexcept
{
    const float safeLo = std::min(lo, hi);
    const float safeHi = std::max(lo, hi);
    if (!std::isfinite(val)) {
        return std::clamp(fallback, safeLo, safeHi);
    }
    return std::clamp(val, safeLo, safeHi);
}

[[nodiscard]] uint32_t sanitizeSampleRateHz(uint32_t sampleRateHz) noexcept
{
    if (sampleRateHz == 0u) {
        return 48000u;
    }
    return std::clamp(sampleRateHz, 4000u, 384000u);
}

[[nodiscard]] QString formatNyquistLabel(uint32_t sampleRateHz)
{
    const double nyquistHz = 0.5 * static_cast<double>(sanitizeSampleRateHz(sampleRateHz));
    if (nyquistHz >= 1000.0) {
        const double khz = nyquistHz / 1000.0;
        const double roundedKhz = std::round(khz);
        if (std::fabs(khz - roundedKhz) < 0.05) {
            return QString::asprintf("%.0f kHz (B63)", roundedKhz);
        }
        return QString::asprintf("%.1f kHz (B63)", khz);
    }
    return QString::asprintf("%.0f Hz (B63)", nyquistHz);
}

struct SubIndexBounds {
    bool enabled{false};
    int maxIndex{0};
    const char *hint{""};
};

[[nodiscard]] SubIndexBounds subIndexBoundsForTarget(uint32_t targetKind) noexcept
{
    switch (static_cast<we_audio_target_kind_t>(targetKind)) {
    case WE_AUDIO_TGT_PASS_UNIFORM_COMP:
        return {true, kMaxEffectPassSubIndex, "Effect or base shader pass index (0..15)"};
    case WE_AUDIO_TGT_PARTICLE_RATE:
    case WE_AUDIO_TGT_PARTICLE_SPEED:
        return {true, kMaxEmitterSubIndex, "Particle emitter index (0..7)"};
    case WE_AUDIO_TGT_PARTICLE_TURB_SPEED:
    case WE_AUDIO_TGT_PARTICLE_VORTEX_SPD:
        return {true, kMaxOperatorSubIndex, "Particle operator index (0..15)"};
    case WE_AUDIO_TGT_LAYER_ORIGIN_X:
    case WE_AUDIO_TGT_LAYER_ORIGIN_Y:
    case WE_AUDIO_TGT_LAYER_SCALE_XY:
    case WE_AUDIO_TGT_LAYER_ROTATION_Z:
    case WE_AUDIO_TGT_LAYER_ALPHA:
    case WE_AUDIO_TGT_LAYER_BRIGHTNESS:
    case WE_AUDIO_TGT_LAYER_COLOR_RGB:
    case WE_AUDIO_TGT_LIGHT_INTENSITY:
    case WE_AUDIO_TGT_LIGHT_RADIUS:
    case WE_AUDIO_TGT_PARTICLE_BURST_TRIG:
    default:
        return {false, 0, "Sub-index is not used by this target parameter"};
    }
}

class BarkRangePickerWidget final : public QWidget {
  public:
    explicit BarkRangePickerWidget(std::function<void(uint16_t, uint16_t)> onRangeChanged,
                                   QWidget *parent = nullptr)
        : QWidget(parent), m_onRangeChanged(std::move(onRangeChanged))
    {
        setMinimumSize(140, 80);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(QStringLiteral("64-Band Bark Range Picker"));
        setAccessibleDescription(
            QStringLiteral("Drag across or press Left and Right arrows on bands 0 to 63 to "
                           "select a frequency range."));
    }

    void setSampleRateHz(uint32_t sampleRateHz) noexcept
    {
        const uint32_t safeRate = sanitizeSampleRateHz(sampleRateHz);
        if (m_sampleRateHz == safeRate) {
            return;
        }
        m_sampleRateHz = safeRate;
        if (isVisible()) {
            update();
        }
    }

    void setBandRange(uint16_t startBand, uint16_t endBand) noexcept
    {
        const uint16_t nextStart = std::min<uint16_t>(startBand, 63u);
        const uint16_t nextEnd = std::clamp<uint16_t>(endBand, nextStart, 63u);
        if (nextStart == m_bandStart && nextEnd == m_bandEnd) {
            return;
        }
        m_bandStart = nextStart;
        m_bandEnd = nextEnd;
        if (isVisible()) {
            update();
        }
    }

    void setLiveSpectrum(const std::array<float, 64> &bands) noexcept
    {
        bool changed = false;
        for (size_t b = 0; b < m_bands.size(); ++b) {
            const float clean = sanitizeFinite(bands[b], 0.0f, 1.0f, 0.0f);
            if (std::fabs(clean - m_bands[b]) > 1e-4f) {
                m_bands[b] = clean;
                changed = true;
            }
        }
        if (changed && isVisible()) {
            update();
        }
    }

    void clearLiveSpectrum() noexcept
    {
        bool hadNonZero = false;
        for (float &v : m_bands) {
            if (v > 0.0f) {
                v = 0.0f;
                hadNonZero = true;
            }
        }
        if (hadNonZero && isVisible()) {
            update();
        }
    }

  protected:
    void paintEvent(QPaintEvent *event) override
    {
        (void)event;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);

        const QRectF r = rect();
        p.fillRect(r, kBgCanvas);
        const bool focused = hasFocus();
        p.setPen(QPen(focused ? kAccentSky : kBorderSubtle, focused ? 2.0 : 1.0));
        p.drawRect(focused ? r.adjusted(1.0, 1.0, -1.0, -1.0) : r.adjusted(0.5, 0.5, -0.5, -0.5));

        const QRectF plot = r.adjusted(4.0, 4.0, -4.0, -18.0);
        if (plot.width() <= 4.0 || plot.height() <= 4.0) {
            return;
        }

        constexpr size_t kNumBands = wwal::studio::AudioSpectrumWidget::NumBands;
        const qreal barW = plot.width() / static_cast<qreal>(kNumBands);

        const qreal selX0 = plot.left() + static_cast<qreal>(m_bandStart) * barW;
        const qreal selX1 = plot.left() + static_cast<qreal>(m_bandEnd + 1u) * barW;
        p.fillRect(QRectF(selX0, plot.top(), std::max(2.0, selX1 - selX0), plot.height()),
                   QColor(0x38, 0xbd, 0xf8, 42));

        for (size_t b = 0; b < kNumBands; ++b) {
            const float mag = sanitizeFinite(m_bands[b], 0.0f, 1.0f, 0.0f);
            const qreal bx = plot.left() + static_cast<qreal>(b) * barW;
            const qreal bh = std::max<qreal>(2.0, static_cast<qreal>(mag) * plot.height());
            const bool inSel = (b >= m_bandStart && b <= m_bandEnd);
            p.fillRect(QRectF(bx + 0.5, plot.bottom() - bh, std::max(1.0, barW - 1.0), bh),
                       inSel ? kAccentSky : kBarInactive);
        }

        p.setPen(QPen(kAccentSky, 1.5));
        p.drawLine(QPointF(selX0, plot.top()), QPointF(selX0, plot.bottom()));
        p.drawLine(QPointF(selX1, plot.top()), QPointF(selX1, plot.bottom()));

        p.setPen(kTextSecondary);
        QFont f = p.font();
        f.setPixelSize(kMinFontSizePx);
        p.setFont(f);
        p.drawText(QRectF(plot.left(), plot.bottom() + 2.0, plot.width() * 0.5, 16.0),
                   Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("20 Hz (B0)"));
        p.drawText(
            QRectF(plot.left() + plot.width() * 0.5, plot.bottom() + 2.0, plot.width() * 0.5, 16.0),
            Qt::AlignRight | Qt::AlignVCenter, formatNyquistLabel(m_sampleRateHz));
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            return;
        }
        m_dragging = true;
        m_anchorBand = bandAtX(event->position().x());
        applySelection(m_anchorBand, m_anchorBand);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_dragging) {
            return;
        }
        const uint16_t cur = bandAtX(event->position().x());
        applySelection(std::min(m_anchorBand, cur), std::max(m_anchorBand, cur));
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        (void)event;
        m_dragging = false;
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        const int delta = (event->key() == Qt::Key_Left)    ? -1
                          : (event->key() == Qt::Key_Right) ? 1
                                                            : 0;
        if (delta == 0) {
            QWidget::keyPressEvent(event);
            return;
        }
        const bool adjustEndOnly = (event->modifiers() & Qt::ShiftModifier) != 0;
        if (adjustEndOnly) {
            const int nextEnd =
                std::clamp(static_cast<int>(m_bandEnd) + delta, static_cast<int>(m_bandStart), 63);
            applySelection(m_bandStart, static_cast<uint16_t>(nextEnd));
        } else {
            const int span = static_cast<int>(m_bandEnd) - static_cast<int>(m_bandStart);
            const int maxStart = 63 - span;
            const int nextStart = std::clamp(static_cast<int>(m_bandStart) + delta, 0, maxStart);
            applySelection(static_cast<uint16_t>(nextStart),
                           static_cast<uint16_t>(nextStart + span));
        }
        event->accept();
    }

  private:
    [[nodiscard]] uint16_t bandAtX(qreal x) const noexcept
    {
        const qreal safeX = std::isfinite(x) ? x : 0.0;
        const qreal w = std::max<qreal>(1.0, width() - 8.0);
        const qreal norm = std::clamp((safeX - 4.0) / w, 0.0, 0.9999);
        return static_cast<uint16_t>(std::clamp(static_cast<int>(norm * 64.0), 0, 63));
    }

    void applySelection(uint16_t s, uint16_t e)
    {
        m_bandStart = std::min<uint16_t>(s, 63u);
        m_bandEnd = std::clamp<uint16_t>(e, m_bandStart, 63u);
        if (isVisible()) {
            update();
        }
        if (m_onRangeChanged) {
            m_onRangeChanged(m_bandStart, m_bandEnd);
        }
    }

    std::function<void(uint16_t, uint16_t)> m_onRangeChanged;
    std::array<float, 64> m_bands{};
    uint32_t m_sampleRateHz{48000u};
    uint16_t m_bandStart{0};
    uint16_t m_bandEnd{1};
    uint16_t m_anchorBand{0};
    bool m_dragging{false};
};

class ClearAudioBindingsCommand final : public ICommand {
  public:
    using ApplyFunction = std::function<void(const std::vector<we_audio_binding_t> &)>;

    ClearAudioBindingsCommand(std::vector<we_audio_binding_t> previous, ApplyFunction applyFunc)
        : m_previous(std::move(previous)), m_applyFunc(std::move(applyFunc))
    {
    }

    void execute() override
    {
        if (m_applyFunc) {
            m_applyFunc({});
        }
    }

    void undo() override
    {
        if (m_applyFunc) {
            m_applyFunc(m_previous);
        }
    }

    [[nodiscard]] QString describe() const override
    {
        return QStringLiteral("Clear all audio bindings (%1)").arg(m_previous.size());
    }

    [[nodiscard]] size_t memoryFootprint() const override
    {
        return sizeof(*this) + m_previous.capacity() * sizeof(we_audio_binding_t);
    }

  private:
    std::vector<we_audio_binding_t> m_previous;
    ApplyFunction m_applyFunc;
};

QDoubleSpinBox *makeSpin(double minV, double maxV, double step, int decimals, QWidget *parent)
{
    auto *spin = new QDoubleSpinBox(parent);
    spin->setRange(minV, maxV);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    spin->setKeyboardTracking(false);
    spin->setMinimumHeight(kMinControlSizePx);
    return spin;
}

QSpinBox *makeIntSpin(int minV, int maxV, QWidget *parent)
{
    auto *spin = new QSpinBox(parent);
    spin->setRange(minV, maxV);
    spin->setKeyboardTracking(false);
    spin->setMinimumHeight(kMinControlSizePx);
    return spin;
}

QString formatBandHzSummary(uint16_t startBand, uint16_t endBand, uint32_t sampleRateHz)
{
    const float f0 = AudioBindingWizard::bandCenterHz(startBand, sampleRateHz);
    const float f1 = AudioBindingWizard::bandCenterHz(endBand, sampleRateHz);
    const float z0 = wwal::studio::AudioSpectrumWidget::hzToBark(f0);
    const float z1 = wwal::studio::AudioSpectrumWidget::hzToBark(f1);
    return QString::asprintf("Bands %u..%u | %.0f Hz - %.0f Hz (%.1f - %.1f Bark)", startBand,
                             endBand, static_cast<double>(f0), static_cast<double>(f1),
                             static_cast<double>(z0), static_cast<double>(z1));
}

QString targetKindName(uint32_t targetKind)
{
    switch (static_cast<we_audio_target_kind_t>(targetKind)) {
    case WE_AUDIO_TGT_LAYER_ORIGIN_X:
        return QStringLiteral("Origin X");
    case WE_AUDIO_TGT_LAYER_ORIGIN_Y:
        return QStringLiteral("Origin Y");
    case WE_AUDIO_TGT_LAYER_SCALE_XY:
        return QStringLiteral("Scale XY");
    case WE_AUDIO_TGT_LAYER_ROTATION_Z:
        return QStringLiteral("Rotation Z");
    case WE_AUDIO_TGT_LAYER_ALPHA:
        return QStringLiteral("Layer Alpha");
    case WE_AUDIO_TGT_LAYER_BRIGHTNESS:
        return QStringLiteral("Brightness");
    case WE_AUDIO_TGT_LAYER_COLOR_RGB:
        return QStringLiteral("Color RGB");
    case WE_AUDIO_TGT_PASS_UNIFORM_COMP:
        return QStringLiteral("Shader Uniform");
    case WE_AUDIO_TGT_LIGHT_INTENSITY:
        return QStringLiteral("Light Intensity");
    case WE_AUDIO_TGT_LIGHT_RADIUS:
        return QStringLiteral("Light Radius");
    case WE_AUDIO_TGT_PARTICLE_RATE:
        return QStringLiteral("Particle Rate");
    case WE_AUDIO_TGT_PARTICLE_SPEED:
        return QStringLiteral("Particle Speed");
    case WE_AUDIO_TGT_PARTICLE_TURB_SPEED:
        return QStringLiteral("Turbulence Speed");
    case WE_AUDIO_TGT_PARTICLE_VORTEX_SPD:
        return QStringLiteral("Vortex Speed");
    case WE_AUDIO_TGT_PARTICLE_BURST_TRIG:
        return QStringLiteral("Particle Burst Trigger");
    }
    return QStringLiteral("Target");
}

QString bindingSummary(const we_audio_binding_t &b)
{
    QString srcStr;
    switch (static_cast<we_audio_src_kind_t>(b.src_kind)) {
    case WE_AUDIO_SRC_BARK_RANGE:
        srcStr = QStringLiteral("Bark[%1..%2]").arg(b.band_start).arg(b.band_end);
        break;
    case WE_AUDIO_SRC_ENVELOPE:
        srcStr = QStringLiteral("RMS Envelope");
        break;
    case WE_AUDIO_SRC_BASS_ENERGY:
        srcStr = QStringLiteral("Bass Energy");
        break;
    case WE_AUDIO_SRC_BEAT_PULSE:
        srcStr = QStringLiteral("Beat Pulse");
        break;
    }

    QString tgtStr = targetKindName(b.target_kind);
    if (b.target_kind == WE_AUDIO_TGT_PASS_UNIFORM_COMP && b.uniform_name[0] != '\0') {
        tgtStr =
            QStringLiteral("%1[%2]").arg(QString::fromUtf8(b.uniform_name)).arg(b.target_component);
    }

    return QStringLiteral("#%1  %2 -> Obj %3 %4 [%5..%6]")
        .arg(b.binding_id)
        .arg(srcStr)
        .arg(b.target_object_id)
        .arg(tgtStr)
        .arg(static_cast<double>(b.out_min), 0, 'f', 2)
        .arg(static_cast<double>(b.out_max), 0, 'f', 2);
}

template <typename Client = IpcClient>
void dispatchAudioBindingsIpc(Client *client, const QString &output, we_audio_bind_op_t op,
                              const we_audio_binding_t *bindings, uint32_t count, uint32_t removeId)
{
    if (!client) {
        return;
    }

    if constexpr (requires(Client *c) {
                      c->sendAudioBindings(output, op, bindings, count, removeId);
                  }) {
        client->sendAudioBindings(output, op, bindings, count, removeId);
    } else if constexpr (requires(Client *c) {
                             c->requestSetAudioBindings(output, op, bindings, count, removeId);
                         }) {
        client->requestSetAudioBindings(output, op, bindings, count, removeId);
    } else {
        (void)output;
        (void)op;
        (void)bindings;
        (void)count;
        (void)removeId;
    }
}

QString darkWizardStyleSheet()
{
    return QStringLiteral(
        "QWidget { background-color: #12141a; color: #f1f5f9; font-size: 12px; }"
        "QLabel { color: #f1f5f9; font-size: 12px; }"
        "QGroupBox { background-color: #181b22; border: 1px solid #2a2f3d; border-radius: 6px; "
        "margin-top: 8px; padding: 8px; font-weight: 600; color: #f1f5f9; font-size: 12px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; color: #94a3b8; }"
        "QComboBox, QSpinBox, QDoubleSpinBox, QLineEdit, QListWidget { background-color: #0b0c10; "
        "border: 1px solid #2a2f3d; border-radius: 4px; padding: 3px 6px; color: #f1f5f9; "
        "min-height: 28px; font-size: 12px; }"
        "QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus, QLineEdit:focus, "
        "QListWidget:focus, QPushButton:focus, QPushButton:checked:focus, "
        "QPushButton#createBindingBtn:focus { border: 2px solid #38bdf8; }"
        "QListWidget::item:selected { background-color: #0369a1; color: #f1f5f9; }"
        "QPushButton { background-color: #181b22; border: 1px solid #2a2f3d; border-radius: 4px; "
        "padding: 4px 10px; color: #f1f5f9; min-height: 28px; min-width: 28px; font-size: 12px; }"
        "QPushButton:hover { background-color: #222631; border-color: #38bdf8; }"
        "QPushButton:checked { background-color: #222631; border-color: #38bdf8; color: #38bdf8; }"
        "QPushButton#createBindingBtn { background-color: #0369a1; border: 1px solid #38bdf8; "
        "color: #f1f5f9; font-weight: 600; }"
        "QPushButton#createBindingBtn:hover { background-color: #0284c7; border-color: #7dd3fc; }"
        "QPushButton:disabled, QComboBox:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, "
        "QLineEdit:disabled { background-color: #12141a; border-color: #1e2430; color: #94a3b8; }");
}

} // namespace

/* ========================================================================= */
/* AudioEnvelopeOscilloscopeWidget                                           */
/* ========================================================================= */

AudioEnvelopeOscilloscopeWidget::AudioEnvelopeOscilloscopeWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(140, 110);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("Audio Envelope Oscilloscope"));
    setAccessibleDescription(
        tr("Dual-trace live oscilloscope showing gained Bark energy, Giannoulis smoothed "
           "envelope, and draggable threshold gate."));
}

void AudioEnvelopeOscilloscopeWidget::setThresholdGate(float thresholdOpen,
                                                       float hysteresis) noexcept
{
    const float nextThresh = sanitizeFinite(thresholdOpen, 0.0f, 0.98f, 0.15f);
    const float nextHyst = sanitizeFinite(hysteresis, 0.0f, 0.25f, 0.04f);
    if (std::fabs(nextThresh - m_thresholdOpen) <= 1e-5f &&
        std::fabs(nextHyst - m_hysteresis) <= 1e-5f) {
        return;
    }
    m_thresholdOpen = nextThresh;
    m_hysteresis = nextHyst;
    if (isVisible()) {
        update();
    }
}

void AudioEnvelopeOscilloscopeWidget::pushSample(float rawEnergy, float smoothedEnvelope,
                                                 bool gateOpen) noexcept
{
    const float cleanRaw = sanitizeFinite(rawEnergy, 0.0f, 1.0f, 0.0f);
    const float cleanEnv = sanitizeFinite(smoothedEnvelope, 0.0f, 1.0f, 0.0f);
    const uint8_t gateVal = gateOpen ? 1u : 0u;

    const bool hadContentBefore = (m_nonZeroCount > 0u);
    const bool oldNonZero = (m_rawTrace[m_writeIdx] > 1e-5f || m_envTrace[m_writeIdx] > 1e-5f ||
                             m_gateTrace[m_writeIdx] != 0u);
    const bool newNonZero = (cleanRaw > 1e-5f || cleanEnv > 1e-5f || gateVal != 0u);

    if (oldNonZero && !newNonZero && m_nonZeroCount > 0u) {
        --m_nonZeroCount;
    } else if (!oldNonZero && newNonZero) {
        ++m_nonZeroCount;
    }

    m_rawTrace[m_writeIdx] = cleanRaw;
    m_envTrace[m_writeIdx] = cleanEnv;
    m_gateTrace[m_writeIdx] = gateVal;
    m_writeIdx = (m_writeIdx + 1u) % kHistorySamples;

    // Skip repaint when hidden or when the full ring buffer is already settled at silence.
    if ((hadContentBefore || newNonZero) && isVisible()) {
        update();
    }
}

void AudioEnvelopeOscilloscopeWidget::clearHistory() noexcept
{
    if (m_nonZeroCount == 0u && m_writeIdx == 0u) {
        return;
    }
    m_rawTrace.fill(0.0f);
    m_envTrace.fill(0.0f);
    m_gateTrace.fill(0u);
    m_writeIdx = 0u;
    m_nonZeroCount = 0u;
    if (isVisible()) {
        update();
    }
}

void AudioEnvelopeOscilloscopeWidget::paintEvent(QPaintEvent *event)
{
    (void)event;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF fullRect = rect();
    p.fillRect(fullRect, kBgCanvas);
    const bool focused = hasFocus();
    p.setPen(QPen(focused ? kAccentSky : kBorderSubtle, focused ? 2.0 : 1.0));
    p.drawRoundedRect(focused ? fullRect.adjusted(1.0, 1.0, -1.0, -1.0)
                              : fullRect.adjusted(0.5, 0.5, -0.5, -0.5),
                      4.0, 4.0);

    const QRectF plot = fullRect.adjusted(8.0, 8.0, -8.0, -8.0);
    if (plot.width() <= 4.0 || plot.height() <= 4.0) {
        return;
    }

    p.setPen(QPen(kBorderSubtle, 1.0, Qt::DotLine));
    for (int i = 1; i <= 3; ++i) {
        const qreal gy = plot.top() + (static_cast<qreal>(i) / 4.0) * plot.height();
        p.drawLine(QPointF(plot.left(), gy), QPointF(plot.right(), gy));
    }

    const qreal gateOpenY = plot.bottom() - static_cast<qreal>(m_thresholdOpen) * plot.height();
    const qreal gateCloseVal = std::max(0.0f, m_thresholdOpen - m_hysteresis);
    const qreal gateCloseY = plot.bottom() - static_cast<qreal>(gateCloseVal) * plot.height();
    if (gateCloseY > gateOpenY) {
        p.fillRect(QRectF(plot.left(), gateOpenY, plot.width(), gateCloseY - gateOpenY),
                   QColor(0xf5, 0x9e, 0x0b, 22));
    }

    QPainterPath rawPath;
    QPainterPath envPath;
    const qreal dx = plot.width() / static_cast<qreal>(kHistorySamples - 1u);

    for (size_t i = 0; i < kHistorySamples; ++i) {
        const size_t ringIdx = (m_writeIdx + i) % kHistorySamples;
        const qreal x = plot.left() + static_cast<qreal>(i) * dx;
        const qreal yRaw = plot.bottom() - static_cast<qreal>(m_rawTrace[ringIdx]) * plot.height();
        const qreal yEnv = plot.bottom() - static_cast<qreal>(m_envTrace[ringIdx]) * plot.height();

        if (m_gateTrace[ringIdx] != 0u) {
            p.fillRect(QRectF(x, plot.bottom() - 3.0, std::max(1.5, dx + 0.5), 3.0),
                       QColor(0x22, 0xc5, 0x5e, 180));
        }

        if (i == 0) {
            rawPath.moveTo(x, yRaw);
            envPath.moveTo(x, yEnv);
        } else {
            rawPath.lineTo(x, yRaw);
            envPath.lineTo(x, yEnv);
        }
    }

    p.setPen(QPen(kTextSecondary, 1.2));
    p.drawPath(rawPath);
    p.setPen(QPen(kAccentSky, 2.0));
    p.drawPath(envPath);

    /* Threshold gate marker uses a 2px dashed amber line + explicit GATE label. */
    QPen gatePen(kSemanticWarn, 2.0, Qt::DashLine);
    p.setPen(gatePen);
    p.drawLine(QPointF(plot.left(), gateOpenY), QPointF(plot.right(), gateOpenY));

    p.setPen(kSemanticWarn);
    QFont labelFont = p.font();
    labelFont.setPixelSize(kMinFontSizePx);
    labelFont.setBold(true);
    p.setFont(labelFont);
    const qreal labelY = std::max(plot.top() + 13.0, gateOpenY - 4.0);
    p.drawText(QPointF(plot.left() + 4.0, labelY),
               QString::asprintf("GATE %.2f", static_cast<double>(m_thresholdOpen)));
}

void AudioEnvelopeOscilloscopeWidget::updateThresholdFromY(qreal posY)
{
    const QRectF plot = QRectF(rect()).adjusted(8.0, 8.0, -8.0, -8.0);
    if (plot.height() <= 2.0 || !std::isfinite(posY)) {
        return;
    }
    const float val = sanitizeFinite(static_cast<float>((plot.bottom() - posY) / plot.height()),
                                     0.0f, 0.98f, m_thresholdOpen);
    if (std::fabs(val - m_thresholdOpen) > 1e-5f) {
        m_thresholdOpen = val;
        emit thresholdGateDragged(val);
        update();
    }
}

void AudioEnvelopeOscilloscopeWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }
    m_draggingGate = true;
    updateThresholdFromY(event->position().y());
}

void AudioEnvelopeOscilloscopeWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_draggingGate) {
        return;
    }
    updateThresholdFromY(event->position().y());
}

void AudioEnvelopeOscilloscopeWidget::mouseReleaseEvent(QMouseEvent *event)
{
    (void)event;
    m_draggingGate = false;
}

void AudioEnvelopeOscilloscopeWidget::keyPressEvent(QKeyEvent *event)
{
    const float step = (event->modifiers() & Qt::ShiftModifier) ? 0.10f : 0.02f;
    float delta = 0.0f;
    if (event->key() == Qt::Key_Up || event->key() == Qt::Key_PageUp) {
        delta = (event->key() == Qt::Key_PageUp) ? 0.10f : step;
    } else if (event->key() == Qt::Key_Down || event->key() == Qt::Key_PageDown) {
        delta = (event->key() == Qt::Key_PageDown) ? -0.10f : -step;
    }
    if (delta == 0.0f) {
        QWidget::keyPressEvent(event);
        return;
    }
    const float nextVal = sanitizeFinite(m_thresholdOpen + delta, 0.0f, 0.98f, m_thresholdOpen);
    if (std::fabs(nextVal - m_thresholdOpen) > 1e-5f) {
        m_thresholdOpen = nextVal;
        emit thresholdGateDragged(nextVal);
        update();
    }
    event->accept();
}

/* ========================================================================= */
/* AudioBindingWizard                                                        */
/* ========================================================================= */

struct AudioBindingWizard::ExtraState {
    QComboBox *srcKindCombo{nullptr};
    QSpinBox *bandStartSpin{nullptr};
    QSpinBox *bandEndSpin{nullptr};
    BarkRangePickerWidget *barkPicker{nullptr};
    QLineEdit *uniformNameEdit{nullptr};
    QSpinBox *componentSpin{nullptr};
    QSpinBox *subIndexSpin{nullptr};
    QDoubleSpinBox *baseValueSpin{nullptr};
    QDoubleSpinBox *hysteresisSpin{nullptr};
    QPushButton *prevStepBtn{nullptr};
    QPushButton *nextStepBtn{nullptr};
    std::array<QPushButton *, 3> stepTabBtns{};
    bool updatingUi{false};
    bool hasPreselectedTarget{false};
    bool hasPresetApplied{false};
    bool curveAutoSetByOnset{false};
};

AudioBindingWizard::AudioBindingWizard(QWidget *parent)
    : QWidget(parent), m_extra(std::make_unique<ExtraState>())
{
    setAccessibleName(tr("Audio Binding Wizard"));
    setAccessibleDescription(
        tr("No-code 3-step PipeWire 64-band Bark FFT audio-binding configurator."));

    m_draft.binding_id = 0u;
    m_draft.target_object_id = 0;
    m_draft.target_kind = WE_AUDIO_TGT_LAYER_ALPHA;
    m_draft.src_kind = WE_AUDIO_SRC_BARK_RANGE;
    m_draft.agg_mode = WE_AUDIO_AGG_PEAK_WEIGHTED;
    m_draft.band_start = 0u;
    m_draft.band_end = 1u;
    m_draft.input_gain = 1.5f;
    m_draft.threshold_open = 0.15f;
    m_draft.hysteresis = 0.04f;
    m_draft.attack_sec = 0.015f;
    m_draft.release_sec = 0.180f;
    m_draft.curve_shape = WE_AUDIO_CURVE_LINEAR;
    m_draft.blend_mode = WE_AUDIO_BLEND_ADD;
    m_draft.out_min = 0.0f;
    m_draft.out_max = 1.0f;
    m_draft.base_value = 0.0f;
    m_draft.enabled = 1u;
}

AudioBindingWizard::~AudioBindingWizard() = default;

float AudioBindingWizard::bandCenterHz(size_t band, uint32_t sampleRateHz) noexcept
{
    const size_t clampedBand =
        std::min<size_t>(band, wwal::studio::AudioSpectrumWidget::NumBands - 1u);
    const float maxFreq = 0.5f * static_cast<float>(sanitizeSampleRateHz(sampleRateHz));
    float maxBark = wwal::studio::AudioSpectrumWidget::hzToBark(maxFreq);
    if (!std::isfinite(maxBark) || maxBark <= 0.1f) {
        maxBark = 24.0f;
    }
    const float zCenter = ((static_cast<float>(clampedBand) + 0.5f) /
                           static_cast<float>(wwal::studio::AudioSpectrumWidget::NumBands)) *
                          maxBark;
    return wwal::studio::AudioSpectrumWidget::barkToHz(zCenter);
}

void AudioBindingWizard::setSampleRateHz(uint32_t sampleRateHz)
{
    const uint32_t safeRate = sanitizeSampleRateHz(sampleRateHz);
    if (m_sampleRateHz == safeRate) {
        return;
    }
    m_sampleRateHz = safeRate;
    if (!m_uiInitialized) {
        return;
    }
    if (m_extra->barkPicker) {
        m_extra->barkPicker->setSampleRateHz(m_sampleRateHz);
    }
    if (m_bandRangeHzLabel) {
        m_bandRangeHzLabel->setText(
            formatBandHzSummary(m_draft.band_start, m_draft.band_end, m_sampleRateHz));
    }
}

int AudioBindingWizard::currentStepIndex() const noexcept
{
    return m_stepStack ? m_stepStack->currentIndex() : 0;
}

std::pair<uint16_t, uint16_t>
AudioBindingWizard::barkBandSpanForPreset(MusicalBandPreset preset) noexcept
{
    switch (preset) {
    case MusicalBandPreset::SubBass20To80Hz:
        return {0u, 1u};
    case MusicalBandPreset::Bass80To250Hz:
        return {2u, 6u};
    case MusicalBandPreset::LowMids250To500Hz:
        return {7u, 12u};
    case MusicalBandPreset::Mids500To2000Hz:
        return {13u, 33u};
    case MusicalBandPreset::UpperMids2To3kHz:
        return {34u, 40u};
    case MusicalBandPreset::Brilliance3To16kHz:
        return {41u, 62u};
    case MusicalBandPreset::BeatPulseOnset:
        return {0u, 63u};
    }
    return {0u, 1u};
}

void AudioBindingWizard::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    ensureUiInitialized();
}

void AudioBindingWizard::setSceneLayers(const std::vector<SceneLayerNode> &layers)
{
    m_layers = layers;
    if (!m_uiInitialized || !m_targetLayerCombo) {
        return;
    }
    const QVariant curData = m_targetLayerCombo->currentData();
    const bool hadExplicitTarget = curData.isValid() || m_extra->hasPreselectedTarget;
    const int32_t prevId = curData.isValid() ? curData.toInt() : m_draft.target_object_id;
    QSignalBlocker block(m_targetLayerCombo);
    m_targetLayerCombo->clear();
    for (const auto &node : m_layers) {
        const QString label = QStringLiteral("[%1] %2").arg(node.id).arg(
            node.name.isEmpty() ? tr("Layer") : node.name);
        m_targetLayerCombo->addItem(label, node.id);
    }
    if (m_layers.empty()) {
        m_targetLayerCombo->addItem(tr("Scene Root (0)"), 0);
    }
    if (const int idx = m_targetLayerCombo->findData(prevId); idx >= 0) {
        m_targetLayerCombo->setCurrentIndex(idx);
    } else if (hadExplicitTarget) {
        m_targetLayerCombo->addItem(tr("Object #%1").arg(prevId), prevId);
        m_targetLayerCombo->setCurrentIndex(m_targetLayerCombo->count() - 1);
    }
}

void AudioBindingWizard::setActiveBindings(std::vector<we_audio_binding_t> bindings)
{
    if (bindings.size() > WE_MAX_AUDIO_BINDINGS) {
        bindings.resize(WE_MAX_AUDIO_BINDINGS);
    }
    m_bindings = std::move(bindings);

    uint32_t maxId = 0u;
    for (const auto &b : m_bindings) {
        maxId = std::max(maxId, b.binding_id);
    }
    if (maxId < std::numeric_limits<uint32_t>::max()) {
        m_nextBindingId = std::max(m_nextBindingId, maxId + 1u);
    }

    if (m_uiInitialized && m_activeBindingsList) {
        m_activeBindingsList->clear();
        for (const auto &b : m_bindings) {
            m_activeBindingsList->addItem(bindingSummary(b));
        }
    }
}

void AudioBindingWizard::preselectTargetUniform(int32_t objectId, const QString &uniformName,
                                                uint32_t componentIdx, float currentBaseline)
{
    m_extra->hasPreselectedTarget = true;
    m_draft.target_object_id = objectId;
    m_draft.target_kind = WE_AUDIO_TGT_PASS_UNIFORM_COMP;
    m_draft.target_component = std::min(componentIdx, 3u);
    m_draft.target_sub_index =
        std::min<uint32_t>(m_draft.target_sub_index, static_cast<uint32_t>(kMaxEffectPassSubIndex));
    m_draft.base_value = sanitizeFinite(currentBaseline, -10000.0f, 10000.0f, 0.0f);
    m_draft.out_min = m_draft.base_value;
    m_draft.out_max = sanitizeFinite(m_draft.base_value + 1.0f, -10000.0f, 10000.0f, 1.0f);
    const QByteArray uname = uniformName.toUtf8();
    std::snprintf(m_draft.uniform_name, sizeof(m_draft.uniform_name), "%s", uname.constData());

    if (!m_uiInitialized) {
        return;
    }

    auto &ex = *m_extra;
    {
        const QScopedValueRollback<bool> guard(ex.updatingUi, true);
        {
            QSignalBlocker blockLayer(m_targetLayerCombo);
            int lIdx = m_targetLayerCombo->findData(objectId);
            if (lIdx < 0) {
                m_targetLayerCombo->addItem(tr("Object #%1").arg(objectId), objectId);
                lIdx = m_targetLayerCombo->count() - 1;
            }
            m_targetLayerCombo->setCurrentIndex(lIdx);
        }
        m_targetCategoryCombo->setCurrentIndex(1);
        if (const int pIdx =
                m_targetParamCombo->findData(static_cast<int>(WE_AUDIO_TGT_PASS_UNIFORM_COMP));
            pIdx >= 0) {
            m_targetParamCombo->setCurrentIndex(pIdx);
        }
        updateTargetParamUiState();
        ex.uniformNameEdit->setText(uniformName);
        ex.componentSpin->setValue(static_cast<int>(m_draft.target_component));
        ex.subIndexSpin->setValue(static_cast<int>(m_draft.target_sub_index));
        ex.baseValueSpin->setValue(static_cast<double>(m_draft.base_value));
        m_outMinSpin->setValue(static_cast<double>(m_draft.out_min));
        m_outMaxSpin->setValue(static_cast<double>(m_draft.out_max));
    }
    syncStepUi(0);
}

void AudioBindingWizard::applyMusicalBandPreset(MusicalBandPreset preset)
{
    const auto span = barkBandSpanForPreset(preset);
    m_extra->hasPresetApplied = true;
    m_draft.band_start = span.first;
    m_draft.band_end = span.second;
    m_draft.src_kind = (preset == MusicalBandPreset::BeatPulseOnset) ? WE_AUDIO_SRC_BEAT_PULSE
                                                                     : WE_AUDIO_SRC_BARK_RANGE;
    if (preset == MusicalBandPreset::BeatPulseOnset) {
        m_draft.curve_shape = WE_AUDIO_CURVE_TRIG_EDGE;
        m_extra->curveAutoSetByOnset = true;
    } else if (m_draft.curve_shape == WE_AUDIO_CURVE_TRIG_EDGE) {
        m_draft.curve_shape = WE_AUDIO_CURVE_LINEAR;
        m_extra->curveAutoSetByOnset = false;
    }

    if (!m_uiInitialized) {
        return;
    }

    auto &ex = *m_extra;
    const QScopedValueRollback<bool> guard(ex.updatingUi, true);
    if (const int pIdx = m_bandPresetCombo->findData(static_cast<int>(preset)); pIdx >= 0) {
        m_bandPresetCombo->setCurrentIndex(pIdx);
    }
    if (const int sIdx = ex.srcKindCombo->findData(static_cast<int>(m_draft.src_kind)); sIdx >= 0) {
        ex.srcKindCombo->setCurrentIndex(sIdx);
    }
    ex.bandStartSpin->setValue(m_draft.band_start);
    ex.bandEndSpin->setValue(m_draft.band_end);
    if (ex.barkPicker) {
        ex.barkPicker->setBandRange(m_draft.band_start, m_draft.band_end);
    }
    if (m_bandRangeHzLabel) {
        m_bandRangeHzLabel->setText(
            formatBandHzSummary(m_draft.band_start, m_draft.band_end, m_sampleRateHz));
    }
    if (const int cIdx = m_curveShapeCombo->findData(static_cast<int>(m_draft.curve_shape));
        cIdx >= 0) {
        m_curveShapeCombo->setCurrentIndex(cIdx);
    }
}

void AudioBindingWizard::updateLiveBarkTelemetry(const std::array<float, 64> &barkBands,
                                                 float envelope, float bassEnergy, float beatPulse,
                                                 float dtSec)
{
    const float clampedDt = sanitizeFinite(dtSec, 1e-4f, 0.25f, 1.0f / 60.0f);
    const uint16_t b0 = std::min<uint16_t>(m_draft.band_start, 63u);
    const uint16_t b1 = std::clamp<uint16_t>(m_draft.band_end, b0, 63u);

    std::array<float, 64> cleanBands{};
    for (size_t b = 0; b < cleanBands.size(); ++b) {
        cleanBands[b] = sanitizeFinite(barkBands[b], 0.0f, 1.0f, 0.0f);
    }

    float rawInput = 0.0f;
    switch (static_cast<we_audio_src_kind_t>(m_draft.src_kind)) {
    case WE_AUDIO_SRC_BARK_RANGE: {
        float sum = 0.0f;
        float peak = 0.0f;
        for (uint16_t b = b0; b <= b1; ++b) {
            const float v = cleanBands[b];
            sum += v;
            peak = std::max(peak, v);
        }
        const float mean = sum / static_cast<float>(b1 - b0 + 1u);
        if (m_draft.agg_mode == WE_AUDIO_AGG_MEAN) {
            rawInput = mean;
        } else if (m_draft.agg_mode == WE_AUDIO_AGG_PEAK) {
            rawInput = peak;
        } else {
            rawInput = 0.7f * peak + 0.3f * mean;
        }
        break;
    }
    case WE_AUDIO_SRC_ENVELOPE:
        rawInput = sanitizeFinite(envelope, 0.0f, 1.0f, 0.0f);
        break;
    case WE_AUDIO_SRC_BASS_ENERGY:
        rawInput = sanitizeFinite(bassEnergy, 0.0f, 1.0f, 0.0f);
        break;
    case WE_AUDIO_SRC_BEAT_PULSE:
        rawInput = sanitizeFinite(beatPulse, 0.0f, 1.0f, 0.0f);
        break;
    }
    rawInput = sanitizeFinite(rawInput, 0.0f, 1.0f, 0.0f);

    const float gain = sanitizeFinite(m_draft.input_gain, 0.0f, 10.0f, 1.5f);
    const float thresholdOpen = sanitizeFinite(m_draft.threshold_open, 0.0f, 0.98f, 0.15f);
    const float hysteresis = sanitizeFinite(m_draft.hysteresis, 0.0f, 0.25f, 0.04f);
    const float gainedInput = sanitizeFinite(rawInput * gain, 0.0f, 1.0f, 0.0f);

    /* Gate compares the same input_gain-scaled signal plotted on the oscilloscope so threshold
     * overlay & trigger align. */
    if (m_draft.gate_open == 0u && gainedInput >= thresholdOpen) {
        m_draft.gate_open = 1u;
    } else if (m_draft.gate_open != 0u &&
               gainedInput < std::max(0.0f, thresholdOpen - hysteresis)) {
        m_draft.gate_open = 0u;
    }

    const float gated = (m_draft.gate_open != 0u) ? gainedInput : 0.0f;
    const float attackSec = sanitizeFinite(m_draft.attack_sec, 0.0f, 60.0f, 0.015f);
    const float releaseSec = sanitizeFinite(m_draft.release_sec, 0.0f, 60.0f, 0.180f);
    const float prevEnv = sanitizeFinite(m_draft.env_state, 0.0f, 1.0f, 0.0f);
    const float tau = (gated > prevEnv) ? attackSec : releaseSec;
    const float alpha = (tau <= 1e-4f) ? 1.0f : (1.0f - std::exp(-clampedDt / tau));
    float nextEnv = sanitizeFinite(prevEnv + alpha * (gated - prevEnv), 0.0f, 1.0f, 0.0f);
    if (gated == 0.0f && nextEnv < 1e-4f) {
        nextEnv = 0.0f;
    }
    m_draft.env_state = nextEnv;
    m_draft.last_output = nextEnv;

    if (!m_uiInitialized) {
        return;
    }

    auto &ex = *m_extra;
    if (ex.barkPicker && isVisible() && m_stepStack && m_stepStack->currentIndex() == 0) {
        ex.barkPicker->setLiveSpectrum(cleanBands);
    }
    if (m_oscilloscope) {
        m_oscilloscope->pushSample(gainedInput, m_draft.env_state, m_draft.gate_open != 0u);
    }
}

void AudioBindingWizard::syncStepUi(int stepIdx)
{
    if (!m_uiInitialized || !m_stepStack || !m_extra) {
        return;
    }
    auto &st = *m_extra;
    stepIdx = std::clamp(stepIdx, 0, 2);
    m_stepStack->setCurrentIndex(stepIdx);
    if (stepIdx != 0 && st.barkPicker) {
        st.barkPicker->clearLiveSpectrum();
    }
    for (int i = 0; i < 3; ++i) {
        if (auto *btn = st.stepTabBtns[static_cast<size_t>(i)]) {
            QSignalBlocker block(btn);
            btn->setChecked(i == stepIdx);
        }
    }
    if (st.prevStepBtn) {
        st.prevStepBtn->setEnabled(stepIdx > 0);
    }
    if (st.nextStepBtn) {
        st.nextStepBtn->setEnabled(stepIdx < 2);
    }
}

void AudioBindingWizard::updateTargetParamUiState()
{
    if (!m_uiInitialized || !m_targetParamCombo || !m_extra) {
        return;
    }
    auto &st = *m_extra;
    const QVariant paramData = m_targetParamCombo->currentData();
    const uint32_t targetKind = paramData.isValid() ? paramData.toUInt() : m_draft.target_kind;
    const SubIndexBounds bounds = subIndexBoundsForTarget(targetKind);
    const bool isUniformTarget = (targetKind == WE_AUDIO_TGT_PASS_UNIFORM_COMP);

    if (st.subIndexSpin) {
        QSignalBlocker blockSub(st.subIndexSpin);
        st.subIndexSpin->setRange(0, bounds.maxIndex);
        if (!bounds.enabled) {
            st.subIndexSpin->setValue(0);
        } else {
            st.subIndexSpin->setValue(std::clamp(st.subIndexSpin->value(), 0, bounds.maxIndex));
        }
        st.subIndexSpin->setEnabled(bounds.enabled);
        st.subIndexSpin->setToolTip(tr(bounds.hint));
    }
    if (st.uniformNameEdit) {
        st.uniformNameEdit->setEnabled(isUniformTarget);
    }
    if (st.componentSpin) {
        st.componentSpin->setEnabled(isUniformTarget);
        if (!isUniformTarget) {
            QSignalBlocker blockComp(st.componentSpin);
            st.componentSpin->setValue(0);
        }
    }
}

void AudioBindingWizard::ensureUiInitialized()
{
    if (m_uiInitialized) {
        return;
    }
    m_uiInitialized = true;
    setStyleSheet(darkWizardStyleSheet());

    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(6, 6, 6, 6);
    outerLayout->setSpacing(6);

    /* Step Navigation Header */
    auto *stepNavRow = new QHBoxLayout();
    stepNavRow->setSpacing(4);
    auto &ex = *m_extra;
    static constexpr const char *kStepLabels[3] = {
        "1. Bark Source",
        "2. Target Param",
        "3. Envelope & Bind",
    };
    for (int i = 0; i < 3; ++i) {
        auto *btn = new QPushButton(QLatin1String(kStepLabels[i]), this);
        btn->setCheckable(true);
        btn->setChecked(i == 0);
        btn->setMinimumSize(kMinControlSizePx, kMinControlSizePx);
        ex.stepTabBtns[static_cast<size_t>(i)] = btn;
        stepNavRow->addWidget(btn, 1);
    }
    outerLayout->addLayout(stepNavRow);

    m_stepStack = new QStackedWidget(this);

    /* ---- Step 1: Bark Source & 64-Band Range Picker ---- */
    auto *step1 = new QWidget(m_stepStack);
    auto *s1Layout = new QVBoxLayout(step1);
    s1Layout->setContentsMargins(0, 4, 0, 4);
    s1Layout->setSpacing(6);

    auto *chipRow1 = new QHBoxLayout();
    auto *chipRow2 = new QHBoxLayout();
    struct ChipDef {
        const char *label;
        MusicalBandPreset preset;
    };
    static constexpr ChipDef kChips[] = {
        {"Sub-Bass 20-80", MusicalBandPreset::SubBass20To80Hz},
        {"Bass 80-250", MusicalBandPreset::Bass80To250Hz},
        {"Low-Mids", MusicalBandPreset::LowMids250To500Hz},
        {"Mids 500-2k", MusicalBandPreset::Mids500To2000Hz},
        {"Upper-Mids", MusicalBandPreset::UpperMids2To3kHz},
        {"Brilliance", MusicalBandPreset::Brilliance3To16kHz},
        {"Beat Pulse", MusicalBandPreset::BeatPulseOnset},
    };
    for (size_t i = 0; i < std::size(kChips); ++i) {
        auto *chipBtn = new QPushButton(QLatin1String(kChips[i].label), step1);
        chipBtn->setMinimumSize(kMinControlSizePx, kMinControlSizePx);
        const auto preset = kChips[i].preset;
        connect(chipBtn, &QPushButton::clicked, this,
                [this, preset]() { applyMusicalBandPreset(preset); });
        (i < 4 ? chipRow1 : chipRow2)->addWidget(chipBtn);
    }
    s1Layout->addLayout(chipRow1);
    s1Layout->addLayout(chipRow2);

    auto *s1Form = new QFormLayout();
    m_bandPresetCombo = new QComboBox(step1);
    m_bandPresetCombo->setMinimumHeight(kMinControlSizePx);
    m_bandPresetCombo->addItem(tr("Sub-Bass (20-80 Hz)"),
                               static_cast<int>(MusicalBandPreset::SubBass20To80Hz));
    m_bandPresetCombo->addItem(tr("Bass (80-250 Hz)"),
                               static_cast<int>(MusicalBandPreset::Bass80To250Hz));
    m_bandPresetCombo->addItem(tr("Low-Mids (250-500 Hz)"),
                               static_cast<int>(MusicalBandPreset::LowMids250To500Hz));
    m_bandPresetCombo->addItem(tr("Mids (500-2,000 Hz)"),
                               static_cast<int>(MusicalBandPreset::Mids500To2000Hz));
    m_bandPresetCombo->addItem(tr("Upper-Mids (2-3 kHz)"),
                               static_cast<int>(MusicalBandPreset::UpperMids2To3kHz));
    m_bandPresetCombo->addItem(tr("Brilliance (3-16 kHz)"),
                               static_cast<int>(MusicalBandPreset::Brilliance3To16kHz));
    m_bandPresetCombo->addItem(tr("Beat Pulse (Onset)"),
                               static_cast<int>(MusicalBandPreset::BeatPulseOnset));

    ex.srcKindCombo = new QComboBox(step1);
    ex.srcKindCombo->setMinimumHeight(kMinControlSizePx);
    ex.srcKindCombo->addItem(tr("Bark Range [min..max]"),
                             static_cast<int>(WE_AUDIO_SRC_BARK_RANGE));
    ex.srcKindCombo->addItem(tr("Global RMS Envelope"), static_cast<int>(WE_AUDIO_SRC_ENVELOPE));
    ex.srcKindCombo->addItem(tr("Sub-Bass Energy"), static_cast<int>(WE_AUDIO_SRC_BASS_ENERGY));
    ex.srcKindCombo->addItem(tr("Beat Onset Pulse"), static_cast<int>(WE_AUDIO_SRC_BEAT_PULSE));

    m_aggModeCombo = new QComboBox(step1);
    m_aggModeCombo->setMinimumHeight(kMinControlSizePx);
    m_aggModeCombo->addItem(tr("Peak Weighted (0.7*Peak + 0.3*Mean)"),
                            static_cast<int>(WE_AUDIO_AGG_PEAK_WEIGHTED));
    m_aggModeCombo->addItem(tr("Mean"), static_cast<int>(WE_AUDIO_AGG_MEAN));
    m_aggModeCombo->addItem(tr("Peak"), static_cast<int>(WE_AUDIO_AGG_PEAK));

    ex.bandStartSpin = makeIntSpin(0, 63, step1);
    ex.bandEndSpin = makeIntSpin(0, 63, step1);
    auto *bandRow = new QHBoxLayout();
    bandRow->addWidget(ex.bandStartSpin);
    bandRow->addWidget(new QLabel(tr("to"), step1));
    bandRow->addWidget(ex.bandEndSpin);

    s1Form->addRow(tr("Preset:"), m_bandPresetCombo);
    s1Form->addRow(tr("Source:"), ex.srcKindCombo);
    s1Form->addRow(tr("Aggregate:"), m_aggModeCombo);
    s1Form->addRow(tr("Bands (0..63):"), bandRow);
    s1Layout->addLayout(s1Form);

    ex.barkPicker = new BarkRangePickerWidget(
        [this](uint16_t startB, uint16_t endB) {
            auto &st = *m_extra;
            if (st.updatingUi) {
                return;
            }
            const QScopedValueRollback<bool> guard(st.updatingUi, true);
            m_draft.src_kind = WE_AUDIO_SRC_BARK_RANGE;
            m_draft.band_start = startB;
            m_draft.band_end = endB;
            st.srcKindCombo->setCurrentIndex(0);
            st.bandStartSpin->setValue(startB);
            st.bandEndSpin->setValue(endB);
            if (st.curveAutoSetByOnset && m_draft.curve_shape == WE_AUDIO_CURVE_TRIG_EDGE) {
                m_draft.curve_shape = WE_AUDIO_CURVE_LINEAR;
                st.curveAutoSetByOnset = false;
                if (const int cIdx =
                        m_curveShapeCombo->findData(static_cast<int>(m_draft.curve_shape));
                    cIdx >= 0) {
                    m_curveShapeCombo->setCurrentIndex(cIdx);
                }
            }
            m_bandRangeHzLabel->setText(formatBandHzSummary(startB, endB, m_sampleRateHz));
        },
        step1);
    ex.barkPicker->setSampleRateHz(m_sampleRateHz);
    s1Layout->addWidget(ex.barkPicker);

    m_bandRangeHzLabel = new QLabel(
        formatBandHzSummary(m_draft.band_start, m_draft.band_end, m_sampleRateHz), step1);
    m_bandRangeHzLabel->setStyleSheet(
        QStringLiteral("color: #38bdf8; font-family: monospace; font-size: 12px;"));
    s1Layout->addWidget(m_bandRangeHzLabel);
    s1Layout->addStretch(1);
    m_stepStack->addWidget(step1);

    /* ---- Step 2: Target Parameter ---- */
    auto *step2 = new QWidget(m_stepStack);
    auto *s2Form = new QFormLayout(step2);
    s2Form->setSpacing(6);

    m_targetLayerCombo = new QComboBox(step2);
    m_targetLayerCombo->setMinimumHeight(kMinControlSizePx);
    m_targetCategoryCombo = new QComboBox(step2);
    m_targetCategoryCombo->setMinimumHeight(kMinControlSizePx);
    m_targetCategoryCombo->addItem(tr("Transform & Color"), 0);
    m_targetCategoryCombo->addItem(tr("Shader Uniform"), 1);
    m_targetCategoryCombo->addItem(tr("Scene Light"), 2);
    m_targetCategoryCombo->addItem(tr("Particle System"), 3);

    m_targetParamCombo = new QComboBox(step2);
    m_targetParamCombo->setMinimumHeight(kMinControlSizePx);
    ex.uniformNameEdit = new QLineEdit(step2);
    ex.uniformNameEdit->setMinimumHeight(kMinControlSizePx);
    ex.uniformNameEdit->setPlaceholderText(QStringLiteral("u_glowIntensity"));
    ex.componentSpin = makeIntSpin(0, 3, step2);
    ex.subIndexSpin = makeIntSpin(0, 0, step2);
    ex.baseValueSpin = makeSpin(-10000.0, 10000.0, 0.05, 3, step2);
    m_outMinSpin = makeSpin(-10000.0, 10000.0, 0.05, 3, step2);
    m_outMaxSpin = makeSpin(-10000.0, 10000.0, 0.05, 3, step2);
    m_outMaxSpin->setValue(1.0);

    m_blendModeCombo = new QComboBox(step2);
    m_blendModeCombo->setMinimumHeight(kMinControlSizePx);
    m_blendModeCombo->addItem(tr("Add onto Baseline"), static_cast<int>(WE_AUDIO_BLEND_ADD));
    m_blendModeCombo->addItem(tr("Multiply Baseline"), static_cast<int>(WE_AUDIO_BLEND_MULTIPLY));
    m_blendModeCombo->addItem(tr("Replace"), static_cast<int>(WE_AUDIO_BLEND_REPLACE));

    s2Form->addRow(tr("Layer:"), m_targetLayerCombo);
    s2Form->addRow(tr("Category:"), m_targetCategoryCombo);
    s2Form->addRow(tr("Parameter:"), m_targetParamCombo);
    s2Form->addRow(tr("Uniform Name:"), ex.uniformNameEdit);
    s2Form->addRow(tr("Component (0..3):"), ex.componentSpin);
    s2Form->addRow(tr("Sub-Index:"), ex.subIndexSpin);
    s2Form->addRow(tr("Baseline:"), ex.baseValueSpin);
    s2Form->addRow(tr("Output Min:"), m_outMinSpin);
    s2Form->addRow(tr("Output Max:"), m_outMaxSpin);
    s2Form->addRow(tr("Blend Mode:"), m_blendModeCombo);
    m_stepStack->addWidget(step2);

    /* ---- Step 3: Envelope Shaping & Live Oscilloscope ---- */
    auto *step3 = new QWidget(m_stepStack);
    auto *s3Layout = new QVBoxLayout(step3);
    s3Layout->setContentsMargins(0, 4, 0, 4);
    s3Layout->setSpacing(6);

    auto *s3Form = new QFormLayout();
    m_gainSpin = makeSpin(0.1, 10.0, 0.1, 2, step3);
    m_gainSpin->setValue(1.5);
    m_gateSpin = makeSpin(0.0, 0.98, 0.02, 2, step3);
    m_gateSpin->setValue(0.15);
    ex.hysteresisSpin = makeSpin(0.0, 0.25, 0.01, 2, step3);
    ex.hysteresisSpin->setValue(0.04);
    m_attackMsSpin = makeSpin(1.0, 1000.0, 5.0, 1, step3);
    m_attackMsSpin->setValue(15.0);
    m_attackMsSpin->setSuffix(QStringLiteral(" ms"));
    m_releaseMsSpin = makeSpin(5.0, 2000.0, 10.0, 1, step3);
    m_releaseMsSpin->setValue(180.0);
    m_releaseMsSpin->setSuffix(QStringLiteral(" ms"));

    m_curveShapeCombo = new QComboBox(step3);
    m_curveShapeCombo->setMinimumHeight(kMinControlSizePx);
    m_curveShapeCombo->addItem(tr("Linear"), static_cast<int>(WE_AUDIO_CURVE_LINEAR));
    m_curveShapeCombo->addItem(tr("Quadratic (Punch x^2)"),
                               static_cast<int>(WE_AUDIO_CURVE_QUADRATIC));
    m_curveShapeCombo->addItem(tr("Square Root (Soft Lift)"),
                               static_cast<int>(WE_AUDIO_CURVE_SQRT));
    m_curveShapeCombo->addItem(tr("Smoothstep"), static_cast<int>(WE_AUDIO_CURVE_SMOOTHSTEP));
    m_curveShapeCombo->addItem(tr("Trigger Edge (One-Shot)"),
                               static_cast<int>(WE_AUDIO_CURVE_TRIG_EDGE));

    s3Form->addRow(tr("Input Gain:"), m_gainSpin);
    s3Form->addRow(tr("Gate Threshold:"), m_gateSpin);
    s3Form->addRow(tr("Hysteresis:"), ex.hysteresisSpin);
    s3Form->addRow(tr("Attack:"), m_attackMsSpin);
    s3Form->addRow(tr("Release:"), m_releaseMsSpin);
    s3Form->addRow(tr("Curve Shape:"), m_curveShapeCombo);
    s3Layout->addLayout(s3Form);

    m_oscilloscope = new AudioEnvelopeOscilloscopeWidget(step3);
    m_oscilloscope->setThresholdGate(m_draft.threshold_open, m_draft.hysteresis);
    s3Layout->addWidget(m_oscilloscope);
    m_stepStack->addWidget(step3);

    outerLayout->addWidget(m_stepStack, 1);

    /* Wizard Footer Actions */
    auto *navFooter = new QHBoxLayout();
    ex.prevStepBtn = new QPushButton(tr("Back"), this);
    ex.prevStepBtn->setMinimumSize(kMinControlSizePx, kMinControlSizePx);
    ex.nextStepBtn = new QPushButton(tr("Next"), this);
    ex.nextStepBtn->setMinimumSize(kMinControlSizePx, kMinControlSizePx);
    m_createBtn = new QPushButton(tr("Create Binding"), this);
    m_createBtn->setObjectName(QStringLiteral("createBindingBtn"));
    m_createBtn->setMinimumSize(kMinControlSizePx, kMinControlSizePx);
    navFooter->addWidget(ex.prevStepBtn);
    navFooter->addWidget(ex.nextStepBtn);
    navFooter->addStretch(1);
    navFooter->addWidget(m_createBtn);
    outerLayout->addLayout(navFooter);

    /* Active Bindings List */
    auto *activeGroup = new QGroupBox(tr("Active Audio Bindings"), this);
    auto *activeV = new QVBoxLayout(activeGroup);
    m_activeBindingsList = new QListWidget(activeGroup);
    m_activeBindingsList->setMaximumHeight(96);
    activeV->addWidget(m_activeBindingsList);
    auto *activeBtns = new QHBoxLayout();
    auto *removeBtn = new QPushButton(tr("Remove Selected"), activeGroup);
    removeBtn->setMinimumSize(kMinControlSizePx, kMinControlSizePx);
    auto *clearBtn = new QPushButton(tr("Clear All"), activeGroup);
    clearBtn->setMinimumSize(kMinControlSizePx, kMinControlSizePx);
    activeBtns->addWidget(removeBtn);
    activeBtns->addWidget(clearBtn);
    activeBtns->addStretch(1);
    activeV->addLayout(activeBtns);
    outerLayout->addWidget(activeGroup);

    for (int i = 0; i < 3; ++i) {
        connect(ex.stepTabBtns[static_cast<size_t>(i)], &QPushButton::clicked, this,
                [this, i]() { syncStepUi(i); });
    }
    connect(ex.prevStepBtn, &QPushButton::clicked, this,
            [this]() { syncStepUi(m_stepStack->currentIndex() - 1); });
    connect(ex.nextStepBtn, &QPushButton::clicked, this,
            [this]() { syncStepUi(m_stepStack->currentIndex() + 1); });

    auto populateCategoryParams = [this](int catIdx) {
        QSignalBlocker block(m_targetParamCombo);
        m_targetParamCombo->clear();
        if (catIdx == 0) {
            m_targetParamCombo->addItem(tr("Origin X"),
                                        static_cast<int>(WE_AUDIO_TGT_LAYER_ORIGIN_X));
            m_targetParamCombo->addItem(tr("Origin Y"),
                                        static_cast<int>(WE_AUDIO_TGT_LAYER_ORIGIN_Y));
            m_targetParamCombo->addItem(tr("Scale XY"),
                                        static_cast<int>(WE_AUDIO_TGT_LAYER_SCALE_XY));
            m_targetParamCombo->addItem(tr("Rotation Z"),
                                        static_cast<int>(WE_AUDIO_TGT_LAYER_ROTATION_Z));
            m_targetParamCombo->addItem(tr("Alpha"), static_cast<int>(WE_AUDIO_TGT_LAYER_ALPHA));
            m_targetParamCombo->addItem(tr("Brightness"),
                                        static_cast<int>(WE_AUDIO_TGT_LAYER_BRIGHTNESS));
            m_targetParamCombo->addItem(tr("Color RGB"),
                                        static_cast<int>(WE_AUDIO_TGT_LAYER_COLOR_RGB));
        } else if (catIdx == 1) {
            m_targetParamCombo->addItem(tr("Pass Uniform Component"),
                                        static_cast<int>(WE_AUDIO_TGT_PASS_UNIFORM_COMP));
        } else if (catIdx == 2) {
            m_targetParamCombo->addItem(tr("Light Intensity"),
                                        static_cast<int>(WE_AUDIO_TGT_LIGHT_INTENSITY));
            m_targetParamCombo->addItem(tr("Light Radius"),
                                        static_cast<int>(WE_AUDIO_TGT_LIGHT_RADIUS));
        } else {
            m_targetParamCombo->addItem(tr("Emission Rate"),
                                        static_cast<int>(WE_AUDIO_TGT_PARTICLE_RATE));
            m_targetParamCombo->addItem(tr("Particle Speed"),
                                        static_cast<int>(WE_AUDIO_TGT_PARTICLE_SPEED));
            m_targetParamCombo->addItem(tr("Turbulence Speed"),
                                        static_cast<int>(WE_AUDIO_TGT_PARTICLE_TURB_SPEED));
            m_targetParamCombo->addItem(tr("Vortex Speed"),
                                        static_cast<int>(WE_AUDIO_TGT_PARTICLE_VORTEX_SPD));
            m_targetParamCombo->addItem(tr("Burst Trigger"),
                                        static_cast<int>(WE_AUDIO_TGT_PARTICLE_BURST_TRIG));
        }
    };
    populateCategoryParams(0);
    connect(m_targetCategoryCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, populateCategoryParams](int idx) {
                populateCategoryParams(idx);
                updateTargetParamUiState();
                if (!m_extra->updatingUi) {
                    m_draft = buildDraftFromUi();
                }
            });

    auto onStep2ControlChanged = [this]() {
        updateTargetParamUiState();
        if (!m_extra->updatingUi) {
            m_draft = buildDraftFromUi();
        }
    };
    connect(m_targetParamCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            onStep2ControlChanged);
    connect(m_targetLayerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            onStep2ControlChanged);
    connect(m_blendModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            onStep2ControlChanged);
    connect(ex.componentSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            onStep2ControlChanged);
    connect(ex.subIndexSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            onStep2ControlChanged);
    connect(ex.baseValueSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            onStep2ControlChanged);
    connect(m_outMinSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            onStep2ControlChanged);
    connect(m_outMaxSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            onStep2ControlChanged);
    connect(ex.uniformNameEdit, &QLineEdit::editingFinished, this, onStep2ControlChanged);

    connect(m_bandPresetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
                if (m_extra->updatingUi) {
                    return;
                }
                const QVariant data = m_bandPresetCombo->currentData();
                if (data.isValid()) {
                    applyMusicalBandPreset(static_cast<MusicalBandPreset>(data.toInt()));
                }
            });

    auto onBandSpinChanged = [this]() {
        auto &st = *m_extra;
        if (st.updatingUi) {
            return;
        }
        const QScopedValueRollback<bool> guard(st.updatingUi, true);
        uint16_t b0 = static_cast<uint16_t>(std::clamp(st.bandStartSpin->value(), 0, 63));
        uint16_t b1 = static_cast<uint16_t>(std::clamp(st.bandEndSpin->value(), 0, 63));
        if (b1 < b0) {
            b1 = b0;
            st.bandEndSpin->setValue(b1);
        }
        m_draft.band_start = b0;
        m_draft.band_end = b1;
        if (st.barkPicker) {
            st.barkPicker->setBandRange(b0, b1);
        }
        m_bandRangeHzLabel->setText(formatBandHzSummary(b0, b1, m_sampleRateHz));
    };
    connect(ex.bandStartSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, onBandSpinChanged);
    connect(ex.bandEndSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, onBandSpinChanged);
    connect(ex.srcKindCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
                auto &st = *m_extra;
                if (st.updatingUi) {
                    return;
                }
                const QVariant data = st.srcKindCombo->currentData();
                if (!data.isValid()) {
                    return;
                }
                m_draft.src_kind = data.toUInt();
                if (m_draft.src_kind != WE_AUDIO_SRC_BEAT_PULSE && st.curveAutoSetByOnset &&
                    m_draft.curve_shape == WE_AUDIO_CURVE_TRIG_EDGE) {
                    const QScopedValueRollback<bool> guard(st.updatingUi, true);
                    m_draft.curve_shape = WE_AUDIO_CURVE_LINEAR;
                    st.curveAutoSetByOnset = false;
                    if (const int cIdx =
                            m_curveShapeCombo->findData(static_cast<int>(m_draft.curve_shape));
                        cIdx >= 0) {
                        m_curveShapeCombo->setCurrentIndex(cIdx);
                    }
                }
            });
    connect(m_aggModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (!m_extra->updatingUi && m_aggModeCombo->currentData().isValid()) {
            m_draft.agg_mode = m_aggModeCombo->currentData().toUInt();
        }
    });

    auto onEnvelopeControlChanged = [this]() {
        auto &st = *m_extra;
        if (st.updatingUi) {
            return;
        }
        m_draft.input_gain = static_cast<float>(m_gainSpin->value());
        m_draft.threshold_open = static_cast<float>(m_gateSpin->value());
        m_draft.hysteresis = static_cast<float>(st.hysteresisSpin->value());
        m_draft.attack_sec = static_cast<float>(m_attackMsSpin->value()) * 0.001f;
        m_draft.release_sec = static_cast<float>(m_releaseMsSpin->value()) * 0.001f;
        if (m_curveShapeCombo->currentData().isValid()) {
            m_draft.curve_shape = m_curveShapeCombo->currentData().toUInt();
            st.curveAutoSetByOnset = false;
        }
        if (m_oscilloscope) {
            m_oscilloscope->setThresholdGate(m_draft.threshold_open, m_draft.hysteresis);
        }
    };
    connect(m_gainSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            onEnvelopeControlChanged);
    connect(m_gateSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            onEnvelopeControlChanged);
    connect(ex.hysteresisSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            onEnvelopeControlChanged);
    connect(m_attackMsSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            onEnvelopeControlChanged);
    connect(m_releaseMsSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            onEnvelopeControlChanged);
    connect(m_curveShapeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            onEnvelopeControlChanged);

    connect(m_oscilloscope, &AudioEnvelopeOscilloscopeWidget::thresholdGateDragged, this,
            [this](float newThresh) {
                const float cleanThresh =
                    sanitizeFinite(newThresh, 0.0f, 0.98f, m_draft.threshold_open);
                QSignalBlocker block(m_gateSpin);
                m_gateSpin->setValue(static_cast<double>(cleanThresh));
                m_draft.threshold_open = cleanThresh;
            });

    connect(m_createBtn, &QPushButton::clicked, this, &AudioBindingWizard::commitDraftBinding);

    connect(removeBtn, &QPushButton::clicked, this, [this]() {
        const int row = m_activeBindingsList->currentRow();
        if (row < 0 || row >= static_cast<int>(m_bindings.size())) {
            return;
        }
        const size_t removedRow = static_cast<size_t>(row);
        const we_audio_binding_t removed = m_bindings[removedRow];
        QPointer<AudioBindingWizard> self(this);
        auto applyFn = [self, removedRow](const std::optional<we_audio_binding_t> &binding,
                                          uint32_t removeId) {
            if (!self) {
                return;
            }
            std::optional<size_t> restoredRow;
            if (binding.has_value()) {
                auto existing = std::find_if(self->m_bindings.begin(), self->m_bindings.end(),
                                             [&binding](const we_audio_binding_t &x) {
                                                 return x.binding_id == binding->binding_id;
                                             });
                if (existing != self->m_bindings.end()) {
                    *existing = *binding;
                    restoredRow = static_cast<size_t>(existing - self->m_bindings.begin());
                    self->syncIpcBindings(WE_AUDIO_BIND_OP_UPSERT, &(*binding), 0u);
                } else if (self->m_bindings.size() < WE_MAX_AUDIO_BINDINGS) {
                    const size_t insertPos = std::min(removedRow, self->m_bindings.size());
                    self->m_bindings.insert(
                        self->m_bindings.begin() + static_cast<ptrdiff_t>(insertPos), *binding);
                    restoredRow = insertPos;
                    if (insertPos + 1u < self->m_bindings.size()) {
                        self->syncIpcBindings(WE_AUDIO_BIND_OP_REPLACE_ALL, nullptr, 0u);
                    } else {
                        self->syncIpcBindings(WE_AUDIO_BIND_OP_UPSERT, &(*binding), 0u);
                    }
                }
            } else {
                self->m_bindings.erase(std::remove_if(self->m_bindings.begin(),
                                                      self->m_bindings.end(),
                                                      [removeId](const we_audio_binding_t &x) {
                                                          return x.binding_id == removeId;
                                                      }),
                                       self->m_bindings.end());
                self->syncIpcBindings(WE_AUDIO_BIND_OP_REMOVE, nullptr, removeId);
            }
            self->setActiveBindings(self->m_bindings);
            if (restoredRow.has_value() && self->m_activeBindingsList &&
                *restoredRow < static_cast<size_t>(self->m_activeBindingsList->count())) {
                self->m_activeBindingsList->setCurrentRow(static_cast<int>(*restoredRow));
            }
            emit self->bindingsUpdated(self->m_bindings);
        };
        if (m_commandBus) {
            m_commandBus->execute(std::make_unique<SetAudioBindingCommand>(
                std::make_optional(removed), std::nullopt, std::move(applyFn)));
        } else {
            applyFn(std::nullopt, removed.binding_id);
        }
    });

    connect(clearBtn, &QPushButton::clicked, this, [this]() {
        if (m_bindings.empty()) {
            return;
        }
        const std::vector<we_audio_binding_t> prevBindings = m_bindings;
        QPointer<AudioBindingWizard> self(this);
        auto applyClearFn = [self](const std::vector<we_audio_binding_t> &nextBindings) {
            if (!self) {
                return;
            }
            self->setActiveBindings(nextBindings);
            if (self->m_bindings.empty()) {
                self->syncIpcBindings(WE_AUDIO_BIND_OP_CLEAR, nullptr, 0u);
            } else {
                self->syncIpcBindings(WE_AUDIO_BIND_OP_REPLACE_ALL, nullptr, 0u);
            }
            emit self->bindingsUpdated(self->m_bindings);
        };
        if (m_commandBus) {
            m_commandBus->execute(
                std::make_unique<ClearAudioBindingsCommand>(prevBindings, std::move(applyClearFn)));
        } else {
            applyClearFn({});
        }
        emit statusMessage(tr("Cleared all audio bindings"));
    });

    setSceneLayers(m_layers);
    setActiveBindings(m_bindings);

    {
        const QScopedValueRollback<bool> guard(ex.updatingUi, true);
        if (const int sIdx = ex.srcKindCombo->findData(static_cast<int>(m_draft.src_kind));
            sIdx >= 0) {
            ex.srcKindCombo->setCurrentIndex(sIdx);
        }
        if (const int aIdx = m_aggModeCombo->findData(static_cast<int>(m_draft.agg_mode));
            aIdx >= 0) {
            m_aggModeCombo->setCurrentIndex(aIdx);
        }
        ex.bandStartSpin->setValue(m_draft.band_start);
        ex.bandEndSpin->setValue(m_draft.band_end);
        if (ex.barkPicker) {
            ex.barkPicker->setBandRange(m_draft.band_start, m_draft.band_end);
        }
        m_bandRangeHzLabel->setText(
            formatBandHzSummary(m_draft.band_start, m_draft.band_end, m_sampleRateHz));
        for (int p = 0; p < m_bandPresetCombo->count(); ++p) {
            const auto preset =
                static_cast<MusicalBandPreset>(m_bandPresetCombo->itemData(p).toInt());
            const auto span = barkBandSpanForPreset(preset);
            if (span.first == m_draft.band_start && span.second == m_draft.band_end &&
                ((preset == MusicalBandPreset::BeatPulseOnset) ==
                 (m_draft.src_kind == WE_AUDIO_SRC_BEAT_PULSE))) {
                m_bandPresetCombo->setCurrentIndex(p);
                break;
            }
        }
        m_gainSpin->setValue(static_cast<double>(m_draft.input_gain));
        m_gateSpin->setValue(static_cast<double>(m_draft.threshold_open));
        ex.hysteresisSpin->setValue(static_cast<double>(m_draft.hysteresis));
        m_attackMsSpin->setValue(static_cast<double>(m_draft.attack_sec) * 1000.0);
        m_releaseMsSpin->setValue(static_cast<double>(m_draft.release_sec) * 1000.0);
        if (const int cIdx = m_curveShapeCombo->findData(static_cast<int>(m_draft.curve_shape));
            cIdx >= 0) {
            m_curveShapeCombo->setCurrentIndex(cIdx);
        }
        if (const int bIdx = m_blendModeCombo->findData(static_cast<int>(m_draft.blend_mode));
            bIdx >= 0) {
            m_blendModeCombo->setCurrentIndex(bIdx);
        }
        if (m_oscilloscope) {
            m_oscilloscope->setThresholdGate(m_draft.threshold_open, m_draft.hysteresis);
        }
        ex.baseValueSpin->setValue(static_cast<double>(m_draft.base_value));
        m_outMinSpin->setValue(static_cast<double>(m_draft.out_min));
        m_outMaxSpin->setValue(static_cast<double>(m_draft.out_max));
    }

    if (ex.hasPreselectedTarget || (m_draft.target_kind == WE_AUDIO_TGT_PASS_UNIFORM_COMP &&
                                    m_draft.uniform_name[0] != '\0')) {
        preselectTargetUniform(m_draft.target_object_id, QString::fromUtf8(m_draft.uniform_name),
                               m_draft.target_component, m_draft.base_value);
    } else {
        updateTargetParamUiState();
        syncStepUi(0);
    }
}

void AudioBindingWizard::syncIpcBindings(we_audio_bind_op_t op,
                                         const we_audio_binding_t *singleOrNull, uint32_t removeId)
{
    if (op == WE_AUDIO_BIND_OP_REPLACE_ALL) {
        dispatchAudioBindingsIpc(m_ipcClient, m_targetOutput, op, m_bindings.data(),
                                 static_cast<uint32_t>(m_bindings.size()), 0u);
    } else if (op == WE_AUDIO_BIND_OP_UPSERT && singleOrNull) {
        dispatchAudioBindingsIpc(m_ipcClient, m_targetOutput, op, singleOrNull, 1u, 0u);
    } else {
        dispatchAudioBindingsIpc(m_ipcClient, m_targetOutput, op, nullptr, 0u, removeId);
    }
}

we_audio_binding_t AudioBindingWizard::buildDraftFromUi() const
{
    we_audio_binding_t b = m_draft;
    if (!m_uiInitialized) {
        return b;
    }

    const auto &ex = *m_extra;
    if (ex.srcKindCombo->currentData().isValid()) {
        b.src_kind = ex.srcKindCombo->currentData().toUInt();
    }
    if (m_aggModeCombo->currentData().isValid()) {
        b.agg_mode = m_aggModeCombo->currentData().toUInt();
    }
    b.band_start = static_cast<uint16_t>(std::clamp(ex.bandStartSpin->value(), 0, 63));
    b.band_end =
        std::max(b.band_start, static_cast<uint16_t>(std::clamp(ex.bandEndSpin->value(), 0, 63)));

    if (m_targetLayerCombo->currentData().isValid()) {
        b.target_object_id = m_targetLayerCombo->currentData().toInt();
    }
    if (m_targetParamCombo->currentData().isValid()) {
        b.target_kind = m_targetParamCombo->currentData().toUInt();
    }
    const SubIndexBounds subBounds = subIndexBoundsForTarget(b.target_kind);
    b.target_sub_index =
        subBounds.enabled
            ? static_cast<uint32_t>(std::clamp(ex.subIndexSpin->value(), 0, subBounds.maxIndex))
            : 0u;
    if (b.target_kind == WE_AUDIO_TGT_PASS_UNIFORM_COMP) {
        b.target_component = static_cast<uint32_t>(std::clamp(ex.componentSpin->value(), 0, 3));
        const QByteArray uname = ex.uniformNameEdit->text().toUtf8();
        std::snprintf(b.uniform_name, sizeof(b.uniform_name), "%s", uname.constData());
    } else {
        b.target_component = 0u;
        b.uniform_name[0] = '\0';
    }

    b.base_value = sanitizeFinite(static_cast<float>(ex.baseValueSpin->value()), -1e6f, 1e6f, 0.0f);
    b.out_min = sanitizeFinite(static_cast<float>(m_outMinSpin->value()), -1e6f, 1e6f, 0.0f);
    b.out_max = sanitizeFinite(static_cast<float>(m_outMaxSpin->value()), -1e6f, 1e6f, 1.0f);
    if (m_blendModeCombo->currentData().isValid()) {
        b.blend_mode = m_blendModeCombo->currentData().toUInt();
    }

    b.input_gain = sanitizeFinite(static_cast<float>(m_gainSpin->value()), 0.1f, 10.0f, 1.5f);
    b.threshold_open = sanitizeFinite(static_cast<float>(m_gateSpin->value()), 0.0f, 0.98f, 0.15f);
    b.hysteresis =
        sanitizeFinite(static_cast<float>(ex.hysteresisSpin->value()), 0.0f, 0.25f, 0.04f);
    b.attack_sec =
        sanitizeFinite(static_cast<float>(m_attackMsSpin->value()) * 0.001f, 0.001f, 1.0f, 0.015f);
    b.release_sec =
        sanitizeFinite(static_cast<float>(m_releaseMsSpin->value()) * 0.001f, 0.005f, 2.0f, 0.180f);
    if (m_curveShapeCombo->currentData().isValid()) {
        b.curve_shape = m_curveShapeCombo->currentData().toUInt();
    }
    b.env_state = sanitizeFinite(b.env_state, 0.0f, 1.0f, 0.0f);
    b.last_output = sanitizeFinite(b.last_output, -1e6f, 1e6f, 0.0f);
    b.cooldown_rem_sec = sanitizeFinite(b.cooldown_rem_sec, 0.0f, 60.0f, 0.0f);
    b.enabled = 1u;
    return b;
}

void AudioBindingWizard::commitDraftBinding()
{
    if (m_bindings.size() >= WE_MAX_AUDIO_BINDINGS) {
        emit statusMessage(tr("Maximum of %1 audio bindings reached").arg(WE_MAX_AUDIO_BINDINGS));
        return;
    }

    we_audio_binding_t created = buildDraftFromUi();
    created.env_state = 0.0f;
    created.last_output = 0.0f;
    created.cooldown_rem_sec = 0.0f;
    created.gate_open = 0u;
    if (m_nextBindingId == 0u) {
        m_nextBindingId = 1u;
    }
    auto idInUse = [this](uint32_t id) {
        return std::any_of(m_bindings.begin(), m_bindings.end(),
                           [id](const we_audio_binding_t &x) { return x.binding_id == id; });
    };
    while (idInUse(m_nextBindingId)) {
        ++m_nextBindingId;
        if (m_nextBindingId == 0u) {
            m_nextBindingId = 1u;
        }
    }
    created.binding_id = m_nextBindingId++;

    QPointer<AudioBindingWizard> self(this);
    auto applyFn = [self](const std::optional<we_audio_binding_t> &binding, uint32_t removeId) {
        if (!self) {
            return;
        }
        if (binding.has_value()) {
            auto it = std::find_if(self->m_bindings.begin(), self->m_bindings.end(),
                                   [&binding](const we_audio_binding_t &x) {
                                       return x.binding_id == binding->binding_id;
                                   });
            if (it != self->m_bindings.end()) {
                *it = *binding;
            } else if (self->m_bindings.size() < WE_MAX_AUDIO_BINDINGS) {
                self->m_bindings.push_back(*binding);
            }
            self->syncIpcBindings(WE_AUDIO_BIND_OP_UPSERT, &(*binding), 0u);
        } else {
            self->m_bindings.erase(std::remove_if(self->m_bindings.begin(), self->m_bindings.end(),
                                                  [removeId](const we_audio_binding_t &x) {
                                                      return x.binding_id == removeId;
                                                  }),
                                   self->m_bindings.end());
            self->syncIpcBindings(WE_AUDIO_BIND_OP_REMOVE, nullptr, removeId);
        }
        self->setActiveBindings(self->m_bindings);
        emit self->bindingsUpdated(self->m_bindings);
    };

    if (m_commandBus) {
        m_commandBus->execute(std::make_unique<SetAudioBindingCommand>(
            std::nullopt, std::make_optional(created), std::move(applyFn)));
    } else {
        applyFn(std::make_optional(created), 0u);
    }

    emit statusMessage(tr("Created audio binding #%1").arg(created.binding_id));
}

} // namespace waywal::studio
