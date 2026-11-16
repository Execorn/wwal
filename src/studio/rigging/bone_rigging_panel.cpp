// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/rigging/bone_rigging_panel.hpp"

#include "waywal/ipc_proto_rigging.h"
#include "waywal/we_physics.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDropEvent>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMetaMethod>
#include <QPointer>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <numeric>
#include <unordered_map>
#include <utility>
#include <vector>

namespace waywal::studio {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kRadToDeg = 180.0f / kPi;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kMinIkSegmentLen = 1e-4f;

float finiteOr(float v, float fallback) noexcept { return std::isfinite(v) ? v : fallback; }

void setIdentityMat4(float m[16]) noexcept
{
    std::memset(m, 0, sizeof(float) * 16u);
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
}

void mat4FromTrs(const float pos[3], const float angle[3], const float scale[3],
                 float out[16]) noexcept
{
    const float cx = std::cos(angle[0]);
    const float sx = std::sin(angle[0]);
    const float cy = std::cos(angle[1]);
    const float sy = std::sin(angle[1]);
    const float cz = std::cos(angle[2]);
    const float sz = std::sin(angle[2]);

    const float scx = scale[0];
    const float scy = scale[1];
    const float scz = scale[2];

    out[0] = (cz * cy) * scx;
    out[1] = (sz * cy) * scx;
    out[2] = (-sy) * scx;
    out[3] = 0.0f;

    out[4] = (cz * sy * sx - sz * cx) * scy;
    out[5] = (sz * sy * sx + cz * cx) * scy;
    out[6] = (cy * sx) * scy;
    out[7] = 0.0f;

    out[8] = (cz * sy * cx + sz * sx) * scz;
    out[9] = (sz * sy * cx - cz * sx) * scz;
    out[10] = (cy * cx) * scz;
    out[11] = 0.0f;

    out[12] = pos[0];
    out[13] = pos[1];
    out[14] = pos[2];
    out[15] = 1.0f;
}

void mat4DecomposeTrs(const float m[16], float outPos[3], float outAngle[3],
                      float outScale[3]) noexcept
{
    outPos[0] = m[12];
    outPos[1] = m[13];
    outPos[2] = m[14];

    float sx = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    float sy = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
    float sz = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);

    const float det3 = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[4] * (m[1] * m[10] - m[2] * m[9]) +
                       m[8] * (m[1] * m[6] - m[2] * m[5]);
    if (det3 < 0.0f) {
        sx = -sx;
    }

    outScale[0] = (std::fabs(sx) > 1e-6f) ? sx : 1.0f;
    outScale[1] = (sy > 1e-6f) ? sy : 1.0f;
    outScale[2] = (sz > 1e-6f) ? sz : 1.0f;

    const float r00 = m[0] / outScale[0];
    const float r10 = m[1] / outScale[0];
    const float r20 = m[2] / outScale[0];
    const float r11 = m[5] / outScale[1];
    const float r21 = m[6] / outScale[1];
    const float r12 = m[9] / outScale[2];
    const float r22 = m[10] / outScale[2];

    const float syRot = std::clamp(-r20, -1.0f, 1.0f);
    outAngle[1] = std::asin(syRot);
    if (std::fabs(r20) < 0.9999f) {
        outAngle[0] = std::atan2(r21, r22);
        outAngle[2] = std::atan2(r10, r00);
    } else {
        outAngle[0] = std::atan2(-r12, r11);
        outAngle[2] = 0.0f;
    }
}

void sanitizeBoneBindTrs(we_mdl_bone_t &bone) noexcept
{
    for (int i = 0; i < 3; ++i) {
        bone.bind_pos[i] = finiteOr(bone.bind_pos[i], 0.0f);
        bone.bind_angle[i] = finiteOr(bone.bind_angle[i], 0.0f);
        bone.bind_scale[i] = finiteOr(bone.bind_scale[i], 1.0f);
        if (std::fabs(bone.bind_scale[i]) < 1e-5f) {
            bone.bind_scale[i] = 1.0f;
        }
    }
    mat4FromTrs(bone.bind_pos, bone.bind_angle, bone.bind_scale, bone.local_trans);
}

void recomputeWorldAndOffsetTransforms(BoneHierarchySnapshot &rig) noexcept
{
    const size_t n = rig.bones.size();
    for (size_t i = 0; i < n; ++i) {
        we_mdl_bone_t &b = rig.bones[i];
        sanitizeBoneBindTrs(b);
        if (b.parent == 0xFFFFFFFFu || b.parent >= i) {
            std::memcpy(b.world_bind, b.local_trans, sizeof(b.world_bind));
        } else {
            we_mat4_mul(rig.bones[b.parent].world_bind, b.local_trans, b.world_bind);
        }
        if (!we_mat4_inverse(b.world_bind, b.offset_trans)) {
            setIdentityMat4(b.offset_trans);
        }
    }
}

void applyWorldPreservingParent(BoneHierarchySnapshot &rig, size_t boneIdx,
                                uint32_t newParent) noexcept
{
    if (boneIdx >= rig.bones.size()) {
        return;
    }
    we_mdl_bone_t &bone = rig.bones[boneIdx];
    float desiredWorld[16];
    std::memcpy(desiredWorld, bone.world_bind, sizeof(desiredWorld));
    bone.parent = newParent;

    if (newParent == 0xFFFFFFFFu || newParent >= rig.bones.size()) {
        mat4DecomposeTrs(desiredWorld, bone.bind_pos, bone.bind_angle, bone.bind_scale);
        return;
    }

    float parentInv[16];
    if (we_mat4_inverse(rig.bones[newParent].world_bind, parentInv)) {
        float newLocal[16];
        we_mat4_mul(parentInv, desiredWorld, newLocal);
        mat4DecomposeTrs(newLocal, bone.bind_pos, bone.bind_angle, bone.bind_scale);
    }
}

void writeDefaultXpbdSimulationJson(we_mdl_bone_t &bone) noexcept
{
    static constexpr const char kDefaultSimJson[] =
        "{\"stiffness\":0.55,\"damping\":0.15,\"gravity\":1.0,\"mass\":0.5}";
    std::memset(bone.simulation_json, 0, sizeof(bone.simulation_json));
    std::memcpy(bone.simulation_json, kDefaultSimJson, sizeof(kDefaultSimJson));
}

bool boneHasActiveSimulationJson(const char *json) noexcept
{
    if (!json || json[0] == '\0') {
        return false;
    }
    we_bone_sim_config_t tmp{};
    return we_physics_parse_simulation_json(json, &tmp) &&
           ((tmp.flags & WE_BONE_PHYS_ENABLED) != 0u);
}

void applyXpbdDriveState(BoneHierarchySnapshot &rig, size_t boneIdx,
                         we_bone_drive_mode_t mode) noexcept
{
    if (boneIdx >= rig.bones.size() || boneIdx >= WE_MDL_MAX_BONES) {
        return;
    }
    rig.driveModes[boneIdx] = mode;
    we_mdl_bone_t &bone = rig.bones[boneIdx];
    if (mode == WE_BONE_DRIVE_XPBD) {
        if (!boneHasActiveSimulationJson(bone.simulation_json)) {
            writeDefaultXpbdSimulationJson(bone);
        }
        for (auto &c : rig.ikChains) {
            if (c.root_bone == boneIdx || c.mid_bone == boneIdx || c.end_bone == boneIdx) {
                c.enabled = 0u;
            }
        }
        for (auto &a : rig.aimConstraints) {
            if (a.bone_index == boneIdx) {
                a.enabled = 0u;
            }
        }
    } else {
        std::memset(bone.simulation_json, 0, sizeof(bone.simulation_json));
        if (mode != WE_BONE_DRIVE_AIM) {
            for (auto &a : rig.aimConstraints) {
                if (a.bone_index == boneIdx) {
                    a.enabled = 0u;
                }
            }
        }
    }
}

[[nodiscard]] bool isBoneDescendant(const BoneHierarchySnapshot &rig, uint16_t ancestorIdx,
                                    uint16_t candidateIdx) noexcept
{
    const size_t n = rig.bones.size();
    if (ancestorIdx >= n || candidateIdx >= n || ancestorIdx == candidateIdx) {
        return false;
    }
    std::vector<uint8_t> visited(n, 0u);
    uint32_t cur = rig.bones[candidateIdx].parent;
    while (cur != 0xFFFFFFFFu && cur < n) {
        if (cur == ancestorIdx) {
            return true;
        }
        if (visited[cur] != 0u) {
            return false;
        }
        visited[cur] = 1u;
        cur = rig.bones[cur].parent;
    }
    return false;
}

[[nodiscard]] float worldBoneDistance(const BoneHierarchySnapshot &rig, uint16_t a,
                                      uint16_t b) noexcept
{
    if (a >= rig.bones.size() || b >= rig.bones.size()) {
        return 0.0f;
    }
    const float dx = rig.bones[b].world_bind[12] - rig.bones[a].world_bind[12];
    const float dy = rig.bones[b].world_bind[13] - rig.bones[a].world_bind[13];
    const float dz = rig.bones[b].world_bind[14] - rig.bones[a].world_bind[14];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

/* Reorders bones topologically (parent < child) when any parent index exceeds its child index,
 * emitting oldToNew so downstream weights, timelines, and cached physics state stay aligned. */
void ensureTopologicalOrder(BoneHierarchySnapshot &rig, int32_t &trackedBoneIndex,
                            std::vector<int32_t> *outOldToNew = nullptr)
{
    const size_t n = rig.bones.size();
    if (outOldToNew) {
        outOldToNew->resize(n);
        std::iota(outOldToNew->begin(), outOldToNew->end(), 0);
    }

    bool needsReorder = false;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t p = rig.bones[i].parent;
        if (p != 0xFFFFFFFFu && p >= i) {
            needsReorder = true;
            break;
        }
    }
    if (!needsReorder) {
        recomputeWorldAndOffsetTransforms(rig);
        return;
    }

    std::vector<uint8_t> visited(n, 0u);
    std::vector<size_t> order;
    order.reserve(n);

    auto visit = [&](auto &self, size_t idx, int depth) -> void {
        if (visited[idx] != 0u || depth > static_cast<int>(n)) {
            return;
        }
        visited[idx] = 1u;
        const uint32_t p = rig.bones[idx].parent;
        if (p != 0xFFFFFFFFu && p < n && p != idx) {
            self(self, p, depth + 1);
        }
        order.push_back(idx);
    };

    for (size_t i = 0; i < n; ++i) {
        visit(visit, i, 0);
    }

    std::vector<int32_t> oldToNew(n, -1);
    for (size_t newIdx = 0; newIdx < order.size(); ++newIdx) {
        oldToNew[order[newIdx]] = static_cast<int32_t>(newIdx);
    }

    std::vector<we_mdl_bone_t> sortedBones(n);
    std::array<we_bone_drive_mode_t, WE_MDL_MAX_BONES> sortedModes{};
    sortedModes.fill(WE_BONE_DRIVE_FK);

    for (size_t newIdx = 0; newIdx < n; ++newIdx) {
        const size_t oldIdx = order[newIdx];
        sortedBones[newIdx] = rig.bones[oldIdx];
        const uint32_t oldParent = rig.bones[oldIdx].parent;
        if (oldParent == 0xFFFFFFFFu || oldParent >= n) {
            sortedBones[newIdx].parent = 0xFFFFFFFFu;
        } else {
            const int32_t mappedParent = oldToNew[oldParent];
            sortedBones[newIdx].parent =
                (mappedParent >= 0 && static_cast<size_t>(mappedParent) < newIdx)
                    ? static_cast<uint32_t>(mappedParent)
                    : 0xFFFFFFFFu;
        }
        if (oldIdx < WE_MDL_MAX_BONES && newIdx < WE_MDL_MAX_BONES) {
            sortedModes[newIdx] = rig.driveModes[oldIdx];
        }
    }

    for (auto &chain : rig.ikChains) {
        if (chain.root_bone < n) {
            chain.root_bone = static_cast<uint16_t>(oldToNew[chain.root_bone]);
        }
        if (chain.mid_bone < n) {
            chain.mid_bone = static_cast<uint16_t>(oldToNew[chain.mid_bone]);
        }
        if (chain.end_bone < n) {
            chain.end_bone = static_cast<uint16_t>(oldToNew[chain.end_bone]);
        }
    }

    for (auto &aim : rig.aimConstraints) {
        if (aim.bone_index < n) {
            aim.bone_index = static_cast<uint16_t>(oldToNew[aim.bone_index]);
        }
    }

    if (trackedBoneIndex >= 0 && static_cast<size_t>(trackedBoneIndex) < n) {
        trackedBoneIndex = oldToNew[static_cast<size_t>(trackedBoneIndex)];
    }

    rig.bones = std::move(sortedBones);
    rig.driveModes = sortedModes;
    recomputeWorldAndOffsetTransforms(rig);
    if (outOldToNew) {
        *outOldToNew = std::move(oldToNew);
    }
}

[[nodiscard]] bool isNonIdentityRemap(const std::vector<int32_t> &oldToNew) noexcept
{
    for (size_t i = 0u; i < oldToNew.size(); ++i) {
        if (oldToNew[i] != static_cast<int32_t>(i)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::vector<int32_t> remapAimTargets(const std::vector<int32_t> &prev,
                                                   const std::vector<int32_t> &oldToNew,
                                                   size_t newBoneCount)
{
    std::vector<int32_t> next(newBoneCount, -1);
    const size_t limit = std::min(prev.size(), oldToNew.size());
    for (size_t oldBone = 0u; oldBone < limit; ++oldBone) {
        const int32_t newBone = oldToNew[oldBone];
        if (newBone < 0 || static_cast<size_t>(newBone) >= newBoneCount) {
            continue;
        }
        const int32_t oldTarget = prev[oldBone];
        if (oldTarget >= 0 && static_cast<size_t>(oldTarget) < oldToNew.size()) {
            const int32_t mappedTarget = oldToNew[static_cast<size_t>(oldTarget)];
            next[static_cast<size_t>(newBone)] =
                (mappedTarget != newBone && mappedTarget >= 0) ? mappedTarget : -1;
        }
    }
    return next;
}

void remapXpbdState(std::array<bool, WE_MDL_MAX_BONES> &pushedXpbd, bool &forceReinit,
                    const std::vector<int32_t> &oldToNew) noexcept
{
    std::array<bool, WE_MDL_MAX_BONES> next{};
    next.fill(false);
    const size_t n = std::min<size_t>(oldToNew.size(), WE_MDL_MAX_BONES);
    for (size_t oldB = 0u; oldB < n; ++oldB) {
        const int32_t newB = oldToNew[oldB];
        if (newB >= 0 && newB < static_cast<int32_t>(WE_MDL_MAX_BONES)) {
            next[static_cast<size_t>(newB)] = pushedXpbd[oldB];
        } else if (pushedXpbd[oldB]) {
            forceReinit = true;
        }
    }
    if (isNonIdentityRemap(oldToNew)) {
        forceReinit = true;
    }
    pushedXpbd = next;
}

QString driveModeLabel(we_bone_drive_mode_t mode)
{
    switch (mode) {
    case WE_BONE_DRIVE_FK:
        return QStringLiteral("FK");
    case WE_BONE_DRIVE_IK:
        return QStringLiteral("IK");
    case WE_BONE_DRIVE_AIM:
        return QStringLiteral("Aim");
    case WE_BONE_DRIVE_XPBD:
        return QStringLiteral("XPBD");
    }
    return QStringLiteral("FK");
}

QColor driveModeColor(we_bone_drive_mode_t mode)
{
    switch (mode) {
    case WE_BONE_DRIVE_FK:
        return QColor(QStringLiteral("#94a3b8"));
    case WE_BONE_DRIVE_IK:
        return QColor(QStringLiteral("#38bdf8"));
    case WE_BONE_DRIVE_AIM:
        return QColor(QStringLiteral("#fbbf24"));
    case WE_BONE_DRIVE_XPBD:
        return QColor(QStringLiteral("#4ade80"));
    }
    return QColor(QStringLiteral("#94a3b8"));
}

QDoubleSpinBox *makeRigSpin(double minVal, double maxVal, double step, int decimals,
                            const QString &suffix, const QString &accessibleName, QWidget *parent)
{
    auto *spin = new QDoubleSpinBox(parent);
    spin->setRange(minVal, maxVal);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    if (!suffix.isEmpty()) {
        spin->setSuffix(suffix);
    }
    spin->setKeyboardTracking(false);
    spin->setAccessibleName(accessibleName);
    return spin;
}

/* Tree widget intercepting internal drag-and-drop so cycle validation runs before mutation. */
class BoneHierarchyTreeWidget final : public QTreeWidget {
  public:
    explicit BoneHierarchyTreeWidget(BoneRiggingPanel *panel, QWidget *parent = nullptr)
        : QTreeWidget(parent), m_panel(panel)
    {
        setDragEnabled(true);
        setAcceptDrops(true);
        setDropIndicatorShown(true);
        setDragDropMode(QAbstractItemView::InternalMove);
        setDefaultDropAction(Qt::MoveAction);
    }

  protected:
    void dropEvent(QDropEvent *event) override
    {
        if (!m_panel) {
            event->ignore();
            return;
        }
        QTreeWidgetItem *draggedItem = currentItem();
        if (!draggedItem) {
            event->ignore();
            return;
        }
        const int32_t childIdx = draggedItem->data(0, Qt::UserRole).toInt();

        QTreeWidgetItem *targetItem = itemAt(event->position().toPoint());
        int32_t newParentIdx = -1;
        if (targetItem) {
            const DropIndicatorPosition pos = dropIndicatorPosition();
            if (pos == QAbstractItemView::OnItem) {
                newParentIdx = targetItem->data(0, Qt::UserRole).toInt();
            } else if (targetItem->parent()) {
                newParentIdx = targetItem->parent()->data(0, Qt::UserRole).toInt();
            }
        }

        if (m_panel->reparentBone(childIdx, newParentIdx)) {
            event->accept();
        } else {
            event->ignore();
        }
    }

  private:
    QPointer<BoneRiggingPanel> m_panel;
};

struct RigObjectState {
    BoneHierarchySnapshot rig;
    int32_t selectedBoneIndex{-1};
    std::array<bool, WE_MDL_MAX_BONES> lastPushedXpbd{};
    bool forceReinitPhysics{false};
    std::vector<int32_t> aimTargetBoneIds;
};

} // namespace

struct BoneRiggingPanel::Impl {
    std::array<bool, WE_MDL_MAX_BONES> lastPushedXpbd{};
    bool forceReinitPhysics{false};
    std::vector<int32_t> aimTargetBoneIds;
    std::unordered_map<int32_t, RigObjectState> objectStates;

    QPushButton *deleteBoneBtn{nullptr};
    QPushButton *makeRootBtn{nullptr};
    QComboBox *parentCombo{nullptr};
    QComboBox *ikChainCombo{nullptr};
    QPushButton *deleteIkBtn{nullptr};
    QComboBox *ikRootCombo{nullptr};
    QComboBox *ikMidCombo{nullptr};
    QComboBox *ikEndCombo{nullptr};
    QDoubleSpinBox *ikTargetXSpin{nullptr};
    QDoubleSpinBox *ikTargetYSpin{nullptr};
    QDoubleSpinBox *ikPoleXSpin{nullptr};
    QDoubleSpinBox *ikPoleYSpin{nullptr};
    QDoubleSpinBox *ikPoleZSpin{nullptr};
    QDoubleSpinBox *ikMidMinAngleSpin{nullptr};
    QDoubleSpinBox *ikMidMaxAngleSpin{nullptr};
    QDoubleSpinBox *ikBlendSpin{nullptr};
    QCheckBox *aimEnableCheck{nullptr};
    QComboBox *aimTargetBoneCombo{nullptr};
    QComboBox *aimAxisCombo{nullptr};
    QDoubleSpinBox *aimTargetXSpin{nullptr};
    QDoubleSpinBox *aimTargetYSpin{nullptr};
    QDoubleSpinBox *aimConeDegSpin{nullptr};
    QDoubleSpinBox *aimBlendSpin{nullptr};
};

BoneRiggingPanel::BoneRiggingPanel(QWidget *parent)
    : QWidget(parent), m_impl(std::make_unique<Impl>())
{
    m_rig.driveModes.fill(WE_BONE_DRIVE_FK);
    m_impl->lastPushedXpbd.fill(false);
    setupUi();
    syncInspectorFromSelectedBone();
}

BoneRiggingPanel::~BoneRiggingPanel() = default;

void BoneRiggingPanel::setupUi()
{
    setStyleSheet(QStringLiteral(
        "QWidget { background-color: #12141a; color: #f1f5f9; font-size: 12px; }"
        "QGroupBox#rigSectionGroup { background-color: #181b22; border: 1px solid #2a2f3d; "
        "border-radius: 6px; margin-top: 10px; padding-top: 10px; font-weight: 600; "
        "color: #f1f5f9; }"
        "QGroupBox#rigSectionGroup::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; "
        "color: #94a3b8; }"
        "QTreeWidget { background-color: #0b0c10; color: #f1f5f9; border: 1px solid #2a2f3d; "
        "border-radius: 4px; alternate-background-color: #12141a; }"
        "QTreeWidget::item:selected { background-color: #1e3a5f; color: #f1f5f9; }"
        "QLineEdit, QComboBox, QDoubleSpinBox, QPushButton { background-color: #0b0c10; "
        "color: #f1f5f9; border: 1px solid #2a2f3d; border-radius: 4px; padding: 3px 6px; }"
        "QPushButton { background-color: #181b22; }"
        "QPushButton:hover, QComboBox:hover, QDoubleSpinBox:hover { background-color: #222631; "
        "border-color: #38bdf8; }"
        "QLineEdit:focus, QComboBox:focus, QDoubleSpinBox:focus, QPushButton:focus, "
        "QCheckBox:focus, QTreeWidget:focus { border: 2px solid #38bdf8; outline: none; }"
        "QLineEdit:disabled, QComboBox:disabled, QDoubleSpinBox:disabled, QPushButton:disabled, "
        "QCheckBox:disabled { color: #788496; background-color: #12141a; border-color: #222631; }"
        "QDoubleSpinBox { font-family: 'JetBrains Mono', monospace; font-size: 12px; }"));

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(6, 6, 6, 6);
    mainLayout->setSpacing(6);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->setChildrenCollapsible(false);

    auto *treeGroup = new QGroupBox(tr("Skeleton Hierarchy"), splitter);
    treeGroup->setObjectName(QStringLiteral("rigSectionGroup"));
    auto *treeLayout = new QVBoxLayout(treeGroup);
    treeLayout->setContentsMargins(8, 8, 8, 8);
    treeLayout->setSpacing(6);

    m_boneTree = new BoneHierarchyTreeWidget(this, treeGroup);
    m_boneTree->setColumnCount(3);
    m_boneTree->setHeaderLabels({tr("Bone"), tr("Drive"), tr("Status")});
    m_boneTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_boneTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_boneTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_boneTree->setAlternatingRowColors(true);
    m_boneTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_boneTree->setAccessibleName(tr("Bone Hierarchy Tree"));
    m_boneTree->setAccessibleDescription(
        tr("Displays parent-child bones. Drag and drop onto a bone to reparent without cycles."));
    connect(m_boneTree, &QTreeWidget::currentItemChanged, this,
            &BoneRiggingPanel::onBoneTreeCurrentItemChanged);
    treeLayout->addWidget(m_boneTree);

    auto *treeBtnRow = new QHBoxLayout();
    treeBtnRow->setSpacing(6);

    m_addBoneBtn = new QPushButton(tr("+ Add Bone"), treeGroup);
    m_addBoneBtn->setAccessibleName(tr("Add Child Bone"));
    m_addBoneBtn->setToolTip(tr("Add a new bone under the selected bone (or as a root bone)"));
    connect(m_addBoneBtn, &QPushButton::clicked, this, &BoneRiggingPanel::onAddBoneClicked);

    m_impl->deleteBoneBtn = new QPushButton(tr("Delete Bone"), treeGroup);
    m_impl->deleteBoneBtn->setObjectName(QStringLiteral("deleteBoneBtn"));
    m_impl->deleteBoneBtn->setAccessibleName(tr("Delete Selected Bone"));
    m_impl->deleteBoneBtn->setToolTip(
        tr("Remove the selected bone and reparent its children safely"));
    connect(m_impl->deleteBoneBtn, &QPushButton::clicked, this,
            &BoneRiggingPanel::onDeleteBoneClicked);

    m_impl->makeRootBtn = new QPushButton(tr("Unparent"), treeGroup);
    m_impl->makeRootBtn->setObjectName(QStringLiteral("makeRootBtn"));
    m_impl->makeRootBtn->setAccessibleName(tr("Make Selected Bone Root"));
    m_impl->makeRootBtn->setToolTip(
        tr("Detach the selected bone from its parent to become a root bone"));
    connect(m_impl->makeRootBtn, &QPushButton::clicked, this, [this]() {
        if (m_selectedBoneIndex >= 0) {
            reparentBone(m_selectedBoneIndex, -1);
        }
    });

    treeBtnRow->addWidget(m_addBoneBtn);
    treeBtnRow->addWidget(m_impl->deleteBoneBtn);
    treeBtnRow->addWidget(m_impl->makeRootBtn);
    treeLayout->addLayout(treeBtnRow);
    splitter->addWidget(treeGroup);

    auto *scrollArea = new QScrollArea(splitter);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto *scrollContent = new QWidget(scrollArea);
    auto *contentLayout = new QVBoxLayout(scrollContent);
    contentLayout->setContentsMargins(2, 2, 4, 2);
    contentLayout->setSpacing(8);

    auto *boneGroup = new QGroupBox(tr("Bone Properties & Bind Pose"), scrollContent);
    boneGroup->setObjectName(QStringLiteral("rigSectionGroup"));
    auto *boneGrid = new QGridLayout(boneGroup);
    boneGrid->setContentsMargins(8, 8, 8, 8);
    boneGrid->setSpacing(6);

    m_boneNameEdit = new QLineEdit(boneGroup);
    m_boneNameEdit->setMaxLength(127);
    m_boneNameEdit->setAccessibleName(tr("Bone Name"));
    m_boneNameEdit->setPlaceholderText(tr("Enter bone name"));
    connect(m_boneNameEdit, &QLineEdit::editingFinished, this,
            &BoneRiggingPanel::onTransformOrConstraintEdited);
    boneGrid->addWidget(new QLabel(tr("Name:"), boneGroup), 0, 0);
    boneGrid->addWidget(m_boneNameEdit, 0, 1, 1, 3);

    m_impl->parentCombo = new QComboBox(boneGroup);
    m_impl->parentCombo->setObjectName(QStringLiteral("parentCombo"));
    m_impl->parentCombo->setAccessibleName(tr("Parent Bone Selector"));
    m_impl->parentCombo->setToolTip(tr("Reparent the selected bone (cycles are rejected)"));
    connect(m_impl->parentCombo, qOverload<int>(&QComboBox::activated), this, [this](int idx) {
        if (m_updatingUi || m_selectedBoneIndex < 0 || idx < 0 || !m_impl->parentCombo) {
            return;
        }
        const int32_t newParent = m_impl->parentCombo->itemData(idx).toInt();
        if (!reparentBone(m_selectedBoneIndex, newParent)) {
            syncInspectorFromSelectedBone();
        }
    });
    boneGrid->addWidget(new QLabel(tr("Parent:"), boneGroup), 1, 0);
    boneGrid->addWidget(m_impl->parentCombo, 1, 1, 1, 3);

    m_driveModeCombo = new QComboBox(boneGroup);
    m_driveModeCombo->addItem(tr("FK (Keyframe)"), static_cast<uint32_t>(WE_BONE_DRIVE_FK));
    m_driveModeCombo->addItem(tr("2-Bone IK"), static_cast<uint32_t>(WE_BONE_DRIVE_IK));
    m_driveModeCombo->addItem(tr("Look-At Aim"), static_cast<uint32_t>(WE_BONE_DRIVE_AIM));
    m_driveModeCombo->addItem(tr("XPBD Spring"), static_cast<uint32_t>(WE_BONE_DRIVE_XPBD));
    m_driveModeCombo->setAccessibleName(tr("Bone Drive Mode"));
    m_driveModeCombo->setToolTip(
        tr("Selects the active solver driving this bone; XPBD links to Physics Spring Inspector"));
    connect(m_driveModeCombo, qOverload<int>(&QComboBox::activated), this, [this](int) {
        if (m_updatingUi || !m_driveModeCombo) {
            return;
        }
        const auto mode =
            static_cast<we_bone_drive_mode_t>(m_driveModeCombo->currentData().toUInt());
        if (m_impl && m_impl->aimEnableCheck) {
            const QSignalBlocker b(m_impl->aimEnableCheck);
            m_impl->aimEnableCheck->setChecked(mode == WE_BONE_DRIVE_AIM);
        }
        onTransformOrConstraintEdited();
    });
    boneGrid->addWidget(new QLabel(tr("Drive Mode:"), boneGroup), 2, 0);
    boneGrid->addWidget(m_driveModeCombo, 2, 1, 1, 2);

    m_conflictBadgeLabel = new QLabel(tr("FK"), boneGroup);
    m_conflictBadgeLabel->setAlignment(Qt::AlignCenter);
    m_conflictBadgeLabel->setAccessibleName(tr("Drive Mode Status Badge"));
    boneGrid->addWidget(m_conflictBadgeLabel, 2, 3);

    m_posXSpin = makeRigSpin(-16384.0, 16384.0, 1.0, 2, QStringLiteral(" px"),
                             tr("Bind Position X"), boneGroup);
    m_posYSpin = makeRigSpin(-16384.0, 16384.0, 1.0, 2, QStringLiteral(" px"),
                             tr("Bind Position Y"), boneGroup);
    boneGrid->addWidget(new QLabel(tr("Pos X:"), boneGroup), 3, 0);
    boneGrid->addWidget(m_posXSpin, 3, 1);
    boneGrid->addWidget(new QLabel(tr("Pos Y:"), boneGroup), 3, 2);
    boneGrid->addWidget(m_posYSpin, 3, 3);

    m_angleZSpin = makeRigSpin(-180.0, 180.0, 1.0, 2, QStringLiteral(" deg"),
                               tr("Bind Angle Z Degrees"), boneGroup);
    boneGrid->addWidget(new QLabel(tr("Angle Z:"), boneGroup), 4, 0);
    boneGrid->addWidget(m_angleZSpin, 4, 1, 1, 3);

    m_scaleXSpin = makeRigSpin(-100.0, 100.0, 0.05, 3, QString(), tr("Bind Scale X"), boneGroup);
    m_scaleYSpin = makeRigSpin(-100.0, 100.0, 0.05, 3, QString(), tr("Bind Scale Y"), boneGroup);
    m_scaleXSpin->setValue(1.0);
    m_scaleYSpin->setValue(1.0);
    boneGrid->addWidget(new QLabel(tr("Scale X:"), boneGroup), 5, 0);
    boneGrid->addWidget(m_scaleXSpin, 5, 1);
    boneGrid->addWidget(new QLabel(tr("Scale Y:"), boneGroup), 5, 2);
    boneGrid->addWidget(m_scaleYSpin, 5, 3);

    for (QDoubleSpinBox *s : {m_posXSpin, m_posYSpin, m_angleZSpin, m_scaleXSpin, m_scaleYSpin}) {
        connect(s, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                &BoneRiggingPanel::onTransformOrConstraintEdited);
    }

    contentLayout->addWidget(boneGroup);

    auto *ikGroup = new QGroupBox(tr("2-Bone Analytic IK Chain"), scrollContent);
    ikGroup->setObjectName(QStringLiteral("rigSectionGroup"));
    auto *ikGrid = new QGridLayout(ikGroup);
    ikGrid->setContentsMargins(8, 8, 8, 8);
    ikGrid->setSpacing(6);

    m_impl->ikChainCombo = new QComboBox(ikGroup);
    m_impl->ikChainCombo->setObjectName(QStringLiteral("ikChainCombo"));
    m_impl->ikChainCombo->setAccessibleName(tr("Active IK Chain Selector"));
    connect(m_impl->ikChainCombo, qOverload<int>(&QComboBox::activated), this,
            [this](int) { syncInspectorFromSelectedBone(); });
    ikGrid->addWidget(new QLabel(tr("Chain:"), ikGroup), 0, 0);
    ikGrid->addWidget(m_impl->ikChainCombo, 0, 1, 1, 2);

    m_impl->deleteIkBtn = new QPushButton(tr("Remove"), ikGroup);
    m_impl->deleteIkBtn->setObjectName(QStringLiteral("deleteIkBtn"));
    m_impl->deleteIkBtn->setAccessibleName(tr("Remove Selected IK Chain"));
    connect(m_impl->deleteIkBtn, &QPushButton::clicked, this, [this]() {
        if (!m_impl->ikChainCombo) {
            return;
        }
        const int idx = m_impl->ikChainCombo->currentData().toInt();
        if (idx < 0 || static_cast<size_t>(idx) >= m_rig.ikChains.size()) {
            return;
        }
        BoneHierarchySnapshot oldRig = m_rig;
        BoneHierarchySnapshot newRig = m_rig;
        const we_ik_chain_t removed = newRig.ikChains[static_cast<size_t>(idx)];
        newRig.ikChains.erase(newRig.ikChains.begin() + idx);
        for (uint16_t bIdx : {removed.root_bone, removed.mid_bone, removed.end_bone}) {
            if (bIdx < WE_MDL_MAX_BONES && newRig.driveModes[bIdx] == WE_BONE_DRIVE_IK) {
                newRig.driveModes[bIdx] = WE_BONE_DRIVE_FK;
            }
        }
        QPointer<BoneRiggingPanel> self(this);
        const int32_t keepSelected = m_selectedBoneIndex;
        auto applyFn = [self, keepSelected](const BoneHierarchySnapshot &snap) {
            if (self) {
                self->applySnapshotState(snap, keepSelected, nullptr);
            }
        };
        if (m_commandBus) {
            m_commandBus->execute(std::make_unique<BoneEditCommand>(
                tr("Remove 2-Bone IK Chain"), std::move(oldRig), std::move(newRig), applyFn));
        } else {
            applyFn(newRig);
        }
    });
    ikGrid->addWidget(m_impl->deleteIkBtn, 0, 3);

    m_impl->ikRootCombo = new QComboBox(ikGroup);
    m_impl->ikRootCombo->setObjectName(QStringLiteral("ikRootCombo"));
    m_impl->ikRootCombo->setAccessibleName(tr("IK Root Bone"));
    m_impl->ikMidCombo = new QComboBox(ikGroup);
    m_impl->ikMidCombo->setObjectName(QStringLiteral("ikMidCombo"));
    m_impl->ikMidCombo->setAccessibleName(tr("IK Mid Bone"));
    m_impl->ikEndCombo = new QComboBox(ikGroup);
    m_impl->ikEndCombo->setObjectName(QStringLiteral("ikEndCombo"));
    m_impl->ikEndCombo->setAccessibleName(tr("IK End Bone"));

    ikGrid->addWidget(new QLabel(tr("Root:"), ikGroup), 1, 0);
    ikGrid->addWidget(m_impl->ikRootCombo, 1, 1);
    ikGrid->addWidget(new QLabel(tr("Mid:"), ikGroup), 1, 2);
    ikGrid->addWidget(m_impl->ikMidCombo, 1, 3);
    ikGrid->addWidget(new QLabel(tr("End:"), ikGroup), 2, 0);
    ikGrid->addWidget(m_impl->ikEndCombo, 2, 1);

    m_createIkBtn = new QPushButton(tr("Create IK Chain"), ikGroup);
    m_createIkBtn->setAccessibleName(tr("Create 2-Bone IK Chain"));
    m_createIkBtn->setToolTip(tr("Bind a 2-bone analytic IK solver to Root -> Mid -> End"));
    connect(m_createIkBtn, &QPushButton::clicked, this, &BoneRiggingPanel::onCreateIkChainClicked);
    ikGrid->addWidget(m_createIkBtn, 2, 2, 1, 2);

    m_impl->ikTargetXSpin =
        makeRigSpin(-16384.0, 16384.0, 1.0, 2, QStringLiteral(" px"), tr("IK Target X"), ikGroup);
    m_impl->ikTargetXSpin->setObjectName(QStringLiteral("ikTargetXSpin"));
    m_impl->ikTargetYSpin =
        makeRigSpin(-16384.0, 16384.0, 1.0, 2, QStringLiteral(" px"), tr("IK Target Y"), ikGroup);
    m_impl->ikTargetYSpin->setObjectName(QStringLiteral("ikTargetYSpin"));
    ikGrid->addWidget(new QLabel(tr("Target X:"), ikGroup), 3, 0);
    ikGrid->addWidget(m_impl->ikTargetXSpin, 3, 1);
    ikGrid->addWidget(new QLabel(tr("Target Y:"), ikGroup), 3, 2);
    ikGrid->addWidget(m_impl->ikTargetYSpin, 3, 3);

    m_impl->ikPoleXSpin = makeRigSpin(-16384.0, 16384.0, 1.0, 2, QStringLiteral(" px"),
                                      tr("IK Pole Vector X"), ikGroup);
    m_impl->ikPoleXSpin->setObjectName(QStringLiteral("ikPoleXSpin"));
    m_impl->ikPoleYSpin = makeRigSpin(-16384.0, 16384.0, 1.0, 2, QStringLiteral(" px"),
                                      tr("IK Pole Vector Y"), ikGroup);
    m_impl->ikPoleYSpin->setObjectName(QStringLiteral("ikPoleYSpin"));
    m_impl->ikPoleZSpin = makeRigSpin(-16384.0, 16384.0, 1.0, 2, QStringLiteral(" px"),
                                      tr("IK Pole Vector Z"), ikGroup);
    m_impl->ikPoleZSpin->setObjectName(QStringLiteral("ikPoleZSpin"));
    ikGrid->addWidget(new QLabel(tr("Pole X:"), ikGroup), 4, 0);
    ikGrid->addWidget(m_impl->ikPoleXSpin, 4, 1);
    ikGrid->addWidget(new QLabel(tr("Pole Y:"), ikGroup), 4, 2);
    ikGrid->addWidget(m_impl->ikPoleYSpin, 4, 3);
    ikGrid->addWidget(new QLabel(tr("Pole Z:"), ikGroup), 5, 0);
    ikGrid->addWidget(m_impl->ikPoleZSpin, 5, 1);

    m_poleFlipCheck = new QCheckBox(tr("Flip Pole Bend (-1 / +1)"), ikGroup);
    m_poleFlipCheck->setAccessibleName(tr("Flip IK Pole Vector Sign"));
    connect(m_poleFlipCheck, &QCheckBox::toggled, this, &BoneRiggingPanel::onTogglePoleFlipClicked);
    ikGrid->addWidget(m_poleFlipCheck, 5, 2, 1, 2);

    m_ikSoftZoneSpin =
        makeRigSpin(0.0, 0.25, 0.01, 3, QString(), tr("IK Soft Stretch Zone"), ikGroup);
    m_ikSoftZoneSpin->setValue(0.05);
    m_ikMaxStretchSpin =
        makeRigSpin(1.0, 1.5, 0.02, 2, QStringLiteral(" x"), tr("IK Max Stretch Ratio"), ikGroup);
    m_ikMaxStretchSpin->setValue(1.0);
    ikGrid->addWidget(new QLabel(tr("Soft Zone:"), ikGroup), 6, 0);
    ikGrid->addWidget(m_ikSoftZoneSpin, 6, 1);
    ikGrid->addWidget(new QLabel(tr("Max Stretch:"), ikGroup), 6, 2);
    ikGrid->addWidget(m_ikMaxStretchSpin, 6, 3);

    m_ikMinAngleSpin = makeRigSpin(-180.0, 0.0, 5.0, 1, QStringLiteral(" deg"),
                                   tr("IK Root Minimum Joint Limit"), ikGroup);
    m_ikMinAngleSpin->setValue(-180.0);
    m_ikMaxAngleSpin = makeRigSpin(0.0, 180.0, 5.0, 1, QStringLiteral(" deg"),
                                   tr("IK Root Maximum Joint Limit"), ikGroup);
    m_ikMaxAngleSpin->setValue(180.0);
    ikGrid->addWidget(new QLabel(tr("Root Min:"), ikGroup), 7, 0);
    ikGrid->addWidget(m_ikMinAngleSpin, 7, 1);
    ikGrid->addWidget(new QLabel(tr("Root Max:"), ikGroup), 7, 2);
    ikGrid->addWidget(m_ikMaxAngleSpin, 7, 3);

    m_impl->ikMidMinAngleSpin = makeRigSpin(-180.0, 0.0, 5.0, 1, QStringLiteral(" deg"),
                                            tr("IK Mid Minimum Joint Limit"), ikGroup);
    m_impl->ikMidMinAngleSpin->setObjectName(QStringLiteral("ikMidMinAngleSpin"));
    m_impl->ikMidMinAngleSpin->setValue(-180.0);
    m_impl->ikMidMaxAngleSpin = makeRigSpin(0.0, 180.0, 5.0, 1, QStringLiteral(" deg"),
                                            tr("IK Mid Maximum Joint Limit"), ikGroup);
    m_impl->ikMidMaxAngleSpin->setObjectName(QStringLiteral("ikMidMaxAngleSpin"));
    m_impl->ikMidMaxAngleSpin->setValue(180.0);
    ikGrid->addWidget(new QLabel(tr("Mid Min:"), ikGroup), 8, 0);
    ikGrid->addWidget(m_impl->ikMidMinAngleSpin, 8, 1);
    ikGrid->addWidget(new QLabel(tr("Mid Max:"), ikGroup), 8, 2);
    ikGrid->addWidget(m_impl->ikMidMaxAngleSpin, 8, 3);

    m_impl->ikBlendSpin =
        makeRigSpin(0.0, 1.0, 0.05, 2, QString(), tr("IK Constraint Blend Weight"), ikGroup);
    m_impl->ikBlendSpin->setObjectName(QStringLiteral("ikBlendSpin"));
    m_impl->ikBlendSpin->setValue(1.0);
    ikGrid->addWidget(new QLabel(tr("IK Blend:"), ikGroup), 9, 0);
    ikGrid->addWidget(m_impl->ikBlendSpin, 9, 1, 1, 3);

    for (QDoubleSpinBox *s :
         {m_impl->ikTargetXSpin, m_impl->ikTargetYSpin, m_impl->ikPoleXSpin, m_impl->ikPoleYSpin,
          m_impl->ikPoleZSpin, m_ikSoftZoneSpin, m_ikMaxStretchSpin, m_ikMinAngleSpin,
          m_ikMaxAngleSpin, m_impl->ikMidMinAngleSpin, m_impl->ikMidMaxAngleSpin,
          m_impl->ikBlendSpin}) {
        connect(s, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                &BoneRiggingPanel::onTransformOrConstraintEdited);
    }

    contentLayout->addWidget(ikGroup);

    auto *aimGroup = new QGroupBox(tr("Single-Bone Look-At / Aim Constraint"), scrollContent);
    aimGroup->setObjectName(QStringLiteral("rigSectionGroup"));
    auto *aimGrid = new QGridLayout(aimGroup);
    aimGrid->setContentsMargins(8, 8, 8, 8);
    aimGrid->setSpacing(6);

    m_impl->aimEnableCheck = new QCheckBox(tr("Enable Look-At Aim for Selected Bone"), aimGroup);
    m_impl->aimEnableCheck->setObjectName(QStringLiteral("aimEnableCheck"));
    m_impl->aimEnableCheck->setAccessibleName(tr("Enable Look-At Aim Constraint"));
    connect(m_impl->aimEnableCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        if (m_updatingUi) {
            return;
        }
        if (m_driveModeCombo) {
            const QSignalBlocker b(m_driveModeCombo);
            const auto cur =
                static_cast<we_bone_drive_mode_t>(m_driveModeCombo->currentData().toUInt());
            if (enabled && cur != WE_BONE_DRIVE_XPBD) {
                const int idx =
                    m_driveModeCombo->findData(static_cast<uint32_t>(WE_BONE_DRIVE_AIM));
                if (idx >= 0) {
                    m_driveModeCombo->setCurrentIndex(idx);
                }
            } else if (!enabled && cur == WE_BONE_DRIVE_AIM) {
                const int idx = m_driveModeCombo->findData(static_cast<uint32_t>(WE_BONE_DRIVE_FK));
                if (idx >= 0) {
                    m_driveModeCombo->setCurrentIndex(idx);
                }
            }
        }
        onTransformOrConstraintEdited();
    });
    aimGrid->addWidget(m_impl->aimEnableCheck, 0, 0, 1, 4);

    m_impl->aimTargetBoneCombo = new QComboBox(aimGroup);
    m_impl->aimTargetBoneCombo->setObjectName(QStringLiteral("aimTargetBoneCombo"));
    m_impl->aimTargetBoneCombo->setAccessibleName(tr("Aim Target Bone Selector"));
    m_impl->aimTargetBoneCombo->setToolTip(
        tr("Select a target bone to track, or Custom World Target for manual coordinates"));
    connect(m_impl->aimTargetBoneCombo, qOverload<int>(&QComboBox::activated), this,
            [this](int idx) {
                if (m_updatingUi || idx < 0 || !m_impl->aimTargetBoneCombo) {
                    return;
                }
                const int targetBone = m_impl->aimTargetBoneCombo->itemData(idx).toInt();
                if (m_selectedBoneIndex >= 0 &&
                    static_cast<size_t>(m_selectedBoneIndex) < m_rig.bones.size()) {
                    if (m_impl->aimTargetBoneIds.size() < m_rig.bones.size()) {
                        m_impl->aimTargetBoneIds.resize(m_rig.bones.size(), -1);
                    }
                    m_impl->aimTargetBoneIds[static_cast<size_t>(m_selectedBoneIndex)] = targetBone;
                }
                if (targetBone >= 0 && static_cast<size_t>(targetBone) < m_rig.bones.size()) {
                    const we_mdl_bone_t &tb = m_rig.bones[static_cast<size_t>(targetBone)];
                    if (m_impl->aimTargetXSpin) {
                        const QSignalBlocker b(m_impl->aimTargetXSpin);
                        m_impl->aimTargetXSpin->setValue(static_cast<double>(tb.world_bind[12]));
                    }
                    if (m_impl->aimTargetYSpin) {
                        const QSignalBlocker b(m_impl->aimTargetYSpin);
                        m_impl->aimTargetYSpin->setValue(static_cast<double>(tb.world_bind[13]));
                    }
                }
                onTransformOrConstraintEdited();
            });
    aimGrid->addWidget(new QLabel(tr("Target:"), aimGroup), 1, 0);
    aimGrid->addWidget(m_impl->aimTargetBoneCombo, 1, 1, 1, 3);

    m_impl->aimAxisCombo = new QComboBox(aimGroup);
    m_impl->aimAxisCombo->setObjectName(QStringLiteral("aimAxisCombo"));
    m_impl->aimAxisCombo->addItem(tr("+X Axis (1, 0)"), 0);
    m_impl->aimAxisCombo->addItem(tr("+Y Axis (0, 1)"), 1);
    m_impl->aimAxisCombo->addItem(tr("-X Axis (-1, 0)"), 2);
    m_impl->aimAxisCombo->addItem(tr("-Y Axis (0, -1)"), 3);
    m_impl->aimAxisCombo->setAccessibleName(tr("Local Aim Axis"));
    connect(m_impl->aimAxisCombo, qOverload<int>(&QComboBox::activated), this,
            &BoneRiggingPanel::onTransformOrConstraintEdited);
    aimGrid->addWidget(new QLabel(tr("Aim Axis:"), aimGroup), 2, 0);
    aimGrid->addWidget(m_impl->aimAxisCombo, 2, 1, 1, 3);

    m_impl->aimTargetXSpin = makeRigSpin(-16384.0, 16384.0, 1.0, 2, QStringLiteral(" px"),
                                         tr("Aim Target World X"), aimGroup);
    m_impl->aimTargetXSpin->setObjectName(QStringLiteral("aimTargetXSpin"));
    m_impl->aimTargetYSpin = makeRigSpin(-16384.0, 16384.0, 1.0, 2, QStringLiteral(" px"),
                                         tr("Aim Target World Y"), aimGroup);
    m_impl->aimTargetYSpin->setObjectName(QStringLiteral("aimTargetYSpin"));
    aimGrid->addWidget(new QLabel(tr("World X:"), aimGroup), 3, 0);
    aimGrid->addWidget(m_impl->aimTargetXSpin, 3, 1);
    aimGrid->addWidget(new QLabel(tr("World Y:"), aimGroup), 3, 2);
    aimGrid->addWidget(m_impl->aimTargetYSpin, 3, 3);

    m_impl->aimConeDegSpin = makeRigSpin(0.0, 180.0, 5.0, 1, QStringLiteral(" deg"),
                                         tr("Aim Maximum Cone Angle"), aimGroup);
    m_impl->aimConeDegSpin->setObjectName(QStringLiteral("aimConeDegSpin"));
    m_impl->aimConeDegSpin->setValue(90.0);
    m_impl->aimBlendSpin =
        makeRigSpin(0.0, 1.0, 0.05, 2, QString(), tr("Aim Blend Weight"), aimGroup);
    m_impl->aimBlendSpin->setObjectName(QStringLiteral("aimBlendSpin"));
    m_impl->aimBlendSpin->setValue(1.0);
    aimGrid->addWidget(new QLabel(tr("Max Cone:"), aimGroup), 4, 0);
    aimGrid->addWidget(m_impl->aimConeDegSpin, 4, 1);
    aimGrid->addWidget(new QLabel(tr("Blend:"), aimGroup), 4, 2);
    aimGrid->addWidget(m_impl->aimBlendSpin, 4, 3);

    for (QDoubleSpinBox *s : {m_impl->aimTargetXSpin, m_impl->aimTargetYSpin,
                              m_impl->aimConeDegSpin, m_impl->aimBlendSpin}) {
        connect(s, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                &BoneRiggingPanel::onTransformOrConstraintEdited);
    }

    contentLayout->addWidget(aimGroup);
    contentLayout->addStretch(1);

    scrollArea->setWidget(scrollContent);
    splitter->addWidget(scrollArea);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    mainLayout->addWidget(splitter, 1);

    setTabOrder(m_boneTree, m_addBoneBtn);
    setTabOrder(m_addBoneBtn, m_impl->deleteBoneBtn);
    setTabOrder(m_impl->deleteBoneBtn, m_impl->makeRootBtn);
    setTabOrder(m_impl->makeRootBtn, m_boneNameEdit);
    setTabOrder(m_boneNameEdit, m_impl->parentCombo);
    setTabOrder(m_impl->parentCombo, m_driveModeCombo);
    setTabOrder(m_driveModeCombo, m_posXSpin);
    setTabOrder(m_posXSpin, m_posYSpin);
    setTabOrder(m_posYSpin, m_angleZSpin);
    setTabOrder(m_angleZSpin, m_scaleXSpin);
    setTabOrder(m_scaleXSpin, m_scaleYSpin);
    setTabOrder(m_scaleYSpin, m_impl->ikChainCombo);
    setTabOrder(m_impl->ikChainCombo, m_impl->deleteIkBtn);
    setTabOrder(m_impl->deleteIkBtn, m_impl->ikRootCombo);
    setTabOrder(m_impl->ikRootCombo, m_impl->ikMidCombo);
    setTabOrder(m_impl->ikMidCombo, m_impl->ikEndCombo);
    setTabOrder(m_impl->ikEndCombo, m_createIkBtn);
    setTabOrder(m_createIkBtn, m_impl->ikTargetXSpin);
    setTabOrder(m_impl->ikTargetXSpin, m_impl->ikTargetYSpin);
    setTabOrder(m_impl->ikTargetYSpin, m_impl->ikPoleXSpin);
    setTabOrder(m_impl->ikPoleXSpin, m_impl->ikPoleYSpin);
    setTabOrder(m_impl->ikPoleYSpin, m_impl->ikPoleZSpin);
    setTabOrder(m_impl->ikPoleZSpin, m_poleFlipCheck);
    setTabOrder(m_poleFlipCheck, m_ikSoftZoneSpin);
    setTabOrder(m_ikSoftZoneSpin, m_ikMaxStretchSpin);
    setTabOrder(m_ikMaxStretchSpin, m_ikMinAngleSpin);
    setTabOrder(m_ikMinAngleSpin, m_ikMaxAngleSpin);
    setTabOrder(m_ikMaxAngleSpin, m_impl->ikMidMinAngleSpin);
    setTabOrder(m_impl->ikMidMinAngleSpin, m_impl->ikMidMaxAngleSpin);
    setTabOrder(m_impl->ikMidMaxAngleSpin, m_impl->ikBlendSpin);
    setTabOrder(m_impl->ikBlendSpin, m_impl->aimEnableCheck);
    setTabOrder(m_impl->aimEnableCheck, m_impl->aimTargetBoneCombo);
    setTabOrder(m_impl->aimTargetBoneCombo, m_impl->aimAxisCombo);
    setTabOrder(m_impl->aimAxisCombo, m_impl->aimTargetXSpin);
    setTabOrder(m_impl->aimTargetXSpin, m_impl->aimTargetYSpin);
    setTabOrder(m_impl->aimTargetYSpin, m_impl->aimConeDegSpin);
    setTabOrder(m_impl->aimConeDegSpin, m_impl->aimBlendSpin);
}

void BoneRiggingPanel::saveActiveObjectState()
{
    if (!m_impl || m_rig.objectId < 0) {
        return;
    }
    RigObjectState &st = m_impl->objectStates[m_rig.objectId];
    st.rig = m_rig;
    st.selectedBoneIndex = m_selectedBoneIndex;
    st.lastPushedXpbd = m_impl->lastPushedXpbd;
    st.forceReinitPhysics = m_impl->forceReinitPhysics;
    st.aimTargetBoneIds = m_impl->aimTargetBoneIds;
}

void BoneRiggingPanel::switchActiveObject(int32_t objectId)
{
    if (!m_impl || objectId == m_rig.objectId) {
        return;
    }
    saveActiveObjectState();
    const auto it = m_impl->objectStates.find(objectId);
    if (it != m_impl->objectStates.end()) {
        m_rig = it->second.rig;
        m_selectedBoneIndex = it->second.selectedBoneIndex;
        m_impl->lastPushedXpbd = it->second.lastPushedXpbd;
        m_impl->forceReinitPhysics = it->second.forceReinitPhysics;
        m_impl->aimTargetBoneIds = it->second.aimTargetBoneIds;
    } else {
        m_rig = BoneHierarchySnapshot{};
        m_rig.objectId = objectId;
        m_rig.driveModes.fill(WE_BONE_DRIVE_FK);
        m_selectedBoneIndex = -1;
        m_impl->lastPushedXpbd.fill(false);
        m_impl->forceReinitPhysics = false;
        m_impl->aimTargetBoneIds.clear();
    }
}

void BoneRiggingPanel::applySnapshotState(const BoneHierarchySnapshot &snap, int32_t nextSelected,
                                          const std::vector<int32_t> *remapForEmit)
{
    if (!m_impl) {
        return;
    }
    if (snap.objectId >= 0 && snap.objectId != m_rig.objectId) {
        switchActiveObject(snap.objectId);
    }

    BoneHierarchySnapshot nextRig = snap;
    if (remapForEmit) {
        remapXpbdState(m_impl->lastPushedXpbd, m_impl->forceReinitPhysics, *remapForEmit);
        m_impl->aimTargetBoneIds =
            remapAimTargets(m_impl->aimTargetBoneIds, *remapForEmit, nextRig.bones.size());
    } else {
        m_impl->aimTargetBoneIds.resize(nextRig.bones.size(), -1);
    }

    const size_t nextLimit = std::min<size_t>(nextRig.bones.size(), WE_MDL_MAX_BONES);
    for (size_t i = 0u; i < nextLimit; ++i) {
        if (nextRig.driveModes[i] != WE_BONE_DRIVE_XPBD) {
            std::memset(nextRig.bones[i].simulation_json, 0,
                        sizeof(nextRig.bones[i].simulation_json));
            const bool wasPrevXpbd =
                (i < m_rig.bones.size() && m_rig.driveModes[i] == WE_BONE_DRIVE_XPBD);
            if (wasPrevXpbd || m_impl->lastPushedXpbd[i]) {
                m_impl->forceReinitPhysics = true;
            }
        }
    }
    for (size_t i = nextLimit; i < WE_MDL_MAX_BONES; ++i) {
        if (m_impl->lastPushedXpbd[i]) {
            m_impl->forceReinitPhysics = true;
        }
    }

    const int32_t prevSelected = m_selectedBoneIndex;
    m_rig = std::move(nextRig);
    m_selectedBoneIndex =
        m_rig.bones.empty()
            ? -1
            : ((nextSelected >= 0)
                   ? std::clamp(nextSelected, 0, static_cast<int32_t>(m_rig.bones.size()) - 1)
                   : -1);

    rebuildBoneTree();
    syncInspectorFromSelectedBone();
    pushSkeletonToDaemon();
    saveActiveObjectState();

    if (remapForEmit && !remapForEmit->empty()) {
        emit bonesRemapped(m_rig.objectId, *remapForEmit);
    }
    if (m_selectedBoneIndex != prevSelected || remapForEmit) {
        emit boneSelected(m_selectedBoneIndex);
    }
    emit skeletonUpdated(m_rig.objectId, m_rig);
}

void BoneRiggingPanel::loadPuppetRig(const we_puppet_t *puppet, int32_t sceneObjectId)
{
    if (m_impl && m_rig.objectId >= 0 && m_rig.objectId != sceneObjectId) {
        saveActiveObjectState();
    }

    m_rig.objectId = sceneObjectId;
    m_rig.bones.clear();
    m_rig.ikChains.clear();
    m_rig.aimConstraints.clear();
    m_rig.driveModes.fill(WE_BONE_DRIVE_FK);

    if (puppet && puppet->bone_count > 0u && puppet->bones) {
        const uint32_t count = std::min<uint32_t>(puppet->bone_count, WE_MDL_MAX_BONES);
        m_rig.bones.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            we_mdl_bone_t b = puppet->bones[i];
            b.name[sizeof(b.name) - 1u] = '\0';
            b.simulation_json[sizeof(b.simulation_json) - 1u] = '\0';
            if (b.parent != 0xFFFFFFFFu && b.parent >= count) {
                b.parent = 0xFFFFFFFFu;
            }
            const bool scaleUnset =
                (std::fabs(b.bind_scale[0]) < 1e-6f && std::fabs(b.bind_scale[1]) < 1e-6f &&
                 std::fabs(b.bind_scale[2]) < 1e-6f);
            if (scaleUnset) {
                mat4DecomposeTrs(b.local_trans, b.bind_pos, b.bind_angle, b.bind_scale);
            }
            m_rig.bones.push_back(b);
            if (boneHasActiveSimulationJson(b.simulation_json)) {
                m_rig.driveModes[i] = WE_BONE_DRIVE_XPBD;
            }
        }
        m_selectedBoneIndex = 0;
        std::vector<int32_t> remap;
        ensureTopologicalOrder(m_rig, m_selectedBoneIndex, &remap);
        if (m_impl && isNonIdentityRemap(remap)) {
            emit bonesRemapped(m_rig.objectId, remap);
        }
    } else {
        m_selectedBoneIndex = -1;
    }

    if (m_impl) {
        m_impl->forceReinitPhysics = false;
        m_impl->aimTargetBoneIds.assign(m_rig.bones.size(), -1);
        for (size_t i = 0u; i < WE_MDL_MAX_BONES; ++i) {
            m_impl->lastPushedXpbd[i] =
                (i < m_rig.bones.size() && m_rig.driveModes[i] == WE_BONE_DRIVE_XPBD);
        }
    }

    rebuildBoneTree();
    syncInspectorFromSelectedBone();
    saveActiveObjectState();
    emit boneSelected(m_selectedBoneIndex);
}

void BoneRiggingPanel::setBoneDriveMode(int32_t objectId, int32_t boneIndex,
                                        we_bone_drive_mode_t mode)
{
    if (objectId < 0 || objectId != m_rig.objectId || boneIndex < 0) {
        return;
    }
    const size_t idx = static_cast<size_t>(boneIndex);
    if (idx >= m_rig.bones.size() || idx >= WE_MDL_MAX_BONES) {
        return;
    }
    if (m_rig.driveModes[idx] == mode) {
        return;
    }

    BoneHierarchySnapshot newRig = m_rig;
    applyXpbdDriveState(newRig, idx, mode);
    applySnapshotState(newRig, m_selectedBoneIndex, nullptr);
}

void BoneRiggingPanel::selectBone(int32_t boneIndex)
{
    if (boneIndex < -1 ||
        (boneIndex >= 0 && static_cast<size_t>(boneIndex) >= m_rig.bones.size())) {
        return;
    }
    m_selectedBoneIndex = boneIndex;

    if (m_boneTree) {
        const QSignalBlocker blocker(m_boneTree);
        if (boneIndex < 0) {
            m_boneTree->setCurrentItem(nullptr);
        } else {
            QTreeWidgetItemIterator it(m_boneTree);
            while (*it) {
                if ((*it)->data(0, Qt::UserRole).toInt() == boneIndex) {
                    m_boneTree->setCurrentItem(*it);
                    break;
                }
                ++it;
            }
        }
    }

    syncInspectorFromSelectedBone();
    saveActiveObjectState();
    emit boneSelected(m_selectedBoneIndex);
}

bool BoneRiggingPanel::wouldCreateCycle(int32_t childIdx, int32_t parentIdx) const noexcept
{
    if (parentIdx < 0) {
        return false;
    }
    const int32_t n = static_cast<int32_t>(m_rig.bones.size());
    if (childIdx < 0 || childIdx >= n || parentIdx >= n) {
        return true;
    }
    if (childIdx == parentIdx) {
        return true;
    }

    std::vector<uint8_t> visited(static_cast<size_t>(n), 0u);
    int32_t cur = parentIdx;
    while (cur >= 0 && cur < n) {
        if (cur == childIdx) {
            return true;
        }
        if (visited[static_cast<size_t>(cur)] != 0u) {
            return true;
        }
        visited[static_cast<size_t>(cur)] = 1u;
        const uint32_t p = m_rig.bones[static_cast<size_t>(cur)].parent;
        if (p == 0xFFFFFFFFu || static_cast<int32_t>(p) >= n) {
            break;
        }
        cur = static_cast<int32_t>(p);
    }
    return false;
}

bool BoneRiggingPanel::addBone(const QString &name, int32_t parentIndex, const float localPos[3],
                               float angleZRad)
{
    if (m_rig.bones.size() >= WE_MDL_MAX_BONES) {
        emit statusMessage(
            tr("Cannot add bone: maximum skeleton limit (%1) reached").arg(WE_MDL_MAX_BONES));
        return false;
    }
    if (parentIndex < -1 || parentIndex >= static_cast<int32_t>(m_rig.bones.size())) {
        return false;
    }

    BoneHierarchySnapshot oldRig = m_rig;
    BoneHierarchySnapshot newRig = m_rig;

    we_mdl_bone_t b{};
    QString cleanName = name.trimmed();
    if (cleanName.isEmpty()) {
        cleanName = QStringLiteral("Bone_%1").arg(newRig.bones.size());
    }
    const QByteArray utf8 = cleanName.toUtf8();
    const size_t copyLen = std::min<size_t>(static_cast<size_t>(utf8.size()), sizeof(b.name) - 1u);
    if (copyLen > 0u) {
        std::memcpy(b.name, utf8.constData(), copyLen);
    }
    b.name[copyLen] = '\0';

    b.parent = (parentIndex < 0) ? 0xFFFFFFFFu : static_cast<uint32_t>(parentIndex);
    if (localPos) {
        b.bind_pos[0] = finiteOr(localPos[0], 0.0f);
        b.bind_pos[1] = finiteOr(localPos[1], 0.0f);
        b.bind_pos[2] = finiteOr(localPos[2], 0.0f);
    }
    b.bind_angle[0] = 0.0f;
    b.bind_angle[1] = 0.0f;
    b.bind_angle[2] = finiteOr(angleZRad, 0.0f);
    b.bind_scale[0] = 1.0f;
    b.bind_scale[1] = 1.0f;
    b.bind_scale[2] = 1.0f;

    const size_t newIdx = newRig.bones.size();
    newRig.bones.push_back(b);
    if (newIdx < WE_MDL_MAX_BONES) {
        newRig.driveModes[newIdx] = WE_BONE_DRIVE_FK;
    }
    recomputeWorldAndOffsetTransforms(newRig);

    const int32_t newBoneIdx = static_cast<int32_t>(newIdx);
    const int32_t prevSelected = m_selectedBoneIndex;
    const size_t newSize = newRig.bones.size();

    QPointer<BoneRiggingPanel> self(this);
    auto applyFn = [self, newSize, newBoneIdx, prevSelected](const BoneHierarchySnapshot &snap) {
        if (!self) {
            return;
        }
        const int32_t targetSel = (snap.bones.size() == newSize) ? newBoneIdx : prevSelected;
        self->applySnapshotState(snap, targetSel, nullptr);
    };

    if (m_commandBus) {
        m_commandBus->execute(std::make_unique<BoneEditCommand>(
            tr("Add Bone %1").arg(cleanName), std::move(oldRig), std::move(newRig), applyFn));
    } else {
        applyFn(newRig);
    }

    emit statusMessage(tr("Added bone '%1'").arg(cleanName));
    return true;
}

bool BoneRiggingPanel::reparentBone(int32_t childIndex, int32_t newParentIndex)
{
    const int32_t n = static_cast<int32_t>(m_rig.bones.size());
    if (childIndex < 0 || childIndex >= n || newParentIndex < -1 || newParentIndex >= n) {
        return false;
    }
    if (wouldCreateCycle(childIndex, newParentIndex)) {
        emit statusMessage(tr("Reparent rejected: assigning bone %1 under %2 would create a cycle")
                               .arg(childIndex)
                               .arg(newParentIndex));
        return false;
    }

    const uint32_t targetParent =
        (newParentIndex < 0) ? 0xFFFFFFFFu : static_cast<uint32_t>(newParentIndex);
    if (m_rig.bones[static_cast<size_t>(childIndex)].parent == targetParent) {
        return true;
    }

    BoneHierarchySnapshot oldRig = m_rig;
    BoneHierarchySnapshot newRig = m_rig;
    recomputeWorldAndOffsetTransforms(newRig);
    applyWorldPreservingParent(newRig, static_cast<size_t>(childIndex), targetParent);

    const int32_t oldSelected = m_selectedBoneIndex;
    int32_t trackedIdx = childIndex;
    std::vector<int32_t> forwardMap;
    ensureTopologicalOrder(newRig, trackedIdx, &forwardMap);

    std::vector<int32_t> inverseMap(forwardMap.size(), -1);
    for (size_t oldI = 0u; oldI < forwardMap.size(); ++oldI) {
        const int32_t newI = forwardMap[oldI];
        if (newI >= 0 && static_cast<size_t>(newI) < inverseMap.size()) {
            inverseMap[static_cast<size_t>(newI)] = static_cast<int32_t>(oldI);
        }
    }

    std::vector<uint32_t> newParents(newRig.bones.size());
    for (size_t i = 0u; i < newRig.bones.size(); ++i) {
        newParents[i] = newRig.bones[i].parent;
    }
    const bool reordered = isNonIdentityRemap(forwardMap);

    QPointer<BoneRiggingPanel> self(this);
    auto applyFn = [self, trackedIdx, oldSelected, forwardMap, inverseMap, newParents,
                    reordered](const BoneHierarchySnapshot &snap) {
        if (!self) {
            return;
        }
        bool isForward = (snap.bones.size() == newParents.size());
        if (isForward) {
            for (size_t i = 0u; i < newParents.size(); ++i) {
                if (snap.bones[i].parent != newParents[i]) {
                    isForward = false;
                    break;
                }
            }
        }
        const std::vector<int32_t> *remapPtr =
            reordered ? (isForward ? &forwardMap : &inverseMap) : nullptr;
        self->applySnapshotState(snap, isForward ? trackedIdx : oldSelected, remapPtr);
    };

    if (m_commandBus) {
        m_commandBus->execute(std::make_unique<BoneEditCommand>(
            tr("Reparent Bone"), std::move(oldRig), std::move(newRig), applyFn));
    } else {
        applyFn(newRig);
    }

    emit statusMessage(tr("Reparented bone #%1").arg(trackedIdx));
    return true;
}

void BoneRiggingPanel::dispatchIkPreview(int32_t objectId, const we_ik_chain_t &chain)
{
    emit ikPreviewRequested(objectId, chain);
    // MainWindow listens to ikPreviewRequested and sends PREVIEW_IK; only send directly when
    // no slot is connected to avoid duplicate wire packets.
    if (m_ipcClient && objectId >= 0 &&
        !isSignalConnected(QMetaMethod::fromSignal(&BoneRiggingPanel::ikPreviewRequested))) {
        waywal_ipc_puppet_preview_ik_t req{};
        req.object_id = static_cast<uint32_t>(objectId);
        req.root_bone = chain.root_bone;
        req.mid_bone = chain.mid_bone;
        req.end_bone = chain.end_bone;
        req.pole_flip = (chain.bend_sign < 0) ? 1u : 0u;
        req.target_world[0] = chain.target_world[0];
        req.target_world[1] = chain.target_world[1];
        req.target_world[2] = chain.target_world[2];
        req.override_count = 0u;
        req.mode_flags = (chain.enabled != 0u) ? 1u : 2u;
        const std::vector<waywal_ipc_bone_trs_t> emptyOverrides;
        m_ipcClient->sendPuppetPreviewIk(req, emptyOverrides);
    }
}

bool BoneRiggingPanel::createTwoBoneIkChain(uint16_t rootBone, uint16_t midBone, uint16_t endBone)
{
    const size_t n = m_rig.bones.size();
    if (rootBone >= n || midBone >= n || endBone >= n) {
        return false;
    }
    if (rootBone == midBone || midBone == endBone || rootBone == endBone) {
        return false;
    }
    if (!isBoneDescendant(m_rig, rootBone, midBone) || !isBoneDescendant(m_rig, midBone, endBone)) {
        emit statusMessage(
            tr("IK chain rejected: mid bone must descend from root and end bone must descend "
               "from mid"));
        return false;
    }

    auto existingIt =
        std::find_if(m_rig.ikChains.begin(), m_rig.ikChains.end(),
                     [endBone](const we_ik_chain_t &c) { return c.end_bone == endBone; });
    if (existingIt == m_rig.ikChains.end() && m_rig.ikChains.size() >= WE_RIG_MAX_IK_CHAINS) {
        emit statusMessage(tr("Maximum IK chain count (%1) reached").arg(WE_RIG_MAX_IK_CHAINS));
        return false;
    }

    BoneHierarchySnapshot oldRig = m_rig;
    BoneHierarchySnapshot newRig = m_rig;
    recomputeWorldAndOffsetTransforms(newRig);

    const float seg1Len = worldBoneDistance(newRig, rootBone, midBone);
    const float seg2Len = worldBoneDistance(newRig, midBone, endBone);
    if (seg1Len <= kMinIkSegmentLen || seg2Len <= kMinIkSegmentLen) {
        emit statusMessage(
            tr("IK chain rejected: bone segment lengths must be non-zero (root->mid=%1, "
               "mid->end=%2)")
                .arg(static_cast<double>(seg1Len), 0, 'f', 3)
                .arg(static_cast<double>(seg2Len), 0, 'f', 3));
        return false;
    }

    we_ik_chain_t chain{};
    chain.root_bone = rootBone;
    chain.mid_bone = midBone;
    chain.end_bone = endBone;
    chain.bend_sign = (m_poleFlipCheck && m_poleFlipCheck->isChecked()) ? -1 : 1;
    chain.enabled = 1u;
    chain.target_world[0] = newRig.bones[endBone].world_bind[12];
    chain.target_world[1] = newRig.bones[endBone].world_bind[13];
    chain.target_world[2] = newRig.bones[endBone].world_bind[14];
    chain.pole_world[0] = newRig.bones[midBone].world_bind[12];
    chain.pole_world[1] =
        newRig.bones[midBone].world_bind[13] + 48.0f * static_cast<float>(chain.bend_sign);
    chain.pole_world[2] = newRig.bones[midBone].world_bind[14];
    chain.soft_zone_ratio =
        m_ikSoftZoneSpin ? std::clamp(static_cast<float>(m_ikSoftZoneSpin->value()), 0.0f, 0.25f)
                         : 0.05f;
    chain.max_stretch_ratio =
        m_ikMaxStretchSpin ? std::clamp(static_cast<float>(m_ikMaxStretchSpin->value()), 1.0f, 1.5f)
                           : 1.0f;
    chain.root_angle_min_rad =
        m_ikMinAngleSpin ? static_cast<float>(m_ikMinAngleSpin->value()) * kDegToRad : -kPi;
    chain.root_angle_max_rad =
        m_ikMaxAngleSpin ? static_cast<float>(m_ikMaxAngleSpin->value()) * kDegToRad : kPi;
    chain.mid_angle_min_rad =
        (m_impl && m_impl->ikMidMinAngleSpin)
            ? static_cast<float>(m_impl->ikMidMinAngleSpin->value()) * kDegToRad
            : chain.root_angle_min_rad;
    chain.mid_angle_max_rad =
        (m_impl && m_impl->ikMidMaxAngleSpin)
            ? static_cast<float>(m_impl->ikMidMaxAngleSpin->value()) * kDegToRad
            : chain.root_angle_max_rad;
    chain.blend_weight = 1.0f;

    if (existingIt != m_rig.ikChains.end()) {
        const size_t idx = static_cast<size_t>(std::distance(m_rig.ikChains.begin(), existingIt));
        newRig.ikChains[idx] = chain;
    } else {
        newRig.ikChains.push_back(chain);
    }

    for (uint16_t bIdx : {rootBone, midBone, endBone}) {
        if (bIdx < WE_MDL_MAX_BONES) {
            newRig.driveModes[bIdx] = WE_BONE_DRIVE_IK;
        }
    }

    QPointer<BoneRiggingPanel> self(this);
    const int32_t keepSelected = m_selectedBoneIndex;
    auto applyFn = [self, keepSelected, chain](const BoneHierarchySnapshot &snap) {
        if (!self) {
            return;
        }
        self->applySnapshotState(snap, keepSelected, nullptr);
        if (chain.enabled != 0u) {
            self->dispatchIkPreview(self->m_rig.objectId, chain);
        }
    };

    if (m_commandBus) {
        m_commandBus->execute(std::make_unique<BoneEditCommand>(
            tr("Create 2-Bone IK Chain"), std::move(oldRig), std::move(newRig), applyFn));
    } else {
        applyFn(newRig);
    }

    emit statusMessage(
        tr("Created 2-bone IK chain (%1 -> %2 -> %3)").arg(rootBone).arg(midBone).arg(endBone));
    return true;
}

void BoneRiggingPanel::onAddBoneClicked()
{
    const QString baseName = QStringLiteral("Bone_%1").arg(m_rig.bones.size());
    const float offset[3] = {m_selectedBoneIndex >= 0 ? 40.0f : 0.0f, 0.0f, 0.0f};
    addBone(baseName, m_selectedBoneIndex, offset, 0.0f);
}

void BoneRiggingPanel::onDeleteBoneClicked()
{
    const size_t n = m_rig.bones.size();
    if (m_selectedBoneIndex < 0 || static_cast<size_t>(m_selectedBoneIndex) >= n) {
        return;
    }

    const uint32_t delIdx = static_cast<uint32_t>(m_selectedBoneIndex);
    BoneHierarchySnapshot oldRig = m_rig;
    BoneHierarchySnapshot newRig = m_rig;
    recomputeWorldAndOffsetTransforms(newRig);

    const uint32_t inheritedParent = newRig.bones[delIdx].parent;
    const QString deletedName = QString::fromUtf8(newRig.bones[delIdx].name);

    for (size_t i = 0; i < n; ++i) {
        if (i == delIdx) {
            continue;
        }
        if (newRig.bones[i].parent == delIdx) {
            applyWorldPreservingParent(newRig, i, inheritedParent);
        }
    }

    auto remapIdx = [delIdx](uint32_t idx) -> uint32_t {
        if (idx == 0xFFFFFFFFu || idx == 0xFFFFu || idx < delIdx) {
            return idx;
        }
        return idx - 1u;
    };

    newRig.bones.erase(newRig.bones.begin() + static_cast<ptrdiff_t>(delIdx));
    for (auto &b : newRig.bones) {
        b.parent = remapIdx(b.parent);
    }

    for (uint32_t i = delIdx; i + 1u < WE_MDL_MAX_BONES; ++i) {
        newRig.driveModes[i] = newRig.driveModes[i + 1u];
    }
    newRig.driveModes[WE_MDL_MAX_BONES - 1u] = WE_BONE_DRIVE_FK;

    newRig.ikChains.erase(std::remove_if(newRig.ikChains.begin(), newRig.ikChains.end(),
                                         [delIdx](const we_ik_chain_t &c) {
                                             return c.root_bone == delIdx || c.mid_bone == delIdx ||
                                                    c.end_bone == delIdx;
                                         }),
                          newRig.ikChains.end());
    for (auto &c : newRig.ikChains) {
        c.root_bone = static_cast<uint16_t>(remapIdx(c.root_bone));
        c.mid_bone = static_cast<uint16_t>(remapIdx(c.mid_bone));
        c.end_bone = static_cast<uint16_t>(remapIdx(c.end_bone));
    }

    newRig.aimConstraints.erase(
        std::remove_if(newRig.aimConstraints.begin(), newRig.aimConstraints.end(),
                       [delIdx](const we_aim_constraint_t &a) { return a.bone_index == delIdx; }),
        newRig.aimConstraints.end());
    for (auto &a : newRig.aimConstraints) {
        a.bone_index = static_cast<uint16_t>(remapIdx(a.bone_index));
    }

    int32_t nextSelected = newRig.bones.empty()
                               ? -1
                               : std::min<int32_t>(static_cast<int32_t>(delIdx),
                                                   static_cast<int32_t>(newRig.bones.size()) - 1);
    std::vector<int32_t> topoMap;
    ensureTopologicalOrder(newRig, nextSelected, &topoMap);

    std::vector<int32_t> forwardMap(n, -1);
    for (size_t oldI = 0u; oldI < n; ++oldI) {
        if (oldI == delIdx) {
            forwardMap[oldI] = -1;
        } else {
            const size_t postErase = (oldI < delIdx) ? oldI : (oldI - 1u);
            forwardMap[oldI] = (postErase < topoMap.size()) ? topoMap[postErase] : -1;
        }
    }

    std::vector<int32_t> inverseMap(newRig.bones.size(), -1);
    for (size_t oldI = 0u; oldI < n; ++oldI) {
        const int32_t newI = forwardMap[oldI];
        if (newI >= 0 && static_cast<size_t>(newI) < inverseMap.size()) {
            inverseMap[static_cast<size_t>(newI)] = static_cast<int32_t>(oldI);
        }
    }

    const int32_t oldSelected = m_selectedBoneIndex;
    const size_t newSize = newRig.bones.size();

    QPointer<BoneRiggingPanel> self(this);
    auto applyFn = [self, newSize, nextSelected, oldSelected, forwardMap,
                    inverseMap](const BoneHierarchySnapshot &snap) {
        if (!self) {
            return;
        }
        const bool isForward = (snap.bones.size() == newSize);
        const std::vector<int32_t> &remap = isForward ? forwardMap : inverseMap;
        self->applySnapshotState(snap, isForward ? nextSelected : oldSelected, &remap);
    };

    if (m_commandBus) {
        m_commandBus->execute(std::make_unique<BoneEditCommand>(
            tr("Delete Bone %1").arg(deletedName), std::move(oldRig), std::move(newRig), applyFn));
    } else {
        applyFn(newRig);
    }

    emit statusMessage(tr("Deleted bone '%1'").arg(deletedName));
}

void BoneRiggingPanel::onCreateIkChainClicked()
{
    if (m_rig.bones.size() < 3u) {
        emit statusMessage(tr("At least 3 bones are required to create a 2-bone IK chain"));
        return;
    }

    int32_t rootIdx = -1;
    int32_t midIdx = -1;
    int32_t endIdx = -1;

    if (m_impl && m_impl->ikRootCombo && m_impl->ikMidCombo && m_impl->ikEndCombo &&
        m_impl->ikRootCombo->currentIndex() >= 0 && m_impl->ikMidCombo->currentIndex() >= 0 &&
        m_impl->ikEndCombo->currentIndex() >= 0) {
        rootIdx = m_impl->ikRootCombo->currentData().toInt();
        midIdx = m_impl->ikMidCombo->currentData().toInt();
        endIdx = m_impl->ikEndCombo->currentData().toInt();
    }

    const bool comboChainValid =
        (rootIdx >= 0 && midIdx >= 0 && endIdx >= 0 && rootIdx != midIdx && midIdx != endIdx &&
         rootIdx != endIdx &&
         isBoneDescendant(m_rig, static_cast<uint16_t>(rootIdx), static_cast<uint16_t>(midIdx)) &&
         isBoneDescendant(m_rig, static_cast<uint16_t>(midIdx), static_cast<uint16_t>(endIdx)));

    if (!comboChainValid && m_selectedBoneIndex >= 0 &&
        static_cast<size_t>(m_selectedBoneIndex) < m_rig.bones.size()) {
        endIdx = m_selectedBoneIndex;
        const uint32_t midU = m_rig.bones[static_cast<size_t>(endIdx)].parent;
        if (midU != 0xFFFFFFFFu && midU < m_rig.bones.size()) {
            midIdx = static_cast<int32_t>(midU);
            const uint32_t rootU = m_rig.bones[midU].parent;
            if (rootU != 0xFFFFFFFFu && rootU < m_rig.bones.size()) {
                rootIdx = static_cast<int32_t>(rootU);
            } else {
                rootIdx = -1;
            }
        } else {
            midIdx = -1;
            rootIdx = -1;
        }
    }

    if (rootIdx < 0 || midIdx < 0 || endIdx < 0 || rootIdx == midIdx || midIdx == endIdx ||
        rootIdx == endIdx) {
        emit statusMessage(
            tr("Select a valid 3-bone descendant chain (Root -> Mid -> End) to create IK"));
        return;
    }

    createTwoBoneIkChain(static_cast<uint16_t>(rootIdx), static_cast<uint16_t>(midIdx),
                         static_cast<uint16_t>(endIdx));
}

void BoneRiggingPanel::onTogglePoleFlipClicked()
{
    if (m_updatingUi) {
        return;
    }
    onTransformOrConstraintEdited();
}

void BoneRiggingPanel::onBoneTreeCurrentItemChanged(QTreeWidgetItem *current,
                                                    QTreeWidgetItem *previous)
{
    (void)previous;
    if (m_updatingUi) {
        return;
    }
    const int32_t nextIdx = current ? current->data(0, Qt::UserRole).toInt() : -1;
    if (nextIdx == m_selectedBoneIndex) {
        return;
    }
    m_selectedBoneIndex = nextIdx;
    syncInspectorFromSelectedBone();
    saveActiveObjectState();
    emit boneSelected(m_selectedBoneIndex);
}

void BoneRiggingPanel::onTransformOrConstraintEdited()
{
    if (m_updatingUi || !m_impl) {
        return;
    }

    const bool hasSelectedBone =
        (m_selectedBoneIndex >= 0 && static_cast<size_t>(m_selectedBoneIndex) < m_rig.bones.size());

    int activeChainIdx = -1;
    if (m_impl->ikChainCombo && !m_rig.ikChains.empty()) {
        const int candidate = m_impl->ikChainCombo->currentData().toInt();
        if (candidate >= 0 && static_cast<size_t>(candidate) < m_rig.ikChains.size()) {
            activeChainIdx = candidate;
        }
    }

    if (!hasSelectedBone && activeChainIdx < 0) {
        return;
    }

    BoneHierarchySnapshot oldRig = m_rig;
    BoneHierarchySnapshot newRig = m_rig;
    QString cmdTitle = tr("Edit IK Chain");

    if (hasSelectedBone) {
        const size_t sel = static_cast<size_t>(m_selectedBoneIndex);
        we_mdl_bone_t &bone = newRig.bones[sel];

        if (m_boneNameEdit) {
            const QString trimmed = m_boneNameEdit->text().trimmed();
            if (!trimmed.isEmpty()) {
                const QByteArray utf8 = trimmed.toUtf8();
                const size_t len =
                    std::min<size_t>(static_cast<size_t>(utf8.size()), sizeof(bone.name) - 1u);
                std::memset(bone.name, 0, sizeof(bone.name));
                std::memcpy(bone.name, utf8.constData(), len);
            }
        }

        if (m_posXSpin) {
            bone.bind_pos[0] = static_cast<float>(m_posXSpin->value());
        }
        if (m_posYSpin) {
            bone.bind_pos[1] = static_cast<float>(m_posYSpin->value());
        }
        if (m_angleZSpin) {
            bone.bind_angle[2] = static_cast<float>(m_angleZSpin->value()) * kDegToRad;
        }
        if (m_scaleXSpin) {
            bone.bind_scale[0] = static_cast<float>(m_scaleXSpin->value());
        }
        if (m_scaleYSpin) {
            bone.bind_scale[1] = static_cast<float>(m_scaleYSpin->value());
        }
        if (std::fabs(bone.bind_scale[0]) < 1e-5f) {
            bone.bind_scale[0] = 1.0f;
        }
        if (std::fabs(bone.bind_scale[1]) < 1e-5f) {
            bone.bind_scale[1] = 1.0f;
        }

        if (m_driveModeCombo && sel < WE_MDL_MAX_BONES) {
            const auto mode =
                static_cast<we_bone_drive_mode_t>(m_driveModeCombo->currentData().toUInt());
            applyXpbdDriveState(newRig, sel, mode);
        }

        if (m_impl->aimEnableCheck) {
            auto aimIt =
                std::find_if(newRig.aimConstraints.begin(), newRig.aimConstraints.end(),
                             [sel](const we_aim_constraint_t &a) { return a.bone_index == sel; });
            const bool enabled = m_impl->aimEnableCheck->isChecked();
            if (enabled && aimIt == newRig.aimConstraints.end() &&
                newRig.aimConstraints.size() < WE_RIG_MAX_AIM_CONSTRAINTS) {
                we_aim_constraint_t created{};
                created.bone_index = static_cast<uint16_t>(sel);
                created.enabled = 1u;
                created.aim_axis_local[0] = 1.0f;
                created.max_cone_rad = 0.5f * kPi;
                created.blend_weight = 1.0f;
                newRig.aimConstraints.push_back(created);
                aimIt = std::prev(newRig.aimConstraints.end());
            }
            if (aimIt != newRig.aimConstraints.end()) {
                aimIt->enabled = enabled ? 1u : 0u;
                if (m_impl->aimAxisCombo) {
                    const int axisId = m_impl->aimAxisCombo->currentData().toInt();
                    aimIt->aim_axis_local[0] = (axisId == 0) ? 1.0f : (axisId == 2 ? -1.0f : 0.0f);
                    aimIt->aim_axis_local[1] = (axisId == 1) ? 1.0f : (axisId == 3 ? -1.0f : 0.0f);
                    aimIt->aim_axis_local[2] = 0.0f;
                }
                if (m_impl->aimTargetXSpin) {
                    aimIt->target_world[0] = static_cast<float>(m_impl->aimTargetXSpin->value());
                }
                if (m_impl->aimTargetYSpin) {
                    aimIt->target_world[1] = static_cast<float>(m_impl->aimTargetYSpin->value());
                }
                if (m_impl->aimConeDegSpin) {
                    aimIt->max_cone_rad =
                        static_cast<float>(m_impl->aimConeDegSpin->value()) * kDegToRad;
                }
                if (m_impl->aimBlendSpin) {
                    aimIt->blend_weight =
                        std::clamp(static_cast<float>(m_impl->aimBlendSpin->value()), 0.0f, 1.0f);
                }
            }
        }

        cmdTitle = tr("Edit Bone %1").arg(QString::fromUtf8(bone.name));
    }

    bool ikChainModified = false;
    if (activeChainIdx >= 0 && static_cast<size_t>(activeChainIdx) < newRig.ikChains.size()) {
        we_ik_chain_t &chain = newRig.ikChains[static_cast<size_t>(activeChainIdx)];
        if (m_impl->ikTargetXSpin) {
            chain.target_world[0] = static_cast<float>(m_impl->ikTargetXSpin->value());
        }
        if (m_impl->ikTargetYSpin) {
            chain.target_world[1] = static_cast<float>(m_impl->ikTargetYSpin->value());
        }
        if (m_impl->ikPoleXSpin) {
            chain.pole_world[0] = static_cast<float>(m_impl->ikPoleXSpin->value());
        }
        if (m_impl->ikPoleYSpin) {
            chain.pole_world[1] = static_cast<float>(m_impl->ikPoleYSpin->value());
        }
        if (m_impl->ikPoleZSpin) {
            chain.pole_world[2] = static_cast<float>(m_impl->ikPoleZSpin->value());
        }
        if (m_poleFlipCheck) {
            chain.bend_sign = m_poleFlipCheck->isChecked() ? -1 : 1;
        }
        if (m_ikSoftZoneSpin) {
            chain.soft_zone_ratio =
                std::clamp(static_cast<float>(m_ikSoftZoneSpin->value()), 0.0f, 0.25f);
        }
        if (m_ikMaxStretchSpin) {
            chain.max_stretch_ratio =
                std::clamp(static_cast<float>(m_ikMaxStretchSpin->value()), 1.0f, 1.5f);
        }
        if (m_ikMinAngleSpin) {
            chain.root_angle_min_rad = static_cast<float>(m_ikMinAngleSpin->value()) * kDegToRad;
        }
        if (m_ikMaxAngleSpin) {
            chain.root_angle_max_rad = static_cast<float>(m_ikMaxAngleSpin->value()) * kDegToRad;
        }
        if (m_impl->ikMidMinAngleSpin) {
            chain.mid_angle_min_rad =
                static_cast<float>(m_impl->ikMidMinAngleSpin->value()) * kDegToRad;
        }
        if (m_impl->ikMidMaxAngleSpin) {
            chain.mid_angle_max_rad =
                static_cast<float>(m_impl->ikMidMaxAngleSpin->value()) * kDegToRad;
        }
        if (m_impl->ikBlendSpin) {
            chain.blend_weight =
                std::clamp(static_cast<float>(m_impl->ikBlendSpin->value()), 0.0f, 1.0f);
        }
        if (static_cast<size_t>(activeChainIdx) < oldRig.ikChains.size()) {
            ikChainModified = (std::memcmp(&oldRig.ikChains[static_cast<size_t>(activeChainIdx)],
                                           &chain, sizeof(we_ik_chain_t)) != 0);
        }
    }

    recomputeWorldAndOffsetTransforms(newRig);

    QPointer<BoneRiggingPanel> self(this);
    const int32_t keepSelected = m_selectedBoneIndex;
    auto applyFn = [self, keepSelected, activeChainIdx,
                    ikChainModified](const BoneHierarchySnapshot &snap) {
        if (!self) {
            return;
        }
        self->applySnapshotState(snap, keepSelected, nullptr);
        if (ikChainModified && activeChainIdx >= 0 &&
            static_cast<size_t>(activeChainIdx) < self->m_rig.ikChains.size()) {
            const we_ik_chain_t &activeChain =
                self->m_rig.ikChains[static_cast<size_t>(activeChainIdx)];
            if (activeChain.enabled != 0u) {
                self->dispatchIkPreview(self->m_rig.objectId, activeChain);
            }
        }
    };

    if (m_commandBus) {
        m_commandBus->execute(std::make_unique<BoneEditCommand>(cmdTitle, std::move(oldRig),
                                                                std::move(newRig), applyFn));
    } else {
        applyFn(newRig);
    }
}

void BoneRiggingPanel::rebuildBoneTree()
{
    if (!m_boneTree) {
        return;
    }

    const QScopedValueRollback<bool> guard(m_updatingUi, true);
    const QSignalBlocker blocker(m_boneTree);
    m_boneTree->clear();

    const size_t n = m_rig.bones.size();
    std::vector<QTreeWidgetItem *> items(n, nullptr);

    for (size_t i = 0; i < n; ++i) {
        const we_mdl_bone_t &b = m_rig.bones[i];
        QString bName = QString::fromUtf8(b.name).trimmed();
        if (bName.isEmpty()) {
            bName = QStringLiteral("Bone_%1").arg(i);
        }

        const we_bone_drive_mode_t mode =
            (i < WE_MDL_MAX_BONES) ? m_rig.driveModes[i] : WE_BONE_DRIVE_FK;

        bool inEnabledIk = false;
        for (const auto &c : m_rig.ikChains) {
            if (c.enabled != 0u && (c.root_bone == i || c.mid_bone == i || c.end_bone == i)) {
                inEnabledIk = true;
                break;
            }
        }
        bool hasEnabledAim = false;
        for (const auto &a : m_rig.aimConstraints) {
            if (a.enabled != 0u && a.bone_index == i) {
                hasEnabledAim = true;
                break;
            }
        }

        QString statusText = driveModeLabel(mode) + QStringLiteral(" Active");
        QColor statusColor = driveModeColor(mode);
        if (mode == WE_BONE_DRIVE_XPBD && (inEnabledIk || hasEnabledAim)) {
            statusText = QStringLiteral("XPBD Overrides IK/Aim");
            statusColor = QColor(QStringLiteral("#f87171"));
        } else if (inEnabledIk && mode != WE_BONE_DRIVE_IK) {
            statusText = QStringLiteral("IK Chain Member");
            statusColor = QColor(QStringLiteral("#38bdf8"));
        }

        auto *item = new QTreeWidgetItem();
        item->setText(0, QStringLiteral("[%1] %2").arg(i).arg(bName));
        item->setData(0, Qt::UserRole, static_cast<int>(i));
        item->setText(1, driveModeLabel(mode));
        item->setForeground(1, QBrush(driveModeColor(mode)));
        item->setText(2, statusText);
        item->setForeground(2, QBrush(statusColor));
        items[i] = item;
    }

    for (size_t i = 0; i < n; ++i) {
        const uint32_t p = m_rig.bones[i].parent;
        if (p != 0xFFFFFFFFu && p < n && p != i && items[p]) {
            items[p]->addChild(items[i]);
        } else {
            m_boneTree->addTopLevelItem(items[i]);
        }
    }
    m_boneTree->expandAll();

    if (m_selectedBoneIndex >= 0 && static_cast<size_t>(m_selectedBoneIndex) < n &&
        items[static_cast<size_t>(m_selectedBoneIndex)]) {
        m_boneTree->setCurrentItem(items[static_cast<size_t>(m_selectedBoneIndex)]);
    }
}

void BoneRiggingPanel::syncInspectorFromSelectedBone()
{
    if (!m_impl) {
        return;
    }
    const QScopedValueRollback<bool> guard(m_updatingUi, true);
    const size_t n = m_rig.bones.size();
    const bool hasSelection =
        (m_selectedBoneIndex >= 0 && static_cast<size_t>(m_selectedBoneIndex) < n);

    for (QWidget *w : std::initializer_list<QWidget *>{
             m_boneNameEdit, m_driveModeCombo, m_posXSpin, m_posYSpin, m_angleZSpin, m_scaleXSpin,
             m_scaleYSpin, m_impl->deleteBoneBtn, m_impl->makeRootBtn, m_impl->parentCombo,
             m_impl->aimEnableCheck}) {
        if (w) {
            w->setEnabled(hasSelection);
        }
    }

    if (m_addBoneBtn) {
        m_addBoneBtn->setEnabled(n < WE_MDL_MAX_BONES);
    }

    if (m_impl->parentCombo) {
        const QSignalBlocker blocker(m_impl->parentCombo);
        m_impl->parentCombo->clear();
        m_impl->parentCombo->addItem(tr("<Root>"), -1);
        for (size_t i = 0; i < n; ++i) {
            if (static_cast<int32_t>(i) == m_selectedBoneIndex) {
                continue;
            }
            QString bName = QString::fromUtf8(m_rig.bones[i].name).trimmed();
            if (bName.isEmpty()) {
                bName = QStringLiteral("Bone_%1").arg(i);
            }
            m_impl->parentCombo->addItem(QStringLiteral("[%1] %2").arg(i).arg(bName),
                                         static_cast<int>(i));
        }
    }

    auto populateBoneCombo = [&](QComboBox *combo) {
        if (!combo) {
            return;
        }
        const int prevData = combo->currentIndex() >= 0 ? combo->currentData().toInt() : -1;
        const QSignalBlocker blocker(combo);
        combo->clear();
        for (size_t i = 0; i < n; ++i) {
            QString bName = QString::fromUtf8(m_rig.bones[i].name).trimmed();
            if (bName.isEmpty()) {
                bName = QStringLiteral("Bone_%1").arg(i);
            }
            combo->addItem(QStringLiteral("[%1] %2").arg(i).arg(bName), static_cast<int>(i));
        }
        const int found = combo->findData(prevData);
        if (found >= 0) {
            combo->setCurrentIndex(found);
        }
    };
    populateBoneCombo(m_impl->ikRootCombo);
    populateBoneCombo(m_impl->ikMidCombo);
    populateBoneCombo(m_impl->ikEndCombo);
    if (m_impl->ikRootCombo && m_impl->ikMidCombo && m_impl->ikEndCombo && n >= 3u) {
        const int r = m_impl->ikRootCombo->currentData().toInt();
        const int m = m_impl->ikMidCombo->currentData().toInt();
        const int e = m_impl->ikEndCombo->currentData().toInt();
        const bool alreadyValid =
            (r >= 0 && m >= 0 && e >= 0 && r != m && m != e && r != e &&
             isBoneDescendant(m_rig, static_cast<uint16_t>(r), static_cast<uint16_t>(m)) &&
             isBoneDescendant(m_rig, static_cast<uint16_t>(m), static_cast<uint16_t>(e)));
        if (!alreadyValid) {
            int candEnd = -1;
            int candMid = -1;
            int candRoot = -1;
            auto tryEnd = [&](size_t endCand) {
                const uint32_t midU = m_rig.bones[endCand].parent;
                if (midU != 0xFFFFFFFFu && midU < n) {
                    const uint32_t rootU = m_rig.bones[midU].parent;
                    if (rootU != 0xFFFFFFFFu && rootU < n && rootU != midU && rootU != endCand) {
                        candRoot = static_cast<int>(rootU);
                        candMid = static_cast<int>(midU);
                        candEnd = static_cast<int>(endCand);
                        return true;
                    }
                }
                return false;
            };
            if (hasSelection) {
                tryEnd(static_cast<size_t>(m_selectedBoneIndex));
            }
            if (candEnd < 0) {
                for (size_t i = 0u; i < n; ++i) {
                    if (tryEnd(i)) {
                        break;
                    }
                }
            }
            if (candEnd >= 0) {
                m_impl->ikRootCombo->setCurrentIndex(m_impl->ikRootCombo->findData(candRoot));
                m_impl->ikMidCombo->setCurrentIndex(m_impl->ikMidCombo->findData(candMid));
                m_impl->ikEndCombo->setCurrentIndex(m_impl->ikEndCombo->findData(candEnd));
            }
        }
    }
    if (m_createIkBtn) {
        m_createIkBtn->setEnabled(n >= 3u && m_rig.ikChains.size() < WE_RIG_MAX_IK_CHAINS);
    }

    if (m_impl->aimTargetBoneCombo) {
        const QSignalBlocker blocker(m_impl->aimTargetBoneCombo);
        m_impl->aimTargetBoneCombo->clear();
        m_impl->aimTargetBoneCombo->addItem(tr("Custom World Target"), -1);
        for (size_t i = 0; i < n; ++i) {
            if (static_cast<int32_t>(i) == m_selectedBoneIndex) {
                continue;
            }
            QString bName = QString::fromUtf8(m_rig.bones[i].name).trimmed();
            if (bName.isEmpty()) {
                bName = QStringLiteral("Bone_%1").arg(i);
            }
            m_impl->aimTargetBoneCombo->addItem(QStringLiteral("[%1] %2").arg(i).arg(bName),
                                                static_cast<int>(i));
        }
        int savedTarget = -1;
        if (hasSelection &&
            static_cast<size_t>(m_selectedBoneIndex) < m_impl->aimTargetBoneIds.size()) {
            savedTarget = m_impl->aimTargetBoneIds[static_cast<size_t>(m_selectedBoneIndex)];
        }
        const int foundTarget = m_impl->aimTargetBoneCombo->findData(savedTarget);
        m_impl->aimTargetBoneCombo->setCurrentIndex(foundTarget >= 0 ? foundTarget : 0);
    }

    if (m_impl->ikChainCombo) {
        const int prevChain = m_impl->ikChainCombo->currentIndex() >= 0
                                  ? m_impl->ikChainCombo->currentData().toInt()
                                  : 0;
        const QSignalBlocker blocker(m_impl->ikChainCombo);
        m_impl->ikChainCombo->clear();
        for (size_t i = 0; i < m_rig.ikChains.size(); ++i) {
            const auto &c = m_rig.ikChains[i];
            m_impl->ikChainCombo->addItem(QStringLiteral("#%1 (%2 -> %3 -> %4)")
                                              .arg(i)
                                              .arg(c.root_bone)
                                              .arg(c.mid_bone)
                                              .arg(c.end_bone),
                                          static_cast<int>(i));
        }
        if (m_impl->ikChainCombo->count() == 0) {
            m_impl->ikChainCombo->addItem(tr("No IK Chains"), -1);
        } else {
            const int found = m_impl->ikChainCombo->findData(prevChain);
            m_impl->ikChainCombo->setCurrentIndex(found >= 0 ? found : 0);
        }
    }

    const bool hasIkChains = !m_rig.ikChains.empty();
    for (QWidget *w : std::initializer_list<QWidget *>{
             m_impl->ikChainCombo, m_impl->deleteIkBtn, m_impl->ikTargetXSpin,
             m_impl->ikTargetYSpin, m_impl->ikPoleXSpin, m_impl->ikPoleYSpin, m_impl->ikPoleZSpin,
             m_poleFlipCheck, m_ikSoftZoneSpin, m_ikMaxStretchSpin, m_ikMinAngleSpin,
             m_ikMaxAngleSpin, m_impl->ikMidMinAngleSpin, m_impl->ikMidMaxAngleSpin,
             m_impl->ikBlendSpin}) {
        if (w) {
            w->setEnabled(hasIkChains);
        }
    }
    if (hasIkChains && m_impl->ikChainCombo) {
        const int cIdx = std::clamp(m_impl->ikChainCombo->currentData().toInt(), 0,
                                    static_cast<int>(m_rig.ikChains.size()) - 1);
        const we_ik_chain_t &c = m_rig.ikChains[static_cast<size_t>(cIdx)];
        if (m_impl->ikTargetXSpin) {
            m_impl->ikTargetXSpin->setValue(static_cast<double>(c.target_world[0]));
        }
        if (m_impl->ikTargetYSpin) {
            m_impl->ikTargetYSpin->setValue(static_cast<double>(c.target_world[1]));
        }
        if (m_impl->ikPoleXSpin) {
            m_impl->ikPoleXSpin->setValue(static_cast<double>(c.pole_world[0]));
        }
        if (m_impl->ikPoleYSpin) {
            m_impl->ikPoleYSpin->setValue(static_cast<double>(c.pole_world[1]));
        }
        if (m_impl->ikPoleZSpin) {
            m_impl->ikPoleZSpin->setValue(static_cast<double>(c.pole_world[2]));
        }
        if (m_poleFlipCheck) {
            m_poleFlipCheck->setChecked(c.bend_sign < 0);
        }
        if (m_ikSoftZoneSpin) {
            m_ikSoftZoneSpin->setValue(static_cast<double>(c.soft_zone_ratio));
        }
        if (m_ikMaxStretchSpin) {
            m_ikMaxStretchSpin->setValue(static_cast<double>(c.max_stretch_ratio));
        }
        if (m_ikMinAngleSpin) {
            m_ikMinAngleSpin->setValue(static_cast<double>(c.root_angle_min_rad * kRadToDeg));
        }
        if (m_ikMaxAngleSpin) {
            m_ikMaxAngleSpin->setValue(static_cast<double>(c.root_angle_max_rad * kRadToDeg));
        }
        if (m_impl->ikMidMinAngleSpin) {
            m_impl->ikMidMinAngleSpin->setValue(
                static_cast<double>(c.mid_angle_min_rad * kRadToDeg));
        }
        if (m_impl->ikMidMaxAngleSpin) {
            m_impl->ikMidMaxAngleSpin->setValue(
                static_cast<double>(c.mid_angle_max_rad * kRadToDeg));
        }
        if (m_impl->ikBlendSpin) {
            m_impl->ikBlendSpin->setValue(static_cast<double>(c.blend_weight));
        }
    }

    if (!hasSelection) {
        if (m_boneNameEdit) {
            m_boneNameEdit->clear();
        }
        if (m_impl->aimEnableCheck) {
            m_impl->aimEnableCheck->setChecked(false);
        }
        if (m_conflictBadgeLabel) {
            m_conflictBadgeLabel->setText(tr("No Selection"));
            m_conflictBadgeLabel->setStyleSheet(QStringLiteral(
                "QLabel { background-color: #181b22; color: #94a3b8; border: 1px solid #2a2f3d; "
                "border-radius: 4px; padding: 3px 6px; font-weight: 600; }"));
        }
        for (QWidget *w : std::initializer_list<QWidget *>{
                 m_impl->aimTargetBoneCombo, m_impl->aimAxisCombo, m_impl->aimTargetXSpin,
                 m_impl->aimTargetYSpin, m_impl->aimConeDegSpin, m_impl->aimBlendSpin}) {
            if (w) {
                w->setEnabled(false);
            }
        }
        return;
    }

    const size_t sel = static_cast<size_t>(m_selectedBoneIndex);
    const we_mdl_bone_t &bone = m_rig.bones[sel];

    if (m_boneNameEdit) {
        m_boneNameEdit->setText(QString::fromUtf8(bone.name));
    }
    if (m_impl->parentCombo) {
        const int pVal = (bone.parent == 0xFFFFFFFFu) ? -1 : static_cast<int>(bone.parent);
        const int idx = m_impl->parentCombo->findData(pVal);
        m_impl->parentCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    if (m_impl->makeRootBtn) {
        m_impl->makeRootBtn->setEnabled(bone.parent != 0xFFFFFFFFu);
    }

    const we_bone_drive_mode_t mode =
        (sel < WE_MDL_MAX_BONES) ? m_rig.driveModes[sel] : WE_BONE_DRIVE_FK;
    if (m_driveModeCombo) {
        const int idx = m_driveModeCombo->findData(static_cast<uint32_t>(mode));
        m_driveModeCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    }

    if (m_posXSpin) {
        m_posXSpin->setValue(static_cast<double>(bone.bind_pos[0]));
    }
    if (m_posYSpin) {
        m_posYSpin->setValue(static_cast<double>(bone.bind_pos[1]));
    }
    if (m_angleZSpin) {
        m_angleZSpin->setValue(static_cast<double>(bone.bind_angle[2] * kRadToDeg));
    }
    if (m_scaleXSpin) {
        m_scaleXSpin->setValue(static_cast<double>(bone.bind_scale[0]));
    }
    if (m_scaleYSpin) {
        m_scaleYSpin->setValue(static_cast<double>(bone.bind_scale[1]));
    }

    if (m_conflictBadgeLabel) {
        bool inIk = false;
        for (const auto &c : m_rig.ikChains) {
            if (c.enabled != 0u && (c.root_bone == sel || c.mid_bone == sel || c.end_bone == sel)) {
                inIk = true;
                break;
            }
        }
        QString badgeText = driveModeLabel(mode) + QStringLiteral(" Active");
        QString bg = QStringLiteral("#1e293b");
        QString fg = QStringLiteral("#94a3b8");
        if (mode == WE_BONE_DRIVE_IK) {
            bg = QStringLiteral("#0c2d48");
            fg = QStringLiteral("#38bdf8");
        } else if (mode == WE_BONE_DRIVE_AIM) {
            bg = QStringLiteral("#3b2506");
            fg = QStringLiteral("#fbbf24");
        } else if (mode == WE_BONE_DRIVE_XPBD) {
            if (inIk) {
                badgeText = QStringLiteral("XPBD Overrides IK");
                bg = QStringLiteral("#3f1219");
                fg = QStringLiteral("#f87171");
            } else {
                bg = QStringLiteral("#0a3622");
                fg = QStringLiteral("#4ade80");
            }
        }
        m_conflictBadgeLabel->setText(badgeText);
        m_conflictBadgeLabel->setStyleSheet(
            QStringLiteral("QLabel { background-color: %1; color: %2; border: 1px solid #2a2f3d; "
                           "border-radius: 4px; padding: 3px 6px; font-weight: 600; }")
                .arg(bg, fg));
    }

    auto aimIt = std::find_if(m_rig.aimConstraints.begin(), m_rig.aimConstraints.end(),
                              [sel](const we_aim_constraint_t &a) { return a.bone_index == sel; });
    const bool aimActive = (aimIt != m_rig.aimConstraints.end() && aimIt->enabled != 0u);
    if (m_impl->aimEnableCheck) {
        m_impl->aimEnableCheck->setChecked(aimActive);
    }
    for (QWidget *w : std::initializer_list<QWidget *>{
             m_impl->aimTargetBoneCombo, m_impl->aimAxisCombo, m_impl->aimTargetXSpin,
             m_impl->aimTargetYSpin, m_impl->aimConeDegSpin, m_impl->aimBlendSpin}) {
        if (w) {
            w->setEnabled(aimActive);
        }
    }
    if (aimIt != m_rig.aimConstraints.end()) {
        if (m_impl->aimAxisCombo) {
            int axisId = 0;
            if (aimIt->aim_axis_local[1] > 0.5f) {
                axisId = 1;
            } else if (aimIt->aim_axis_local[0] < -0.5f) {
                axisId = 2;
            } else if (aimIt->aim_axis_local[1] < -0.5f) {
                axisId = 3;
            }
            const int found = m_impl->aimAxisCombo->findData(axisId);
            m_impl->aimAxisCombo->setCurrentIndex(found >= 0 ? found : 0);
        }
        if (m_impl->aimTargetXSpin) {
            m_impl->aimTargetXSpin->setValue(static_cast<double>(aimIt->target_world[0]));
        }
        if (m_impl->aimTargetYSpin) {
            m_impl->aimTargetYSpin->setValue(static_cast<double>(aimIt->target_world[1]));
        }
        if (m_impl->aimConeDegSpin) {
            m_impl->aimConeDegSpin->setValue(static_cast<double>(aimIt->max_cone_rad * kRadToDeg));
        }
        if (m_impl->aimBlendSpin) {
            m_impl->aimBlendSpin->setValue(static_cast<double>(aimIt->blend_weight));
        }
    }
}

void BoneRiggingPanel::pushSkeletonToDaemon()
{
    if (!m_ipcClient || m_rig.objectId < 0 || !m_impl) {
        return;
    }

    std::vector<waywal_ipc_mdl_bone_t> wireBones;
    wireBones.reserve(m_rig.bones.size());

    bool reinitPhysics = m_impl->forceReinitPhysics;
    for (size_t i = 0; i < m_rig.bones.size(); ++i) {
        const we_mdl_bone_t &src = m_rig.bones[i];
        waywal_ipc_mdl_bone_t dst{};
        std::memcpy(dst.name, src.name, sizeof(dst.name));
        dst.name[sizeof(dst.name) - 1u] = '\0';
        dst.unk = src.unk;
        dst.parent = (src.parent == 0xFFFFFFFFu || src.parent < i) ? src.parent : 0xFFFFFFFFu;
        std::memcpy(dst.local_trans, src.local_trans, sizeof(dst.local_trans));
        const bool isXpbd = (i < WE_MDL_MAX_BONES && m_rig.driveModes[i] == WE_BONE_DRIVE_XPBD);
        if (isXpbd) {
            std::memcpy(dst.simulation_json, src.simulation_json, sizeof(dst.simulation_json));
            dst.simulation_json[sizeof(dst.simulation_json) - 1u] = '\0';
            reinitPhysics = true;
        } else if (i < WE_MDL_MAX_BONES && m_impl->lastPushedXpbd[i]) {
            reinitPhysics = true;
        }
        wireBones.push_back(dst);
    }

    for (size_t i = m_rig.bones.size(); i < WE_MDL_MAX_BONES; ++i) {
        if (m_impl->lastPushedXpbd[i]) {
            reinitPhysics = true;
        }
        m_impl->lastPushedXpbd[i] = false;
    }
    for (size_t i = 0; i < std::min<size_t>(m_rig.bones.size(), WE_MDL_MAX_BONES); ++i) {
        m_impl->lastPushedXpbd[i] = (m_rig.driveModes[i] == WE_BONE_DRIVE_XPBD);
    }
    m_impl->forceReinitPhysics = false;

    waywal_ipc_puppet_set_skeleton_t req{};
    req.object_id = static_cast<uint32_t>(m_rig.objectId);
    req.bone_count = static_cast<uint32_t>(wireBones.size());
    req.flags = reinitPhysics ? 1u : 0u;
    m_ipcClient->sendPuppetSetSkeleton(req, wireBones);
}

} // namespace waywal::studio
