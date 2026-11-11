// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_INSPECTOR_CAMERA_LIGHTING_INSPECTOR_WIDGET_HPP
#define WAYWAL_STUDIO_INSPECTOR_CAMERA_LIGHTING_INSPECTOR_WIDGET_HPP

#include "studio/core/command_bus_phase3.hpp"
#include "studio/core/ipc_client.hpp"
#include "waywal/we_camera3d.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QListWidget>
#include <QPushButton>
#include <QString>
#include <QWidget>

#include <cstdint>

namespace waywal::studio {

class CameraLightingInspectorWidget : public QWidget {
    Q_OBJECT

  public:
    explicit CameraLightingInspectorWidget(QWidget *parent = nullptr);
    ~CameraLightingInspectorWidget() override = default;

    void setCommandBus(CommandBus *bus) noexcept { m_commandBus = bus; }
    [[nodiscard]] CommandBus *commandBus() const noexcept { return m_commandBus; }

    void setIpcClient(IpcClient *client) noexcept { m_ipcClient = client; }
    [[nodiscard]] IpcClient *ipcClient() const noexcept { return m_ipcClient; }

    void setTargetOutput(const QString &output) { m_targetOutput = output; }
    [[nodiscard]] QString targetOutput() const noexcept { return m_targetOutput; }

    void setCameraAndLights(const we_camera3d_t &cam, const we_lighting_uniforms_t &lights);
    [[nodiscard]] const we_camera3d_t &camera() const noexcept { return m_camera; }
    [[nodiscard]] const we_lighting_uniforms_t &lighting() const noexcept { return m_lighting; }

    /* Keyframe & Light Slot Management */
    bool addCameraKeyframe(float timeSec);
    bool removeCameraKeyframe(uint32_t index);
    void setSelectedLightSlot(uint32_t slotIndex);
    void resetToDefaults();

  signals:
    void cameraOrLightsChanged(const QString &output, const we_camera3d_t &cam,
                               const we_lighting_uniforms_t &lights);
    void camera3DChanged(const QString &output, const we_camera3d_t &cam);
    void lightingChanged(const QString &output, const we_lighting_uniforms_t &lights);
    void statusMessage(const QString &msg);

  private slots:
    void onCameraControlChanged();
    void onDofControlChanged();
    void onParallaxControlChanged();
    void onKeyframeSelectionChanged();
    void onAddKeyframeClicked();
    void onRemoveKeyframeClicked();
    void onClearKeyframesClicked();
    void onLightSlotChanged(int index);
    void onLightControlChanged();
    void onPickLightColor();
    void onPickAmbientColor();
    void onPickSkyColor();

  private:
    void setupUi();
    void syncUiFromModel();
    void syncLightUiFromSlot(uint32_t slot);
    void updateKeyframeList();
    void updateColorButtonSwatches();
    void commitChange(const we_camera3d_t &newCam, const we_lighting_uniforms_t &newLights);
    void sendIpcUpdate(const QString &output, const we_camera3d_t &cam,
                       const we_lighting_uniforms_t &lights);

    QString m_targetOutput;
    we_camera3d_t m_camera{};
    we_lighting_uniforms_t m_lighting{};
    uint32_t m_selectedLightSlot{0};
    bool m_updatingUi{false};

    CommandBus *m_commandBus{nullptr};
    IpcClient *m_ipcClient{nullptr};

    // Camera Controls
    QComboBox *m_projModeCombo{nullptr};
    QDoubleSpinBox *m_fovSpin{nullptr};
    QDoubleSpinBox *m_nearZSpin{nullptr};
    QDoubleSpinBox *m_farZSpin{nullptr};
    QDoubleSpinBox *m_eyeXSpin{nullptr};
    QDoubleSpinBox *m_eyeYSpin{nullptr};
    QDoubleSpinBox *m_eyeZSpin{nullptr};
    QDoubleSpinBox *m_targetXSpin{nullptr};
    QDoubleSpinBox *m_targetYSpin{nullptr};
    QDoubleSpinBox *m_targetZSpin{nullptr};
    QDoubleSpinBox *m_upXSpin{nullptr};
    QDoubleSpinBox *m_upYSpin{nullptr};
    QDoubleSpinBox *m_upZSpin{nullptr};

    // Parallax Controls
    QDoubleSpinBox *m_parallaxXSpin{nullptr};
    QDoubleSpinBox *m_parallaxYSpin{nullptr};
    QDoubleSpinBox *m_parallaxSmoothingSpin{nullptr};

    // Depth of Field Controls
    QCheckBox *m_dofEnableCheck{nullptr};
    QDoubleSpinBox *m_dofFocalDistSpin{nullptr};
    QDoubleSpinBox *m_dofFocalRegionSpin{nullptr};
    QDoubleSpinBox *m_dofBokehSpin{nullptr};

    // Keyframe Controls
    QCheckBox *m_pathEnableCheck{nullptr};
    QComboBox *m_loopModeCombo{nullptr};
    QDoubleSpinBox *m_kfTimeSpin{nullptr};
    QPushButton *m_addKfBtn{nullptr};
    QPushButton *m_removeKfBtn{nullptr};
    QPushButton *m_clearKfBtn{nullptr};
    QListWidget *m_keyframeList{nullptr};

    // Dynamic Lights Controls
    QComboBox *m_lightSlotCombo{nullptr};
    QComboBox *m_lightTypeCombo{nullptr};
    QPushButton *m_lightColorBtn{nullptr};
    QDoubleSpinBox *m_lightIntensitySpin{nullptr};
    QDoubleSpinBox *m_lightRadiusSpin{nullptr};
    QDoubleSpinBox *m_lightPosXSpin{nullptr};
    QDoubleSpinBox *m_lightPosYSpin{nullptr};
    QDoubleSpinBox *m_lightPosZSpin{nullptr};
    QDoubleSpinBox *m_spotDirXSpin{nullptr};
    QDoubleSpinBox *m_spotDirYSpin{nullptr};
    QDoubleSpinBox *m_spotDirZSpin{nullptr};
    QDoubleSpinBox *m_spotInnerSpin{nullptr};
    QDoubleSpinBox *m_spotOuterSpin{nullptr};
    QDoubleSpinBox *m_tubeLengthSpin{nullptr};
    QDoubleSpinBox *m_specularPowerSpin{nullptr};

    // Ambient / Environment Controls
    QPushButton *m_ambientColorBtn{nullptr};
    QPushButton *m_skyColorBtn{nullptr};
    QDoubleSpinBox *m_normalMapStrengthSpin{nullptr};
    QPushButton *m_resetBtn{nullptr};
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_INSPECTOR_CAMERA_LIGHTING_INSPECTOR_WIDGET_HPP
