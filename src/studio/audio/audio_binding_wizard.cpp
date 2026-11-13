// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/audio/audio_binding_wizard.hpp"

#include <QCheckBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPointer>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace waywal::studio {

namespace {

constexpr QColor kBgCanvas(0x0b, 0x0c, 0x10);
constexpr QColor kBorderSubtle(0x2a, 0x2f, 0x3d);
constexpr QColor kTextSecondary(0x94, 0xa3, 0xb8);
constexpr QColor kAccentSky(0x38, 0xbd, 0xf8);
constexpr QColor kSemanticWarn(0xf5, 0x9e, 0x0b);

class BarkRangePickerWidget final : public QWidget {
  public:
    explicit BarkRangePickerWidget(std::function<void(uint16_t, uint16_t)> onRangeChanged,
                                   QWidget *parent = nullptr)
        : QWidget(parent), m_onRangeChanged(std::move(onRangeChanged))
    {
        setMinimumHeight(74);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(QStringLiteral("64-Band Bark Range Picker"));
        setAccessibleDescription(
            QStringLiteral("Drag across bands 0 to 63 to select a frequency range."));
    }

    void setBandRange(uint16_t startBand, uint16_t endBand) noexcept
    {
        m_bandStart = std::min<uint16_t>(startBand, 63u);
        m_bandEnd = std::clamp<uint16_t>(endBand, m_bandStart, 63u);
        update();
    }

    void setLiveSpectrum(const std::array<float, 64> &bands) noexcept
    {
        m_bands = bands;
        update();
    }

  protected:
    void paintEvent(QPaintEvent *event) override
    {
        (void)event;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);

        const QRectF r = rect();
        p.fillRect(r, kBgCanvas);
        p.setPen(QPen(hasFocus() ? kAccentSky : kBorderSubtle, hasFocus() ? 2.0 : 1.0));
        p.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));

        const QRectF plot = r.adjusted(4.0, 4.0, -4.0, -14.0);
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
            const float mag = std::clamp(std::isfinite(m_bands[b]) ? m_bands[b] : 0.0f, 0.0f, 1.0f);
            const qreal bx = plot.left() + static_cast<qreal>(b) * barW;
            const qreal bh = std::max<qreal>(2.0, static_cast<qreal>(mag) * plot.height());
            const bool inSel = (b >= m_bandStart && b <= m_bandEnd);
            p.fillRect(QRectF(bx + 0.5, plot.bottom() - bh, std::max(1.0, barW - 1.0), bh),
                       inSel ? kAccentSky : QColor(0x47, 0x55, 0x69));
        }

        p.setPen(QPen(kAccentSky, 1.5));
        p.drawLine(QPointF(selX0, plot.top()), QPointF(selX0, plot.bottom()));
        p.drawLine(QPointF(selX1, plot.top()), QPointF(selX1, plot.bottom()));

        p.setPen(kTextSecondary);
        QFont f = p.font();
        f.setPointSize(8);
        p.setFont(f);
        p.drawText(QRectF(plot.left(), plot.bottom() + 1.0, plot.width() * 0.5, 12.0),
                   Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("20 Hz (B0)"));
        p.drawText(
            QRectF(plot.left() + plot.width() * 0.5, plot.bottom() + 1.0, plot.width() * 0.5, 12.0),
            Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("20 kHz (B63)"));
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

  private:
    [[nodiscard]] uint16_t bandAtX(qreal x) const noexcept
    {
        const qreal w = std::max<qreal>(1.0, width() - 8.0);
        const qreal norm = std::clamp((x - 4.0) / w, 0.0, 0.9999);
        return static_cast<uint16_t>(std::clamp(static_cast<int>(norm * 64.0), 0, 63));
    }

    void applySelection(uint16_t s, uint16_t e)
    {
        m_bandStart = std::min<uint16_t>(s, 63u);
        m_bandEnd = std::clamp<uint16_t>(e, m_bandStart, 63u);
        update();
        if (m_onRangeChanged) {
            m_onRangeChanged(m_bandStart, m_bandEnd);
        }
    }

    std::function<void(uint16_t, uint16_t)> m_onRangeChanged;
    std::array<float, 64> m_bands{};
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
    return spin;
}

QString formatBandHzSummary(uint16_t startBand, uint16_t endBand)
{
    const float f0 = wwal::studio::AudioSpectrumWidget::bandCenterHz(startBand);
    const float f1 = wwal::studio::AudioSpectrumWidget::bandCenterHz(endBand);
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
        "QGroupBox { background-color: #181b22; border: 1px solid #2a2f3d; border-radius: 6px; "
        "margin-top: 8px; padding: 8px; font-weight: 600; color: #f1f5f9; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; color: #94a3b8; }"
        "QComboBox, QSpinBox, QDoubleSpinBox, QLineEdit, QListWidget { background-color: #0b0c10; "
        "border: 1px solid #2a2f3d; border-radius: 4px; padding: 3px 6px; color: #f1f5f9; }"
        "QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus, QLineEdit:focus, "
        "QListWidget:focus, "
        "QPushButton:focus { border: 2px solid #38bdf8; }"
        "QPushButton { background-color: #181b22; border: 1px solid #2a2f3d; border-radius: 4px; "
        "padding: 4px 8px; color: #f1f5f9; }"
        "QPushButton:hover { background-color: #222631; border-color: #38bdf8; }"
        "QPushButton:checked { background-color: #222631; border-color: #38bdf8; color: #38bdf8; "
        "}");
}

} // namespace

/* ========================================================================= */
/* AudioEnvelopeOscilloscopeWidget                                           */
/* ========================================================================= */

AudioEnvelopeOscilloscopeWidget::AudioEnvelopeOscilloscopeWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(110);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("Audio Envelope Oscilloscope"));
    setAccessibleDescription(
        tr("Dual-trace live oscilloscope showing raw Bark energy, Giannoulis smoothed envelope, "
           "and draggable threshold gate."));
}

void AudioEnvelopeOscilloscopeWidget::setThresholdGate(float thresholdOpen,
                                                       float hysteresis) noexcept
{
    m_thresholdOpen = std::clamp(std::isfinite(thresholdOpen) ? thresholdOpen : 0.15f, 0.0f, 0.98f);
    m_hysteresis = std::clamp(std::isfinite(hysteresis) ? hysteresis : 0.04f, 0.0f, 0.25f);
    update();
}

void AudioEnvelopeOscilloscopeWidget::pushSample(float rawEnergy, float smoothedEnvelope,
                                                 bool gateOpen) noexcept
{
    m_rawTrace[m_writeIdx] = std::clamp(std::isfinite(rawEnergy) ? rawEnergy : 0.0f, 0.0f, 1.0f);
    m_envTrace[m_writeIdx] =
        std::clamp(std::isfinite(smoothedEnvelope) ? smoothedEnvelope : 0.0f, 0.0f, 1.0f);
    m_gateTrace[m_writeIdx] = gateOpen ? 1u : 0u;
    m_writeIdx = (m_writeIdx + 1u) % kHistorySamples;
    update();
}

void AudioEnvelopeOscilloscopeWidget::clearHistory() noexcept
{
    m_rawTrace.fill(0.0f);
    m_envTrace.fill(0.0f);
    m_gateTrace.fill(0u);
    m_writeIdx = 0u;
    update();
}

void AudioEnvelopeOscilloscopeWidget::paintEvent(QPaintEvent *event)
{
    (void)event;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF fullRect = rect();
    p.fillRect(fullRect, kBgCanvas);
    p.setPen(QPen(hasFocus() ? kAccentSky : kBorderSubtle, hasFocus() ? 2.0 : 1.0));
    p.drawRoundedRect(fullRect.adjusted(0.5, 0.5, -0.5, -0.5), 4.0, 4.0);

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
    labelFont.setPointSize(8);
    labelFont.setBold(true);
    p.setFont(labelFont);
    const qreal labelY = std::max(plot.top() + 10.0, gateOpenY - 3.0);
    p.drawText(QPointF(plot.left() + 4.0, labelY),
               QString::asprintf("GATE %.2f", static_cast<double>(m_thresholdOpen)));
}

void AudioEnvelopeOscilloscopeWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }
    m_draggingGate = true;
    const QRectF plot = QRectF(rect()).adjusted(8.0, 8.0, -8.0, -8.0);
    if (plot.height() > 2.0) {
        const float val =
            std::clamp(static_cast<float>((plot.bottom() - event->position().y()) / plot.height()),
                       0.0f, 0.98f);
        m_thresholdOpen = val;
        emit thresholdGateDragged(val);
        update();
    }
}

void AudioEnvelopeOscilloscopeWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_draggingGate) {
        return;
    }
    const QRectF plot = QRectF(rect()).adjusted(8.0, 8.0, -8.0, -8.0);
    if (plot.height() > 2.0) {
        const float val =
            std::clamp(static_cast<float>((plot.bottom() - event->position().y()) / plot.height()),
                       0.0f, 0.98f);
        m_thresholdOpen = val;
        emit thresholdGateDragged(val);
        update();
    }
}

void AudioEnvelopeOscilloscopeWidget::mouseReleaseEvent(QMouseEvent *event)
{
    (void)event;
    m_draggingGate = false;
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
    bool oscNeedsRepaint{false};
    bool hasLiveTelemetry{false};
    bool hasPreselectedTarget{false};
    bool hasPresetApplied{false};
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
    if (m_idleRenderTimer && m_extra->hasLiveTelemetry && !m_idleRenderTimer->isActive()) {
        m_idleRenderTimer->start(16);
    }
}

void AudioBindingWizard::hideEvent(QHideEvent *event)
{
    if (m_idleRenderTimer && m_idleRenderTimer->isActive()) {
        m_idleRenderTimer->stop();
    }
    QWidget::hideEvent(event);
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
    m_nextBindingId = std::max(m_nextBindingId, maxId + 1u);

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
    m_draft.base_value = std::isfinite(currentBaseline) ? currentBaseline : 0.0f;
    m_draft.out_min = m_draft.base_value;
    m_draft.out_max = m_draft.base_value + 1.0f;
    const QByteArray uname = uniformName.toUtf8();
    std::snprintf(m_draft.uniform_name, sizeof(m_draft.uniform_name), "%s", uname.constData());

    if (!m_uiInitialized) {
        return;
    }

    auto &ex = *m_extra;
    ex.updatingUi = true;
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
    ex.uniformNameEdit->setText(uniformName);
    ex.componentSpin->setValue(static_cast<int>(m_draft.target_component));
    ex.baseValueSpin->setValue(static_cast<double>(m_draft.base_value));
    m_outMinSpin->setValue(static_cast<double>(m_draft.out_min));
    m_outMaxSpin->setValue(static_cast<double>(m_draft.out_max));
    if (m_stepStack) {
        m_stepStack->setCurrentIndex(0);
    }
    ex.updatingUi = false;
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
    }

    if (!m_uiInitialized) {
        return;
    }

    auto &ex = *m_extra;
    ex.updatingUi = true;
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
        m_bandRangeHzLabel->setText(formatBandHzSummary(m_draft.band_start, m_draft.band_end));
    }
    if (const int cIdx = m_curveShapeCombo->findData(static_cast<int>(m_draft.curve_shape));
        cIdx >= 0) {
        m_curveShapeCombo->setCurrentIndex(cIdx);
    }
    ex.updatingUi = false;
}

void AudioBindingWizard::updateLiveBarkTelemetry(const std::array<float, 64> &barkBands,
                                                 float envelope, float bassEnergy, float beatPulse,
                                                 float dtSec)
{
    const float clampedDt = std::clamp(std::isfinite(dtSec) ? dtSec : (1.0f / 60.0f), 1e-4f, 0.25f);
    const uint16_t b0 = std::min<uint16_t>(m_draft.band_start, 63u);
    const uint16_t b1 = std::clamp<uint16_t>(m_draft.band_end, b0, 63u);

    float rawInput = 0.0f;
    switch (static_cast<we_audio_src_kind_t>(m_draft.src_kind)) {
    case WE_AUDIO_SRC_BARK_RANGE: {
        float sum = 0.0f;
        float peak = 0.0f;
        for (uint16_t b = b0; b <= b1; ++b) {
            const float v =
                std::clamp(std::isfinite(barkBands[b]) ? barkBands[b] : 0.0f, 0.0f, 1.0f);
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
        rawInput = std::clamp(envelope, 0.0f, 1.0f);
        break;
    case WE_AUDIO_SRC_BASS_ENERGY:
        rawInput = std::clamp(bassEnergy, 0.0f, 1.0f);
        break;
    case WE_AUDIO_SRC_BEAT_PULSE:
        rawInput = std::clamp(beatPulse, 0.0f, 1.0f);
        break;
    }

    if (m_draft.gate_open == 0u && rawInput >= m_draft.threshold_open) {
        m_draft.gate_open = 1u;
    } else if (m_draft.gate_open != 0u &&
               rawInput < std::max(0.0f, m_draft.threshold_open - m_draft.hysteresis)) {
        m_draft.gate_open = 0u;
    }

    const float gated = (m_draft.gate_open != 0u) ? (rawInput * m_draft.input_gain) : 0.0f;
    const float clampedGated = std::clamp(gated, 0.0f, 1.0f);
    const float tau = (clampedGated > m_draft.env_state) ? m_draft.attack_sec : m_draft.release_sec;
    const float alpha = (tau <= 1e-4f) ? 1.0f : (1.0f - std::exp(-clampedDt / tau));
    m_draft.env_state += alpha * (clampedGated - m_draft.env_state);
    m_draft.last_output = m_draft.env_state;

    if (m_uiInitialized) {
        auto &ex = *m_extra;
        ex.hasLiveTelemetry = true;
        if (ex.barkPicker) {
            ex.barkPicker->setLiveSpectrum(barkBands);
        }
        if (m_oscilloscope) {
            m_oscilloscope->pushSample(std::clamp(rawInput * m_draft.input_gain, 0.0f, 1.0f),
                                       m_draft.env_state, m_draft.gate_open != 0u);
            ex.oscNeedsRepaint = true;
        }
        if (isVisible() && m_idleRenderTimer && !m_idleRenderTimer->isActive()) {
            m_idleRenderTimer->start(16);
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
        const auto preset = kChips[i].preset;
        connect(chipBtn, &QPushButton::clicked, this,
                [this, preset]() { applyMusicalBandPreset(preset); });
        (i < 4 ? chipRow1 : chipRow2)->addWidget(chipBtn);
    }
    s1Layout->addLayout(chipRow1);
    s1Layout->addLayout(chipRow2);

    auto *s1Form = new QFormLayout();
    m_bandPresetCombo = new QComboBox(step1);
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
    ex.srcKindCombo->addItem(tr("Bark Range [min..max]"),
                             static_cast<int>(WE_AUDIO_SRC_BARK_RANGE));
    ex.srcKindCombo->addItem(tr("Global RMS Envelope"), static_cast<int>(WE_AUDIO_SRC_ENVELOPE));
    ex.srcKindCombo->addItem(tr("Sub-Bass Energy"), static_cast<int>(WE_AUDIO_SRC_BASS_ENERGY));
    ex.srcKindCombo->addItem(tr("Beat Onset Pulse"), static_cast<int>(WE_AUDIO_SRC_BEAT_PULSE));

    m_aggModeCombo = new QComboBox(step1);
    m_aggModeCombo->addItem(tr("Peak Weighted (0.7*Peak + 0.3*Mean)"),
                            static_cast<int>(WE_AUDIO_AGG_PEAK_WEIGHTED));
    m_aggModeCombo->addItem(tr("Mean"), static_cast<int>(WE_AUDIO_AGG_MEAN));
    m_aggModeCombo->addItem(tr("Peak"), static_cast<int>(WE_AUDIO_AGG_PEAK));

    ex.bandStartSpin = new QSpinBox(step1);
    ex.bandStartSpin->setRange(0, 63);
    ex.bandEndSpin = new QSpinBox(step1);
    ex.bandEndSpin->setRange(0, 63);
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
            st.updatingUi = true;
            m_draft.src_kind = WE_AUDIO_SRC_BARK_RANGE;
            m_draft.band_start = startB;
            m_draft.band_end = endB;
            st.srcKindCombo->setCurrentIndex(0);
            st.bandStartSpin->setValue(startB);
            st.bandEndSpin->setValue(endB);
            m_bandRangeHzLabel->setText(formatBandHzSummary(startB, endB));
            st.updatingUi = false;
        },
        step1);
    s1Layout->addWidget(ex.barkPicker);

    m_bandRangeHzLabel =
        new QLabel(formatBandHzSummary(m_draft.band_start, m_draft.band_end), step1);
    m_bandRangeHzLabel->setStyleSheet(QStringLiteral("color: #38bdf8; font-family: monospace;"));
    s1Layout->addWidget(m_bandRangeHzLabel);
    s1Layout->addStretch(1);
    m_stepStack->addWidget(step1);

    /* ---- Step 2: Target Parameter ---- */
    auto *step2 = new QWidget(m_stepStack);
    auto *s2Form = new QFormLayout(step2);
    s2Form->setSpacing(6);

    m_targetLayerCombo = new QComboBox(step2);
    m_targetCategoryCombo = new QComboBox(step2);
    m_targetCategoryCombo->addItem(tr("Transform & Color"), 0);
    m_targetCategoryCombo->addItem(tr("Shader Uniform"), 1);
    m_targetCategoryCombo->addItem(tr("Scene Light"), 2);
    m_targetCategoryCombo->addItem(tr("Particle System"), 3);

    m_targetParamCombo = new QComboBox(step2);
    ex.uniformNameEdit = new QLineEdit(step2);
    ex.uniformNameEdit->setPlaceholderText(QStringLiteral("u_glowIntensity"));
    ex.componentSpin = new QSpinBox(step2);
    ex.componentSpin->setRange(0, 3);
    ex.subIndexSpin = new QSpinBox(step2);
    ex.subIndexSpin->setRange(0, 15);
    ex.baseValueSpin = makeSpin(-10000.0, 10000.0, 0.05, 3, step2);
    m_outMinSpin = makeSpin(-10000.0, 10000.0, 0.05, 3, step2);
    m_outMaxSpin = makeSpin(-10000.0, 10000.0, 0.05, 3, step2);
    m_outMaxSpin->setValue(1.0);

    m_blendModeCombo = new QComboBox(step2);
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
    ex.nextStepBtn = new QPushButton(tr("Next"), this);
    m_createBtn = new QPushButton(tr("Create Binding"), this);
    m_createBtn->setStyleSheet(
        QStringLiteral("background-color: #0284c7; border-color: #38bdf8; color: #f1f5f9; "
                       "font-weight: 600;"));
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
    auto *clearBtn = new QPushButton(tr("Clear All"), activeGroup);
    activeBtns->addWidget(removeBtn);
    activeBtns->addWidget(clearBtn);
    activeBtns->addStretch(1);
    activeV->addLayout(activeBtns);
    outerLayout->addWidget(activeGroup);

    /* Render at 60 Hz only while Bark telemetry frames are actively arriving. */
    m_idleRenderTimer = new QTimer(this);
    m_idleRenderTimer->setInterval(16);
    connect(m_idleRenderTimer, &QTimer::timeout, this, [this]() {
        if (m_extra->oscNeedsRepaint) {
            if (m_oscilloscope) {
                m_oscilloscope->update();
            }
            m_extra->oscNeedsRepaint = false;
        } else {
            m_extra->hasLiveTelemetry = false;
            m_idleRenderTimer->stop();
        }
    });

    auto syncStepUi = [this](int stepIdx) {
        auto &st = *m_extra;
        stepIdx = std::clamp(stepIdx, 0, 2);
        m_stepStack->setCurrentIndex(stepIdx);
        for (int i = 0; i < 3; ++i) {
            st.stepTabBtns[static_cast<size_t>(i)]->setChecked(i == stepIdx);
        }
        st.prevStepBtn->setEnabled(stepIdx > 0);
        st.nextStepBtn->setEnabled(stepIdx < 2);
    };

    for (int i = 0; i < 3; ++i) {
        connect(ex.stepTabBtns[static_cast<size_t>(i)], &QPushButton::clicked, this,
                [syncStepUi, i]() { syncStepUi(i); });
    }
    connect(ex.prevStepBtn, &QPushButton::clicked, this,
            [this, syncStepUi]() { syncStepUi(m_stepStack->currentIndex() - 1); });
    connect(ex.nextStepBtn, &QPushButton::clicked, this,
            [this, syncStepUi]() { syncStepUi(m_stepStack->currentIndex() + 1); });

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
                if (!m_extra->updatingUi) {
                    m_draft = buildDraftFromUi();
                }
            });

    connect(m_bandPresetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
                if (m_extra->updatingUi) {
                    return;
                }
                applyMusicalBandPreset(
                    static_cast<MusicalBandPreset>(m_bandPresetCombo->currentData().toInt()));
            });

    auto onBandSpinChanged = [this]() {
        auto &st = *m_extra;
        if (st.updatingUi) {
            return;
        }
        st.updatingUi = true;
        uint16_t b0 = static_cast<uint16_t>(st.bandStartSpin->value());
        uint16_t b1 = static_cast<uint16_t>(st.bandEndSpin->value());
        if (b1 < b0) {
            b1 = b0;
            st.bandEndSpin->setValue(b1);
        }
        m_draft.band_start = b0;
        m_draft.band_end = b1;
        if (st.barkPicker) {
            st.barkPicker->setBandRange(b0, b1);
        }
        m_bandRangeHzLabel->setText(formatBandHzSummary(b0, b1));
        st.updatingUi = false;
    };
    connect(ex.bandStartSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, onBandSpinChanged);
    connect(ex.bandEndSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, onBandSpinChanged);
    connect(ex.srcKindCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
                if (!m_extra->updatingUi) {
                    m_draft.src_kind = m_extra->srcKindCombo->currentData().toUInt();
                }
            });
    connect(m_aggModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (!m_extra->updatingUi) {
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
        m_draft.curve_shape = m_curveShapeCombo->currentData().toUInt();
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
                QSignalBlocker block(m_gateSpin);
                m_gateSpin->setValue(static_cast<double>(newThresh));
                m_draft.threshold_open = newThresh;
            });

    connect(m_createBtn, &QPushButton::clicked, this, &AudioBindingWizard::commitDraftBinding);

    connect(removeBtn, &QPushButton::clicked, this, [this]() {
        const int row = m_activeBindingsList->currentRow();
        if (row < 0 || row >= static_cast<int>(m_bindings.size())) {
            return;
        }
        const we_audio_binding_t removed = m_bindings[static_cast<size_t>(row)];
        QPointer<AudioBindingWizard> self(this);
        auto applyFn = [self](const std::optional<we_audio_binding_t> &binding, uint32_t removeId) {
            if (!self) {
                return;
            }
            if (binding.has_value()) {
                self->m_bindings.push_back(*binding);
                self->syncIpcBindings(WE_AUDIO_BIND_OP_UPSERT, &(*binding), 0u);
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

    ex.updatingUi = true;
    if (const int sIdx = ex.srcKindCombo->findData(static_cast<int>(m_draft.src_kind)); sIdx >= 0) {
        ex.srcKindCombo->setCurrentIndex(sIdx);
    }
    if (const int aIdx = m_aggModeCombo->findData(static_cast<int>(m_draft.agg_mode)); aIdx >= 0) {
        m_aggModeCombo->setCurrentIndex(aIdx);
    }
    ex.bandStartSpin->setValue(m_draft.band_start);
    ex.bandEndSpin->setValue(m_draft.band_end);
    if (ex.barkPicker) {
        ex.barkPicker->setBandRange(m_draft.band_start, m_draft.band_end);
    }
    m_bandRangeHzLabel->setText(formatBandHzSummary(m_draft.band_start, m_draft.band_end));
    for (int p = 0; p < m_bandPresetCombo->count(); ++p) {
        const auto preset = static_cast<MusicalBandPreset>(m_bandPresetCombo->itemData(p).toInt());
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
    if (m_draft.target_kind == WE_AUDIO_TGT_PASS_UNIFORM_COMP && m_draft.uniform_name[0] != '\0') {
        ex.updatingUi = false;
        preselectTargetUniform(m_draft.target_object_id, QString::fromUtf8(m_draft.uniform_name),
                               m_draft.target_component, m_draft.base_value);
    } else {
        ex.updatingUi = false;
    }
    syncStepUi(0);
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
    b.src_kind = ex.srcKindCombo->currentData().toUInt();
    b.agg_mode = m_aggModeCombo->currentData().toUInt();
    b.band_start = static_cast<uint16_t>(ex.bandStartSpin->value());
    b.band_end = std::max(b.band_start, static_cast<uint16_t>(ex.bandEndSpin->value()));

    b.target_object_id = m_targetLayerCombo->currentData().toInt();
    b.target_kind = m_targetParamCombo->currentData().toUInt();
    b.target_sub_index = static_cast<uint32_t>(ex.subIndexSpin->value());
    b.target_component = static_cast<uint32_t>(ex.componentSpin->value());
    const QByteArray uname = ex.uniformNameEdit->text().toUtf8();
    std::snprintf(b.uniform_name, sizeof(b.uniform_name), "%s", uname.constData());

    b.base_value = static_cast<float>(ex.baseValueSpin->value());
    b.out_min = static_cast<float>(m_outMinSpin->value());
    b.out_max = static_cast<float>(m_outMaxSpin->value());
    b.blend_mode = m_blendModeCombo->currentData().toUInt();

    b.input_gain = static_cast<float>(m_gainSpin->value());
    b.threshold_open = static_cast<float>(m_gateSpin->value());
    b.hysteresis = static_cast<float>(ex.hysteresisSpin->value());
    b.attack_sec = static_cast<float>(m_attackMsSpin->value()) * 0.001f;
    b.release_sec = static_cast<float>(m_releaseMsSpin->value()) * 0.001f;
    b.curve_shape = m_curveShapeCombo->currentData().toUInt();
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
