// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/core/command_bus_authoring.hpp"

#include <utility>

namespace waywal::studio {

namespace {

[[nodiscard]] QString layerLabel(const QString &name, int32_t objectId)
{
    if (!name.isEmpty()) {
        return name;
    }
    return QStringLiteral("#%1").arg(objectId);
}

[[nodiscard]] QString brushChannelLabel(we_brush_channel_t channel)
{
    switch (channel) {
    case WE_BRUSH_CHAN_MASK_R8:
        return QStringLiteral("Mask");
    case WE_BRUSH_CHAN_FLOW_RG88:
        return QStringLiteral("Flowmap");
    case WE_BRUSH_CHAN_COLOR_RGBA8:
        return QStringLiteral("Paint");
    case WE_BRUSH_CHAN_BLUR:
        return QStringLiteral("Blur");
    case WE_BRUSH_CHAN_SMUDGE:
        return QStringLiteral("Smudge");
    }
    return QStringLiteral("Brush");
}

} // namespace

AddSceneLayerCommand::AddSceneLayerCommand(SceneLayerSnapshot layer, MutateFn mutateFn)
    : m_layer(std::move(layer)), m_mutateFn(std::move(mutateFn))
{
}

void AddSceneLayerCommand::execute()
{
    if (m_mutateFn) {
        m_mutateFn(m_layer, true);
    }
}

void AddSceneLayerCommand::undo()
{
    if (m_mutateFn) {
        m_mutateFn(m_layer, false);
    }
}

QString AddSceneLayerCommand::describe() const
{
    return QStringLiteral("Add layer '%1'").arg(layerLabel(m_layer.name, m_layer.objectId));
}

RemoveSceneLayerCommand::RemoveSceneLayerCommand(SceneLayerSnapshot removedLayer,
                                                 std::vector<int32_t> childIds, RestoreFn restoreFn)
    : m_removedLayer(std::move(removedLayer)), m_childIds(std::move(childIds)),
      m_restoreFn(std::move(restoreFn))
{
}

void RemoveSceneLayerCommand::execute()
{
    if (m_restoreFn) {
        m_restoreFn(m_removedLayer, m_childIds, false);
    }
}

void RemoveSceneLayerCommand::undo()
{
    if (m_restoreFn) {
        m_restoreFn(m_removedLayer, m_childIds, true);
    }
}

QString RemoveSceneLayerCommand::describe() const
{
    return QStringLiteral("Remove layer '%1'")
        .arg(layerLabel(m_removedLayer.name, m_removedLayer.objectId));
}

ReorderSceneLayerCommand::ReorderSceneLayerCommand(int32_t objectId, QString layerName,
                                                   uint32_t oldZ, uint32_t newZ,
                                                   ReorderFn reorderFn)
    : m_objectId(objectId), m_layerName(std::move(layerName)), m_oldZ(oldZ), m_newZ(newZ),
      m_reorderFn(std::move(reorderFn))
{
}

void ReorderSceneLayerCommand::execute()
{
    if (m_reorderFn) {
        m_reorderFn(m_objectId, m_newZ);
    }
}

void ReorderSceneLayerCommand::undo()
{
    if (m_reorderFn) {
        m_reorderFn(m_objectId, m_oldZ);
    }
}

QString ReorderSceneLayerCommand::describe() const
{
    return QStringLiteral("Reorder '%1' to Z %2")
        .arg(layerLabel(m_layerName, m_objectId))
        .arg(m_newZ);
}

ReparentSceneLayerCommand::ReparentSceneLayerCommand(
    int32_t objectId, QString layerName, int32_t oldParentId, int32_t newParentId,
    std::array<float, 3> oldOrigin, std::array<float, 3> newOrigin, std::array<float, 3> oldScale,
    std::array<float, 3> newScale, std::array<float, 3> oldAngles, std::array<float, 3> newAngles,
    ReparentFn reparentFn)
    : m_objectId(objectId), m_layerName(std::move(layerName)), m_oldParentId(oldParentId),
      m_newParentId(newParentId), m_oldOrigin(oldOrigin), m_newOrigin(newOrigin),
      m_oldScale(oldScale), m_newScale(newScale), m_oldAngles(oldAngles), m_newAngles(newAngles),
      m_reparentFn(std::move(reparentFn))
{
}

void ReparentSceneLayerCommand::execute()
{
    if (m_reparentFn) {
        m_reparentFn(m_objectId, m_newParentId, m_newOrigin, m_newScale, m_newAngles);
    }
}

void ReparentSceneLayerCommand::undo()
{
    if (m_reparentFn) {
        m_reparentFn(m_objectId, m_oldParentId, m_oldOrigin, m_oldScale, m_oldAngles);
    }
}

QString ReparentSceneLayerCommand::describe() const
{
    return QStringLiteral("Reparent '%1'").arg(layerLabel(m_layerName, m_objectId));
}

SetLayerTransformCommand::SetLayerTransformCommand(int32_t objectId, QString layerName,
                                                   TransformState oldState, TransformState newState,
                                                   ApplyTransformFn applyFn)
    : m_objectId(objectId), m_layerName(std::move(layerName)), m_oldState(oldState),
      m_newState(newState), m_applyFn(std::move(applyFn)),
      m_timestamp(std::chrono::steady_clock::now())
{
}

void SetLayerTransformCommand::execute()
{
    if (m_applyFn) {
        m_applyFn(m_objectId, m_newState);
    }
}

void SetLayerTransformCommand::undo()
{
    if (m_applyFn) {
        m_applyFn(m_objectId, m_oldState);
    }
}

QString SetLayerTransformCommand::describe() const
{
    return QStringLiteral("Transform '%1'").arg(layerLabel(m_layerName, m_objectId));
}

bool SetLayerTransformCommand::mergeWith(const ICommand *other)
{
    const auto *rhs = dynamic_cast<const SetLayerTransformCommand *>(other);
    if (!rhs || rhs->m_objectId != m_objectId) {
        return false;
    }
    const auto delta =
        std::chrono::duration_cast<std::chrono::milliseconds>(rhs->m_timestamp - m_timestamp);
    if (delta.count() < 0 || delta > std::chrono::milliseconds(500)) {
        return false;
    }
    m_newState = rhs->m_newState;
    m_timestamp = rhs->m_timestamp;
    if (rhs->m_applyFn) {
        m_applyFn = rhs->m_applyFn;
    }
    return true;
}

SetLayerFlagsCommand::SetLayerFlagsCommand(int32_t objectId, QString layerName,
                                           LayerFlagState oldFlags, LayerFlagState newFlags,
                                           ApplyFlagsFn applyFn)
    : m_objectId(objectId), m_layerName(std::move(layerName)), m_oldFlags(oldFlags),
      m_newFlags(newFlags), m_applyFn(std::move(applyFn))
{
}

void SetLayerFlagsCommand::execute()
{
    if (m_applyFn) {
        m_applyFn(m_objectId, m_newFlags);
    }
}

void SetLayerFlagsCommand::undo()
{
    if (m_applyFn) {
        m_applyFn(m_objectId, m_oldFlags);
    }
}

QString SetLayerFlagsCommand::describe() const
{
    return QStringLiteral("Update properties '%1'").arg(layerLabel(m_layerName, m_objectId));
}

BrushStrokeCommand::BrushStrokeCommand(uint64_t strokeId, we_brush_channel_t channel,
                                       uint32_t touchedTileCount, StrokeHistoryFn historyFn)
    : m_strokeId(strokeId), m_channel(channel), m_touchedTileCount(touchedTileCount),
      m_historyFn(std::move(historyFn))
{
}

void BrushStrokeCommand::execute()
{
    /* Interactive brush strokes are already stamped into VRAM on release; only repeat calls
     * re-apply. */
    if (!m_initialApplied) {
        m_initialApplied = true;
        return;
    }
    redo();
}

void BrushStrokeCommand::undo()
{
    if (m_historyFn) {
        m_historyFn(m_strokeId, false);
    }
}

void BrushStrokeCommand::redo()
{
    m_initialApplied = true;
    if (m_historyFn) {
        m_historyFn(m_strokeId, true);
    }
}

QString BrushStrokeCommand::describe() const
{
    return QStringLiteral("%1 stroke (%2 tiles)")
        .arg(brushChannelLabel(m_channel))
        .arg(m_touchedTileCount);
}

EditParticleSystemCommand::EditParticleSystemCommand(int32_t objectId, QString paramGroup,
                                                     QString oldJson, QString newJson,
                                                     ApplyJsonFn applyFn)
    : m_objectId(objectId), m_paramGroup(std::move(paramGroup)), m_oldJson(std::move(oldJson)),
      m_newJson(std::move(newJson)), m_applyFn(std::move(applyFn)),
      m_timestamp(std::chrono::steady_clock::now())
{
}

void EditParticleSystemCommand::execute()
{
    if (m_applyFn) {
        m_applyFn(m_objectId, m_newJson);
    }
}

void EditParticleSystemCommand::undo()
{
    if (m_applyFn) {
        m_applyFn(m_objectId, m_oldJson);
    }
}

QString EditParticleSystemCommand::describe() const
{
    if (m_paramGroup.isEmpty()) {
        return QStringLiteral("Edit particle #%1").arg(m_objectId);
    }
    return QStringLiteral("Edit particle %1 (#%2)").arg(m_paramGroup).arg(m_objectId);
}

bool EditParticleSystemCommand::mergeWith(const ICommand *other)
{
    const auto *rhs = dynamic_cast<const EditParticleSystemCommand *>(other);
    if (!rhs || rhs->m_objectId != m_objectId || rhs->m_paramGroup != m_paramGroup) {
        return false;
    }
    const auto delta =
        std::chrono::duration_cast<std::chrono::milliseconds>(rhs->m_timestamp - m_timestamp);
    if (delta.count() < 0 || delta > std::chrono::milliseconds(250)) {
        return false;
    }
    m_newJson = rhs->m_newJson;
    m_timestamp = rhs->m_timestamp;
    if (rhs->m_applyFn) {
        m_applyFn = rhs->m_applyFn;
    }
    return true;
}

SetAudioBindingCommand::SetAudioBindingCommand(std::optional<we_audio_binding_t> oldBinding,
                                               std::optional<we_audio_binding_t> newBinding,
                                               ApplyBindingFn applyFn)
    : m_oldBinding(oldBinding), m_newBinding(newBinding), m_applyFn(std::move(applyFn))
{
}

void SetAudioBindingCommand::execute()
{
    if (!m_applyFn) {
        return;
    }
    if (m_newBinding.has_value()) {
        m_applyFn(m_newBinding, 0u);
    } else if (m_oldBinding.has_value()) {
        m_applyFn(std::nullopt, m_oldBinding->binding_id);
    }
}

void SetAudioBindingCommand::undo()
{
    if (!m_applyFn) {
        return;
    }
    if (m_oldBinding.has_value()) {
        m_applyFn(m_oldBinding, 0u);
    } else if (m_newBinding.has_value()) {
        m_applyFn(std::nullopt, m_newBinding->binding_id);
    }
}

QString SetAudioBindingCommand::describe() const
{
    if (!m_oldBinding.has_value() && m_newBinding.has_value()) {
        return QStringLiteral("Add audio binding #%1").arg(m_newBinding->binding_id);
    }
    if (m_oldBinding.has_value() && !m_newBinding.has_value()) {
        return QStringLiteral("Remove audio binding #%1").arg(m_oldBinding->binding_id);
    }
    if (m_newBinding.has_value()) {
        return QStringLiteral("Update audio binding #%1").arg(m_newBinding->binding_id);
    }
    return QStringLiteral("Audio binding change");
}

} // namespace waywal::studio
