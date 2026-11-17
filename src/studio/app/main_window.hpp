// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_MAIN_WINDOW_HPP
#define WAYWAL_STUDIO_MAIN_WINDOW_HPP

#include "studio/core/rigging_commands.hpp"
#include "studio/scene/scene_layer_tree_widget.hpp"
#include "waywal/ipc_proto_authoring.h"

#include <QPointer>

#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QAction>
#include <QComboBox>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

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
class PresetStore;
class StudioSessionContext;
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
class PuppetMeshPanel;
class BoneRiggingPanel;
class WeightPaintPanel;
class DopeSheetTimelineWidget;

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
    bool realizeAllPanels{false};
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
    CommandBus *commandBus() const;
    PresetStore *presetStore() const;
    StudioSessionContext *sessionContext() const { return m_session.get(); }
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
    CatalogIndexManager *catalogIndexManager() const;
    DownloadManager *downloadManager() const;
    RecursiveFsWatcher *fsWatcher() const;
    IpcClient *ipcClient() const;
    NodeGraphEditorWidget *nodeGraphEditor() const;
    EffectLibraryBrowserWidget *effectLibraryBrowser() const;
    CameraLightingInspectorWidget *cameraLightingInspector() const;
    PhysicsSpringInspectorWidget *physicsSpringInspector() const;
    SceneLayerTreeWidget *sceneLayerTree() const;
    GpuBrushPainterWidget *brushPainter() const;
    ParticleDesignerWidget *particleDesigner() const;
    AudioBindingWizard *audioBindingWizard() const;
    GlslSceneScriptEditorWidget *codeEditor() const;
    PuppetMeshPanel *puppetMeshPanel() const;
    BoneRiggingPanel *boneRiggingPanel() const;
    WeightPaintPanel *weightPaintPanel() const;
    DopeSheetTimelineWidget *dopeSheetTimeline() const;

    // Global Actions
    QAction *undoAction() const { return m_undoAction; }
    QAction *redoAction() const { return m_redoAction; }
    QAction *searchFocusAction() const { return m_searchFocusAction; }
    QAction *playPauseAction() const { return m_playPauseAction; }
    QAction *fullscreenAction() const { return m_fullscreenAction; }
    QAction *packageWizardAction() const { return m_packageWizardAction; }
    QAction *exportScenePackageAction() const { return m_exportScenePkgAction; }
    QPushButton *pauseButton() const { return m_pauseBtn; }

    bool loadPuppetFromMdlFile(const QString &filePath, int32_t objectId = 0);
    void loadExistingPuppetIntoPanels(int32_t objectId, const we_puppet_t *puppet,
                                      uint32_t texWidth = 0u, uint32_t texHeight = 0u);

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
    void applyWallpaperPath(const QString &path, const QString &explicitMonitorSpec = QString());
    void installPackageToDaemon(const QString &packageIdOrPath, const QString &targetOutput);
    [[nodiscard]] QString resolveSelectedOutputsCsv() const;
    void syncMonitorCatalogToPanels();
    void updateGpuShaderEditorContext();
    void syncThemeStateToIpcOrActiveWallpaper(bool includeApplyHint = false);
    void dispatchScenePackageExport(const QString &destPath);
    struct CachedAlphaBuffer {
        uint32_t width{0};
        uint32_t height{0};
        std::vector<uint8_t> alpha;
        bool pushed{false};
    };

    void detachPanelServicePointers() noexcept;
    void setActivePuppetObjectContext(int32_t objectId, uint32_t texWidth, uint32_t texHeight);
    void hydratePuppetMeshPanel(PuppetMeshPanel *panel);
    void hydrateWeightPaintPanel(WeightPaintPanel *panel) const;
    void syncWeightPaintBrushAndLocks(WeightPaintPanel *panel) const;
    void syncSourceAlphaFromPreviewImage(const QImage &image);
    void hydrateDopeSheetTimeline(DopeSheetTimelineWidget *timeline);
    void refreshOnionSkinFrames();
    void wireWorkshopBrowserSignals(WorkshopBrowserWidget *workshop);
    void wireOpenHubBrowserSignals(OpenHubBrowserWidget *openHub);
    void wireThemeStudioSignals(ThemeStudioWidget *theme);
    void wireNodeGraphEditorSignals(NodeGraphEditorWidget *graph);
    void wireEffectLibrarySignals(EffectLibraryBrowserWidget *browser);
    void wireCameraLightingSignals(CameraLightingInspectorWidget *camLights);
    void wirePhysicsSpringSignals(PhysicsSpringInspectorWidget *phys);
    void syncCachedSceneLayersToAudioWizard();

    struct OnionSkinScratch {
        std::vector<std::array<float, 16>> localMats;
        std::vector<std::array<float, 16>> worldMats;
        std::vector<uint8_t> visitState;
        std::vector<size_t> dfsStack;
        std::vector<uint8_t> hasChild;
    };

    StudioOptions m_options;
    DockLayoutManager *m_layoutManager{nullptr};
    std::unique_ptr<StudioSessionContext> m_session;

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

    QPointer<NodeGraphEditorWidget> m_nodeGraphEditor;
    QPointer<EffectLibraryBrowserWidget> m_effectLibraryBrowser;
    QPointer<CameraLightingInspectorWidget> m_cameraLightingInspector;
    QPointer<PhysicsSpringInspectorWidget> m_physicsSpringInspector;
    QPointer<SceneLayerTreeWidget> m_sceneLayerTree;
    QPointer<GpuBrushPainterWidget> m_brushPainter;
    QPointer<ParticleDesignerWidget> m_particleDesigner;
    QPointer<AudioBindingWizard> m_audioBindingWizard;
    QPointer<GlslSceneScriptEditorWidget> m_codeEditor;
    QPointer<PuppetMeshPanel> m_puppetMeshPanel;
    QPointer<BoneRiggingPanel> m_boneRiggingPanel;
    QPointer<WeightPaintPanel> m_weightPaintPanel;
    QPointer<DopeSheetTimelineWidget> m_dopeSheetTimeline;

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
    QPointer<CreatorPackagingWizard> m_activePackagingWizard;
    float m_audioVolume{1.0f};
    bool m_muteOnAudio{false};
    uint8_t m_batteryPolicy{0};

    bool m_isFullscreenPreview{false};
    bool m_isPlaying{true};
    bool m_updatingPerspectiveCombo{false};

    int32_t m_activePuppetObjectId{-1};
    uint32_t m_activePuppetTexWidth{0};
    uint32_t m_activePuppetTexHeight{0};
    int32_t m_timelineHydratedObjectId{-1};
    PuppetMeshSnapshot m_cachedPuppetMesh;
    CachedAlphaBuffer m_cachedPreviewAlpha;
    std::unordered_map<int32_t, CachedAlphaBuffer> m_cachedObjectAlphas;
    std::deque<int32_t> m_pendingAlphaReadbackObjectIds;
    BoneHierarchySnapshot m_cachedPuppetRig;
    bool m_hasCachedPuppetRig{false};
    std::unordered_map<int32_t, std::unordered_map<int32_t, we_bone_drive_mode_t>>
        m_pendingDriveModes;
    std::unordered_map<int32_t, QString> m_pendingMdlExportPaths;
    bool m_syncingBoneDriveMode{false};
    bool m_onionSkinEnabled{false};
    int m_onionBeforeFrames{2};
    int m_onionAfterFrames{2};
    float m_onionOpacity{0.35f};
    std::vector<SceneLayerNode> m_cachedSceneLayers;
    std::vector<we_audio_binding_t> m_cachedAudioBindings;
    std::vector<uint32_t> m_playlistQueue;
    OnionSkinScratch m_onionScratch;
};

} // namespace waywal::studio

namespace wwal::studio {
using MainWindow = waywal::studio::MainWindow;
using StudioOptions = waywal::studio::StudioOptions;
} // namespace wwal::studio

#endif // WAYWAL_STUDIO_MAIN_WINDOW_HPP
