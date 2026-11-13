// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_PARTICLE_PARTICLE_DESIGNER_WIDGET_HPP
#define WAYWAL_STUDIO_PARTICLE_PARTICLE_DESIGNER_WIDGET_HPP

#include "studio/core/command_bus_authoring.hpp"
#include "studio/core/ipc_client.hpp"
#include "waywal/ipc_proto_authoring.h"
#include "waywal/we_particle.h"

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace waywal::studio {

/* Interactive Fritsch-Carlson monotone cubic Hermite curve editor baking to 65-sample LUT. */
class ScalarCurveEditorWidget : public QWidget {
    Q_OBJECT

  public:
    explicit ScalarCurveEditorWidget(QWidget *parent = nullptr);
    ~ScalarCurveEditorWidget() override = default;

    void setValueRange(float minVal, float maxVal);
    void setKnots(const std::vector<we_particle_curve_knot_t> &knots);
    [[nodiscard]] const std::vector<we_particle_curve_knot_t> &knots() const noexcept
    {
        return m_knots;
    }

    /* Evaluates piecewise monotone cubic Hermite spline and bakes 65 exact LUT samples. */
    [[nodiscard]] float evaluateAt(float t) const noexcept;
    void bakeLut(float outLut[WE_PARTICLE_RAMP_SAMPLES]) const noexcept;

  signals:
    void curveEdited(const std::vector<we_particle_curve_knot_t> &knots);

  protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

  private:
    void rebuildTangentCache();

    float m_minVal{0.0f};
    float m_maxVal{1.0f};
    int m_selectedKnot{-1};
    bool m_draggingKnot{false};
    std::vector<we_particle_curve_knot_t> m_knots;
    std::vector<float> m_cachedAutoTangents;
};

/* Multi-stop OKLab color gradient ramp bar integrated with OklchColorDialog. */
class OklabColorRampWidget : public QWidget {
    Q_OBJECT

  public:
    explicit OklabColorRampWidget(QWidget *parent = nullptr);
    ~OklabColorRampWidget() override = default;

    void setStops(const std::vector<we_particle_color_stop_t> &stops);
    [[nodiscard]] const std::vector<we_particle_color_stop_t> &stops() const noexcept
    {
        return m_stops;
    }

    /* Interpolates adjacent stops in perceptual OKLab space into sRGB [0,1]^3 65-sample LUTs. */
    void bakeOklabRgbLut(float outR[WE_PARTICLE_RAMP_SAMPLES], float outG[WE_PARTICLE_RAMP_SAMPLES],
                         float outB[WE_PARTICLE_RAMP_SAMPLES]) const noexcept;

  signals:
    void rampEdited(const std::vector<we_particle_color_stop_t> &stops);
    void stopColorPickerRequested(int stopIndex, const QColor &initialColor);

  protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

  private:
    int m_selectedStop{-1};
    bool m_draggingStop{false};
    std::vector<we_particle_color_stop_t> m_stops;
};

class ParticleDesignerWidget : public QWidget {
    Q_OBJECT

  public:
    explicit ParticleDesignerWidget(QWidget *parent = nullptr);
    ~ParticleDesignerWidget() override;

    void setCommandBus(CommandBus *bus) noexcept { m_commandBus = bus; }
    [[nodiscard]] CommandBus *commandBus() const noexcept { return m_commandBus; }

    void setIpcClient(IpcClient *client) noexcept { m_ipcClient = client; }
    [[nodiscard]] IpcClient *ipcClient() const noexcept { return m_ipcClient; }

    void setTargetOutput(const QString &output) { m_targetOutput = output; }
    [[nodiscard]] QString targetOutput() const noexcept { return m_targetOutput; }

    void loadParticleSystem(int32_t objectId, const we_particle_sys_t *sys);
    void applyCleanRoomPreset(we_particle_preset_id_t presetId);
    void triggerBurst();
    void updateTelemetry(uint32_t aliveCount, uint32_t maxCount, uint32_t simTimeUs);
    [[nodiscard]] QString exportSystemJson() const;

    /* Canonical metadata for the 8 clean-room 1-click presets */
    [[nodiscard]] static const std::array<we_particle_preset_desc_t, WE_PARTICLE_PRESET_COUNT> &
    builtinPresetCatalog() noexcept;

  signals:
    void particleSystemModified(int32_t objectId, const QString &jsonDefinition);
    void controlPointOverlayUpdated(int32_t objectId);
    void statusMessage(const QString &msg);

  protected:
    void showEvent(QShowEvent *event) override;

  private:
    struct ExtraState;

    void ensureUiInitialized();
    void syncUiFromModel();
    bool importSystemJson(const QString &jsonState);
    void commitSystemEdit(const QString &paramGroup);
    void pushRampLutIpc(uint32_t opIndex, uint32_t channelKind);
    void pushRampLutIpcForObject(int32_t objectId, uint32_t opIndex, uint32_t channelKind);

    bool m_uiInitialized{false};
    bool m_updatingUi{false};
    bool m_simPaused{false};
    QString m_targetOutput{QStringLiteral("*")};
    int32_t m_objectId{-1};
    QString m_lastCommittedJson;
    std::unique_ptr<ExtraState> m_extra;

    /* Parsed system parameters mirrored in Studio UI */
    uint32_t m_maxCount{WE_PARTICLE_DEFAULT_CAP};
    float m_startTime{0.0f};
    bool m_worldSpace{false};
    bool m_perspective{false};
    we_renderer_type_t m_rendererType{WE_RENDERER_SPRITE};
    uint32_t m_blending{0};
    std::vector<we_particle_emitter_t> m_emitters;
    std::vector<we_particle_initializer_t> m_initializers;
    std::vector<we_particle_operator_t> m_operators;
    std::vector<we_particle_cp_t> m_controlPoints;

    CommandBus *m_commandBus{nullptr};
    IpcClient *m_ipcClient{nullptr};

    QComboBox *m_presetCombo{nullptr};
    QPushButton *m_applyPresetBtn{nullptr};
    QPushButton *m_burstBtn{nullptr};
    QPushButton *m_pauseBtn{nullptr};
    QProgressBar *m_aliveBar{nullptr};
    QLabel *m_simTimeLabel{nullptr};
    QSpinBox *m_maxCountSpin{nullptr};
    QDoubleSpinBox *m_startTimeSpin{nullptr};
    QComboBox *m_rendererCombo{nullptr};
    QComboBox *m_blendingCombo{nullptr};
    QCheckBox *m_worldSpaceCheck{nullptr};
    QListWidget *m_emitterList{nullptr};
    QListWidget *m_initializerList{nullptr};
    QListWidget *m_operatorList{nullptr};
    ScalarCurveEditorWidget *m_alphaCurveEditor{nullptr};
    ScalarCurveEditorWidget *m_sizeCurveEditor{nullptr};
    OklabColorRampWidget *m_colorRampEditor{nullptr};
    QTimer *m_telemetryThrottleTimer{nullptr};
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_PARTICLE_PARTICLE_DESIGNER_WIDGET_HPP
