// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_GRAPH_NODE_GRAPH_EDITOR_WIDGET_HPP
#define WAYWAL_STUDIO_GRAPH_NODE_GRAPH_EDITOR_WIDGET_HPP

#include "studio/core/command_bus_phase3.hpp"
#include "studio/core/ipc_client.hpp"
#include "waywal/shader_graph.h"
#include "waywal/we_builtin_effects.h"

#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPointF>
#include <QPushButton>
#include <QString>
#include <QWidget>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace waywal::studio {

enum class GraphSemanticZoomLod : uint8_t {
    MacroOverview = 0, /* scale < 0.40: Node silhouette + category bar + major pass wires only */
    CompactPins = 1, /* 0.40 <= scale < 0.75: Header + typed pin shapes, inline spinboxes hidden */
    FullDetail = 2,  /* scale >= 0.75: Full controls, swizzle badges, 64x64 live thumbnails */
};

class NodeGraphEditorWidget : public QWidget {
    Q_OBJECT

  public:
    static constexpr float kMagneticSnapRadiusPx = 14.0f;
    static constexpr float kLodMacroThreshold = 0.40f;
    static constexpr float kLodCompactThreshold = 0.75f;

    explicit NodeGraphEditorWidget(QWidget *parent = nullptr);
    ~NodeGraphEditorWidget() override;

    void setCommandBus(CommandBus *bus) noexcept { m_commandBus = bus; }
    [[nodiscard]] CommandBus *commandBus() const noexcept { return m_commandBus; }

    void setIpcClient(IpcClient *client) noexcept { m_ipcClient = client; }
    [[nodiscard]] IpcClient *ipcClient() const noexcept { return m_ipcClient; }

    void setTargetOutput(const QString &output) { m_targetOutput = output; }
    [[nodiscard]] QString targetOutput() const noexcept { return m_targetOutput; }

    /* Graph document lifecycle */
    void newEmptyGraph(const QString &name = QStringLiteral("Untitled Shader Graph"));
    bool loadGraphFromFile(const QString &filePath);
    bool saveGraphToFile(const QString &filePath) const;
    [[nodiscard]] const waywal_shader_graph_t &graph() const noexcept { return *m_graph; }
    [[nodiscard]] const waywal_graph_schedule_t &lastSchedule() const noexcept
    {
        return m_schedule;
    }

    /* Graph mutations (dispatched through CommandBus for 1,000-step undo/redo) */
    uint32_t addNode(waywal_node_op_t op, const QPointF &scenePos,
                     const QString &propOrEffectSlug = QString());
    uint32_t addBuiltinEffectNode(we_builtin_effect_id_t effectId, const QPointF &scenePos);
    void removeSelectedNodes();
    bool connectPins(uint32_t srcNodeId, uint32_t srcPinIdx, uint32_t dstNodeId,
                     uint32_t dstPinIdx);
    void disconnectWire(uint32_t edgeId);
    void setNodeConstant(uint32_t nodeId, const std::array<float, 4> &val);

    /* Compilation & Live Daemon Dispatch */
    bool validateAndSchedule();
    QString compileToGlslSource();
    bool compileAndHotSwapDaemon();

    /* Semantic Zoom & Accessibility */
    void setZoomScale(float scale);
    [[nodiscard]] float zoomScale() const noexcept { return m_zoomScale; }
    [[nodiscard]] GraphSemanticZoomLod currentLod() const noexcept;
    void openQuickConnectPopup(const QPointF &scenePos, int32_t fromNodeId = -1,
                               int32_t fromPinIdx = -1, bool fromOutput = true);

  signals:
    void graphModified();
    void graphCompiled(bool success, const QString &glslCode, const QString &diagnosticMsg,
                       uint32_t aluOps, uint32_t texFetches);
    void graphCompiled(const QString &glslCode, uint32_t activePassMask);
    void nodeSelected(uint32_t nodeId);
    void statusMessage(const QString &msg);
    void nodeConstantScrubbed(uint32_t nodeId, uint32_t pinIdx, float val);

  protected:
    void keyPressEvent(QKeyEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

  private:
    friend class NodeItem;
    friend class WireItem;
    friend class GraphCanvasView;

    void setupUi();
    void rebuildSceneFromModel();
    void applyNodeMutation(const waywal_graph_node_t &node, bool isAdd);
    void applyWireMutation(const waywal_graph_edge_t &edge, bool isConnect);

    std::unique_ptr<waywal_shader_graph_t, void (*)(waywal_shader_graph_t *)> m_graph;
    waywal_graph_schedule_t m_schedule{};
    CommandBus *m_commandBus{nullptr};
    IpcClient *m_ipcClient{nullptr};
    QString m_targetOutput;
    float m_zoomScale{1.0f};
    uint32_t m_nextNodeId{1};
    uint32_t m_nextEdgeId{1};

    QGraphicsScene *m_scene{nullptr};
    QGraphicsView *m_view{nullptr};
    QLineEdit *m_searchBox{nullptr};
    QLabel *m_costBadgeLabel{nullptr};
    QPushButton *m_compileBtn{nullptr};
    QMap<uint32_t, QImage> m_nodeThumbnails;
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_GRAPH_NODE_GRAPH_EDITOR_WIDGET_HPP
