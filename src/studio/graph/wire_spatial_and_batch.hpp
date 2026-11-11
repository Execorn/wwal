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
        const int32_t cx = static_cast<int32_t>(std::floor(entry.scenePos.x() / kCellSize));
        const int32_t cy = static_cast<int32_t>(std::floor(entry.scenePos.y() / kCellSize));
        m_cells[packKey(cx, cy)].push_back(entry);
        ++m_pinCount;
    }

    void rebuildFromGraph(const waywal_shader_graph_t &graph, float nodeWidth = 200.0f,
                          float headerHeight = 28.0f, float pinRowHeight = 24.0f)
    {
        clear();
        m_cells.reserve(static_cast<size_t>(graph.node_count) * 2u + 1u);

        for (uint32_t i = 0; i < graph.node_count; ++i) {
            const auto &node = graph.nodes[i];
            const double baseY = static_cast<double>(node.canvas_pos[1] + headerHeight + 16.0f);

            for (uint32_t p = 0; p < node.input_pin_count; ++p) {
                const auto &pin = node.inputs[p];
                if ((pin.flags & WAYWAL_PIN_FLAG_HIDDEN) != 0) {
                    continue;
                }
                insert({node.node_id, p, pin.pin_type, false,
                        QPointF(node.canvas_pos[0], baseY + p * pinRowHeight)});
            }

            for (uint32_t p = 0; p < node.output_pin_count; ++p) {
                const auto &pin = node.outputs[p];
                if ((pin.flags & WAYWAL_PIN_FLAG_HIDDEN) != 0) {
                    continue;
                }
                insert({node.node_id, p, pin.pin_type, true,
                        QPointF(node.canvas_pos[0] + nodeWidth, baseY + p * pinRowHeight)});
            }
        }
    }

    [[nodiscard]] std::optional<PinSpatialEntry>
    findNearestCompatible(const QPointF &cursorPos, bool wantOutputPin, uint32_t draggedPinType,
                          float snapRadius, uint32_t excludeNodeId = 0) const
    {
        if (!std::isfinite(snapRadius) || snapRadius <= 0.0f) {
            return std::nullopt;
        }

        const int32_t cx = static_cast<int32_t>(std::floor(cursorPos.x() / kCellSize));
        const int32_t cy = static_cast<int32_t>(std::floor(cursorPos.y() / kCellSize));
        const int32_t span =
            (snapRadius <= kCellSize) ? 1 : static_cast<int32_t>(std::ceil(snapRadius / kCellSize));

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
                    const bool compat =
                        wantOutputPin ? waywal_pin_types_compatible(e.pinType, draggedPinType)
                                      : waywal_pin_types_compatible(draggedPinType, e.pinType);
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
