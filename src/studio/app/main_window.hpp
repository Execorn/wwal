// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_MAIN_WINDOW_HPP
#define WAYWAL_STUDIO_MAIN_WINDOW_HPP

#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QAction>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <memory>
#include <thread>

namespace wwal::studio {
class VirtualizedGridWidget;
class AudioSpectrumWidget;
class PowerGovernorWidget;
class TelemetryHudWidget;
class StudioFooterBar;
class CatalogIndexManager;
class DownloadManager;
class RecursiveFsWatcher;
class WorkshopBrowserWidget;
class OpenHubBrowserWidget;
class CreatorPackagingWizard;
} // namespace wwal::studio

namespace waywal::studio {

using ::wwal::studio::CatalogIndexManager;
using ::wwal::studio::CreatorPackagingWizard;
using ::wwal::studio::DownloadManager;
using ::wwal::studio::OpenHubBrowserWidget;
using ::wwal::studio::RecursiveFsWatcher;
using ::wwal::studio::StudioFooterBar;
using ::wwal::studio::VirtualizedGridWidget;
using ::wwal::studio::WorkshopBrowserWidget;

class DockLayoutManager;
class CommandBus;
class IpcClient;
class GpuPreviewViewport;
class CubicBezierEditorWidget;
class PropertyInspectorWidget;
class ThemeStudioWidget;
class MonitorTopologyCanvas;
class MonitorTopologyBar;
class NodeGraphEditorWidget;
class EffectLibraryBrowserWidget;
class CameraLightingInspectorWidget;
class PhysicsSpringInspectorWidget;

/**
 * @brief Startup options parsed from CLI arguments.
 */
struct StudioOptions {
    QString namespaceStr{QStringLiteral("default")};
    QString socketPath;
    QStringList wallpaperDirs;
    QStringList excludeDirs;
    bool benchmarkGrid{false};
    bool headlessCheck{false};
};

/**
 * @brief Main Studio Application Window for WayWal Studio.
 *
 * Features:
 * - Wires all 6 studio dock panels & bars:
 *   - Top: MonitorTopologyBar
 *   - Left Dock: VirtualizedGridWidget
 *   - Center Dock: GpuPreviewViewport + CubicBezierEditorWidget
 *   - Right Dock: PropertyInspectorWidget + ThemeStudioWidget
 *   - Bottom Footer: StudioFooterBar (AudioSpectrumWidget, PowerGovernorWidget, TelemetryHudWidget)
 * - Configures dark pro-studio fusion stylesheet (Blender / DaVinci Resolve aesthetic)
 * - High-DPI fractional scaling (Qt::HighDpiScaleFactorRoundingPolicy::PassThrough for
 * wp_fractional_scale_v1)
 * - Global shortcuts: Ctrl+Z Undo, Ctrl+Y / Ctrl+Shift+Z Redo, Ctrl+F Search Focus, Space
 * Pause/Resume, F11 Fullscreen Preview
 * - Headless verification check execution mode
 */
class MainWindow : public QMainWindow {
    Q_OBJECT

  public:
    explicit MainWindow(const StudioOptions &options = StudioOptions(), QWidget *parent = nullptr);
    ~MainWindow() override;

    DockLayoutManager *dockLayoutManager() const { return m_layoutManager; }
    CommandBus *commandBus() const { return m_commandBus.get(); }
    const StudioOptions &options() const { return m_options; }

    // Direct access to core panels
    MonitorTopologyBar *topologyBar() const { return m_topologyBar; }
    VirtualizedGridWidget *gridWidget() const { return m_gridWidget; }
    GpuPreviewViewport *previewViewport() const { return m_previewViewport; }
    CubicBezierEditorWidget *bezierEditor() const { return m_bezierEditor; }
    PropertyInspectorWidget *propertyInspector() const { return m_propertyInspector; }
    ThemeStudioWidget *themeStudio() const { return m_themeStudio; }
    StudioFooterBar *footerBar() const { return m_footerBar; }
    WorkshopBrowserWidget *workshopBrowser() const { return m_workshopBrowser; }
    OpenHubBrowserWidget *openHubBrowser() const { return m_openHubBrowser; }
    CatalogIndexManager *catalogIndexManager() const { return m_catalogIndexMgr.get(); }
    DownloadManager *downloadManager() const { return m_downloadMgr.get(); }
    RecursiveFsWatcher *fsWatcher() const { return m_fsWatcher.get(); }
    IpcClient *ipcClient() const { return m_ipcClient.get(); }
    NodeGraphEditorWidget *nodeGraphEditor() const { return m_nodeGraphEditor; }
    EffectLibraryBrowserWidget *effectLibraryBrowser() const { return m_effectLibraryBrowser; }
    CameraLightingInspectorWidget *cameraLightingInspector() const
    {
        return m_cameraLightingInspector;
    }
    PhysicsSpringInspectorWidget *physicsSpringInspector() const
    {
        return m_physicsSpringInspector;
    }

    // Global Actions
    QAction *undoAction() const { return m_undoAction; }
    QAction *redoAction() const { return m_redoAction; }
    QAction *searchFocusAction() const { return m_searchFocusAction; }
    QAction *playPauseAction() const { return m_playPauseAction; }
    QAction *fullscreenAction() const { return m_fullscreenAction; }
    QAction *packageWizardAction() const { return m_packageWizardAction; }
    QPushButton *pauseButton() const { return m_pauseBtn; }

  public slots:
    void handleUndo();
    void handleRedo();
    void handleSearchFocus();
    void handlePlayPauseToggle();
    void handleFullscreenToggle();
    void handlePerspectiveSelected(const QString &perspectiveName);
    void handleResetLayout();
    void handleOpenPackagingWizard();

  protected:
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

  private:
    void setupPanels();
    void setupMenuBar();
    void setupToolBar();
    void setupShortcuts();
    void applyProStudioDarkTheme();
    void wireInterPanelSignals();
    void updateCommandBusHistoryUI();
    void applyWallpaperPath(const QString &path);
    void installPackageToDaemon(const QString &packageIdOrPath, const QString &targetOutput);

    StudioOptions m_options;
    DockLayoutManager *m_layoutManager{nullptr};
    std::unique_ptr<CommandBus> m_commandBus;

    // Steam Workshop, Open Hub & Catalog Engine
    std::shared_ptr<CatalogIndexManager> m_catalogIndexMgr;
    std::shared_ptr<DownloadManager> m_downloadMgr;
    std::unique_ptr<RecursiveFsWatcher> m_fsWatcher;
    std::jthread m_reconcileThread;

    // 6+ Studio Dock Panels & Bars
    MonitorTopologyBar *m_topologyBar{nullptr};
    VirtualizedGridWidget *m_gridWidget{nullptr};
    WorkshopBrowserWidget *m_workshopBrowser{nullptr};
    OpenHubBrowserWidget *m_openHubBrowser{nullptr};
    GpuPreviewViewport *m_previewViewport{nullptr};
    CubicBezierEditorWidget *m_bezierEditor{nullptr};
    PropertyInspectorWidget *m_propertyInspector{nullptr};
    ThemeStudioWidget *m_themeStudio{nullptr};
    StudioFooterBar *m_footerBar{nullptr};

    std::unique_ptr<IpcClient> m_ipcClient;
    NodeGraphEditorWidget *m_nodeGraphEditor{nullptr};
    EffectLibraryBrowserWidget *m_effectLibraryBrowser{nullptr};
    CameraLightingInspectorWidget *m_cameraLightingInspector{nullptr};
    PhysicsSpringInspectorWidget *m_physicsSpringInspector{nullptr};

    // Global Actions
    QAction *m_undoAction{nullptr};
    QAction *m_redoAction{nullptr};
    QAction *m_searchFocusAction{nullptr};
    QAction *m_playPauseAction{nullptr};
    QAction *m_fullscreenAction{nullptr};
    QAction *m_packageWizardAction{nullptr};

    // Perspective & Status Controls
    QComboBox *m_perspectiveCombo{nullptr};
    QPushButton *m_pauseBtn{nullptr};
    QLabel *m_statusLabel{nullptr};
    QString m_activeWallpaperPath;

    bool m_isFullscreenPreview{false};
    bool m_isPlaying{true};
    bool m_updatingPerspectiveCombo{false};
};

} // namespace waywal::studio

using waywal::studio::MainWindow;
using waywal::studio::StudioOptions;

#endif // WAYWAL_STUDIO_MAIN_WINDOW_HPP
