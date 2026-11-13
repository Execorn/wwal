// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/viewport/viewport_gizmo_overlay.hpp"

#include <QVector4D>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace waywal::studio {

namespace {

constexpr float kTwoPi = 6.28318530718f;

struct ProjectedPoint {
    bool valid{false};
    QPointF screenPos;
};

[[nodiscard]] ProjectedPoint projectWorldToWidget(const QVector3D &worldPos,
                                                  const QSize &viewportSize,
                                                  const QMatrix4x4 &viewProj) noexcept
{
    if (viewportSize.width() <= 0 || viewportSize.height() <= 0) {
        return {};
    }
    const QVector4D clip = viewProj.map(QVector4D(worldPos, 1.0f));
    if (std::abs(clip.w()) < 1e-6f) {
        return {};
    }
    const float invW = 1.0f / clip.w();
    const float ndcX = clip.x() * invW;
    const float ndcY = clip.y() * invW;
    const float sx = (ndcX * 0.5f + 0.5f) * static_cast<float>(viewportSize.width());
    const float sy = (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(viewportSize.height());
    return {true, QPointF(sx, sy)};
}

[[nodiscard]] bool unprojectWidgetRay(const QPointF &widgetPos, const QSize &viewportSize,
                                      const QMatrix4x4 &invViewProj, QVector3D &outOrigin,
                                      QVector3D &outDir) noexcept
{
    if (viewportSize.width() <= 0 || viewportSize.height() <= 0) {
        return false;
    }
    const float ndcX =
        2.0f * (static_cast<float>(widgetPos.x()) / static_cast<float>(viewportSize.width())) -
        1.0f;
    const float ndcY = 1.0f - 2.0f * (static_cast<float>(widgetPos.y()) /
                                      static_cast<float>(viewportSize.height()));

    const QVector4D nearH = invViewProj.map(QVector4D(ndcX, ndcY, -1.0f, 1.0f));
    const QVector4D farH = invViewProj.map(QVector4D(ndcX, ndcY, 1.0f, 1.0f));
    if (std::abs(nearH.w()) < 1e-6f || std::abs(farH.w()) < 1e-6f) {
        return false;
    }

    outOrigin = nearH.toVector3DAffine();
    const QVector3D farPt = farH.toVector3DAffine();
    const QVector3D dir = farPt - outOrigin;
    if (dir.lengthSquared() < 1e-9f) {
        return false;
    }
    outDir = dir.normalized();
    return true;
}

[[nodiscard]] bool intersectRayPlane(const QVector3D &rayOrigin, const QVector3D &rayDir,
                                     const QVector3D &planePoint, const QVector3D &planeNormal,
                                     QVector3D &outHit) noexcept
{
    const float denom = QVector3D::dotProduct(rayDir, planeNormal);
    if (std::abs(denom) < 1e-5f) {
        return false;
    }
    const float t = QVector3D::dotProduct(planePoint - rayOrigin, planeNormal) / denom;
    outHit = rayOrigin + rayDir * t;
    return std::isfinite(outHit.x()) && std::isfinite(outHit.y()) && std::isfinite(outHit.z());
}

[[nodiscard]] float distancePointToSegment2D(const QPointF &p, const QPointF &a,
                                             const QPointF &b) noexcept
{
    const float abx = static_cast<float>(b.x() - a.x());
    const float aby = static_cast<float>(b.y() - a.y());
    const float apx = static_cast<float>(p.x() - a.x());
    const float apy = static_cast<float>(p.y() - a.y());
    const float lenSq = abx * abx + aby * aby;
    if (lenSq < 1e-6f) {
        return std::hypot(apx, apy);
    }
    const float t = std::clamp((apx * abx + apy * aby) / lenSq, 0.0f, 1.0f);
    const float dx = apx - t * abx;
    const float dy = apy - t * aby;
    return std::hypot(dx, dy);
}

[[nodiscard]] float distance2D(const QPointF &a, const QPointF &b) noexcept
{
    return std::hypot(static_cast<float>(a.x() - b.x()), static_cast<float>(a.y() - b.y()));
}

[[nodiscard]] float pointInQuadDistance2D(const QPointF &p, const QPointF &q0, const QPointF &q1,
                                          const QPointF &q2, const QPointF &q3) noexcept
{
    const std::array<QPointF, 4> poly{q0, q1, q2, q3};
    bool inside = false;
    for (size_t i = 0, j = 3; i < 4; j = i++) {
        const bool ySpan = (poly[i].y() > p.y()) != (poly[j].y() > p.y());
        if (ySpan) {
            const double xIntersect =
                (poly[j].x() - poly[i].x()) * (p.y() - poly[i].y()) / (poly[j].y() - poly[i].y()) +
                poly[i].x();
            if (p.x() < xIntersect) {
                inside = !inside;
            }
        }
    }
    if (inside) {
        return 0.0f;
    }
    float best = distancePointToSegment2D(p, q0, q1);
    best = std::min(best, distancePointToSegment2D(p, q1, q2));
    best = std::min(best, distancePointToSegment2D(p, q2, q3));
    best = std::min(best, distancePointToSegment2D(p, q3, q0));
    return best;
}

[[nodiscard]] float snapValue(float val, float step) noexcept
{
    if (step <= 1e-5f) {
        return val;
    }
    return std::round(val / step) * step;
}

[[nodiscard]] float estimateWorldScaleFromViewProj(const QVector3D &origin,
                                                   const QSize &viewportSize,
                                                   const QMatrix4x4 &viewProj) noexcept
{
    const ProjectedPoint p0 = projectWorldToWidget(origin, viewportSize, viewProj);
    if (!p0.valid) {
        return 1.0f;
    }
    const ProjectedPoint px =
        projectWorldToWidget(origin + QVector3D(1.0f, 0.0f, 0.0f), viewportSize, viewProj);
    const ProjectedPoint py =
        projectWorldToWidget(origin + QVector3D(0.0f, 1.0f, 0.0f), viewportSize, viewProj);
    const ProjectedPoint pz =
        projectWorldToWidget(origin + QVector3D(0.0f, 0.0f, 1.0f), viewportSize, viewProj);

    float pxPerWorld = 0.0f;
    if (px.valid) {
        pxPerWorld = std::max(pxPerWorld, distance2D(p0.screenPos, px.screenPos));
    }
    if (py.valid) {
        pxPerWorld = std::max(pxPerWorld, distance2D(p0.screenPos, py.screenPos));
    }
    if (pz.valid) {
        pxPerWorld = std::max(pxPerWorld, distance2D(p0.screenPos, pz.screenPos));
    }
    if (pxPerWorld < 1e-4f) {
        return 1.0f;
    }
    return std::clamp(ViewportGizmoOverlay::kHandleScreenRadiusPx / pxPerWorld, 0.05f, 5000.0f);
}

void pushLine(std::vector<GizmoVertex> &out, const QVector3D &a, const QVector3D &b, float r,
              float g, float bl, float al)
{
    if (out.size() + 2u > ViewportGizmoOverlay::kMaxDynamicVertices) {
        return;
    }
    out.push_back({a.x(), a.y(), a.z(), r, g, bl, al});
    out.push_back({b.x(), b.y(), b.z(), r, g, bl, al});
}

void pushCircle(std::vector<GizmoVertex> &out, const QVector3D &center, const QVector3D &uAxis,
                const QVector3D &vAxis, float radius, int segments, float r, float g, float b,
                float a)
{
    if (segments < 4 || radius <= 1e-4f) {
        return;
    }
    QVector3D prev = center + uAxis * radius;
    for (int i = 1; i <= segments; ++i) {
        const float theta = kTwoPi * static_cast<float>(i) / static_cast<float>(segments);
        const QVector3D next =
            center + (uAxis * std::cos(theta) + vAxis * std::sin(theta)) * radius;
        pushLine(out, prev, next, r, g, b, a);
        prev = next;
    }
}

void pushBoxWireframe(std::vector<GizmoVertex> &out, const QVector3D &minPt, const QVector3D &maxPt,
                      float r, float g, float b, float a)
{
    const QVector3D c000(minPt.x(), minPt.y(), minPt.z());
    const QVector3D c100(maxPt.x(), minPt.y(), minPt.z());
    const QVector3D c110(maxPt.x(), maxPt.y(), minPt.z());
    const QVector3D c010(minPt.x(), maxPt.y(), minPt.z());
    const QVector3D c001(minPt.x(), minPt.y(), maxPt.z());
    const QVector3D c101(maxPt.x(), minPt.y(), maxPt.z());
    const QVector3D c111(maxPt.x(), maxPt.y(), maxPt.z());
    const QVector3D c011(minPt.x(), maxPt.y(), maxPt.z());

    pushLine(out, c000, c100, r, g, b, a);
    pushLine(out, c100, c110, r, g, b, a);
    pushLine(out, c110, c010, r, g, b, a);
    pushLine(out, c010, c000, r, g, b, a);

    pushLine(out, c001, c101, r, g, b, a);
    pushLine(out, c101, c111, r, g, b, a);
    pushLine(out, c111, c011, r, g, b, a);
    pushLine(out, c011, c001, r, g, b, a);

    pushLine(out, c000, c001, r, g, b, a);
    pushLine(out, c100, c101, r, g, b, a);
    pushLine(out, c110, c111, r, g, b, a);
    pushLine(out, c010, c011, r, g, b, a);
}

void activeNodeBBoxCorners(const SceneLayerNode &node, QVector3D &outMin,
                           QVector3D &outMax) noexcept
{
    const float halfW = 0.5f * std::abs(node.size[0] * node.scale[0]);
    const float halfH = 0.5f * std::abs(node.size[1] * node.scale[1]);
    const QVector3D center(node.origin[0], node.origin[1], node.origin[2]);
    outMin = QVector3D(center.x() - halfW, center.y() - halfH, center.z());
    outMax = QVector3D(center.x() + halfW, center.y() + halfH, center.z());
}

void pushOctoPrismBone(std::vector<GizmoVertex> &out, const QVector3D &head, const QVector3D &tail,
                       float r, float g, float b, float a)
{
    const QVector3D delta = tail - head;
    const float len = delta.length();
    if (len < 1e-3f) {
        return;
    }
    const QVector3D dir = delta / len;
    QVector3D perp1(-dir.y(), dir.x(), 0.0f);
    if (perp1.lengthSquared() < 1e-5f) {
        perp1 = QVector3D::crossProduct(dir, QVector3D(0.0f, 1.0f, 0.0f));
    }
    if (perp1.lengthSquared() < 1e-5f) {
        perp1 = QVector3D::crossProduct(dir, QVector3D(1.0f, 0.0f, 0.0f));
    }
    perp1.normalize();
    const QVector3D perp2 = QVector3D::crossProduct(dir, perp1).normalized();

    const float midDist = 0.20f * len;
    const float radius = std::clamp(0.085f * len, 3.0f, 16.0f);
    const QVector3D midCenter = head + dir * midDist;
    const std::array<QVector3D, 4> ring{{
        midCenter + perp1 * radius,
        midCenter + perp2 * radius,
        midCenter - perp1 * radius,
        midCenter - perp2 * radius,
    }};

    for (size_t i = 0; i < 4; ++i) {
        const QVector3D &curr = ring[i];
        const QVector3D &next = ring[(i + 1) % 4];
        pushLine(out, head, curr, r, g, b, a);
        pushLine(out, curr, next, r, g, b, a);
        pushLine(out, curr, tail, r, g, b, a);
    }
}

void pushDiamond2D(std::vector<GizmoVertex> &out, const QVector3D &center, float radius, float r,
                   float g, float b, float a)
{
    const QVector3D n = center + QVector3D(0.0f, radius, 0.0f);
    const QVector3D s = center + QVector3D(0.0f, -radius, 0.0f);
    const QVector3D e = center + QVector3D(radius, 0.0f, 0.0f);
    const QVector3D w = center + QVector3D(-radius, 0.0f, 0.0f);
    pushLine(out, n, e, r, g, b, a);
    pushLine(out, e, s, r, g, b, a);
    pushLine(out, s, w, r, g, b, a);
    pushLine(out, w, n, r, g, b, a);
}

void pushReticle2D(std::vector<GizmoVertex> &out, const QVector3D &center, float radius, float r,
                   float g, float b, float a)
{
    pushCircle(out, center, QVector3D(1.0f, 0.0f, 0.0f), QVector3D(0.0f, 1.0f, 0.0f), radius, 16, r,
               g, b, a);
    const float ext = radius * 1.35f;
    pushLine(out, center - QVector3D(ext, 0.0f, 0.0f), center + QVector3D(ext, 0.0f, 0.0f), r, g, b,
             a);
    pushLine(out, center - QVector3D(0.0f, ext, 0.0f), center + QVector3D(0.0f, ext, 0.0f), r, g, b,
             a);
}

[[nodiscard]] float evalBoneWeightForVertex(const we_mdl_vertex_t &v, uint32_t boneIdx) noexcept
{
    float total = 0.0f;
    for (int k = 0; k < 4; ++k) {
        if (v.blend_indices[k] == boneIdx && std::isfinite(v.weight[k])) {
            total += v.weight[k];
        }
    }
    return std::clamp(total, 0.0f, 1.0f);
}

} // namespace

void ViewportGizmoOverlay::setActiveNode(const SceneLayerNode *node)
{
    if (!node) {
        m_hasActiveNode = false;
        m_dragging = false;
        m_activeHandle = GizmoAxisHandle::None;
        return;
    }
    m_hasActiveNode = true;
    m_activeNode = *node;
}

void ViewportGizmoOverlay::setCameraAndLights(const we_camera3d_t &cam,
                                              const we_lighting_uniforms_t &lights)
{
    m_camera = cam;
    m_lights = lights;
}

void ViewportGizmoOverlay::setEmitterWireframes(std::vector<GizmoEmitterWireframe> emitters)
{
    m_emitters = std::move(emitters);
}

void ViewportGizmoOverlay::setMeshWireframe(bool visible, std::vector<we_mdl_vertex_t> vertices,
                                            std::vector<uint16_t> indices)
{
    m_meshWireframe.visible = visible;
    m_meshWireframe.vertices = std::move(vertices);
    m_meshWireframe.indices = std::move(indices);
}

void ViewportGizmoOverlay::setMeshWireframeVisible(bool visible) noexcept
{
    m_meshWireframe.visible = visible;
}

void ViewportGizmoOverlay::setMeshPins(std::vector<we_cdt_pin_t> pins, int32_t selectedPin)
{
    m_meshWireframe.pins = std::move(pins);
    m_meshWireframe.selectedPinIndex = selectedPin;
}

void ViewportGizmoOverlay::setPuppetBones(std::vector<GizmoBoneNode> bones,
                                          int32_t selectedBoneIndex)
{
    m_bones = std::move(bones);
    setSelectedBoneIndex(selectedBoneIndex);
}

void ViewportGizmoOverlay::setSelectedBoneIndex(int32_t boneIndex) noexcept
{
    m_selectedBoneIndex = boneIndex;
    for (size_t i = 0; i < m_bones.size(); ++i) {
        m_bones[i].selected = (static_cast<int32_t>(i) == boneIndex);
    }
}

void ViewportGizmoOverlay::setIkHandles(std::vector<GizmoIkHandleData> chains)
{
    m_ikHandles = std::move(chains);
}

void ViewportGizmoOverlay::setOnionSkinFrames(std::vector<GizmoOnionGhostFrame> frames)
{
    m_onionFrames = std::move(frames);
}

void ViewportGizmoOverlay::setWeightHeatmapEnabled(bool enabled, uint32_t activeBoneIdx,
                                                   float opacity,
                                                   we_heatmap_palette_t palette) noexcept
{
    m_weightBrush.heatmapEnabled = enabled;
    m_weightBrush.activeBoneIndex = activeBoneIdx;
    m_weightBrush.heatmapOpacity = std::clamp(opacity, 0.0f, 1.0f);
    m_weightBrush.palette = palette;
}

void ViewportGizmoOverlay::setWeightBrushCursor(const QPointF &worldPos, bool inside) noexcept
{
    m_weightBrush.cursorWorldPos = worldPos;
    m_weightBrush.cursorInside = inside;
}

void ViewportGizmoOverlay::setBoneCreatePreview(bool active, const QVector3D &headWorld,
                                                const QVector3D &tailWorld) noexcept
{
    m_boneCreatePreviewActive = active;
    m_boneCreateHead = headWorld;
    m_boneCreateTail = tailWorld;
}

bool ViewportGizmoOverlay::hasVisibleRiggingOverlay() const noexcept
{
    if (m_toolMode >= ViewportToolMode::ContourPin) {
        return true;
    }
    if (m_meshWireframe.visible && !m_meshWireframe.indices.empty()) {
        return true;
    }
    if (!m_bones.empty() || !m_ikHandles.empty()) {
        return true;
    }
    if (m_onionSkinEnabled && !m_onionFrames.empty()) {
        return true;
    }
    if (m_weightBrush.heatmapEnabled && !m_meshWireframe.vertices.empty() &&
        !m_meshWireframe.indices.empty()) {
        return true;
    }
    return false;
}

float ViewportGizmoOverlay::computeWorldGizmoScale(const QVector3D &gizmoPos,
                                                   const QVector3D &camEye, float fovYDeg,
                                                   int viewportHeightPx, float orthoZoom,
                                                   bool perspectiveMode) const noexcept
{
    if (perspectiveMode) {
        const float dist = std::max(0.05f, (gizmoPos - camEye).length());
        const float fovRad = qDegreesToRadians(std::clamp(fovYDeg, 10.0f, 140.0f));
        const float worldPerPx = (2.0f * dist * std::tan(0.5f * fovRad)) /
                                 static_cast<float>(std::max(1, viewportHeightPx));
        return std::clamp(worldPerPx * kHandleScreenRadiusPx, 0.05f, 5000.0f);
    }
    const float zoom = std::max(0.01f, orthoZoom);
    return std::clamp(kHandleScreenRadiusPx / zoom, 0.05f, 5000.0f);
}

GizmoAxisHandle ViewportGizmoOverlay::pickHandle(const QPointF &widgetPos,
                                                 const QSize &viewportSize,
                                                 const QMatrix4x4 &viewProj) const noexcept
{
    if (!m_hasActiveNode || m_activeNode.locked || m_toolMode == ViewportToolMode::Navigate ||
        m_toolMode >= ViewportToolMode::BrushMask) {
        return GizmoAxisHandle::None;
    }

    const QVector3D origin(m_activeNode.origin[0], m_activeNode.origin[1], m_activeNode.origin[2]);
    const float s = estimateWorldScaleFromViewProj(origin, viewportSize, viewProj);
    const ProjectedPoint p0 = projectWorldToWidget(origin, viewportSize, viewProj);
    if (!p0.valid) {
        return GizmoAxisHandle::None;
    }

    float bestDist = kPickTolerancePx;
    GizmoAxisHandle best = GizmoAxisHandle::None;

    if (m_toolMode == ViewportToolMode::SelectBBox) {
        QVector3D bMin;
        QVector3D bMax;
        activeNodeBBoxCorners(m_activeNode, bMin, bMax);
        const ProjectedPoint pMin = projectWorldToWidget(bMin, viewportSize, viewProj);
        const ProjectedPoint pMax = projectWorldToWidget(bMax, viewportSize, viewProj);
        const float cornerTol = kPickTolerancePx * 1.5f;
        if (pMin.valid && distance2D(widgetPos, pMin.screenPos) <= cornerTol) {
            return GizmoAxisHandle::BBoxCornerMin;
        }
        if (pMax.valid && distance2D(widgetPos, pMax.screenPos) <= cornerTol) {
            return GizmoAxisHandle::BBoxCornerMax;
        }
        return GizmoAxisHandle::None;
    }

    if (m_toolMode == ViewportToolMode::Rotate) {
        const ProjectedPoint px =
            projectWorldToWidget(origin + QVector3D(s, 0.0f, 0.0f), viewportSize, viewProj);
        if (!px.valid) {
            return GizmoAxisHandle::None;
        }
        const float ringRadiusPx = distance2D(p0.screenPos, px.screenPos);
        const float cursorRadiusPx = distance2D(p0.screenPos, widgetPos);
        if (std::abs(cursorRadiusPx - ringRadiusPx) <= kPickTolerancePx) {
            return GizmoAxisHandle::RotateZ;
        }
        return GizmoAxisHandle::None;
    }

    /* Plane handle takes precedence over overlapping axes inside the small corner quad. */
    if (m_toolMode == ViewportToolMode::Translate || m_toolMode == ViewportToolMode::Scale) {
        const float q0 = 0.12f * s;
        const float q1 = 0.34f * s;
        const ProjectedPoint pA =
            projectWorldToWidget(origin + QVector3D(q0, q0, 0.0f), viewportSize, viewProj);
        const ProjectedPoint pB =
            projectWorldToWidget(origin + QVector3D(q1, q0, 0.0f), viewportSize, viewProj);
        const ProjectedPoint pC =
            projectWorldToWidget(origin + QVector3D(q1, q1, 0.0f), viewportSize, viewProj);
        const ProjectedPoint pD =
            projectWorldToWidget(origin + QVector3D(q0, q1, 0.0f), viewportSize, viewProj);
        if (pA.valid && pB.valid && pC.valid && pD.valid) {
            const float dQuad = pointInQuadDistance2D(widgetPos, pA.screenPos, pB.screenPos,
                                                      pC.screenPos, pD.screenPos);
            if (dQuad <= kPickTolerancePx * 0.5f) {
                return GizmoAxisHandle::PlaneXY;
            }
        }
    }

    const std::array<std::pair<GizmoAxisHandle, QVector3D>, 3> axisEnds{{
        {GizmoAxisHandle::AxisX, origin + QVector3D(s, 0.0f, 0.0f)},
        {GizmoAxisHandle::AxisY, origin + QVector3D(0.0f, s, 0.0f)},
        {GizmoAxisHandle::AxisZ, origin + QVector3D(0.0f, 0.0f, s)},
    }};
    for (const auto &[handle, endWorld] : axisEnds) {
        const ProjectedPoint pEnd = projectWorldToWidget(endWorld, viewportSize, viewProj);
        if (!pEnd.valid) {
            continue;
        }
        const float dist = distancePointToSegment2D(widgetPos, p0.screenPos, pEnd.screenPos);
        if (dist <= bestDist) {
            bestDist = dist;
            best = handle;
        }
    }

    return best;
}

QVector3D ViewportGizmoOverlay::texelToCanvasWorld(double px, double py,
                                                   const QSize &viewportSize) const noexcept
{
    const uint32_t ch = (m_contentHeight > 0u)
                            ? m_contentHeight
                            : static_cast<uint32_t>(std::max(1, viewportSize.height()));
    return QVector3D(static_cast<float>(px), static_cast<float>(static_cast<double>(ch) - py),
                     0.0f);
}

QVector3D ViewportGizmoOverlay::puppetLocalToCanvasWorld(const QVector3D &localPos,
                                                         const QSize &viewportSize) const noexcept
{
    const uint32_t cw = (m_contentWidth > 0u)
                            ? m_contentWidth
                            : static_cast<uint32_t>(std::max(0, viewportSize.width()));
    const uint32_t ch = (m_contentHeight > 0u)
                            ? m_contentHeight
                            : static_cast<uint32_t>(std::max(0, viewportSize.height()));
    return QVector3D(localPos.x() + 0.5f * static_cast<float>(cw),
                     localPos.y() + 0.5f * static_cast<float>(ch), localPos.z());
}

RigPickResult ViewportGizmoOverlay::pickRiggingHandle(const QPointF &widgetPos,
                                                      const QSize &viewportSize,
                                                      const QMatrix4x4 &viewProj) const noexcept
{
    RigPickResult best{};
    const float jointTol = kPickTolerancePx * 1.35f;

    /* 1. IK Target & Pole-Vector diamond handles */
    for (const auto &ik : m_ikHandles) {
        const ProjectedPoint pTarget = projectWorldToWidget(
            puppetLocalToCanvasWorld(ik.targetWorld, viewportSize), viewportSize, viewProj);
        if (pTarget.valid) {
            const float d = distance2D(widgetPos, pTarget.screenPos);
            if (d <= jointTol && d < best.distancePx) {
                best = {GizmoAxisHandle::IkTarget, ik.chainIndex, d};
            }
        }
        const ProjectedPoint pPole = projectWorldToWidget(
            puppetLocalToCanvasWorld(ik.poleWorld, viewportSize), viewportSize, viewProj);
        if (pPole.valid) {
            const float d = distance2D(widgetPos, pPole.screenPos);
            if (d <= jointTol && d < best.distancePx) {
                best = {GizmoAxisHandle::IkPole, ik.chainIndex, d};
            }
        }
    }
    if (best.handle != GizmoAxisHandle::None) {
        return best;
    }

    /* 2. CDT Contour constraint pins (when ContourPin mode or pins are visible) */
    if (m_toolMode == ViewportToolMode::ContourPin || m_meshWireframe.visible) {
        for (size_t i = 0; i < m_meshWireframe.pins.size(); ++i) {
            const auto &pin = m_meshWireframe.pins[i];
            const ProjectedPoint pPin = projectWorldToWidget(
                texelToCanvasWorld(pin.px, pin.py, viewportSize), viewportSize, viewProj);
            if (pPin.valid) {
                const float d = distance2D(widgetPos, pPin.screenPos);
                if (d <= jointTol && d < best.distancePx) {
                    best = {GizmoAxisHandle::ContourPinHandle, static_cast<int32_t>(i), d};
                }
            }
        }
        if (best.handle != GizmoAxisHandle::None) {
            return best;
        }
    }

    /* 3. Selected bone pivot rotate ring */
    if (m_selectedBoneIndex >= 0 && static_cast<size_t>(m_selectedBoneIndex) < m_bones.size() &&
        (m_toolMode == ViewportToolMode::Rotate || m_toolMode == ViewportToolMode::BonePlace)) {
        const auto &selBone = m_bones[static_cast<size_t>(m_selectedBoneIndex)];
        const float ringRadiusWorld =
            std::clamp((selBone.tailWorld - selBone.headWorld).length() * 0.45f, 16.0f, 64.0f);
        const QVector3D headCanvas = puppetLocalToCanvasWorld(selBone.headWorld, viewportSize);
        const ProjectedPoint pHead = projectWorldToWidget(headCanvas, viewportSize, viewProj);
        const ProjectedPoint pRing = projectWorldToWidget(
            headCanvas + QVector3D(ringRadiusWorld, 0.0f, 0.0f), viewportSize, viewProj);
        if (pHead.valid && pRing.valid) {
            const float ringPx = distance2D(pHead.screenPos, pRing.screenPos);
            const float cursorPx = distance2D(pHead.screenPos, widgetPos);
            const float ringDelta = std::abs(cursorPx - ringPx);
            if (ringDelta <= kPickTolerancePx) {
                return {GizmoAxisHandle::BonePivotRotate, m_selectedBoneIndex, ringDelta};
            }
        }
    }

    /* 4. Bone joint pivots */
    for (size_t i = 0; i < m_bones.size(); ++i) {
        const ProjectedPoint pHead = projectWorldToWidget(
            puppetLocalToCanvasWorld(m_bones[i].headWorld, viewportSize), viewportSize, viewProj);
        if (!pHead.valid) {
            continue;
        }
        const float d = distance2D(widgetPos, pHead.screenPos);
        if (d <= jointTol && d < best.distancePx) {
            best = {GizmoAxisHandle::BoneJoint, static_cast<int32_t>(i), d};
        }
    }
    if (best.handle != GizmoAxisHandle::None) {
        return best;
    }

    /* 5. Bone octo-prism bodies */
    for (size_t i = 0; i < m_bones.size(); ++i) {
        const ProjectedPoint pHead = projectWorldToWidget(
            puppetLocalToCanvasWorld(m_bones[i].headWorld, viewportSize), viewportSize, viewProj);
        const ProjectedPoint pTail = projectWorldToWidget(
            puppetLocalToCanvasWorld(m_bones[i].tailWorld, viewportSize), viewportSize, viewProj);
        if (!pHead.valid || !pTail.valid) {
            continue;
        }
        const float d = distancePointToSegment2D(widgetPos, pHead.screenPos, pTail.screenPos);
        if (d <= kPickTolerancePx && d < best.distancePx) {
            best = {GizmoAxisHandle::BoneBody, static_cast<int32_t>(i), d};
        }
    }

    return best;
}

bool ViewportGizmoOverlay::beginDrag(const QPointF &widgetPos, const QSize &viewportSize,
                                     const QMatrix4x4 &viewProj)
{
    if (!m_hasActiveNode || m_activeNode.locked) {
        return false;
    }
    const GizmoAxisHandle hit = pickHandle(widgetPos, viewportSize, viewProj);
    if (hit == GizmoAxisHandle::None) {
        return false;
    }

    m_dragging = true;
    m_activeHandle = hit;
    m_dragStartWidgetPos = widgetPos;
    m_dragInitialTransform.origin = m_activeNode.origin;
    m_dragInitialTransform.scale = m_activeNode.scale;
    m_dragInitialTransform.angles = m_activeNode.angles;
    m_dragInitialTransform.size = m_activeNode.size;
    m_dragInitialTransform.parallaxDepth = m_activeNode.parallaxDepth;
    return true;
}

SetLayerTransformCommand::TransformState
ViewportGizmoOverlay::updateDrag(const QPointF &widgetPos, const QSize &viewportSize,
                                 const QMatrix4x4 &viewProj, bool ctrlSnapHeld,
                                 bool shiftAspectHeld) const noexcept
{
    SetLayerTransformCommand::TransformState out = m_dragInitialTransform;
    if (!m_dragging || m_activeHandle == GizmoAxisHandle::None) {
        return out;
    }

    const bool snap = ctrlSnapHeld || m_snap.enabled;
    const bool lockAspect = shiftAspectHeld || m_snap.lockAspectRatio;
    const QVector3D origin0(m_dragInitialTransform.origin[0], m_dragInitialTransform.origin[1],
                            m_dragInitialTransform.origin[2]);

    bool invertible = false;
    const QMatrix4x4 invVP = viewProj.inverted(&invertible);

    if (m_toolMode == ViewportToolMode::Rotate || m_activeHandle == GizmoAxisHandle::RotateZ) {
        const ProjectedPoint pivot = projectWorldToWidget(origin0, viewportSize, viewProj);
        if (!pivot.valid) {
            return out;
        }
        const double a0 = std::atan2(m_dragStartWidgetPos.y() - pivot.screenPos.y(),
                                     m_dragStartWidgetPos.x() - pivot.screenPos.x());
        const double a1 =
            std::atan2(widgetPos.y() - pivot.screenPos.y(), widgetPos.x() - pivot.screenPos.x());
        double dRad = a1 - a0;
        while (dRad > M_PI) {
            dRad -= 2.0 * M_PI;
        }
        while (dRad < -M_PI) {
            dRad += 2.0 * M_PI;
        }

        const ProjectedPoint py =
            projectWorldToWidget(origin0 + QVector3D(0.0f, 1.0f, 0.0f), viewportSize, viewProj);
        const bool yUpWorld = !py.valid || (py.screenPos.y() < pivot.screenPos.y());
        const float dDeg = static_cast<float>(qRadiansToDegrees(yUpWorld ? -dRad : dRad));
        float newAngleZ = m_dragInitialTransform.angles[2] + dDeg;
        if (snap) {
            newAngleZ = snapValue(newAngleZ, std::max(0.1f, m_snap.angleStepDeg));
        }
        out.angles[2] = newAngleZ;
        return out;
    }

    QVector3D r0Orig;
    QVector3D r0Dir;
    QVector3D r1Orig;
    QVector3D r1Dir;
    const bool raysOk =
        invertible &&
        unprojectWidgetRay(m_dragStartWidgetPos, viewportSize, invVP, r0Orig, r0Dir) &&
        unprojectWidgetRay(widgetPos, viewportSize, invVP, r1Orig, r1Dir);

    const auto computeWorldDelta = [&](const QVector3D &planeNormal) -> QVector3D {
        if (!raysOk) {
            return {};
        }
        QVector3D h0;
        QVector3D h1;
        if (intersectRayPlane(r0Orig, r0Dir, origin0, planeNormal, h0) &&
            intersectRayPlane(r1Orig, r1Dir, origin0, planeNormal, h1)) {
            return h1 - h0;
        }
        return {};
    };

    const auto computeAxisDelta = [&](const QVector3D &axisDir) -> float {
        if (raysOk) {
            QVector3D tangent = QVector3D::crossProduct(axisDir, r0Dir);
            if (tangent.lengthSquared() < 1e-6f) {
                tangent = QVector3D::crossProduct(axisDir,
                                                  QVector3D(axisDir.y(), axisDir.z(), axisDir.x()));
            }
            const QVector3D planeNormal = QVector3D::crossProduct(tangent, axisDir).normalized();
            const QVector3D d = computeWorldDelta(planeNormal);
            return QVector3D::dotProduct(d, axisDir);
        }
        const ProjectedPoint p0 = projectWorldToWidget(origin0, viewportSize, viewProj);
        const ProjectedPoint p1 = projectWorldToWidget(origin0 + axisDir, viewportSize, viewProj);
        if (!p0.valid || !p1.valid) {
            return 0.0f;
        }
        const QPointF screenAxis = p1.screenPos - p0.screenPos;
        const float pxSq =
            static_cast<float>(screenAxis.x() * screenAxis.x() + screenAxis.y() * screenAxis.y());
        if (pxSq < 1e-6f) {
            return 0.0f;
        }
        const QPointF dPx = widgetPos - m_dragStartWidgetPos;
        return static_cast<float>(dPx.x() * screenAxis.x() + dPx.y() * screenAxis.y()) / pxSq;
    };

    if (m_toolMode == ViewportToolMode::Translate) {
        QVector3D delta;
        switch (m_activeHandle) {
        case GizmoAxisHandle::AxisX:
            delta.setX(computeAxisDelta(QVector3D(1.0f, 0.0f, 0.0f)));
            break;
        case GizmoAxisHandle::AxisY:
            delta.setY(computeAxisDelta(QVector3D(0.0f, 1.0f, 0.0f)));
            break;
        case GizmoAxisHandle::AxisZ:
            delta.setZ(computeAxisDelta(QVector3D(0.0f, 0.0f, 1.0f)));
            break;
        case GizmoAxisHandle::PlaneXY:
            delta = computeWorldDelta(QVector3D(0.0f, 0.0f, 1.0f));
            delta.setZ(0.0f);
            break;
        case GizmoAxisHandle::PlaneXZ:
            delta = computeWorldDelta(QVector3D(0.0f, 1.0f, 0.0f));
            delta.setY(0.0f);
            break;
        case GizmoAxisHandle::PlaneYZ:
            delta = computeWorldDelta(QVector3D(1.0f, 0.0f, 0.0f));
            delta.setX(0.0f);
            break;
        default:
            break;
        }

        out.origin[0] = m_dragInitialTransform.origin[0] + delta.x();
        out.origin[1] = m_dragInitialTransform.origin[1] + delta.y();
        out.origin[2] = m_dragInitialTransform.origin[2] + delta.z();

        if (snap) {
            const float step = std::max(0.01f, m_snap.gridStepPx);
            if (m_activeHandle == GizmoAxisHandle::AxisX ||
                m_activeHandle == GizmoAxisHandle::PlaneXY ||
                m_activeHandle == GizmoAxisHandle::PlaneXZ) {
                out.origin[0] = snapValue(out.origin[0], step);
            }
            if (m_activeHandle == GizmoAxisHandle::AxisY ||
                m_activeHandle == GizmoAxisHandle::PlaneXY ||
                m_activeHandle == GizmoAxisHandle::PlaneYZ) {
                out.origin[1] = snapValue(out.origin[1], step);
            }
            if (m_activeHandle == GizmoAxisHandle::AxisZ ||
                m_activeHandle == GizmoAxisHandle::PlaneXZ ||
                m_activeHandle == GizmoAxisHandle::PlaneYZ) {
                out.origin[2] = snapValue(out.origin[2], step);
            }
        }
        return out;
    }

    if (m_toolMode == ViewportToolMode::Scale) {
        const float s = estimateWorldScaleFromViewProj(origin0, viewportSize, viewProj);
        const float normDenom = std::max(1.0f, s);
        float factorX = 1.0f;
        float factorY = 1.0f;
        float factorZ = 1.0f;

        if (m_activeHandle == GizmoAxisHandle::AxisX) {
            factorX =
                std::max(0.01f, 1.0f + computeAxisDelta(QVector3D(1.0f, 0.0f, 0.0f)) / normDenom);
            if (lockAspect) {
                factorY = factorX;
                factorZ = factorX;
            }
        } else if (m_activeHandle == GizmoAxisHandle::AxisY) {
            factorY =
                std::max(0.01f, 1.0f + computeAxisDelta(QVector3D(0.0f, 1.0f, 0.0f)) / normDenom);
            if (lockAspect) {
                factorX = factorY;
                factorZ = factorY;
            }
        } else if (m_activeHandle == GizmoAxisHandle::AxisZ) {
            factorZ =
                std::max(0.01f, 1.0f + computeAxisDelta(QVector3D(0.0f, 0.0f, 1.0f)) / normDenom);
            if (lockAspect) {
                factorX = factorZ;
                factorY = factorZ;
            }
        } else if (m_activeHandle == GizmoAxisHandle::PlaneXY) {
            const QPointF dPx = widgetPos - m_dragStartWidgetPos;
            const float uniform = std::max(0.01f, 1.0f + static_cast<float>(dPx.x() - dPx.y()) /
                                                             kHandleScreenRadiusPx);
            factorX = uniform;
            factorY = uniform;
            if (lockAspect) {
                factorZ = uniform;
            }
        }

        out.scale[0] = m_dragInitialTransform.scale[0] * factorX;
        out.scale[1] = m_dragInitialTransform.scale[1] * factorY;
        out.scale[2] = m_dragInitialTransform.scale[2] * factorZ;
        if (snap) {
            const float step = std::max(0.01f, m_snap.scaleStep);
            out.scale[0] = std::max(step, snapValue(out.scale[0], step));
            out.scale[1] = std::max(step, snapValue(out.scale[1], step));
            out.scale[2] = std::max(step, snapValue(out.scale[2], step));
        }
        return out;
    }

    if (m_toolMode == ViewportToolMode::SelectBBox &&
        (m_activeHandle == GizmoAxisHandle::BBoxCornerMin ||
         m_activeHandle == GizmoAxisHandle::BBoxCornerMax)) {
        const QVector3D delta = computeWorldDelta(QVector3D(0.0f, 0.0f, 1.0f));
        const float sign = (m_activeHandle == GizmoAxisHandle::BBoxCornerMax) ? 2.0f : -2.0f;
        const float sx = std::max(1e-3f, std::abs(m_dragInitialTransform.scale[0]));
        const float sy = std::max(1e-3f, std::abs(m_dragInitialTransform.scale[1]));
        float w = std::max(1.0f, m_dragInitialTransform.size[0] + sign * (delta.x() / sx));
        float h = std::max(1.0f, m_dragInitialTransform.size[1] + sign * (delta.y() / sy));
        if (snap) {
            const float step = std::max(1.0f, m_snap.gridStepPx);
            w = std::max(step, snapValue(w, step));
            h = std::max(step, snapValue(h, step));
        }
        if (lockAspect && m_dragInitialTransform.size[0] > 1e-3f &&
            m_dragInitialTransform.size[1] > 1e-3f) {
            const float aspect = m_dragInitialTransform.size[0] / m_dragInitialTransform.size[1];
            h = std::max(1.0f, w / aspect);
        }
        out.size[0] = w;
        out.size[1] = h;
    }

    return out;
}

void ViewportGizmoOverlay::endDrag() noexcept
{
    m_dragging = false;
    m_activeHandle = GizmoAxisHandle::None;
}

void ViewportGizmoOverlay::buildRenderVertices(const QSize &viewportSize,
                                               const QMatrix4x4 &viewProj, const QVector3D &camEye,
                                               float fovYDeg, float orthoZoom, bool perspectiveMode,
                                               std::vector<GizmoVertex> &outLineVerts) const
{
    outLineVerts.clear();
    outLineVerts.reserve(kMaxDynamicVertices);
    (void)viewProj;

    const int vh = std::max(1, viewportSize.height());

    if (m_hasActiveNode && m_toolMode != ViewportToolMode::Navigate &&
        m_toolMode < ViewportToolMode::BrushMask) {
        const QVector3D origin(m_activeNode.origin[0], m_activeNode.origin[1],
                               m_activeNode.origin[2]);
        const float s =
            computeWorldGizmoScale(origin, camEye, fovYDeg, vh, orthoZoom, perspectiveMode);

        QVector3D bMin;
        QVector3D bMax;
        activeNodeBBoxCorners(m_activeNode, bMin, bMax);
        const bool isSelect = (m_toolMode == ViewportToolMode::SelectBBox);
        pushBoxWireframe(outLineVerts, bMin, bMax, 0.220f, 0.741f, 0.973f,
                         isSelect ? 0.85f : 0.35f);

        const float xHi = (m_activeHandle == GizmoAxisHandle::AxisX) ? 1.0f : 0.85f;
        const float yHi = (m_activeHandle == GizmoAxisHandle::AxisY) ? 1.0f : 0.85f;
        const float zHi = (m_activeHandle == GizmoAxisHandle::AxisZ) ? 1.0f : 0.85f;

        if (m_toolMode == ViewportToolMode::Translate) {
            const QVector3D xEnd = origin + QVector3D(s, 0.0f, 0.0f);
            const QVector3D yEnd = origin + QVector3D(0.0f, s, 0.0f);
            const QVector3D zEnd = origin + QVector3D(0.0f, 0.0f, s);
            const float tip = 0.14f * s;

            pushLine(outLineVerts, origin, xEnd, 0.937f, 0.267f, 0.267f, xHi);
            pushLine(outLineVerts, xEnd, xEnd + QVector3D(-tip, 0.5f * tip, 0.0f), 0.937f, 0.267f,
                     0.267f, xHi);
            pushLine(outLineVerts, xEnd, xEnd + QVector3D(-tip, -0.5f * tip, 0.0f), 0.937f, 0.267f,
                     0.267f, xHi);

            pushLine(outLineVerts, origin, yEnd, 0.133f, 0.773f, 0.369f, yHi);
            pushLine(outLineVerts, yEnd, yEnd + QVector3D(0.5f * tip, -tip, 0.0f), 0.133f, 0.773f,
                     0.369f, yHi);
            pushLine(outLineVerts, yEnd, yEnd + QVector3D(-0.5f * tip, -tip, 0.0f), 0.133f, 0.773f,
                     0.369f, yHi);

            pushLine(outLineVerts, origin, zEnd, 0.220f, 0.741f, 0.973f, zHi);

            const float q0 = 0.12f * s;
            const float q1 = 0.34f * s;
            const float pAlpha = (m_activeHandle == GizmoAxisHandle::PlaneXY) ? 1.0f : 0.75f;
            const QVector3D pq0 = origin + QVector3D(q0, q0, 0.0f);
            const QVector3D pq1 = origin + QVector3D(q1, q0, 0.0f);
            const QVector3D pq2 = origin + QVector3D(q1, q1, 0.0f);
            const QVector3D pq3 = origin + QVector3D(q0, q1, 0.0f);
            pushLine(outLineVerts, pq0, pq1, 0.961f, 0.620f, 0.043f, pAlpha);
            pushLine(outLineVerts, pq1, pq2, 0.961f, 0.620f, 0.043f, pAlpha);
            pushLine(outLineVerts, pq2, pq3, 0.961f, 0.620f, 0.043f, pAlpha);
            pushLine(outLineVerts, pq3, pq0, 0.961f, 0.620f, 0.043f, pAlpha);
        } else if (m_toolMode == ViewportToolMode::Rotate) {
            const float rAlpha = (m_activeHandle == GizmoAxisHandle::RotateZ) ? 1.0f : 0.85f;
            pushCircle(outLineVerts, origin, QVector3D(1.0f, 0.0f, 0.0f),
                       QVector3D(0.0f, 1.0f, 0.0f), s, 32, 0.220f, 0.741f, 0.973f, rAlpha);
            for (int i = 0; i < 4; ++i) {
                const float ang = static_cast<float>(i) * (kTwoPi * 0.25f);
                const QVector3D dir(std::cos(ang), std::sin(ang), 0.0f);
                pushLine(outLineVerts, origin + dir * (0.90f * s), origin + dir * (1.08f * s),
                         0.961f, 0.620f, 0.043f, 0.90f);
            }
        } else if (m_toolMode == ViewportToolMode::Scale) {
            const float boxR = 0.07f * s;
            const QVector3D xEnd = origin + QVector3D(s, 0.0f, 0.0f);
            const QVector3D yEnd = origin + QVector3D(0.0f, s, 0.0f);
            const QVector3D zEnd = origin + QVector3D(0.0f, 0.0f, s);
            pushLine(outLineVerts, origin, xEnd, 0.937f, 0.267f, 0.267f, xHi);
            pushBoxWireframe(outLineVerts, xEnd - QVector3D(boxR, boxR, boxR),
                             xEnd + QVector3D(boxR, boxR, boxR), 0.937f, 0.267f, 0.267f, xHi);
            pushLine(outLineVerts, origin, yEnd, 0.133f, 0.773f, 0.369f, yHi);
            pushBoxWireframe(outLineVerts, yEnd - QVector3D(boxR, boxR, boxR),
                             yEnd + QVector3D(boxR, boxR, boxR), 0.133f, 0.773f, 0.369f, yHi);
            pushLine(outLineVerts, origin, zEnd, 0.220f, 0.741f, 0.973f, zHi);
        } else if (m_toolMode == ViewportToolMode::SelectBBox) {
            const float tick = 0.15f * s;
            pushLine(outLineVerts, bMin, bMin + QVector3D(tick, 0.0f, 0.0f), 0.961f, 0.620f, 0.043f,
                     0.95f);
            pushLine(outLineVerts, bMin, bMin + QVector3D(0.0f, tick, 0.0f), 0.961f, 0.620f, 0.043f,
                     0.95f);
            pushLine(outLineVerts, bMax, bMax - QVector3D(tick, 0.0f, 0.0f), 0.961f, 0.620f, 0.043f,
                     0.95f);
            pushLine(outLineVerts, bMax, bMax - QVector3D(0.0f, tick, 0.0f), 0.961f, 0.620f, 0.043f,
                     0.95f);
        }
    }

    /* 2.5D Camera frustum and optional DOF focal slab wireframe */
    if (m_camera.projection_mode != WE_PROJ_ORTHOGRAPHIC_2D || m_camera.dof_enabled > 0.5f ||
        (m_hasActiveNode && m_activeNode.kind == WE_NODE_CAMERA3D)) {
        const QVector3D eye(m_camera.eye_pos[0], m_camera.eye_pos[1], m_camera.eye_pos[2]);
        const QVector3D target(m_camera.target_pos[0], m_camera.target_pos[1],
                               m_camera.target_pos[2]);
        const QVector3D fwd = (target - eye).lengthSquared() > 1e-6f ? (target - eye).normalized()
                                                                     : QVector3D(0.0f, 0.0f, -1.0f);
        const QVector3D upHint(m_camera.up_vec[0], m_camera.up_vec[1], m_camera.up_vec[2]);
        QVector3D right = QVector3D::crossProduct(fwd, upHint.lengthSquared() > 1e-6f
                                                           ? upHint.normalized()
                                                           : QVector3D(0.0f, 1.0f, 0.0f));
        if (right.lengthSquared() < 1e-6f) {
            right = QVector3D(1.0f, 0.0f, 0.0f);
        } else {
            right.normalize();
        }
        const QVector3D up = QVector3D::crossProduct(right, fwd).normalized();

        const float aspect =
            static_cast<float>(std::max(1, viewportSize.width())) / static_cast<float>(vh);
        const float halfFovRad = qDegreesToRadians(
            0.5f *
            std::clamp(m_camera.fov_y_deg > 1.0f ? m_camera.fov_y_deg : 60.0f, 10.0f, 140.0f));
        const float farDist = std::clamp(
            m_camera.dof_enabled > 0.5f && m_camera.dof_focal_dist > 1.0f ? m_camera.dof_focal_dist
                                                                          : 240.0f,
            16.0f, 2000.0f);
        const float halfH = farDist * std::tan(halfFovRad);
        const float halfW = halfH * aspect;
        const QVector3D center = eye + fwd * farDist;
        const QVector3D tl = center - right * halfW + up * halfH;
        const QVector3D tr = center + right * halfW + up * halfH;
        const QVector3D br = center + right * halfW - up * halfH;
        const QVector3D bl = center - right * halfW - up * halfH;

        pushLine(outLineVerts, eye, tl, 0.655f, 0.545f, 0.980f, 0.70f);
        pushLine(outLineVerts, eye, tr, 0.655f, 0.545f, 0.980f, 0.70f);
        pushLine(outLineVerts, eye, br, 0.655f, 0.545f, 0.980f, 0.70f);
        pushLine(outLineVerts, eye, bl, 0.655f, 0.545f, 0.980f, 0.70f);
        pushLine(outLineVerts, tl, tr, 0.220f, 0.741f, 0.973f, 0.80f);
        pushLine(outLineVerts, tr, br, 0.220f, 0.741f, 0.973f, 0.80f);
        pushLine(outLineVerts, br, bl, 0.220f, 0.741f, 0.973f, 0.80f);
        pushLine(outLineVerts, bl, tl, 0.220f, 0.741f, 0.973f, 0.80f);
    }

    /* Particle emitter wireframes */
    for (const auto &em : m_emitters) {
        const QVector3D orig(em.origin[0], em.origin[1], em.origin[2]);
        const bool selected = m_hasActiveNode && m_activeNode.id == em.objectId;
        const float alpha = selected ? 0.90f : 0.45f;
        if (em.emitterType == 0u) {
            const QVector3D bMin(orig.x() + em.distMin[0], orig.y() + em.distMin[1],
                                 orig.z() + em.distMin[2]);
            const QVector3D bMax(orig.x() + em.distMax[0], orig.y() + em.distMax[1],
                                 orig.z() + em.distMax[2]);
            pushBoxWireframe(outLineVerts, bMin, bMax, 0.133f, 0.773f, 0.369f, alpha);
        } else if (em.emitterType == 1u) {
            const float radius = std::max(
                {std::abs(em.distMax[0]), std::abs(em.distMax[1]), std::abs(em.distMax[2]), 24.0f});
            pushCircle(outLineVerts, orig, QVector3D(1.0f, 0.0f, 0.0f), QVector3D(0.0f, 1.0f, 0.0f),
                       radius, 16, 0.133f, 0.773f, 0.369f, alpha);
            pushCircle(outLineVerts, orig, QVector3D(1.0f, 0.0f, 0.0f), QVector3D(0.0f, 0.0f, 1.0f),
                       radius, 16, 0.133f, 0.773f, 0.369f, alpha);
        } else {
            const float r = 18.0f;
            pushLine(outLineVerts, orig - QVector3D(r, 0.0f, 0.0f), orig + QVector3D(r, 0.0f, 0.0f),
                     0.133f, 0.773f, 0.369f, alpha);
            pushLine(outLineVerts, orig - QVector3D(0.0f, r, 0.0f), orig + QVector3D(0.0f, r, 0.0f),
                     0.133f, 0.773f, 0.369f, alpha);
        }
        const QVector3D dir(em.directions[0], em.directions[1], em.directions[2]);
        if (dir.lengthSquared() > 1e-6f) {
            pushLine(outLineVerts, orig, orig + dir.normalized() * 48.0f, 0.961f, 0.620f, 0.043f,
                     alpha);
        }
    }

    /* Scene light attenuation rings and spot cones */
    const uint32_t lightCount =
        std::min<uint32_t>(m_lights.active_light_count, WE_MAX_SCENE_LIGHTS);
    for (uint32_t i = 0; i < lightCount; ++i) {
        const we_scene_light_t &l = m_lights.lights[i];
        if (l.light_type == WE_LIGHT_DISABLED) {
            continue;
        }
        const float r = l.color_rgb[0] > 0.01f ? std::min(1.0f, l.color_rgb[0]) : 0.961f;
        const float g = l.color_rgb[1] > 0.01f ? std::min(1.0f, l.color_rgb[1]) : 0.620f;
        const float b = l.color_rgb[2] > 0.01f ? std::min(1.0f, l.color_rgb[2]) : 0.043f;
        const QVector3D pos(l.position_or_dir[0], l.position_or_dir[1], l.position_or_dir[2]);
        const float atten = std::clamp(l.attenuation_radius, 16.0f, 1500.0f);

        if (l.light_type == WE_LIGHT_POINT) {
            pushCircle(outLineVerts, pos, QVector3D(1.0f, 0.0f, 0.0f), QVector3D(0.0f, 1.0f, 0.0f),
                       atten, 16, r, g, b, 0.55f);
        } else if (l.light_type == WE_LIGHT_SPOT) {
            const QVector3D axisRaw(l.spot_dir[0], l.spot_dir[1], l.spot_dir[2]);
            const QVector3D axis = axisRaw.lengthSquared() > 1e-6f ? axisRaw.normalized()
                                                                   : QVector3D(0.0f, -1.0f, 0.0f);
            QVector3D perp1 = QVector3D::crossProduct(axis, QVector3D(0.0f, 0.0f, 1.0f));
            if (perp1.lengthSquared() < 1e-6f) {
                perp1 = QVector3D::crossProduct(axis, QVector3D(1.0f, 0.0f, 0.0f));
            }
            perp1.normalize();
            const QVector3D perp2 = QVector3D::crossProduct(axis, perp1).normalized();
            const float outerCos = std::clamp(l.spot_outer_cos, 0.1f, 0.999f);
            const float coneSin = std::sqrt(std::max(0.0f, 1.0f - outerCos * outerCos));
            const float reach = std::min(atten, 320.0f);
            const float coneR = reach * (coneSin / outerCos);
            const QVector3D baseCenter = pos + axis * reach;
            pushLine(outLineVerts, pos, baseCenter + perp1 * coneR, r, g, b, 0.70f);
            pushLine(outLineVerts, pos, baseCenter - perp1 * coneR, r, g, b, 0.70f);
            pushLine(outLineVerts, pos, baseCenter + perp2 * coneR, r, g, b, 0.70f);
            pushLine(outLineVerts, pos, baseCenter - perp2 * coneR, r, g, b, 0.70f);
            pushCircle(outLineVerts, baseCenter, perp1, perp2, coneR, 12, r, g, b, 0.55f);
        } else if (l.light_type == WE_LIGHT_TUBE) {
            const QVector3D axisRaw(l.spot_dir[0], l.spot_dir[1], l.spot_dir[2]);
            const QVector3D axis = axisRaw.lengthSquared() > 1e-6f ? axisRaw.normalized()
                                                                   : QVector3D(1.0f, 0.0f, 0.0f);
            const float halfLen = 0.5f * std::max(8.0f, l.tube_length);
            pushLine(outLineVerts, pos - axis * halfLen, pos + axis * halfLen, r, g, b, 0.85f);
        }
    }

    /* CDT Mesh Wireframe + Contour Constraint Pins */
    if (m_meshWireframe.visible && !m_meshWireframe.vertices.empty() &&
        m_meshWireframe.indices.size() >= 3) {
        const size_t vertCount = m_meshWireframe.vertices.size();
        const size_t triCount = m_meshWireframe.indices.size() / 3;
        std::vector<uint32_t> packedEdges;
        packedEdges.reserve(triCount * 3);
        for (size_t t = 0; t < triCount; ++t) {
            const uint16_t i0 = m_meshWireframe.indices[t * 3 + 0];
            const uint16_t i1 = m_meshWireframe.indices[t * 3 + 1];
            const uint16_t i2 = m_meshWireframe.indices[t * 3 + 2];
            if (i0 >= vertCount || i1 >= vertCount || i2 >= vertCount) {
                continue;
            }
            const std::array<std::pair<uint16_t, uint16_t>, 3> edges{{
                {std::min(i0, i1), std::max(i0, i1)},
                {std::min(i1, i2), std::max(i1, i2)},
                {std::min(i2, i0), std::max(i2, i0)},
            }};
            for (const auto &[a, b] : edges) {
                packedEdges.push_back((static_cast<uint32_t>(a) << 16) | static_cast<uint32_t>(b));
            }
        }
        std::sort(packedEdges.begin(), packedEdges.end());
        for (size_t i = 0; i < packedEdges.size();) {
            size_t j = i + 1;
            while (j < packedEdges.size() && packedEdges[j] == packedEdges[i]) {
                ++j;
            }
            const uint16_t a = static_cast<uint16_t>(packedEdges[i] >> 16);
            const uint16_t b = static_cast<uint16_t>(packedEdges[i] & 0xFFFFu);
            const auto &va = m_meshWireframe.vertices[a];
            const auto &vb = m_meshWireframe.vertices[b];
            const bool isBoundary = ((j - i) == 1) && m_meshWireframe.highlightContourEdges;
            const QVector3D pa =
                puppetLocalToCanvasWorld(QVector3D(va.pos[0], va.pos[1], va.pos[2]), viewportSize);
            const QVector3D pb =
                puppetLocalToCanvasWorld(QVector3D(vb.pos[0], vb.pos[1], vb.pos[2]), viewportSize);
            if (isBoundary) {
                pushLine(outLineVerts, pa, pb, 0.220f, 0.741f, 0.973f, 0.85f);
            } else {
                pushLine(outLineVerts, pa, pb, 0.580f, 0.639f, 0.722f, 0.45f);
            }
            i = j;
        }
    }

    if (m_toolMode == ViewportToolMode::ContourPin || m_meshWireframe.visible) {
        for (size_t i = 0; i < m_meshWireframe.pins.size(); ++i) {
            const auto &pin = m_meshWireframe.pins[i];
            const QVector3D p = texelToCanvasWorld(pin.px, pin.py, viewportSize);
            const bool sel = (static_cast<int32_t>(i) == m_meshWireframe.selectedPinIndex);
            const float pr = sel ? 10.0f : 7.0f;
            if (sel) {
                pushDiamond2D(outLineVerts, p, pr, 0.220f, 0.741f, 0.973f, 0.98f);
            } else {
                pushDiamond2D(outLineVerts, p, pr, 0.976f, 0.886f, 0.686f, 0.90f);
            }
        }
    }

    /* Onion-Skinning Ghost Bones (#89b4fa past, #f38ba8 future) */
    if (m_onionSkinEnabled) {
        for (const auto &ghost : m_onionFrames) {
            const float alpha = std::clamp(ghost.opacity, 0.08f, 0.85f);
            const bool isPast = ghost.frameDelta < 0;
            const float r = isPast ? 0.537f : 0.953f;
            const float g = isPast ? 0.706f : 0.545f;
            const float b = isPast ? 0.980f : 0.659f;
            for (const auto &[head, tail] : ghost.boneSegments) {
                pushOctoPrismBone(outLineVerts, puppetLocalToCanvasWorld(head, viewportSize),
                                  puppetLocalToCanvasWorld(tail, viewportSize), r, g, b, alpha);
            }
        }
    }

    /* Puppet Skeleton Hierarchy (Tapered Octo-Prisms + Joint Pivots) */
    for (size_t i = 0; i < m_bones.size(); ++i) {
        const auto &bone = m_bones[i];
        float r = 0.804f;
        float g = 0.839f;
        float b = 0.957f;
        float a = 0.85f;
        if (bone.selected) {
            r = 0.220f;
            g = 0.741f;
            b = 0.973f;
            a = 1.0f;
        } else if (bone.driveMode == WE_BONE_DRIVE_IK) {
            r = 0.976f;
            g = 0.886f;
            b = 0.686f;
            a = 0.90f;
        } else if (bone.driveMode == WE_BONE_DRIVE_XPBD) {
            r = 0.651f;
            g = 0.890f;
            b = 0.631f;
            a = 0.90f;
        }

        const QVector3D headCanvas = puppetLocalToCanvasWorld(bone.headWorld, viewportSize);
        const QVector3D tailCanvas = puppetLocalToCanvasWorld(bone.tailWorld, viewportSize);
        if (bone.parentIndex != 0xFFFFFFFFu &&
            static_cast<size_t>(bone.parentIndex) < m_bones.size()) {
            const auto &parent = m_bones[bone.parentIndex];
            if ((bone.headWorld - parent.tailWorld).lengthSquared() > 1.0f) {
                pushLine(outLineVerts, puppetLocalToCanvasWorld(parent.tailWorld, viewportSize),
                         headCanvas, 0.580f, 0.639f, 0.722f, 0.45f);
            }
        }

        pushOctoPrismBone(outLineVerts, headCanvas, tailCanvas, r, g, b, a);
        const float jointR = bone.selected ? 7.5f : 5.0f;
        pushCircle(outLineVerts, headCanvas, QVector3D(1.0f, 0.0f, 0.0f),
                   QVector3D(0.0f, 1.0f, 0.0f), jointR, 12, r, g, b, a);

        if (bone.selected &&
            (m_toolMode == ViewportToolMode::Rotate || m_toolMode == ViewportToolMode::BonePlace)) {
            const float pivotRingR =
                std::clamp((bone.tailWorld - bone.headWorld).length() * 0.45f, 16.0f, 64.0f);
            pushCircle(outLineVerts, headCanvas, QVector3D(1.0f, 0.0f, 0.0f),
                       QVector3D(0.0f, 1.0f, 0.0f), pivotRingR, 24, 0.220f, 0.741f, 0.973f, 0.85f);
        }
    }

    if (m_boneCreatePreviewActive) {
        pushOctoPrismBone(outLineVerts, puppetLocalToCanvasWorld(m_boneCreateHead, viewportSize),
                          puppetLocalToCanvasWorld(m_boneCreateTail, viewportSize), 0.220f, 0.741f,
                          0.973f, 0.95f);
    }

    /* 2-Bone IK Target Reticle + Pole-Vector Diamond Handles (#f9e2af) */
    for (const auto &ik : m_ikHandles) {
        const QVector3D rootCanvas = puppetLocalToCanvasWorld(ik.rootWorld, viewportSize);
        const QVector3D midCanvas = puppetLocalToCanvasWorld(ik.midWorld, viewportSize);
        const QVector3D endCanvas = puppetLocalToCanvasWorld(ik.endWorld, viewportSize);
        const QVector3D targetCanvas = puppetLocalToCanvasWorld(ik.targetWorld, viewportSize);
        const QVector3D poleCanvas = puppetLocalToCanvasWorld(ik.poleWorld, viewportSize);
        pushLine(outLineVerts, rootCanvas, midCanvas, 0.976f, 0.886f, 0.686f, 0.55f);
        pushLine(outLineVerts, midCanvas, endCanvas, 0.976f, 0.886f, 0.686f, 0.55f);
        pushLine(outLineVerts, endCanvas, targetCanvas, 0.976f, 0.886f, 0.686f, 0.40f);
        pushLine(outLineVerts, midCanvas, poleCanvas, 0.976f, 0.886f, 0.686f, 0.35f);
        pushReticle2D(outLineVerts, targetCanvas, 11.0f, 0.976f, 0.886f, 0.686f, 0.95f);
        pushDiamond2D(outLineVerts, poleCanvas, 9.0f, 0.976f, 0.886f, 0.686f, 0.95f);
    }

    /* Weight-Paint Brush Cursor Circle */
    if (m_toolMode == ViewportToolMode::WeightBrush && m_weightBrush.cursorInside) {
        const QVector3D c(static_cast<float>(m_weightBrush.cursorWorldPos.x()),
                          static_cast<float>(m_weightBrush.cursorWorldPos.y()), 0.0f);
        const float outerR = std::max(2.0f, m_weightBrush.radiusPx);
        const float innerR =
            std::max(1.0f, outerR * std::clamp(m_weightBrush.strength, 0.1f, 1.0f));
        pushCircle(outLineVerts, c, QVector3D(1.0f, 0.0f, 0.0f), QVector3D(0.0f, 1.0f, 0.0f),
                   outerR, 32, 0.976f, 0.886f, 0.686f, 0.92f);
        pushCircle(outLineVerts, c, QVector3D(1.0f, 0.0f, 0.0f), QVector3D(0.0f, 1.0f, 0.0f),
                   innerR, 20, 0.220f, 0.741f, 0.973f, 0.65f);
    }
}

void ViewportGizmoOverlay::evaluateHeatmapColor(we_heatmap_palette_t palette, float weight,
                                                float &outR, float &outG, float &outB) noexcept
{
    const float t = std::clamp(weight, 0.0f, 1.0f);
    if (palette == WE_HEATMAP_PALETTE_VIRIDIS) {
        const float r = 0.267004f + t * (-0.0049f + t * (1.2656f - t * 0.5357f));
        const float g = 0.004874f + t * (1.3840f + t * (-0.4910f + t * 0.0080f));
        const float b = 0.329415f + t * (1.4250f + t * (-2.6810f + t * 1.0710f));
        outR = std::clamp(r, 0.0f, 1.0f);
        outG = std::clamp(g, 0.0f, 1.0f);
        outB = std::clamp(b, 0.0f, 1.0f);
        return;
    }
    if (palette == WE_HEATMAP_PALETTE_INFERNO) {
        const float r = 0.001462f + t * (1.5800f + t * (0.4800f - t * 1.0700f));
        const float g = 0.000466f + t * (0.1200f + t * (1.2800f - t * 0.4000f));
        const float b = 0.013866f + t * (2.3500f + t * (-3.4000f + t * 1.6500f));
        outR = std::clamp(r, 0.0f, 1.0f);
        outG = std::clamp(g, 0.0f, 1.0f);
        outB = std::clamp(b, 0.0f, 1.0f);
        return;
    }
    /* WE_HEATMAP_TURBO */
    const float r =
        0.13572f +
        t * (4.61539f + t * (-42.6603f + t * (132.131f + t * (-152.942f + t * 59.286f))));
    const float g =
        0.09140f +
        t * (2.19419f + t * (4.84297f + t * (-14.1850f + t * (4.27730f + t * 2.82957f))));
    const float b =
        0.10667f +
        t * (12.6419f + t * (-60.5820f + t * (110.363f + t * (-89.9031f + t * 27.3482f))));
    outR = std::clamp(r, 0.0f, 1.0f);
    outG = std::clamp(g, 0.0f, 1.0f);
    outB = std::clamp(b, 0.0f, 1.0f);
}

void ViewportGizmoOverlay::buildHeatmapTriangleVertices(std::vector<GizmoVertex> &outTriVerts) const
{
    outTriVerts.clear();
    if (!m_weightBrush.heatmapEnabled || m_meshWireframe.vertices.empty() ||
        m_meshWireframe.indices.size() < 3) {
        return;
    }

    const size_t vertCount = m_meshWireframe.vertices.size();
    const size_t triCount = m_meshWireframe.indices.size() / 3;
    outTriVerts.reserve(triCount * 3);
    const float alpha = std::clamp(m_weightBrush.heatmapOpacity, 0.05f, 0.95f);

    for (size_t t = 0; t < triCount; ++t) {
        const uint16_t i0 = m_meshWireframe.indices[t * 3 + 0];
        const uint16_t i1 = m_meshWireframe.indices[t * 3 + 1];
        const uint16_t i2 = m_meshWireframe.indices[t * 3 + 2];
        if (i0 >= vertCount || i1 >= vertCount || i2 >= vertCount) {
            continue;
        }
        const std::array<uint16_t, 3> tri{i0, i1, i2};
        for (uint16_t idx : tri) {
            const auto &v = m_meshWireframe.vertices[idx];
            const float w = evalBoneWeightForVertex(v, m_weightBrush.activeBoneIndex);
            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;
            evaluateHeatmapColor(m_weightBrush.palette, w, r, g, b);
            const QVector3D wp =
                puppetLocalToCanvasWorld(QVector3D(v.pos[0], v.pos[1], v.pos[2]), QSize());
            outTriVerts.push_back({wp.x(), wp.y(), wp.z(), r, g, b, alpha});
        }
    }
}

} // namespace waywal::studio
