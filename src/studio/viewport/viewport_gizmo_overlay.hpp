// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_VIEWPORT_VIEWPORT_GIZMO_OVERLAY_HPP
#define WAYWAL_STUDIO_VIEWPORT_VIEWPORT_GIZMO_OVERLAY_HPP

#include "studio/core/command_bus_authoring.hpp"
#include "studio/scene/scene_layer_tree_widget.hpp"
#include "waywal/we_camera3d.h"

#include <QMatrix4x4>
#include <QPointF>
#include <QSize>
#include <QVector3D>

#include <array>
#include <cstdint>
#include <vector>

namespace waywal::studio {

enum class ViewportToolMode : uint8_t {
    Navigate = 0,
    SelectBBox = 1,
    Translate = 2,
    Rotate = 3,
    Scale = 4,
    BrushMask = 5,
    BrushFlow = 6,
    BrushBlurSmudge = 7,
};

enum class GizmoAxisHandle : uint8_t {
    None = 0,
    AxisX = 1,
    AxisY = 2,
    AxisZ = 3,
    PlaneXY = 4,
    PlaneXZ = 5,
    PlaneYZ = 6,
    RotateZ = 7,
    BBoxCornerMin = 8,
    BBoxCornerMax = 9,
};

struct ViewportGizmoSnapConfig {
    bool enabled{false};
    float gridStepPx{16.0f};   /* 4, 8, 16, 32, or 64 px */
    float angleStepDeg{15.0f}; /* 5, 15, or 45 deg */
    float scaleStep{0.10f};
    bool lockAspectRatio{false};
};

struct GizmoEmitterWireframe {
    int32_t objectId{-1};
    uint32_t emitterType{0}; /* 0=BoxRandom, 1=SphereRandom, 2=Point */
    std::array<float, 3> origin{0.0f, 0.0f, 0.0f};
    std::array<float, 3> distMin{0.0f, 0.0f, 0.0f};
    std::array<float, 3> distMax{0.0f, 0.0f, 0.0f};
    std::array<float, 3> directions{0.0f, 1.0f, 0.0f};
};

struct GizmoVertex {
    float x;
    float y;
    float z;
    float r;
    float g;
    float b;
    float a;
};

class ViewportGizmoOverlay {
  public:
    static constexpr float kHandleScreenRadiusPx = 88.0f;
    static constexpr float kPickTolerancePx = 8.0f;
    static constexpr size_t kMaxDynamicVertices = 1024u;

    ViewportGizmoOverlay() = default;
    ~ViewportGizmoOverlay() = default;

    void setToolMode(ViewportToolMode mode) noexcept { m_toolMode = mode; }
    [[nodiscard]] ViewportToolMode toolMode() const noexcept { return m_toolMode; }

    void setSnapConfig(const ViewportGizmoSnapConfig &cfg) noexcept { m_snap = cfg; }
    [[nodiscard]] const ViewportGizmoSnapConfig &snapConfig() const noexcept { return m_snap; }

    void setActiveNode(const SceneLayerNode *node);
    void setCameraAndLights(const we_camera3d_t &cam, const we_lighting_uniforms_t &lights);
    void setEmitterWireframes(std::vector<GizmoEmitterWireframe> emitters);

    /* Constant screen-space handle scale clamped to [0.05, 5000.0] */
    [[nodiscard]] float computeWorldGizmoScale(const QVector3D &gizmoPos, const QVector3D &camEye,
                                               float fovYDeg, int viewportHeightPx, float orthoZoom,
                                               bool perspectiveMode) const noexcept;

    /* Zero-readback analytical ray-segment / ray-ring picking (< 15 us) */
    [[nodiscard]] GizmoAxisHandle pickHandle(const QPointF &widgetPos, const QSize &viewportSize,
                                             const QMatrix4x4 &viewProj) const noexcept;

    /* Drift-free drag evaluation relative to initialTransform captured on mousePress */
    bool beginDrag(const QPointF &widgetPos, const QSize &viewportSize, const QMatrix4x4 &viewProj);
    [[nodiscard]] SetLayerTransformCommand::TransformState
    updateDrag(const QPointF &widgetPos, const QSize &viewportSize, const QMatrix4x4 &viewProj,
               bool ctrlSnapHeld, bool shiftAspectHeld) const noexcept;
    void endDrag() noexcept;
    [[nodiscard]] bool isDragging() const noexcept { return m_dragging; }
    [[nodiscard]] const SetLayerTransformCommand::TransformState &
    dragInitialTransform() const noexcept
    {
        return m_dragInitialTransform;
    }
    [[nodiscard]] const we_camera3d_t &camera() const noexcept { return m_camera; }

    /* Populates colored GL_LINES vertex array for GLES 3.1 m_gizmoProgram */
    void buildRenderVertices(const QSize &viewportSize, const QMatrix4x4 &viewProj,
                             const QVector3D &camEye, float fovYDeg, float orthoZoom,
                             bool perspectiveMode, std::vector<GizmoVertex> &outLineVerts) const;

  private:
    ViewportToolMode m_toolMode{ViewportToolMode::Navigate};
    ViewportGizmoSnapConfig m_snap;
    bool m_hasActiveNode{false};
    SceneLayerNode m_activeNode;
    we_camera3d_t m_camera{};
    we_lighting_uniforms_t m_lights{};
    std::vector<GizmoEmitterWireframe> m_emitters;

    bool m_dragging{false};
    GizmoAxisHandle m_activeHandle{GizmoAxisHandle::None};
    QPointF m_dragStartWidgetPos;
    SetLayerTransformCommand::TransformState m_dragInitialTransform;
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_VIEWPORT_VIEWPORT_GIZMO_OVERLAY_HPP
