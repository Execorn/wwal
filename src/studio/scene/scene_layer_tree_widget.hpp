// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_SCENE_SCENE_LAYER_TREE_WIDGET_HPP
#define WAYWAL_STUDIO_SCENE_SCENE_LAYER_TREE_WIDGET_HPP

#include "studio/core/command_bus_authoring.hpp"
#include "studio/core/ipc_client.hpp"
#include "waywal/ipc_proto_authoring.h"

#include <QAbstractItemModel>
#include <QLineEdit>
#include <QMimeData>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QString>
#include <QTreeView>
#include <QWidget>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace waywal::studio {

enum class SceneLayerViewMode : uint8_t {
    HierarchyTree = 0,   /* Parent-child transform hierarchy */
    FlatZOrderStack = 1, /* Pure back-to-front draw order (foreground on top) */
};

struct SceneLayerNode {
    int32_t id{-1};          /* Matches we_scene_object_t::id */
    int32_t parentId{-1};    /* -1 for root */
    uint32_t zOrderIndex{0}; /* Ascending back-to-front draw order index */
    we_scene_node_kind_t kind{WE_NODE_IMAGE};
    uint32_t blendMode{0}; /* we_blend_mode_t */
    bool visible{true};
    bool locked{false};
    bool hasScript{false};
    bool hasShader{false};
    bool hasAudioBinding{false};
    std::array<float, 3> origin{0.0f, 0.0f, 0.0f};
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
    std::array<float, 3> angles{0.0f, 0.0f, 0.0f};
    std::array<float, 2> size{1920.0f, 1080.0f};
    std::array<float, 2> parallaxDepth{0.0f, 0.0f};
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
    QString name;
};

class SceneLayerTreeModel final : public QAbstractItemModel {
    Q_OBJECT

  public:
    static constexpr uint32_t kMaxParentDepth = 16u;
    static constexpr const char *kMimeType = "application/x-wwal-layer-ids";

    enum Column : int {
        ColName = 0,
        ColBlendBadge = 1,
        ColLock = 2,
        ColVisible = 3,
        ColCount = 4,
    };

    explicit SceneLayerTreeModel(QObject *parent = nullptr);
    ~SceneLayerTreeModel() override = default;

    void setViewMode(SceneLayerViewMode mode);
    [[nodiscard]] SceneLayerViewMode viewMode() const noexcept { return m_viewMode; }

    void setNodes(std::vector<SceneLayerNode> nodes);
    [[nodiscard]] const std::vector<SceneLayerNode> &nodes() const noexcept { return m_nodes; }
    [[nodiscard]] const SceneLayerNode *findNodeById(int32_t objectId) const noexcept;
    [[nodiscard]] bool wouldCreateCycleOrExceedDepth(int32_t childId,
                                                     int32_t candidateParentId) const noexcept;
    [[nodiscard]] QModelIndex indexForObjectId(int32_t objectId) const;

    /* QAbstractItemModel interface */
    [[nodiscard]] QModelIndex index(int row, int column,
                                    const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex &child) const override;
    [[nodiscard]] int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index,
                                int role = Qt::DisplayRole) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex &index) const override;
    [[nodiscard]] QStringList mimeTypes() const override;
    [[nodiscard]] QMimeData *mimeData(const QModelIndexList &indexes) const override;
    [[nodiscard]] bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row,
                                    int column, const QModelIndex &parent) override;
    [[nodiscard]] Qt::DropActions supportedDropActions() const override;

  signals:
    void reparentRequested(int32_t objectId, int32_t newParentId, bool preserveWorldTransform);
    void reorderZRequested(int32_t objectId, uint32_t newZOrderIndex);

  private:
    void rebuildIndexMaps();
    [[nodiscard]] const std::vector<int32_t> &childrenOf(int32_t parentId) const;

    SceneLayerViewMode m_viewMode{SceneLayerViewMode::HierarchyTree};
    std::vector<SceneLayerNode> m_nodes;
    std::unordered_map<int32_t, size_t> m_idToSlot;
    std::unordered_map<int32_t, std::vector<int32_t>> m_childrenByParent;
    std::vector<int32_t> m_flatZOrderIds;
    std::vector<int32_t> m_emptyChildren;
};

class SceneLayerTreeWidget : public QWidget {
    Q_OBJECT

  public:
    explicit SceneLayerTreeWidget(QWidget *parent = nullptr);
    ~SceneLayerTreeWidget() override = default;

    void setCommandBus(CommandBus *bus) noexcept { m_commandBus = bus; }
    [[nodiscard]] CommandBus *commandBus() const noexcept { return m_commandBus; }

    void setIpcClient(IpcClient *client) noexcept { m_ipcClient = client; }
    [[nodiscard]] IpcClient *ipcClient() const noexcept { return m_ipcClient; }

    void setTargetOutput(const QString &output) { m_targetOutput = output; }
    [[nodiscard]] QString targetOutput() const noexcept { return m_targetOutput; }

    void loadFromSceneJson(const QString &sceneTreeJson);
    void selectObject(int32_t objectId);
    [[nodiscard]] int32_t selectedObjectId() const noexcept;
    [[nodiscard]] const SceneLayerNode *selectedNode() const noexcept;

  signals:
    void objectSelected(int32_t objectId, we_scene_node_kind_t kind);
    void layerMutated(int32_t objectId);
    void statusMessage(const QString &msg);

  protected:
    void showEvent(QShowEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

  private:
    void ensureUiInitialized();
    void applyNodesPreservingSelection(std::vector<SceneLayerNode> nodes, int32_t preferredId = -1);
    void executeFlagEdit(int32_t objectId, const SetLayerFlagsCommand::LayerFlagState &nextFlags);
    void dispatchMutateIpc(const waywal_ipc_req_scene_mutate_t &req);

    bool m_uiInitialized{false};
    QString m_targetOutput{QStringLiteral("*")};
    CommandBus *m_commandBus{nullptr};
    IpcClient *m_ipcClient{nullptr};

    SceneLayerTreeModel *m_model{nullptr};
    QSortFilterProxyModel *m_proxyModel{nullptr};
    QLineEdit *m_searchEdit{nullptr};
    QPushButton *m_viewModeBtn{nullptr};
    QPushButton *m_addLayerBtn{nullptr};
    QTreeView *m_treeView{nullptr};
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_SCENE_SCENE_LAYER_TREE_WIDGET_HPP
