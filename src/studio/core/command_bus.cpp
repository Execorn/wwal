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

} // namespace waywal::studio
