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
#include <QPointer>
#include <QProcess>
#include <QSignalBlocker>
#include <QStandardPaths>

#include <cstdio>
#include <filesystem>
#include <system_error>
#include <type_traits>

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
        layout->setContentsMargins(10, 6, 10, 6);
        layout->setSpacing(12);

        m_canvas = new MonitorTopologyCanvas(this);
        layout->addWidget(m_canvas, 1);

        auto *controlsBox = new QWidget(this);
        controlsBox->setFixedWidth(210);
        auto *ctrlLayout = new QVBoxLayout(controlsBox);
        ctrlLayout->setContentsMargins(0, 0, 0, 0);
        ctrlLayout->setSpacing(8);

        auto *topBtnRow = new QHBoxLayout();
        topBtnRow->setContentsMargins(0, 0, 0, 0);
        topBtnRow->setSpacing(8);

        auto *btnRefresh = new QPushButton(QStringLiteral("Refresh"), controlsBox);
        btnRefresh->setToolTip(QStringLiteral("Refresh connected Wayland outputs"));
        btnRefresh->setFixedHeight(26);
        connect(btnRefresh, &QPushButton::clicked, this, [this]() {
            if (m_canvas)
                m_canvas->refreshFromWayland();
        });
        topBtnRow->addWidget(btnRefresh, 1);

        auto *btnSpan = new QPushButton(QStringLiteral("Span"), controlsBox);
        btnSpan->setCheckable(true);
        btnSpan->setFixedHeight(26);
        btnSpan->setToolTip(QStringLiteral("Span wallpaper across all displays"));
        connect(btnSpan, &QPushButton::toggled, this, [this](bool checked) {
            if (m_canvas)
                m_canvas->setDesktopSpanMode(checked);
        });
        topBtnRow->addWidget(btnSpan, 1);
        ctrlLayout->addLayout(topBtnRow);

        auto *scaleRow = new QHBoxLayout();
        scaleRow->setContentsMargins(0, 0, 0, 0);
        scaleRow->setSpacing(8);
        auto *scaleLbl = new QLabel(QStringLiteral("Scale"), controlsBox);
        scaleLbl->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 11px;"));
        scaleRow->addWidget(scaleLbl);

        auto *comboScaling = new QComboBox(controlsBox);
        comboScaling->setFixedHeight(26);
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
#include "studio/core/ipc_client.hpp"
#include "studio/core/services.hpp"
#include "studio/graph/node_graph_editor_widget.hpp"
#include "studio/graph/effect_library_browser_widget.hpp"
#include "studio/inspector/camera_lighting_inspector_widget.hpp"
#include "studio/inspector/physics_spring_inspector_widget.hpp"
#include "waywal/we_vdf.h"
#include "waywal/wwpkg.h"

namespace waywal::studio {

namespace {

QString normalizeTargetOutputSpec(const QString &targetOutput)
{
    const QString trimmed = targetOutput.trimmed();
    if (trimmed.isEmpty() || trimmed == QStringLiteral("*") ||
        trimmed.compare(QStringLiteral("All Displays"), Qt::CaseInsensitive) == 0 ||
        trimmed.compare(QStringLiteral("All Monitors"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("*");
    }
    return trimmed;
}

QString resolveLocalWwpkgFile(const QString &packageIdOrPath,
                              const std::shared_ptr<DownloadManager> &downloadMgr)
{
    const QString trimmed = packageIdOrPath.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }

    QFileInfo directInfo(trimmed);
    if (directInfo.exists() && directInfo.isFile()) {
        return directInfo.absoluteFilePath();
    }

    QStringList candidateDirs;
    const QString xdgHub = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                           QStringLiteral("/waywal/hub");
    if (!xdgHub.isEmpty()) {
        candidateDirs << xdgHub;
    }
    const QString homeHub = QDir::homePath() + QStringLiteral("/.local/share/waywal/hub");
    if (!candidateDirs.contains(homeHub)) {
        candidateDirs << homeHub;
    }
    if (downloadMgr) {
        const QString dlDir = QString::fromStdString(downloadMgr->downloadDirectory());
        if (!dlDir.isEmpty() && !candidateDirs.contains(dlDir)) {
            candidateDirs << dlDir;
        }
    }

    for (const QString &dirStr : candidateDirs) {
        std::error_code ec;
        const std::filesystem::path dirPath(dirStr.toStdString());
        if (!std::filesystem::exists(dirPath, ec) || ec ||
            !std::filesystem::is_directory(dirPath, ec) || ec) {
            continue;
        }

        const QString namedCandidate = QDir(dirStr).filePath(trimmed + QStringLiteral(".wwpkg"));
        QFileInfo namedInfo(namedCandidate);
        if (namedInfo.exists() && namedInfo.isFile()) {
            return namedInfo.absoluteFilePath();
        }

        for (const auto &entry : std::filesystem::directory_iterator(dirPath, ec)) {
            if (ec) {
                break;
            }
            std::error_code fileEc;
            if (!entry.is_regular_file(fileEc) || fileEc || entry.path().extension() != ".wwpkg") {
                continue;
            }
            const std::string pkgPathStd = entry.path().string();
            wwpkg_package_t *pkg = wwpkg_open(pkgPathStd.c_str());
            if (!pkg) {
                continue;
            }
            bool matched = false;
            const wwpkg_manifest_t *man = wwpkg_get_manifest(pkg);
            if (man) {
                const QString manId = QString::fromUtf8(man->id);
                const QString manSlug = QString::fromUtf8(man->slug);
                if (manId.compare(trimmed, Qt::CaseInsensitive) == 0 ||
                    manSlug.compare(trimmed, Qt::CaseInsensitive) == 0) {
                    matched = true;
                }
            }
            wwpkg_close(pkg);
            if (matched) {
                return QString::fromStdString(pkgPathStd);
            }
        }
    }

    return {};
}

QString resolveExtractedEntrypointPath(const uint8_t installedUuid[16],
                                       const QString &relativeEntrypoint)
{
    if (!installedUuid || relativeEntrypoint.isEmpty()) {
        return {};
    }
    char uuidHex[33]{};
    for (size_t i = 0; i < 16; ++i) {
        std::snprintf(uuidHex + (i * 2), 3, "%02x", installedUuid[i]);
    }
    const QString hexDir = QString::fromLatin1(uuidHex, 32);

    auto appendUniquePkgBase = [&](QStringList &out, const QString &root) {
        if (root.isEmpty()) {
            return;
        }
        const QString candidate =
            QDir::cleanPath(root + QStringLiteral("/wwal/packages/") + hexDir);
        if (!out.contains(candidate)) {
            out << candidate;
        }
    };

    QStringList baseDirs;
    const QString envXdgData = qEnvironmentVariable("XDG_DATA_HOME");
    if (envXdgData.startsWith(QLatin1Char('/'))) {
        appendUniquePkgBase(baseDirs, envXdgData);
    }
    appendUniquePkgBase(baseDirs,
                        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
    appendUniquePkgBase(baseDirs, QDir::homePath() + QStringLiteral("/.local/share"));

    /* Live daemon sessions fall back to XDG_STATE_HOME / ~/.local/state when Landlock restricts
     * ~/.local/share to read-only access. */
    const QString envXdgState = qEnvironmentVariable("XDG_STATE_HOME");
    if (envXdgState.startsWith(QLatin1Char('/'))) {
        appendUniquePkgBase(baseDirs, envXdgState);
    }
    appendUniquePkgBase(baseDirs,
                        QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation));
    appendUniquePkgBase(baseDirs, QDir::homePath() + QStringLiteral("/.local/state"));
    const QString envWwalState = qEnvironmentVariable("WWAL_STATE_DIR");
    if (envWwalState.startsWith(QLatin1Char('/'))) {
        const QString stateCandidate =
            QDir::cleanPath(envWwalState + QStringLiteral("/packages/") + hexDir);
        if (!baseDirs.contains(stateCandidate)) {
            baseDirs << stateCandidate;
        }
    }

    QString bestPath;
    std::filesystem::file_time_type bestMtime{};
    bool hasBest = false;

    for (const QString &base : baseDirs) {
        const QString full = QDir(base).filePath(relativeEntrypoint);
        QFileInfo fi(full);
        if (fi.exists() && fi.isFile()) {
            const QString absPath = fi.absoluteFilePath();
            std::error_code ec;
            const auto mtime = std::filesystem::last_write_time(absPath.toStdString(), ec);
            if (!hasBest || (!ec && mtime > bestMtime)) {
                bestPath = absPath;
                if (!ec) {
                    bestMtime = mtime;
                }
                hasBest = true;
            }
        }
    }
    return bestPath;
}

template <typename WizardT, typename HandlerT>
void connectWizardInstallSignalIfPresent(MainWindow *self, WizardT *wizard, HandlerT &&handler)
{
    if constexpr (requires { &WizardT::packageInstallRequested; }) {
        using SigPtr = decltype(&WizardT::packageInstallRequested);
        if constexpr (std::is_invocable_v<SigPtr, WizardT *, QString, QString>) {
            QObject::connect(wizard, &WizardT::packageInstallRequested, self,
                             [fn = std::forward<HandlerT>(handler)](const QString &pkgPathOrId,
                                                                    const QString &targetOutput) {
                                 fn(pkgPathOrId, targetOutput);
                             });
        } else if constexpr (std::is_invocable_v<SigPtr, WizardT *, QString>) {
            QObject::connect(wizard, &WizardT::packageInstallRequested, self,
                             [fn = std::forward<HandlerT>(handler)](const QString &pkgPathOrId) {
                                 fn(pkgPathOrId, QStringLiteral("*"));
                             });
        }
    }
}

} // namespace

/* ========================================================================= */
/* MainWindow Implementation                                                 */
/* ========================================================================= */

MainWindow::MainWindow(const StudioOptions &options, QWidget *parent)
    : QMainWindow(parent), m_options(options)
{
    // 1. Configure Window Properties
    setObjectName(QStringLiteral("WayWalStudioMainWindow"));
    setWindowTitle(QStringLiteral("WayWal Studio"));
    setMinimumSize(860, 600);
    resize(1600, 960);

    // 2. Setup Central Docking Architecture
    m_layoutManager = new DockLayoutManager(this, this);

    // 3. Initialize CommandBus with 1,000-Step History & Drag Coalescing
    m_commandBus = std::make_unique<CommandBus>(1000, this);

    // 3b. Initialize IpcClient
    const bool interactiveMode = !m_options.headlessCheck && !m_options.benchmarkGrid;
    m_ipcClient =
        std::make_unique<IpcClient>(m_options.namespaceStr, m_options.socketPath, this, false);
    Services::instance().setIpcClient(m_ipcClient.get());
    if (interactiveMode) {
        m_ipcClient->start();
    }

    // 4. Create and Wire all Panels & Bars
    setupPanels();

    // 5. Construct Menus, Toolbars, and Shortcuts
    setupMenuBar();
    setupToolBar();
    setupShortcuts();

    // 6. Apply Unified Studio Dark Styling
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
        QPointer<MainWindow> self(this);
        m_fsWatcher =
            std::make_unique<RecursiveFsWatcher>([self](const std::vector<FsWatchEvent> &events) {
                if (events.empty() || !self)
                    return;
                QMetaObject::invokeMethod(
                    self.data(),
                    [self]() {
                        if (!self) {
                            return;
                        }
                        if (self->m_workshopBrowser) {
                            self->m_workshopBrowser->refreshLibrary();
                        }
                        if (self->m_openHubBrowser) {
                            self->m_openHubBrowser->refreshFeed();
                        }
                    },
                    Qt::QueuedConnection);
            });

        waywal_steam_library_t libs[16];
        size_t libCount = waywal_steam_discover_libraries(libs, 16);
        for (size_t i = 0; i < libCount; ++i) {
            std::string wsDir = std::string(libs[i].path) + "/steamapps/workshop/content/431960";
            std::error_code fsEc;
            if (std::filesystem::exists(wsDir, fsEc) && !fsEc) {
                m_fsWatcher->addWatchRoot(wsDir);
            }
        }
        QString localHub = QDir::homePath() + QStringLiteral("/.local/share/waywal/hub");
        QDir().mkpath(localHub);
        const std::string localHubStd = localHub.toStdString();
        std::error_code hubEc;
        if (std::filesystem::exists(localHubStd, hubEc) && !hubEc) {
            m_fsWatcher->addWatchRoot(localHubStd);
        }
        m_fsWatcher->start();
    }
}

MainWindow::~MainWindow()
{
    if (m_fsWatcher) {
        m_fsWatcher->stop();
        m_fsWatcher.reset();
    }
    if (m_reconcileThread.joinable()) {
        m_reconcileThread.request_stop();
        m_reconcileThread.join();
    }
    if (m_downloadMgr) {
        m_downloadMgr->shutdown();
    }
    if (Services::instance().ipcClient() == m_ipcClient.get()) {
        Services::instance().setIpcClient(nullptr);
    }
    if (m_ipcClient) {
        m_ipcClient->stop();
    }
}

void MainWindow::setupPanels()
{
    // Panel 1 (Top Bar): Monitor Topology & Span Mode
    m_topologyBar = new MonitorTopologyBar(this);
    m_layoutManager->registerPanel(QStringLiteral("MonitorTopologyBar"), m_topologyBar,
                                   QStringLiteral("Displays"), Qt::TopDockWidgetArea);

    // Catalog & Download Managers
    m_catalogIndexMgr = std::make_shared<CatalogIndexManager>();
    m_downloadMgr = std::make_shared<DownloadManager>();
    if (!m_options.headlessCheck && !m_options.benchmarkGrid) {
        (void)m_downloadMgr->initialize();
    }

    // Panel 2 (Left Dock Tab 1): Virtualized Wallpaper Grid
    m_gridWidget = new VirtualizedGridWidget(this);
    m_layoutManager->registerPanel(QStringLiteral("Wallpapers"), m_gridWidget,
                                   QStringLiteral("Library"), Qt::LeftDockWidgetArea);

    // Panel 2 (Left Dock Tab 2): Steam Workshop Browser
    m_workshopBrowser = new WorkshopBrowserWidget(m_catalogIndexMgr, m_downloadMgr, this);
    m_layoutManager->registerPanel(QStringLiteral("WorkshopBrowser"), m_workshopBrowser,
                                   QStringLiteral("Workshop"), Qt::LeftDockWidgetArea);

    // Panel 2 (Left Dock Tab 3): Open Hub Browser
    m_openHubBrowser = new OpenHubBrowserWidget(m_downloadMgr, this);
    m_layoutManager->registerPanel(QStringLiteral("OpenHubBrowser"), m_openHubBrowser,
                                   QStringLiteral("Open Hub"), Qt::LeftDockWidgetArea);

    if (!m_options.headlessCheck && !m_options.benchmarkGrid && m_catalogIndexMgr) {
        QPointer<MainWindow> self(this);
        auto indexMgr = m_catalogIndexMgr;
        m_reconcileThread = std::jthread([self, indexMgr](std::stop_token st) {
            if (!indexMgr || st.stop_requested()) {
                return;
            }
            (void)indexMgr->reconcileAllLibraries();
            if (st.stop_requested() || !self) {
                return;
            }
            QMetaObject::invokeMethod(
                self.data(),
                [self]() {
                    if (self && self->m_workshopBrowser) {
                        self->m_workshopBrowser->refreshLibrary();
                    }
                },
                Qt::QueuedConnection);
        });
    }

    // Panel 3 (Center Dock Top): GPU Preview Viewport
    m_previewViewport = new GpuPreviewViewport(this);
    m_layoutManager->registerPanel(QStringLiteral("GpuPreviewViewport"), m_previewViewport,
                                   QStringLiteral("Preview"), Qt::RightDockWidgetArea);

    // Panel 4 (Center Dock Bottom): Transition Cubic Bezier Editor
    m_bezierEditor = new CubicBezierEditorWidget(this);
    m_bezierEditor->setCommandBus(m_commandBus.get());
    m_layoutManager->registerPanel(QStringLiteral("CubicBezier"), m_bezierEditor,
                                   QStringLiteral("Transitions"), Qt::RightDockWidgetArea);

    // Panel 5 (Right Dock Tab 1): Live Scene & Shader Inspector
    m_propertyInspector = new PropertyInspectorWidget(this);
    m_propertyInspector->setCommandBus(m_commandBus.get());
    m_propertyInspector->setIpcClient(m_ipcClient.get());
    m_layoutManager->registerPanel(QStringLiteral("PropertyInspectorWidget"), m_propertyInspector,
                                   QStringLiteral("Inspector"), Qt::RightDockWidgetArea);

    // Panel 5 (Right Dock Tab 2): Material You Theme Studio
    m_themeStudio = new ThemeStudioWidget(this);
    m_layoutManager->registerPanel(QStringLiteral("ThemeStudioWidget"), m_themeStudio,
                                   QStringLiteral("Theme"), Qt::RightDockWidgetArea);

    m_nodeGraphEditor = new NodeGraphEditorWidget(this);
    m_nodeGraphEditor->setCommandBus(m_commandBus.get());
    m_nodeGraphEditor->setIpcClient(m_ipcClient.get());
    m_layoutManager->registerPanel(QStringLiteral("NodeGraphEditorWidget"), m_nodeGraphEditor,
                                   QStringLiteral("Shader Graph"), Qt::RightDockWidgetArea);

    m_effectLibraryBrowser = new EffectLibraryBrowserWidget(this);
    m_layoutManager->registerPanel(QStringLiteral("EffectLibraryBrowserWidget"),
                                   m_effectLibraryBrowser, QStringLiteral("Effect Library"),
                                   Qt::LeftDockWidgetArea);

    m_cameraLightingInspector = new CameraLightingInspectorWidget(this);
    m_cameraLightingInspector->setCommandBus(m_commandBus.get());
    m_cameraLightingInspector->setIpcClient(m_ipcClient.get());
    m_layoutManager->registerPanel(QStringLiteral("CameraLightingInspectorWidget"),
                                   m_cameraLightingInspector, QStringLiteral("Camera & Lights"),
                                   Qt::RightDockWidgetArea);

    m_physicsSpringInspector = new PhysicsSpringInspectorWidget(this);
    m_physicsSpringInspector->setCommandBus(m_commandBus.get());
    m_physicsSpringInspector->setIpcClient(m_ipcClient.get());
    m_layoutManager->registerPanel(QStringLiteral("PhysicsSpringInspectorWidget"),
                                   m_physicsSpringInspector, QStringLiteral("Puppet Physics"),
                                   Qt::RightDockWidgetArea);

    // Panel 6 (Bottom Footer): Audio FFT Spectrum, Power Governor & Telemetry HUD
    m_footerBar = new StudioFooterBar(this);
    m_layoutManager->registerPanel(QStringLiteral("StudioFooterBar"), m_footerBar,
                                   QStringLiteral("Telemetry"), Qt::BottomDockWidgetArea);

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

    auto *folderSettingsAction = fileMenu->addAction(QStringLiteral("Wallpaper &Folders..."));
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
            this, QStringLiteral("Open GLSL Shader"), QString(),
            QStringLiteral("GLSL Shader (*.glsl *.frag *.comp);;All Files (*)"));
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
                m_statusLabel->setText(QStringLiteral("Loaded shader: %1").arg(file));
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

    m_packageWizardAction = fileMenu->addAction(QStringLiteral("&Package Wallpaper..."));
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

    m_searchFocusAction = editMenu->addAction(QStringLiteral("&Find Wallpaper"));
    m_searchFocusAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+F")));
    connect(m_searchFocusAction, &QAction::triggered, this, &MainWindow::handleSearchFocus);

    // --- View Menu ---
    auto *viewMenu = mb->addMenu(QStringLiteral("&View"));

    auto *perspsMenu = viewMenu->addMenu(QStringLiteral("&Workspaces"));

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

    auto *resetLayoutAction = perspsMenu->addAction(QStringLiteral("&Reset Layout"));
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

    m_fullscreenAction = viewMenu->addAction(QStringLiteral("&Fullscreen"));
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
        QMessageBox::about(this, QStringLiteral("About WayWal Studio"),
                           QStringLiteral("<b>WayWal Studio</b><br>"
                                          "Wallpaper manager and shader editor for Wayland."));
    });
}

void MainWindow::setupToolBar()
{
    auto *toolbar = addToolBar(QStringLiteral("Main Controls"));
    toolbar->setObjectName(QStringLiteral("MainStudioToolBar"));
    toolbar->setMovable(false);

    // Workspace Selector
    auto *perspLabel = new QLabel(QStringLiteral("Workspace"), this);
    perspLabel->setStyleSheet(
        QStringLiteral("color: #94a3b8; font-size: 11px; font-weight: 500; padding-left: 4px;"));
    toolbar->addWidget(perspLabel);

    m_perspectiveCombo = new QComboBox(this);
    m_perspectiveCombo->setObjectName(QStringLiteral("PerspectiveComboBox"));
    m_perspectiveCombo->setFixedWidth(148);
    m_perspectiveCombo->setFixedHeight(28);
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
    m_pauseBtn = new QPushButton(QStringLiteral("Pause"), this);
    m_pauseBtn->setObjectName(QStringLiteral("PlayPauseButton"));
    m_pauseBtn->setCheckable(true);
    m_pauseBtn->setChecked(false);
    m_pauseBtn->setToolTip(QStringLiteral("Pause or resume preview playback (Space)"));
    m_pauseBtn->setFixedHeight(28);
    connect(m_pauseBtn, &QPushButton::clicked, this, &MainWindow::handlePlayPauseToggle);
    toolbar->addWidget(m_pauseBtn);

    // Fullscreen Preview button
    auto *btnFullscreen = new QPushButton(QStringLiteral("Fullscreen"), this);
    btnFullscreen->setToolTip(QStringLiteral("Toggle fullscreen window (F11)"));
    btnFullscreen->setFixedHeight(28);
    connect(btnFullscreen, &QPushButton::clicked, this, &MainWindow::handleFullscreenToggle);
    toolbar->addWidget(btnFullscreen);

    // Creator Packaging Wizard button
    auto *btnWizard = new QPushButton(QStringLiteral("Package..."), this);
    btnWizard->setToolTip(
        QStringLiteral("Create a signed .wwpkg wallpaper package (Ctrl+Shift+P)"));
    btnWizard->setFixedHeight(28);
    connect(btnWizard, &QPushButton::clicked, this, &MainWindow::handleOpenPackagingWizard);
    toolbar->addWidget(btnWizard);

    // Spacer
    auto *spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    // Status label
    m_statusLabel = new QLabel(QStringLiteral("Ready"), this);
    m_statusLabel->setStyleSheet(
        QStringLiteral("color: #64748b; font-size: 11px; padding-right: 8px;"));
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
    // 1. Unified Dark Studio Palette
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(0x12, 0x14, 0x1a));
    palette.setColor(QPalette::WindowText, QColor(0xf1, 0xf5, 0xf9));
    palette.setColor(QPalette::Base, QColor(0x18, 0x1b, 0x22));
    palette.setColor(QPalette::AlternateBase, QColor(0x20, 0x24, 0x2e));
    palette.setColor(QPalette::ToolTipBase, QColor(0x18, 0x1b, 0x22));
    palette.setColor(QPalette::ToolTipText, QColor(0xf1, 0xf5, 0xf9));
    palette.setColor(QPalette::Text, QColor(0xf1, 0xf5, 0xf9));
    palette.setColor(QPalette::Button, QColor(0x20, 0x24, 0x2e));
    palette.setColor(QPalette::ButtonText, QColor(0xf1, 0xf5, 0xf9));
    palette.setColor(QPalette::BrightText, QColor(0xff, 0xff, 0xff));
    palette.setColor(QPalette::Highlight, QColor(0x02, 0x84, 0xc7));
    palette.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(0x64, 0x74, 0x8b));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x64, 0x74, 0x8b));
    QApplication::setPalette(palette);

    // 2. Unified Studio Stylesheet
    setStyleSheet(
        QStringLiteral("QMainWindow {"
                       "  background: #12141a;"
                       "}"
                       "QWidget {"
                       "  color: #f1f5f9;"
                       "  font-family: 'Inter', 'Cantarell', 'Segoe UI', sans-serif;"
                       "  font-size: 11px;"
                       "}"
                       "QMenuBar {"
                       "  background: #14171f;"
                       "  border-bottom: 1px solid #262b36;"
                       "  padding: 2px 6px;"
                       "}"
                       "QMenuBar::item {"
                       "  padding: 5px 10px;"
                       "  background: transparent;"
                       "  border-radius: 4px;"
                       "}"
                       "QMenuBar::item:selected {"
                       "  background: #20242e;"
                       "  color: #ffffff;"
                       "}"
                       "QMenu {"
                       "  background: #181b22;"
                       "  border: 1px solid #2c3240;"
                       "  padding: 4px 0px;"
                       "}"
                       "QMenu::item {"
                       "  padding: 6px 24px 6px 20px;"
                       "}"
                       "QMenu::item:selected {"
                       "  background: #0284c7;"
                       "  color: #ffffff;"
                       "}"
                       "QMenu::separator {"
                       "  height: 1px;"
                       "  background: #262b36;"
                       "  margin: 4px 0px;"
                       "}"
                       "QToolBar {"
                       "  background: #14171f;"
                       "  border-bottom: 1px solid #262b36;"
                       "  spacing: 10px;"
                       "  padding: 5px 12px;"
                       "}"
                       "QTabBar::tab {"
                       "  background: #14171f;"
                       "  color: #94a3b8;"
                       "  padding: 6px 14px;"
                       "  border: none;"
                       "  border-top-left-radius: 4px;"
                       "  border-top-right-radius: 4px;"
                       "  margin-right: 4px;"
                       "}"
                       "QTabBar::tab:selected {"
                       "  background: #20242e;"
                       "  color: #f8fafc;"
                       "  border-bottom: 2px solid #38bdf8;"
                       "  font-weight: 600;"
                       "}"
                       "QTabBar::tab:hover:!selected {"
                       "  background: #1b1f29;"
                       "  color: #e2e8f0;"
                       "}"
                       "QDockWidget {"
                       "  titlebar-close-icon: none;"
                       "  titlebar-normal-icon: none;"
                       "  border: 1px solid #262b36;"
                       "}"
                       "QSplitter::handle {"
                       "  background: #12141a;"
                       "}"
                       "QSplitter::handle:hover {"
                       "  background: #0284c7;"
                       "}"
                       "QPushButton, QToolButton {"
                       "  background: #20242e;"
                       "  border: 1px solid #2e3545;"
                       "  border-radius: 5px;"
                       "  padding: 5px 12px;"
                       "  color: #e2e8f0;"
                       "  font-weight: 500;"
                       "}"
                       "QPushButton:hover, QToolButton:hover {"
                       "  background: #2a303d;"
                       "  border-color: #38bdf8;"
                       "  color: #ffffff;"
                       "}"
                       "QPushButton:pressed, QToolButton:pressed {"
                       "  background: #161920;"
                       "}"
                       "QPushButton:checked {"
                       "  background: #0284c7;"
                       "  color: #ffffff;"
                       "  font-weight: 600;"
                       "  border-color: #38bdf8;"
                       "}"
                       "QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {"
                       "  background: #14171f;"
                       "  border: 1px solid #2e3545;"
                       "  border-radius: 5px;"
                       "  padding: 4px 8px;"
                       "  color: #f1f5f9;"
                       "}"
                       "QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {"
                       "  border: 1px solid #38bdf8;"
                       "}"
                       "QScrollBar:vertical {"
                       "  background: transparent;"
                       "  width: 8px;"
                       "  margin: 0px;"
                       "}"
                       "QScrollBar::handle:vertical {"
                       "  background: #2e3545;"
                       "  min-height: 24px;"
                       "  border-radius: 4px;"
                       "}"
                       "QScrollBar::handle:vertical:hover {"
                       "  background: #475569;"
                       "}"
                       "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
                       "  height: 0px;"
                       "}"
                       "QSlider::groove:horizontal {"
                       "  height: 4px;"
                       "  background: #262b36;"
                       "  border-radius: 2px;"
                       "}"
                       "QSlider::sub-page:horizontal {"
                       "  background: #0284c7;"
                       "  border-radius: 2px;"
                       "}"
                       "QSlider::handle:horizontal {"
                       "  width: 12px;"
                       "  margin: -4px 0;"
                       "  background: #f8fafc;"
                       "  border: 1px solid #0284c7;"
                       "  border-radius: 6px;"
                       "}"
                       "QSlider::handle:horizontal:hover {"
                       "  background: #38bdf8;"
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
        connect(m_openHubBrowser, &OpenHubBrowserWidget::packageInstallRequested, this,
                [this](const QString &packageId, const QString &targetOutput) {
                    installPackageToDaemon(packageId, targetOutput);
                });
    }

    if (m_effectLibraryBrowser && m_nodeGraphEditor) {
        connect(m_effectLibraryBrowser, &EffectLibraryBrowserWidget::effectInsertRequested, this,
                [this](uint32_t effectId) {
                    if (m_nodeGraphEditor) {
                        m_nodeGraphEditor->addBuiltinEffectNode(
                            static_cast<we_builtin_effect_id_t>(effectId), QPointF(100.0, 100.0));
                    }
                });
        connect(m_effectLibraryBrowser, &EffectLibraryBrowserWidget::effectActivated, this,
                [this](we_builtin_effect_id_t effectId, const QString & /*slug*/) {
                    if (m_nodeGraphEditor) {
                        m_nodeGraphEditor->addBuiltinEffectNode(effectId, QPointF(100.0, 100.0));
                    }
                });
    }

    if (m_nodeGraphEditor) {
        connect(m_nodeGraphEditor,
                qOverload<const QString &, uint32_t>(&NodeGraphEditorWidget::graphCompiled), this,
                [this](const QString &glslCode, uint32_t /*activePassMask*/) {
                    if (m_previewViewport && !glslCode.isEmpty()) {
                        m_previewViewport->setPreviewShader(glslCode);
                    }
                    if (m_ipcClient && !glslCode.isEmpty()) {
                        m_ipcClient->requestCompileShaderGraph(
                            QStringLiteral("*"), QString(), glslCode, WAYWAL_COMPILE_FLAG_HOT_SWAP);
                    }
                });
    }

    if (m_cameraLightingInspector) {
        connect(m_cameraLightingInspector, &CameraLightingInspectorWidget::camera3DChanged, this,
                [this](const QString &output, const we_camera3d_t &cam) {
                    if (m_previewViewport) {
                        QMatrix4x4 vpMat(cam.view_proj_matrix);
                        m_previewViewport->setShaderUniform(
                            QStringLiteral("g_ViewProjectionMatrix"), vpMat);
                    }
                    if (m_ipcClient) {
                        m_ipcClient->sendSetCamera3D(output, cam);
                    }
                });
        connect(m_cameraLightingInspector, &CameraLightingInspectorWidget::lightingChanged, this,
                [this](const QString &output, const we_lighting_uniforms_t &lights) {
                    if (m_ipcClient) {
                        m_ipcClient->sendSetSceneLights(output, lights);
                    }
                });
    }

    if (m_physicsSpringInspector) {
        connect(m_physicsSpringInspector, &PhysicsSpringInspectorWidget::physicsParamsChanged, this,
                [this](const QString &output, int32_t objectId, int32_t boneIndex,
                       const we_bone_sim_config_t &cfg, const we_wind_field_t &wind,
                       const we_cursor_force_t &cursor) {
                    if (m_ipcClient) {
                        m_ipcClient->sendSetPhysicsParams(output, objectId, boneIndex, cfg, wind,
                                                          cursor);
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

void MainWindow::installPackageToDaemon(const QString &packageIdOrPath, const QString &targetOutput)
{
    const QString resolvedPkgPath = resolveLocalWwpkgFile(packageIdOrPath, m_downloadMgr);
    if (resolvedPkgPath.isEmpty()) {
        if (m_statusLabel) {
            m_statusLabel->setText(
                QStringLiteral("Package not found locally: %1").arg(packageIdOrPath));
        }
        return;
    }

    QString relativeEntrypoint;
    {
        const QByteArray pathUtf8 = resolvedPkgPath.toUtf8();
        wwpkg_package_t *pkg = wwpkg_open(pathUtf8.constData());
        if (pkg) {
            const wwpkg_manifest_t *man = wwpkg_get_manifest(pkg);
            if (man && man->entrypoint_pipeline[0] != '\0') {
                relativeEntrypoint = QString::fromUtf8(man->entrypoint_pipeline);
            }
            wwpkg_close(pkg);
        }
    }

    const QString normalizedTarget = normalizeTargetOutputSpec(targetOutput);
    const QString pkgFileName = QFileInfo(resolvedPkgPath).fileName();
    if (m_statusLabel) {
        m_statusLabel->setText(QStringLiteral("Installing package: %1").arg(pkgFileName));
    }

    if (!m_ipcClient) {
        return;
    }

    QPointer<MainWindow> self(this);
    m_ipcClient->installWwpkg(
        resolvedPkgPath, normalizedTarget,
        WAYWAL_WWPKG_INSTALL_FLAG_AUTO_APPLY | WAYWAL_WWPKG_INSTALL_FLAG_FORCE_OVERWRITE,
        [self, pkgFileName, relativeEntrypoint](const waywal_ipc_resp_wwpkg_install_t &resp,
                                                const QString &errorMsg) {
            if (!self) {
                return;
            }
            if (resp.status_code == 0) {
                if (self->m_openHubBrowser) {
                    self->m_openHubBrowser->refreshFeed();
                }
                const QString extractedEntry =
                    resolveExtractedEntrypointPath(resp.package_uuid, relativeEntrypoint);
                if (!extractedEntry.isEmpty()) {
                    self->m_activeWallpaperPath = extractedEntry;
                }
                if (self->m_statusLabel) {
                    self->m_statusLabel->setText(
                        QStringLiteral("Installed package: %1").arg(pkgFileName));
                }
            } else if (self->m_statusLabel) {
                const QString detail =
                    errorMsg.isEmpty() ? QStringLiteral("code %1").arg(resp.status_code) : errorMsg;
                self->m_statusLabel->setText(
                    QStringLiteral("Package install failed: %1").arg(detail));
            }
        });
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
    if (m_pauseBtn) {
        const QSignalBlocker blocker(m_pauseBtn);
        m_pauseBtn->setChecked(!m_isPlaying);
        m_pauseBtn->setText(m_isPlaying ? QStringLiteral("Pause") : QStringLiteral("Resume"));
    }
    if (m_previewViewport) {
        m_previewViewport->pauseAnimation(!m_isPlaying);
    }
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

    auto installedOnce = std::make_shared<bool>(false);
    QPointer<MainWindow> self(this);
    connectWizardInstallSignalIfPresent(
        this, wizard,
        [self, installedOnce](const QString &pkgPathOrId, const QString &targetOutput) {
            if (!self || *installedOnce) {
                return;
            }
            *installedOnce = true;
            if (self->m_openHubBrowser) {
                self->m_openHubBrowser->refreshFeed();
            }
            self->installPackageToDaemon(pkgPathOrId, targetOutput);
        });

    connect(wizard, &QWizard::accepted, this, [self, wizard, installedOnce]() {
        if (!self) {
            return;
        }
        if (self->m_openHubBrowser) {
            self->m_openHubBrowser->refreshFeed();
        }
        if (*installedOnce) {
            return;
        }
        const QString pkgPath = wizard->compiledPackagePath();
        if (!pkgPath.isEmpty() && wizard->context() && wizard->context()->installedToLocalHub) {
            *installedOnce = true;
            self->installPackageToDaemon(pkgPath, QStringLiteral("*"));
        }
    });

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
