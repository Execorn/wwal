// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_RIGGING_PUPPET_MESH_PANEL_HPP
#define WAYWAL_STUDIO_RIGGING_PUPPET_MESH_PANEL_HPP

#include "studio/core/command_bus.hpp"
#include "studio/core/ipc_client.hpp"
#include "studio/core/rigging_commands.hpp"
#include "waywal/we_mesh_cdt.h"
#include "waywal/we_puppet.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QTreeWidget>
#include <QWidget>

#include <cstdint>
#include <vector>

namespace waywal::studio {

class PuppetMeshPanel : public QWidget {
    Q_OBJECT

  public:
    explicit PuppetMeshPanel(QWidget *parent = nullptr);
    ~PuppetMeshPanel() override = default;

    void setCommandBus(CommandBus *bus) noexcept { m_commandBus = bus; }
    [[nodiscard]] CommandBus *commandBus() const noexcept { return m_commandBus; }

    void setIpcClient(IpcClient *client) noexcept { m_ipcClient = client; }
    [[nodiscard]] IpcClient *ipcClient() const noexcept { return m_ipcClient; }

    void setTargetObjectId(int32_t objectId, uint32_t texWidth, uint32_t texHeight);
    void setSourceAlpha(std::vector<uint8_t> alphaBuffer, uint32_t width, uint32_t height);
    void loadExistingMesh(const we_puppet_t *puppet);
    void addConstraintPin(double px, double py);
    void removeConstraintPin(uint32_t index);

    [[nodiscard]] we_mesh_gen_cfg_t currentConfig() const noexcept;
    [[nodiscard]] const we_mesh_gen_result_t &lastResult() const noexcept { return m_lastResult; }
    [[nodiscard]] const std::vector<we_cdt_pin_t> &pins() const noexcept { return m_pins; }
    [[nodiscard]] bool isWireframeVisible() const noexcept;

  public slots:
    void onGenerateCutoutMeshClicked();
    void onExtractContoursClicked();
    void onClearPinsClicked();
    void onExportMdlClicked();

  signals:
    void meshGenerated(int32_t objectId, uint32_t vertexCount, uint32_t triangleCount,
                       float minAngleDeg);
    void wireframeOverlayRequested(bool visible, const we_mesh_gen_result_t &mesh);
    void contourPinsChanged(int32_t objectId, const std::vector<we_cdt_pin_t> &pins);
    void exportMdlRequested(int32_t objectId, const QString &destPath);
    void statusMessage(const QString &msg);

  private:
    void setupUi();
    void updateMeshStatsLabel();
    void syncMeshToDaemon(const PuppetMeshSnapshot &snapshot);

    int32_t m_objectId{-1};
    uint32_t m_texWidth{0};
    uint32_t m_texHeight{0};
    std::vector<uint8_t> m_alphaBuffer;
    std::vector<we_cdt_pin_t> m_pins;
    we_mesh_gen_result_t m_lastResult{};

    CommandBus *m_commandBus{nullptr};
    IpcClient *m_ipcClient{nullptr};

    QDoubleSpinBox *m_alphaThreshSpin{nullptr};
    QDoubleSpinBox *m_dilationPxSpin{nullptr};
    QDoubleSpinBox *m_simplifyEpsSpin{nullptr};
    QDoubleSpinBox *m_interiorSpacingSpin{nullptr};
    QDoubleSpinBox *m_minAngleSpin{nullptr};
    QSpinBox *m_maxVertsSpin{nullptr};
    QCheckBox *m_wireframeCheck{nullptr};
    QTreeWidget *m_pinTree{nullptr};
    QLabel *m_statsLabel{nullptr};
    QPushButton *m_generateBtn{nullptr};
    QPushButton *m_exportBtn{nullptr};
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_RIGGING_PUPPET_MESH_PANEL_HPP
