// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_COMMAND_BUS_AUTHORING_HPP
#define WAYWAL_STUDIO_COMMAND_BUS_AUTHORING_HPP

#include "studio/core/command_bus.hpp"
#include "waywal/ipc_proto_authoring.h"
#include "waywal/we_gpu_brush.h"

#include <QString>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace waywal::studio {

struct SceneLayerSnapshot {
    int32_t objectId{-1};
    int32_t parentId{-1};
    uint32_t zOrderIndex{0};
    we_scene_node_kind_t kind{WE_NODE_IMAGE};
    uint32_t blendMode{0};
    bool visible{true};
    bool locked{false};
    bool fullscreen{false};
    bool passthrough{false};
    std::array<float, 3> origin{0.0f, 0.0f, 0.0f};
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
    std::array<float, 3> angles{0.0f, 0.0f, 0.0f};
    std::array<float, 2> size{1920.0f, 1080.0f};
    std::array<float, 2> parallaxDepth{0.0f, 0.0f};
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
    QString name;
};

class AddSceneLayerCommand final : public ICommand {
  public:
    using MutateFn = std::function<void(const SceneLayerSnapshot &, bool isAdd)>;

    AddSceneLayerCommand(SceneLayerSnapshot layer, MutateFn mutateFn);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;
    [[nodiscard]] int32_t objectId() const noexcept { return m_layer.objectId; }

  private:
    SceneLayerSnapshot m_layer;
    MutateFn m_mutateFn;
};

class RemoveSceneLayerCommand final : public ICommand {
  public:
    using RestoreFn =
        std::function<void(const SceneLayerSnapshot &,
                           const std::vector<int32_t> &reparentedChildren, bool isRestore)>;

    RemoveSceneLayerCommand(SceneLayerSnapshot removedLayer, std::vector<int32_t> childIds,
                            RestoreFn restoreFn);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;

  private:
    SceneLayerSnapshot m_removedLayer;
    std::vector<int32_t> m_childIds;
    RestoreFn m_restoreFn;
};

class ReorderSceneLayerCommand final : public ICommand {
  public:
    using ReorderFn = std::function<void(int32_t objectId, uint32_t zOrderIndex)>;

    ReorderSceneLayerCommand(int32_t objectId, QString layerName, uint32_t oldZ, uint32_t newZ,
                             ReorderFn reorderFn);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;

  private:
    int32_t m_objectId{-1};
    QString m_layerName;
    uint32_t m_oldZ{0};
    uint32_t m_newZ{0};
    ReorderFn m_reorderFn;
};

class ReparentSceneLayerCommand final : public ICommand {
  public:
    using ReparentFn =
        std::function<void(int32_t objectId, int32_t parentId, const std::array<float, 3> &origin,
                           const std::array<float, 3> &scale, const std::array<float, 3> &angles)>;

    ReparentSceneLayerCommand(int32_t objectId, QString layerName, int32_t oldParentId,
                              int32_t newParentId, std::array<float, 3> oldOrigin,
                              std::array<float, 3> newOrigin, std::array<float, 3> oldScale,
                              std::array<float, 3> newScale, std::array<float, 3> oldAngles,
                              std::array<float, 3> newAngles, ReparentFn reparentFn);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;

  private:
    int32_t m_objectId{-1};
    QString m_layerName;
    int32_t m_oldParentId{-1};
    int32_t m_newParentId{-1};
    std::array<float, 3> m_oldOrigin{};
    std::array<float, 3> m_newOrigin{};
    std::array<float, 3> m_oldScale{};
    std::array<float, 3> m_newScale{};
    std::array<float, 3> m_oldAngles{};
    std::array<float, 3> m_newAngles{};
    ReparentFn m_reparentFn;
};

class SetLayerTransformCommand final : public ICommand {
  public:
    struct TransformState {
        std::array<float, 3> origin{0.0f, 0.0f, 0.0f};
        std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
        std::array<float, 3> angles{0.0f, 0.0f, 0.0f};
        std::array<float, 2> size{1920.0f, 1080.0f};
        std::array<float, 2> parallaxDepth{0.0f, 0.0f};
    };
    using ApplyTransformFn = std::function<void(int32_t objectId, const TransformState &)>;

    SetLayerTransformCommand(int32_t objectId, QString layerName, TransformState oldState,
                             TransformState newState, ApplyTransformFn applyFn);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;
    bool mergeWith(const ICommand *other) override; /* Coalesces gizmo drags within 500 ms */

    [[nodiscard]] int32_t objectId() const noexcept { return m_objectId; }
    [[nodiscard]] const TransformState &newState() const noexcept { return m_newState; }

  private:
    int32_t m_objectId{-1};
    QString m_layerName;
    TransformState m_oldState;
    TransformState m_newState;
    ApplyTransformFn m_applyFn;
    std::chrono::steady_clock::time_point m_timestamp;
};

class SetLayerFlagsCommand final : public ICommand {
  public:
    struct LayerFlagState {
        bool visible{true};
        bool locked{false};
        uint32_t blendMode{0};
        float alpha{1.0f};
        std::array<float, 3> color{1.0f, 1.0f, 1.0f};
    };
    using ApplyFlagsFn = std::function<void(int32_t objectId, const LayerFlagState &)>;

    SetLayerFlagsCommand(int32_t objectId, QString layerName, LayerFlagState oldFlags,
                         LayerFlagState newFlags, ApplyFlagsFn applyFn);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;

  private:
    int32_t m_objectId{-1};
    QString m_layerName;
    LayerFlagState m_oldFlags;
    LayerFlagState m_newFlags;
    ApplyFlagsFn m_applyFn;
};

class BrushStrokeCommand final : public ICommand {
  public:
    using StrokeHistoryFn = std::function<void(uint64_t strokeId, bool isRedo)>;

    BrushStrokeCommand(uint64_t strokeId, we_brush_channel_t channel, uint32_t touchedTileCount,
                       StrokeHistoryFn historyFn);

    void execute() override;
    void undo() override;
    void redo() override;
    [[nodiscard]] QString describe() const override;
    [[nodiscard]] uint64_t strokeId() const noexcept { return m_strokeId; }

  private:
    uint64_t m_strokeId{0};
    we_brush_channel_t m_channel{WE_BRUSH_CHAN_MASK_R8};
    uint32_t m_touchedTileCount{0};
    StrokeHistoryFn m_historyFn;
    bool m_initialApplied{false};
};

class EditParticleSystemCommand final : public ICommand {
  public:
    using ApplyJsonFn = std::function<void(int32_t objectId, const QString &jsonState)>;

    EditParticleSystemCommand(int32_t objectId, QString paramGroup, QString oldJson,
                              QString newJson, ApplyJsonFn applyFn);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;
    bool mergeWith(const ICommand *other) override; /* Coalesces curve/slider drags within 250 ms */

    [[nodiscard]] int32_t objectId() const noexcept { return m_objectId; }
    [[nodiscard]] const QString &paramGroup() const noexcept { return m_paramGroup; }

  private:
    int32_t m_objectId{-1};
    QString m_paramGroup;
    QString m_oldJson;
    QString m_newJson;
    ApplyJsonFn m_applyFn;
    std::chrono::steady_clock::time_point m_timestamp;
};

class SetAudioBindingCommand final : public ICommand {
  public:
    using ApplyBindingFn =
        std::function<void(const std::optional<we_audio_binding_t> &binding, uint32_t removeId)>;

    SetAudioBindingCommand(std::optional<we_audio_binding_t> oldBinding,
                           std::optional<we_audio_binding_t> newBinding, ApplyBindingFn applyFn);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;

  private:
    std::optional<we_audio_binding_t> m_oldBinding;
    std::optional<we_audio_binding_t> m_newBinding;
    ApplyBindingFn m_applyFn;
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_COMMAND_BUS_AUTHORING_HPP
