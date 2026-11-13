// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_AUDIO_AUDIO_BINDING_WIZARD_HPP
#define WAYWAL_STUDIO_AUDIO_AUDIO_BINDING_WIZARD_HPP

#include "studio/core/command_bus_authoring.hpp"
#include "studio/core/ipc_client.hpp"
#include "studio/footer/audio_spectrum_widget.hpp"
#include "studio/scene/scene_layer_tree_widget.hpp"
#include "waywal/ipc_proto_authoring.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace waywal::studio {

/* 60 FPS dual-trace oscilloscope plotting raw Bark energy, threshold gate, and IIR envelope. */
class AudioEnvelopeOscilloscopeWidget : public QWidget {
    Q_OBJECT

  public:
    static constexpr size_t kHistorySamples = 180u; /* 3.0 s @ 60 Hz */

    explicit AudioEnvelopeOscilloscopeWidget(QWidget *parent = nullptr);
    ~AudioEnvelopeOscilloscopeWidget() override = default;

    void setThresholdGate(float thresholdOpen, float hysteresis) noexcept;
    void pushSample(float rawEnergy, float smoothedEnvelope, bool gateOpen) noexcept;
    void clearHistory() noexcept;

  signals:
    void thresholdGateDragged(float newThresholdOpen);

  protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

  private:
    float m_thresholdOpen{0.15f};
    float m_hysteresis{0.04f};
    bool m_draggingGate{false};
    size_t m_writeIdx{0};
    std::array<float, kHistorySamples> m_rawTrace{};
    std::array<float, kHistorySamples> m_envTrace{};
    std::array<uint8_t, kHistorySamples> m_gateTrace{};
};

/* Guided 3-step No-Code Audio-Binding Wizard backed by PipeWire 64-band Bark FFT. */
class AudioBindingWizard : public QWidget {
    Q_OBJECT

  public:
    enum class MusicalBandPreset : uint8_t {
        SubBass20To80Hz = 0,    /* Bark bands 0..1 */
        Bass80To250Hz = 1,      /* Bark bands 2..6 */
        LowMids250To500Hz = 2,  /* Bark bands 7..12 */
        Mids500To2000Hz = 3,    /* Bark bands 13..33 */
        UpperMids2To3kHz = 4,   /* Bark bands 34..40 */
        Brilliance3To16kHz = 5, /* Bark bands 41..62 */
        BeatPulseOnset = 6,     /* Spectral-flux beat onset pulse */
    };

    explicit AudioBindingWizard(QWidget *parent = nullptr);
    ~AudioBindingWizard() override;

    void setCommandBus(CommandBus *bus) noexcept { m_commandBus = bus; }
    [[nodiscard]] CommandBus *commandBus() const noexcept { return m_commandBus; }

    void setIpcClient(IpcClient *client) noexcept { m_ipcClient = client; }
    [[nodiscard]] IpcClient *ipcClient() const noexcept { return m_ipcClient; }

    void setTargetOutput(const QString &output) { m_targetOutput = output; }
    [[nodiscard]] QString targetOutput() const noexcept { return m_targetOutput; }

    void setSceneLayers(const std::vector<SceneLayerNode> &layers);
    void setActiveBindings(std::vector<we_audio_binding_t> bindings);
    [[nodiscard]] const std::vector<we_audio_binding_t> &activeBindings() const noexcept
    {
        return m_bindings;
    }

    /* Pre-targets wizard to Step 1/3 when launched from a PropertyRowWidget badge. */
    void preselectTargetUniform(int32_t objectId, const QString &uniformName, uint32_t componentIdx,
                                float currentBaseline);
    void applyMusicalBandPreset(MusicalBandPreset preset);
    void updateLiveBarkTelemetry(const std::array<float, 64> &barkBands, float envelope,
                                 float bassEnergy, float beatPulse, float dtSec);

    [[nodiscard]] static std::pair<uint16_t, uint16_t>
    barkBandSpanForPreset(MusicalBandPreset preset) noexcept;

  signals:
    void bindingsUpdated(const std::vector<we_audio_binding_t> &bindings);
    void statusMessage(const QString &msg);

  protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

  private:
    void ensureUiInitialized();
    void syncIpcBindings(we_audio_bind_op_t op, const we_audio_binding_t *singleOrNull,
                         uint32_t removeId = 0);
    [[nodiscard]] we_audio_binding_t buildDraftFromUi() const;
    void commitDraftBinding();

    bool m_uiInitialized{false};
    QString m_targetOutput{QStringLiteral("*")};
    uint32_t m_nextBindingId{1};
    we_audio_binding_t m_draft{};
    std::vector<SceneLayerNode> m_layers;
    std::vector<we_audio_binding_t> m_bindings;

    CommandBus *m_commandBus{nullptr};
    IpcClient *m_ipcClient{nullptr};

    QStackedWidget *m_stepStack{nullptr};
    QComboBox *m_bandPresetCombo{nullptr};
    QComboBox *m_aggModeCombo{nullptr};
    QComboBox *m_targetLayerCombo{nullptr};
    QComboBox *m_targetCategoryCombo{nullptr};
    QComboBox *m_targetParamCombo{nullptr};
    QComboBox *m_curveShapeCombo{nullptr};
    QComboBox *m_blendModeCombo{nullptr};
    QDoubleSpinBox *m_gainSpin{nullptr};
    QDoubleSpinBox *m_gateSpin{nullptr};
    QDoubleSpinBox *m_attackMsSpin{nullptr};
    QDoubleSpinBox *m_releaseMsSpin{nullptr};
    QDoubleSpinBox *m_outMinSpin{nullptr};
    QDoubleSpinBox *m_outMaxSpin{nullptr};
    QLabel *m_bandRangeHzLabel{nullptr};
    QListWidget *m_activeBindingsList{nullptr};
    QPushButton *m_createBtn{nullptr};
    AudioEnvelopeOscilloscopeWidget *m_oscilloscope{nullptr};
    QTimer *m_idleRenderTimer{nullptr};

    struct ExtraState;
    std::unique_ptr<ExtraState> m_extra;
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_AUDIO_AUDIO_BINDING_WIZARD_HPP
