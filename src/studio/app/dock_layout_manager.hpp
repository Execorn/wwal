// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_DOCK_LAYOUT_MANAGER_HPP
#define WAYWAL_STUDIO_DOCK_LAYOUT_MANAGER_HPP

#include <QObject>
#include <QString>
#include <QStringList>
#include <QMap>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDockWidget>
#include <QMainWindow>
#include <QWidget>
#include <QLabel>
#include <QToolButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMouseEvent>

#ifdef HAVE_QT_ADS
#include <DockManager.h>
#include <DockWidget.h>
#include <DockAreaWidget.h>
#endif

namespace waywal::studio {

/**
 * @brief Sleek custom dark titlebar for dock panels.
 *
 * Implements Blender / DaVinci Resolve compact title styling:
 * - Drag handle & panel title label
 * - Float / Detach button with live icon toggle
 * - Close / Hide button
 * - Double-click to toggle floating state
 */
class StudioDockTitleBar : public QWidget {
    Q_OBJECT

public:
    explicit StudioDockTitleBar(const QString& title, QDockWidget* parentDock, QWidget* parent = nullptr);
    ~StudioDockTitleBar() override = default;

    void setTitle(const QString& title);
    QString title() const;
    void updateFloatingState(bool floating);

signals:
    void floatToggleRequested();
    void closeRequested();

protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    QDockWidget* m_parentDock{nullptr};
    QLabel* m_titleLabel{nullptr};
    QToolButton* m_floatButton{nullptr};
    QToolButton* m_closeButton{nullptr};
};

/**
 * @brief Studio dockable, tabbable, and detachable panel container.
 */
class StudioDockWidget : public QDockWidget {
    Q_OBJECT

public:
    explicit StudioDockWidget(const QString& id, const QString& title, QWidget* content, QWidget* parent = nullptr);
    ~StudioDockWidget() override = default;

    const QString& panelId() const { return m_id; }
    QWidget* contentWidget() const { return m_content; }
    void setTitleText(const QString& title);
    void setChromelessBarMode(bool chromeless);

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onTopLevelChanged(bool floating);

private:
    QString m_id;
    QWidget* m_content{nullptr};
    StudioDockTitleBar* m_customTitleBar{nullptr};
};

/**
 * @brief High-performance layout & perspective manager for wwal-studio.
 *
 * Key capabilities:
 * 1. Supports dockable, tabbable, splittable, and detachable floating panels.
 * 2. Uses Qt-Advanced-Docking-System if detected via HAVE_QT_ADS, with a
 *    zero-dependency native QDockWidget nested splitter/tab tear-off engine built-in.
 * 3. Serializes and restores perspectives (Default, Shader Coding, Theme Studio,
 *    Multi-Monitor) to $XDG_CONFIG_HOME/wwal/studio_layout.json.
 */
class DockLayoutManager : public QObject {
    Q_OBJECT

public:
    explicit DockLayoutManager(QMainWindow* mainWindow, QObject* parent = nullptr);
    ~DockLayoutManager() override;

    // Panel Registration
    void registerPanel(const QString& id, QWidget* widget, const QString& title,
                       Qt::DockWidgetArea defaultArea = Qt::LeftDockWidgetArea);
    void unregisterPanel(const QString& id);
    StudioDockWidget* dockWidget(const QString& id) const;
    QWidget* panelWidget(const QString& id) const;
    QStringList registeredPanelIds() const;

    // Visibility & Floating
    void setPanelVisible(const QString& id, bool visible);
    bool isPanelVisible(const QString& id) const;
    void togglePanelVisibility(const QString& id);
    void setPanelFloating(const QString& id, bool floating);
    bool isPanelFloating(const QString& id) const;

    // Perspectives
    QStringList availablePerspectives() const;
    QString activePerspective() const;
    bool switchPerspective(const QString& name);
    void savePerspective(const QString& name);
    void removePerspective(const QString& name);
    void resetToDefaultPerspective();
    void applyCurrentSplitSizes();

    // Layout Persistence
    QJsonObject serializeLayout() const;
    bool deserializeLayout(const QJsonObject& root);

    QJsonObject serializePerspectiveTree(const QString& perspectiveName) const;
    bool deserializePerspectiveTree(const QString& perspectiveName, const QJsonObject& perspectiveObj);

    bool saveLayoutToFile(const QString& filePath = QString());
    bool loadLayoutFromFile(const QString& filePath = QString());
    QString defaultLayoutFilePath() const;

    // Built-in Perspective Trees
    static QJsonObject createDefaultPerspectiveTree();
    static QJsonObject createShaderCodingPerspectiveTree();
    static QJsonObject createThemeStudioPerspectiveTree();
    static QJsonObject createMultiMonitorPerspectiveTree();

signals:
    void perspectiveChanged(const QString& perspectiveName);
    void layoutChanged();
    void panelVisibilityChanged(const QString& panelId, bool visible);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QString resolvePanelId(const QString& idOrAlias) const;
    StudioDockWidget* resolveDock(const QString& idOrAlias) const;
    StudioDockWidget* findPrimaryDock(const QJsonObject& node) const;
    void applyPerspectiveTree(const QJsonObject& tree);
    void buildSubtreeTopDown(const QJsonObject& node, StudioDockWidget* currentPrimary);
    void applySubtreeSizes(const QJsonObject& node, int availW, int availH,
                           QList<QDockWidget*>& horizDocks, QList<int>& horizSizes);
    int computeSubtreeSpan(const QJsonObject& node, Qt::Orientation orient) const;
    void syncLiveRatiosFromGeometry(QJsonObject& node) const;
    QDockWidget* layoutSubtree(const QJsonObject& node, Qt::Orientation parentOrientation);
    void collectDocksInTree(const QJsonObject& node, QSet<QString>& outIds);
    void setupDefaultPerspectives();

    QMainWindow* m_mainWindow{nullptr};
    QMap<QString, StudioDockWidget*> m_docks;
    QMap<QString, QString> m_aliases;
    QMap<QString, QJsonObject> m_perspectives;
    QString m_activePerspective{"Default"};
    bool m_applyingLayout{false};

#ifdef HAVE_QT_ADS
    ads::CDockManager* m_adsManager{nullptr};
    QMap<QString, ads::CDockWidget*> m_adsDocks;
#endif
};

} // namespace waywal::studio

// Expose in global scope for convenience
using waywal::studio::DockLayoutManager;
using waywal::studio::StudioDockWidget;
using waywal::studio::StudioDockTitleBar;

#endif // WAYWAL_STUDIO_DOCK_LAYOUT_MANAGER_HPP
