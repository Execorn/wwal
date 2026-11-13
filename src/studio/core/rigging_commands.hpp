// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_CORE_RIGGING_COMMANDS_HPP
#define WAYWAL_STUDIO_CORE_RIGGING_COMMANDS_HPP

#include "studio/core/command_bus.hpp"
#include "waywal/ipc_proto_rigging.h"
#include "waywal/we_mesh_cdt.h"
#include "waywal/we_mesh_rig.h"
#include "waywal/we_puppet.h"

#include <QString>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

namespace waywal::studio {

struct PuppetMeshSnapshot {
    int32_t objectId{-1};
    std::vector<we_mdl_vertex_t> vertices;
    std::vector<uint16_t> indices;
    we_mesh_gen_cfg_t cdtConfig{};
    std::vector<we_cdt_pin_t> pins;
};

struct BoneHierarchySnapshot {
    int32_t objectId{-1};
    std::vector<we_mdl_bone_t> bones;
    std::vector<we_ik_chain_t> ikChains;
    std::vector<we_aim_constraint_t> aimConstraints;
    std::array<we_bone_drive_mode_t, WE_MDL_MAX_BONES> driveModes{};
};

struct VertexWeightBlockSnapshot {
    uint32_t blockIndex{0}; /* 256-vertex CoW block index */
    std::vector<we_mdl_vertex_t> slice;
};

struct FCurveKeySnapshot {
    uint32_t animId{0};
    int32_t boneIndex{-1};
    uint32_t channelIndex{0}; /* 0..8: pos.xyz, angle.xyz, scale.xyz */
    double frame{0.0};
    double value{0.0};
    uint8_t tangentMode{0}; /* 0=Auto, 1=Aligned, 2=Vector, 3=Stepped */
    bool handlesBroken{false};
    double handleInDx{-1.0};
    double handleInDy{0.0};
    double handleOutDx{1.0};
    double handleOutDy{0.0};
};

class MeshEditCommand final : public ICommand {
  public:
    using ApplyMeshFn = std::function<void(const PuppetMeshSnapshot &)>;

    MeshEditCommand(QString description, PuppetMeshSnapshot oldMesh, PuppetMeshSnapshot newMesh,
                    ApplyMeshFn applyFn);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;
    bool mergeWith(const ICommand *other) override;

  private:
    QString m_description;
    PuppetMeshSnapshot m_oldMesh;
    PuppetMeshSnapshot m_newMesh;
    ApplyMeshFn m_applyFn;
    std::chrono::steady_clock::time_point m_timestamp;
};

class BoneEditCommand final : public ICommand {
  public:
    using ApplySkeletonFn = std::function<void(const BoneHierarchySnapshot &)>;

    BoneEditCommand(QString description, BoneHierarchySnapshot oldRig, BoneHierarchySnapshot newRig,
                    ApplySkeletonFn applyFn);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;
    bool mergeWith(const ICommand *other) override;

  private:
    QString m_description;
    BoneHierarchySnapshot m_oldRig;
    BoneHierarchySnapshot m_newRig;
    ApplySkeletonFn m_applyFn;
    std::chrono::steady_clock::time_point m_timestamp;
};

class IkPoseCommand final : public ICommand {
  public:
    using ApplyIkPoseFn =
        std::function<void(int32_t objectId, const we_ik_chain_t &chain,
                           const std::vector<waywal_ipc_bone_trs_t> &poseOverrides)>;

    IkPoseCommand(int32_t objectId, uint32_t chainIndex, we_ik_chain_t oldChain,
                  we_ik_chain_t newChain, std::vector<waywal_ipc_bone_trs_t> oldPose,
                  std::vector<waywal_ipc_bone_trs_t> newPose, ApplyIkPoseFn applyFn);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;
    bool mergeWith(const ICommand *other) override;

  private:
    int32_t m_objectId{-1};
    uint32_t m_chainIndex{0};
    we_ik_chain_t m_oldChain{};
    we_ik_chain_t m_newChain{};
    std::vector<waywal_ipc_bone_trs_t> m_oldPose;
    std::vector<waywal_ipc_bone_trs_t> m_newPose;
    ApplyIkPoseFn m_applyFn;
    std::chrono::steady_clock::time_point m_timestamp;
};

class WeightPaintStrokeCommand final : public ICommand {
  public:
    using ApplyBlocksFn =
        std::function<void(int32_t objectId, const std::vector<VertexWeightBlockSnapshot> &blocks)>;

    WeightPaintStrokeCommand(int32_t objectId, QString label,
                             std::vector<VertexWeightBlockSnapshot> preBlocks,
                             std::vector<VertexWeightBlockSnapshot> postBlocks,
                             ApplyBlocksFn applyFn);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;

  private:
    int32_t m_objectId{-1};
    QString m_label;
    std::vector<VertexWeightBlockSnapshot> m_preBlocks;
    std::vector<VertexWeightBlockSnapshot> m_postBlocks;
    ApplyBlocksFn m_applyFn;
};

class KeyframeEditCommand final : public ICommand {
  public:
    using ApplyKeysFn = std::function<void(int32_t objectId, uint32_t animId,
                                           const std::vector<FCurveKeySnapshot> &keys)>;

    KeyframeEditCommand(int32_t objectId, uint32_t animId, QString label,
                        std::vector<FCurveKeySnapshot> oldKeys,
                        std::vector<FCurveKeySnapshot> newKeys, ApplyKeysFn applyFn);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;
    bool mergeWith(const ICommand *other) override;

  private:
    int32_t m_objectId{-1};
    uint32_t m_animId{0};
    QString m_label;
    std::vector<FCurveKeySnapshot> m_oldKeys;
    std::vector<FCurveKeySnapshot> m_newKeys;
    ApplyKeysFn m_applyFn;
    std::chrono::steady_clock::time_point m_timestamp;
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_CORE_RIGGING_COMMANDS_HPP
