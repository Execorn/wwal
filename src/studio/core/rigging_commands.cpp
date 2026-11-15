// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/core/rigging_commands.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace waywal::studio {

namespace {

constexpr int64_t kCoalesceWindowMs = 500;
constexpr uint32_t kMaxWeightBlocks =
    (WE_MDL_MAX_VERTICES + WE_RIG_COW_BLOCK_VERTS - 1u) / WE_RIG_COW_BLOCK_VERTS;

[[nodiscard]] size_t qStringHeapBytes(const QString &s) noexcept
{
    return static_cast<size_t>(std::max<qsizetype>(0, s.capacity())) * sizeof(QChar);
}

[[nodiscard]] bool isMeshSnapshotValid(const PuppetMeshSnapshot &mesh) noexcept
{
    for (const auto &v : mesh.vertices) {
        for (float f : v.pos) {
            if (!std::isfinite(f)) {
                return false;
            }
        }
        for (float w : v.weight) {
            if (!std::isfinite(w) || w < 0.0f) {
                return false;
            }
        }
        for (float uv : v.texcoord) {
            if (!std::isfinite(uv)) {
                return false;
            }
        }
    }
    if (!mesh.vertices.empty()) {
        for (uint16_t idx : mesh.indices) {
            if (static_cast<size_t>(idx) >= mesh.vertices.size()) {
                return false;
            }
        }
    }
    for (const auto &pin : mesh.pins) {
        if (!std::isfinite(pin.px) || !std::isfinite(pin.py)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isRigSnapshotValid(const BoneHierarchySnapshot &rig) noexcept
{
    if (rig.bones.size() > WE_MDL_MAX_BONES) {
        return false;
    }
    for (const auto &b : rig.bones) {
        if (b.parent != 0xFFFFFFFFu && b.parent >= rig.bones.size()) {
            return false;
        }
        for (float f : b.local_trans) {
            if (!std::isfinite(f)) {
                return false;
            }
        }
    }
    for (const auto &ik : rig.ikChains) {
        if (!rig.bones.empty() &&
            (ik.root_bone >= rig.bones.size() || ik.mid_bone >= rig.bones.size() ||
             ik.end_bone >= rig.bones.size())) {
            return false;
        }
        for (float f : ik.target_world) {
            if (!std::isfinite(f)) {
                return false;
            }
        }
        for (float f : ik.pole_world) {
            if (!std::isfinite(f)) {
                return false;
            }
        }
        if (!std::isfinite(ik.blend_weight)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isPoseOverridesValid(const std::vector<waywal_ipc_bone_trs_t> &pose) noexcept
{
    for (const auto &trs : pose) {
        for (float f : trs.local_pos) {
            if (!std::isfinite(f)) {
                return false;
            }
        }
        for (float f : trs.local_angle) {
            if (!std::isfinite(f)) {
                return false;
            }
        }
        for (float f : trs.local_scale) {
            if (!std::isfinite(f)) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool isWeightBlocksValid(const std::vector<VertexWeightBlockSnapshot> &blocks) noexcept
{
    for (const auto &blk : blocks) {
        if (blk.blockIndex >= kMaxWeightBlocks || blk.slice.size() > WE_RIG_COW_BLOCK_VERTS) {
            return false;
        }
        for (const auto &v : blk.slice) {
            for (float w : v.weight) {
                if (!std::isfinite(w) || w < 0.0f) {
                    return false;
                }
            }
        }
    }
    return true;
}

[[nodiscard]] bool isKeySnapshotValid(const FCurveKeySnapshot &k) noexcept
{
    return std::isfinite(k.frame) && std::isfinite(k.value) && std::isfinite(k.handleInDx) &&
           std::isfinite(k.handleInDy) && std::isfinite(k.handleOutDx) &&
           std::isfinite(k.handleOutDy);
}

[[nodiscard]] size_t meshSnapshotFootprint(const PuppetMeshSnapshot &m) noexcept
{
    return m.vertices.capacity() * sizeof(we_mdl_vertex_t) + m.indices.capacity() * sizeof(uint16_t) +
           m.pins.capacity() * sizeof(we_cdt_pin_t);
}

[[nodiscard]] size_t rigSnapshotFootprint(const BoneHierarchySnapshot &r) noexcept
{
    return r.bones.capacity() * sizeof(we_mdl_bone_t) + r.ikChains.capacity() * sizeof(we_ik_chain_t) +
           r.aimConstraints.capacity() * sizeof(we_aim_constraint_t);
}

[[nodiscard]] size_t weightBlocksFootprint(const std::vector<VertexWeightBlockSnapshot> &blocks) noexcept
{
    size_t bytes = blocks.capacity() * sizeof(VertexWeightBlockSnapshot);
    for (const auto &blk : blocks) {
        bytes += blk.slice.capacity() * sizeof(we_mdl_vertex_t);
    }
    return bytes;
}

[[nodiscard]] bool withinCoalesceWindow(std::chrono::steady_clock::time_point earlier,
                                        std::chrono::steady_clock::time_point later) noexcept
{
    const auto diff =
        std::chrono::duration_cast<std::chrono::milliseconds>(later - earlier).count();
    return diff >= 0 && diff <= kCoalesceWindowMs;
}

[[nodiscard]] std::vector<VertexWeightBlockSnapshot>
sanitizeWeightBlocks(std::vector<VertexWeightBlockSnapshot> blocks, bool keepLastOnDuplicate)
{
    std::vector<VertexWeightBlockSnapshot> out;
    out.reserve(std::min<size_t>(blocks.size(), kMaxWeightBlocks));

    for (auto &blk : blocks) {
        if (blk.blockIndex >= kMaxWeightBlocks) {
            continue;
        }
        if (blk.slice.size() > WE_RIG_COW_BLOCK_VERTS) {
            blk.slice.resize(WE_RIG_COW_BLOCK_VERTS);
        }

        auto existing =
            std::find_if(out.begin(), out.end(), [&](const VertexWeightBlockSnapshot &s) {
                return s.blockIndex == blk.blockIndex;
            });
        if (existing != out.end()) {
            if (keepLastOnDuplicate) {
                *existing = std::move(blk);
            }
        } else {
            out.push_back(std::move(blk));
        }
    }
    return out;
}

} // namespace

/* ========================================================================= */
/* MeshEditCommand                                                           */
/* ========================================================================= */

MeshEditCommand::MeshEditCommand(QString description, PuppetMeshSnapshot oldMesh,
                                 PuppetMeshSnapshot newMesh, ApplyMeshFn applyFn)
    : m_description(std::move(description)), m_oldMesh(std::move(oldMesh)),
      m_newMesh(std::move(newMesh)), m_applyFn(std::move(applyFn)),
      m_timestamp(std::chrono::steady_clock::now())
{
}

void MeshEditCommand::execute()
{
    if (m_applyFn && isValid()) {
        m_applyFn(m_newMesh);
    }
}

void MeshEditCommand::undo()
{
    if (m_applyFn && isValid()) {
        m_applyFn(m_oldMesh);
    }
}

QString MeshEditCommand::describe() const
{
    if (!m_description.isEmpty()) {
        return m_description;
    }
    return QStringLiteral("Edit puppet mesh (#%1)").arg(m_newMesh.objectId);
}

bool MeshEditCommand::mergeWith(const ICommand *other)
{
    const auto *rhs = dynamic_cast<const MeshEditCommand *>(other);
    if (!rhs || !isValid() || !rhs->isValid() || m_newMesh.objectId != rhs->m_newMesh.objectId ||
        m_oldMesh.objectId != rhs->m_oldMesh.objectId || m_description != rhs->m_description) {
        return false;
    }

    // Keep pin additions/removals as distinct undo steps even within the drag window.
    if (m_oldMesh.pins.size() != m_newMesh.pins.size() ||
        rhs->m_oldMesh.pins.size() != rhs->m_newMesh.pins.size()) {
        return false;
    }

    if (!withinCoalesceWindow(m_timestamp, rhs->m_timestamp)) {
        return false;
    }

    m_newMesh = rhs->m_newMesh;
    m_timestamp = rhs->m_timestamp;
    if (rhs->m_applyFn) {
        m_applyFn = rhs->m_applyFn;
    }
    return true;
}

size_t MeshEditCommand::memoryFootprint() const
{
    return sizeof(*this) + qStringHeapBytes(m_description) + meshSnapshotFootprint(m_oldMesh) +
           meshSnapshotFootprint(m_newMesh);
}

bool MeshEditCommand::isValid() const
{
    return m_oldMesh.objectId == m_newMesh.objectId && isMeshSnapshotValid(m_oldMesh) &&
           isMeshSnapshotValid(m_newMesh);
}

/* ========================================================================= */
/* BoneEditCommand                                                           */
/* ========================================================================= */

BoneEditCommand::BoneEditCommand(QString description, BoneHierarchySnapshot oldRig,
                                 BoneHierarchySnapshot newRig, ApplySkeletonFn applyFn)
    : m_description(std::move(description)), m_oldRig(std::move(oldRig)),
      m_newRig(std::move(newRig)), m_applyFn(std::move(applyFn)),
      m_timestamp(std::chrono::steady_clock::now())
{
}

void BoneEditCommand::execute()
{
    if (m_applyFn && isValid()) {
        m_applyFn(m_newRig);
    }
}

void BoneEditCommand::undo()
{
    if (m_applyFn && isValid()) {
        m_applyFn(m_oldRig);
    }
}

QString BoneEditCommand::describe() const
{
    if (!m_description.isEmpty()) {
        return m_description;
    }
    return QStringLiteral("Edit skeleton rig (#%1)").arg(m_newRig.objectId);
}

bool BoneEditCommand::mergeWith(const ICommand *other)
{
    const auto *rhs = dynamic_cast<const BoneEditCommand *>(other);
    if (!rhs || !isValid() || !rhs->isValid() || m_newRig.objectId != rhs->m_newRig.objectId ||
        m_oldRig.objectId != rhs->m_oldRig.objectId || m_description != rhs->m_description) {
        return false;
    }

    // Structural hierarchy edits (bone/chain add, delete, reparent) must never coalesce.
    if (m_oldRig.bones.size() != m_newRig.bones.size() ||
        rhs->m_oldRig.bones.size() != rhs->m_newRig.bones.size() ||
        m_newRig.bones.size() != rhs->m_newRig.bones.size() ||
        m_oldRig.ikChains.size() != m_newRig.ikChains.size() ||
        rhs->m_oldRig.ikChains.size() != rhs->m_newRig.ikChains.size() ||
        m_oldRig.aimConstraints.size() != m_newRig.aimConstraints.size() ||
        rhs->m_oldRig.aimConstraints.size() != rhs->m_newRig.aimConstraints.size()) {
        return false;
    }

    for (size_t i = 0; i < m_newRig.bones.size(); ++i) {
        if (m_oldRig.bones[i].parent != m_newRig.bones[i].parent ||
            rhs->m_oldRig.bones[i].parent != rhs->m_newRig.bones[i].parent) {
            return false;
        }
    }

    if (!withinCoalesceWindow(m_timestamp, rhs->m_timestamp)) {
        return false;
    }

    m_newRig = rhs->m_newRig;
    m_timestamp = rhs->m_timestamp;
    if (rhs->m_applyFn) {
        m_applyFn = rhs->m_applyFn;
    }
    return true;
}

size_t BoneEditCommand::memoryFootprint() const
{
    return sizeof(*this) + qStringHeapBytes(m_description) + rigSnapshotFootprint(m_oldRig) +
           rigSnapshotFootprint(m_newRig);
}

bool BoneEditCommand::isValid() const
{
    return m_oldRig.objectId == m_newRig.objectId && isRigSnapshotValid(m_oldRig) &&
           isRigSnapshotValid(m_newRig);
}

/* ========================================================================= */
/* IkPoseCommand                                                             */
/* ========================================================================= */

IkPoseCommand::IkPoseCommand(int32_t objectId, uint32_t chainIndex, we_ik_chain_t oldChain,
                             we_ik_chain_t newChain, std::vector<waywal_ipc_bone_trs_t> oldPose,
                             std::vector<waywal_ipc_bone_trs_t> newPose, ApplyIkPoseFn applyFn)
    : m_objectId(objectId), m_chainIndex(chainIndex), m_oldChain(oldChain), m_newChain(newChain),
      m_oldPose(std::move(oldPose)), m_newPose(std::move(newPose)), m_applyFn(std::move(applyFn)),
      m_timestamp(std::chrono::steady_clock::now())
{
}

void IkPoseCommand::execute()
{
    if (m_applyFn && isValid()) {
        m_applyFn(m_objectId, m_newChain, m_newPose);
    }
}

void IkPoseCommand::undo()
{
    if (m_applyFn && isValid()) {
        m_applyFn(m_objectId, m_oldChain, m_oldPose);
    }
}

QString IkPoseCommand::describe() const
{
    return QStringLiteral("Adjust IK pose (chain #%1)").arg(m_chainIndex);
}

bool IkPoseCommand::mergeWith(const ICommand *other)
{
    const auto *rhs = dynamic_cast<const IkPoseCommand *>(other);
    if (!rhs || !isValid() || !rhs->isValid() || m_objectId != rhs->m_objectId ||
        m_chainIndex != rhs->m_chainIndex) {
        return false;
    }

    if (!withinCoalesceWindow(m_timestamp, rhs->m_timestamp)) {
        return false;
    }

    m_newChain = rhs->m_newChain;
    m_newPose = rhs->m_newPose;
    m_timestamp = rhs->m_timestamp;
    if (rhs->m_applyFn) {
        m_applyFn = rhs->m_applyFn;
    }
    return true;
}

size_t IkPoseCommand::memoryFootprint() const
{
    return sizeof(*this) +
           (m_oldPose.capacity() + m_newPose.capacity()) * sizeof(waywal_ipc_bone_trs_t);
}

bool IkPoseCommand::isValid() const
{
    auto validChain = [](const we_ik_chain_t &c) {
        for (float f : c.target_world) {
            if (!std::isfinite(f)) {
                return false;
            }
        }
        for (float f : c.pole_world) {
            if (!std::isfinite(f)) {
                return false;
            }
        }
        return std::isfinite(c.blend_weight);
    };
    return m_objectId >= 0 && validChain(m_oldChain) && validChain(m_newChain) &&
           isPoseOverridesValid(m_oldPose) && isPoseOverridesValid(m_newPose);
}

/* ========================================================================= */
/* WeightPaintStrokeCommand                                                  */
/* ========================================================================= */

WeightPaintStrokeCommand::WeightPaintStrokeCommand(
    int32_t objectId, QString label, std::vector<VertexWeightBlockSnapshot> preBlocks,
    std::vector<VertexWeightBlockSnapshot> postBlocks, ApplyBlocksFn applyFn)
    : m_objectId(objectId), m_label(std::move(label)),
      m_preBlocks(sanitizeWeightBlocks(std::move(preBlocks), false)),
      m_postBlocks(sanitizeWeightBlocks(std::move(postBlocks), true)), m_applyFn(std::move(applyFn))
{
}

void WeightPaintStrokeCommand::execute()
{
    if (m_applyFn && isValid()) {
        m_applyFn(m_objectId, m_postBlocks);
    }
}

void WeightPaintStrokeCommand::undo()
{
    if (m_applyFn && isValid()) {
        m_applyFn(m_objectId, m_preBlocks);
    }
}

QString WeightPaintStrokeCommand::describe() const
{
    if (!m_label.isEmpty()) {
        return m_label;
    }
    return QStringLiteral("Weight paint stroke (%1 CoW blocks)").arg(m_postBlocks.size());
}

size_t WeightPaintStrokeCommand::memoryFootprint() const
{
    return sizeof(*this) + qStringHeapBytes(m_label) + weightBlocksFootprint(m_preBlocks) +
           weightBlocksFootprint(m_postBlocks);
}

bool WeightPaintStrokeCommand::isValid() const
{
    return m_objectId >= 0 && m_preBlocks.size() == m_postBlocks.size() &&
           isWeightBlocksValid(m_preBlocks) && isWeightBlocksValid(m_postBlocks);
}

/* ========================================================================= */
/* KeyframeEditCommand                                                       */
/* ========================================================================= */

KeyframeEditCommand::KeyframeEditCommand(int32_t objectId, uint32_t animId, QString label,
                                         std::vector<FCurveKeySnapshot> oldKeys,
                                         std::vector<FCurveKeySnapshot> newKeys,
                                         ApplyKeysFn applyFn)
    : m_objectId(objectId), m_animId(animId), m_label(std::move(label)),
      m_oldKeys(std::move(oldKeys)), m_newKeys(std::move(newKeys)), m_applyFn(std::move(applyFn)),
      m_timestamp(std::chrono::steady_clock::now())
{
}

void KeyframeEditCommand::execute()
{
    if (m_applyFn && isValid()) {
        m_applyFn(m_objectId, m_animId, m_newKeys);
    }
}

void KeyframeEditCommand::undo()
{
    if (m_applyFn && isValid()) {
        m_applyFn(m_objectId, m_animId, m_oldKeys);
    }
}

QString KeyframeEditCommand::describe() const
{
    if (!m_label.isEmpty()) {
        return m_label;
    }
    return QStringLiteral("Edit keyframes (anim #%1)").arg(m_animId);
}

bool KeyframeEditCommand::mergeWith(const ICommand *other)
{
    const auto *rhs = dynamic_cast<const KeyframeEditCommand *>(other);
    if (!rhs || !isValid() || !rhs->isValid() || m_objectId != rhs->m_objectId ||
        m_animId != rhs->m_animId || m_label != rhs->m_label) {
        return false;
    }

    if (m_oldKeys.size() != m_newKeys.size() || rhs->m_oldKeys.size() != rhs->m_newKeys.size() ||
        m_newKeys.size() != rhs->m_newKeys.size() || m_newKeys.empty()) {
        return false;
    }

    for (size_t i = 0; i < m_newKeys.size(); ++i) {
        if (m_oldKeys[i].boneIndex != rhs->m_oldKeys[i].boneIndex ||
            m_oldKeys[i].channelIndex != rhs->m_oldKeys[i].channelIndex ||
            m_newKeys[i].boneIndex != rhs->m_newKeys[i].boneIndex ||
            m_newKeys[i].channelIndex != rhs->m_newKeys[i].channelIndex) {
            return false;
        }
        // Reject coalescing edits targeting distinct keyframes on the same bone/channel.
        if (std::abs(m_newKeys[i].frame - rhs->m_oldKeys[i].frame) > 1e-4) {
            return false;
        }
        const bool lhsFrameFixed = (std::abs(m_oldKeys[i].frame - m_newKeys[i].frame) <= 1e-4);
        const bool rhsFrameFixed =
            (std::abs(rhs->m_oldKeys[i].frame - rhs->m_newKeys[i].frame) <= 1e-4);
        if (lhsFrameFixed != rhsFrameFixed) {
            return false;
        }
    }

    if (!withinCoalesceWindow(m_timestamp, rhs->m_timestamp)) {
        return false;
    }

    m_newKeys = rhs->m_newKeys;
    m_timestamp = rhs->m_timestamp;
    if (rhs->m_applyFn) {
        m_applyFn = rhs->m_applyFn;
    }
    return true;
}

size_t KeyframeEditCommand::memoryFootprint() const
{
    return sizeof(*this) + qStringHeapBytes(m_label) +
           (m_oldKeys.capacity() + m_newKeys.capacity()) * sizeof(FCurveKeySnapshot);
}

bool KeyframeEditCommand::isValid() const
{
    return std::all_of(m_oldKeys.begin(), m_oldKeys.end(), isKeySnapshotValid) &&
           std::all_of(m_newKeys.begin(), m_newKeys.end(), isKeySnapshotValid);
}

} // namespace waywal::studio
