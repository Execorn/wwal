// SPDX-License-Identifier: GPL-3.0-or-later
#include "command_bus.hpp"

#include <QFileInfo>

namespace waywal::studio {

/* ========================================================================= */
/* CommandBus Implementation                                                 */
/* ========================================================================= */

CommandBus::CommandBus(size_t maxHistory, QObject *parent)
    : QObject(parent), m_maxHistory(maxHistory > 0 ? maxHistory : 1000)
{
}

void CommandBus::execute(std::unique_ptr<ICommand> cmd)
{
    if (!cmd)
        return;

    if (!m_undoStack.empty() && m_undoStack.back()->mergeWith(cmd.get())) {
        // Coalesced micro-drag action (e.g. continuous slider or bezier drag)
        m_undoStack.back()->execute();
        notifyHistoryChanged();
        emit stateChanged();
        return;
    }

    cmd->execute();
    m_undoStack.push_back(std::move(cmd));
    m_redoStack.clear();

    if (m_undoStack.size() > m_maxHistory) {
        m_undoStack.pop_front();
    }

    notifyHistoryChanged();
    emit stateChanged();
}

void CommandBus::undo()
{
    if (m_undoStack.empty())
        return;

    auto cmd = std::move(m_undoStack.back());
    m_undoStack.pop_back();

    cmd->undo();
    m_redoStack.push_back(std::move(cmd));

    notifyHistoryChanged();
    emit stateChanged();
}

void CommandBus::redo()
{
    if (m_redoStack.empty())
        return;

    auto cmd = std::move(m_redoStack.back());
    m_redoStack.pop_back();

    cmd->redo();
    m_undoStack.push_back(std::move(cmd));

    notifyHistoryChanged();
    emit stateChanged();
}

void CommandBus::clear()
{
    m_undoStack.clear();
    m_redoStack.clear();
    notifyHistoryChanged();
    emit stateChanged();
}

bool CommandBus::canUndo() const { return !m_undoStack.empty(); }

bool CommandBus::canRedo() const { return !m_redoStack.empty(); }

QString CommandBus::undoText() const
{
    if (m_undoStack.empty())
        return {};
    return m_undoStack.back()->describe();
}

QString CommandBus::redoText() const
{
    if (m_redoStack.empty())
        return {};
    return m_redoStack.back()->describe();
}

size_t CommandBus::undoCount() const { return m_undoStack.size(); }

size_t CommandBus::redoCount() const { return m_redoStack.size(); }

void CommandBus::notifyHistoryChanged()
{
    emit historyChanged(canUndo(), canRedo(), undoText(), redoText());
}

/* ========================================================================= */
/* SetUniformCommand Implementation                                          */
/* ========================================================================= */

SetUniformCommand::SetUniformCommand(QString output, QString uniformName,
                                     std::vector<float> oldValues, std::vector<float> newValues,
                                     uint32_t flags, ApplyFunction applyFunc)
    : m_output(std::move(output)), m_uniformName(std::move(uniformName)),
      m_oldValues(std::move(oldValues)), m_newValues(std::move(newValues)), m_flags(flags),
      m_applyFunc(std::move(applyFunc)), m_timestamp(std::chrono::steady_clock::now())
{
}

void SetUniformCommand::execute()
{
    if (m_applyFunc) {
        m_applyFunc(m_output, m_uniformName, m_newValues.data(),
                    static_cast<uint32_t>(m_newValues.size()), m_flags);
    } else if (auto *ipc = Services::instance().ipcClient()) {
        ipc->sendSetUniform(m_output, m_uniformName, m_newValues.data(),
                            static_cast<uint32_t>(m_newValues.size()),
                            (m_flags & WAYWAL_UNIFORM_FLAG_PERSIST) != 0);
    }
}

void SetUniformCommand::undo()
{
    if (m_applyFunc) {
        m_applyFunc(m_output, m_uniformName, m_oldValues.data(),
                    static_cast<uint32_t>(m_oldValues.size()), m_flags);
    } else if (auto *ipc = Services::instance().ipcClient()) {
        ipc->sendSetUniform(m_output, m_uniformName, m_oldValues.data(),
                            static_cast<uint32_t>(m_oldValues.size()),
                            (m_flags & WAYWAL_UNIFORM_FLAG_PERSIST) != 0);
    }
}

QString SetUniformCommand::describe() const
{
    if (m_output.isEmpty()) {
        return QString("Set uniform '%1'").arg(m_uniformName);
    }
    return QString("Set uniform '%1' on %2").arg(m_uniformName, m_output);
}

bool SetUniformCommand::mergeWith(const ICommand *other)
{
    const auto *otherCmd = dynamic_cast<const SetUniformCommand *>(other);
    if (!otherCmd)
        return false;

    if (m_output != otherCmd->m_output || m_uniformName != otherCmd->m_uniformName) {
        return false;
    }

    auto diff =
        std::chrono::duration_cast<std::chrono::milliseconds>(otherCmd->m_timestamp - m_timestamp)
            .count();

    // Coalesce continuous slider/color micro-drags within 500 ms into a single undo step
    if (diff < 0 || diff > 500) {
        return false;
    }

    m_newValues = otherCmd->m_newValues;
    m_flags = otherCmd->m_flags;
    m_timestamp = otherCmd->m_timestamp;
    return true;
}

/* ========================================================================= */
/* SetBezierCurveCommand Implementation                                      */
/* ========================================================================= */

SetBezierCurveCommand::SetBezierCurveCommand(QString output, bezier_curve_t oldCurve,
                                             bezier_curve_t newCurve, ApplyFunction applyFunc)
    : m_output(std::move(output)), m_oldCurve(oldCurve), m_newCurve(newCurve),
      m_applyFunc(std::move(applyFunc)), m_timestamp(std::chrono::steady_clock::now())
{
}

void SetBezierCurveCommand::execute()
{
    if (m_applyFunc) {
        m_applyFunc(m_output, m_newCurve);
    }
}

void SetBezierCurveCommand::undo()
{
    if (m_applyFunc) {
        m_applyFunc(m_output, m_oldCurve);
    }
}

QString SetBezierCurveCommand::describe() const
{
    if (m_output.isEmpty()) {
        return QString("Adjust Cubic-Bezier easing curve");
    }
    return QString("Adjust Cubic-Bezier easing curve on %1").arg(m_output);
}

bool SetBezierCurveCommand::mergeWith(const ICommand *other)
{
    const auto *otherCmd = dynamic_cast<const SetBezierCurveCommand *>(other);
    if (!otherCmd)
        return false;

    if (m_output != otherCmd->m_output) {
        return false;
    }

    auto diff =
        std::chrono::duration_cast<std::chrono::milliseconds>(otherCmd->m_timestamp - m_timestamp)
            .count();

    if (diff < 0 || diff > 500) {
        return false;
    }

    m_newCurve = otherCmd->m_newCurve;
    m_timestamp = otherCmd->m_timestamp;
    return true;
}

/* ========================================================================= */
/* ApplyWallpaperCommand Implementation                                      */
/* ========================================================================= */

ApplyWallpaperCommand::ApplyWallpaperCommand(QString output, waywal_img_metadata_t oldMeta,
                                             waywal_img_metadata_t newMeta, QString oldPath,
                                             QString newPath, ApplyFunction applyFunc)
    : m_output(std::move(output)), m_oldMeta(oldMeta), m_newMeta(newMeta),
      m_oldPath(std::move(oldPath)), m_newPath(std::move(newPath)),
      m_applyFunc(std::move(applyFunc))
{
}

void ApplyWallpaperCommand::execute()
{
    if (m_applyFunc) {
        m_applyFunc(m_output, m_newMeta, m_newPath);
    } else if (auto *ipc = Services::instance().ipcClient()) {
        ipc->sendSetImage(m_newMeta, -1);
    }
}

void ApplyWallpaperCommand::undo()
{
    if (m_applyFunc) {
        m_applyFunc(m_output, m_oldMeta, m_oldPath);
    } else if (auto *ipc = Services::instance().ipcClient()) {
        ipc->sendSetImage(m_oldMeta, -1);
    }
}

QString ApplyWallpaperCommand::describe() const
{
    QString fileName = QFileInfo(m_newPath).fileName();
    if (fileName.isEmpty()) {
        fileName = m_newPath;
    }
    if (m_output.isEmpty()) {
        return QString("Apply wallpaper '%1'").arg(fileName);
    }
    return QString("Apply wallpaper '%1' to %2").arg(fileName, m_output);
}

/* ========================================================================= */
/* SetPowerPolicyCommand Implementation                                      */
/* ========================================================================= */

SetPowerPolicyCommand::SetPowerPolicyCommand(uint32_t oldPolicy, uint32_t newPolicy,
                                             ApplyFunction applyFunc)
    : m_oldPolicy(oldPolicy), m_newPolicy(newPolicy), m_applyFunc(std::move(applyFunc))
{
}

void SetPowerPolicyCommand::execute()
{
    if (m_applyFunc) {
        m_applyFunc(m_newPolicy);
    } else if (auto *ipc = Services::instance().ipcClient()) {
        ipc->sendAudioPowerConfig(-1.0f, -1, -1, static_cast<uint8_t>(m_newPolicy));
    }
}

void SetPowerPolicyCommand::undo()
{
    if (m_applyFunc) {
        m_applyFunc(m_oldPolicy);
    } else if (auto *ipc = Services::instance().ipcClient()) {
        ipc->sendAudioPowerConfig(-1.0f, -1, -1, static_cast<uint8_t>(m_oldPolicy));
    }
}

QString SetPowerPolicyCommand::describe() const
{
    const char *policyStr = "unknown";
    switch (m_newPolicy) {
    case 0:
        policyStr = "ignore";
        break;
    case 1:
        policyStr = "reduce-fps";
        break;
    case 2:
        policyStr = "pause-video";
        break;
    }
    return QString("Set battery policy: %1").arg(policyStr);
}

/* ========================================================================= */
/* Shader Graph, 3D Camera & Puppet Physics Commands                         */
/* ========================================================================= */

AddNodeCommand::AddNodeCommand(waywal_graph_node_t node, MutateFn mutateFn)
    : m_node(node), m_mutateFn(std::move(mutateFn))
{
}

void AddNodeCommand::execute()
{
    if (m_mutateFn) {
        m_mutateFn(m_node, true);
    }
}

void AddNodeCommand::undo()
{
    if (m_mutateFn) {
        m_mutateFn(m_node, false);
    }
}

QString AddNodeCommand::describe() const
{
    return QStringLiteral("Add node '%1' (#%2)")
        .arg(QString::fromUtf8(m_node.title), QString::number(m_node.node_id));
}

RemoveNodeCommand::RemoveNodeCommand(waywal_graph_node_t removedNode,
                                     std::vector<waywal_graph_edge_t> detachedEdges,
                                     RestoreFn restoreFn)
    : m_removedNode(removedNode), m_detachedEdges(std::move(detachedEdges)),
      m_restoreFn(std::move(restoreFn))
{
}

void RemoveNodeCommand::execute()
{
    if (m_restoreFn) {
        m_restoreFn(m_removedNode, m_detachedEdges, false);
    }
}

void RemoveNodeCommand::undo()
{
    if (m_restoreFn) {
        m_restoreFn(m_removedNode, m_detachedEdges, true);
    }
}

QString RemoveNodeCommand::describe() const
{
    return QStringLiteral("Remove node '%1' (#%2)")
        .arg(QString::fromUtf8(m_removedNode.title), QString::number(m_removedNode.node_id));
}

MoveNodesCommand::MoveNodesCommand(std::vector<NodeMoveDelta> deltas, ApplyMoveFn applyFn)
    : m_deltas(std::move(deltas)), m_applyFn(std::move(applyFn)),
      m_timestamp(std::chrono::steady_clock::now())
{
}

void MoveNodesCommand::execute()
{
    if (m_applyFn) {
        m_applyFn(m_deltas, true);
    }
}

void MoveNodesCommand::undo()
{
    if (m_applyFn) {
        m_applyFn(m_deltas, false);
    }
}

QString MoveNodesCommand::describe() const
{
    return QStringLiteral("Move %1 node(s)").arg(m_deltas.size());
}

bool MoveNodesCommand::mergeWith(const ICommand *other)
{
    const auto *otherCmd = dynamic_cast<const MoveNodesCommand *>(other);
    if (!otherCmd) {
        return false;
    }

    auto diff =
        std::chrono::duration_cast<std::chrono::milliseconds>(otherCmd->m_timestamp - m_timestamp)
            .count();
    if (diff < 0 || diff > 500) {
        return false;
    }

    if (m_deltas.size() != otherCmd->m_deltas.size()) {
        return false;
    }

    for (size_t i = 0; i < m_deltas.size(); ++i) {
        if (m_deltas[i].nodeId != otherCmd->m_deltas[i].nodeId) {
            return false;
        }
    }

    for (size_t i = 0; i < m_deltas.size(); ++i) {
        m_deltas[i].newPos = otherCmd->m_deltas[i].newPos;
    }
    m_timestamp = otherCmd->m_timestamp;
    return true;
}

ConnectWireCommand::ConnectWireCommand(waywal_graph_edge_t edge, WireFn wireFn)
    : m_edge(edge), m_wireFn(std::move(wireFn))
{
}

void ConnectWireCommand::execute()
{
    if (m_wireFn) {
        m_wireFn(m_edge, true);
    }
}

void ConnectWireCommand::undo()
{
    if (m_wireFn) {
        m_wireFn(m_edge, false);
    }
}

QString ConnectWireCommand::describe() const
{
    return QStringLiteral("Connect wire (#%1: %2:%3 -> %4:%5)")
        .arg(QString::number(m_edge.edge_id), QString::number(m_edge.src_node_id),
             QString::number(m_edge.src_pin_index), QString::number(m_edge.dst_node_id),
             QString::number(m_edge.dst_pin_index));
}

DisconnectWireCommand::DisconnectWireCommand(waywal_graph_edge_t edge, WireFn wireFn)
    : m_edge(edge), m_wireFn(std::move(wireFn))
{
}

void DisconnectWireCommand::execute()
{
    if (m_wireFn) {
        m_wireFn(m_edge, false);
    }
}

void DisconnectWireCommand::undo()
{
    if (m_wireFn) {
        m_wireFn(m_edge, true);
    }
}

QString DisconnectWireCommand::describe() const
{
    return QStringLiteral("Disconnect wire (#%1: %2:%3 -> %4:%5)")
        .arg(QString::number(m_edge.edge_id), QString::number(m_edge.src_node_id),
             QString::number(m_edge.src_pin_index), QString::number(m_edge.dst_node_id),
             QString::number(m_edge.dst_pin_index));
}

SetNodeConstantCommand::SetNodeConstantCommand(uint32_t nodeId, QString nodeTitle,
                                               std::array<float, 4> oldVal,
                                               std::array<float, 4> newVal, ApplyConstFn applyFn)
    : m_nodeId(nodeId), m_nodeTitle(std::move(nodeTitle)), m_oldVal(oldVal), m_newVal(newVal),
      m_applyFn(std::move(applyFn)), m_timestamp(std::chrono::steady_clock::now())
{
}

void SetNodeConstantCommand::execute()
{
    if (m_applyFn) {
        m_applyFn(m_nodeId, m_newVal);
    }
}

void SetNodeConstantCommand::undo()
{
    if (m_applyFn) {
        m_applyFn(m_nodeId, m_oldVal);
    }
}

QString SetNodeConstantCommand::describe() const
{
    return QStringLiteral("Set constant on '%1' (#%2)").arg(m_nodeTitle, QString::number(m_nodeId));
}

bool SetNodeConstantCommand::mergeWith(const ICommand *other)
{
    const auto *otherCmd = dynamic_cast<const SetNodeConstantCommand *>(other);
    if (!otherCmd || m_nodeId != otherCmd->m_nodeId) {
        return false;
    }

    auto diff =
        std::chrono::duration_cast<std::chrono::milliseconds>(otherCmd->m_timestamp - m_timestamp)
            .count();
    if (diff < 0 || diff > 500) {
        return false;
    }

    m_newVal = otherCmd->m_newVal;
    m_timestamp = otherCmd->m_timestamp;
    return true;
}

SetCamera3DCommand::SetCamera3DCommand(QString output, we_camera3d_t oldCam, we_camera3d_t newCam,
                                       we_lighting_uniforms_t oldLights,
                                       we_lighting_uniforms_t newLights, ApplyCamFn applyFn)
    : m_output(std::move(output)), m_oldCam(oldCam), m_newCam(newCam), m_oldLights(oldLights),
      m_newLights(newLights), m_applyFn(std::move(applyFn)),
      m_timestamp(std::chrono::steady_clock::now())
{
}

void SetCamera3DCommand::execute()
{
    if (m_applyFn) {
        m_applyFn(m_output, m_newCam, m_newLights);
    }
}

void SetCamera3DCommand::undo()
{
    if (m_applyFn) {
        m_applyFn(m_output, m_oldCam, m_oldLights);
    }
}

QString SetCamera3DCommand::describe() const
{
    if (m_output.isEmpty()) {
        return QStringLiteral("Adjust 3D Camera & Lighting");
    }
    return QStringLiteral("Adjust 3D Camera & Lighting on %1").arg(m_output);
}

bool SetCamera3DCommand::mergeWith(const ICommand *other)
{
    const auto *otherCmd = dynamic_cast<const SetCamera3DCommand *>(other);
    if (!otherCmd || m_output != otherCmd->m_output) {
        return false;
    }

    auto diff =
        std::chrono::duration_cast<std::chrono::milliseconds>(otherCmd->m_timestamp - m_timestamp)
            .count();
    if (diff < 0 || diff > 500) {
        return false;
    }

    m_newCam = otherCmd->m_newCam;
    m_newLights = otherCmd->m_newLights;
    m_timestamp = otherCmd->m_timestamp;
    return true;
}

SetPhysicsSpringCommand::SetPhysicsSpringCommand(QString output, int32_t objectId,
                                                 int32_t boneIndex, we_bone_sim_config_t oldCfg,
                                                 we_bone_sim_config_t newCfg,
                                                 we_wind_field_t oldWind, we_wind_field_t newWind,
                                                 we_cursor_force_t oldCursor,
                                                 we_cursor_force_t newCursor, ApplyPhysFn applyFn)
    : m_output(std::move(output)), m_objectId(objectId), m_boneIndex(boneIndex), m_oldCfg(oldCfg),
      m_newCfg(newCfg), m_oldWind(oldWind), m_newWind(newWind), m_oldCursor(oldCursor),
      m_newCursor(newCursor), m_applyFn(std::move(applyFn)),
      m_timestamp(std::chrono::steady_clock::now())
{
}

void SetPhysicsSpringCommand::execute()
{
    if (m_applyFn) {
        m_applyFn(m_output, m_objectId, m_boneIndex, m_newCfg, m_newWind, m_newCursor);
    }
}

void SetPhysicsSpringCommand::undo()
{
    if (m_applyFn) {
        m_applyFn(m_output, m_objectId, m_boneIndex, m_oldCfg, m_oldWind, m_oldCursor);
    }
}

QString SetPhysicsSpringCommand::describe() const
{
    if (m_output.isEmpty()) {
        return QStringLiteral("Adjust physics spring bone %1").arg(m_boneIndex);
    }
    return QStringLiteral("Adjust physics spring bone %1 on %2").arg(m_boneIndex).arg(m_output);
}

bool SetPhysicsSpringCommand::mergeWith(const ICommand *other)
{
    const auto *otherCmd = dynamic_cast<const SetPhysicsSpringCommand *>(other);
    if (!otherCmd || m_output != otherCmd->m_output || m_objectId != otherCmd->m_objectId ||
        m_boneIndex != otherCmd->m_boneIndex) {
        return false;
    }

    auto diff =
        std::chrono::duration_cast<std::chrono::milliseconds>(otherCmd->m_timestamp - m_timestamp)
            .count();
    if (diff < 0 || diff > 500) {
        return false;
    }

    m_newCfg = otherCmd->m_newCfg;
    m_newWind = otherCmd->m_newWind;
    m_newCursor = otherCmd->m_newCursor;
    m_timestamp = otherCmd->m_timestamp;
    return true;
}

} // namespace waywal::studio
