// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_RIGGING_BONE_RIGGING_PANEL_HPP
#define WAYWAL_STUDIO_RIGGING_BONE_RIGGING_PANEL_HPP

#include "studio/core/command_bus.hpp"
#include "studio/core/ipc_client.hpp"
#include "studio/core/rigging_commands.hpp"
#include "waywal/we_mesh_rig.h"
#include "waywal/we_puppet.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QWidget>

#include <cstdint>
#include <vector>

namespace waywal::studio {

class BoneRiggingPanel : public QWidget {
    Q_OBJECT

  public:
    explicit BoneRiggingPanel(QWidget *parent = nullptr);
    ~BoneRiggingPanel() override = default;

    void setCommandBus(CommandBus *bus) noexcept { m_commandBus = bus; }
    [[nodiscard]] CommandBus *commandBus() const noexcept { return m_commandBus; }

    void setIpcClient(IpcClient *client) noexcept { m_ipcClient = client; }
    [[nodiscard]] IpcClient *ipcClient() const noexcept { return m_ipcClient; }

    void loadPuppetRig(const we_puppet_t *puppet, int32_t sceneObjectId);
    void selectBone(int32_t boneIndex);
    bool addBone(const QString &name, int32_t parentIndex, const float localPos[3],
                 float angleZRad);
    bool reparentBone(int32_t childIndex, int32_t newParentIndex);
    bool createTwoBoneIkChain(uint16_t rootBone, uint16_t midBone, uint16_t endBone);

    [[nodiscard]] int32_t selectedBoneIndex() const noexcept { return m_selectedBoneIndex; }
    [[nodiscard]] const BoneHierarchySnapshot &rigSnapshot() const noexcept { return m_rig; }

  public slots:
    void onAddBoneClicked();
    void onDeleteBoneClicked();
    void onCreateIkChainClicked();
    void onTogglePoleFlipClicked();
    void setBoneDriveMode(int32_t objectId, int32_t boneIndex, we_bone_drive_mode_t mode);

  signals:
    void boneSelected(int32_t boneIndex);
    void skeletonUpdated(int32_t objectId, const BoneHierarchySnapshot &snapshot);
    void ikPreviewRequested(int32_t objectId, const we_ik_chain_t &chain);
    void statusMessage(const QString &msg);

  private slots:
    void onBoneTreeCurrentItemChanged(QTreeWidgetItem *current, QTreeWidgetItem *previous);
    void onTransformOrConstraintEdited();

  private:
    void setupUi();
    void rebuildBoneTree();
    void syncInspectorFromSelectedBone();
    void pushSkeletonToDaemon();
    [[nodiscard]] bool wouldCreateCycle(int32_t childIdx, int32_t parentIdx) const noexcept;

    int32_t m_selectedBoneIndex{-1};
    bool m_updatingUi{false};
    BoneHierarchySnapshot m_rig;

    CommandBus *m_commandBus{nullptr};
    IpcClient *m_ipcClient{nullptr};

    QTreeWidget *m_boneTree{nullptr};
    QLineEdit *m_boneNameEdit{nullptr};
    QComboBox *m_driveModeCombo{nullptr};
    QDoubleSpinBox *m_posXSpin{nullptr};
    QDoubleSpinBox *m_posYSpin{nullptr};
    QDoubleSpinBox *m_angleZSpin{nullptr};
    QDoubleSpinBox *m_scaleXSpin{nullptr};
    QDoubleSpinBox *m_scaleYSpin{nullptr};
    QDoubleSpinBox *m_ikSoftZoneSpin{nullptr};
    QDoubleSpinBox *m_ikMaxStretchSpin{nullptr};
    QDoubleSpinBox *m_ikMinAngleSpin{nullptr};
    QDoubleSpinBox *m_ikMaxAngleSpin{nullptr};
    QCheckBox *m_poleFlipCheck{nullptr};
    QLabel *m_conflictBadgeLabel{nullptr};
    QPushButton *m_addBoneBtn{nullptr};
    QPushButton *m_createIkBtn{nullptr};
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_RIGGING_BONE_RIGGING_PANEL_HPP
