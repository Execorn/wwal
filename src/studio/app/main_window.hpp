// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_MAIN_WINDOW_HPP
#define WAYWAL_STUDIO_MAIN_WINDOW_HPP

#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QAction>
#include <QComboBox>
#include <QLabel>
#include <memory>

namespace wwal::studio {
class VirtualizedGridWidget;
class AudioSpectrumWidget;
class PowerGovernorWidget;
class TelemetryHudWidget;
class StudioFooterBar;
} // namespace wwal::studio

namespace waywal::studio {

using ::wwal::studio::StudioFooterBar;
using ::wwal::studio::VirtualizedGridWidget;

class DockLayoutManager;
class CommandBus;
class GpuPreviewViewport;
class CubicBezierEditorWidget;
class PropertyInspectorWidget;
class ThemeStudioWidget;
class MonitorTopologyCanvas;
class MonitorTopologyBar;

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

    // Global Actions
    QAction *undoAction() const { return m_undoAction; }
    QAction *redoAction() const { return m_redoAction; }
    QAction *searchFocusAction() const { return m_searchFocusAction; }
    QAction *playPauseAction() const { return m_playPauseAction; }
    QAction *fullscreenAction() const { return m_fullscreenAction; }

  public slots:
    void handleUndo();
    void handleRedo();
    void handleSearchFocus();
    void handlePlayPauseToggle();
    void handleFullscreenToggle();
    void handlePerspectiveSelected(const QString &perspectiveName);
    void handleResetLayout();

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

    StudioOptions m_options;
    DockLayoutManager *m_layoutManager{nullptr};
    std::unique_ptr<CommandBus> m_commandBus;

    // 6 Studio Dock Panels & Bars
    MonitorTopologyBar *m_topologyBar{nullptr};
    VirtualizedGridWidget *m_gridWidget{nullptr};
    GpuPreviewViewport *m_previewViewport{nullptr};
    CubicBezierEditorWidget *m_bezierEditor{nullptr};
    PropertyInspectorWidget *m_propertyInspector{nullptr};
    ThemeStudioWidget *m_themeStudio{nullptr};
    StudioFooterBar *m_footerBar{nullptr};

    // Global Actions
    QAction *m_undoAction{nullptr};
    QAction *m_redoAction{nullptr};
    QAction *m_searchFocusAction{nullptr};
    QAction *m_playPauseAction{nullptr};
    QAction *m_fullscreenAction{nullptr};

    // Perspective & Status Controls
    QComboBox *m_perspectiveCombo{nullptr};
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
