// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_MAIN_WINDOW_HPP
#define WAYWAL_STUDIO_MAIN_WINDOW_HPP

#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QAction>
#include <QComboBox>
#include <QLabel>
#include <QPointer>
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
class SceneLayerTreeWidget;
class GpuBrushPainterWidget;
class ParticleDesignerWidget;
class AudioBindingWizard;
class GlslSceneScriptEditorWidget;

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

    // Direct access to core panels (defined out-of-line so QPointer<T> members stay incomplete-type
    // safe)
    MonitorTopologyBar *topologyBar() const;
    VirtualizedGridWidget *gridWidget() const;
    GpuPreviewViewport *previewViewport() const;
    CubicBezierEditorWidget *bezierEditor() const;
    PropertyInspectorWidget *propertyInspector() const;
    ThemeStudioWidget *themeStudio() const;
    StudioFooterBar *footerBar() const;
    WorkshopBrowserWidget *workshopBrowser() const;
    OpenHubBrowserWidget *openHubBrowser() const;
    CatalogIndexManager *catalogIndexManager() const { return m_catalogIndexMgr.get(); }
    DownloadManager *downloadManager() const { return m_downloadMgr.get(); }
    RecursiveFsWatcher *fsWatcher() const { return m_fsWatcher.get(); }
    IpcClient *ipcClient() const { return m_ipcClient.get(); }
    NodeGraphEditorWidget *nodeGraphEditor() const;
    EffectLibraryBrowserWidget *effectLibraryBrowser() const;
    CameraLightingInspectorWidget *cameraLightingInspector() const;
    PhysicsSpringInspectorWidget *physicsSpringInspector() const;
    SceneLayerTreeWidget *sceneLayerTree() const;
    GpuBrushPainterWidget *brushPainter() const;
    ParticleDesignerWidget *particleDesigner() const;
    AudioBindingWizard *audioBindingWizard() const;
    GlslSceneScriptEditorWidget *codeEditor() const;

    // Global Actions
    QAction *undoAction() const { return m_undoAction; }
    QAction *redoAction() const { return m_redoAction; }
    QAction *searchFocusAction() const { return m_searchFocusAction; }
    QAction *playPauseAction() const { return m_playPauseAction; }
    QAction *fullscreenAction() const { return m_fullscreenAction; }
    QAction *packageWizardAction() const { return m_packageWizardAction; }
    QAction *exportScenePackageAction() const { return m_exportScenePkgAction; }
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
    void handleExportScenePackage();
    void handleViewportToolModeChanged(int index);

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
    QPointer<MonitorTopologyBar> m_topologyBar;
    QPointer<VirtualizedGridWidget> m_gridWidget;
    QPointer<WorkshopBrowserWidget> m_workshopBrowser;
    QPointer<OpenHubBrowserWidget> m_openHubBrowser;
    QPointer<GpuPreviewViewport> m_previewViewport;
    QPointer<CubicBezierEditorWidget> m_bezierEditor;
    QPointer<PropertyInspectorWidget> m_propertyInspector;
    QPointer<ThemeStudioWidget> m_themeStudio;
    QPointer<StudioFooterBar> m_footerBar;

    std::unique_ptr<IpcClient> m_ipcClient;
    QPointer<NodeGraphEditorWidget> m_nodeGraphEditor;
    QPointer<EffectLibraryBrowserWidget> m_effectLibraryBrowser;
    QPointer<CameraLightingInspectorWidget> m_cameraLightingInspector;
    QPointer<PhysicsSpringInspectorWidget> m_physicsSpringInspector;
    QPointer<SceneLayerTreeWidget> m_sceneLayerTree;
    QPointer<GpuBrushPainterWidget> m_brushPainter;
    QPointer<ParticleDesignerWidget> m_particleDesigner;
    QPointer<AudioBindingWizard> m_audioBindingWizard;
    QPointer<GlslSceneScriptEditorWidget> m_codeEditor;

    // Global Actions
    QAction *m_undoAction{nullptr};
    QAction *m_redoAction{nullptr};
    QAction *m_searchFocusAction{nullptr};
    QAction *m_playPauseAction{nullptr};
    QAction *m_fullscreenAction{nullptr};
    QAction *m_packageWizardAction{nullptr};
    QAction *m_exportScenePkgAction{nullptr};

    // Perspective & Status Controls
    QComboBox *m_perspectiveCombo{nullptr};
    QComboBox *m_toolModeCombo{nullptr};
    QPushButton *m_pauseBtn{nullptr};
    QLabel *m_statusLabel{nullptr};
    QString m_activeWallpaperPath;
    QString m_lastSceneTreeJson;
    QString m_pendingExportStagePath;
    QString m_pendingExportDestPath;
    bool m_pendingExportIsPkgFile{false};

    bool m_isFullscreenPreview{false};
    bool m_isPlaying{true};
    bool m_updatingPerspectiveCombo{false};
};

} // namespace waywal::studio

namespace wwal::studio {
using MainWindow = waywal::studio::MainWindow;
using StudioOptions = waywal::studio::StudioOptions;
} // namespace wwal::studio

#endif // WAYWAL_STUDIO_MAIN_WINDOW_HPP
