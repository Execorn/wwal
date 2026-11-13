// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/core/rigging_commands.hpp"

#include <algorithm>
#include <utility>

namespace waywal::studio {

namespace {

constexpr int64_t kCoalesceWindowMs = 500;
constexpr uint32_t kMaxWeightBlocks =
    (WE_MDL_MAX_VERTICES + WE_RIG_COW_BLOCK_VERTS - 1u) / WE_RIG_COW_BLOCK_VERTS;

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
    if (m_applyFn) {
        m_applyFn(m_newMesh);
    }
}

void MeshEditCommand::undo()
{
    if (m_applyFn) {
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
    if (!rhs || m_newMesh.objectId != rhs->m_newMesh.objectId ||
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
    if (m_applyFn) {
        m_applyFn(m_newRig);
    }
}

void BoneEditCommand::undo()
{
    if (m_applyFn) {
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
    if (!rhs || m_newRig.objectId != rhs->m_newRig.objectId ||
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
    if (m_applyFn) {
        m_applyFn(m_objectId, m_newChain, m_newPose);
    }
}

void IkPoseCommand::undo()
{
    if (m_applyFn) {
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
    if (!rhs || m_objectId != rhs->m_objectId || m_chainIndex != rhs->m_chainIndex) {
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
    if (m_applyFn) {
        m_applyFn(m_objectId, m_postBlocks);
    }
}

void WeightPaintStrokeCommand::undo()
{
    if (m_applyFn) {
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
    if (m_applyFn) {
        m_applyFn(m_objectId, m_animId, m_newKeys);
    }
}

void KeyframeEditCommand::undo()
{
    if (m_applyFn) {
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
    if (!rhs || m_objectId != rhs->m_objectId || m_animId != rhs->m_animId ||
        m_label != rhs->m_label) {
        return false;
    }

    if (m_oldKeys.size() != m_newKeys.size() || rhs->m_oldKeys.size() != rhs->m_newKeys.size() ||
        m_newKeys.size() != rhs->m_newKeys.size()) {
        return false;
    }

    for (size_t i = 0; i < m_newKeys.size(); ++i) {
        if (m_newKeys[i].boneIndex != rhs->m_newKeys[i].boneIndex ||
            m_newKeys[i].channelIndex != rhs->m_newKeys[i].channelIndex) {
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

} // namespace waywal::studio
