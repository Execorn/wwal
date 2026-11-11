// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_COMMAND_BUS_PHASE3_HPP
#define WAYWAL_STUDIO_COMMAND_BUS_PHASE3_HPP

#include "studio/core/command_bus.hpp"
#include "waywal/shader_graph.h"
#include "waywal/we_camera3d.h"
#include "waywal/we_physics.h"

#include <QPointF>
#include <QString>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace waywal::studio {

class AddNodeCommand final : public ICommand {
  public:
    using MutateFn = std::function<void(const waywal_graph_node_t &, bool isAdd)>;

    AddNodeCommand(waywal_graph_node_t node, MutateFn mutateFn);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;
    [[nodiscard]] uint32_t nodeId() const noexcept { return m_node.node_id; }

  private:
    waywal_graph_node_t m_node{};
    MutateFn m_mutateFn;
};

class RemoveNodeCommand final : public ICommand {
  public:
    using RestoreFn = std::function<void(const waywal_graph_node_t &,
                                         const std::vector<waywal_graph_edge_t> &, bool isRestore)>;

    RemoveNodeCommand(waywal_graph_node_t removedNode,
                      std::vector<waywal_graph_edge_t> detachedEdges, RestoreFn restoreFn);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;

  private:
    waywal_graph_node_t m_removedNode{};
    std::vector<waywal_graph_edge_t> m_detachedEdges;
    RestoreFn m_restoreFn;
};

class MoveNodesCommand final : public ICommand {
  public:
    struct NodeMoveDelta {
        uint32_t nodeId{0};
        QPointF oldPos;
        QPointF newPos;
    };
    using ApplyMoveFn = std::function<void(const std::vector<NodeMoveDelta> &, bool useNewPos)>;

    MoveNodesCommand(std::vector<NodeMoveDelta> deltas, ApplyMoveFn applyFn);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;
    bool mergeWith(const ICommand *other) override; /* Coalesces drags within 500 ms */

    [[nodiscard]] const std::vector<NodeMoveDelta> &deltas() const noexcept { return m_deltas; }

  private:
    std::vector<NodeMoveDelta> m_deltas;
    ApplyMoveFn m_applyFn;
    std::chrono::steady_clock::time_point m_timestamp;
};

class ConnectWireCommand final : public ICommand {
  public:
    using WireFn = std::function<void(const waywal_graph_edge_t &, bool isConnect)>;

    ConnectWireCommand(waywal_graph_edge_t edge, WireFn wireFn,
                       std::optional<waywal_graph_edge_t> replacedEdge = std::nullopt);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;
    [[nodiscard]] const std::optional<waywal_graph_edge_t> &replacedEdge() const noexcept
    {
        return m_replacedEdge;
    }

  private:
    waywal_graph_edge_t m_edge{};
    std::optional<waywal_graph_edge_t> m_replacedEdge;
    WireFn m_wireFn;
};

class DisconnectWireCommand final : public ICommand {
  public:
    using WireFn = std::function<void(const waywal_graph_edge_t &, bool isConnect)>;

    DisconnectWireCommand(waywal_graph_edge_t edge, WireFn wireFn);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;

  private:
    waywal_graph_edge_t m_edge{};
    WireFn m_wireFn;
};

class SetNodeConstantCommand final : public ICommand {
  public:
    using ApplyConstFn = std::function<void(uint32_t nodeId, const std::array<float, 4> &val)>;

    SetNodeConstantCommand(uint32_t nodeId, QString nodeTitle, std::array<float, 4> oldVal,
                           std::array<float, 4> newVal, ApplyConstFn applyFn);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;
    bool mergeWith(const ICommand *other) override; /* Coalesces slider drags within 500 ms */

    [[nodiscard]] uint32_t nodeId() const noexcept { return m_nodeId; }
    [[nodiscard]] const std::array<float, 4> &newVal() const noexcept { return m_newVal; }

  private:
    uint32_t m_nodeId{0};
    QString m_nodeTitle;
    std::array<float, 4> m_oldVal{};
    std::array<float, 4> m_newVal{};
    ApplyConstFn m_applyFn;
    std::chrono::steady_clock::time_point m_timestamp;
};

class SetCamera3DCommand final : public ICommand {
  public:
    using ApplyCamFn = std::function<void(const QString &output, const we_camera3d_t &cam,
                                          const we_lighting_uniforms_t &lights)>;

    SetCamera3DCommand(QString output, we_camera3d_t oldCam, we_camera3d_t newCam,
                       we_lighting_uniforms_t oldLights, we_lighting_uniforms_t newLights,
                       ApplyCamFn applyFn);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;
    bool mergeWith(const ICommand *other) override; /* Coalesces 500 ms camera/light scrubs */

    [[nodiscard]] const QString &output() const noexcept { return m_output; }
    [[nodiscard]] const we_camera3d_t &newCamera() const noexcept { return m_newCam; }
    [[nodiscard]] const we_lighting_uniforms_t &newLights() const noexcept { return m_newLights; }

  private:
    QString m_output;
    we_camera3d_t m_oldCam{};
    we_camera3d_t m_newCam{};
    we_lighting_uniforms_t m_oldLights{};
    we_lighting_uniforms_t m_newLights{};
    ApplyCamFn m_applyFn;
    std::chrono::steady_clock::time_point m_timestamp;
};

class SetPhysicsSpringCommand final : public ICommand {
  public:
    using ApplyPhysFn = std::function<void(
        const QString &output, int32_t objectId, int32_t boneIndex, const we_bone_sim_config_t &cfg,
        const we_wind_field_t &wind, const we_cursor_force_t &cursor)>;

    SetPhysicsSpringCommand(QString output, int32_t objectId, int32_t boneIndex,
                            we_bone_sim_config_t oldCfg, we_bone_sim_config_t newCfg,
                            we_wind_field_t oldWind, we_wind_field_t newWind,
                            we_cursor_force_t oldCursor, we_cursor_force_t newCursor,
                            ApplyPhysFn applyFn);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;
    bool mergeWith(const ICommand *other) override; /* Coalesces 500 ms spring/wind drags */

    [[nodiscard]] const QString &output() const noexcept { return m_output; }
    [[nodiscard]] int32_t objectId() const noexcept { return m_objectId; }
    [[nodiscard]] int32_t boneIndex() const noexcept { return m_boneIndex; }

  private:
    QString m_output;
    int32_t m_objectId{-1};
    int32_t m_boneIndex{-1};
    we_bone_sim_config_t m_oldCfg{};
    we_bone_sim_config_t m_newCfg{};
    we_wind_field_t m_oldWind{};
    we_wind_field_t m_newWind{};
    we_cursor_force_t m_oldCursor{};
    we_cursor_force_t m_newCursor{};
    ApplyPhysFn m_applyFn;
    std::chrono::steady_clock::time_point m_timestamp;
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_COMMAND_BUS_PHASE3_HPP
