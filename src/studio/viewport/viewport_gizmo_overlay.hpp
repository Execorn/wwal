// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_VIEWPORT_VIEWPORT_GIZMO_OVERLAY_HPP
#define WAYWAL_STUDIO_VIEWPORT_VIEWPORT_GIZMO_OVERLAY_HPP

#include "studio/core/command_bus_authoring.hpp"
#include "studio/scene/scene_layer_tree_widget.hpp"
#include "waywal/we_camera3d.h"
#include "waywal/we_mesh_cdt.h"
#include "waywal/we_mesh_rig.h"
#include "waywal/we_puppet.h"

#include <QMatrix4x4>
#include <QPointF>
#include <QSize>
#include <QVector3D>

#include <array>
#include <cstdint>
#include <limits>
#include <utility>
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
    ContourPin = 8,
    BonePlace = 9,
    IkDrag = 10,
    WeightBrush = 11,
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
    BoneJoint = 10,
    BoneBody = 11,
    BonePivotRotate = 12,
    IkTarget = 13,
    IkPole = 14,
    ContourPinHandle = 15,
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

struct GizmoMeshWireframeData {
    bool visible{false};
    bool highlightContourEdges{true};
    std::vector<we_mdl_vertex_t> vertices;
    std::vector<uint16_t> indices;
    std::vector<we_cdt_pin_t> pins;
    int32_t selectedPinIndex{-1};
};

struct GizmoBoneNode {
    uint32_t index{0};
    uint32_t parentIndex{0xFFFFFFFFu};
    QVector3D headWorld{0.0f, 0.0f, 0.0f};
    QVector3D tailWorld{32.0f, 0.0f, 0.0f};
    std::array<float, 3> localPos{0.0f, 0.0f, 0.0f};
    float localAngleRad{0.0f};
    we_bone_drive_mode_t driveMode{WE_BONE_DRIVE_FK};
    bool selected{false};
    bool locked{false};
};

struct GizmoIkHandleData {
    int32_t chainIndex{0};
    we_ik_chain_t chain{};
    QVector3D rootWorld{0.0f, 0.0f, 0.0f};
    QVector3D midWorld{0.0f, 0.0f, 0.0f};
    QVector3D endWorld{0.0f, 0.0f, 0.0f};
    QVector3D targetWorld{0.0f, 0.0f, 0.0f};
    QVector3D poleWorld{0.0f, 0.0f, 0.0f};
};

struct GizmoOnionGhostFrame {
    int32_t frameDelta{0};
    float opacity{0.35f};
    std::vector<std::pair<QVector3D, QVector3D>> boneSegments;
};

struct GizmoWeightBrushState {
    bool heatmapEnabled{false};
    we_heatmap_palette_t palette{WE_HEATMAP_PALETTE_TURBO};
    float heatmapOpacity{0.75f};
    uint32_t activeBoneIndex{0};
    QPointF cursorWorldPos{0.0, 0.0};
    bool cursorInside{false};
    float radiusPx{32.0f};
    float strength{0.5f};
    we_weight_brush_op_t op{WE_WEIGHT_OP_ADD};
};

struct RigPickResult {
    GizmoAxisHandle handle{GizmoAxisHandle::None};
    int32_t index{-1};
    float distancePx{std::numeric_limits<float>::max()};
};

class ViewportGizmoOverlay {
  public:
    static constexpr float kHandleScreenRadiusPx = 88.0f;
    static constexpr float kPickTolerancePx = 8.0f;
    static constexpr size_t kMaxDynamicVertices = 262144u;

    ViewportGizmoOverlay() = default;
    ~ViewportGizmoOverlay() = default;

    void setToolMode(ViewportToolMode mode) noexcept { m_toolMode = mode; }
    [[nodiscard]] ViewportToolMode toolMode() const noexcept { return m_toolMode; }

    void setSnapConfig(const ViewportGizmoSnapConfig &cfg) noexcept { m_snap = cfg; }
    [[nodiscard]] const ViewportGizmoSnapConfig &snapConfig() const noexcept { return m_snap; }

    void setActiveNode(const SceneLayerNode *node);
    void setCameraAndLights(const we_camera3d_t &cam, const we_lighting_uniforms_t &lights);
    void setEmitterWireframes(std::vector<GizmoEmitterWireframe> emitters);

    // Content canvas dimensions for mapping centered Y-up puppet coords & top-down texels
    void setContentSize(uint32_t width, uint32_t height) noexcept
    {
        m_contentWidth = width;
        m_contentHeight = height;
    }
    [[nodiscard]] uint32_t contentWidth() const noexcept { return m_contentWidth; }
    [[nodiscard]] uint32_t contentHeight() const noexcept { return m_contentHeight; }

    // Rigging & Animation Overlay Data
    void setMeshWireframe(bool visible, std::vector<we_mdl_vertex_t> vertices,
                          std::vector<uint16_t> indices);
    void setMeshWireframeVisible(bool visible) noexcept;
    void setMeshPins(std::vector<we_cdt_pin_t> pins, int32_t selectedPin = -1);
    [[nodiscard]] const GizmoMeshWireframeData &meshWireframe() const noexcept
    {
        return m_meshWireframe;
    }
    [[nodiscard]] GizmoMeshWireframeData &mutableMeshWireframe() noexcept
    {
        return m_meshWireframe;
    }

    void setPuppetBones(std::vector<GizmoBoneNode> bones, int32_t selectedBoneIndex = -1);
    void setSelectedBoneIndex(int32_t boneIndex) noexcept;
    [[nodiscard]] int32_t selectedBoneIndex() const noexcept { return m_selectedBoneIndex; }
    [[nodiscard]] const std::vector<GizmoBoneNode> &puppetBones() const noexcept { return m_bones; }
    [[nodiscard]] std::vector<GizmoBoneNode> &mutablePuppetBones() noexcept { return m_bones; }

    void setIkHandles(std::vector<GizmoIkHandleData> chains);
    [[nodiscard]] const std::vector<GizmoIkHandleData> &ikHandles() const noexcept
    {
        return m_ikHandles;
    }
    [[nodiscard]] std::vector<GizmoIkHandleData> &mutableIkHandles() noexcept
    {
        return m_ikHandles;
    }

    void setOnionSkinEnabled(bool enabled) noexcept { m_onionSkinEnabled = enabled; }
    [[nodiscard]] bool isOnionSkinEnabled() const noexcept { return m_onionSkinEnabled; }
    void setOnionSkinFrames(std::vector<GizmoOnionGhostFrame> frames);

    void setWeightBrushState(const GizmoWeightBrushState &state) noexcept { m_weightBrush = state; }
    [[nodiscard]] const GizmoWeightBrushState &weightBrushState() const noexcept
    {
        return m_weightBrush;
    }
    void setWeightHeatmapEnabled(bool enabled, uint32_t activeBoneIdx, float opacity,
                                 we_heatmap_palette_t palette) noexcept;
    void setWeightBrushCursor(const QPointF &worldPos, bool inside) noexcept;

    void setBoneCreatePreview(bool active, const QVector3D &headWorld,
                              const QVector3D &tailWorld) noexcept;
    [[nodiscard]] bool hasVisibleRiggingOverlay() const noexcept;

    /* Constant screen-space handle scale clamped to [0.05, 5000.0] */
    [[nodiscard]] float computeWorldGizmoScale(const QVector3D &gizmoPos, const QVector3D &camEye,
                                               float fovYDeg, int viewportHeightPx, float orthoZoom,
                                               bool perspectiveMode) const noexcept;

    /* Zero-readback analytical ray-segment / ray-ring picking (< 15 us) */
    [[nodiscard]] GizmoAxisHandle pickHandle(const QPointF &widgetPos, const QSize &viewportSize,
                                             const QMatrix4x4 &viewProj) const noexcept;
    [[nodiscard]] RigPickResult pickRiggingHandle(const QPointF &widgetPos,
                                                  const QSize &viewportSize,
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
    /* Populates colored GL_TRIANGLES vertex array for perceptual weight heatmap overlay */
    void buildHeatmapTriangleVertices(std::vector<GizmoVertex> &outTriVerts) const;

    /* Perceptual colormap evaluation (0 = Turbo, 1 = Viridis, 2 = Inferno) */
    static void evaluateHeatmapColor(we_heatmap_palette_t palette, float weight, float &outR,
                                     float &outG, float &outB) noexcept;

  private:
    [[nodiscard]] QVector3D texelToCanvasWorld(double px, double py,
                                               const QSize &viewportSize) const noexcept;
    [[nodiscard]] QVector3D puppetLocalToCanvasWorld(const QVector3D &localPos,
                                                     const QSize &viewportSize) const noexcept;

    ViewportToolMode m_toolMode{ViewportToolMode::Navigate};
    ViewportGizmoSnapConfig m_snap;
    bool m_hasActiveNode{false};
    SceneLayerNode m_activeNode;
    we_camera3d_t m_camera{};
    we_lighting_uniforms_t m_lights{};
    std::vector<GizmoEmitterWireframe> m_emitters;
    uint32_t m_contentWidth{0};
    uint32_t m_contentHeight{0};

    bool m_dragging{false};
    GizmoAxisHandle m_activeHandle{GizmoAxisHandle::None};
    QPointF m_dragStartWidgetPos;
    SetLayerTransformCommand::TransformState m_dragInitialTransform;

    GizmoMeshWireframeData m_meshWireframe;
    std::vector<GizmoBoneNode> m_bones;
    int32_t m_selectedBoneIndex{-1};
    std::vector<GizmoIkHandleData> m_ikHandles;
    bool m_onionSkinEnabled{false};
    std::vector<GizmoOnionGhostFrame> m_onionFrames;
    GizmoWeightBrushState m_weightBrush;
    bool m_boneCreatePreviewActive{false};
    QVector3D m_boneCreateHead{0.0f, 0.0f, 0.0f};
    QVector3D m_boneCreateTail{0.0f, 0.0f, 0.0f};
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_VIEWPORT_VIEWPORT_GIZMO_OVERLAY_HPP
