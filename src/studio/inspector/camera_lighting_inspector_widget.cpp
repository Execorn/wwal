// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/inspector/camera_lighting_inspector_widget.hpp"
#include "studio/inspector/oklch_color_dialog.hpp"

#include <QColor>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace waywal::studio {

CameraLightingInspectorWidget::CameraLightingInspectorWidget(QWidget *parent) : QWidget(parent)
{
    resetToDefaults();
    setupUi();
    syncUiFromModel();
}

void CameraLightingInspectorWidget::resetToDefaults()
{
    we_camera3d_init_default(&m_camera, 1920.0f, 1080.0f);
    m_camera.parallax_amount[0] = 15.0f;
    m_camera.parallax_amount[1] = 10.0f;
    m_camera.parallax_smoothing = 0.15f;
    m_camera.dof_focal_dist = 5.0f;
    m_camera.dof_focal_region = 2.0f;
    m_camera.dof_bokeh_radius_px = 6.0f;

    // Ambient & Sky hemisphere defaults
    m_lighting.ambient_color_rgb[0] = 0.12f;
    m_lighting.ambient_color_rgb[1] = 0.14f;
    m_lighting.ambient_color_rgb[2] = 0.18f;

    m_lighting.sky_color_rgb[0] = 0.25f;
    m_lighting.sky_color_rgb[1] = 0.35f;
    m_lighting.sky_color_rgb[2] = 0.50f;

    m_lighting.normal_map_strength = 1.0f;
    m_lighting.active_light_count = 1;

    // Default Light 0: Point light
    m_lighting.lights[0].light_type = WE_LIGHT_POINT;
    m_lighting.lights[0].position_or_dir[0] = 0.0f;
    m_lighting.lights[0].position_or_dir[1] = 2.5f;
    m_lighting.lights[0].position_or_dir[2] = 3.0f;
    m_lighting.lights[0].color_rgb[0] = 1.0f;
    m_lighting.lights[0].color_rgb[1] = 0.95f;
    m_lighting.lights[0].color_rgb[2] = 0.85f;
    m_lighting.lights[0].intensity = 2.5f;
    m_lighting.lights[0].attenuation_radius = 20.0f;
    m_lighting.lights[0].specular_power = 32.0f;

    // Lights 1..7: Disabled by default
    for (uint32_t i = 1; i < WE_MAX_SCENE_LIGHTS; ++i) {
        m_lighting.lights[i].light_type = WE_LIGHT_DISABLED;
        m_lighting.lights[i].position_or_dir[0] = 0.0f;
        m_lighting.lights[i].position_or_dir[1] = 1.0f;
        m_lighting.lights[i].position_or_dir[2] = 0.0f;
        m_lighting.lights[i].color_rgb[0] = 1.0f;
        m_lighting.lights[i].color_rgb[1] = 1.0f;
        m_lighting.lights[i].color_rgb[2] = 1.0f;
        m_lighting.lights[i].intensity = 1.0f;
        m_lighting.lights[i].attenuation_radius = 10.0f;
        m_lighting.lights[i].spot_inner_cos = 0.95f;
        m_lighting.lights[i].spot_outer_cos = 0.80f;
        m_lighting.lights[i].spot_dir[0] = 0.0f;
        m_lighting.lights[i].spot_dir[1] = 0.0f;
        m_lighting.lights[i].spot_dir[2] = -1.0f;
        m_lighting.lights[i].tube_length = 2.0f;
        m_lighting.lights[i].specular_power = 32.0f;
    }

    m_selectedLightSlot = 0;
}

void CameraLightingInspectorWidget::setCameraAndLights(const we_camera3d_t &cam,
                                                       const we_lighting_uniforms_t &lights)
{
    m_camera = cam;
    m_lighting = lights;
    syncUiFromModel();
}

bool CameraLightingInspectorWidget::addCameraKeyframe(float timeSec)
{
    if (m_camera.keyframe_count >= WE_CAMERA_MAX_KEYFRAMES) {
        emit statusMessage(QStringLiteral("Maximum camera keyframes (64) reached."));
        return false;
    }

    we_camera_kf_t kf{};
    kf.time_sec = timeSec;
    kf.position[0] = m_camera.eye_pos[0];
    kf.position[1] = m_camera.eye_pos[1];
    kf.position[2] = m_camera.eye_pos[2];
    quat_identity(kf.orientation_quat);
    kf.fov_deg = m_camera.fov_y_deg;
    kf.focal_distance = m_camera.dof_focal_dist;
    kf.aperture_fstop = 2.8f;
    kf.tension = 0.5f;

    // Insert sorted by time_sec
    uint32_t insertIdx = m_camera.keyframe_count;
    for (uint32_t i = 0; i < m_camera.keyframe_count; ++i) {
        if (m_camera.keyframes[i].time_sec > timeSec) {
            insertIdx = i;
            break;
        }
    }

    for (uint32_t i = m_camera.keyframe_count; i > insertIdx; --i) {
        m_camera.keyframes[i] = m_camera.keyframes[i - 1];
    }
    m_camera.keyframes[insertIdx] = kf;
    ++m_camera.keyframe_count;

    commitChange(m_camera, m_lighting);
    emit statusMessage(QStringLiteral("Added camera keyframe at %1 s").arg(timeSec, 0, 'f', 2));
    return true;
}

bool CameraLightingInspectorWidget::removeCameraKeyframe(uint32_t index)
{
    if (index >= m_camera.keyframe_count) {
        return false;
    }

    for (uint32_t i = index; i + 1 < m_camera.keyframe_count; ++i) {
        m_camera.keyframes[i] = m_camera.keyframes[i + 1];
    }
    --m_camera.keyframe_count;

    commitChange(m_camera, m_lighting);
    emit statusMessage(QStringLiteral("Removed camera keyframe #%1").arg(index));
    return true;
}

void CameraLightingInspectorWidget::setSelectedLightSlot(uint32_t slotIndex)
{
    if (slotIndex >= WE_MAX_SCENE_LIGHTS) {
        return;
    }
    m_selectedLightSlot = slotIndex;
    if (m_lightSlotCombo && m_lightSlotCombo->currentIndex() != static_cast<int>(slotIndex)) {
        m_updatingUi = true;
        m_lightSlotCombo->setCurrentIndex(static_cast<int>(slotIndex));
        m_updatingUi = false;
    }
    syncLightUiFromSlot(slotIndex);
}

void CameraLightingInspectorWidget::commitChange(const we_camera3d_t &newCam,
                                                 const we_lighting_uniforms_t &newLights)
{
    if (m_updatingUi) {
        return;
    }

    if (m_commandBus) {
        we_camera3d_t oldCam = m_camera;
        we_lighting_uniforms_t oldLights = m_lighting;
        auto cmd = std::make_unique<SetCamera3DCommand>(
            m_targetOutput, oldCam, newCam, oldLights, newLights,
            [this](const QString &output, const we_camera3d_t &c, const we_lighting_uniforms_t &l) {
                m_camera = c;
                m_lighting = l;
                syncUiFromModel();
                sendIpcUpdate(output, c, l);
                emit cameraOrLightsChanged(output, c, l);
                emit camera3DChanged(output, c);
                emit lightingChanged(output, l);
            });
        m_commandBus->execute(std::move(cmd));
    } else {
        m_camera = newCam;
        m_lighting = newLights;
        syncUiFromModel();
        sendIpcUpdate(m_targetOutput, m_camera, m_lighting);
        emit cameraOrLightsChanged(m_targetOutput, m_camera, m_lighting);
        emit camera3DChanged(m_targetOutput, m_camera);
        emit lightingChanged(m_targetOutput, m_lighting);
    }
}

void CameraLightingInspectorWidget::sendIpcUpdate(const QString &output, const we_camera3d_t &cam,
                                                  const we_lighting_uniforms_t &lights)
{
    if (m_ipcClient) {
        m_ipcClient->sendSetCamera3D(output, cam);
        m_ipcClient->sendSetSceneLights(output, lights);
    }
}

void CameraLightingInspectorWidget::setupUi()
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(4, 4, 4, 4);
    rootLayout->setSpacing(6);

    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto *containerWidget = new QWidget(scrollArea);
    auto *contentLayout = new QVBoxLayout(containerWidget);
    contentLayout->setContentsMargins(6, 6, 6, 6);
    contentLayout->setSpacing(8);

    // =========================================================================
    // Section 1: Camera Projection & Transform
    // =========================================================================
    auto *camGroup = new QGroupBox(tr("3D Camera Frustum & Transform"), containerWidget);
    camGroup->setStyleSheet(
        "QGroupBox { font-weight: bold; border: 1px solid #313244; border-radius: 6px; "
        "margin-top: 10px; padding-top: 10px; color: #cdd6f4; } "
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }");
    auto *camLayout = new QGridLayout(camGroup);
    camLayout->setContentsMargins(8, 8, 8, 8);
    camLayout->setSpacing(6);

    m_projModeCombo = new QComboBox(camGroup);
    m_projModeCombo->addItem(tr("Orthographic 2D"), WE_PROJ_ORTHOGRAPHIC_2D);
    m_projModeCombo->addItem(tr("Perspective 3D"), WE_PROJ_PERSPECTIVE_3D);
    m_projModeCombo->addItem(tr("Depth Parallax 2.5D"), WE_PROJ_DEPTH_PARALLAX_25D);
    camLayout->addWidget(new QLabel(tr("Projection:"), camGroup), 0, 0);
    camLayout->addWidget(m_projModeCombo, 0, 1, 1, 3);

    m_fovSpin = new QDoubleSpinBox(camGroup);
    m_fovSpin->setRange(10.0, 140.0);
    m_fovSpin->setSingleStep(1.0);
    m_fovSpin->setDecimals(1);
    m_fovSpin->setSuffix(QStringLiteral(" deg"));
    camLayout->addWidget(new QLabel(tr("FOV (Y):"), camGroup), 1, 0);
    camLayout->addWidget(m_fovSpin, 1, 1);

    m_nearZSpin = new QDoubleSpinBox(camGroup);
    m_nearZSpin->setRange(0.01, 100.0);
    m_nearZSpin->setSingleStep(0.1);
    m_nearZSpin->setDecimals(3);
    camLayout->addWidget(new QLabel(tr("Near Z:"), camGroup), 1, 2);
    camLayout->addWidget(m_nearZSpin, 1, 3);

    m_farZSpin = new QDoubleSpinBox(camGroup);
    m_farZSpin->setRange(0.1, 10000.0);
    m_farZSpin->setSingleStep(10.0);
    m_farZSpin->setDecimals(1);
    camLayout->addWidget(new QLabel(tr("Far Z:"), camGroup), 2, 2);
    camLayout->addWidget(m_farZSpin, 2, 3);

    // Eye Position
    camLayout->addWidget(new QLabel(tr("Eye Pos (X/Y/Z):"), camGroup), 3, 0);
    auto *eyeLayout = new QHBoxLayout();
    eyeLayout->setSpacing(2);
    m_eyeXSpin = new QDoubleSpinBox(camGroup);
    m_eyeYSpin = new QDoubleSpinBox(camGroup);
    m_eyeZSpin = new QDoubleSpinBox(camGroup);
    for (auto *s : {m_eyeXSpin, m_eyeYSpin, m_eyeZSpin}) {
        s->setRange(-10000.0, 10000.0);
        s->setSingleStep(0.2);
        s->setDecimals(2);
        eyeLayout->addWidget(s);
    }
    camLayout->addLayout(eyeLayout, 3, 1, 1, 3);

    // Target Position
    camLayout->addWidget(new QLabel(tr("Target Pos (X/Y/Z):"), camGroup), 4, 0);
    auto *targetLayout = new QHBoxLayout();
    targetLayout->setSpacing(2);
    m_targetXSpin = new QDoubleSpinBox(camGroup);
    m_targetYSpin = new QDoubleSpinBox(camGroup);
    m_targetZSpin = new QDoubleSpinBox(camGroup);
    for (auto *s : {m_targetXSpin, m_targetYSpin, m_targetZSpin}) {
        s->setRange(-10000.0, 10000.0);
        s->setSingleStep(0.2);
        s->setDecimals(2);
        targetLayout->addWidget(s);
    }
    camLayout->addLayout(targetLayout, 4, 1, 1, 3);

    // Up Vector
    camLayout->addWidget(new QLabel(tr("Up Vector (X/Y/Z):"), camGroup), 5, 0);
    auto *upLayout = new QHBoxLayout();
    upLayout->setSpacing(2);
    m_upXSpin = new QDoubleSpinBox(camGroup);
    m_upYSpin = new QDoubleSpinBox(camGroup);
    m_upZSpin = new QDoubleSpinBox(camGroup);
    for (auto *s : {m_upXSpin, m_upYSpin, m_upZSpin}) {
        s->setRange(-1.0, 1.0);
        s->setSingleStep(0.1);
        s->setDecimals(2);
        upLayout->addWidget(s);
    }
    camLayout->addLayout(upLayout, 5, 1, 1, 3);

    contentLayout->addWidget(camGroup);

    // =========================================================================
    // Section 2: Mouse Parallax Spring Damping
    // =========================================================================
    auto *parallaxGroup = new QGroupBox(tr("Mouse Orbit & Parallax Spring"), containerWidget);
    parallaxGroup->setStyleSheet(
        "QGroupBox { font-weight: bold; border: 1px solid #313244; border-radius: 6px; "
        "margin-top: 10px; padding-top: 10px; color: #cdd6f4; } "
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }");
    auto *parallaxLayout = new QGridLayout(parallaxGroup);
    parallaxLayout->setContentsMargins(8, 8, 8, 8);
    parallaxLayout->setSpacing(6);

    m_parallaxXSpin = new QDoubleSpinBox(parallaxGroup);
    m_parallaxXSpin->setRange(0.0, 200.0);
    m_parallaxXSpin->setSingleStep(1.0);
    m_parallaxXSpin->setDecimals(1);
    parallaxLayout->addWidget(new QLabel(tr("Sway X:"), parallaxGroup), 0, 0);
    parallaxLayout->addWidget(m_parallaxXSpin, 0, 1);

    m_parallaxYSpin = new QDoubleSpinBox(parallaxGroup);
    m_parallaxYSpin->setRange(0.0, 200.0);
    m_parallaxYSpin->setSingleStep(1.0);
    m_parallaxYSpin->setDecimals(1);
    parallaxLayout->addWidget(new QLabel(tr("Sway Y:"), parallaxGroup), 0, 2);
    parallaxLayout->addWidget(m_parallaxYSpin, 0, 3);

    m_parallaxSmoothingSpin = new QDoubleSpinBox(parallaxGroup);
    m_parallaxSmoothingSpin->setRange(0.01, 1.0);
    m_parallaxSmoothingSpin->setSingleStep(0.05);
    m_parallaxSmoothingSpin->setDecimals(2);
    parallaxLayout->addWidget(new QLabel(tr("Smoothing:"), parallaxGroup), 1, 0);
    parallaxLayout->addWidget(m_parallaxSmoothingSpin, 1, 1, 1, 3);

    contentLayout->addWidget(parallaxGroup);

    // =========================================================================
    // Section 3: Circle-of-Confusion Depth of Field
    // =========================================================================
    auto *dofGroup = new QGroupBox(tr("Circle-of-Confusion Depth of Field (DOF)"), containerWidget);
    dofGroup->setStyleSheet(
        "QGroupBox { font-weight: bold; border: 1px solid #313244; border-radius: 6px; "
        "margin-top: 10px; padding-top: 10px; color: #cdd6f4; } "
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }");
    auto *dofLayout = new QGridLayout(dofGroup);
    dofLayout->setContentsMargins(8, 8, 8, 8);
    dofLayout->setSpacing(6);

    m_dofEnableCheck = new QCheckBox(tr("Enable Depth of Field"), dofGroup);
    dofLayout->addWidget(m_dofEnableCheck, 0, 0, 1, 4);

    m_dofFocalDistSpin = new QDoubleSpinBox(dofGroup);
    m_dofFocalDistSpin->setRange(0.01, 1000.0);
    m_dofFocalDistSpin->setSingleStep(0.2);
    m_dofFocalDistSpin->setDecimals(2);
    dofLayout->addWidget(new QLabel(tr("Focal Distance:"), dofGroup), 1, 0);
    dofLayout->addWidget(m_dofFocalDistSpin, 1, 1);

    m_dofFocalRegionSpin = new QDoubleSpinBox(dofGroup);
    m_dofFocalRegionSpin->setRange(0.0, 1000.0);
    m_dofFocalRegionSpin->setSingleStep(0.2);
    m_dofFocalRegionSpin->setDecimals(2);
    dofLayout->addWidget(new QLabel(tr("Focal Region:"), dofGroup), 1, 2);
    dofLayout->addWidget(m_dofFocalRegionSpin, 1, 3);

    m_dofBokehSpin = new QDoubleSpinBox(dofGroup);
    m_dofBokehSpin->setRange(0.0, 16.0);
    m_dofBokehSpin->setSingleStep(0.5);
    m_dofBokehSpin->setDecimals(1);
    m_dofBokehSpin->setSuffix(QStringLiteral(" px"));
    dofLayout->addWidget(new QLabel(tr("Bokeh Radius:"), dofGroup), 2, 0);
    dofLayout->addWidget(m_dofBokehSpin, 2, 1);

    contentLayout->addWidget(dofGroup);

    // =========================================================================
    // Section 4: Centripetal Catmull-Rom Keyframe Path
    // =========================================================================
    auto *pathGroup = new QGroupBox(tr("Centripetal Catmull-Rom Camera Path"), containerWidget);
    pathGroup->setStyleSheet(
        "QGroupBox { font-weight: bold; border: 1px solid #313244; border-radius: 6px; "
        "margin-top: 10px; padding-top: 10px; color: #cdd6f4; } "
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }");
    auto *pathLayout = new QVBoxLayout(pathGroup);
    pathLayout->setContentsMargins(8, 8, 8, 8);
    pathLayout->setSpacing(6);

    auto *pathHBox = new QHBoxLayout();
    m_pathEnableCheck = new QCheckBox(tr("Active Path"), pathGroup);
    m_loopModeCombo = new QComboBox(pathGroup);
    m_loopModeCombo->addItem(tr("Loop"), WE_CAM_PATH_LOOP);
    m_loopModeCombo->addItem(tr("Ping-Pong"), WE_CAM_PATH_PINGPONG);
    m_loopModeCombo->addItem(tr("Once"), WE_CAM_PATH_ONCE);
    pathHBox->addWidget(m_pathEnableCheck);
    pathHBox->addWidget(new QLabel(tr("Mode:"), pathGroup));
    pathHBox->addWidget(m_loopModeCombo);
    pathLayout->addLayout(pathHBox);

    m_keyframeList = new QListWidget(pathGroup);
    m_keyframeList->setMaximumHeight(110);
    m_keyframeList->setStyleSheet(
        "QListWidget { background-color: #181825; border: 1px solid #313244; "
        "border-radius: 4px; color: #cdd6f4; font-family: monospace; font-size: 11px; }");
    pathLayout->addWidget(m_keyframeList);

    auto *kfBtnLayout = new QHBoxLayout();
    m_kfTimeSpin = new QDoubleSpinBox(pathGroup);
    m_kfTimeSpin->setRange(0.0, 3600.0);
    m_kfTimeSpin->setSingleStep(0.5);
    m_kfTimeSpin->setDecimals(2);
    m_kfTimeSpin->setSuffix(QStringLiteral(" s"));

    m_addKfBtn = new QPushButton(tr("Add Keyframe"), pathGroup);
    m_removeKfBtn = new QPushButton(tr("Remove"), pathGroup);
    m_clearKfBtn = new QPushButton(tr("Clear"), pathGroup);

    kfBtnLayout->addWidget(new QLabel(tr("Time:"), pathGroup));
    kfBtnLayout->addWidget(m_kfTimeSpin);
    kfBtnLayout->addWidget(m_addKfBtn);
    kfBtnLayout->addWidget(m_removeKfBtn);
    kfBtnLayout->addWidget(m_clearKfBtn);
    pathLayout->addLayout(kfBtnLayout);

    contentLayout->addWidget(pathGroup);

    // =========================================================================
    // Section 5: Dynamic Scene Lights (8 Max)
    // =========================================================================
    auto *lightGroup =
        new QGroupBox(tr("Multi-Light Dynamic Grading (8 Lights Max)"), containerWidget);
    lightGroup->setStyleSheet(
        "QGroupBox { font-weight: bold; border: 1px solid #313244; border-radius: 6px; "
        "margin-top: 10px; padding-top: 10px; color: #cdd6f4; } "
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }");
    auto *lightLayout = new QGridLayout(lightGroup);
    lightLayout->setContentsMargins(8, 8, 8, 8);
    lightLayout->setSpacing(6);

    m_lightSlotCombo = new QComboBox(lightGroup);
    for (uint32_t i = 0; i < WE_MAX_SCENE_LIGHTS; ++i) {
        m_lightSlotCombo->addItem(QStringLiteral("Light Slot %1").arg(i), i);
    }
    lightLayout->addWidget(new QLabel(tr("Slot:"), lightGroup), 0, 0);
    lightLayout->addWidget(m_lightSlotCombo, 0, 1);

    m_lightTypeCombo = new QComboBox(lightGroup);
    m_lightTypeCombo->addItem(tr("Disabled"), WE_LIGHT_DISABLED);
    m_lightTypeCombo->addItem(tr("Point"), WE_LIGHT_POINT);
    m_lightTypeCombo->addItem(tr("Spot"), WE_LIGHT_SPOT);
    m_lightTypeCombo->addItem(tr("Directional"), WE_LIGHT_DIRECTIONAL);
    m_lightTypeCombo->addItem(tr("Tube"), WE_LIGHT_TUBE);
    lightLayout->addWidget(new QLabel(tr("Type:"), lightGroup), 0, 2);
    lightLayout->addWidget(m_lightTypeCombo, 0, 3);

    m_lightColorBtn = new QPushButton(tr("Pick Color"), lightGroup);
    m_lightColorBtn->setMinimumWidth(80);
    lightLayout->addWidget(new QLabel(tr("Color (OkLCH):"), lightGroup), 1, 0);
    lightLayout->addWidget(m_lightColorBtn, 1, 1);

    m_lightIntensitySpin = new QDoubleSpinBox(lightGroup);
    m_lightIntensitySpin->setRange(0.0, 20.0);
    m_lightIntensitySpin->setSingleStep(0.2);
    m_lightIntensitySpin->setDecimals(2);
    lightLayout->addWidget(new QLabel(tr("Intensity:"), lightGroup), 1, 2);
    lightLayout->addWidget(m_lightIntensitySpin, 1, 3);

    m_lightRadiusSpin = new QDoubleSpinBox(lightGroup);
    m_lightRadiusSpin->setRange(0.1, 1000.0);
    m_lightRadiusSpin->setSingleStep(1.0);
    m_lightRadiusSpin->setDecimals(1);
    lightLayout->addWidget(new QLabel(tr("Radius:"), lightGroup), 2, 0);
    lightLayout->addWidget(m_lightRadiusSpin, 2, 1);

    m_specularPowerSpin = new QDoubleSpinBox(lightGroup);
    m_specularPowerSpin->setRange(1.0, 256.0);
    m_specularPowerSpin->setSingleStep(4.0);
    m_specularPowerSpin->setDecimals(1);
    lightLayout->addWidget(new QLabel(tr("Shininess:"), lightGroup), 2, 2);
    lightLayout->addWidget(m_specularPowerSpin, 2, 3);

    // Pos / Dir
    lightLayout->addWidget(new QLabel(tr("Pos / Dir:"), lightGroup), 3, 0);
    auto *lposLayout = new QHBoxLayout();
    lposLayout->setSpacing(2);
    m_lightPosXSpin = new QDoubleSpinBox(lightGroup);
    m_lightPosYSpin = new QDoubleSpinBox(lightGroup);
    m_lightPosZSpin = new QDoubleSpinBox(lightGroup);
    for (auto *s : {m_lightPosXSpin, m_lightPosYSpin, m_lightPosZSpin}) {
        s->setRange(-10000.0, 10000.0);
        s->setSingleStep(0.5);
        s->setDecimals(2);
        lposLayout->addWidget(s);
    }
    lightLayout->addLayout(lposLayout, 3, 1, 1, 3);

    // Spot Axis Dir
    lightLayout->addWidget(new QLabel(tr("Spot Dir:"), lightGroup), 4, 0);
    auto *sdirLayout = new QHBoxLayout();
    sdirLayout->setSpacing(2);
    m_spotDirXSpin = new QDoubleSpinBox(lightGroup);
    m_spotDirYSpin = new QDoubleSpinBox(lightGroup);
    m_spotDirZSpin = new QDoubleSpinBox(lightGroup);
    for (auto *s : {m_spotDirXSpin, m_spotDirYSpin, m_spotDirZSpin}) {
        s->setRange(-1.0, 1.0);
        s->setSingleStep(0.1);
        s->setDecimals(2);
        sdirLayout->addWidget(s);
    }
    lightLayout->addLayout(sdirLayout, 4, 1, 1, 3);

    // Cones & Tube Length
    m_spotInnerSpin = new QDoubleSpinBox(lightGroup);
    m_spotInnerSpin->setRange(-1.0, 1.0);
    m_spotInnerSpin->setSingleStep(0.05);
    m_spotInnerSpin->setDecimals(3);
    lightLayout->addWidget(new QLabel(tr("Inner Cos:"), lightGroup), 5, 0);
    lightLayout->addWidget(m_spotInnerSpin, 5, 1);

    m_spotOuterSpin = new QDoubleSpinBox(lightGroup);
    m_spotOuterSpin->setRange(-1.0, 1.0);
    m_spotOuterSpin->setSingleStep(0.05);
    m_spotOuterSpin->setDecimals(3);
    lightLayout->addWidget(new QLabel(tr("Outer Cos:"), lightGroup), 5, 2);
    lightLayout->addWidget(m_spotOuterSpin, 5, 3);

    m_tubeLengthSpin = new QDoubleSpinBox(lightGroup);
    m_tubeLengthSpin->setRange(0.0, 100.0);
    m_tubeLengthSpin->setSingleStep(0.5);
    m_tubeLengthSpin->setDecimals(2);
    lightLayout->addWidget(new QLabel(tr("Tube Length:"), lightGroup), 6, 0);
    lightLayout->addWidget(m_tubeLengthSpin, 6, 1, 1, 3);

    contentLayout->addWidget(lightGroup);

    // =========================================================================
    // Section 6: Hemisphere Ambient & Normal Map
    // =========================================================================
    auto *ambGroup = new QGroupBox(tr("Hemisphere Ambient & Normal Map Bump"), containerWidget);
    ambGroup->setStyleSheet(
        "QGroupBox { font-weight: bold; border: 1px solid #313244; border-radius: 6px; "
        "margin-top: 10px; padding-top: 10px; color: #cdd6f4; } "
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }");
    auto *ambLayout = new QGridLayout(ambGroup);
    ambLayout->setContentsMargins(8, 8, 8, 8);
    ambLayout->setSpacing(6);

    m_ambientColorBtn = new QPushButton(tr("Ground Ambient"), ambGroup);
    m_skyColorBtn = new QPushButton(tr("Sky Tint"), ambGroup);
    ambLayout->addWidget(new QLabel(tr("Ambient (OkLCH):"), ambGroup), 0, 0);
    ambLayout->addWidget(m_ambientColorBtn, 0, 1);
    ambLayout->addWidget(new QLabel(tr("Sky Tint (OkLCH):"), ambGroup), 0, 2);
    ambLayout->addWidget(m_skyColorBtn, 0, 3);

    m_normalMapStrengthSpin = new QDoubleSpinBox(ambGroup);
    m_normalMapStrengthSpin->setRange(0.0, 2.0);
    m_normalMapStrengthSpin->setSingleStep(0.05);
    m_normalMapStrengthSpin->setDecimals(2);
    ambLayout->addWidget(new QLabel(tr("Normal Map Scale:"), ambGroup), 1, 0);
    ambLayout->addWidget(m_normalMapStrengthSpin, 1, 1, 1, 3);

    contentLayout->addWidget(ambGroup);

    // Reset Button
    m_resetBtn = new QPushButton(tr("Reset Camera & Lights to Defaults"), containerWidget);
    m_resetBtn->setStyleSheet(
        "QPushButton { background-color: #313244; color: #f38ba8; border-radius: 4px; "
        "padding: 6px; font-weight: bold; } "
        "QPushButton:hover { background-color: #45475a; }");
    contentLayout->addWidget(m_resetBtn);

    scrollArea->setWidget(containerWidget);
    rootLayout->addWidget(scrollArea);

    // =========================================================================
    // Signal Wiring
    // =========================================================================
    // Camera
    connect(m_projModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &CameraLightingInspectorWidget::onCameraControlChanged);
    for (auto *s : {m_fovSpin, m_nearZSpin, m_farZSpin, m_eyeXSpin, m_eyeYSpin, m_eyeZSpin,
                    m_targetXSpin, m_targetYSpin, m_targetZSpin, m_upXSpin, m_upYSpin, m_upZSpin}) {
        connect(s, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                &CameraLightingInspectorWidget::onCameraControlChanged);
    }
    connect(m_pathEnableCheck, &QCheckBox::toggled, this,
            &CameraLightingInspectorWidget::onCameraControlChanged);
    connect(m_loopModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &CameraLightingInspectorWidget::onCameraControlChanged);

    // Parallax
    for (auto *s : {m_parallaxXSpin, m_parallaxYSpin, m_parallaxSmoothingSpin}) {
        connect(s, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                &CameraLightingInspectorWidget::onParallaxControlChanged);
    }

    // DOF
    connect(m_dofEnableCheck, &QCheckBox::toggled, this,
            &CameraLightingInspectorWidget::onDofControlChanged);
    for (auto *s : {m_dofFocalDistSpin, m_dofFocalRegionSpin, m_dofBokehSpin}) {
        connect(s, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                &CameraLightingInspectorWidget::onDofControlChanged);
    }

    // Keyframes
    connect(m_addKfBtn, &QPushButton::clicked, this,
            &CameraLightingInspectorWidget::onAddKeyframeClicked);
    connect(m_removeKfBtn, &QPushButton::clicked, this,
            &CameraLightingInspectorWidget::onRemoveKeyframeClicked);
    connect(m_clearKfBtn, &QPushButton::clicked, this,
            &CameraLightingInspectorWidget::onClearKeyframesClicked);
    connect(m_keyframeList, &QListWidget::currentRowChanged, this,
            &CameraLightingInspectorWidget::onKeyframeSelectionChanged);

    // Lights
    connect(m_lightSlotCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &CameraLightingInspectorWidget::onLightSlotChanged);
    connect(m_lightTypeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &CameraLightingInspectorWidget::onLightControlChanged);
    for (auto *s :
         {m_lightIntensitySpin, m_lightRadiusSpin, m_specularPowerSpin, m_lightPosXSpin,
          m_lightPosYSpin, m_lightPosZSpin, m_spotDirXSpin, m_spotDirYSpin, m_spotDirZSpin,
          m_spotInnerSpin, m_spotOuterSpin, m_tubeLengthSpin, m_normalMapStrengthSpin}) {
        connect(s, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                &CameraLightingInspectorWidget::onLightControlChanged);
    }

    connect(m_lightColorBtn, &QPushButton::clicked, this,
            &CameraLightingInspectorWidget::onPickLightColor);
    connect(m_ambientColorBtn, &QPushButton::clicked, this,
            &CameraLightingInspectorWidget::onPickAmbientColor);
    connect(m_skyColorBtn, &QPushButton::clicked, this,
            &CameraLightingInspectorWidget::onPickSkyColor);

    connect(m_resetBtn, &QPushButton::clicked, this, [this]() {
        resetToDefaults();
        commitChange(m_camera, m_lighting);
        emit statusMessage(tr("Camera & lighting reset to defaults."));
    });
}

void CameraLightingInspectorWidget::syncUiFromModel()
{
    m_updatingUi = true;

    // Camera
    m_projModeCombo->setCurrentIndex(static_cast<int>(m_camera.projection_mode));
    m_fovSpin->setValue(m_camera.fov_y_deg);
    m_nearZSpin->setValue(m_camera.near_z);
    m_farZSpin->setValue(m_camera.far_z);
    m_eyeXSpin->setValue(m_camera.eye_pos[0]);
    m_eyeYSpin->setValue(m_camera.eye_pos[1]);
    m_eyeZSpin->setValue(m_camera.eye_pos[2]);
    m_targetXSpin->setValue(m_camera.target_pos[0]);
    m_targetYSpin->setValue(m_camera.target_pos[1]);
    m_targetZSpin->setValue(m_camera.target_pos[2]);
    m_upXSpin->setValue(m_camera.up_vec[0]);
    m_upYSpin->setValue(m_camera.up_vec[1]);
    m_upZSpin->setValue(m_camera.up_vec[2]);

    m_pathEnableCheck->setChecked(m_camera.path_enabled != 0);
    m_loopModeCombo->setCurrentIndex(static_cast<int>(m_camera.path_loop_mode));

    // Parallax
    m_parallaxXSpin->setValue(m_camera.parallax_amount[0]);
    m_parallaxYSpin->setValue(m_camera.parallax_amount[1]);
    m_parallaxSmoothingSpin->setValue(m_camera.parallax_smoothing);

    // DOF
    m_dofEnableCheck->setChecked(m_camera.dof_enabled > 0.5f);
    m_dofFocalDistSpin->setValue(m_camera.dof_focal_dist);
    m_dofFocalRegionSpin->setValue(m_camera.dof_focal_region);
    m_dofBokehSpin->setValue(m_camera.dof_bokeh_radius_px);

    // Keyframes
    updateKeyframeList();

    // Lights
    for (uint32_t i = 0; i < WE_MAX_SCENE_LIGHTS; ++i) {
        QString typeStr;
        switch (m_lighting.lights[i].light_type) {
        case WE_LIGHT_POINT:
            typeStr = tr("Point");
            break;
        case WE_LIGHT_SPOT:
            typeStr = tr("Spot");
            break;
        case WE_LIGHT_DIRECTIONAL:
            typeStr = tr("Directional");
            break;
        case WE_LIGHT_TUBE:
            typeStr = tr("Tube");
            break;
        case WE_LIGHT_DISABLED:
        default:
            typeStr = tr("Off");
            break;
        }
        m_lightSlotCombo->setItemText(static_cast<int>(i),
                                      QStringLiteral("Light %1 [%2]").arg(i).arg(typeStr));
    }
    m_lightSlotCombo->setCurrentIndex(static_cast<int>(m_selectedLightSlot));
    syncLightUiFromSlot(m_selectedLightSlot);

    // Ambient & Normal
    m_normalMapStrengthSpin->setValue(m_lighting.normal_map_strength);
    updateColorButtonSwatches();

    m_updatingUi = false;
}

void CameraLightingInspectorWidget::syncLightUiFromSlot(uint32_t slot)
{
    if (slot >= WE_MAX_SCENE_LIGHTS) {
        return;
    }

    const auto &l = m_lighting.lights[slot];
    m_updatingUi = true;
    m_lightTypeCombo->setCurrentIndex(static_cast<int>(l.light_type));
    m_lightIntensitySpin->setValue(l.intensity);
    m_lightRadiusSpin->setValue(l.attenuation_radius);
    m_specularPowerSpin->setValue(l.specular_power);

    m_lightPosXSpin->setValue(l.position_or_dir[0]);
    m_lightPosYSpin->setValue(l.position_or_dir[1]);
    m_lightPosZSpin->setValue(l.position_or_dir[2]);

    m_spotDirXSpin->setValue(l.spot_dir[0]);
    m_spotDirYSpin->setValue(l.spot_dir[1]);
    m_spotDirZSpin->setValue(l.spot_dir[2]);

    m_spotInnerSpin->setValue(l.spot_inner_cos);
    m_spotOuterSpin->setValue(l.spot_outer_cos);
    m_tubeLengthSpin->setValue(l.tube_length);

    m_updatingUi = false;
    updateColorButtonSwatches();
}

void CameraLightingInspectorWidget::updateKeyframeList()
{
    m_keyframeList->clear();
    for (uint32_t i = 0; i < m_camera.keyframe_count; ++i) {
        const auto &kf = m_camera.keyframes[i];
        QString itemText = QStringLiteral("#%1: t=%2s  pos=(%3, %4, %5)  fov=%6 deg")
                               .arg(i)
                               .arg(kf.time_sec, 0, 'f', 2)
                               .arg(kf.position[0], 0, 'f', 1)
                               .arg(kf.position[1], 0, 'f', 1)
                               .arg(kf.position[2], 0, 'f', 1)
                               .arg(kf.fov_deg, 0, 'f', 0);
        m_keyframeList->addItem(itemText);
    }
}

void CameraLightingInspectorWidget::updateColorButtonSwatches()
{
    auto applyButtonColor = [](QPushButton *btn, float linR, float linG, float linB,
                               const QString &label) {
        if (!btn)
            return;
        float sR = std::clamp(ColorScience::linearToSrgb(linR), 0.0f, 1.0f);
        float sG = std::clamp(ColorScience::linearToSrgb(linG), 0.0f, 1.0f);
        float sB = std::clamp(ColorScience::linearToSrgb(linB), 0.0f, 1.0f);
        int r = static_cast<int>(sR * 255.0f);
        int g = static_cast<int>(sG * 255.0f);
        int b = static_cast<int>(sB * 255.0f);

        // Compute luminance to choose dark or light text
        float lum = 0.299f * sR + 0.587f * sG + 0.114f * sB;
        QString textCol = lum > 0.5f ? QStringLiteral("#11111b") : QStringLiteral("#cdd6f4");

        btn->setText(label);
        btn->setStyleSheet(
            QStringLiteral("QPushButton { background-color: rgb(%1, %2, %3); color: %4; "
                           "border: 1px solid #45475a; border-radius: 4px; padding: 4px; "
                           "font-weight: bold; } "
                           "QPushButton:hover { border: 1px solid #89b4fa; }")
                .arg(r)
                .arg(g)
                .arg(b)
                .arg(textCol));
    };

    if (m_selectedLightSlot < WE_MAX_SCENE_LIGHTS) {
        const auto &l = m_lighting.lights[m_selectedLightSlot];
        applyButtonColor(m_lightColorBtn, l.color_rgb[0], l.color_rgb[1], l.color_rgb[2],
                         tr("Light Color"));
    }

    applyButtonColor(m_ambientColorBtn, m_lighting.ambient_color_rgb[0],
                     m_lighting.ambient_color_rgb[1], m_lighting.ambient_color_rgb[2],
                     tr("Ground Amb"));
    applyButtonColor(m_skyColorBtn, m_lighting.sky_color_rgb[0], m_lighting.sky_color_rgb[1],
                     m_lighting.sky_color_rgb[2], tr("Sky Tint"));
}

void CameraLightingInspectorWidget::onCameraControlChanged()
{
    if (m_updatingUi)
        return;

    we_camera3d_t newCam = m_camera;
    newCam.projection_mode = static_cast<uint32_t>(m_projModeCombo->currentIndex());
    newCam.fov_y_deg = static_cast<float>(m_fovSpin->value());
    newCam.near_z = static_cast<float>(m_nearZSpin->value());
    newCam.far_z = static_cast<float>(m_farZSpin->value());

    newCam.eye_pos[0] = static_cast<float>(m_eyeXSpin->value());
    newCam.eye_pos[1] = static_cast<float>(m_eyeYSpin->value());
    newCam.eye_pos[2] = static_cast<float>(m_eyeZSpin->value());

    newCam.target_pos[0] = static_cast<float>(m_targetXSpin->value());
    newCam.target_pos[1] = static_cast<float>(m_targetYSpin->value());
    newCam.target_pos[2] = static_cast<float>(m_targetZSpin->value());

    newCam.up_vec[0] = static_cast<float>(m_upXSpin->value());
    newCam.up_vec[1] = static_cast<float>(m_upYSpin->value());
    newCam.up_vec[2] = static_cast<float>(m_upZSpin->value());

    newCam.path_enabled = m_pathEnableCheck->isChecked() ? 1u : 0u;
    newCam.path_loop_mode = static_cast<uint32_t>(m_loopModeCombo->currentIndex());

    we_camera3d_evaluate(&newCam, 0.0, 0.0f, 0.0f, 16.0f / 9.0f);
    commitChange(newCam, m_lighting);
}

void CameraLightingInspectorWidget::onParallaxControlChanged()
{
    if (m_updatingUi)
        return;

    we_camera3d_t newCam = m_camera;
    newCam.parallax_amount[0] = static_cast<float>(m_parallaxXSpin->value());
    newCam.parallax_amount[1] = static_cast<float>(m_parallaxYSpin->value());
    newCam.parallax_smoothing = static_cast<float>(m_parallaxSmoothingSpin->value());

    commitChange(newCam, m_lighting);
}

void CameraLightingInspectorWidget::onDofControlChanged()
{
    if (m_updatingUi)
        return;

    we_camera3d_t newCam = m_camera;
    newCam.dof_enabled = m_dofEnableCheck->isChecked() ? 1.0f : 0.0f;
    newCam.dof_focal_dist = static_cast<float>(m_dofFocalDistSpin->value());
    newCam.dof_focal_region = static_cast<float>(m_dofFocalRegionSpin->value());
    newCam.dof_bokeh_radius_px = static_cast<float>(m_dofBokehSpin->value());

    commitChange(newCam, m_lighting);
}

void CameraLightingInspectorWidget::onKeyframeSelectionChanged()
{
    int row = m_keyframeList->currentRow();
    if (row >= 0 && static_cast<uint32_t>(row) < m_camera.keyframe_count) {
        m_kfTimeSpin->setValue(m_camera.keyframes[row].time_sec);
    }
}

void CameraLightingInspectorWidget::onAddKeyframeClicked()
{
    float timeSec = static_cast<float>(m_kfTimeSpin->value());
    addCameraKeyframe(timeSec);
}

void CameraLightingInspectorWidget::onRemoveKeyframeClicked()
{
    int row = m_keyframeList->currentRow();
    if (row >= 0) {
        removeCameraKeyframe(static_cast<uint32_t>(row));
    }
}

void CameraLightingInspectorWidget::onClearKeyframesClicked()
{
    if (m_camera.keyframe_count == 0)
        return;

    we_camera3d_t newCam = m_camera;
    newCam.keyframe_count = 0;
    commitChange(newCam, m_lighting);
    emit statusMessage(tr("All camera keyframes cleared."));
}

void CameraLightingInspectorWidget::onLightSlotChanged(int index)
{
    if (index >= 0 && static_cast<uint32_t>(index) < WE_MAX_SCENE_LIGHTS) {
        setSelectedLightSlot(static_cast<uint32_t>(index));
    }
}

void CameraLightingInspectorWidget::onLightControlChanged()
{
    if (m_updatingUi)
        return;

    we_lighting_uniforms_t newLights = m_lighting;
    auto &l = newLights.lights[m_selectedLightSlot];

    l.light_type = static_cast<uint32_t>(m_lightTypeCombo->currentIndex());
    l.intensity = static_cast<float>(m_lightIntensitySpin->value());
    l.attenuation_radius = static_cast<float>(m_lightRadiusSpin->value());
    l.specular_power = static_cast<float>(m_specularPowerSpin->value());

    l.position_or_dir[0] = static_cast<float>(m_lightPosXSpin->value());
    l.position_or_dir[1] = static_cast<float>(m_lightPosYSpin->value());
    l.position_or_dir[2] = static_cast<float>(m_lightPosZSpin->value());

    l.spot_dir[0] = static_cast<float>(m_spotDirXSpin->value());
    l.spot_dir[1] = static_cast<float>(m_spotDirYSpin->value());
    l.spot_dir[2] = static_cast<float>(m_spotDirZSpin->value());

    l.spot_inner_cos = static_cast<float>(m_spotInnerSpin->value());
    l.spot_outer_cos = static_cast<float>(m_spotOuterSpin->value());
    l.tube_length = static_cast<float>(m_tubeLengthSpin->value());

    newLights.normal_map_strength = static_cast<float>(m_normalMapStrengthSpin->value());

    // Update active light count
    uint32_t count = 0;
    for (uint32_t i = 0; i < WE_MAX_SCENE_LIGHTS; ++i) {
        if (newLights.lights[i].light_type != WE_LIGHT_DISABLED) {
            count = i + 1;
        }
    }
    newLights.active_light_count = count;

    commitChange(m_camera, newLights);
}

void CameraLightingInspectorWidget::onPickLightColor()
{
    if (m_selectedLightSlot >= WE_MAX_SCENE_LIGHTS)
        return;

    const auto &l = m_lighting.lights[m_selectedLightSlot];
    float sR = std::clamp(ColorScience::linearToSrgb(l.color_rgb[0]), 0.0f, 1.0f);
    float sG = std::clamp(ColorScience::linearToSrgb(l.color_rgb[1]), 0.0f, 1.0f);
    float sB = std::clamp(ColorScience::linearToSrgb(l.color_rgb[2]), 0.0f, 1.0f);
    QColor initialCol = QColor::fromRgbF(sR, sG, sB);

    OklchColorDialog dlg(initialCol, this);
    dlg.setWindowTitle(QStringLiteral("Light %1 Perceptual OkLCH Color").arg(m_selectedLightSlot));
    if (dlg.exec() == QDialog::Accepted) {
        float linR{1.0f}, linG{1.0f}, linB{1.0f};
        dlg.getLinearRgb(linR, linG, linB);
        we_lighting_uniforms_t newLights = m_lighting;
        newLights.lights[m_selectedLightSlot].color_rgb[0] = linR;
        newLights.lights[m_selectedLightSlot].color_rgb[1] = linG;
        newLights.lights[m_selectedLightSlot].color_rgb[2] = linB;
        commitChange(m_camera, newLights);
    }
}

void CameraLightingInspectorWidget::onPickAmbientColor()
{
    float sR = std::clamp(ColorScience::linearToSrgb(m_lighting.ambient_color_rgb[0]), 0.0f, 1.0f);
    float sG = std::clamp(ColorScience::linearToSrgb(m_lighting.ambient_color_rgb[1]), 0.0f, 1.0f);
    float sB = std::clamp(ColorScience::linearToSrgb(m_lighting.ambient_color_rgb[2]), 0.0f, 1.0f);
    QColor initialCol = QColor::fromRgbF(sR, sG, sB);

    OklchColorDialog dlg(initialCol, this);
    dlg.setWindowTitle(tr("Ground Hemisphere Ambient Color (OkLCH)"));
    if (dlg.exec() == QDialog::Accepted) {
        float linR{0.0f}, linG{0.0f}, linB{0.0f};
        dlg.getLinearRgb(linR, linG, linB);
        we_lighting_uniforms_t newLights = m_lighting;
        newLights.ambient_color_rgb[0] = linR;
        newLights.ambient_color_rgb[1] = linG;
        newLights.ambient_color_rgb[2] = linB;
        commitChange(m_camera, newLights);
    }
}

void CameraLightingInspectorWidget::onPickSkyColor()
{
    float sR = std::clamp(ColorScience::linearToSrgb(m_lighting.sky_color_rgb[0]), 0.0f, 1.0f);
    float sG = std::clamp(ColorScience::linearToSrgb(m_lighting.sky_color_rgb[1]), 0.0f, 1.0f);
    float sB = std::clamp(ColorScience::linearToSrgb(m_lighting.sky_color_rgb[2]), 0.0f, 1.0f);
    QColor initialCol = QColor::fromRgbF(sR, sG, sB);

    OklchColorDialog dlg(initialCol, this);
    dlg.setWindowTitle(tr("Upper Hemisphere Sky Color (OkLCH)"));
    if (dlg.exec() == QDialog::Accepted) {
        float linR{0.0f}, linG{0.0f}, linB{0.0f};
        dlg.getLinearRgb(linR, linG, linB);
        we_lighting_uniforms_t newLights = m_lighting;
        newLights.sky_color_rgb[0] = linR;
        newLights.sky_color_rgb[1] = linG;
        newLights.sky_color_rgb[2] = linB;
        commitChange(m_camera, newLights);
    }
}

} // namespace waywal::studio
