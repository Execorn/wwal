// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/app/main_window.hpp"
#include "studio/app/dock_layout_manager.hpp"

#include <QApplication>
#include <QMenuBar>
#include <QMenu>
#include <QToolBar>
#include <QStatusBar>
#include <QKeySequence>
#include <QShortcut>
#include <QStyleFactory>
#include <QPalette>
#include <QColor>
#include <QSplitter>
#include <QTabWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QDebug>
#include <QTimer>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QStandardPaths>

// Include Core Services & CommandBus
#if __has_include("studio/core/command_bus.hpp")
#include "studio/core/command_bus.hpp"
#elif __has_include("core/command_bus.hpp")
#include "core/command_bus.hpp"
#endif

// Include Monitor Topology Canvas
#if __has_include("studio/topology/monitor_topology_bar.hpp")
#include "studio/topology/monitor_topology_bar.hpp"
#elif __has_include("topology/monitor_topology_bar.hpp")
#include "topology/monitor_topology_bar.hpp"
#else
#include "studio/topology/monitor_topology_canvas.hpp"
namespace waywal::studio {
class MonitorTopologyBar : public QWidget {
  public:
    explicit MonitorTopologyBar(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("MonitorTopologyBar"));
        setMinimumHeight(74);

        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(6, 4, 6, 4);
        layout->setSpacing(8);

        m_canvas = new MonitorTopologyCanvas(this);
        layout->addWidget(m_canvas, 1);

        auto *controlsBox = new QWidget(this);
        controlsBox->setFixedWidth(136);
        auto *ctrlLayout = new QVBoxLayout(controlsBox);
        ctrlLayout->setContentsMargins(0, 0, 0, 0);
        ctrlLayout->setSpacing(4);

        auto *btnRefresh = new QPushButton(QStringLiteral("↻ Refresh Outputs"), controlsBox);
        btnRefresh->setToolTip(QStringLiteral("Refresh connected Wayland outputs"));
        btnRefresh->setFixedHeight(22);
        connect(btnRefresh, &QPushButton::clicked, this, [this]() {
            if (m_canvas)
                m_canvas->refreshFromWayland();
        });
        ctrlLayout->addWidget(btnRefresh);

        auto *btnSpan = new QPushButton(QStringLiteral("Span Displays"), controlsBox);
        btnSpan->setCheckable(true);
        btnSpan->setFixedHeight(22);
        btnSpan->setToolTip(
            QStringLiteral("Calculate and apply seamless multi-display panoramic span"));
        connect(btnSpan, &QPushButton::toggled, this, [this](bool checked) {
            if (m_canvas)
                m_canvas->setDesktopSpanMode(checked);
        });
        ctrlLayout->addWidget(btnSpan);

        auto *scaleRow = new QHBoxLayout();
        scaleRow->setContentsMargins(0, 0, 0, 0);
        scaleRow->setSpacing(4);
        auto *scaleLbl = new QLabel(QStringLiteral("Scale:"), controlsBox);
        scaleLbl->setStyleSheet(QStringLiteral("color: #9a9a9e; font-size: 10px;"));
        scaleRow->addWidget(scaleLbl);

        auto *comboScaling = new QComboBox(controlsBox);
        comboScaling->setFixedHeight(22);
        comboScaling->addItems({QStringLiteral("Fill"), QStringLiteral("Fit"),
                                QStringLiteral("Center"), QStringLiteral("Stretch"),
                                QStringLiteral("Tile")});
        connect(comboScaling, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int idx) {
                    if (m_canvas)
                        m_canvas->setScalingMode(idx);
                });
        scaleRow->addWidget(comboScaling, 1);
        ctrlLayout->addLayout(scaleRow);
        ctrlLayout->addStretch(1);

        layout->addWidget(controlsBox);
    }

    MonitorTopologyCanvas *canvas() const { return m_canvas; }

  private:
    MonitorTopologyCanvas *m_canvas{nullptr};
};
} // namespace waywal::studio
#endif

// Include Virtualized Grid Widget
#if __has_include("studio/grid/virtualized_grid_widget.hpp")
#include "studio/grid/virtualized_grid_widget.hpp"
#elif __has_include("grid/virtualized_grid_widget.hpp")
#include "grid/virtualized_grid_widget.hpp"
#endif

// Include GPU Preview Viewport
#include "studio/viewport/gpu_preview_viewport.hpp"

// Include Cubic Bezier Editor
#include "studio/topology/cubic_bezier_editor.hpp"

// Include Property Inspector
#include "studio/inspector/property_inspector_widget.hpp"

// Include Theme Studio Widget
#if __has_include("studio/theme/theme_studio_widget.hpp")
#include "studio/theme/theme_studio_widget.hpp"
#elif __has_include("theme/theme_studio_widget.hpp")
#include "theme/theme_studio_widget.hpp"
#endif

// Include Footer Components
#if __has_include("studio/footer/studio_footer_bar.hpp")
#include "studio/footer/studio_footer_bar.hpp"
#elif __has_include("footer/studio_footer_bar.hpp")
#include "footer/studio_footer_bar.hpp"
#endif

// Include Workshop, Open Hub & Catalog Subsystems
#include "studio/core/catalog_index_manager.hpp"
#include "studio/core/recursive_fs_watcher.hpp"
#include "studio/queue/download_manager.hpp"
#include "studio/dock/workshop_browser_widget.hpp"
#include "studio/dock/openhub_browser_widget.hpp"
#include "studio/wizard/creator_packaging_wizard.hpp"
#include "waywal/we_vdf.h"

namespace waywal::studio {

/* ========================================================================= */
/* MainWindow Implementation                                                 */
/* ========================================================================= */

MainWindow::MainWindow(const StudioOptions &options, QWidget *parent)
    : QMainWindow(parent), m_options(options)
{
    // 1. Configure Window Properties
    setObjectName(QStringLiteral("WayWalStudioMainWindow"));
    setWindowTitle(QStringLiteral("WayWal Studio — Linux Wayland Wallpaper IDE"));
    setMinimumSize(860, 600);
    resize(1600, 960);

    // 2. Setup Central Docking Architecture
    m_layoutManager = new DockLayoutManager(this, this);

    // 3. Initialize CommandBus with 1,000-Step History & Drag Coalescing
    m_commandBus = std::make_unique<CommandBus>(1000, this);

    // 4. Create and Wire all 6 Panels & Bars
    setupPanels();

    // 5. Construct Menus, Toolbars, and Shortcuts
    setupMenuBar();
    setupToolBar();
    setupShortcuts();

    // 6. Apply Blender / DaVinci Resolve Dark Pro-Studio Styling
    applyProStudioDarkTheme();

    // 7. Wire Inter-Panel Signals & Slots
    wireInterPanelSignals();

    // 8. Connect CommandBus Signals
    connect(m_commandBus.get(), &CommandBus::historyChanged, this,
            [this](bool canUndo, bool canRedo, const QString &undoText, const QString &redoText) {
                if (m_undoAction) {
                    m_undoAction->setEnabled(canUndo);
                    m_undoAction->setText(canUndo ? QStringLiteral("Undo %1").arg(undoText)
                                                  : QStringLiteral("Undo"));
                }
                if (m_redoAction) {
                    m_redoAction->setEnabled(canRedo);
                    m_redoAction->setText(canRedo ? QStringLiteral("Redo %1").arg(redoText)
                                                  : QStringLiteral("Redo"));
                }
            });

    // 9. Synchronize Perspective Switcher
    connect(m_layoutManager, &DockLayoutManager::perspectiveChanged, this,
            [this](const QString &name) {
                if (m_perspectiveCombo && !m_updatingPerspectiveCombo) {
                    m_updatingPerspectiveCombo = true;
                    int idx = m_perspectiveCombo->findText(name);
                    if (idx >= 0) {
                        m_perspectiveCombo->setCurrentIndex(idx);
                    }
                    m_updatingPerspectiveCombo = false;
                }
            });

    // 10. Load Layout Persistence from $XDG_CONFIG_HOME/wwal/studio_layout.json
    if (!m_options.headlessCheck && !m_options.benchmarkGrid) {
        m_layoutManager->loadLayoutFromFile();
    } else {
        m_layoutManager->resetToDefaultPerspective();
    }

    // 11. Scan wallpaper catalog asynchronously in interactive mode
    if (!m_options.headlessCheck && !m_options.benchmarkGrid && m_gridWidget) {
        QTimer::singleShot(0, this, [this]() {
            if (m_gridWidget) {
                auto &idx = m_gridWidget->catalogIndex();
                (void)idx.loadConfigFromFile();
                if (!m_options.wallpaperDirs.isEmpty()) {
                    std::vector<std::string> dirs;
                    dirs.reserve(static_cast<size_t>(m_options.wallpaperDirs.size()));
                    for (const auto &d : m_options.wallpaperDirs) {
                        dirs.push_back(d.toStdString());
                    }
                    idx.setIncludedDirectories(dirs);
                    idx.setScanSystemAndWorkshopPaths(false);
                }
                if (!m_options.excludeDirs.isEmpty()) {
                    for (const auto &ex : m_options.excludeDirs) {
                        idx.addExcludedDirectory(ex.toStdString());
                    }
                }
                m_gridWidget->scanWallpapers();
                m_gridWidget->populateSampleCatalogIfEmpty();
                m_gridWidget->selectFirstItemIfAvailable();
            }
        });
    }

    // 12. Setup Recursive Filesystem Watcher for Steam Workshop & Open Hub
    if (!m_options.headlessCheck && !m_options.benchmarkGrid) {
        m_fsWatcher =
            std::make_unique<RecursiveFsWatcher>([this](const std::vector<FsWatchEvent> &events) {
                if (events.empty())
                    return;
                QMetaObject::invokeMethod(
                    this,
                    [this]() {
                        if (m_workshopBrowser) {
                            m_workshopBrowser->refreshLibrary();
                        }
                        if (m_openHubBrowser) {
                            m_openHubBrowser->refreshFeed();
                        }
                    },
                    Qt::QueuedConnection);
            });

        waywal_steam_library_t libs[16];
        size_t libCount = waywal_steam_discover_libraries(libs, 16);
        for (size_t i = 0; i < libCount; ++i) {
            std::string wsDir = std::string(libs[i].path) + "/steamapps/workshop/content/431960";
            if (std::filesystem::exists(wsDir)) {
                m_fsWatcher->addWatchRoot(wsDir);
            }
        }
        QString localHub = QDir::homePath() + QStringLiteral("/.local/share/waywal/hub");
        QDir().mkpath(localHub);
        m_fsWatcher->addWatchRoot(localHub.toStdString());
        m_fsWatcher->start();
    }
}

MainWindow::~MainWindow() = default;

void MainWindow::setupPanels()
{
    // Panel 1 (Top Bar): Monitor Topology & Span Mode
    m_topologyBar = new MonitorTopologyBar(this);
    m_layoutManager->registerPanel(QStringLiteral("MonitorTopologyBar"), m_topologyBar,
                                   QStringLiteral("Monitor Topology & Span"),
                                   Qt::TopDockWidgetArea);

    // Catalog & Download Managers
    m_catalogIndexMgr = std::make_shared<CatalogIndexManager>();
    m_downloadMgr = std::make_shared<DownloadManager>();

    // Panel 2 (Left Dock Tab 1): Virtualized Wallpaper Grid
    m_gridWidget = new VirtualizedGridWidget(this);
    m_layoutManager->registerPanel(QStringLiteral("Wallpapers"), m_gridWidget,
                                   QStringLiteral("Wallpaper Library"), Qt::LeftDockWidgetArea);

    // Panel 2 (Left Dock Tab 2): Steam Workshop Browser
    m_workshopBrowser = new WorkshopBrowserWidget(m_catalogIndexMgr, m_downloadMgr, this);
    m_layoutManager->registerPanel(QStringLiteral("WorkshopBrowser"), m_workshopBrowser,
                                   QStringLiteral("Steam Workshop"), Qt::LeftDockWidgetArea);

    // Panel 2 (Left Dock Tab 3): Open Hub Browser
    m_openHubBrowser = new OpenHubBrowserWidget(m_downloadMgr, this);
    m_layoutManager->registerPanel(QStringLiteral("OpenHubBrowser"), m_openHubBrowser,
                                   QStringLiteral("Open Hub"), Qt::LeftDockWidgetArea);

    // Panel 3 (Center Dock Top): GPU Preview Viewport
    m_previewViewport = new GpuPreviewViewport(this);
    m_layoutManager->registerPanel(QStringLiteral("GpuPreviewViewport"), m_previewViewport,
                                   QStringLiteral("GPU Viewport Preview"), Qt::RightDockWidgetArea);

    // Panel 4 (Center Dock Bottom): Transition Cubic Bézier Editor
    m_bezierEditor = new CubicBezierEditorWidget(this);
    m_bezierEditor->setCommandBus(m_commandBus.get());
    m_layoutManager->registerPanel(QStringLiteral("CubicBezier"), m_bezierEditor,
                                   QStringLiteral("Transition Bézier Curves"),
                                   Qt::RightDockWidgetArea);

    // Panel 5 (Right Dock Tab 1): Live Scene & Shader Inspector
    m_propertyInspector = new PropertyInspectorWidget(this);
    m_propertyInspector->setCommandBus(m_commandBus.get());
    m_layoutManager->registerPanel(QStringLiteral("PropertyInspectorWidget"), m_propertyInspector,
                                   QStringLiteral("Shader & Scene Inspector"),
                                   Qt::RightDockWidgetArea);

    // Panel 5 (Right Dock Tab 2): Material You Theme Studio
    m_themeStudio = new ThemeStudioWidget(this);
    m_layoutManager->registerPanel(QStringLiteral("ThemeStudioWidget"), m_themeStudio,
                                   QStringLiteral("Material You Theme Studio"),
                                   Qt::RightDockWidgetArea);

    // Panel 6 (Bottom Footer): Audio FFT Spectrum, Power Governor & Telemetry HUD
    m_footerBar = new StudioFooterBar(this);
    m_layoutManager->registerPanel(QStringLiteral("StudioFooterBar"), m_footerBar,
                                   QStringLiteral("Studio Footer & Telemetry"),
                                   Qt::BottomDockWidgetArea);

    // Remove standard status bar to let custom FooterBar shine
    setStatusBar(nullptr);
}

void MainWindow::setupMenuBar()
{
    auto *mb = menuBar();

    // --- File Menu ---
    auto *fileMenu = mb->addMenu(QStringLiteral("&File"));

    auto *openWallpaperAction = fileMenu->addAction(QStringLiteral("Open &Wallpaper..."));
    openWallpaperAction->setShortcut(QKeySequence::Open);
    connect(openWallpaperAction, &QAction::triggered, this, [this]() {
        QString file = QFileDialog::getOpenFileName(
            this, QStringLiteral("Open Wallpaper"), QString(),
            QStringLiteral("Wallpaper Media (*.png *.jpg *.jpeg *.webp *.jxl *.avif *.svg *.mp4 "
                           "*.webm *.gif *.glsl *.frag);;All Files (*)"));
        if (!file.isEmpty()) {
            m_activeWallpaperPath = file;
            const QString ext = QFileInfo(file).suffix().toLower();
            const bool isShader = (ext == QStringLiteral("glsl") || ext == QStringLiteral("frag") ||
                                   ext == QStringLiteral("comp"));
            if (m_previewViewport) {
                if (isShader) {
                    QFile f(file);
                    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                        m_previewViewport->setPreviewShader(QString::fromUtf8(f.readAll()));
                    }
                } else {
                    m_previewViewport->setPreviewImagePath(file);
                }
            }
            if (isShader && m_propertyInspector) {
                m_propertyInspector->loadShaderFile(file);
            }
            if (!isShader && m_themeStudio) {
                if (m_previewViewport && !m_previewViewport->previewImage().isNull()) {
                    m_themeStudio->setWallpaperImage(m_previewViewport->previewImage(), file);
                } else {
                    m_themeStudio->loadWallpaper(file);
                }
            }
            if (m_statusLabel) {
                m_statusLabel->setText(QStringLiteral("Loaded: %1").arg(file));
            }
        }
    });

    auto *folderSettingsAction =
        fileMenu->addAction(QStringLiteral("Wallpaper &Folders && Exclusions..."));
    folderSettingsAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    connect(folderSettingsAction, &QAction::triggered, this, [this]() {
        if (m_gridWidget) {
            m_gridWidget->openFolderSettingsDialog();
        }
    });

    auto *openShaderAction = fileMenu->addAction(QStringLiteral("Open &Shader..."));
    openShaderAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+O")));
    connect(openShaderAction, &QAction::triggered, this, [this]() {
        QString file = QFileDialog::getOpenFileName(
            this, QStringLiteral("Open Shadertoy GLSL Shader"), QString(),
            QStringLiteral("GLSL Compute / Fragment (*.glsl *.frag *.comp);;All Files (*)"));
        if (!file.isEmpty()) {
            m_activeWallpaperPath = file;
            if (m_previewViewport) {
                QFile f(file);
                if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    m_previewViewport->setPreviewShader(QString::fromUtf8(f.readAll()));
                }
            }
            if (m_propertyInspector) {
                m_propertyInspector->loadShaderFile(file);
            }
            if (m_statusLabel) {
                m_statusLabel->setText(QStringLiteral("Loaded Shader: %1").arg(file));
            }
        }
    });

    fileMenu->addSeparator();

    auto *savePresetAction = fileMenu->addAction(QStringLiteral("&Save Preset..."));
    savePresetAction->setShortcut(QKeySequence::Save);
    connect(savePresetAction, &QAction::triggered, this, [this]() {
        if (m_propertyInspector) {
            m_propertyInspector->savePreset();
        }
    });

    fileMenu->addSeparator();

    m_packageWizardAction =
        fileMenu->addAction(QStringLiteral("📦 &Package Wallpaper (.wwpkg)..."));
    m_packageWizardAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+P")));
    connect(m_packageWizardAction, &QAction::triggered, this,
            &MainWindow::handleOpenPackagingWizard);

    fileMenu->addSeparator();

    auto *exitAction = fileMenu->addAction(QStringLiteral("E&xit"));
    exitAction->setShortcut(QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, this, &QWidget::close);

    // --- Edit Menu ---
    auto *editMenu = mb->addMenu(QStringLiteral("&Edit"));

    m_undoAction = editMenu->addAction(QStringLiteral("&Undo"));
    m_undoAction->setShortcut(QKeySequence::Undo);
    m_undoAction->setEnabled(false);
    connect(m_undoAction, &QAction::triggered, this, &MainWindow::handleUndo);

    m_redoAction = editMenu->addAction(QStringLiteral("&Redo"));
    m_redoAction->setShortcut(QKeySequence::Redo);
    m_redoAction->setEnabled(false);
    connect(m_redoAction, &QAction::triggered, this, &MainWindow::handleRedo);

    editMenu->addSeparator();

    m_searchFocusAction = editMenu->addAction(QStringLiteral("&Search Focus"));
    m_searchFocusAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+F")));
    connect(m_searchFocusAction, &QAction::triggered, this, &MainWindow::handleSearchFocus);

    // --- View Menu ---
    auto *viewMenu = mb->addMenu(QStringLiteral("&View"));

    auto *perspsMenu = viewMenu->addMenu(QStringLiteral("&Perspectives"));

    auto *defaultPerspAction = perspsMenu->addAction(QStringLiteral("&1 Default"));
    defaultPerspAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+1")));
    connect(defaultPerspAction, &QAction::triggered, this,
            [this]() { handlePerspectiveSelected(QStringLiteral("Default")); });

    auto *shaderPerspAction = perspsMenu->addAction(QStringLiteral("&2 Shader Coding"));
    shaderPerspAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+2")));
    connect(shaderPerspAction, &QAction::triggered, this,
            [this]() { handlePerspectiveSelected(QStringLiteral("Shader Coding")); });

    auto *themePerspAction = perspsMenu->addAction(QStringLiteral("&3 Theme Studio"));
    themePerspAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+3")));
    connect(themePerspAction, &QAction::triggered, this,
            [this]() { handlePerspectiveSelected(QStringLiteral("Theme Studio")); });

    auto *monitorPerspAction = perspsMenu->addAction(QStringLiteral("&4 Multi-Monitor"));
    monitorPerspAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+4")));
    connect(monitorPerspAction, &QAction::triggered, this,
            [this]() { handlePerspectiveSelected(QStringLiteral("Multi-Monitor")); });

    perspsMenu->addSeparator();

    auto *resetLayoutAction = perspsMenu->addAction(QStringLiteral("&Reset Layout to Default"));
    connect(resetLayoutAction, &QAction::triggered, this, &MainWindow::handleResetLayout);

    viewMenu->addSeparator();

    // Toggle panels
    auto *panelsMenu = viewMenu->addMenu(QStringLiteral("&Panels"));
    const QStringList pIds = m_layoutManager->registeredPanelIds();
    for (const auto &id : pIds) {
        auto *dock = m_layoutManager->dockWidget(id);
        if (dock) {
            auto *toggleAction = panelsMenu->addAction(dock->windowTitle());
            toggleAction->setCheckable(true);
            toggleAction->setChecked(!dock->isHidden());
            connect(toggleAction, &QAction::triggered, this,
                    [this, id](bool checked) { m_layoutManager->setPanelVisible(id, checked); });
            connect(m_layoutManager, &DockLayoutManager::panelVisibilityChanged, toggleAction,
                    [toggleAction, id](const QString &changedId, bool visible) {
                        if (changedId == id) {
                            toggleAction->setChecked(visible);
                        }
                    });
        }
    }

    viewMenu->addSeparator();

    m_fullscreenAction = viewMenu->addAction(QStringLiteral("&Fullscreen Preview"));
    m_fullscreenAction->setShortcut(QKeySequence(Qt::Key_F11));
    connect(m_fullscreenAction, &QAction::triggered, this, &MainWindow::handleFullscreenToggle);

    // --- Playback / Daemon Menu ---
    auto *playMenu = mb->addMenu(QStringLiteral("&Playback"));

    m_playPauseAction = playMenu->addAction(QStringLiteral("&Pause / Resume"));
    m_playPauseAction->setShortcut(QKeySequence(Qt::Key_Space));
    connect(m_playPauseAction, &QAction::triggered, this, &MainWindow::handlePlayPauseToggle);

    // --- Help Menu ---
    auto *helpMenu = mb->addMenu(QStringLiteral("&Help"));
    auto *aboutAction = helpMenu->addAction(QStringLiteral("&About WayWal Studio"));
    connect(aboutAction, &QAction::triggered, this, [this]() {
        QMessageBox::about(
            this, QStringLiteral("About WayWal Studio"),
            QStringLiteral("<b>WayWal Studio</b> v1.0.0<br>"
                           "Linux Wayland Creative Wallpaper IDE & Live Pipeline.<br><br>"
                           "Built with C++23, Qt 6, GLES 3.1 compute shaders, and zero-copy "
                           "DMA-BUF scanout."));
    });
}

void MainWindow::setupToolBar()
{
    auto *toolbar = addToolBar(QStringLiteral("Main Studio Controls"));
    toolbar->setObjectName(QStringLiteral("MainStudioToolBar"));
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(18, 18));

    // Logo / Branding
    auto *brandLabel = new QLabel(QStringLiteral(" ✦ WayWal Studio "), this);
    brandLabel->setStyleSheet(QStringLiteral("font-weight: 700; color: #e58824; font-size: 13px;"));
    toolbar->addWidget(brandLabel);

    toolbar->addSeparator();

    // Perspective Selector
    auto *perspLabel = new QLabel(QStringLiteral(" Perspective: "), this);
    perspLabel->setStyleSheet(QStringLiteral("color: #aaaaaa; font-size: 11px;"));
    toolbar->addWidget(perspLabel);

    m_perspectiveCombo = new QComboBox(this);
    m_perspectiveCombo->setObjectName(QStringLiteral("PerspectiveComboBox"));
    m_perspectiveCombo->setFixedWidth(150);
    m_perspectiveCombo->addItems({QStringLiteral("Default"), QStringLiteral("Shader Coding"),
                                  QStringLiteral("Theme Studio"), QStringLiteral("Multi-Monitor")});
    connect(m_perspectiveCombo, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        if (!m_updatingPerspectiveCombo) {
            handlePerspectiveSelected(text);
        }
    });
    toolbar->addWidget(m_perspectiveCombo);

    toolbar->addSeparator();

    // Play/Pause button
    auto *btnPlayPause = new QPushButton(QStringLiteral("⏸ Pause"), this);
    btnPlayPause->setToolTip(QStringLiteral("Pause / Resume animation & live preview [Space]"));
    btnPlayPause->setFixedHeight(24);
    connect(btnPlayPause, &QPushButton::clicked, this, [this, btnPlayPause]() {
        handlePlayPauseToggle();
        btnPlayPause->setText(m_isPlaying ? QStringLiteral("⏸ Pause") : QStringLiteral("▶ Resume"));
    });
    toolbar->addWidget(btnPlayPause);

    toolbar->addSeparator();

    // Fullscreen Preview button
    auto *btnFullscreen = new QPushButton(QStringLiteral("⛶ Fullscreen"), this);
    btnFullscreen->setToolTip(QStringLiteral("Toggle Fullscreen Preview [F11]"));
    btnFullscreen->setFixedHeight(24);
    connect(btnFullscreen, &QPushButton::clicked, this, &MainWindow::handleFullscreenToggle);
    toolbar->addWidget(btnFullscreen);

    toolbar->addSeparator();

    // Search focus button
    auto *btnSearch = new QPushButton(QStringLiteral("🔍 Find"), this);
    btnSearch->setToolTip(QStringLiteral("Focus wallpaper search box [Ctrl+F]"));
    btnSearch->setFixedHeight(24);
    connect(btnSearch, &QPushButton::clicked, this, &MainWindow::handleSearchFocus);
    toolbar->addWidget(btnSearch);

    toolbar->addSeparator();

    // Creator Packaging Wizard button
    auto *btnWizard = new QPushButton(QStringLiteral("📦 Package"), this);
    btnWizard->setToolTip(QStringLiteral("Open Creator Packaging Wizard (.wwpkg) [Ctrl+Shift+P]"));
    btnWizard->setFixedHeight(24);
    connect(btnWizard, &QPushButton::clicked, this, &MainWindow::handleOpenPackagingWizard);
    toolbar->addWidget(btnWizard);

    // Spacer
    auto *spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    // Status label
    m_statusLabel = new QLabel(QStringLiteral("Wayland Client Ready • 144 Hz"), this);
    m_statusLabel->setStyleSheet(
        QStringLiteral("color: #888888; font-size: 11px; padding-right: 8px;"));
    toolbar->addWidget(m_statusLabel);
}

void MainWindow::setupShortcuts()
{
    // Secondary Redo Shortcut: Ctrl+Shift+Z
    auto *redoSecondary = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Z")), this);
    connect(redoSecondary, &QShortcut::activated, this, &MainWindow::handleRedo);
}

void MainWindow::applyProStudioDarkTheme()
{
    // 1. Fusion Pro Dark Palette
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(0x1e, 0x1e, 0x1e));
    palette.setColor(QPalette::WindowText, QColor(0xe0, 0xe0, 0xe0));
    palette.setColor(QPalette::Base, QColor(0x16, 0x16, 0x16));
    palette.setColor(QPalette::AlternateBase, QColor(0x24, 0x24, 0x24));
    palette.setColor(QPalette::ToolTipBase, QColor(0x28, 0x28, 0x28));
    palette.setColor(QPalette::ToolTipText, QColor(0xe0, 0xe0, 0xe0));
    palette.setColor(QPalette::Text, QColor(0xe0, 0xe0, 0xe0));
    palette.setColor(QPalette::Button, QColor(0x2b, 0x2b, 0x2b));
    palette.setColor(QPalette::ButtonText, QColor(0xe0, 0xe0, 0xe0));
    palette.setColor(QPalette::BrightText, QColor(0xff, 0xff, 0xff));
    palette.setColor(QPalette::Highlight, QColor(0xe5, 0x88, 0x24)); // Warm DaVinci / Blender Amber
    palette.setColor(QPalette::HighlightedText, QColor(0x18, 0x18, 0x18));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(0x55, 0x55, 0x55));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x55, 0x55, 0x55));
    QApplication::setPalette(palette);

    // 2. Comprehensive Blender / DaVinci Resolve Stylesheet
    setStyleSheet(
        QStringLiteral("QMainWindow {"
                       "  background: #1e1e1e;"
                       "}"
                       "QWidget {"
                       "  color: #e0e0e0;"
                       "  font-family: 'Inter', 'Cantarell', 'Segoe UI', sans-serif;"
                       "  font-size: 11px;"
                       "}"
                       "QMenuBar {"
                       "  background: #1a1a1a;"
                       "  border-bottom: 1px solid #2a2a2a;"
                       "  padding: 2px 4px;"
                       "}"
                       "QMenuBar::item {"
                       "  padding: 4px 10px;"
                       "  background: transparent;"
                       "  border-radius: 2px;"
                       "}"
                       "QMenuBar::item:selected {"
                       "  background: #333333;"
                       "  color: #ffffff;"
                       "}"
                       "QMenu {"
                       "  background: #242424;"
                       "  border: 1px solid #383838;"
                       "  padding: 4px 0px;"
                       "}"
                       "QMenu::item {"
                       "  padding: 5px 24px 5px 20px;"
                       "}"
                       "QMenu::item:selected {"
                       "  background: #e58824;"
                       "  color: #181818;"
                       "  font-weight: bold;"
                       "}"
                       "QMenu::separator {"
                       "  height: 1px;"
                       "  background: #333333;"
                       "  margin: 4px 0px;"
                       "}"
                       "QToolBar {"
                       "  background: #202020;"
                       "  border-bottom: 1px solid #2a2a2a;"
                       "  spacing: 6px;"
                       "  padding: 3px 8px;"
                       "}"
                       "QTabBar::tab {"
                       "  background: #1a1a1a;"
                       "  color: #999999;"
                       "  padding: 5px 14px;"
                       "  border: none;"
                       "  border-top-left-radius: 3px;"
                       "  border-top-right-radius: 3px;"
                       "  margin-right: 2px;"
                       "}"
                       "QTabBar::tab:selected {"
                       "  background: #262626;"
                       "  color: #ffffff;"
                       "  border-bottom: 2px solid #e58824;"
                       "  font-weight: 600;"
                       "}"
                       "QTabBar::tab:hover:!selected {"
                       "  background: #222222;"
                       "  color: #cccccc;"
                       "}"
                       "QDockWidget {"
                       "  titlebar-close-icon: none;"
                       "  titlebar-normal-icon: none;"
                       "  border: 1px solid #2a2a2a;"
                       "}"
                       "QSplitter::handle {"
                       "  background: #202020;"
                       "}"
                       "QSplitter::handle:hover {"
                       "  background: #e58824;"
                       "}"
                       "QPushButton, QToolButton {"
                       "  background: #2d2d2d;"
                       "  border: 1px solid #3d3d3d;"
                       "  border-radius: 3px;"
                       "  padding: 4px 10px;"
                       "  color: #e0e0e0;"
                       "  font-weight: 500;"
                       "}"
                       "QPushButton:hover, QToolButton:hover {"
                       "  background: #383838;"
                       "  border-color: #e58824;"
                       "  color: #ffffff;"
                       "}"
                       "QPushButton:pressed, QToolButton:pressed {"
                       "  background: #1f1f1f;"
                       "}"
                       "QPushButton:checked {"
                       "  background: #e58824;"
                       "  color: #181818;"
                       "  font-weight: 600;"
                       "  border-color: #f08d49;"
                       "}"
                       "QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {"
                       "  background: #181818;"
                       "  border: 1px solid #383838;"
                       "  border-radius: 3px;"
                       "  padding: 3px 6px;"
                       "  color: #e0e0e0;"
                       "}"
                       "QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {"
                       "  border: 1px solid #e58824;"
                       "}"
                       "QScrollBar:vertical {"
                       "  background: transparent;"
                       "  width: 8px;"
                       "  margin: 0px;"
                       "}"
                       "QScrollBar::handle:vertical {"
                       "  background: #383838;"
                       "  min-height: 24px;"
                       "  border-radius: 4px;"
                       "}"
                       "QScrollBar::handle:vertical:hover {"
                       "  background: #555555;"
                       "}"
                       "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
                       "  height: 0px;"
                       "}"
                       "QSlider::groove:horizontal {"
                       "  height: 4px;"
                       "  background: #2a2a2a;"
                       "  border-radius: 2px;"
                       "}"
                       "QSlider::sub-page:horizontal {"
                       "  background: #e58824;"
                       "  border-radius: 2px;"
                       "}"
                       "QSlider::handle:horizontal {"
                       "  width: 12px;"
                       "  margin: -4px 0;"
                       "  background: #f08d49;"
                       "  border-radius: 6px;"
                       "}"
                       "QSlider::handle:horizontal:hover {"
                       "  background: #ffa666;"
                       "}"));
}

void MainWindow::wireInterPanelSignals()
{
    // 1. Connect VirtualizedGrid wallpaper selection to Preview Viewport, Inspector & ThemeStudio
    if (m_gridWidget) {
        connect(m_gridWidget, &VirtualizedGridWidget::wallpaperSelected, this,
                [this](const wwal::studio::CatalogItem &item) {
                    QString path = QString::fromStdString(item.path);
                    m_activeWallpaperPath = path;
                    if (m_statusLabel) {
                        m_statusLabel->setText(QStringLiteral("Selected: %1").arg(path));
                    }
                    if (m_previewViewport) {
                        if (item.badge == wwal::studio::WallpaperBadge::Shader) {
                            QFile f(path);
                            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                                m_previewViewport->setPreviewShader(QString::fromUtf8(f.readAll()));
                            } else {
                                m_previewViewport->setPreviewImagePath(path);
                            }
                        } else {
                            bool loaded = false;
                            if (!item.preview_path.empty()) {
                                loaded = m_previewViewport->setPreviewImagePath(
                                    QString::fromStdString(item.preview_path));
                            }
                            if (!loaded) {
                                m_previewViewport->setPreviewImagePath(path);
                            }
                        }
                    }
                    if (m_propertyInspector) {
                        if (item.badge == wwal::studio::WallpaperBadge::Shader) {
                            m_propertyInspector->loadShaderFile(path);
                        } else if (item.badge == wwal::studio::WallpaperBadge::Scene) {
                            m_propertyInspector->loadWallpaperEngineDir(
                                QFileInfo(path).absolutePath());
                        }
                    }
                    if (m_themeStudio) {
                        QString previewPath = !item.preview_path.empty()
                                                  ? QString::fromStdString(item.preview_path)
                                                  : path;
                        if (m_previewViewport && !m_previewViewport->previewImage().isNull() &&
                            item.badge != wwal::studio::WallpaperBadge::Shader) {
                            m_themeStudio->setWallpaperImage(m_previewViewport->previewImage(),
                                                             previewPath);
                        } else {
                            m_themeStudio->loadWallpaper(previewPath);
                        }
                    }
                });

        connect(m_gridWidget, &VirtualizedGridWidget::wallpaperDoubleClicked, this,
                [this](const wwal::studio::CatalogItem &item) {
                    applyWallpaperPath(QString::fromStdString(item.path));
                });
    }

    if (m_previewViewport) {
        connect(m_previewViewport, &GpuPreviewViewport::applyRequested, this, [this]() {
            if (!m_activeWallpaperPath.isEmpty()) {
                applyWallpaperPath(m_activeWallpaperPath);
            }
        });
    }

    // 2. Connect Cubic Bezier easing changes
    if (m_bezierEditor) {
        connect(m_bezierEditor, &CubicBezierEditorWidget::curveChanged, this,
                [this](float p1x, float p1y, float p2x, float p2y) {
                    if (m_statusLabel) {
                        m_statusLabel->setText(QStringLiteral("Easing Curve: [%1, %2, %3, %4]")
                                                   .arg(p1x, 0, 'f', 2)
                                                   .arg(p1y, 0, 'f', 2)
                                                   .arg(p2x, 0, 'f', 2)
                                                   .arg(p2y, 0, 'f', 2));
                    }
                });
    }

    // 3. Connect Property Inspector uniform updates
    if (m_propertyInspector) {
        connect(m_propertyInspector, &PropertyInspectorWidget::statusMessage, this,
                [this](const QString &msg) {
                    if (m_statusLabel) {
                        m_statusLabel->setText(msg);
                    }
                });
    }

    // 4. Connect Steam Workshop & Open Hub browser selections
    if (m_workshopBrowser) {
        connect(m_workshopBrowser, &WorkshopBrowserWidget::wallpaperSelected, this,
                [this](uint64_t /*id*/, const QString &filePath) { applyWallpaperPath(filePath); });
    }
    if (m_openHubBrowser) {
        connect(m_openHubBrowser, &OpenHubBrowserWidget::packageSelected, this,
                [this](const QString & /*pkgId*/, const QString &cid) {
                    if (m_statusLabel) {
                        m_statusLabel->setText(QStringLiteral("Selected Hub Package: %1").arg(cid));
                    }
                });
    }
}

void MainWindow::applyWallpaperPath(const QString &path)
{
    if (path.isEmpty())
        return;
    m_activeWallpaperPath = path;

    QFileInfo fi(path);
    if (!fi.exists()) {
        if (m_statusLabel) {
            m_statusLabel->setText(QStringLiteral("Preview Active: %1").arg(path));
        }
        return;
    }

    QString wwalBin = QStandardPaths::findExecutable(QStringLiteral("wwal"));
    if (wwalBin.isEmpty()) {
        QString localBin = QDir::homePath() + QStringLiteral("/.local/bin/wwal");
        if (QFileInfo::exists(localBin)) {
            wwalBin = localBin;
        }
    }

    if (!wwalBin.isEmpty()) {
        QStringList args;
        if (!m_options.namespaceStr.isEmpty() &&
            m_options.namespaceStr != QStringLiteral("default")) {
            args << QStringLiteral("--namespace") << m_options.namespaceStr;
        }
        args << QStringLiteral("img") << path;
        QProcess::startDetached(wwalBin, args);
    }

    if (m_statusLabel) {
        m_statusLabel->setText(QStringLiteral("Applied Wallpaper: %1").arg(fi.fileName()));
    }
}

void MainWindow::handleUndo()
{
    if (m_commandBus && m_commandBus->canUndo()) {
        m_commandBus->undo();
    }
}

void MainWindow::handleRedo()
{
    if (m_commandBus && m_commandBus->canRedo()) {
        m_commandBus->redo();
    }
}

void MainWindow::handleSearchFocus()
{
    if (m_gridWidget) {
        auto *edit = m_gridWidget->findChild<QLineEdit *>();
        if (edit) {
            edit->setFocus();
            edit->selectAll();
        }
    }
}

void MainWindow::handlePlayPauseToggle()
{
    m_isPlaying = !m_isPlaying;
    if (m_statusLabel) {
        m_statusLabel->setText(m_isPlaying ? QStringLiteral("Playback Resumed")
                                           : QStringLiteral("Playback Paused"));
    }
}

void MainWindow::handleFullscreenToggle()
{
    m_isFullscreenPreview = !m_isFullscreenPreview;
    if (m_isFullscreenPreview) {
        showFullScreen();
    } else {
        showNormal();
    }
}

void MainWindow::handlePerspectiveSelected(const QString &perspectiveName)
{
    if (m_layoutManager) {
        m_layoutManager->switchPerspective(perspectiveName);
    }
}

void MainWindow::handleResetLayout()
{
    if (m_layoutManager) {
        m_layoutManager->resetToDefaultPerspective();
    }
}

void MainWindow::handleOpenPackagingWizard()
{
    auto *wizard = new CreatorPackagingWizard(this);
    wizard->setAttribute(Qt::WA_DeleteOnClose);
    if (!m_activeWallpaperPath.isEmpty()) {
        wizard->setSourcePath(m_activeWallpaperPath);
    }
    wizard->show();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Auto-save docking layout to $XDG_CONFIG_HOME/wwal/studio_layout.json
    if (m_layoutManager && !m_options.headlessCheck && !m_options.benchmarkGrid) {
        m_layoutManager->saveLayoutToFile();
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && m_isFullscreenPreview) {
        handleFullscreenToggle();
        event->accept();
        return;
    }
    QMainWindow::keyPressEvent(event);
}

} // namespace waywal::studio
