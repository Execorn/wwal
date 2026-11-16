// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_GRAPH_WIRE_SPATIAL_AND_BATCH_HPP
#define WAYWAL_STUDIO_GRAPH_WIRE_SPATIAL_AND_BATCH_HPP

#include "waywal/shader_graph.h"

#include <QPainterPath>
#include <QPointF>
#include <QRectF>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace waywal::studio {

inline constexpr float kGraphNodeWidth = 200.0f;
inline constexpr float kGraphHeaderHeight = 28.0f;
inline constexpr float kGraphPinRowHeight = 24.0f;
inline constexpr float kGraphPinYOffset = 16.0f;

[[nodiscard]] inline constexpr float
graphPinCenterY(uint32_t pinIdx, float headerHeight = kGraphHeaderHeight,
                float pinRowHeight = kGraphPinRowHeight) noexcept
{
    return headerHeight + kGraphPinYOffset + static_cast<float>(pinIdx) * pinRowHeight;
}

[[nodiscard]] inline QPointF graphPinScenePos(const QPointF &nodePos, uint32_t pinIdx,
                                              bool isOutput, float nodeWidth = kGraphNodeWidth,
                                              float headerHeight = kGraphHeaderHeight,
                                              float pinRowHeight = kGraphPinRowHeight) noexcept
{
    return QPointF(nodePos.x() + (isOutput ? static_cast<double>(nodeWidth) : 0.0),
                   nodePos.y() +
                       static_cast<double>(graphPinCenterY(pinIdx, headerHeight, pinRowHeight)));
}

struct CachedWireGeometry {
    QPointF p0{};
    QPointF p1{};
    QPointF c0{};
    QPointF c1{};
    QPainterPath cachedPath{};
    QPointF cachedMidpoint{};
    QRectF cachedBounds{};
    bool dirty{true};

    void setEndpoints(const QPointF &start, const QPointF &end) noexcept
    {
        if (p0 != start || p1 != end) {
            p0 = start;
            p1 = end;
            dirty = true;
        }
    }

    void ensureUpdated()
    {
        if (!dirty) {
            return;
        }
        const qreal dx = std::max(60.0, std::abs(p1.x() - p0.x()) * 0.45);
        c0 = QPointF(p0.x() + dx, p0.y());
        c1 = QPointF(p1.x() - dx, p1.y());

        cachedPath = QPainterPath(p0);
        cachedPath.cubicTo(c0, c1, p1);
        cachedMidpoint = (p0 + 3.0 * c0 + 3.0 * c1 + p1) * 0.125;

        const QRectF pillBounds(cachedMidpoint.x() - 48.0, cachedMidpoint.y() - 12.0, 96.0, 24.0);
        cachedBounds = cachedPath.boundingRect().adjusted(-6.0, -6.0, 6.0, 6.0).united(pillBounds);
        dirty = false;
    }
};

struct PinSpatialEntry {
    uint32_t nodeId{0};
    uint32_t pinIdx{0};
    uint32_t pinType{0};
    bool isOutput{false};
    QPointF scenePos{};
    uint32_t pinFlags{0};
};

class UniformPinSpatialGrid {
  public:
    static constexpr float kCellSize = 64.0f;

    void clear()
    {
        m_cells.clear();
        m_pinCount = 0;
    }

    [[nodiscard]] size_t pinCount() const noexcept { return m_pinCount; }

    void insert(const PinSpatialEntry &entry)
    {
        if (!std::isfinite(entry.scenePos.x()) || !std::isfinite(entry.scenePos.y())) {
            return;
        }
        const int32_t cx = cellCoord(entry.scenePos.x());
        const int32_t cy = cellCoord(entry.scenePos.y());
        m_cells[packKey(cx, cy)].push_back(entry);
        ++m_pinCount;
    }

    void rebuildFromGraph(const waywal_shader_graph_t &graph, float nodeWidth = kGraphNodeWidth,
                          float headerHeight = kGraphHeaderHeight,
                          float pinRowHeight = kGraphPinRowHeight)
    {
        clear();
        const uint32_t nodeCount = std::min<uint32_t>(graph.node_count, WAYWAL_GRAPH_MAX_NODES);
        m_cells.reserve(static_cast<size_t>(nodeCount) * 2u + 1u);

        for (uint32_t i = 0; i < nodeCount; ++i) {
            const auto &node = graph.nodes[i];
            if ((node.flags & WAYWAL_NODE_FLAG_COLLAPSED) != 0) {
                continue;
            }
            if (!std::isfinite(node.canvas_pos[0]) || !std::isfinite(node.canvas_pos[1])) {
                continue;
            }
            const QPointF nodePos(node.canvas_pos[0], node.canvas_pos[1]);

            const uint32_t inCount =
                std::min<uint32_t>(node.input_pin_count, WAYWAL_GRAPH_MAX_PINS_PER_NODE);
            for (uint32_t p = 0; p < inCount; ++p) {
                const auto &pin = node.inputs[p];
                if ((pin.flags & WAYWAL_PIN_FLAG_HIDDEN) != 0) {
                    continue;
                }
                insert({node.node_id, p, pin.pin_type, false,
                        graphPinScenePos(nodePos, p, false, nodeWidth, headerHeight, pinRowHeight),
                        pin.flags});
            }

            const uint32_t outCount =
                std::min<uint32_t>(node.output_pin_count, WAYWAL_GRAPH_MAX_PINS_PER_NODE);
            for (uint32_t p = 0; p < outCount; ++p) {
                const auto &pin = node.outputs[p];
                if ((pin.flags & WAYWAL_PIN_FLAG_HIDDEN) != 0) {
                    continue;
                }
                insert({node.node_id, p, pin.pin_type, true,
                        graphPinScenePos(nodePos, p, true, nodeWidth, headerHeight, pinRowHeight),
                        pin.flags});
            }
        }
    }

    [[nodiscard]] std::optional<PinSpatialEntry>
    findNearestCompatible(const QPointF &cursorPos, bool wantOutputPin, uint32_t draggedPinType,
                          float snapRadius, uint32_t excludeNodeId = 0,
                          uint32_t draggedPinFlags = 0) const
    {
        if (!std::isfinite(cursorPos.x()) || !std::isfinite(cursorPos.y()) ||
            !std::isfinite(snapRadius) || snapRadius <= 0.0f) {
            return std::nullopt;
        }

        const int32_t cx = cellCoord(cursorPos.x());
        const int32_t cy = cellCoord(cursorPos.y());
        const double rawSpan = std::ceil(static_cast<double>(snapRadius) / kCellSize);
        const int32_t span =
            (snapRadius <= kCellSize) ? 1 : static_cast<int32_t>(std::clamp(rawSpan, 1.0, 64.0));

        double bestDistSq = static_cast<double>(snapRadius) * static_cast<double>(snapRadius);
        std::optional<PinSpatialEntry> best;

        for (int32_t dy = -span; dy <= span; ++dy) {
            for (int32_t dx = -span; dx <= span; ++dx) {
                const auto it = m_cells.find(packKey(cx + dx, cy + dy));
                if (it == m_cells.end()) {
                    continue;
                }
                for (const auto &e : it->second) {
                    if (e.isOutput != wantOutputPin || e.nodeId == excludeNodeId) {
                        continue;
                    }
                    const bool nonCoercible =
                        ((e.pinFlags | draggedPinFlags) & WAYWAL_PIN_FLAG_NON_COERCIBLE) != 0;
                    const bool compat =
                        nonCoercible
                            ? (e.pinType == draggedPinType)
                            : (wantOutputPin
                                   ? waywal_pin_types_compatible(e.pinType, draggedPinType)
                                   : waywal_pin_types_compatible(draggedPinType, e.pinType));
                    if (!compat) {
                        continue;
                    }
                    const double ddx = e.scenePos.x() - cursorPos.x();
                    const double ddy = e.scenePos.y() - cursorPos.y();
                    const double distSq = ddx * ddx + ddy * ddy;
                    if (distSq <= bestDistSq) {
                        bestDistSq = distSq;
                        best = e;
                    }
                }
            }
        }
        return best;
    }

  private:
    [[nodiscard]] static int32_t cellCoord(double pos) noexcept
    {
        const double scaled = std::clamp(pos / kCellSize, -1000000.0, 1000000.0);
        return static_cast<int32_t>(std::floor(scaled));
    }
    [[nodiscard]] static constexpr uint64_t packKey(int32_t x, int32_t y) noexcept
    {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
               static_cast<uint64_t>(static_cast<uint32_t>(y));
    }

    std::unordered_map<uint64_t, std::vector<PinSpatialEntry>> m_cells;
    size_t m_pinCount{0};
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_GRAPH_WIRE_SPATIAL_AND_BATCH_HPP
