// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/inspector/camera_lighting_inspector_widget.hpp"
#include "studio/inspector/oklch_color_dialog.hpp"

#include <QColor>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>

namespace waywal::studio {

namespace {

[[nodiscard]] float sanitizeFinite(float v, float fallback, float minVal, float maxVal) noexcept
{
    if (!std::isfinite(v)) {
        return fallback;
    }
    return std::clamp(v, minVal, maxVal);
}

void computeLookAtQuaternion(const float eye[3], const float target[3], const float up[3],
                             float outQuat[4]) noexcept
{
    float fx = target[0] - eye[0];
    float fy = target[1] - eye[1];
    float fz = target[2] - eye[2];
    const float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (!std::isfinite(flen) || flen < 1e-5f) {
        quat_identity(outQuat);
        return;
    }
    fx /= flen;
    fy /= flen;
    fz /= flen;

    // Cross(forward, up) yields right vector matching quat_to_mat4 view basis columns [s, u, -f].
    float sx = fy * up[2] - fz * up[1];
    float sy = fz * up[0] - fx * up[2];
    float sz = fx * up[1] - fy * up[0];
    float slen = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (!std::isfinite(slen) || slen < 1e-5f) {
        quat_identity(outQuat);
        return;
    }
    sx /= slen;
    sy /= slen;
    sz /= slen;

    const float ux = sy * fz - sz * fy;
    const float uy = sz * fx - sx * fz;
    const float uz = sx * fy - sy * fx;

    const float r00 = sx, r01 = ux, r02 = -fx;
    const float r10 = sy, r11 = uy, r12 = -fy;
    const float r20 = sz, r21 = uz, r22 = -fz;

    float qx = 0.0f;
    float qy = 0.0f;
    float qz = 0.0f;
    float qw = 1.0f;
    const float trace = r00 + r11 + r22;
    if (trace > 0.0f) {
        const float s = 2.0f * std::sqrt(trace + 1.0f);
        qw = 0.25f * s;
        qx = (r21 - r12) / s;
        qy = (r02 - r20) / s;
        qz = (r10 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
        const float s = 2.0f * std::sqrt(std::max(1e-8f, 1.0f + r00 - r11 - r22));
        qw = (r21 - r12) / s;
        qx = 0.25f * s;
        qy = (r01 + r10) / s;
        qz = (r02 + r20) / s;
    } else if (r11 > r22) {
        const float s = 2.0f * std::sqrt(std::max(1e-8f, 1.0f + r11 - r00 - r22));
        qw = (r02 - r20) / s;
        qx = (r01 + r10) / s;
        qy = 0.25f * s;
        qz = (r12 + r21) / s;
    } else {
        const float s = 2.0f * std::sqrt(std::max(1e-8f, 1.0f + r22 - r00 - r11));
        qw = (r10 - r01) / s;
        qx = (r02 + r20) / s;
        qy = (r12 + r21) / s;
        qz = 0.25f * s;
    }

    const float qlen = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    if (!std::isfinite(qlen) || qlen < 1e-6f) {
        quat_identity(outQuat);
        return;
    }
    outQuat[0] = qx / qlen;
    outQuat[1] = qy / qlen;
    outQuat[2] = qz / qlen;
    outQuat[3] = qw / qlen;
}

[[nodiscard]] int findComboIndexByUint(const QComboBox *combo, uint32_t value)
{
    if (!combo) {
        return -1;
    }
    return combo->findData(QVariant::fromValue<uint>(value));
}

[[nodiscard]] uint32_t readComboUint(const QComboBox *combo, uint32_t fallback, uint32_t maxValid)
{
    if (!combo) {
        return fallback;
    }
    const int idx = combo->currentIndex();
    if (idx < 0) {
        return fallback;
    }
    const QVariant data = combo->itemData(idx);
    bool ok = false;
    const uint val = data.toUInt(&ok);
    if (!ok || val > maxValid) {
        return fallback;
    }
    return static_cast<uint32_t>(val);
}

class ResetCameraLightsCommand final : public ICommand {
  public:
    using ApplyFn =
        std::function<void(const QString &output, const we_camera3d_t &cam,
                           const we_lighting_uniforms_t &lights, uint32_t selectedSlot)>;

    ResetCameraLightsCommand(QString output, const we_camera3d_t &oldCam,
                             const we_camera3d_t &newCam, const we_lighting_uniforms_t &oldLights,
                             const we_lighting_uniforms_t &newLights, uint32_t oldSlot,
                             uint32_t newSlot, ApplyFn applyFn)
        : m_output(std::move(output)), m_oldCam(oldCam), m_newCam(newCam), m_oldLights(oldLights),
          m_newLights(newLights), m_oldSlot(oldSlot), m_newSlot(newSlot),
          m_applyFn(std::move(applyFn))
    {
    }

    void execute() override
    {
        if (m_applyFn) {
            m_applyFn(m_output, m_newCam, m_newLights, m_newSlot);
        }
    }

    void undo() override
    {
        if (m_applyFn) {
            m_applyFn(m_output, m_oldCam, m_oldLights, m_oldSlot);
        }
    }

    QString describe() const override { return QStringLiteral("Reset 3D camera & scene lights"); }

    bool mergeWith(const ICommand *other) override
    {
        (void)other;
        return false;
    }

  private:
    QString m_output;
    we_camera3d_t m_oldCam{};
    we_camera3d_t m_newCam{};
    we_lighting_uniforms_t m_oldLights{};
    we_lighting_uniforms_t m_newLights{};
    uint32_t m_oldSlot{0};
    uint32_t m_newSlot{0};
    ApplyFn m_applyFn;
};

} // namespace

CameraLightingInspectorWidget::CameraLightingInspectorWidget(QWidget *parent) : QWidget(parent)
{
    resetToDefaults();
    setupUi();
    syncUiFromModel();
}

CameraLightingInspectorWidget::~CameraLightingInspectorWidget() { closeActiveColorDialog(); }

void CameraLightingInspectorWidget::populateDefaults(we_camera3d_t &cam,
                                                     we_lighting_uniforms_t &lights)
{
    std::memset(&cam, 0, sizeof(cam));
    std::memset(&lights, 0, sizeof(lights));

    we_camera3d_init_default(&cam, 1920.0f, 1080.0f);
    cam.parallax_amount[0] = 15.0f;
    cam.parallax_amount[1] = 10.0f;
    cam.parallax_smoothing = 0.15f;
    cam.dof_focal_dist = 5.0f;
    cam.dof_focal_region = 2.0f;
    cam.dof_bokeh_radius_px = 6.0f;

    // Ambient & Sky hemisphere defaults
    lights.ambient_color_rgb[0] = 0.12f;
    lights.ambient_color_rgb[1] = 0.14f;
    lights.ambient_color_rgb[2] = 0.18f;

    lights.sky_color_rgb[0] = 0.25f;
    lights.sky_color_rgb[1] = 0.35f;
    lights.sky_color_rgb[2] = 0.50f;

    lights.normal_map_strength = 1.0f;
    lights.active_light_count = 1;

    // Default Light 0: Point light
    lights.lights[0].light_type = WE_LIGHT_POINT;
    lights.lights[0].position_or_dir[0] = 0.0f;
    lights.lights[0].position_or_dir[1] = 2.5f;
    lights.lights[0].position_or_dir[2] = 3.0f;
    lights.lights[0].color_rgb[0] = 1.0f;
    lights.lights[0].color_rgb[1] = 0.95f;
    lights.lights[0].color_rgb[2] = 0.85f;
    lights.lights[0].intensity = 2.5f;
    lights.lights[0].attenuation_radius = 20.0f;
    lights.lights[0].spot_inner_cos = 0.95f;
    lights.lights[0].spot_outer_cos = 0.80f;
    lights.lights[0].spot_dir[0] = 0.0f;
    lights.lights[0].spot_dir[1] = 0.0f;
    lights.lights[0].spot_dir[2] = -1.0f;
    lights.lights[0].tube_length = 2.0f;
    lights.lights[0].specular_power = 32.0f;

    // Lights 1..7: Disabled by default
    for (uint32_t i = 1; i < WE_MAX_SCENE_LIGHTS; ++i) {
        lights.lights[i].light_type = WE_LIGHT_DISABLED;
        lights.lights[i].position_or_dir[0] = 0.0f;
        lights.lights[i].position_or_dir[1] = 1.0f;
        lights.lights[i].position_or_dir[2] = 0.0f;
        lights.lights[i].color_rgb[0] = 1.0f;
        lights.lights[i].color_rgb[1] = 1.0f;
        lights.lights[i].color_rgb[2] = 1.0f;
        lights.lights[i].intensity = 1.0f;
        lights.lights[i].attenuation_radius = 10.0f;
        lights.lights[i].spot_inner_cos = 0.95f;
        lights.lights[i].spot_outer_cos = 0.80f;
        lights.lights[i].spot_dir[0] = 0.0f;
        lights.lights[i].spot_dir[1] = 0.0f;
        lights.lights[i].spot_dir[2] = -1.0f;
        lights.lights[i].tube_length = 2.0f;
        lights.lights[i].specular_power = 32.0f;
    }
}

void CameraLightingInspectorWidget::sanitizeLighting(we_lighting_uniforms_t &lights,
                                                     bool clampUnusedSlotsToDisabled)
{
    for (int i = 0; i < 3; ++i) {
        lights.ambient_color_rgb[i] =
            sanitizeFinite(lights.ambient_color_rgb[i], 0.1f, 0.0f, 100.0f);
        lights.sky_color_rgb[i] = sanitizeFinite(lights.sky_color_rgb[i], 0.2f, 0.0f, 100.0f);
    }
    lights.normal_map_strength = sanitizeFinite(lights.normal_map_strength, 1.0f, 0.0f, 10.0f);

    const uint32_t boundedActive =
        std::min<uint32_t>(lights.active_light_count, WE_MAX_SCENE_LIGHTS);
    uint32_t lastActivePlusOne = 0;
    for (uint32_t i = 0; i < WE_MAX_SCENE_LIGHTS; ++i) {
        auto &l = lights.lights[i];
        if ((clampUnusedSlotsToDisabled && i >= boundedActive) || l.light_type > WE_LIGHT_TUBE) {
            l.light_type = WE_LIGHT_DISABLED;
        }

        for (int c = 0; c < 3; ++c) {
            l.position_or_dir[c] =
                sanitizeFinite(l.position_or_dir[c], 0.0f, -100000.0f, 100000.0f);
            l.color_rgb[c] = sanitizeFinite(l.color_rgb[c], 1.0f, 0.0f, 100.0f);
            l.spot_dir[c] = sanitizeFinite(l.spot_dir[c], (c == 2) ? -1.0f : 0.0f, -1.0f, 1.0f);
        }

        if (l.light_type == WE_LIGHT_SPOT || l.light_type == WE_LIGHT_TUBE) {
            const float slen =
                std::sqrt(l.spot_dir[0] * l.spot_dir[0] + l.spot_dir[1] * l.spot_dir[1] +
                          l.spot_dir[2] * l.spot_dir[2]);
            if (!std::isfinite(slen) || slen < 1e-5f) {
                l.spot_dir[0] = 0.0f;
                l.spot_dir[1] = 0.0f;
                l.spot_dir[2] = -1.0f;
            }
        }

        l.intensity = sanitizeFinite(l.intensity, 1.0f, 0.0f, 10000.0f);
        l.attenuation_radius = sanitizeFinite(l.attenuation_radius, 10.0f, 1e-3f, 1.0e6f);
        l.tube_length = sanitizeFinite(l.tube_length, 0.0f, 0.0f, 1.0e5f);
        l.specular_power = sanitizeFinite(l.specular_power, 32.0f, 1.0f, 2048.0f);

        float inner = sanitizeFinite(l.spot_inner_cos, 0.95f, -1.0f, 1.0f);
        float outer = sanitizeFinite(l.spot_outer_cos, 0.80f, -1.0f, 1.0f);
        if (inner < outer) {
            std::swap(inner, outer);
        }
        if (inner - outer < 1e-4f) {
            if (inner <= 1.0f - 1e-4f) {
                inner = outer + 1e-4f;
            } else {
                outer = inner - 1e-4f;
            }
        }
        l.spot_inner_cos = inner;
        l.spot_outer_cos = outer;

        if (l.light_type != WE_LIGHT_DISABLED) {
            lastActivePlusOne = i + 1;
        }
    }
    lights.active_light_count = lastActivePlusOne;
}

void CameraLightingInspectorWidget::sanitizeAndEvaluateCamera(we_camera3d_t &cam)
{
    if (cam.projection_mode > WE_PROJ_DEPTH_PARALLAX_25D) {
        cam.projection_mode = WE_PROJ_PERSPECTIVE_3D;
    }
    if (cam.path_loop_mode > WE_CAM_PATH_ONCE) {
        cam.path_loop_mode = WE_CAM_PATH_LOOP;
    }
    cam.path_enabled = (cam.path_enabled != 0) ? 1u : 0u;
    cam.keyframe_count = std::min<uint32_t>(cam.keyframe_count, WE_CAMERA_MAX_KEYFRAMES);

    for (uint32_t i = 0; i < cam.keyframe_count; ++i) {
        auto &kf = cam.keyframes[i];
        kf.time_sec = sanitizeFinite(kf.time_sec, 0.0f, 0.0f, 3600.0f);
        for (int c = 0; c < 3; ++c) {
            kf.position[c] = sanitizeFinite(kf.position[c], 0.0f, -100000.0f, 100000.0f);
        }
        float qlen2 = 0.0f;
        bool qFinite = true;
        for (int c = 0; c < 4; ++c) {
            if (!std::isfinite(kf.orientation_quat[c])) {
                qFinite = false;
                break;
            }
            qlen2 += kf.orientation_quat[c] * kf.orientation_quat[c];
        }
        if (!qFinite || qlen2 < 1e-8f) {
            quat_identity(kf.orientation_quat);
        } else {
            const float inv = 1.0f / std::sqrt(qlen2);
            for (int c = 0; c < 4; ++c) {
                kf.orientation_quat[c] *= inv;
            }
        }
        kf.fov_deg = sanitizeFinite(kf.fov_deg, 45.0f, 1.0f, 179.0f);
        kf.focal_distance = sanitizeFinite(kf.focal_distance, 5.0f, 0.01f, 1000.0f);
        kf.aperture_fstop = sanitizeFinite(kf.aperture_fstop, 2.8f, 0.7f, 64.0f);
        kf.tension = sanitizeFinite(kf.tension, 0.5f, 0.0f, 1.0f);
    }

    cam.parallax_amount[0] = sanitizeFinite(cam.parallax_amount[0], 0.0f, 0.0f, 200.0f);
    cam.parallax_amount[1] = sanitizeFinite(cam.parallax_amount[1], 0.0f, 0.0f, 200.0f);
    cam.parallax_smoothing = sanitizeFinite(cam.parallax_smoothing, 0.15f, 0.01f, 1.0f);

    cam.dof_enabled = (std::isfinite(cam.dof_enabled) && cam.dof_enabled > 0.5f) ? 1.0f : 0.0f;
    cam.dof_focal_dist = sanitizeFinite(cam.dof_focal_dist, 5.0f, 0.01f, 1000.0f);
    cam.dof_focal_region = sanitizeFinite(cam.dof_focal_region, 2.0f, 0.0f, 1000.0f);
    cam.dof_bokeh_radius_px = sanitizeFinite(cam.dof_bokeh_radius_px, 6.0f, 0.0f, 16.0f);

    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(cam.eye_pos[i])) {
            cam.eye_pos[i] = (i == 2) ? 5.0f : 0.0f;
        }
        if (!std::isfinite(cam.target_pos[i])) {
            cam.target_pos[i] = 0.0f;
        }
        if (!std::isfinite(cam.up_vec[i])) {
            cam.up_vec[i] = (i == 1) ? 1.0f : 0.0f;
        }
    }

    float fx = cam.target_pos[0] - cam.eye_pos[0];
    float fy = cam.target_pos[1] - cam.eye_pos[1];
    float fz = cam.target_pos[2] - cam.eye_pos[2];
    float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (!std::isfinite(flen) || flen < 1e-5f) {
        // Displace target along -Z when eye and target coincide so mat4_look_at remains
        // non-singular.
        cam.target_pos[2] = cam.eye_pos[2] - 1.0f;
        fx = 0.0f;
        fy = 0.0f;
        fz = -1.0f;
        flen = 1.0f;
    }
    fx /= flen;
    fy /= flen;
    fz /= flen;

    float ulen = std::sqrt(cam.up_vec[0] * cam.up_vec[0] + cam.up_vec[1] * cam.up_vec[1] +
                           cam.up_vec[2] * cam.up_vec[2]);
    if (!std::isfinite(ulen) || ulen < 1e-5f) {
        cam.up_vec[0] = 0.0f;
        cam.up_vec[1] = 1.0f;
        cam.up_vec[2] = 0.0f;
        ulen = 1.0f;
    }

    const float dotFU =
        std::fabs((fx * cam.up_vec[0] + fy * cam.up_vec[1] + fz * cam.up_vec[2]) / ulen);
    if (dotFU > 0.999f) {
        if (std::fabs(fy) < 0.9f) {
            cam.up_vec[0] = 0.0f;
            cam.up_vec[1] = 1.0f;
            cam.up_vec[2] = 0.0f;
        } else {
            cam.up_vec[0] = 0.0f;
            cam.up_vec[1] = 0.0f;
            cam.up_vec[2] = (fz >= 0.0f) ? -1.0f : 1.0f;
        }
    }

    if (!std::isfinite(cam.fov_y_deg)) {
        cam.fov_y_deg = 45.0f;
    }
    cam.fov_y_deg = std::clamp(cam.fov_y_deg, 1.0f, 179.0f);

    if (!std::isfinite(cam.near_z) || cam.near_z < 0.001f) {
        cam.near_z = 0.01f;
    }
    if (!std::isfinite(cam.far_z) || cam.far_z <= cam.near_z + 0.01f) {
        cam.far_z = cam.near_z + 100.0f;
    }

    we_camera3d_evaluate(&cam, 0.0, 0.0f, 0.0f, 16.0f / 9.0f);
}

void CameraLightingInspectorWidget::resetToDefaults()
{
    closeActiveColorDialog();
    populateDefaults(m_camera, m_lighting);
    m_selectedLightSlot = 0;
    if (m_projModeCombo) {
        syncUiFromModel();
    }
}

void CameraLightingInspectorWidget::setCameraAndLights(const we_camera3d_t &cam,
                                                       const we_lighting_uniforms_t &lights)
{
    closeActiveColorDialog();
    m_camera = cam;
    sanitizeAndEvaluateCamera(m_camera);
    m_lighting = lights;
    sanitizeLighting(m_lighting, true);
    if (m_selectedLightSlot >= WE_MAX_SCENE_LIGHTS) {
        m_selectedLightSlot = 0;
    }
    syncUiFromModel();
}

bool CameraLightingInspectorWidget::addCameraKeyframe(float timeSec)
{
    if (!std::isfinite(timeSec) || timeSec < 0.0f) {
        emit statusMessage(QStringLiteral("Keyframe time must be finite and non-negative."));
        return false;
    }
    if (m_camera.keyframe_count >= WE_CAMERA_MAX_KEYFRAMES) {
        emit statusMessage(QStringLiteral("Maximum camera keyframes (64) reached."));
        return false;
    }

    we_camera3d_t newCam = m_camera;
    sanitizeAndEvaluateCamera(newCam);

    we_camera_kf_t kf{};
    kf.time_sec = std::clamp(timeSec, 0.0f, 3600.0f);
    kf.position[0] = newCam.eye_pos[0];
    kf.position[1] = newCam.eye_pos[1];
    kf.position[2] = newCam.eye_pos[2];
    computeLookAtQuaternion(newCam.eye_pos, newCam.target_pos, newCam.up_vec, kf.orientation_quat);
    kf.fov_deg = newCam.fov_y_deg;
    kf.focal_distance = newCam.dof_focal_dist;
    kf.aperture_fstop = m_kfApertureSpin ? static_cast<float>(m_kfApertureSpin->value()) : 2.8f;
    kf.tension = m_kfTensionSpin ? static_cast<float>(m_kfTensionSpin->value()) : 0.5f;

    // Insert sorted by time_sec
    uint32_t insertIdx = newCam.keyframe_count;
    for (uint32_t i = 0; i < newCam.keyframe_count; ++i) {
        if (newCam.keyframes[i].time_sec > kf.time_sec) {
            insertIdx = i;
            break;
        }
    }

    for (uint32_t i = newCam.keyframe_count; i > insertIdx; --i) {
        newCam.keyframes[i] = newCam.keyframes[i - 1];
    }
    newCam.keyframes[insertIdx] = kf;
    ++newCam.keyframe_count;
    sanitizeAndEvaluateCamera(newCam);

    commitChange(newCam, m_lighting);
    emit statusMessage(QStringLiteral("Added camera keyframe at %1 s").arg(kf.time_sec, 0, 'f', 2));
    return true;
}

bool CameraLightingInspectorWidget::removeCameraKeyframe(uint32_t index)
{
    if (index >= m_camera.keyframe_count || m_camera.keyframe_count > WE_CAMERA_MAX_KEYFRAMES) {
        return false;
    }

    we_camera3d_t newCam = m_camera;
    for (uint32_t i = index; i + 1 < newCam.keyframe_count; ++i) {
        newCam.keyframes[i] = newCam.keyframes[i + 1];
    }
    --newCam.keyframe_count;
    std::memset(&newCam.keyframes[newCam.keyframe_count], 0, sizeof(we_camera_kf_t));
    sanitizeAndEvaluateCamera(newCam);

    commitChange(newCam, m_lighting);
    emit statusMessage(QStringLiteral("Removed camera keyframe #%1").arg(index));
    return true;
}

void CameraLightingInspectorWidget::setSelectedLightSlot(uint32_t slotIndex)
{
    if (slotIndex >= WE_MAX_SCENE_LIGHTS) {
        return;
    }
    m_selectedLightSlot = slotIndex;
    if (m_lightSlotCombo) {
        const int targetIdx = findComboIndexByUint(m_lightSlotCombo, slotIndex);
        if (targetIdx >= 0 && m_lightSlotCombo->currentIndex() != targetIdx) {
            const QScopedValueRollback<bool> guard(m_updatingUi, true);
            m_lightSlotCombo->setCurrentIndex(targetIdx);
        }
    }
    syncLightUiFromSlot(slotIndex);
}

void CameraLightingInspectorWidget::emitStateUpdated(const QString &output,
                                                     const we_camera3d_t &cam,
                                                     const we_lighting_uniforms_t &lights)
{
    sendIpcUpdate(output, cam, lights);
    emit cameraOrLightsChanged(output, cam, lights);
    emit camera3DChanged(output, cam);
    emit lightingChanged(output, lights);
}

void CameraLightingInspectorWidget::commitChange(const we_camera3d_t &newCam,
                                                 const we_lighting_uniforms_t &newLights)
{
    if (m_updatingUi) {
        return;
    }

    closeActiveColorDialog();

    we_camera3d_t sanitizedCam = newCam;
    we_lighting_uniforms_t sanitizedLights = newLights;
    sanitizeAndEvaluateCamera(sanitizedCam);
    sanitizeLighting(sanitizedLights);

    if (m_commandBus) {
        we_camera3d_t oldCam = m_camera;
        we_lighting_uniforms_t oldLights = m_lighting;
        QPointer<CameraLightingInspectorWidget> self(this);
        auto cmd = std::make_unique<SetCamera3DCommand>(
            m_targetOutput, oldCam, sanitizedCam, oldLights, sanitizedLights,
            [self](const QString &output, const we_camera3d_t &c, const we_lighting_uniforms_t &l) {
                if (!self) {
                    return;
                }
                self->closeActiveColorDialog();
                self->m_camera = c;
                self->m_lighting = l;
                self->syncUiFromModel();
                self->emitStateUpdated(output, c, l);
            });
        m_commandBus->execute(std::move(cmd));
    } else {
        m_camera = sanitizedCam;
        m_lighting = sanitizedLights;
        syncUiFromModel();
        emitStateUpdated(m_targetOutput, m_camera, m_lighting);
    }
}

void CameraLightingInspectorWidget::sendIpcUpdate(const QString &output, const we_camera3d_t &cam,
                                                  const we_lighting_uniforms_t &lights)
{
    if (m_ipcClient) {
        m_ipcClient->sendSetCamera3D(output, cam);

        // Compact disabled light slots so daemon shaders pack active lights contiguously.
        we_scene_light_t activeLights[WE_MAX_SCENE_LIGHTS]{};
        uint32_t activeCount = 0;
        const uint32_t slotBound =
            std::min<uint32_t>(lights.active_light_count, WE_MAX_SCENE_LIGHTS);
        for (uint32_t i = 0; i < slotBound; ++i) {
            if (lights.lights[i].light_type != WE_LIGHT_DISABLED &&
                lights.lights[i].light_type <= WE_LIGHT_TUBE) {
                activeLights[activeCount++] = lights.lights[i];
            }
        }
        we_lighting_uniforms_t packed{};
        we_lighting_pack_ubo(activeLights, activeCount, lights.ambient_color_rgb,
                             lights.sky_color_rgb, lights.normal_map_strength, &packed);
        m_ipcClient->sendSetSceneLights(output, packed);
    }
}

void CameraLightingInspectorWidget::setupUi()
{
    setStyleSheet(
        "CameraLightingInspectorWidget { background-color: #181B22; color: #F1F5F9; "
        "font-size: 12px; } "
        "QLabel { color: #94A3B8; font-size: 11px; } "
        "QCheckBox { color: #F1F5F9; font-size: 12px; min-height: 28px; spacing: 6px; } "
        "QCheckBox:focus { outline: 2px solid #38BDF8; border-radius: 4px; } "
        "QComboBox, QDoubleSpinBox, QSpinBox { background-color: #12141A; color: #F1F5F9; "
        "border: 1px solid #2D3340; border-radius: 4px; padding: 3px 6px; "
        "min-height: 28px; font-size: 12px; } "
        "QComboBox:focus, QDoubleSpinBox:focus, QSpinBox:focus { border: 2px solid #38BDF8; } "
        "QPushButton { background-color: #242936; color: #F1F5F9; "
        "border: 1px solid #334155; border-radius: 4px; padding: 4px 10px; "
        "min-height: 28px; min-width: 28px; font-size: 12px; } "
        "QPushButton:hover { background-color: #2E3444; } "
        "QPushButton:focus { border: 2px solid #38BDF8; }");

    const QString groupStyle =
        QStringLiteral("QGroupBox { font-weight: bold; font-size: 12px; border: 1px solid #2D3340; "
                       "border-radius: 6px; margin-top: 10px; padding-top: 10px; color: #F1F5F9; } "
                       "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; "
                       "color: #F1F5F9; }");

    auto initSpin = [](QDoubleSpinBox *spin, double minVal, double maxVal, double step,
                       int decimals, const QString &suffix = QString()) {
        spin->setRange(minVal, maxVal);
        spin->setSingleStep(step);
        spin->setDecimals(decimals);
        spin->setKeyboardTracking(false);
        spin->setMinimumHeight(28);
        if (!suffix.isEmpty()) {
            spin->setSuffix(suffix);
        }
    };

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(8);

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
    camGroup->setStyleSheet(groupStyle);
    auto *camLayout = new QGridLayout(camGroup);
    camLayout->setContentsMargins(8, 8, 8, 8);
    camLayout->setSpacing(6);

    m_projModeCombo = new QComboBox(camGroup);
    m_projModeCombo->setMinimumHeight(28);
    m_projModeCombo->addItem(tr("Orthographic 2D"), WE_PROJ_ORTHOGRAPHIC_2D);
    m_projModeCombo->addItem(tr("Perspective 3D"), WE_PROJ_PERSPECTIVE_3D);
    m_projModeCombo->addItem(tr("Depth Parallax 2.5D"), WE_PROJ_DEPTH_PARALLAX_25D);
    camLayout->addWidget(new QLabel(tr("Projection:"), camGroup), 0, 0);
    camLayout->addWidget(m_projModeCombo, 0, 1, 1, 3);

    m_fovSpin = new QDoubleSpinBox(camGroup);
    initSpin(m_fovSpin, 10.0, 140.0, 1.0, 1, QStringLiteral(" deg"));
    camLayout->addWidget(new QLabel(tr("FOV (Y):"), camGroup), 1, 0);
    camLayout->addWidget(m_fovSpin, 1, 1);

    m_nearZSpin = new QDoubleSpinBox(camGroup);
    initSpin(m_nearZSpin, 0.01, 100.0, 0.1, 3);
    camLayout->addWidget(new QLabel(tr("Near Z:"), camGroup), 1, 2);
    camLayout->addWidget(m_nearZSpin, 1, 3);

    m_farZSpin = new QDoubleSpinBox(camGroup);
    initSpin(m_farZSpin, 0.1, 10000.0, 10.0, 1);
    camLayout->addWidget(new QLabel(tr("Far Z:"), camGroup), 2, 2);
    camLayout->addWidget(m_farZSpin, 2, 3);

    // Eye Position
    camLayout->addWidget(new QLabel(tr("Eye Pos (X/Y/Z):"), camGroup), 3, 0);
    auto *eyeLayout = new QHBoxLayout();
    eyeLayout->setSpacing(4);
    m_eyeXSpin = new QDoubleSpinBox(camGroup);
    m_eyeYSpin = new QDoubleSpinBox(camGroup);
    m_eyeZSpin = new QDoubleSpinBox(camGroup);
    for (auto *s : {m_eyeXSpin, m_eyeYSpin, m_eyeZSpin}) {
        initSpin(s, -10000.0, 10000.0, 0.2, 2);
        eyeLayout->addWidget(s);
    }
    camLayout->addLayout(eyeLayout, 3, 1, 1, 3);

    // Target Position
    camLayout->addWidget(new QLabel(tr("Target Pos (X/Y/Z):"), camGroup), 4, 0);
    auto *targetLayout = new QHBoxLayout();
    targetLayout->setSpacing(4);
    m_targetXSpin = new QDoubleSpinBox(camGroup);
    m_targetYSpin = new QDoubleSpinBox(camGroup);
    m_targetZSpin = new QDoubleSpinBox(camGroup);
    for (auto *s : {m_targetXSpin, m_targetYSpin, m_targetZSpin}) {
        initSpin(s, -10000.0, 10000.0, 0.2, 2);
        targetLayout->addWidget(s);
    }
    camLayout->addLayout(targetLayout, 4, 1, 1, 3);

    // Up Vector
    camLayout->addWidget(new QLabel(tr("Up Vector (X/Y/Z):"), camGroup), 5, 0);
    auto *upLayout = new QHBoxLayout();
    upLayout->setSpacing(4);
    m_upXSpin = new QDoubleSpinBox(camGroup);
    m_upYSpin = new QDoubleSpinBox(camGroup);
    m_upZSpin = new QDoubleSpinBox(camGroup);
    for (auto *s : {m_upXSpin, m_upYSpin, m_upZSpin}) {
        initSpin(s, -1.0, 1.0, 0.1, 2);
        upLayout->addWidget(s);
    }
    camLayout->addLayout(upLayout, 5, 1, 1, 3);

    contentLayout->addWidget(camGroup);

    // =========================================================================
    // Section 2: Mouse Parallax Spring Damping
    // =========================================================================
    auto *parallaxGroup = new QGroupBox(tr("Mouse Orbit & Parallax Spring"), containerWidget);
    parallaxGroup->setStyleSheet(groupStyle);
    auto *parallaxLayout = new QGridLayout(parallaxGroup);
    parallaxLayout->setContentsMargins(8, 8, 8, 8);
    parallaxLayout->setSpacing(6);

    m_parallaxXSpin = new QDoubleSpinBox(parallaxGroup);
    initSpin(m_parallaxXSpin, 0.0, 200.0, 1.0, 1);
    parallaxLayout->addWidget(new QLabel(tr("Sway X:"), parallaxGroup), 0, 0);
    parallaxLayout->addWidget(m_parallaxXSpin, 0, 1);

    m_parallaxYSpin = new QDoubleSpinBox(parallaxGroup);
    initSpin(m_parallaxYSpin, 0.0, 200.0, 1.0, 1);
    parallaxLayout->addWidget(new QLabel(tr("Sway Y:"), parallaxGroup), 0, 2);
    parallaxLayout->addWidget(m_parallaxYSpin, 0, 3);

    m_parallaxSmoothingSpin = new QDoubleSpinBox(parallaxGroup);
    initSpin(m_parallaxSmoothingSpin, 0.01, 1.0, 0.05, 2);
    parallaxLayout->addWidget(new QLabel(tr("Smoothing:"), parallaxGroup), 1, 0);
    parallaxLayout->addWidget(m_parallaxSmoothingSpin, 1, 1, 1, 3);

    contentLayout->addWidget(parallaxGroup);

    // =========================================================================
    // Section 3: Circle-of-Confusion Depth of Field
    // =========================================================================
    auto *dofGroup = new QGroupBox(tr("Circle-of-Confusion Depth of Field (DOF)"), containerWidget);
    dofGroup->setStyleSheet(groupStyle);
    auto *dofLayout = new QGridLayout(dofGroup);
    dofLayout->setContentsMargins(8, 8, 8, 8);
    dofLayout->setSpacing(6);

    m_dofEnableCheck = new QCheckBox(tr("Enable Depth of Field"), dofGroup);
    dofLayout->addWidget(m_dofEnableCheck, 0, 0, 1, 4);

    m_dofFocalDistSpin = new QDoubleSpinBox(dofGroup);
    initSpin(m_dofFocalDistSpin, 0.01, 1000.0, 0.2, 2);
    dofLayout->addWidget(new QLabel(tr("Focal Distance:"), dofGroup), 1, 0);
    dofLayout->addWidget(m_dofFocalDistSpin, 1, 1);

    m_dofFocalRegionSpin = new QDoubleSpinBox(dofGroup);
    initSpin(m_dofFocalRegionSpin, 0.0, 1000.0, 0.2, 2);
    dofLayout->addWidget(new QLabel(tr("Focal Region:"), dofGroup), 1, 2);
    dofLayout->addWidget(m_dofFocalRegionSpin, 1, 3);

    m_dofBokehSpin = new QDoubleSpinBox(dofGroup);
    initSpin(m_dofBokehSpin, 0.0, 16.0, 0.5, 1, QStringLiteral(" px"));
    dofLayout->addWidget(new QLabel(tr("Bokeh Radius:"), dofGroup), 2, 0);
    dofLayout->addWidget(m_dofBokehSpin, 2, 1);

    contentLayout->addWidget(dofGroup);

    // =========================================================================
    // Section 4: Centripetal Catmull-Rom Keyframe Path
    // =========================================================================
    auto *pathGroup = new QGroupBox(tr("Centripetal Catmull-Rom Camera Path"), containerWidget);
    pathGroup->setStyleSheet(groupStyle);
    auto *pathLayout = new QVBoxLayout(pathGroup);
    pathLayout->setContentsMargins(8, 8, 8, 8);
    pathLayout->setSpacing(6);

    auto *pathHBox = new QHBoxLayout();
    m_pathEnableCheck = new QCheckBox(tr("Active Path"), pathGroup);
    m_loopModeCombo = new QComboBox(pathGroup);
    m_loopModeCombo->setMinimumHeight(28);
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
        "QListWidget { background-color: #12141A; border: 1px solid #2D3340; "
        "border-radius: 4px; color: #F1F5F9; font-family: monospace; font-size: 11px; } "
        "QListWidget:focus { border: 2px solid #38BDF8; }");
    pathLayout->addWidget(m_keyframeList);

    auto *kfParamLayout = new QHBoxLayout();
    m_kfTimeSpin = new QDoubleSpinBox(pathGroup);
    initSpin(m_kfTimeSpin, 0.0, 3600.0, 0.5, 2, QStringLiteral(" s"));

    m_kfApertureSpin = new QDoubleSpinBox(pathGroup);
    initSpin(m_kfApertureSpin, 0.7, 32.0, 0.1, 1);
    m_kfApertureSpin->setValue(2.8);

    m_kfTensionSpin = new QDoubleSpinBox(pathGroup);
    initSpin(m_kfTensionSpin, 0.0, 1.0, 0.05, 2);
    m_kfTensionSpin->setValue(0.5);

    kfParamLayout->addWidget(new QLabel(tr("Time:"), pathGroup));
    kfParamLayout->addWidget(m_kfTimeSpin);
    kfParamLayout->addWidget(new QLabel(tr("f-stop:"), pathGroup));
    kfParamLayout->addWidget(m_kfApertureSpin);
    kfParamLayout->addWidget(new QLabel(tr("Tension:"), pathGroup));
    kfParamLayout->addWidget(m_kfTensionSpin);
    pathLayout->addLayout(kfParamLayout);

    auto *kfBtnLayout = new QHBoxLayout();
    m_addKfBtn = new QPushButton(tr("Add Keyframe"), pathGroup);
    m_updateKfBtn = new QPushButton(tr("Update"), pathGroup);
    m_removeKfBtn = new QPushButton(tr("Remove"), pathGroup);
    m_clearKfBtn = new QPushButton(tr("Clear"), pathGroup);
    for (auto *btn : {m_addKfBtn, m_updateKfBtn, m_removeKfBtn, m_clearKfBtn}) {
        btn->setMinimumHeight(28);
        btn->setMinimumWidth(28);
        kfBtnLayout->addWidget(btn);
    }
    pathLayout->addLayout(kfBtnLayout);

    contentLayout->addWidget(pathGroup);

    // =========================================================================
    // Section 5: Dynamic Scene Lights (8 Max)
    // =========================================================================
    auto *lightGroup =
        new QGroupBox(tr("Multi-Light Dynamic Grading (8 Lights Max)"), containerWidget);
    lightGroup->setStyleSheet(groupStyle);
    auto *lightLayout = new QGridLayout(lightGroup);
    lightLayout->setContentsMargins(8, 8, 8, 8);
    lightLayout->setSpacing(6);

    m_lightSlotCombo = new QComboBox(lightGroup);
    m_lightSlotCombo->setMinimumHeight(28);
    for (uint32_t i = 0; i < WE_MAX_SCENE_LIGHTS; ++i) {
        m_lightSlotCombo->addItem(QStringLiteral("Light Slot %1").arg(i), i);
    }
    lightLayout->addWidget(new QLabel(tr("Slot:"), lightGroup), 0, 0);
    lightLayout->addWidget(m_lightSlotCombo, 0, 1);

    m_lightTypeCombo = new QComboBox(lightGroup);
    m_lightTypeCombo->setMinimumHeight(28);
    m_lightTypeCombo->addItem(tr("Disabled"), WE_LIGHT_DISABLED);
    m_lightTypeCombo->addItem(tr("Point"), WE_LIGHT_POINT);
    m_lightTypeCombo->addItem(tr("Spot"), WE_LIGHT_SPOT);
    m_lightTypeCombo->addItem(tr("Directional"), WE_LIGHT_DIRECTIONAL);
    m_lightTypeCombo->addItem(tr("Tube"), WE_LIGHT_TUBE);
    lightLayout->addWidget(new QLabel(tr("Type:"), lightGroup), 0, 2);
    lightLayout->addWidget(m_lightTypeCombo, 0, 3);

    m_lightColorBtn = new QPushButton(tr("Pick Color"), lightGroup);
    m_lightColorBtn->setMinimumSize(96, 28);
    lightLayout->addWidget(new QLabel(tr("Color (OkLCH):"), lightGroup), 1, 0);
    lightLayout->addWidget(m_lightColorBtn, 1, 1);

    m_lightIntensitySpin = new QDoubleSpinBox(lightGroup);
    initSpin(m_lightIntensitySpin, 0.0, 20.0, 0.2, 2);
    lightLayout->addWidget(new QLabel(tr("Intensity:"), lightGroup), 1, 2);
    lightLayout->addWidget(m_lightIntensitySpin, 1, 3);

    m_lightRadiusSpin = new QDoubleSpinBox(lightGroup);
    initSpin(m_lightRadiusSpin, 0.1, 1000.0, 1.0, 1);
    lightLayout->addWidget(new QLabel(tr("Radius:"), lightGroup), 2, 0);
    lightLayout->addWidget(m_lightRadiusSpin, 2, 1);

    m_specularPowerSpin = new QDoubleSpinBox(lightGroup);
    initSpin(m_specularPowerSpin, 1.0, 256.0, 4.0, 1);
    lightLayout->addWidget(new QLabel(tr("Shininess:"), lightGroup), 2, 2);
    lightLayout->addWidget(m_specularPowerSpin, 2, 3);

    // Pos / Dir
    lightLayout->addWidget(new QLabel(tr("Pos / Dir:"), lightGroup), 3, 0);
    auto *lposLayout = new QHBoxLayout();
    lposLayout->setSpacing(4);
    m_lightPosXSpin = new QDoubleSpinBox(lightGroup);
    m_lightPosYSpin = new QDoubleSpinBox(lightGroup);
    m_lightPosZSpin = new QDoubleSpinBox(lightGroup);
    for (auto *s : {m_lightPosXSpin, m_lightPosYSpin, m_lightPosZSpin}) {
        initSpin(s, -10000.0, 10000.0, 0.5, 2);
        lposLayout->addWidget(s);
    }
    lightLayout->addLayout(lposLayout, 3, 1, 1, 3);

    // Spot Axis Dir
    lightLayout->addWidget(new QLabel(tr("Spot Dir:"), lightGroup), 4, 0);
    auto *sdirLayout = new QHBoxLayout();
    sdirLayout->setSpacing(4);
    m_spotDirXSpin = new QDoubleSpinBox(lightGroup);
    m_spotDirYSpin = new QDoubleSpinBox(lightGroup);
    m_spotDirZSpin = new QDoubleSpinBox(lightGroup);
    for (auto *s : {m_spotDirXSpin, m_spotDirYSpin, m_spotDirZSpin}) {
        initSpin(s, -1.0, 1.0, 0.1, 2);
        sdirLayout->addWidget(s);
    }
    lightLayout->addLayout(sdirLayout, 4, 1, 1, 3);

    // Cones & Tube Length
    m_spotInnerSpin = new QDoubleSpinBox(lightGroup);
    initSpin(m_spotInnerSpin, -1.0, 1.0, 0.05, 3);
    lightLayout->addWidget(new QLabel(tr("Inner Cos:"), lightGroup), 5, 0);
    lightLayout->addWidget(m_spotInnerSpin, 5, 1);

    m_spotOuterSpin = new QDoubleSpinBox(lightGroup);
    initSpin(m_spotOuterSpin, -1.0, 1.0, 0.05, 3);
    lightLayout->addWidget(new QLabel(tr("Outer Cos:"), lightGroup), 5, 2);
    lightLayout->addWidget(m_spotOuterSpin, 5, 3);

    m_tubeLengthSpin = new QDoubleSpinBox(lightGroup);
    initSpin(m_tubeLengthSpin, 0.0, 100.0, 0.5, 2);
    lightLayout->addWidget(new QLabel(tr("Tube Length:"), lightGroup), 6, 0);
    lightLayout->addWidget(m_tubeLengthSpin, 6, 1, 1, 3);

    contentLayout->addWidget(lightGroup);

    // =========================================================================
    // Section 6: Hemisphere Ambient & Normal Map
    // =========================================================================
    auto *ambGroup = new QGroupBox(tr("Hemisphere Ambient & Normal Map Bump"), containerWidget);
    ambGroup->setStyleSheet(groupStyle);
    auto *ambLayout = new QGridLayout(ambGroup);
    ambLayout->setContentsMargins(8, 8, 8, 8);
    ambLayout->setSpacing(6);

    m_ambientColorBtn = new QPushButton(tr("Ground Ambient"), ambGroup);
    m_ambientColorBtn->setMinimumSize(96, 28);
    m_skyColorBtn = new QPushButton(tr("Sky Tint"), ambGroup);
    m_skyColorBtn->setMinimumSize(96, 28);
    ambLayout->addWidget(new QLabel(tr("Ambient (OkLCH):"), ambGroup), 0, 0);
    ambLayout->addWidget(m_ambientColorBtn, 0, 1);
    ambLayout->addWidget(new QLabel(tr("Sky Tint (OkLCH):"), ambGroup), 0, 2);
    ambLayout->addWidget(m_skyColorBtn, 0, 3);

    m_normalMapStrengthSpin = new QDoubleSpinBox(ambGroup);
    initSpin(m_normalMapStrengthSpin, 0.0, 2.0, 0.05, 2);
    ambLayout->addWidget(new QLabel(tr("Normal Map Scale:"), ambGroup), 1, 0);
    ambLayout->addWidget(m_normalMapStrengthSpin, 1, 1, 1, 3);

    contentLayout->addWidget(ambGroup);

    // Reset Button
    m_resetBtn = new QPushButton(tr("Reset Camera & Lights to Defaults"), containerWidget);
    m_resetBtn->setMinimumHeight(30);
    m_resetBtn->setStyleSheet(
        "QPushButton { background-color: #242936; color: #FCA5A5; border: 1px solid #475569; "
        "border-radius: 4px; padding: 6px 10px; font-weight: bold; font-size: 12px; } "
        "QPushButton:hover { background-color: #2E3444; } "
        "QPushButton:focus { border: 2px solid #38BDF8; }");
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
    connect(m_updateKfBtn, &QPushButton::clicked, this,
            &CameraLightingInspectorWidget::onUpdateKeyframeClicked);
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
        closeActiveColorDialog();
        we_camera3d_t newCam{};
        we_lighting_uniforms_t newLights{};
        populateDefaults(newCam, newLights);
        if (m_commandBus) {
            const we_camera3d_t oldCam = m_camera;
            const we_lighting_uniforms_t oldLights = m_lighting;
            const uint32_t oldSlot = m_selectedLightSlot;
            QPointer<CameraLightingInspectorWidget> self(this);
            auto cmd = std::make_unique<ResetCameraLightsCommand>(
                m_targetOutput, oldCam, newCam, oldLights, newLights, oldSlot, 0u,
                [self](const QString &output, const we_camera3d_t &c,
                       const we_lighting_uniforms_t &l, uint32_t slot) {
                    if (!self) {
                        return;
                    }
                    self->closeActiveColorDialog();
                    self->m_camera = c;
                    self->m_lighting = l;
                    self->m_selectedLightSlot = (slot < WE_MAX_SCENE_LIGHTS) ? slot : 0u;
                    self->syncUiFromModel();
                    self->emitStateUpdated(output, c, l);
                });
            m_commandBus->execute(std::move(cmd));
        } else {
            m_camera = newCam;
            m_lighting = newLights;
            m_selectedLightSlot = 0;
            syncUiFromModel();
            emitStateUpdated(m_targetOutput, m_camera, m_lighting);
        }
        emit statusMessage(tr("Camera & lighting reset to defaults."));
    });
}

void CameraLightingInspectorWidget::syncUiFromModel()
{
    // Preserve outer m_updatingUi when nested helper syncs return.
    const QScopedValueRollback<bool> guard(m_updatingUi, true);

    // Camera
    const int projIdx = findComboIndexByUint(m_projModeCombo, m_camera.projection_mode);
    m_projModeCombo->setCurrentIndex(projIdx >= 0 ? projIdx : 0);
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
    const int loopIdx = findComboIndexByUint(m_loopModeCombo, m_camera.path_loop_mode);
    m_loopModeCombo->setCurrentIndex(loopIdx >= 0 ? loopIdx : 0);

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
    const uint32_t safeSlot =
        (m_selectedLightSlot < WE_MAX_SCENE_LIGHTS) ? m_selectedLightSlot : 0u;
    m_selectedLightSlot = safeSlot;
    const int slotIdx = findComboIndexByUint(m_lightSlotCombo, safeSlot);
    m_lightSlotCombo->setCurrentIndex(slotIdx >= 0 ? slotIdx : 0);
    syncLightUiFromSlot(safeSlot);

    // Ambient & Normal
    m_normalMapStrengthSpin->setValue(m_lighting.normal_map_strength);
    updateColorButtonSwatches();
}

void CameraLightingInspectorWidget::syncLightUiFromSlot(uint32_t slot)
{
    if (slot >= WE_MAX_SCENE_LIGHTS) {
        return;
    }

    const auto &l = m_lighting.lights[slot];
    const QScopedValueRollback<bool> guard(m_updatingUi, true);
    const int typeIdx = findComboIndexByUint(m_lightTypeCombo, l.light_type);
    m_lightTypeCombo->setCurrentIndex(typeIdx >= 0 ? typeIdx : 0);
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

    updateColorButtonSwatches();
}

void CameraLightingInspectorWidget::updateKeyframeList()
{
    const int prevRow = m_keyframeList->currentRow();
    m_keyframeList->clear();
    const uint32_t kfCount = std::min<uint32_t>(m_camera.keyframe_count, WE_CAMERA_MAX_KEYFRAMES);
    for (uint32_t i = 0; i < kfCount; ++i) {
        const auto &kf = m_camera.keyframes[i];
        QString itemText = QStringLiteral("#%1: t=%2s  pos=(%3, %4, %5)  fov=%6 deg  f/%7  tens=%8")
                               .arg(i)
                               .arg(kf.time_sec, 0, 'f', 2)
                               .arg(kf.position[0], 0, 'f', 1)
                               .arg(kf.position[1], 0, 'f', 1)
                               .arg(kf.position[2], 0, 'f', 1)
                               .arg(kf.fov_deg, 0, 'f', 0)
                               .arg(kf.aperture_fstop, 0, 'f', 1)
                               .arg(kf.tension, 0, 'f', 2);
        m_keyframeList->addItem(itemText);
    }
    if (prevRow >= 0 && static_cast<uint32_t>(prevRow) < kfCount) {
        m_keyframeList->setCurrentRow(prevRow);
    }
}

void CameraLightingInspectorWidget::updateColorButtonSwatches()
{
    auto applyButtonColor = [](QPushButton *btn, float linR, float linG, float linB,
                               const QString &label) {
        if (!btn)
            return;
        const float clampedLinR = sanitizeFinite(linR, 0.0f, 0.0f, 1.0f);
        const float clampedLinG = sanitizeFinite(linG, 0.0f, 0.0f, 1.0f);
        const float clampedLinB = sanitizeFinite(linB, 0.0f, 0.0f, 1.0f);
        const float sR = std::clamp(ColorScience::linearToSrgb(clampedLinR), 0.0f, 1.0f);
        const float sG = std::clamp(ColorScience::linearToSrgb(clampedLinG), 0.0f, 1.0f);
        const float sB = std::clamp(ColorScience::linearToSrgb(clampedLinB), 0.0f, 1.0f);
        const int r = std::clamp(static_cast<int>(std::lround(sR * 255.0f)), 0, 255);
        const int g = std::clamp(static_cast<int>(std::lround(sG * 255.0f)), 0, 255);
        const int b = std::clamp(static_cast<int>(std::lround(sB * 255.0f)), 0, 255);

        // Linear relative luminance threshold keeps WCAG >= 4.5:1 contrast for label text.
        const float lum = 0.2126f * clampedLinR + 0.7152f * clampedLinG + 0.0722f * clampedLinB;
        const QString textCol =
            lum > 0.184f ? QStringLiteral("#0F172A") : QStringLiteral("#F1F5F9");

        btn->setText(label);
        btn->setStyleSheet(
            QStringLiteral("QPushButton { background-color: rgb(%1, %2, %3); color: %4; "
                           "border: 1px solid #334155; border-radius: 4px; padding: 4px 8px; "
                           "min-height: 28px; font-weight: bold; font-size: 12px; } "
                           "QPushButton:hover { border: 1px solid #94A3B8; } "
                           "QPushButton:focus { border: 2px solid #38BDF8; }")
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
    newCam.projection_mode =
        readComboUint(m_projModeCombo, m_camera.projection_mode, WE_PROJ_DEPTH_PARALLAX_25D);
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
    newCam.path_loop_mode =
        readComboUint(m_loopModeCombo, m_camera.path_loop_mode, WE_CAM_PATH_ONCE);

    sanitizeAndEvaluateCamera(newCam);
    commitChange(newCam, m_lighting);
}

void CameraLightingInspectorWidget::onParallaxControlChanged()
{
    if (m_updatingUi)
        return;

    we_camera3d_t newCam = m_camera;
    newCam.parallax_amount[0] =
        sanitizeFinite(static_cast<float>(m_parallaxXSpin->value()), 0.0f, 0.0f, 200.0f);
    newCam.parallax_amount[1] =
        sanitizeFinite(static_cast<float>(m_parallaxYSpin->value()), 0.0f, 0.0f, 200.0f);
    newCam.parallax_smoothing =
        sanitizeFinite(static_cast<float>(m_parallaxSmoothingSpin->value()), 0.15f, 0.01f, 1.0f);

    sanitizeAndEvaluateCamera(newCam);
    commitChange(newCam, m_lighting);
}

void CameraLightingInspectorWidget::onDofControlChanged()
{
    if (m_updatingUi)
        return;

    we_camera3d_t newCam = m_camera;
    newCam.dof_enabled = m_dofEnableCheck->isChecked() ? 1.0f : 0.0f;
    newCam.dof_focal_dist =
        sanitizeFinite(static_cast<float>(m_dofFocalDistSpin->value()), 5.0f, 0.01f, 1000.0f);
    newCam.dof_focal_region =
        sanitizeFinite(static_cast<float>(m_dofFocalRegionSpin->value()), 2.0f, 0.0f, 1000.0f);
    newCam.dof_bokeh_radius_px =
        sanitizeFinite(static_cast<float>(m_dofBokehSpin->value()), 6.0f, 0.0f, 16.0f);

    sanitizeAndEvaluateCamera(newCam);
    commitChange(newCam, m_lighting);
}

void CameraLightingInspectorWidget::onKeyframeSelectionChanged()
{
    const int row = m_keyframeList->currentRow();
    const uint32_t kfCount = std::min<uint32_t>(m_camera.keyframe_count, WE_CAMERA_MAX_KEYFRAMES);
    if (row >= 0 && static_cast<uint32_t>(row) < kfCount) {
        const QScopedValueRollback<bool> guard(m_updatingUi, true);
        const auto &kf = m_camera.keyframes[row];
        m_kfTimeSpin->setValue(kf.time_sec);
        if (m_kfApertureSpin) {
            m_kfApertureSpin->setValue(kf.aperture_fstop);
        }
        if (m_kfTensionSpin) {
            m_kfTensionSpin->setValue(kf.tension);
        }
    }
}

void CameraLightingInspectorWidget::onAddKeyframeClicked()
{
    const float timeSec = static_cast<float>(m_kfTimeSpin->value());
    addCameraKeyframe(timeSec);
}

void CameraLightingInspectorWidget::onUpdateKeyframeClicked()
{
    const int row = m_keyframeList->currentRow();
    const uint32_t kfCount = std::min<uint32_t>(m_camera.keyframe_count, WE_CAMERA_MAX_KEYFRAMES);
    if (row < 0 || static_cast<uint32_t>(row) >= kfCount) {
        emit statusMessage(QStringLiteral("Select a keyframe to update."));
        return;
    }

    const float timeSec = static_cast<float>(m_kfTimeSpin->value());
    if (!std::isfinite(timeSec) || timeSec < 0.0f) {
        return;
    }

    we_camera3d_t newCam = m_camera;
    auto &kf = newCam.keyframes[row];
    kf.time_sec = std::clamp(timeSec, 0.0f, 3600.0f);
    kf.position[0] = newCam.eye_pos[0];
    kf.position[1] = newCam.eye_pos[1];
    kf.position[2] = newCam.eye_pos[2];
    computeLookAtQuaternion(newCam.eye_pos, newCam.target_pos, newCam.up_vec, kf.orientation_quat);
    kf.fov_deg = newCam.fov_y_deg;
    kf.focal_distance = newCam.dof_focal_dist;
    kf.aperture_fstop =
        m_kfApertureSpin ? static_cast<float>(m_kfApertureSpin->value()) : kf.aperture_fstop;
    kf.tension = m_kfTensionSpin ? static_cast<float>(m_kfTensionSpin->value()) : kf.tension;

    std::stable_sort(
        newCam.keyframes, newCam.keyframes + kfCount,
        [](const we_camera_kf_t &a, const we_camera_kf_t &b) { return a.time_sec < b.time_sec; });
    sanitizeAndEvaluateCamera(newCam);
    commitChange(newCam, m_lighting);
    emit statusMessage(
        QStringLiteral("Updated camera keyframe #%1 at %2 s").arg(row).arg(kf.time_sec, 0, 'f', 2));
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
    std::memset(newCam.keyframes, 0, sizeof(newCam.keyframes));
    sanitizeAndEvaluateCamera(newCam);
    commitChange(newCam, m_lighting);
    emit statusMessage(tr("All camera keyframes cleared."));
}

void CameraLightingInspectorWidget::onLightSlotChanged(int index)
{
    if (index < 0 || !m_lightSlotCombo) {
        return;
    }
    const QVariant data = m_lightSlotCombo->itemData(index);
    bool ok = false;
    const uint val = data.toUInt(&ok);
    if (ok && val < WE_MAX_SCENE_LIGHTS) {
        setSelectedLightSlot(static_cast<uint32_t>(val));
    } else if (static_cast<uint32_t>(index) < WE_MAX_SCENE_LIGHTS) {
        setSelectedLightSlot(static_cast<uint32_t>(index));
    }
}

void CameraLightingInspectorWidget::onLightControlChanged()
{
    if (m_updatingUi || m_selectedLightSlot >= WE_MAX_SCENE_LIGHTS)
        return;

    we_lighting_uniforms_t newLights = m_lighting;
    auto &l = newLights.lights[m_selectedLightSlot];

    l.light_type = readComboUint(m_lightTypeCombo, WE_LIGHT_DISABLED, WE_LIGHT_TUBE);
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
    sanitizeLighting(newLights);

    commitChange(m_camera, newLights);
}

void CameraLightingInspectorWidget::closeActiveColorDialog()
{
    if (!m_activeColorDialog) {
        return;
    }
    QPointer<OklchColorDialog> dlg = m_activeColorDialog;
    m_activeColorDialog.clear();
    dlg->disconnect(this);
    dlg->close();
    dlg->deleteLater();
}

void CameraLightingInspectorWidget::openAsyncColorDialog(const QString &title, ColorTarget target,
                                                         uint32_t lightSlot)
{
    if (target == ColorTarget::LightSlot && lightSlot >= WE_MAX_SCENE_LIGHTS) {
        return;
    }

    closeActiveColorDialog();

    auto readTargetColor = [target, lightSlot](const we_lighting_uniforms_t &src) {
        const float *ptr = (target == ColorTarget::LightSlot) ? src.lights[lightSlot].color_rgb
                           : (target == ColorTarget::Ambient) ? src.ambient_color_rgb
                                                              : src.sky_color_rgb;
        return std::array<float, 3>{
            sanitizeFinite(ptr[0], 1.0f, 0.0f, 1.0f),
            sanitizeFinite(ptr[1], 1.0f, 0.0f, 1.0f),
            sanitizeFinite(ptr[2], 1.0f, 0.0f, 1.0f),
        };
    };

    auto applyTargetColor = [target, lightSlot](we_lighting_uniforms_t &dst,
                                                const std::array<float, 3> &rgb) {
        float *ptr = (target == ColorTarget::LightSlot) ? dst.lights[lightSlot].color_rgb
                     : (target == ColorTarget::Ambient) ? dst.ambient_color_rgb
                                                        : dst.sky_color_rgb;
        ptr[0] = rgb[0];
        ptr[1] = rgb[1];
        ptr[2] = rgb[2];
    };

    const std::array<float, 3> initialRgb = readTargetColor(m_lighting);
    const float sR = std::clamp(ColorScience::linearToSrgb(initialRgb[0]), 0.0f, 1.0f);
    const float sG = std::clamp(ColorScience::linearToSrgb(initialRgb[1]), 0.0f, 1.0f);
    const float sB = std::clamp(ColorScience::linearToSrgb(initialRgb[2]), 0.0f, 1.0f);
    const QColor initialCol = QColor::fromRgbF(sR, sG, sB);

    auto *dlg = new OklchColorDialog(initialCol, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose, true);
    dlg->setModal(false);
    dlg->setWindowTitle(title);
    m_activeColorDialog = dlg;

    QPointer<CameraLightingInspectorWidget> self(this);
    QPointer<OklchColorDialog> dlgPtr(dlg);

    auto readClampedLinear = [initialRgb](const OklchColorDialog *d) {
        float r = initialRgb[0];
        float g = initialRgb[1];
        float b = initialRgb[2];
        d->getLinearRgb(r, g, b);
        return std::array<float, 3>{
            sanitizeFinite(r, initialRgb[0], 0.0f, 1.0f),
            sanitizeFinite(g, initialRgb[1], 0.0f, 1.0f),
            sanitizeFinite(b, initialRgb[2], 0.0f, 1.0f),
        };
    };

    connect(dlg, &OklchColorDialog::colorChanged, this,
            [self, dlgPtr, applyTargetColor, readClampedLinear](const QColor &) {
                if (!self || !dlgPtr || self->m_activeColorDialog != dlgPtr) {
                    return;
                }
                const std::array<float, 3> previewRgb = readClampedLinear(dlgPtr);
                applyTargetColor(self->m_lighting, previewRgb);
                self->updateColorButtonSwatches();
                self->emitStateUpdated(self->m_targetOutput, self->m_camera, self->m_lighting);
            });

    connect(dlg, &QDialog::accepted, this,
            [self, dlgPtr, initialRgb, applyTargetColor, readClampedLinear]() {
                if (!self || !dlgPtr) {
                    return;
                }
                const std::array<float, 3> acceptedRgb = readClampedLinear(dlgPtr);
                self->m_activeColorDialog.clear();
                // Restore initialRgb before commitChange so undo captures pre-dialog color.
                we_lighting_uniforms_t newLights = self->m_lighting;
                applyTargetColor(self->m_lighting, initialRgb);
                applyTargetColor(newLights, acceptedRgb);
                self->commitChange(self->m_camera, newLights);
            });

    connect(dlg, &QDialog::rejected, this, [self, dlgPtr, initialRgb, applyTargetColor]() {
        if (!self) {
            return;
        }
        if (self->m_activeColorDialog == dlgPtr) {
            self->m_activeColorDialog.clear();
        }
        applyTargetColor(self->m_lighting, initialRgb);
        self->updateColorButtonSwatches();
        self->emitStateUpdated(self->m_targetOutput, self->m_camera, self->m_lighting);
    });

    dlg->show();
}

void CameraLightingInspectorWidget::onPickLightColor()
{
    openAsyncColorDialog(QStringLiteral("Light %1 Perceptual OkLCH Color").arg(m_selectedLightSlot),
                         ColorTarget::LightSlot, m_selectedLightSlot);
}

void CameraLightingInspectorWidget::onPickAmbientColor()
{
    openAsyncColorDialog(tr("Ground Hemisphere Ambient Color (OkLCH)"), ColorTarget::Ambient, 0);
}

void CameraLightingInspectorWidget::onPickSkyColor()
{
    openAsyncColorDialog(tr("Upper Hemisphere Sky Color (OkLCH)"), ColorTarget::Sky, 0);
}

} // namespace waywal::studio
