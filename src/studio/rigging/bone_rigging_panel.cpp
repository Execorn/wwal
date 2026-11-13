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
#include <QPointer>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <numeric>
#include <vector>

namespace waywal::studio {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kRadToDeg = 180.0f / kPi;
constexpr float kDegToRad = kPi / 180.0f;

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

/* Reorders bones topologically (parent < child) only when an invariant violation exists,
 * so MDL export and daemon IPC always receive valid parent-before-child ordering. */
void ensureTopologicalOrder(BoneHierarchySnapshot &rig, int32_t &trackedBoneIndex)
{
    const size_t n = rig.bones.size();
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

bool boneHasActiveSimulationJson(const char *json) noexcept
{
    if (!json || json[0] == '\0') {
        return false;
    }
    we_bone_sim_config_t tmp{};
    return we_physics_parse_simulation_json(json, &tmp) &&
           ((tmp.flags & WE_BONE_PHYS_ENABLED) != 0u);
}

void writeDefaultXpbdSimulationJson(we_mdl_bone_t &bone) noexcept
{
    static constexpr const char kDefaultSimJson[] =
        "{\"stiffness\":0.55,\"damping\":0.15,\"gravity\":1.0,\"mass\":0.5}";
    std::memset(bone.simulation_json, 0, sizeof(bone.simulation_json));
    std::memcpy(bone.simulation_json, kDefaultSimJson, sizeof(kDefaultSimJson));
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

template <typename Client = IpcClient>
void dispatchSkeletonIpc(Client *client, int32_t objectId,
                         const std::vector<waywal_ipc_mdl_bone_t> &bones, uint32_t flags)
{
    if (!client || objectId < 0) {
        return;
    }

    waywal_ipc_puppet_set_skeleton_t req{};
    req.object_id = static_cast<uint32_t>(objectId);
    req.bone_count = static_cast<uint32_t>(bones.size());
    req.flags = flags;

    if constexpr (requires(Client *c) { c->sendPuppetSetSkeleton(req, bones); }) {
        client->sendPuppetSetSkeleton(req, bones);
    } else if constexpr (requires(Client *c) {
                             c->sendPuppetSetSkeleton(static_cast<uint32_t>(objectId), bones,
                                                      flags);
                         }) {
        client->sendPuppetSetSkeleton(static_cast<uint32_t>(objectId), bones, flags);
    } else if constexpr (requires(Client *c) {
                             c->sendPuppetSetSkeleton(objectId, bones, flags);
                         }) {
        client->sendPuppetSetSkeleton(objectId, bones, flags);
    } else {
        (void)req;
    }
}

template <typename Client = IpcClient>
void dispatchIkPreviewIpc(Client *client, int32_t objectId, const we_ik_chain_t &chain)
{
    if (!client || objectId < 0) {
        return;
    }

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
    if constexpr (requires(Client *c) { c->sendPuppetPreviewIk(req, emptyOverrides); }) {
        client->sendPuppetPreviewIk(req, emptyOverrides);
    } else if constexpr (requires(Client *c) { c->sendPuppetPreviewIk(req); }) {
        client->sendPuppetPreviewIk(req);
    } else if constexpr (requires(Client *c) {
                             c->sendPuppetPreviewIk(objectId, chain, emptyOverrides);
                         }) {
        client->sendPuppetPreviewIk(objectId, chain, emptyOverrides);
    } else {
        (void)req;
    }
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

class BoneRigAuxState final : public QObject {
  public:
    explicit BoneRigAuxState(QObject *parent = nullptr) : QObject(parent)
    {
        setObjectName(QStringLiteral("BoneRigAuxState"));
        lastPushedXpbd.fill(false);
    }

    std::array<bool, WE_MDL_MAX_BONES> lastPushedXpbd{};
    bool forceReinitPhysics{false};
};

[[nodiscard]] BoneRigAuxState *ensureBoneAux(BoneRiggingPanel *panel)
{
    if (!panel) {
        return nullptr;
    }
    if (auto *existing = panel->findChild<QObject *>(QStringLiteral("BoneRigAuxState"),
                                                     Qt::FindDirectChildrenOnly)) {
        return static_cast<BoneRigAuxState *>(existing);
    }
    return new BoneRigAuxState(panel);
}

void sanitizeAndTrackXpbdTransition(const BoneHierarchySnapshot &prevRig,
                                    BoneHierarchySnapshot &nextRig, BoneRigAuxState *aux)
{
    const size_t nextLimit = std::min<size_t>(nextRig.bones.size(), WE_MDL_MAX_BONES);
    for (size_t i = 0u; i < nextLimit; ++i) {
        if (nextRig.driveModes[i] != WE_BONE_DRIVE_XPBD) {
            std::memset(nextRig.bones[i].simulation_json, 0,
                        sizeof(nextRig.bones[i].simulation_json));
            const bool wasPrevXpbd =
                (i < prevRig.bones.size() && prevRig.driveModes[i] == WE_BONE_DRIVE_XPBD);
            if (aux && (wasPrevXpbd || aux->lastPushedXpbd[i])) {
                aux->forceReinitPhysics = true;
            }
        }
    }
    if (aux) {
        for (size_t i = nextLimit; i < WE_MDL_MAX_BONES; ++i) {
            if (aux->lastPushedXpbd[i]) {
                aux->forceReinitPhysics = true;
            }
        }
    }
}

} // namespace

BoneRiggingPanel::BoneRiggingPanel(QWidget *parent) : QWidget(parent)
{
    m_rig.driveModes.fill(WE_BONE_DRIVE_FK);
    setupUi();
    syncInspectorFromSelectedBone();
}

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

    // -------------------------------------------------------------------------
    // Top: Topological Bone Hierarchy Tree
    // -------------------------------------------------------------------------
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

    auto *deleteBoneBtn = new QPushButton(tr("Delete Bone"), treeGroup);
    deleteBoneBtn->setObjectName(QStringLiteral("deleteBoneBtn"));
    deleteBoneBtn->setAccessibleName(tr("Delete Selected Bone"));
    deleteBoneBtn->setToolTip(tr("Remove the selected bone and reparent its children safely"));
    connect(deleteBoneBtn, &QPushButton::clicked, this, &BoneRiggingPanel::onDeleteBoneClicked);

    auto *makeRootBtn = new QPushButton(tr("Unparent"), treeGroup);
    makeRootBtn->setObjectName(QStringLiteral("makeRootBtn"));
    makeRootBtn->setAccessibleName(tr("Make Selected Bone Root"));
    makeRootBtn->setToolTip(tr("Detach the selected bone from its parent to become a root bone"));
    connect(makeRootBtn, &QPushButton::clicked, this, [this]() {
        if (m_selectedBoneIndex >= 0) {
            reparentBone(m_selectedBoneIndex, -1);
        }
    });

    treeBtnRow->addWidget(m_addBoneBtn);
    treeBtnRow->addWidget(deleteBoneBtn);
    treeBtnRow->addWidget(makeRootBtn);
    treeLayout->addLayout(treeBtnRow);
    splitter->addWidget(treeGroup);

    // -------------------------------------------------------------------------
    // Bottom: Scrollable Bind TRS, Drive Mode, 2-Bone IK, and Look-At Aim
    // -------------------------------------------------------------------------
    auto *scrollArea = new QScrollArea(splitter);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto *scrollContent = new QWidget(scrollArea);
    auto *contentLayout = new QVBoxLayout(scrollContent);
    contentLayout->setContentsMargins(2, 2, 4, 2);
    contentLayout->setSpacing(8);

    // --- Section 1: Bone Identity, Parent, Drive Mode & Bind TRS ---
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

    auto *parentCombo = new QComboBox(boneGroup);
    parentCombo->setObjectName(QStringLiteral("parentCombo"));
    parentCombo->setAccessibleName(tr("Parent Bone Selector"));
    parentCombo->setToolTip(tr("Reparent the selected bone (cycles are rejected)"));
    connect(parentCombo, qOverload<int>(&QComboBox::activated), this, [this, parentCombo](int idx) {
        if (m_updatingUi || m_selectedBoneIndex < 0 || idx < 0) {
            return;
        }
        const int32_t newParent = parentCombo->itemData(idx).toInt();
        if (!reparentBone(m_selectedBoneIndex, newParent)) {
            syncInspectorFromSelectedBone();
        }
    });
    boneGrid->addWidget(new QLabel(tr("Parent:"), boneGroup), 1, 0);
    boneGrid->addWidget(parentCombo, 1, 1, 1, 3);

    m_driveModeCombo = new QComboBox(boneGroup);
    m_driveModeCombo->addItem(tr("FK (Keyframe)"), static_cast<uint32_t>(WE_BONE_DRIVE_FK));
    m_driveModeCombo->addItem(tr("2-Bone IK"), static_cast<uint32_t>(WE_BONE_DRIVE_IK));
    m_driveModeCombo->addItem(tr("Look-At Aim"), static_cast<uint32_t>(WE_BONE_DRIVE_AIM));
    m_driveModeCombo->addItem(tr("XPBD Spring"), static_cast<uint32_t>(WE_BONE_DRIVE_XPBD));
    m_driveModeCombo->setAccessibleName(tr("Bone Drive Mode"));
    m_driveModeCombo->setToolTip(
        tr("Selects the active solver driving this bone; XPBD links to Physics Spring Inspector"));
    connect(m_driveModeCombo, qOverload<int>(&QComboBox::activated), this,
            &BoneRiggingPanel::onTransformOrConstraintEdited);
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

    // --- Section 2: 2-Bone Analytic IK Chain Controls ---
    auto *ikGroup = new QGroupBox(tr("2-Bone Analytic IK Chain"), scrollContent);
    ikGroup->setObjectName(QStringLiteral("rigSectionGroup"));
    auto *ikGrid = new QGridLayout(ikGroup);
    ikGrid->setContentsMargins(8, 8, 8, 8);
    ikGrid->setSpacing(6);

    auto *ikChainCombo = new QComboBox(ikGroup);
    ikChainCombo->setObjectName(QStringLiteral("ikChainCombo"));
    ikChainCombo->setAccessibleName(tr("Active IK Chain Selector"));
    connect(ikChainCombo, qOverload<int>(&QComboBox::activated), this,
            [this](int) { syncInspectorFromSelectedBone(); });
    ikGrid->addWidget(new QLabel(tr("Chain:"), ikGroup), 0, 0);
    ikGrid->addWidget(ikChainCombo, 0, 1, 1, 2);

    auto *deleteIkBtn = new QPushButton(tr("Remove"), ikGroup);
    deleteIkBtn->setObjectName(QStringLiteral("deleteIkBtn"));
    deleteIkBtn->setAccessibleName(tr("Remove Selected IK Chain"));
    connect(deleteIkBtn, &QPushButton::clicked, this, [this, ikChainCombo]() {
        const int idx = ikChainCombo->currentData().toInt();
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
        const int32_t targetObjectId = m_rig.objectId;
        auto applyFn = [self, targetObjectId](const BoneHierarchySnapshot &snap) {
            if (!self || self->m_rig.objectId != targetObjectId) {
                return;
            }
            BoneHierarchySnapshot nextRig = snap;
            sanitizeAndTrackXpbdTransition(self->m_rig, nextRig, ensureBoneAux(self.data()));
            self->m_rig = std::move(nextRig);
            self->rebuildBoneTree();
            self->syncInspectorFromSelectedBone();
            self->pushSkeletonToDaemon();
            emit self->skeletonUpdated(self->m_rig.objectId, self->m_rig);
        };
        if (m_commandBus) {
            m_commandBus->execute(std::make_unique<BoneEditCommand>(
                tr("Remove 2-Bone IK Chain"), std::move(oldRig), std::move(newRig), applyFn));
        } else {
            applyFn(newRig);
        }
    });
    ikGrid->addWidget(deleteIkBtn, 0, 3);

    auto *ikRootCombo = new QComboBox(ikGroup);
    ikRootCombo->setObjectName(QStringLiteral("ikRootCombo"));
    ikRootCombo->setAccessibleName(tr("IK Root Bone"));
    auto *ikMidCombo = new QComboBox(ikGroup);
    ikMidCombo->setObjectName(QStringLiteral("ikMidCombo"));
    ikMidCombo->setAccessibleName(tr("IK Mid Bone"));
    auto *ikEndCombo = new QComboBox(ikGroup);
    ikEndCombo->setObjectName(QStringLiteral("ikEndCombo"));
    ikEndCombo->setAccessibleName(tr("IK End Bone"));

    ikGrid->addWidget(new QLabel(tr("Root:"), ikGroup), 1, 0);
    ikGrid->addWidget(ikRootCombo, 1, 1);
    ikGrid->addWidget(new QLabel(tr("Mid:"), ikGroup), 1, 2);
    ikGrid->addWidget(ikMidCombo, 1, 3);
    ikGrid->addWidget(new QLabel(tr("End:"), ikGroup), 2, 0);
    ikGrid->addWidget(ikEndCombo, 2, 1);

    m_createIkBtn = new QPushButton(tr("Create IK Chain"), ikGroup);
    m_createIkBtn->setAccessibleName(tr("Create 2-Bone IK Chain"));
    m_createIkBtn->setToolTip(tr("Bind a 2-bone analytic IK solver to Root -> Mid -> End"));
    connect(m_createIkBtn, &QPushButton::clicked, this, &BoneRiggingPanel::onCreateIkChainClicked);
    ikGrid->addWidget(m_createIkBtn, 2, 2, 1, 2);

    auto *ikTargetXSpin =
        makeRigSpin(-16384.0, 16384.0, 1.0, 2, QStringLiteral(" px"), tr("IK Target X"), ikGroup);
    ikTargetXSpin->setObjectName(QStringLiteral("ikTargetXSpin"));
    auto *ikTargetYSpin =
        makeRigSpin(-16384.0, 16384.0, 1.0, 2, QStringLiteral(" px"), tr("IK Target Y"), ikGroup);
    ikTargetYSpin->setObjectName(QStringLiteral("ikTargetYSpin"));
    ikGrid->addWidget(new QLabel(tr("Target X:"), ikGroup), 3, 0);
    ikGrid->addWidget(ikTargetXSpin, 3, 1);
    ikGrid->addWidget(new QLabel(tr("Target Y:"), ikGroup), 3, 2);
    ikGrid->addWidget(ikTargetYSpin, 3, 3);

    auto *ikPoleXSpin = makeRigSpin(-16384.0, 16384.0, 1.0, 2, QStringLiteral(" px"),
                                    tr("IK Pole Vector X"), ikGroup);
    ikPoleXSpin->setObjectName(QStringLiteral("ikPoleXSpin"));
    auto *ikPoleYSpin = makeRigSpin(-16384.0, 16384.0, 1.0, 2, QStringLiteral(" px"),
                                    tr("IK Pole Vector Y"), ikGroup);
    ikPoleYSpin->setObjectName(QStringLiteral("ikPoleYSpin"));
    ikGrid->addWidget(new QLabel(tr("Pole X:"), ikGroup), 4, 0);
    ikGrid->addWidget(ikPoleXSpin, 4, 1);
    ikGrid->addWidget(new QLabel(tr("Pole Y:"), ikGroup), 4, 2);
    ikGrid->addWidget(ikPoleYSpin, 4, 3);

    m_poleFlipCheck = new QCheckBox(tr("Flip Pole Bend Direction (-1 / +1)"), ikGroup);
    m_poleFlipCheck->setAccessibleName(tr("Flip IK Pole Vector Sign"));
    connect(m_poleFlipCheck, &QCheckBox::toggled, this, &BoneRiggingPanel::onTogglePoleFlipClicked);
    ikGrid->addWidget(m_poleFlipCheck, 5, 0, 1, 4);

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
                                   tr("IK Minimum Joint Limit"), ikGroup);
    m_ikMinAngleSpin->setValue(-180.0);
    m_ikMaxAngleSpin = makeRigSpin(0.0, 180.0, 5.0, 1, QStringLiteral(" deg"),
                                   tr("IK Maximum Joint Limit"), ikGroup);
    m_ikMaxAngleSpin->setValue(180.0);
    ikGrid->addWidget(new QLabel(tr("Min Limit:"), ikGroup), 7, 0);
    ikGrid->addWidget(m_ikMinAngleSpin, 7, 1);
    ikGrid->addWidget(new QLabel(tr("Max Limit:"), ikGroup), 7, 2);
    ikGrid->addWidget(m_ikMaxAngleSpin, 7, 3);

    auto *ikBlendSpin =
        makeRigSpin(0.0, 1.0, 0.05, 2, QString(), tr("IK Constraint Blend Weight"), ikGroup);
    ikBlendSpin->setObjectName(QStringLiteral("ikBlendSpin"));
    ikBlendSpin->setValue(1.0);
    ikGrid->addWidget(new QLabel(tr("IK Blend:"), ikGroup), 8, 0);
    ikGrid->addWidget(ikBlendSpin, 8, 1, 1, 3);

    for (QDoubleSpinBox *s :
         {ikTargetXSpin, ikTargetYSpin, ikPoleXSpin, ikPoleYSpin, m_ikSoftZoneSpin,
          m_ikMaxStretchSpin, m_ikMinAngleSpin, m_ikMaxAngleSpin, ikBlendSpin}) {
        connect(s, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                &BoneRiggingPanel::onTransformOrConstraintEdited);
    }

    contentLayout->addWidget(ikGroup);

    // --- Section 3: Single-Bone Look-At / Aim Constraint ---
    auto *aimGroup = new QGroupBox(tr("Single-Bone Look-At / Aim Constraint"), scrollContent);
    aimGroup->setObjectName(QStringLiteral("rigSectionGroup"));
    auto *aimGrid = new QGridLayout(aimGroup);
    aimGrid->setContentsMargins(8, 8, 8, 8);
    aimGrid->setSpacing(6);

    auto *aimEnableCheck = new QCheckBox(tr("Enable Look-At Aim for Selected Bone"), aimGroup);
    aimEnableCheck->setObjectName(QStringLiteral("aimEnableCheck"));
    aimEnableCheck->setAccessibleName(tr("Enable Look-At Aim Constraint"));
    connect(aimEnableCheck, &QCheckBox::toggled, this,
            &BoneRiggingPanel::onTransformOrConstraintEdited);
    aimGrid->addWidget(aimEnableCheck, 0, 0, 1, 4);

    auto *aimTargetBoneCombo = new QComboBox(aimGroup);
    aimTargetBoneCombo->setObjectName(QStringLiteral("aimTargetBoneCombo"));
    aimTargetBoneCombo->setAccessibleName(tr("Aim Target Bone Selector"));
    aimTargetBoneCombo->setToolTip(
        tr("Select a target bone to track, or Custom World Target for manual coordinates"));
    connect(aimTargetBoneCombo, qOverload<int>(&QComboBox::activated), this,
            [this, aimTargetBoneCombo](int idx) {
                if (m_updatingUi || idx < 0) {
                    return;
                }
                const int targetBone = aimTargetBoneCombo->itemData(idx).toInt();
                if (targetBone >= 0 && static_cast<size_t>(targetBone) < m_rig.bones.size()) {
                    const we_mdl_bone_t &tb = m_rig.bones[static_cast<size_t>(targetBone)];
                    if (auto *tx = findChild<QDoubleSpinBox *>(QStringLiteral("aimTargetXSpin"))) {
                        const QSignalBlocker b(tx);
                        tx->setValue(static_cast<double>(tb.world_bind[12]));
                    }
                    if (auto *ty = findChild<QDoubleSpinBox *>(QStringLiteral("aimTargetYSpin"))) {
                        const QSignalBlocker b(ty);
                        ty->setValue(static_cast<double>(tb.world_bind[13]));
                    }
                }
                onTransformOrConstraintEdited();
            });
    aimGrid->addWidget(new QLabel(tr("Target:"), aimGroup), 1, 0);
    aimGrid->addWidget(aimTargetBoneCombo, 1, 1, 1, 3);

    auto *aimAxisCombo = new QComboBox(aimGroup);
    aimAxisCombo->setObjectName(QStringLiteral("aimAxisCombo"));
    aimAxisCombo->addItem(tr("+X Axis (1, 0)"), 0);
    aimAxisCombo->addItem(tr("+Y Axis (0, 1)"), 1);
    aimAxisCombo->addItem(tr("-X Axis (-1, 0)"), 2);
    aimAxisCombo->addItem(tr("-Y Axis (0, -1)"), 3);
    aimAxisCombo->setAccessibleName(tr("Local Aim Axis"));
    connect(aimAxisCombo, qOverload<int>(&QComboBox::activated), this,
            &BoneRiggingPanel::onTransformOrConstraintEdited);
    aimGrid->addWidget(new QLabel(tr("Aim Axis:"), aimGroup), 2, 0);
    aimGrid->addWidget(aimAxisCombo, 2, 1, 1, 3);

    auto *aimTargetXSpin = makeRigSpin(-16384.0, 16384.0, 1.0, 2, QStringLiteral(" px"),
                                       tr("Aim Target World X"), aimGroup);
    aimTargetXSpin->setObjectName(QStringLiteral("aimTargetXSpin"));
    auto *aimTargetYSpin = makeRigSpin(-16384.0, 16384.0, 1.0, 2, QStringLiteral(" px"),
                                       tr("Aim Target World Y"), aimGroup);
    aimTargetYSpin->setObjectName(QStringLiteral("aimTargetYSpin"));
    aimGrid->addWidget(new QLabel(tr("World X:"), aimGroup), 3, 0);
    aimGrid->addWidget(aimTargetXSpin, 3, 1);
    aimGrid->addWidget(new QLabel(tr("World Y:"), aimGroup), 3, 2);
    aimGrid->addWidget(aimTargetYSpin, 3, 3);

    auto *aimConeDegSpin = makeRigSpin(0.0, 180.0, 5.0, 1, QStringLiteral(" deg"),
                                       tr("Aim Maximum Cone Angle"), aimGroup);
    aimConeDegSpin->setObjectName(QStringLiteral("aimConeDegSpin"));
    aimConeDegSpin->setValue(90.0);
    auto *aimBlendSpin =
        makeRigSpin(0.0, 1.0, 0.05, 2, QString(), tr("Aim Blend Weight"), aimGroup);
    aimBlendSpin->setObjectName(QStringLiteral("aimBlendSpin"));
    aimBlendSpin->setValue(1.0);
    aimGrid->addWidget(new QLabel(tr("Max Cone:"), aimGroup), 4, 0);
    aimGrid->addWidget(aimConeDegSpin, 4, 1);
    aimGrid->addWidget(new QLabel(tr("Blend:"), aimGroup), 4, 2);
    aimGrid->addWidget(aimBlendSpin, 4, 3);

    for (QDoubleSpinBox *s : {aimTargetXSpin, aimTargetYSpin, aimConeDegSpin, aimBlendSpin}) {
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
    setTabOrder(m_addBoneBtn, deleteBoneBtn);
    setTabOrder(deleteBoneBtn, makeRootBtn);
    setTabOrder(makeRootBtn, m_boneNameEdit);
    setTabOrder(m_boneNameEdit, parentCombo);
    setTabOrder(parentCombo, m_driveModeCombo);
    setTabOrder(m_driveModeCombo, m_posXSpin);
    setTabOrder(m_posXSpin, m_posYSpin);
    setTabOrder(m_posYSpin, m_angleZSpin);
    setTabOrder(m_angleZSpin, m_scaleXSpin);
    setTabOrder(m_scaleXSpin, m_scaleYSpin);
    setTabOrder(m_scaleYSpin, m_createIkBtn);
}

void BoneRiggingPanel::loadPuppetRig(const we_puppet_t *puppet, int32_t sceneObjectId)
{
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
        ensureTopologicalOrder(m_rig, m_selectedBoneIndex);
    } else {
        m_selectedBoneIndex = -1;
    }

    if (auto *aux = ensureBoneAux(this)) {
        aux->forceReinitPhysics = false;
        for (size_t i = 0u; i < WE_MDL_MAX_BONES; ++i) {
            aux->lastPushedXpbd[i] =
                (i < m_rig.bones.size() && m_rig.driveModes[i] == WE_BONE_DRIVE_XPBD);
        }
    }

    rebuildBoneTree();
    syncInspectorFromSelectedBone();
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

    const we_bone_drive_mode_t prevMode = m_rig.driveModes[idx];
    m_rig.driveModes[idx] = mode;

    we_mdl_bone_t &bone = m_rig.bones[idx];
    if (mode == WE_BONE_DRIVE_XPBD) {
        if (!boneHasActiveSimulationJson(bone.simulation_json)) {
            writeDefaultXpbdSimulationJson(bone);
        }
        for (auto &c : m_rig.ikChains) {
            if (c.root_bone == idx || c.mid_bone == idx || c.end_bone == idx) {
                c.enabled = 0u;
            }
        }
        for (auto &a : m_rig.aimConstraints) {
            if (a.bone_index == idx) {
                a.enabled = 0u;
            }
        }
    } else {
        std::memset(bone.simulation_json, 0, sizeof(bone.simulation_json));
        if (prevMode == WE_BONE_DRIVE_XPBD) {
            if (auto *aux = ensureBoneAux(this)) {
                aux->forceReinitPhysics = true;
            }
        }
    }

    rebuildBoneTree();
    syncInspectorFromSelectedBone();
    pushSkeletonToDaemon();
    emit skeletonUpdated(m_rig.objectId, m_rig);
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

    int32_t cur = parentIdx;
    for (int32_t step = 0; step < n; ++step) {
        if (cur == childIdx) {
            return true;
        }
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

    QPointer<BoneRiggingPanel> self(this);
    const int32_t targetObjectId = m_rig.objectId;
    auto applyFn = [self, targetObjectId, newBoneIdx,
                    prevSelected](const BoneHierarchySnapshot &snap) {
        if (!self || self->m_rig.objectId != targetObjectId) {
            return;
        }
        BoneHierarchySnapshot nextRig = snap;
        sanitizeAndTrackXpbdTransition(self->m_rig, nextRig, ensureBoneAux(self.data()));
        self->m_rig = std::move(nextRig);
        if (static_cast<size_t>(newBoneIdx) < self->m_rig.bones.size()) {
            self->m_selectedBoneIndex = newBoneIdx;
        } else {
            self->m_selectedBoneIndex =
                (prevSelected >= 0 && static_cast<size_t>(prevSelected) < self->m_rig.bones.size())
                    ? prevSelected
                    : (self->m_rig.bones.empty() ? -1 : 0);
        }
        self->rebuildBoneTree();
        self->syncInspectorFromSelectedBone();
        self->pushSkeletonToDaemon();
        emit self->boneSelected(self->m_selectedBoneIndex);
        emit self->skeletonUpdated(self->m_rig.objectId, self->m_rig);
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

    we_mdl_bone_t &childBone = newRig.bones[static_cast<size_t>(childIndex)];
    float desiredWorld[16];
    std::memcpy(desiredWorld, childBone.world_bind, sizeof(desiredWorld));
    childBone.parent = targetParent;

    if (targetParent == 0xFFFFFFFFu) {
        mat4DecomposeTrs(desiredWorld, childBone.bind_pos, childBone.bind_angle,
                         childBone.bind_scale);
    } else {
        float parentInv[16];
        if (we_mat4_inverse(newRig.bones[targetParent].world_bind, parentInv)) {
            float newLocal[16];
            we_mat4_mul(parentInv, desiredWorld, newLocal);
            mat4DecomposeTrs(newLocal, childBone.bind_pos, childBone.bind_angle,
                             childBone.bind_scale);
        }
    }

    int32_t trackedIdx = childIndex;
    ensureTopologicalOrder(newRig, trackedIdx);

    QPointer<BoneRiggingPanel> self(this);
    const int32_t targetObjectId = m_rig.objectId;
    auto applyFn = [self, targetObjectId, trackedIdx](const BoneHierarchySnapshot &snap) {
        if (!self || self->m_rig.objectId != targetObjectId) {
            return;
        }
        BoneHierarchySnapshot nextRig = snap;
        sanitizeAndTrackXpbdTransition(self->m_rig, nextRig, ensureBoneAux(self.data()));
        self->m_rig = std::move(nextRig);
        if (self->m_rig.bones.empty()) {
            self->m_selectedBoneIndex = -1;
        } else {
            self->m_selectedBoneIndex =
                std::clamp(trackedIdx, 0, static_cast<int32_t>(self->m_rig.bones.size()) - 1);
        }
        self->rebuildBoneTree();
        self->syncInspectorFromSelectedBone();
        self->pushSkeletonToDaemon();
        emit self->boneSelected(self->m_selectedBoneIndex);
        emit self->skeletonUpdated(self->m_rig.objectId, self->m_rig);
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

bool BoneRiggingPanel::createTwoBoneIkChain(uint16_t rootBone, uint16_t midBone, uint16_t endBone)
{
    const size_t n = m_rig.bones.size();
    if (rootBone >= n || midBone >= n || endBone >= n) {
        return false;
    }
    if (rootBone == midBone || midBone == endBone || rootBone == endBone) {
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
    chain.mid_angle_min_rad = chain.root_angle_min_rad;
    chain.mid_angle_max_rad = chain.root_angle_max_rad;
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
    const int32_t targetObjectId = m_rig.objectId;
    auto applyFn = [self, targetObjectId, chain](const BoneHierarchySnapshot &snap) {
        if (!self || self->m_rig.objectId != targetObjectId) {
            return;
        }
        BoneHierarchySnapshot nextRig = snap;
        sanitizeAndTrackXpbdTransition(self->m_rig, nextRig, ensureBoneAux(self.data()));
        self->m_rig = std::move(nextRig);
        self->rebuildBoneTree();
        self->syncInspectorFromSelectedBone();
        self->pushSkeletonToDaemon();
        emit self->skeletonUpdated(self->m_rig.objectId, self->m_rig);
        emit self->ikPreviewRequested(self->m_rig.objectId, chain);
    };

    if (m_commandBus) {
        m_commandBus->execute(std::make_unique<BoneEditCommand>(
            tr("Create 2-Bone IK Chain"), std::move(oldRig), std::move(newRig), applyFn));
    } else {
        applyFn(newRig);
    }

    dispatchIkPreviewIpc(m_ipcClient, m_rig.objectId, chain);
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
            float desiredWorld[16];
            std::memcpy(desiredWorld, newRig.bones[i].world_bind, sizeof(desiredWorld));
            newRig.bones[i].parent = inheritedParent;
            if (inheritedParent == 0xFFFFFFFFu) {
                mat4DecomposeTrs(desiredWorld, newRig.bones[i].bind_pos, newRig.bones[i].bind_angle,
                                 newRig.bones[i].bind_scale);
            } else {
                float pInv[16];
                if (we_mat4_inverse(newRig.bones[inheritedParent].world_bind, pInv)) {
                    float localMat[16];
                    we_mat4_mul(pInv, desiredWorld, localMat);
                    mat4DecomposeTrs(localMat, newRig.bones[i].bind_pos, newRig.bones[i].bind_angle,
                                     newRig.bones[i].bind_scale);
                }
            }
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
    ensureTopologicalOrder(newRig, nextSelected);

    QPointer<BoneRiggingPanel> self(this);
    const int32_t targetObjectId = m_rig.objectId;
    auto applyFn = [self, targetObjectId, nextSelected](const BoneHierarchySnapshot &snap) {
        if (!self || self->m_rig.objectId != targetObjectId) {
            return;
        }
        BoneHierarchySnapshot nextRig = snap;
        sanitizeAndTrackXpbdTransition(self->m_rig, nextRig, ensureBoneAux(self.data()));
        self->m_rig = std::move(nextRig);
        self->m_selectedBoneIndex =
            self->m_rig.bones.empty()
                ? -1
                : std::clamp(nextSelected, 0, static_cast<int32_t>(self->m_rig.bones.size()) - 1);
        self->rebuildBoneTree();
        self->syncInspectorFromSelectedBone();
        self->pushSkeletonToDaemon();
        emit self->boneSelected(self->m_selectedBoneIndex);
        emit self->skeletonUpdated(self->m_rig.objectId, self->m_rig);
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

    auto *rootCombo = findChild<QComboBox *>(QStringLiteral("ikRootCombo"));
    auto *midCombo = findChild<QComboBox *>(QStringLiteral("ikMidCombo"));
    auto *endCombo = findChild<QComboBox *>(QStringLiteral("ikEndCombo"));
    if (rootCombo && midCombo && endCombo && rootCombo->currentIndex() >= 0 &&
        midCombo->currentIndex() >= 0 && endCombo->currentIndex() >= 0) {
        rootIdx = rootCombo->currentData().toInt();
        midIdx = midCombo->currentData().toInt();
        endIdx = endCombo->currentData().toInt();
    }

    if (rootIdx < 0 || midIdx < 0 || endIdx < 0 || rootIdx == midIdx || midIdx == endIdx ||
        rootIdx == endIdx) {
        if (m_selectedBoneIndex >= 0 &&
            static_cast<size_t>(m_selectedBoneIndex) < m_rig.bones.size()) {
            endIdx = m_selectedBoneIndex;
            const uint32_t midU = m_rig.bones[static_cast<size_t>(endIdx)].parent;
            if (midU != 0xFFFFFFFFu && midU < m_rig.bones.size()) {
                midIdx = static_cast<int32_t>(midU);
                const uint32_t rootU = m_rig.bones[midU].parent;
                if (rootU != 0xFFFFFFFFu && rootU < m_rig.bones.size()) {
                    rootIdx = static_cast<int32_t>(rootU);
                }
            }
        }
    }

    if (rootIdx < 0 || midIdx < 0 || endIdx < 0 || rootIdx == midIdx || midIdx == endIdx ||
        rootIdx == endIdx) {
        rootIdx = 0;
        midIdx = 1;
        endIdx = 2;
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
    emit boneSelected(m_selectedBoneIndex);
}

void BoneRiggingPanel::onTransformOrConstraintEdited()
{
    if (m_updatingUi || m_selectedBoneIndex < 0 ||
        static_cast<size_t>(m_selectedBoneIndex) >= m_rig.bones.size()) {
        return;
    }

    BoneHierarchySnapshot oldRig = m_rig;
    BoneHierarchySnapshot newRig = m_rig;
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
        newRig.driveModes[sel] = mode;

        if (mode == WE_BONE_DRIVE_XPBD) {
            if (!boneHasActiveSimulationJson(bone.simulation_json)) {
                writeDefaultXpbdSimulationJson(bone);
            }
            for (auto &c : newRig.ikChains) {
                if (c.root_bone == sel || c.mid_bone == sel || c.end_bone == sel) {
                    c.enabled = 0u;
                }
            }
            for (auto &a : newRig.aimConstraints) {
                if (a.bone_index == sel) {
                    a.enabled = 0u;
                }
            }
        } else {
            std::memset(bone.simulation_json, 0, sizeof(bone.simulation_json));
        }
    }

    // Sync active 2-Bone IK chain controls when a chain is selected.
    int activeChainIdx = -1;
    bool ikChainModified = false;
    auto *ikChainCombo = findChild<QComboBox *>(QStringLiteral("ikChainCombo"));
    if (ikChainCombo && !newRig.ikChains.empty()) {
        const int chainIdx = ikChainCombo->currentData().toInt();
        if (chainIdx >= 0 && static_cast<size_t>(chainIdx) < newRig.ikChains.size()) {
            activeChainIdx = chainIdx;
            we_ik_chain_t &chain = newRig.ikChains[static_cast<size_t>(chainIdx)];
            if (auto *tx = findChild<QDoubleSpinBox *>(QStringLiteral("ikTargetXSpin"))) {
                chain.target_world[0] = static_cast<float>(tx->value());
            }
            if (auto *ty = findChild<QDoubleSpinBox *>(QStringLiteral("ikTargetYSpin"))) {
                chain.target_world[1] = static_cast<float>(ty->value());
            }
            if (auto *px = findChild<QDoubleSpinBox *>(QStringLiteral("ikPoleXSpin"))) {
                chain.pole_world[0] = static_cast<float>(px->value());
            }
            if (auto *py = findChild<QDoubleSpinBox *>(QStringLiteral("ikPoleYSpin"))) {
                chain.pole_world[1] = static_cast<float>(py->value());
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
                chain.root_angle_min_rad =
                    static_cast<float>(m_ikMinAngleSpin->value()) * kDegToRad;
                chain.mid_angle_min_rad = chain.root_angle_min_rad;
            }
            if (m_ikMaxAngleSpin) {
                chain.root_angle_max_rad =
                    static_cast<float>(m_ikMaxAngleSpin->value()) * kDegToRad;
                chain.mid_angle_max_rad = chain.root_angle_max_rad;
            }
            if (auto *bs = findChild<QDoubleSpinBox *>(QStringLiteral("ikBlendSpin"))) {
                chain.blend_weight = std::clamp(static_cast<float>(bs->value()), 0.0f, 1.0f);
            }
            if (static_cast<size_t>(chainIdx) < oldRig.ikChains.size()) {
                ikChainModified = (std::memcmp(&oldRig.ikChains[static_cast<size_t>(chainIdx)],
                                               &chain, sizeof(we_ik_chain_t)) != 0);
            }
        }
    }

    // Sync single-bone Look-At / Aim constraint for the selected bone.
    if (auto *aimEnable = findChild<QCheckBox *>(QStringLiteral("aimEnableCheck"))) {
        auto aimIt =
            std::find_if(newRig.aimConstraints.begin(), newRig.aimConstraints.end(),
                         [sel](const we_aim_constraint_t &a) { return a.bone_index == sel; });
        const bool enabled = aimEnable->isChecked();
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
            if (auto *axisCombo = findChild<QComboBox *>(QStringLiteral("aimAxisCombo"))) {
                const int axisId = axisCombo->currentData().toInt();
                aimIt->aim_axis_local[0] = (axisId == 0) ? 1.0f : (axisId == 2 ? -1.0f : 0.0f);
                aimIt->aim_axis_local[1] = (axisId == 1) ? 1.0f : (axisId == 3 ? -1.0f : 0.0f);
                aimIt->aim_axis_local[2] = 0.0f;
            }
            if (auto *tx = findChild<QDoubleSpinBox *>(QStringLiteral("aimTargetXSpin"))) {
                aimIt->target_world[0] = static_cast<float>(tx->value());
            }
            if (auto *ty = findChild<QDoubleSpinBox *>(QStringLiteral("aimTargetYSpin"))) {
                aimIt->target_world[1] = static_cast<float>(ty->value());
            }
            if (auto *cone = findChild<QDoubleSpinBox *>(QStringLiteral("aimConeDegSpin"))) {
                aimIt->max_cone_rad = static_cast<float>(cone->value()) * kDegToRad;
            }
            if (auto *blend = findChild<QDoubleSpinBox *>(QStringLiteral("aimBlendSpin"))) {
                aimIt->blend_weight = std::clamp(static_cast<float>(blend->value()), 0.0f, 1.0f);
            }
            if (enabled && sel < WE_MDL_MAX_BONES && newRig.driveModes[sel] != WE_BONE_DRIVE_XPBD) {
                newRig.driveModes[sel] = WE_BONE_DRIVE_AIM;
            }
        }
    }

    recomputeWorldAndOffsetTransforms(newRig);

    QPointer<BoneRiggingPanel> self(this);
    const int32_t targetObjectId = m_rig.objectId;
    auto applyFn = [self, targetObjectId, activeChainIdx,
                    ikChainModified](const BoneHierarchySnapshot &snap) {
        if (!self || self->m_rig.objectId != targetObjectId) {
            return;
        }
        BoneHierarchySnapshot nextRig = snap;
        sanitizeAndTrackXpbdTransition(self->m_rig, nextRig, ensureBoneAux(self.data()));
        self->m_rig = std::move(nextRig);
        self->rebuildBoneTree();
        self->syncInspectorFromSelectedBone();
        self->pushSkeletonToDaemon();
        emit self->skeletonUpdated(self->m_rig.objectId, self->m_rig);
        if (ikChainModified && activeChainIdx >= 0 &&
            static_cast<size_t>(activeChainIdx) < self->m_rig.ikChains.size()) {
            const we_ik_chain_t &activeChain =
                self->m_rig.ikChains[static_cast<size_t>(activeChainIdx)];
            if (activeChain.enabled != 0u) {
                emit self->ikPreviewRequested(self->m_rig.objectId, activeChain);
                dispatchIkPreviewIpc(self->m_ipcClient, self->m_rig.objectId, activeChain);
            }
        }
    };

    if (m_commandBus) {
        m_commandBus->execute(
            std::make_unique<BoneEditCommand>(tr("Edit Bone %1").arg(QString::fromUtf8(bone.name)),
                                              std::move(oldRig), std::move(newRig), applyFn));
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
    const QScopedValueRollback<bool> guard(m_updatingUi, true);
    const size_t n = m_rig.bones.size();
    const bool hasSelection =
        (m_selectedBoneIndex >= 0 && static_cast<size_t>(m_selectedBoneIndex) < n);

    auto *deleteBoneBtn = findChild<QPushButton *>(QStringLiteral("deleteBoneBtn"));
    auto *makeRootBtn = findChild<QPushButton *>(QStringLiteral("makeRootBtn"));
    auto *parentCombo = findChild<QComboBox *>(QStringLiteral("parentCombo"));
    auto *ikChainCombo = findChild<QComboBox *>(QStringLiteral("ikChainCombo"));
    auto *deleteIkBtn = findChild<QPushButton *>(QStringLiteral("deleteIkBtn"));
    auto *ikRootCombo = findChild<QComboBox *>(QStringLiteral("ikRootCombo"));
    auto *ikMidCombo = findChild<QComboBox *>(QStringLiteral("ikMidCombo"));
    auto *ikEndCombo = findChild<QComboBox *>(QStringLiteral("ikEndCombo"));
    auto *ikTargetXSpin = findChild<QDoubleSpinBox *>(QStringLiteral("ikTargetXSpin"));
    auto *ikTargetYSpin = findChild<QDoubleSpinBox *>(QStringLiteral("ikTargetYSpin"));
    auto *ikPoleXSpin = findChild<QDoubleSpinBox *>(QStringLiteral("ikPoleXSpin"));
    auto *ikPoleYSpin = findChild<QDoubleSpinBox *>(QStringLiteral("ikPoleYSpin"));
    auto *ikBlendSpin = findChild<QDoubleSpinBox *>(QStringLiteral("ikBlendSpin"));
    auto *aimEnableCheck = findChild<QCheckBox *>(QStringLiteral("aimEnableCheck"));
    auto *aimTargetBoneCombo = findChild<QComboBox *>(QStringLiteral("aimTargetBoneCombo"));
    auto *aimAxisCombo = findChild<QComboBox *>(QStringLiteral("aimAxisCombo"));
    auto *aimTargetXSpin = findChild<QDoubleSpinBox *>(QStringLiteral("aimTargetXSpin"));
    auto *aimTargetYSpin = findChild<QDoubleSpinBox *>(QStringLiteral("aimTargetYSpin"));
    auto *aimConeDegSpin = findChild<QDoubleSpinBox *>(QStringLiteral("aimConeDegSpin"));
    auto *aimBlendSpin = findChild<QDoubleSpinBox *>(QStringLiteral("aimBlendSpin"));

    for (QWidget *w : std::initializer_list<QWidget *>{
             m_boneNameEdit, m_driveModeCombo, m_posXSpin, m_posYSpin, m_angleZSpin, m_scaleXSpin,
             m_scaleYSpin, deleteBoneBtn, makeRootBtn, parentCombo, aimEnableCheck}) {
        if (w) {
            w->setEnabled(hasSelection);
        }
    }

    if (m_addBoneBtn) {
        m_addBoneBtn->setEnabled(n < WE_MDL_MAX_BONES);
    }

    if (parentCombo) {
        const QSignalBlocker blocker(parentCombo);
        parentCombo->clear();
        parentCombo->addItem(tr("<Root>"), -1);
        for (size_t i = 0; i < n; ++i) {
            if (static_cast<int32_t>(i) == m_selectedBoneIndex) {
                continue;
            }
            QString bName = QString::fromUtf8(m_rig.bones[i].name).trimmed();
            if (bName.isEmpty()) {
                bName = QStringLiteral("Bone_%1").arg(i);
            }
            parentCombo->addItem(QStringLiteral("[%1] %2").arg(i).arg(bName), static_cast<int>(i));
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
    populateBoneCombo(ikRootCombo);
    populateBoneCombo(ikMidCombo);
    populateBoneCombo(ikEndCombo);
    if (ikRootCombo && ikMidCombo && ikEndCombo && n >= 3u && ikRootCombo->currentIndex() <= 0 &&
        ikMidCombo->currentIndex() <= 0 && ikEndCombo->currentIndex() <= 0) {
        ikRootCombo->setCurrentIndex(0);
        ikMidCombo->setCurrentIndex(1);
        ikEndCombo->setCurrentIndex(2);
    }
    if (m_createIkBtn) {
        m_createIkBtn->setEnabled(n >= 3u && m_rig.ikChains.size() < WE_RIG_MAX_IK_CHAINS);
    }

    if (aimTargetBoneCombo) {
        const QSignalBlocker blocker(aimTargetBoneCombo);
        aimTargetBoneCombo->clear();
        aimTargetBoneCombo->addItem(tr("Custom World Target"), -1);
        for (size_t i = 0; i < n; ++i) {
            if (static_cast<int32_t>(i) == m_selectedBoneIndex) {
                continue;
            }
            QString bName = QString::fromUtf8(m_rig.bones[i].name).trimmed();
            if (bName.isEmpty()) {
                bName = QStringLiteral("Bone_%1").arg(i);
            }
            aimTargetBoneCombo->addItem(QStringLiteral("[%1] %2").arg(i).arg(bName),
                                        static_cast<int>(i));
        }
    }

    if (ikChainCombo) {
        const int prevChain =
            ikChainCombo->currentIndex() >= 0 ? ikChainCombo->currentData().toInt() : 0;
        const QSignalBlocker blocker(ikChainCombo);
        ikChainCombo->clear();
        for (size_t i = 0; i < m_rig.ikChains.size(); ++i) {
            const auto &c = m_rig.ikChains[i];
            ikChainCombo->addItem(QStringLiteral("#%1 (%2 -> %3 -> %4)")
                                      .arg(i)
                                      .arg(c.root_bone)
                                      .arg(c.mid_bone)
                                      .arg(c.end_bone),
                                  static_cast<int>(i));
        }
        if (ikChainCombo->count() == 0) {
            ikChainCombo->addItem(tr("No IK Chains"), -1);
        } else {
            const int found = ikChainCombo->findData(prevChain);
            ikChainCombo->setCurrentIndex(found >= 0 ? found : 0);
        }
    }

    const bool hasIkChains = !m_rig.ikChains.empty();
    for (QWidget *w : std::initializer_list<QWidget *>{
             ikChainCombo, deleteIkBtn, ikTargetXSpin, ikTargetYSpin, ikPoleXSpin, ikPoleYSpin,
             m_poleFlipCheck, m_ikSoftZoneSpin, m_ikMaxStretchSpin, m_ikMinAngleSpin,
             m_ikMaxAngleSpin, ikBlendSpin}) {
        if (w) {
            w->setEnabled(hasIkChains);
        }
    }
    if (hasIkChains && ikChainCombo) {
        const int cIdx = std::clamp(ikChainCombo->currentData().toInt(), 0,
                                    static_cast<int>(m_rig.ikChains.size()) - 1);
        const we_ik_chain_t &c = m_rig.ikChains[static_cast<size_t>(cIdx)];
        if (ikTargetXSpin) {
            ikTargetXSpin->setValue(static_cast<double>(c.target_world[0]));
        }
        if (ikTargetYSpin) {
            ikTargetYSpin->setValue(static_cast<double>(c.target_world[1]));
        }
        if (ikPoleXSpin) {
            ikPoleXSpin->setValue(static_cast<double>(c.pole_world[0]));
        }
        if (ikPoleYSpin) {
            ikPoleYSpin->setValue(static_cast<double>(c.pole_world[1]));
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
        if (ikBlendSpin) {
            ikBlendSpin->setValue(static_cast<double>(c.blend_weight));
        }
    }

    if (!hasSelection) {
        if (m_boneNameEdit) {
            m_boneNameEdit->clear();
        }
        if (m_conflictBadgeLabel) {
            m_conflictBadgeLabel->setText(tr("No Selection"));
            m_conflictBadgeLabel->setStyleSheet(QStringLiteral(
                "QLabel { background-color: #181b22; color: #94a3b8; border: 1px solid #2a2f3d; "
                "border-radius: 4px; padding: 3px 6px; font-weight: 600; }"));
        }
        for (QWidget *w :
             std::initializer_list<QWidget *>{aimTargetBoneCombo, aimAxisCombo, aimTargetXSpin,
                                              aimTargetYSpin, aimConeDegSpin, aimBlendSpin}) {
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
    if (parentCombo) {
        const int pVal = (bone.parent == 0xFFFFFFFFu) ? -1 : static_cast<int>(bone.parent);
        const int idx = parentCombo->findData(pVal);
        parentCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    if (makeRootBtn) {
        makeRootBtn->setEnabled(bone.parent != 0xFFFFFFFFu);
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

    // Update conflict & drive mode pill badge.
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
    if (aimEnableCheck) {
        aimEnableCheck->setChecked(aimActive);
    }
    for (QWidget *w :
         std::initializer_list<QWidget *>{aimTargetBoneCombo, aimAxisCombo, aimTargetXSpin,
                                          aimTargetYSpin, aimConeDegSpin, aimBlendSpin}) {
        if (w) {
            w->setEnabled(aimActive);
        }
    }
    if (aimIt != m_rig.aimConstraints.end()) {
        if (aimAxisCombo) {
            int axisId = 0;
            if (aimIt->aim_axis_local[1] > 0.5f) {
                axisId = 1;
            } else if (aimIt->aim_axis_local[0] < -0.5f) {
                axisId = 2;
            } else if (aimIt->aim_axis_local[1] < -0.5f) {
                axisId = 3;
            }
            const int found = aimAxisCombo->findData(axisId);
            aimAxisCombo->setCurrentIndex(found >= 0 ? found : 0);
        }
        if (aimTargetXSpin) {
            aimTargetXSpin->setValue(static_cast<double>(aimIt->target_world[0]));
        }
        if (aimTargetYSpin) {
            aimTargetYSpin->setValue(static_cast<double>(aimIt->target_world[1]));
        }
        if (aimConeDegSpin) {
            aimConeDegSpin->setValue(static_cast<double>(aimIt->max_cone_rad * kRadToDeg));
        }
        if (aimBlendSpin) {
            aimBlendSpin->setValue(static_cast<double>(aimIt->blend_weight));
        }
    }
}

void BoneRiggingPanel::pushSkeletonToDaemon()
{
    if (!m_ipcClient || m_rig.objectId < 0) {
        return;
    }

    auto *aux = ensureBoneAux(this);
    std::vector<waywal_ipc_mdl_bone_t> wireBones;
    wireBones.reserve(m_rig.bones.size());

    bool reinitPhysics = aux ? aux->forceReinitPhysics : false;
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
        } else if (aux && i < WE_MDL_MAX_BONES && aux->lastPushedXpbd[i]) {
            reinitPhysics = true;
        }
        wireBones.push_back(dst);
    }

    if (aux) {
        for (size_t i = m_rig.bones.size(); i < WE_MDL_MAX_BONES; ++i) {
            if (aux->lastPushedXpbd[i]) {
                reinitPhysics = true;
            }
            aux->lastPushedXpbd[i] = false;
        }
        for (size_t i = 0; i < std::min<size_t>(m_rig.bones.size(), WE_MDL_MAX_BONES); ++i) {
            aux->lastPushedXpbd[i] = (m_rig.driveModes[i] == WE_BONE_DRIVE_XPBD);
        }
        aux->forceReinitPhysics = false;
    }

    dispatchSkeletonIpc(m_ipcClient, m_rig.objectId, wireBones, reinitPhysics ? 1u : 0u);
}

} // namespace waywal::studio
