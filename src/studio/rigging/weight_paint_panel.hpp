// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_RIGGING_WEIGHT_PAINT_PANEL_HPP
#define WAYWAL_STUDIO_RIGGING_WEIGHT_PAINT_PANEL_HPP

#include "studio/core/command_bus.hpp"
#include "studio/core/ipc_client.hpp"
#include "studio/core/rigging_commands.hpp"
#include "waywal/we_mesh_rig.h"
#include "waywal/we_puppet.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QWidget>

#include <array>
#include <cstdint>

namespace waywal::studio {

class WeightPaintPanel : public QWidget {
    Q_OBJECT

  public:
    explicit WeightPaintPanel(QWidget *parent = nullptr);
    ~WeightPaintPanel() override = default;

    void setCommandBus(CommandBus *bus) noexcept { m_commandBus = bus; }
    [[nodiscard]] CommandBus *commandBus() const noexcept { return m_commandBus; }

    void setIpcClient(IpcClient *client) noexcept { m_ipcClient = client; }
    [[nodiscard]] IpcClient *ipcClient() const noexcept { return m_ipcClient; }

    void setPuppetContext(int32_t objectId, const we_puppet_t *puppet);
    void setActiveBone(int32_t boneIndex);
    void setBoneLocked(uint32_t boneIndex, bool locked);
    [[nodiscard]] bool isBoneLocked(uint32_t boneIndex) const noexcept;
    [[nodiscard]] we_weight_brush_op_t activeBrushOp() const noexcept;
    [[nodiscard]] we_heatmap_palette_t activePalette() const noexcept;
    [[nodiscard]] float brushRadiusPx() const noexcept;
    [[nodiscard]] float brushStrength() const noexcept;
    [[nodiscard]] float targetWeightValue() const noexcept;
    [[nodiscard]] const std::array<uint64_t, 2> &lockedBoneMask() const noexcept
    {
        return m_lockedBoneMask;
    }

  public slots:
    void onSolveHeatWeightsClicked();
    void onNormalizeAllClicked();
    void onHeatmapToggleChanged(bool enabled);
    void setSolveTelemetry(uint32_t vertexCount, uint32_t boneCount, float elapsedMs,
                           bool usedIc0Fallback);

  signals:
    void heatmapModeChanged(int32_t objectId, bool enabled, uint32_t activeBoneIdx, float opacity,
                            we_heatmap_palette_t palette);
    void heatSolveRequested(int32_t objectId, const we_heat_skin_cfg_t &cfg);
    void normalizeAllWeightsRequested(int32_t objectId, uint64_t lockMaskLo, uint64_t lockMaskHi);
    void brushParamsChanged();
    void statusMessage(const QString &msg);

  private:
    void setupUi();
    void sendHeatmapModeIpc();

    int32_t m_objectId{-1};
    int32_t m_activeBoneIndex{0};
    std::array<uint64_t, 2> m_lockedBoneMask{0ull, 0ull};

    CommandBus *m_commandBus{nullptr};
    IpcClient *m_ipcClient{nullptr};

    QListWidget *m_boneWeightList{nullptr};
    QComboBox *m_brushOpCombo{nullptr};
    QComboBox *m_paletteCombo{nullptr};
    QDoubleSpinBox *m_radiusSpin{nullptr};
    QDoubleSpinBox *m_strengthSpin{nullptr};
    QDoubleSpinBox *m_targetWeightSpin{nullptr};
    QDoubleSpinBox *m_heatFalloffSpin{nullptr};
    QDoubleSpinBox *m_heatSmoothSpin{nullptr};
    QCheckBox *m_heatmapEnableCheck{nullptr};
    QCheckBox *m_visibilityRaycastCheck{nullptr};
    QSlider *m_heatmapOpacitySlider{nullptr};
    QLabel *m_solveTelemetryLabel{nullptr};
    QPushButton *m_solveHeatBtn{nullptr};
    QPushButton *m_normalizeBtn{nullptr};
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_RIGGING_WEIGHT_PAINT_PANEL_HPP
