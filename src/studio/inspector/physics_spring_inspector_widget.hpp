// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_INSPECTOR_PHYSICS_SPRING_INSPECTOR_WIDGET_HPP
#define WAYWAL_STUDIO_INSPECTOR_PHYSICS_SPRING_INSPECTOR_WIDGET_HPP

#include "studio/core/command_bus_phase3.hpp"
#include "studio/core/ipc_client.hpp"
#include "waywal/we_physics.h"
#include "waywal/we_puppet.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QTreeWidget>
#include <QWidget>

#include <cstdint>
#include <vector>

namespace waywal::studio {

class PhysicsSpringInspectorWidget : public QWidget {
    Q_OBJECT

  public:
    explicit PhysicsSpringInspectorWidget(QWidget *parent = nullptr);
    ~PhysicsSpringInspectorWidget() override = default;

    void setCommandBus(CommandBus *bus) noexcept { m_commandBus = bus; }
    [[nodiscard]] CommandBus *commandBus() const noexcept { return m_commandBus; }

    void setIpcClient(IpcClient *client) noexcept { m_ipcClient = client; }
    [[nodiscard]] IpcClient *ipcClient() const noexcept { return m_ipcClient; }

    void setTargetOutput(const QString &output) { m_targetOutput = output; }
    [[nodiscard]] QString targetOutput() const noexcept { return m_targetOutput; }

    void loadPuppetSkeleton(const we_puppet_t *puppet, int32_t sceneObjectId = -1);
    void applyMaterialPreset(we_physics_preset_t preset);
    void selectBoneIndex(int32_t boneIndex);
    [[nodiscard]] int32_t selectedBoneIndex() const noexcept { return m_selectedBoneIndex; }
    [[nodiscard]] const we_bone_sim_config_t &currentBoneConfig() const noexcept;
    [[nodiscard]] const we_bone_sim_config_t &boneConfig(int32_t boneIndex) const noexcept;
    [[nodiscard]] const we_wind_field_t &windField() const noexcept { return m_ctx.wind; }
    [[nodiscard]] const we_cursor_force_t &cursorForce() const noexcept { return m_ctx.cursor; }

    void populateBoneList(const we_puppet_t *puppet = nullptr);

  public slots:
    void updateSolverTelemetry(uint32_t activeBones, float stepTimeUs);
    void onPropagateToChildren() { onApplyPresetToDescendantsClicked(); }

  signals:
    void physicsParamsChanged(const QString &output, int32_t objectId, int32_t boneIndex,
                              const we_bone_sim_config_t &cfg, const we_wind_field_t &wind,
                              const we_cursor_force_t &cursor);
    void statusMessage(const QString &msg);

  private slots:
    void onBoneTreeCurrentItemChanged(QTreeWidgetItem *current, QTreeWidgetItem *previous);
    void onPresetComboActivated(int index);
    void onBoneConfigControlChanged();
    void onWindControlChanged();
    void onCursorControlChanged();
    void onResetBoneClicked();
    void onApplyPresetToDescendantsClicked();

  private:
    void setupUi();
    void syncUiFromSelectedBone();
    void refreshBoneTreeStates();
    void commitPhysicsChange(const we_bone_sim_config_t &newCfg, const we_wind_field_t &newWind,
                             const we_cursor_force_t &newCursor);
    void sendIpcUpdate(const QString &output, int32_t objectId, int32_t boneIndex,
                       const we_bone_sim_config_t &cfg, const we_wind_field_t &wind,
                       const we_cursor_force_t &cursor);

    QString m_targetOutput;
    int32_t m_objectId{-1};
    int32_t m_selectedBoneIndex{0};
    we_puppet_physics_ctx_t m_ctx{};
    bool m_updatingUi{false};
    std::vector<QString> m_boneNames;
    std::vector<int32_t> m_boneParents;

    CommandBus *m_commandBus{nullptr};
    IpcClient *m_ipcClient{nullptr};

    QTreeWidget *m_boneTree{nullptr};
    QComboBox *m_presetCombo{nullptr};
    QCheckBox *m_boneEnableCheck{nullptr};
    QDoubleSpinBox *m_stretchComplianceSpin{nullptr};
    QDoubleSpinBox *m_bendComplianceSpin{nullptr};
    QDoubleSpinBox *m_dampingSpin{nullptr};
    QDoubleSpinBox *m_massSpin{nullptr};
    QDoubleSpinBox *m_gravityScaleSpin{nullptr};
    QDoubleSpinBox *m_angleMinSpin{nullptr};
    QDoubleSpinBox *m_angleMaxSpin{nullptr};
    QDoubleSpinBox *m_collisionRadiusSpin{nullptr};
    QDoubleSpinBox *m_audioDragSpin{nullptr};
    QDoubleSpinBox *m_rayleighDampingSpin{nullptr};
    QCheckBox *m_windEnableCheck{nullptr};
    QDoubleSpinBox *m_windDirectionSpin{nullptr};
    QDoubleSpinBox *m_windSpeedSpin{nullptr};
    QDoubleSpinBox *m_windGustSpin{nullptr};
    QDoubleSpinBox *m_windGustFreqSpin{nullptr};
    QDoubleSpinBox *m_windSpatialScaleSpin{nullptr};
    QCheckBox *m_cursorEnableCheck{nullptr};
    QDoubleSpinBox *m_cursorRadiusSpin{nullptr};
    QDoubleSpinBox *m_cursorStrengthSpin{nullptr};
    QDoubleSpinBox *m_cursorFalloffSpin{nullptr};
    QLabel *m_telemetryLabel{nullptr};
    QPushButton *m_resetBoneBtn{nullptr};
    QPushButton *m_applyDescendantsBtn{nullptr};
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_INSPECTOR_PHYSICS_SPRING_INSPECTOR_WIDGET_HPP
