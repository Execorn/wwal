// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/app/main_window.hpp"
#include "studio/app/dock_layout_manager.hpp"
#include "waywal/ipc_proto_authoring.h"

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
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QStandardPaths>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <type_traits>
#include <utility>

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
#include "studio/scene/scene_layer_tree_widget.hpp"
#include "studio/brush/gpu_brush_painter_widget.hpp"
#include "studio/particle/particle_designer_widget.hpp"
#include "studio/audio/audio_binding_wizard.hpp"
#include "studio/editor/glsl_scenescript_editor_widget.hpp"
#include "studio/core/command_bus_authoring.hpp"
#include "studio/core/rigging_commands.hpp"
#include "studio/rigging/puppet_mesh_panel.hpp"
#include "studio/rigging/bone_rigging_panel.hpp"
#include "studio/rigging/weight_paint_panel.hpp"
#include "studio/rigging/dope_sheet_timeline_widget.hpp"
#include "waywal/we_pkg.h"
#include "waywal/we_vdf.h"
#include "waywal/wwpkg.h"

#include <QOpenGLContext>
#include <QSet>
#include <algorithm>
#include <array>

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

void composeBoneLocalMat4(const float pos[3], const float angle[3], const float scale[3],
                          float out[16]) noexcept
{
    const float cx = std::cos(angle[0]);
    const float sx = std::sin(angle[0]);
    const float cy = std::cos(angle[1]);
    const float sy = std::sin(angle[1]);
    const float cz = std::cos(angle[2]);
    const float sz = std::sin(angle[2]);

    out[0] = (cz * cy) * scale[0];
    out[1] = (sz * cy) * scale[0];
    out[2] = (-sy) * scale[0];
    out[3] = 0.0f;

    out[4] = (cz * sy * sx - sz * cx) * scale[1];
    out[5] = (sz * sy * sx + cz * cx) * scale[1];
    out[6] = (cy * sx) * scale[1];
    out[7] = 0.0f;

    out[8] = (cz * sy * cx + sz * sx) * scale[2];
    out[9] = (sz * sy * cx - cz * sx) * scale[2];
    out[10] = (cy * cx) * scale[2];
    out[11] = 0.0f;

    out[12] = pos[0];
    out[13] = pos[1];
    out[14] = pos[2];
    out[15] = 1.0f;
}

std::vector<std::array<float, 16>>
resolveBoneWorldMatrices(const std::vector<we_mdl_bone_t> &bones,
                         const std::vector<std::array<float, 16>> &localMats)
{
    const size_t boneCount = std::min(bones.size(), localMats.size());
    std::vector<std::array<float, 16>> worldMats(boneCount);
    auto mulColMajor = [](const float *a, const float *b, float *out) {
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                out[col * 4 + row] =
                    a[0 * 4 + row] * b[col * 4 + 0] + a[1 * 4 + row] * b[col * 4 + 1] +
                    a[2 * 4 + row] * b[col * 4 + 2] + a[3 * 4 + row] * b[col * 4 + 3];
            }
        }
    };

    std::vector<uint8_t> visitState(boneCount, 0u);
    std::vector<size_t> stack;
    stack.reserve(boneCount);
    for (size_t i = 0; i < boneCount; ++i) {
        if (visitState[i] == 2u) {
            continue;
        }
        stack.clear();
        size_t cur = i;
        while (cur < boneCount && visitState[cur] == 0u) {
            visitState[cur] = 1u;
            stack.push_back(cur);
            const uint32_t p = bones[cur].parent;
            if (p == 0xFFFFFFFFu || p >= boneCount) {
                break;
            }
            cur = static_cast<size_t>(p);
        }
        while (!stack.empty()) {
            const size_t idx = stack.back();
            stack.pop_back();
            const uint32_t p = bones[idx].parent;
            if (p != 0xFFFFFFFFu && p < boneCount && visitState[p] == 2u) {
                mulColMajor(worldMats[p].data(), localMats[idx].data(), worldMats[idx].data());
            } else {
                worldMats[idx] = localMats[idx];
            }
            visitState[idx] = 2u;
        }
    }
    return worldMats;
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
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    appendUniquePkgBase(baseDirs,
                        QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation));
#endif
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
    if (m_previewViewport && !m_previewViewport->parent()) {
        delete m_previewViewport;
        m_previewViewport = nullptr;
    }
}

MonitorTopologyBar *MainWindow::topologyBar() const { return m_topologyBar; }
VirtualizedGridWidget *MainWindow::gridWidget() const { return m_gridWidget; }
GpuPreviewViewport *MainWindow::previewViewport() const { return m_previewViewport; }
CubicBezierEditorWidget *MainWindow::bezierEditor() const { return m_bezierEditor; }
PropertyInspectorWidget *MainWindow::propertyInspector() const { return m_propertyInspector; }
ThemeStudioWidget *MainWindow::themeStudio() const { return m_themeStudio; }
StudioFooterBar *MainWindow::footerBar() const { return m_footerBar; }
WorkshopBrowserWidget *MainWindow::workshopBrowser() const { return m_workshopBrowser; }
OpenHubBrowserWidget *MainWindow::openHubBrowser() const { return m_openHubBrowser; }
NodeGraphEditorWidget *MainWindow::nodeGraphEditor() const { return m_nodeGraphEditor; }
EffectLibraryBrowserWidget *MainWindow::effectLibraryBrowser() const
{
    return m_effectLibraryBrowser;
}
CameraLightingInspectorWidget *MainWindow::cameraLightingInspector() const
{
    return m_cameraLightingInspector;
}
PhysicsSpringInspectorWidget *MainWindow::physicsSpringInspector() const
{
    return m_physicsSpringInspector;
}
SceneLayerTreeWidget *MainWindow::sceneLayerTree() const { return m_sceneLayerTree; }
GpuBrushPainterWidget *MainWindow::brushPainter() const { return m_brushPainter; }
ParticleDesignerWidget *MainWindow::particleDesigner() const { return m_particleDesigner; }
AudioBindingWizard *MainWindow::audioBindingWizard() const { return m_audioBindingWizard; }
GlslSceneScriptEditorWidget *MainWindow::codeEditor() const { return m_codeEditor; }
PuppetMeshPanel *MainWindow::puppetMeshPanel() const { return m_puppetMeshPanel; }
BoneRiggingPanel *MainWindow::boneRiggingPanel() const { return m_boneRiggingPanel; }
WeightPaintPanel *MainWindow::weightPaintPanel() const { return m_weightPaintPanel; }
DopeSheetTimelineWidget *MainWindow::dopeSheetTimeline() const { return m_dopeSheetTimeline; }

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
#if QT_VERSION < QT_VERSION_CHECK(6, 6, 0)
    if (QGuiApplication::platformName() == QLatin1String("offscreen")) {
        m_previewViewport = new GpuPreviewViewport(nullptr);
        m_previewViewport->setCommandBus(m_commandBus.get());
        auto *offscreenPlaceholder = new QWidget(this);
        offscreenPlaceholder->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        m_layoutManager->registerPanel(QStringLiteral("GpuPreviewViewport"), offscreenPlaceholder,
                                       QStringLiteral("Preview"), Qt::RightDockWidgetArea);
    } else {
        m_previewViewport = new GpuPreviewViewport(this);
        m_previewViewport->setCommandBus(m_commandBus.get());
        m_layoutManager->registerPanel(QStringLiteral("GpuPreviewViewport"), m_previewViewport,
                                       QStringLiteral("Preview"), Qt::RightDockWidgetArea);
    }
#else
    m_previewViewport = new GpuPreviewViewport(this);
    m_previewViewport->setCommandBus(m_commandBus.get());
    m_layoutManager->registerPanel(QStringLiteral("GpuPreviewViewport"), m_previewViewport,
                                   QStringLiteral("Preview"), Qt::RightDockWidgetArea);
#endif

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

    // Register authoring panels lazily so startup RssAnon stays within the 80 MB budget.
    m_layoutManager->registerLazyPanel(
        QStringLiteral("SceneLayerTree"), QStringLiteral("Scene Hierarchy"),
        [this](QWidget *parent) -> QWidget * {
            auto *tree = new SceneLayerTreeWidget(parent);
            tree->setCommandBus(m_commandBus.get());
            tree->setIpcClient(m_ipcClient.get());
            if (!m_lastSceneTreeJson.isEmpty()) {
                tree->loadFromSceneJson(m_lastSceneTreeJson);
            }
            connect(tree, &SceneLayerTreeWidget::objectSelected, this,
                    [this, tree](int32_t objectId, we_scene_node_kind_t kind) {
                        if (m_previewViewport) {
                            m_previewViewport->setGizmoActiveNode(tree->selectedNode());
                        }
                        if (m_propertyInspector) {
                            m_propertyInspector->setTargetObjectId(objectId, 0);
                        }
                        if (objectId >= 0 && kind == WE_NODE_IMAGE) {
                            const auto *node = tree->selectedNode();
                            const uint32_t texW =
                                (node && node->size[0] >= 1.0f)
                                    ? static_cast<uint32_t>(std::lround(node->size[0]))
                                    : (m_previewViewport && m_previewViewport->contentWidth() > 0u
                                           ? m_previewViewport->contentWidth()
                                           : 512u);
                            const uint32_t texH =
                                (node && node->size[1] >= 1.0f)
                                    ? static_cast<uint32_t>(std::lround(node->size[1]))
                                    : (m_previewViewport && m_previewViewport->contentHeight() > 0u
                                           ? m_previewViewport->contentHeight()
                                           : 512u);
                            setActivePuppetObjectContext(objectId, texW, texH);
                        }
                    });
            connect(tree, &SceneLayerTreeWidget::statusMessage, this, [this](const QString &msg) {
                if (m_statusLabel) {
                    m_statusLabel->setText(msg);
                }
            });
            m_sceneLayerTree = tree;
            return tree;
        },
        Qt::LeftDockWidgetArea);

    m_layoutManager->registerLazyPanel(
        QStringLiteral("GpuBrushPainter"), QStringLiteral("GPU Paint Brush"),
        [this](QWidget *parent) -> QWidget * {
            auto *brush = new GpuBrushPainterWidget(parent);
            brush->setCommandBus(m_commandBus.get());
            brush->setIpcClient(m_ipcClient.get());
            connect(brush, &GpuBrushPainterWidget::statusMessage, this, [this](const QString &msg) {
                if (m_statusLabel) {
                    m_statusLabel->setText(msg);
                }
            });
            m_brushPainter = brush;
            return brush;
        },
        Qt::RightDockWidgetArea);

    m_layoutManager->registerLazyPanel(
        QStringLiteral("ParticleDesigner"), QStringLiteral("Particle Designer"),
        [this](QWidget *parent) -> QWidget * {
            auto *designer = new ParticleDesignerWidget(parent);
            designer->setCommandBus(m_commandBus.get());
            designer->setIpcClient(m_ipcClient.get());
            connect(designer, &ParticleDesignerWidget::statusMessage, this,
                    [this](const QString &msg) {
                        if (m_statusLabel) {
                            m_statusLabel->setText(msg);
                        }
                    });
            m_particleDesigner = designer;
            return designer;
        },
        Qt::LeftDockWidgetArea);

    m_layoutManager->registerLazyPanel(
        QStringLiteral("AudioBindingWizard"), QStringLiteral("Audio Bindings"),
        [this](QWidget *parent) -> QWidget * {
            auto *wiz = new AudioBindingWizard(parent);
            wiz->setCommandBus(m_commandBus.get());
            wiz->setIpcClient(m_ipcClient.get());
            connect(wiz, &AudioBindingWizard::bindingsUpdated, this,
                    [this](const std::vector<we_audio_binding_t> &bindings) {
                        if (!m_propertyInspector) {
                            return;
                        }
                        for (auto *row : m_propertyInspector->findChildren<PropertyRowWidget *>()) {
                            row->setAudioBound(false);
                        }
                        for (const auto &b : bindings) {
                            if (b.uniform_name[0] == '\0') {
                                continue;
                            }
                            if (auto *row = m_propertyInspector->propertyRow(
                                    QString::fromUtf8(b.uniform_name))) {
                                if (b.enabled != 0) {
                                    row->setAudioBound(true);
                                }
                            }
                        }
                    });
            connect(wiz, &AudioBindingWizard::statusMessage, this, [this](const QString &msg) {
                if (m_statusLabel) {
                    m_statusLabel->setText(msg);
                }
            });
            m_audioBindingWizard = wiz;
            return wiz;
        },
        Qt::RightDockWidgetArea);

    m_layoutManager->registerLazyPanel(
        QStringLiteral("GlslSceneScriptEditor"), QStringLiteral("Code Editor"),
        [this](QWidget *parent) -> QWidget * {
            auto *editor = new GlslSceneScriptEditorWidget(parent);
            editor->setIpcClient(m_ipcClient.get());
            if (m_previewViewport && m_previewViewport->context()) {
                editor->setSharedOpenGLContext(m_previewViewport->context());
            }
            connect(editor, &GlslSceneScriptEditorWidget::shaderMetadataExtracted, this,
                    [this](const waywal_shader_meta_t &meta, const QString &rawComboJson) {
                        if (m_propertyInspector) {
                            m_propertyInspector->setShaderMetadata(meta, rawComboJson);
                        }
                    });
            connect(editor, &GlslSceneScriptEditorWidget::liveHotReloadApplied, this,
                    [this, editor](bool success, uint32_t /*compileTimeUs*/) {
                        if (success && m_previewViewport &&
                            editor->languageMode() == EditorLanguageMode::GlslEs310Fragment) {
                            m_previewViewport->setPreviewShader(editor->sourceText());
                        }
                    });
            connect(editor, &GlslSceneScriptEditorWidget::statusMessage, this,
                    [this](const QString &msg) {
                        if (m_statusLabel) {
                            m_statusLabel->setText(msg);
                        }
                    });
            if (!m_activeWallpaperPath.isEmpty()) {
                const QString lower = m_activeWallpaperPath.toLower();
                if (lower.endsWith(QStringLiteral(".glsl")) ||
                    lower.endsWith(QStringLiteral(".frag")) ||
                    lower.endsWith(QStringLiteral(".comp"))) {
                    QFile f(m_activeWallpaperPath);
                    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                        editor->openDocument(EditorLanguageMode::GlslEs310Fragment, -1, 0,
                                             QFileInfo(m_activeWallpaperPath).fileName(),
                                             QString::fromUtf8(f.readAll()));
                    }
                }
            }
            m_codeEditor = editor;
            return editor;
        },
        Qt::BottomDockWidgetArea);

    m_layoutManager->registerLazyPanel(
        QStringLiteral("PuppetMeshPanel"), QStringLiteral("Puppet Mesh"),
        [this](QWidget *parent) -> QWidget * {
            auto *panel = new PuppetMeshPanel(parent);
            panel->setCommandBus(m_commandBus.get());
            panel->setIpcClient(m_ipcClient.get());
            connect(panel, &PuppetMeshPanel::meshGenerated, this,
                    [this, panel](int32_t objectId, uint32_t /*vertexCount*/,
                                  uint32_t /*triangleCount*/, float /*minAngleDeg*/) {
                        if (objectId >= 0) {
                            m_activePuppetObjectId = objectId;
                            m_cachedPuppetMesh = PuppetMeshSnapshot{};
                            m_cachedPuppetMesh.objectId = objectId;
                            const auto &res = panel->lastResult();
                            if (res.vertices && res.vertex_count > 0u) {
                                m_cachedPuppetMesh.vertices.assign(res.vertices,
                                                                   res.vertices + res.vertex_count);
                            }
                            if (res.indices && res.triangle_count > 0u) {
                                m_cachedPuppetMesh.indices.assign(
                                    res.indices,
                                    res.indices + static_cast<size_t>(res.triangle_count) * 3u);
                            }
                            m_cachedPuppetMesh.pins = panel->pins();
                        }
                        if (m_previewViewport) {
                            m_previewViewport->setPuppetObjectId(objectId);
                            m_previewViewport->setPuppetMeshWireframe(panel->isWireframeVisible(),
                                                                      panel->lastResult());
                            m_previewViewport->setPuppetContourPins(panel->pins(), -1);
                        }
                        if (m_weightPaintPanel) {
                            hydrateWeightPaintPanel(m_weightPaintPanel);
                        }
                    });
            connect(panel, &PuppetMeshPanel::wireframeOverlayRequested, this,
                    [this](bool visible, const we_mesh_gen_result_t &mesh) {
                        if (m_previewViewport) {
                            m_previewViewport->setPuppetMeshWireframe(visible, mesh);
                        }
                    });
            connect(panel, &PuppetMeshPanel::contourPinsChanged, this,
                    [this](int32_t objectId, const std::vector<we_cdt_pin_t> &pins) {
                        if (objectId >= 0 && m_cachedPuppetMesh.objectId == objectId) {
                            m_cachedPuppetMesh.pins = pins;
                        }
                        if (m_previewViewport && m_previewViewport->puppetObjectId() == objectId) {
                            m_previewViewport->setPuppetContourPins(pins, -1);
                        }
                    });
            connect(
                panel, &PuppetMeshPanel::exportMdlRequested, this,
                [this](int32_t objectId, const QString &destPath) {
                    if (!m_ipcClient || objectId < 0 || destPath.isEmpty()) {
                        return;
                    }
                    m_pendingMdlExportPaths[objectId] = destPath;
                    char resolved[512] = {};
                    const QByteArray pathBa = destPath.toUtf8();
                    const bool inAllowedDir =
                        static_cast<size_t>(pathBa.size()) < sizeof(resolved) &&
                        waywal_ipc_export_path_is_allowed(pathBa.constData(), resolved,
                                                          sizeof(resolved));
                    const uint32_t flags = inAllowedDir ? 0x3u : 0x1u;
                    if (!m_ipcClient->sendExportPuppetMdl(static_cast<uint32_t>(objectId), flags,
                                                          inAllowedDir ? destPath : QString())) {
                        m_pendingMdlExportPaths.erase(objectId);
                    }
                });
            connect(panel, &PuppetMeshPanel::statusMessage, this, [this](const QString &msg) {
                if (m_statusLabel) {
                    m_statusLabel->setText(msg);
                }
            });
            if (m_activePuppetObjectId >= 0) {
                panel->setTargetObjectId(m_activePuppetObjectId, m_activePuppetTexWidth,
                                         m_activePuppetTexHeight);
            }
            m_puppetMeshPanel = panel;
            return panel;
        },
        Qt::LeftDockWidgetArea);

    m_layoutManager->registerLazyPanel(
        QStringLiteral("BoneRiggingPanel"), QStringLiteral("Bone Rigging"),
        [this](QWidget *parent) -> QWidget * {
            auto *panel = new BoneRiggingPanel(parent);
            panel->setCommandBus(m_commandBus.get());
            panel->setIpcClient(m_ipcClient.get());
            connect(panel, &BoneRiggingPanel::boneSelected, this, [this](int32_t boneIndex) {
                if (m_previewViewport) {
                    m_previewViewport->setSelectedPuppetBone(boneIndex);
                }
                if (m_weightPaintPanel) {
                    m_weightPaintPanel->setActiveBone(boneIndex);
                }
                if (m_physicsSpringInspector) {
                    m_physicsSpringInspector->selectBoneIndex(boneIndex);
                }
            });
            connect(
                panel, &BoneRiggingPanel::skeletonUpdated, this,
                [this, panel](int32_t objectId, const BoneHierarchySnapshot &snapshot) {
                    if (objectId >= 0) {
                        m_activePuppetObjectId = objectId;
                    }
                    m_cachedPuppetRig = snapshot;
                    m_hasCachedPuppetRig = true;

                    if (m_previewViewport) {
                        m_previewViewport->setPuppetObjectId(objectId);
                        m_previewViewport->setPuppetSkeleton(snapshot, panel->selectedBoneIndex());
                    }
                    if (m_physicsSpringInspector && !m_syncingBoneDriveMode) {
                        const QScopedValueRollback<bool> guard(m_syncingBoneDriveMode, true);
                        std::vector<QString> names;
                        std::vector<int32_t> parents;
                        names.reserve(snapshot.bones.size());
                        parents.reserve(snapshot.bones.size());
                        for (size_t i = 0; i < snapshot.bones.size(); ++i) {
                            QString bName = QString::fromUtf8(snapshot.bones[i].name).trimmed();
                            if (bName.isEmpty()) {
                                bName = QStringLiteral("Bone %1").arg(i);
                            }
                            names.push_back(bName);
                            parents.push_back(snapshot.bones[i].parent == 0xFFFFFFFFu
                                                  ? -1
                                                  : static_cast<int32_t>(snapshot.bones[i].parent));
                        }
                        m_physicsSpringInspector->setBoneList(names, parents, objectId);
                        m_physicsSpringInspector->setBoneDriveModes(snapshot.driveModes);
                    }
                    if (m_weightPaintPanel) {
                        hydrateWeightPaintPanel(m_weightPaintPanel);
                    }
                    if (m_dopeSheetTimeline) {
                        hydrateDopeSheetTimeline(m_dopeSheetTimeline);
                    }
                    if (m_onionSkinEnabled) {
                        refreshOnionSkinFrames();
                    }
                });
            connect(panel, &BoneRiggingPanel::ikPreviewRequested, this,
                    [this](int32_t objectId, const we_ik_chain_t &chain) {
                        if (m_ipcClient && objectId >= 0) {
                            waywal_ipc_puppet_preview_ik_t req{};
                            req.object_id = static_cast<uint32_t>(objectId);
                            req.root_bone = chain.root_bone;
                            req.mid_bone = chain.mid_bone;
                            req.end_bone = chain.end_bone;
                            req.pole_flip = (chain.bend_sign < 0) ? 1u : 0u;
                            req.target_world[0] = chain.target_world[0];
                            req.target_world[1] = chain.target_world[1];
                            req.target_world[2] = chain.target_world[2];
                            req.mode_flags = 1u;
                            m_ipcClient->sendPuppetPreviewIk(req);
                        }
                    });
            connect(panel, &BoneRiggingPanel::statusMessage, this, [this](const QString &msg) {
                if (m_statusLabel) {
                    m_statusLabel->setText(msg);
                }
            });
            if (m_hasCachedPuppetRig && (m_activePuppetObjectId < 0 ||
                                         m_cachedPuppetRig.objectId == m_activePuppetObjectId)) {
                we_puppet_t puppet{};
                std::vector<we_mdl_bone_t> bones = m_cachedPuppetRig.bones;
                puppet.bone_count = static_cast<uint32_t>(bones.size());
                puppet.bones = bones.empty() ? nullptr : bones.data();
                panel->loadPuppetRig(&puppet, m_cachedPuppetRig.objectId);
            } else if (m_activePuppetObjectId >= 0) {
                panel->loadPuppetRig(nullptr, m_activePuppetObjectId);
            }
            const int32_t rigObjectId = panel->rigSnapshot().objectId;
            if (const auto pendIt = m_pendingDriveModes.find(rigObjectId);
                pendIt != m_pendingDriveModes.end()) {
                const QScopedValueRollback<bool> guard(m_syncingBoneDriveMode, true);
                for (const auto &[boneIdx, mode] : pendIt->second) {
                    panel->setBoneDriveMode(rigObjectId, boneIdx, mode);
                }
                m_pendingDriveModes.erase(pendIt);
            }
            m_boneRiggingPanel = panel;
            return panel;
        },
        Qt::LeftDockWidgetArea);

    m_layoutManager->registerLazyPanel(
        QStringLiteral("WeightPaintPanel"), QStringLiteral("Weight Paint"),
        [this](QWidget *parent) -> QWidget * {
            auto *panel = new WeightPaintPanel(parent);
            panel->setCommandBus(m_commandBus.get());
            panel->setIpcClient(m_ipcClient.get());
            connect(panel, &WeightPaintPanel::heatmapModeChanged, this,
                    [this](int32_t /*objectId*/, bool enabled, uint32_t activeBoneIdx,
                           float opacity, we_heatmap_palette_t palette) {
                        if (m_previewViewport) {
                            m_previewViewport->setWeightHeatmapMode(enabled, activeBoneIdx, opacity,
                                                                    palette);
                        }
                    });
            connect(panel, &WeightPaintPanel::normalizeAllWeightsRequested, this,
                    [this](int32_t objectId, uint64_t lockMaskLo, uint64_t lockMaskHi) {
                        if (m_previewViewport && objectId >= 0) {
                            m_previewViewport->normalizeAllVertexWeights(
                                objectId, std::array<uint64_t, 2>{lockMaskLo, lockMaskHi});
                        }
                    });
            connect(panel, &WeightPaintPanel::brushParamsChanged, this, [this, panel]() {
                if (m_previewViewport) {
                    m_previewViewport->setWeightBrushParameters(
                        panel->activeBrushOp(), panel->brushRadiusPx(), panel->brushStrength(),
                        panel->targetWeightValue(), panel->lockedBoneMask());
                }
            });
            connect(panel, &WeightPaintPanel::statusMessage, this, [this](const QString &msg) {
                if (m_statusLabel) {
                    m_statusLabel->setText(msg);
                }
            });
            hydrateWeightPaintPanel(panel);
            m_weightPaintPanel = panel;
            return panel;
        },
        Qt::RightDockWidgetArea);

    m_layoutManager->registerLazyPanel(
        QStringLiteral("DopeSheetTimelineWidget"), QStringLiteral("Timeline / Dope-Sheet"),
        [this](QWidget *parent) -> QWidget * {
            auto *timeline = new DopeSheetTimelineWidget(parent);
            timeline->setCommandBus(m_commandBus.get());
            timeline->setIpcClient(m_ipcClient.get());
            connect(timeline, &DopeSheetTimelineWidget::frameChanged, this,
                    [this](double /*frame*/, double /*timeSec*/) {
                        if (m_onionSkinEnabled) {
                            refreshOnionSkinFrames();
                        }
                    });
            connect(timeline, &DopeSheetTimelineWidget::onionSkinChanged, this,
                    [this](bool enabled, int beforeFrames, int afterFrames, float opacity) {
                        m_onionSkinEnabled = enabled;
                        m_onionBeforeFrames = std::clamp(beforeFrames, 0, 8);
                        m_onionAfterFrames = std::clamp(afterFrames, 0, 8);
                        m_onionOpacity = std::clamp(opacity, 0.05f, 1.0f);
                        if (m_previewViewport) {
                            m_previewViewport->setOnionSkinEnabled(enabled);
                            refreshOnionSkinFrames();
                        }
                    });
            connect(timeline, &DopeSheetTimelineWidget::statusMessage, this,
                    [this](const QString &msg) {
                        if (m_statusLabel) {
                            m_statusLabel->setText(msg);
                        }
                    });
            hydrateDopeSheetTimeline(timeline);
            m_dopeSheetTimeline = timeline;
            return timeline;
        },
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
            if (isShader && m_codeEditor) {
                QFile f(file);
                if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    m_codeEditor->openDocument(EditorLanguageMode::GlslEs310Fragment, -1, 0,
                                               QFileInfo(file).fileName(),
                                               QString::fromUtf8(f.readAll()));
                }
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
            if (m_codeEditor) {
                QFile f(file);
                if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    m_codeEditor->openDocument(EditorLanguageMode::GlslEs310Fragment, -1, 0,
                                               QFileInfo(file).fileName(),
                                               QString::fromUtf8(f.readAll()));
                }
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

    m_exportScenePkgAction = fileMenu->addAction(QStringLiteral("&Export Scene Package..."));
    m_exportScenePkgAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+E")));
    connect(m_exportScenePkgAction, &QAction::triggered, this,
            &MainWindow::handleExportScenePackage);

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

    auto *scenePerspAction = perspsMenu->addAction(QStringLiteral("&3 Scene Composer"));
    scenePerspAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+3")));
    connect(scenePerspAction, &QAction::triggered, this,
            [this]() { handlePerspectiveSelected(QStringLiteral("Scene Composer")); });

    auto *particlePerspAction = perspsMenu->addAction(QStringLiteral("&4 Particle & Audio"));
    particlePerspAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+4")));
    connect(particlePerspAction, &QAction::triggered, this,
            [this]() { handlePerspectiveSelected(QStringLiteral("Particle & Audio")); });

    auto *themePerspAction = perspsMenu->addAction(QStringLiteral("&5 Theme Studio"));
    themePerspAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+5")));
    connect(themePerspAction, &QAction::triggered, this,
            [this]() { handlePerspectiveSelected(QStringLiteral("Theme Studio")); });

    auto *monitorPerspAction = perspsMenu->addAction(QStringLiteral("&6 Multi-Monitor"));
    monitorPerspAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+6")));
    connect(monitorPerspAction, &QAction::triggered, this,
            [this]() { handlePerspectiveSelected(QStringLiteral("Multi-Monitor")); });

    auto *riggingPerspAction = perspsMenu->addAction(QStringLiteral("&7 Rigging && Animation"));
    riggingPerspAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+7")));
    connect(riggingPerspAction, &QAction::triggered, this,
            [this]() { handlePerspectiveSelected(QStringLiteral("Rigging & Animation")); });

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
                                  QStringLiteral("Scene Composer"),
                                  QStringLiteral("Particle & Audio"),
                                  QStringLiteral("Theme Studio"), QStringLiteral("Multi-Monitor"),
                                  QStringLiteral("Rigging & Animation")});
    connect(m_perspectiveCombo, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        if (!m_updatingPerspectiveCombo) {
            handlePerspectiveSelected(text);
        }
    });
    toolbar->addWidget(m_perspectiveCombo);

    toolbar->addSeparator();

    auto *toolLabel = new QLabel(QStringLiteral("Tool"), this);
    toolLabel->setStyleSheet(
        QStringLiteral("color: #94a3b8; font-size: 11px; font-weight: 500; padding-left: 4px;"));
    toolbar->addWidget(toolLabel);

    m_toolModeCombo = new QComboBox(this);
    m_toolModeCombo->setObjectName(QStringLiteral("ViewportToolModeCombo"));
    m_toolModeCombo->setFixedWidth(136);
    m_toolModeCombo->setFixedHeight(28);
    m_toolModeCombo->addItems(
        {QStringLiteral("Navigate"), QStringLiteral("Select"), QStringLiteral("Translate"),
         QStringLiteral("Rotate"), QStringLiteral("Scale"), QStringLiteral("Brush: Mask"),
         QStringLiteral("Brush: Flow"), QStringLiteral("Brush: Blur/Smudge"),
         QStringLiteral("Rig: Contour Pin"), QStringLiteral("Rig: Bone Place"),
         QStringLiteral("Rig: IK Drag"), QStringLiteral("Rig: Weight Brush")});
    connect(m_toolModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::handleViewportToolModeChanged);
    toolbar->addWidget(m_toolModeCombo);

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
                [this](we_builtin_effect_id_t /*effectId*/, const QString &slug) {
                    if (m_statusLabel && !slug.isEmpty()) {
                        m_statusLabel->setText(QStringLiteral("Effect: %1").arg(slug));
                    }
                });
    }

    if (m_nodeGraphEditor) {
        connect(m_nodeGraphEditor,
                qOverload<const QString &, uint32_t>(&NodeGraphEditorWidget::graphCompiled), this,
                [this](const QString &glslCode, uint32_t /*activePassMask*/) {
                    if (glslCode.isEmpty()) {
                        return;
                    }
                    if (m_previewViewport) {
                        m_previewViewport->setPreviewShader(glslCode);
                    }
                    if (m_nodeGraphEditor) {
                        m_nodeGraphEditor->compileAndHotSwapDaemon();
                    }
                });
    }

    if (m_cameraLightingInspector) {
        connect(m_cameraLightingInspector, &CameraLightingInspectorWidget::camera3DChanged, this,
                [this](const QString & /*output*/, const we_camera3d_t &cam) {
                    if (m_previewViewport) {
                        QMatrix4x4 vpMat;
                        std::memcpy(vpMat.data(), cam.view_proj_matrix, 16 * sizeof(float));
                        m_previewViewport->setShaderUniform(
                            QStringLiteral("g_ViewProjectionMatrix"), vpMat);
                    }
                });
    }

    // 5. Connect Viewport Gizmos, Brush Routing, Audio Wizard, Code Editor & Scene Tree
    if (m_previewViewport) {
        connect(
            m_previewViewport, &GpuPreviewViewport::layerTransformCommitted, this,
            [this](int32_t objectId, const SetLayerTransformCommand::TransformState &oldState,
                   const SetLayerTransformCommand::TransformState &newState) {
                auto applyTransform = [this](int32_t objId,
                                             const SetLayerTransformCommand::TransformState &st) {
                    if (m_ipcClient) {
                        waywal_ipc_req_scene_mutate_t req{};
                        req.mutate_op = WE_MUTATE_SET_TRANSFORM;
                        req.object_id = objId;
                        for (size_t i = 0; i < 3; ++i) {
                            req.origin[i] = st.origin[i];
                            req.scale[i] = st.scale[i];
                            req.angles[i] = st.angles[i];
                        }
                        req.size[0] = st.size[0];
                        req.size[1] = st.size[1];
                        req.parallax_depth[0] = st.parallaxDepth[0];
                        req.parallax_depth[1] = st.parallaxDepth[1];
                        m_ipcClient->sendSceneMutate(req);
                    }
                    if (m_sceneLayerTree) {
                        // Update the layer model transform before refreshing the gizmo target
                        // so the overlay immediately reflects committed state without snapping
                        // back.
                        if (auto *model = m_sceneLayerTree->findChild<SceneLayerTreeModel *>()) {
                            std::vector<SceneLayerNode> updatedNodes = model->nodes();
                            bool nodeUpdated = false;
                            for (auto &n : updatedNodes) {
                                if (n.id == objId) {
                                    for (size_t i = 0; i < 3; ++i) {
                                        n.origin[i] = st.origin[i];
                                        n.scale[i] = st.scale[i];
                                        n.angles[i] = st.angles[i];
                                    }
                                    n.size[0] = st.size[0];
                                    n.size[1] = st.size[1];
                                    n.parallaxDepth[0] = st.parallaxDepth[0];
                                    n.parallaxDepth[1] = st.parallaxDepth[1];
                                    nodeUpdated = true;
                                    break;
                                }
                            }
                            if (nodeUpdated) {
                                const int32_t prevSelId = m_sceneLayerTree->selectedObjectId();
                                model->setNodes(std::move(updatedNodes));
                                if (prevSelId >= 0) {
                                    const QSignalBlocker blocker(m_sceneLayerTree.data());
                                    m_sceneLayerTree->selectObject(prevSelId);
                                }
                            }
                        }
                        m_previewViewport->setGizmoActiveNode(m_sceneLayerTree->selectedNode());
                    }
                };
                if (m_commandBus) {
                    m_commandBus->execute(std::make_unique<SetLayerTransformCommand>(
                        objectId, QStringLiteral("Layer %1").arg(objectId), oldState, newState,
                        applyTransform));
                } else {
                    applyTransform(objectId, newState);
                }
            });

        connect(m_previewViewport, &GpuPreviewViewport::brushPointerPressed, this,
                [this](const QPointF &texelPos, float pressure, double timestampSec) {
                    if (!m_brushPainter && m_layoutManager) {
                        m_layoutManager->realizeLazyPanel(QStringLiteral("GpuBrushPainter"));
                    }
                    if (m_brushPainter) {
                        m_brushPainter->handlePointerPress(texelPos, pressure, timestampSec);
                    }
                });
        connect(m_previewViewport, &GpuPreviewViewport::brushPointerMoved, this,
                [this](const QPointF &texelPos, float pressure, double timestampSec) {
                    if (m_brushPainter) {
                        m_brushPainter->handlePointerMove(texelPos, pressure, timestampSec);
                    }
                });
        connect(m_previewViewport, &GpuPreviewViewport::brushPointerReleased, this,
                [this](const QPointF &texelPos, float pressure, double timestampSec) {
                    if (m_brushPainter) {
                        m_brushPainter->handlePointerRelease(texelPos, pressure, timestampSec);
                    }
                });
        connect(m_previewViewport, &GpuPreviewViewport::toolModeChanged, this,
                [this](ViewportToolMode mode) {
                    if (m_toolModeCombo) {
                        const QSignalBlocker blocker(m_toolModeCombo);
                        const int idx = static_cast<int>(mode);
                        if (idx >= 0 && idx < m_toolModeCombo->count()) {
                            m_toolModeCombo->setCurrentIndex(idx);
                        }
                    }
                });
    }

    if (m_propertyInspector) {
        connect(m_propertyInspector, &PropertyInspectorWidget::audioBindingRequested, this,
                [this](const QString &uniformName, float baseline) {
                    if (m_layoutManager) {
                        m_layoutManager->realizeLazyPanel(QStringLiteral("AudioBindingWizard"));
                        m_layoutManager->setPanelVisible(QStringLiteral("AudioBindingWizard"),
                                                         true);
                    }
                    if (m_audioBindingWizard) {
                        m_audioBindingWizard->preselectTargetUniform(
                            m_propertyInspector->targetObjectId(), uniformName, 0, baseline);
                    }
                });
        connect(m_propertyInspector, &PropertyInspectorWidget::uniformDefaultEdited, this,
                [this](const QString &symbolName, const QString &formattedVal) {
                    if (m_codeEditor) {
                        m_codeEditor->patchUniformOrComboDefaultInSource(symbolName, formattedVal);
                    }
                });
        connect(m_propertyInspector, &PropertyInspectorWidget::shaderComboChanged, this,
                [this](const QString &output, int32_t objectId, uint32_t passIndex,
                       const QString &comboName, int32_t comboValue) {
                    if (m_ipcClient) {
                        m_ipcClient->sendSetShaderCombo(output, objectId, passIndex, comboName,
                                                        comboValue);
                    }
                });
    }

    if (m_ipcClient) {
        connect(m_ipcClient.get(), &IpcClient::sceneTreeQueried, this,
                [this](const waywal_ipc_resp_query_scene_tree_t &resp, const QString &treeJson) {
                    if (resp.status_code == 0 && !treeJson.isEmpty()) {
                        m_lastSceneTreeJson = treeJson;
                        if (m_sceneLayerTree) {
                            m_sceneLayerTree->loadFromSceneJson(treeJson);
                        }
                    }
                });
        connect(m_ipcClient.get(), &IpcClient::telemetryReceived, this,
                [this](const TelemetrySnapshot &snapshot) {
                    if (!m_audioBindingWizard || !m_audioBindingWizard->isVisible()) {
                        return;
                    }
                    std::array<float, 64> bands{};
                    const size_t n = std::min<size_t>(snapshot.barkBands.size(), 64);
                    for (size_t i = 0; i < n; ++i) {
                        bands[i] = snapshot.barkBands[i];
                    }
                    float bassEnergy = 0.0f;
                    for (size_t i = 0; i < 7; ++i) {
                        bassEnergy += bands[i];
                    }
                    bassEnergy /= 7.0f;
                    m_audioBindingWizard->updateLiveBarkTelemetry(
                        bands, snapshot.rmsLevel, bassEnergy, snapshot.beatPulse, 1.0f / 60.0f);
                });
        connect(
            m_ipcClient.get(), &IpcClient::scenePkgExported, this,
            [this](const waywal_ipc_resp_export_scene_pkg_t &resp) {
                const QString stagePath = std::exchange(m_pendingExportStagePath, QString());
                const QString destPath = std::exchange(m_pendingExportDestPath, QString());
                const bool isPkgFile = std::exchange(m_pendingExportIsPkgFile, false);
                if (stagePath.isEmpty() || destPath.isEmpty()) {
                    return;
                }

                auto cleanupStage = [&stagePath]() {
                    std::error_code ec;
                    const std::filesystem::path sp(stagePath.toStdString());
                    std::filesystem::remove_all(sp, ec);
                };

                if (resp.status_code != 0) {
                    cleanupStage();
                    const QString err = QString::fromUtf8(resp.error_msg);
                    if (m_statusLabel) {
                        m_statusLabel->setText(
                            QStringLiteral("Scene export failed: %1")
                                .arg(err.isEmpty() ? QStringLiteral("daemon error") : err));
                    }
                    return;
                }

                std::error_code ec;
                if (isPkgFile) {
                    QString srcFile;
                    if (QFileInfo(stagePath).isFile()) {
                        srcFile = stagePath;
                    } else if (QFileInfo(stagePath + QStringLiteral("/scene.pkg")).isFile()) {
                        srcFile = stagePath + QStringLiteral("/scene.pkg");
                    }
                    if (srcFile.isEmpty()) {
                        cleanupStage();
                        if (m_statusLabel) {
                            m_statusLabel->setText(QStringLiteral(
                                "Scene export completed, but staged .pkg file was missing."));
                        }
                        return;
                    }

                    const QFileInfo destInfo(destPath);
                    destInfo.dir().mkpath(QStringLiteral("."));
                    const std::filesystem::path srcFs(srcFile.toStdString());
                    const std::filesystem::path dstFs(destPath.toStdString());
                    // Rename directly when on the same mount, or copy into a sibling temp
                    // file and rename atomically across filesystems.
                    std::filesystem::rename(srcFs, dstFs, ec);
                    if (ec) {
                        ec.clear();
                        const std::filesystem::path tmpFs(
                            (destPath + QStringLiteral(".tmp")).toStdString());
                        std::filesystem::copy_file(
                            srcFs, tmpFs, std::filesystem::copy_options::overwrite_existing, ec);
                        if (!ec) {
                            std::filesystem::rename(tmpFs, dstFs, ec);
                        }
                        if (ec) {
                            std::error_code rmEc;
                            std::filesystem::remove(tmpFs, rmEc);
                        }
                    }
                    cleanupStage();
                } else {
                    const std::filesystem::path srcDir(stagePath.toStdString());
                    const std::filesystem::path dstDir(destPath.toStdString());
                    std::filesystem::create_directories(dstDir, ec);
                    if (!ec) {
                        std::filesystem::copy(srcDir, dstDir,
                                              std::filesystem::copy_options::recursive |
                                                  std::filesystem::copy_options::overwrite_existing,
                                              ec);
                    }
                    cleanupStage();
                }

                if (m_statusLabel) {
                    if (ec) {
                        m_statusLabel->setText(
                            QStringLiteral("Failed to move exported scene to %1: %2")
                                .arg(destPath, QString::fromStdString(ec.message())));
                    } else {
                        m_statusLabel->setText(
                            QStringLiteral("Exported scene package: %1 (%2 bytes)")
                                .arg(destPath)
                                .arg(resp.total_bytes));
                    }
                }
            });
    }

    if (m_physicsSpringInspector) {
        connect(m_physicsSpringInspector, &PhysicsSpringInspectorWidget::boneDriveModeChanged, this,
                [this](int32_t objectId, int32_t boneIndex, we_bone_drive_mode_t mode) {
                    if (m_syncingBoneDriveMode) {
                        return;
                    }
                    const QScopedValueRollback<bool> guard(m_syncingBoneDriveMode, true);
                    if (m_hasCachedPuppetRig && m_cachedPuppetRig.objectId == objectId &&
                        boneIndex >= 0 && boneIndex < static_cast<int32_t>(WE_MDL_MAX_BONES)) {
                        m_cachedPuppetRig.driveModes[static_cast<size_t>(boneIndex)] = mode;
                    }
                    if (m_boneRiggingPanel) {
                        m_boneRiggingPanel->setBoneDriveMode(objectId, boneIndex, mode);
                    } else if (objectId >= 0 && boneIndex >= 0 &&
                               boneIndex < static_cast<int32_t>(WE_MDL_MAX_BONES)) {
                        m_pendingDriveModes[objectId][boneIndex] = mode;
                    }
                    if (m_statusLabel) {
                        m_statusLabel->setText(QStringLiteral("Bone %1 drive mode: %2")
                                                   .arg(boneIndex)
                                                   .arg(mode == WE_BONE_DRIVE_XPBD
                                                            ? QStringLiteral("XPBD Active")
                                                            : QStringLiteral("FK/Rigid")));
                    }
                });
    }

    if (m_previewViewport) {
        connect(m_previewViewport, &GpuPreviewViewport::contourPinAdded, this,
                [this](int32_t /*objectId*/, double px, double py) {
                    if (m_layoutManager && !m_puppetMeshPanel) {
                        m_layoutManager->realizeLazyPanel(QStringLiteral("PuppetMeshPanel"));
                    }
                    if (m_puppetMeshPanel) {
                        m_puppetMeshPanel->addConstraintPin(px, py);
                        m_previewViewport->setPuppetContourPins(
                            m_puppetMeshPanel->pins(),
                            static_cast<int32_t>(m_puppetMeshPanel->pins().size()) - 1);
                    }
                });

        connect(m_previewViewport, &GpuPreviewViewport::puppetBoneSelected, this,
                [this](int32_t boneIndex) {
                    if (m_boneRiggingPanel) {
                        m_boneRiggingPanel->selectBone(boneIndex);
                    }
                    if (m_weightPaintPanel) {
                        m_weightPaintPanel->setActiveBone(boneIndex);
                    }
                    if (m_physicsSpringInspector) {
                        m_physicsSpringInspector->selectBoneIndex(boneIndex);
                    }
                });

        connect(m_previewViewport, &GpuPreviewViewport::puppetBoneCreated, this,
                [this](int32_t /*objectId*/, int32_t parentBoneIndex, const QPointF &headCanvas,
                       const QPointF & /*tailCanvas*/, float angleZRad) {
                    if (m_layoutManager && !m_boneRiggingPanel) {
                        m_layoutManager->realizeLazyPanel(QStringLiteral("BoneRiggingPanel"));
                    }
                    if (m_boneRiggingPanel) {
                        const float localPos[3] = {static_cast<float>(headCanvas.x()),
                                                   static_cast<float>(headCanvas.y()), 0.0f};
                        const QString name = QStringLiteral("Bone %1").arg(
                            m_boneRiggingPanel->rigSnapshot().bones.size());
                        m_boneRiggingPanel->addBone(name, parentBoneIndex, localPos, angleZRad);
                    }
                });

        connect(m_previewViewport, &GpuPreviewViewport::puppetBoneReparented, this,
                [this](int32_t /*objectId*/, int32_t childBoneIndex, int32_t newParentIndex) {
                    if (m_boneRiggingPanel) {
                        m_boneRiggingPanel->reparentBone(childBoneIndex, newParentIndex);
                    }
                });

        connect(m_previewViewport, &GpuPreviewViewport::puppetIkTargetChanged, this,
                [this](int32_t objectId, int32_t chainIndex, const QVector3D &targetWorld,
                       bool committed) {
                    if (m_ipcClient && objectId >= 0 && chainIndex >= 0) {
                        waywal_ipc_puppet_preview_ik_t req{};
                        req.object_id = static_cast<uint32_t>(objectId);
                        req.root_bone = 0xFFFFu;
                        req.mid_bone = 0xFFFFu;
                        req.end_bone = 0xFFFFu;
                        if (m_boneRiggingPanel) {
                            const auto &chains = m_boneRiggingPanel->rigSnapshot().ikChains;
                            if (static_cast<size_t>(chainIndex) < chains.size()) {
                                const auto &ch = chains[static_cast<size_t>(chainIndex)];
                                req.root_bone = ch.root_bone;
                                req.mid_bone = ch.mid_bone;
                                req.end_bone = ch.end_bone;
                                req.pole_flip = (ch.bend_sign < 0) ? 1u : 0u;
                            }
                        }
                        req.target_world[0] = targetWorld.x();
                        req.target_world[1] = targetWorld.y();
                        req.target_world[2] = targetWorld.z();
                        req.mode_flags = committed ? 5u : 1u;
                        m_ipcClient->sendPuppetPreviewIk(req);
                    }
                    if (committed && m_statusLabel) {
                        m_statusLabel->setText(
                            QStringLiteral("IK chain %1 target updated").arg(chainIndex));
                    }
                });

        connect(m_previewViewport, &GpuPreviewViewport::puppetIkPoleChanged, this,
                [this](int32_t objectId, int32_t chainIndex, const QVector3D & /*poleWorld*/,
                       bool poleFlip, bool committed) {
                    if (m_ipcClient && objectId >= 0 && chainIndex >= 0) {
                        waywal_ipc_puppet_preview_ik_t req{};
                        req.object_id = static_cast<uint32_t>(objectId);
                        req.root_bone = 0xFFFFu;
                        req.mid_bone = 0xFFFFu;
                        req.end_bone = 0xFFFFu;
                        if (m_boneRiggingPanel) {
                            const auto &chains = m_boneRiggingPanel->rigSnapshot().ikChains;
                            if (static_cast<size_t>(chainIndex) < chains.size()) {
                                const auto &ch = chains[static_cast<size_t>(chainIndex)];
                                req.root_bone = ch.root_bone;
                                req.mid_bone = ch.mid_bone;
                                req.end_bone = ch.end_bone;
                                req.target_world[0] = ch.target_world[0];
                                req.target_world[1] = ch.target_world[1];
                                req.target_world[2] = ch.target_world[2];
                            }
                        }
                        req.pole_flip = poleFlip ? 1u : 0u;
                        req.mode_flags = committed ? 5u : 1u;
                        m_ipcClient->sendPuppetPreviewIk(req);
                    }
                    if (committed && m_statusLabel) {
                        m_statusLabel->setText(
                            QStringLiteral("IK chain %1 pole updated").arg(chainIndex));
                    }
                });

        connect(m_previewViewport, &GpuPreviewViewport::weightBrushDabBatchReady, this,
                [this](int32_t objectId, const std::vector<WeightBrushDab> &dabs) {
                    if (!m_ipcClient || objectId < 0 || dabs.empty()) {
                        return;
                    }
                    if (m_previewViewport && m_previewViewport->puppetObjectId() == objectId) {
                        const auto &verts =
                            m_previewViewport->gizmoOverlay().meshWireframe().vertices;
                        if (!verts.empty()) {
                            const float cw = static_cast<float>(
                                std::max<uint32_t>(1u, m_previewViewport->contentWidth()));
                            const float ch = static_cast<float>(
                                std::max<uint32_t>(1u, m_previewViewport->contentHeight()));
                            std::vector<waywal_ipc_vertex_weight_t> syncEntries;
                            for (size_t i = 0; i < verts.size(); ++i) {
                                const float vx = verts[i].pos[0];
                                const float vy = verts[i].pos[1];
                                bool touched = false;
                                for (const auto &dab : dabs) {
                                    const float lx = dab.texelX - 0.5f * cw;
                                    const float ly = 0.5f * ch - dab.texelY;
                                    const float dx = vx - lx;
                                    const float dy = vy - ly;
                                    const float r = std::max(1.0f, dab.radiusPx) + 1.0f;
                                    if (dx * dx + dy * dy <= r * r) {
                                        touched = true;
                                        break;
                                    }
                                }
                                if (touched) {
                                    waywal_ipc_vertex_weight_t vw{};
                                    vw.vertex_index = static_cast<uint32_t>(i);
                                    std::memcpy(vw.blend_indices, verts[i].blend_indices,
                                                sizeof(vw.blend_indices));
                                    std::memcpy(vw.weight, verts[i].weight, sizeof(vw.weight));
                                    syncEntries.push_back(vw);
                                }
                            }
                            if (!syncEntries.empty()) {
                                m_ipcClient->sendPuppetPaintWeights(static_cast<uint32_t>(objectId),
                                                                    1u, syncEntries);
                            }
                        }
                    }
                    if (m_statusLabel) {
                        m_statusLabel->setText(
                            QStringLiteral("Weight brush stroke: %1 dabs").arg(dabs.size()));
                    }
                });

        connect(
            m_previewViewport, &GpuPreviewViewport::weightPaintStrokeCommitted, this,
            [this](int32_t objectId, const std::vector<VertexWeightBlockSnapshot> &preBlocks,
                   const std::vector<VertexWeightBlockSnapshot> &postBlocks,
                   const std::vector<waywal_ipc_vertex_weight_t> &dirtyEntries) {
                if (m_commandBus && (!preBlocks.empty() || !postBlocks.empty())) {
                    QPointer<GpuPreviewViewport> viewportPtr(m_previewViewport);
                    auto cmd = std::make_unique<WeightPaintStrokeCommand>(
                        objectId, QStringLiteral("Weight Paint Stroke"), preBlocks, postBlocks,
                        [this, viewportPtr](int32_t oid,
                                            const std::vector<VertexWeightBlockSnapshot> &blocks) {
                            if (!viewportPtr || oid < 0 || viewportPtr->puppetObjectId() != oid) {
                                return;
                            }
                            viewportPtr->applyVertexWeightBlocks(oid, blocks);
                            if (!m_ipcClient) {
                                return;
                            }
                            std::vector<waywal_ipc_vertex_weight_t> syncEntries;
                            for (const auto &blk : blocks) {
                                const uint32_t base = blk.blockIndex * 256u;
                                for (size_t i = 0; i < blk.slice.size(); ++i) {
                                    waywal_ipc_vertex_weight_t vw{};
                                    vw.vertex_index = base + static_cast<uint32_t>(i);
                                    std::memcpy(vw.blend_indices, blk.slice[i].blend_indices,
                                                sizeof(vw.blend_indices));
                                    std::memcpy(vw.weight, blk.slice[i].weight, sizeof(vw.weight));
                                    syncEntries.push_back(vw);
                                }
                            }
                            if (!syncEntries.empty()) {
                                m_ipcClient->sendPuppetPaintWeights(static_cast<uint32_t>(oid), 1u,
                                                                    syncEntries);
                            }
                        });
                    m_commandBus->execute(std::move(cmd));
                } else if (m_ipcClient && objectId >= 0 && !dirtyEntries.empty()) {
                    m_ipcClient->sendPuppetPaintWeights(static_cast<uint32_t>(objectId), 1u,
                                                        dirtyEntries);
                }
            });
    }

    if (m_ipcClient) {
        connect(m_ipcClient.get(), &IpcClient::puppetMeshAckReceived, this,
                [this](const waywal_ipc_puppet_mesh_ack_t &resp) {
                    if (m_statusLabel) {
                        m_statusLabel->setText(
                            QStringLiteral("Puppet mesh synced: %1 verts, %2 tris")
                                .arg(resp.vertex_count)
                                .arg(resp.triangle_count));
                    }
                });
        connect(m_ipcClient.get(), &IpcClient::puppetWeightsSolved, this,
                [this](const waywal_ipc_puppet_weights_resp_t &resp,
                       const std::vector<waywal_ipc_vertex_weight_t> &entries) {
                    if (resp.status == 0 && m_previewViewport && !entries.empty()) {
                        m_previewViewport->applySolvedVertexWeights(
                            static_cast<int32_t>(resp.object_id), entries);
                    }
                    if (m_weightPaintPanel) {
                        const uint32_t boneCount =
                            (m_hasCachedPuppetRig &&
                             m_cachedPuppetRig.objectId == static_cast<int32_t>(resp.object_id))
                                ? static_cast<uint32_t>(m_cachedPuppetRig.bones.size())
                                : 0u;
                        m_weightPaintPanel->setSolveTelemetry(resp.entry_count, boneCount, 0.0f,
                                                              resp.status != 0);
                    }
                    if (m_statusLabel) {
                        m_statusLabel->setText(
                            QStringLiteral("Solved heat weights (%1 entries, status=%2)")
                                .arg(resp.entry_count)
                                .arg(resp.status));
                    }
                });
        connect(m_ipcClient.get(), &IpcClient::puppetIkPoseReceived, this,
                [this](const waywal_ipc_puppet_ik_pose_resp_t &resp,
                       const std::vector<waywal_ipc_bone_trs_t> &bones) {
                    const int32_t oid = static_cast<int32_t>(resp.object_id);
                    if (resp.status == 0 && !bones.empty() && m_hasCachedPuppetRig &&
                        m_cachedPuppetRig.objectId == oid) {
                        BoneHierarchySnapshot posedRig = m_cachedPuppetRig;
                        for (const auto &b : bones) {
                            if (b.bone_index >= posedRig.bones.size()) {
                                continue;
                            }
                            auto &dst = posedRig.bones[b.bone_index];
                            for (int k = 0; k < 3; ++k) {
                                dst.bind_pos[k] = std::isfinite(b.local_pos[k]) ? b.local_pos[k]
                                                                                : dst.bind_pos[k];
                                dst.bind_angle[k] = std::isfinite(b.local_angle[k])
                                                        ? b.local_angle[k]
                                                        : dst.bind_angle[k];
                                dst.bind_scale[k] = std::isfinite(b.local_scale[k])
                                                        ? b.local_scale[k]
                                                        : dst.bind_scale[k];
                            }
                            composeBoneLocalMat4(dst.bind_pos, dst.bind_angle, dst.bind_scale,
                                                 dst.local_trans);
                        }
                        if (m_previewViewport && m_previewViewport->puppetObjectId() == oid) {
                            const int32_t selBone =
                                m_boneRiggingPanel ? m_boneRiggingPanel->selectedBoneIndex() : -1;
                            m_previewViewport->setPuppetSkeleton(posedRig, selBone);
                        }
                    }
                    if (m_statusLabel) {
                        m_statusLabel->setText(
                            QStringLiteral("IK pose updated (%1 bones)").arg(resp.bone_count));
                    }
                });
        connect(
            m_ipcClient.get(), &IpcClient::puppetMdlExported, this,
            [this](const waywal_ipc_export_puppet_mdl_resp_t &resp, const QByteArray &mdlBytes) {
                QString destPath;
                const auto it = m_pendingMdlExportPaths.find(static_cast<int32_t>(resp.object_id));
                if (it != m_pendingMdlExportPaths.end()) {
                    destPath = it->second;
                    m_pendingMdlExportPaths.erase(it);
                }
                if (resp.status != 0) {
                    if (m_statusLabel) {
                        m_statusLabel->setText(
                            QStringLiteral("Puppet MDL export failed (object %1, status=%2)")
                                .arg(resp.object_id)
                                .arg(resp.status));
                    }
                    return;
                }
                if (!destPath.isEmpty() && !mdlBytes.isEmpty()) {
                    const QFileInfo destInfo(destPath);
                    destInfo.dir().mkpath(QStringLiteral("."));
                    QSaveFile out(destPath);
                    if (!out.open(QIODevice::WriteOnly) || out.write(mdlBytes) != mdlBytes.size() ||
                        !out.commit()) {
                        if (m_statusLabel) {
                            m_statusLabel->setText(
                                QStringLiteral("Failed to write puppet MDL to %1").arg(destPath));
                        }
                        return;
                    }
                    if (m_statusLabel) {
                        m_statusLabel->setText(
                            QStringLiteral("Exported puppet MDL to %1 (%2 bytes)")
                                .arg(destPath)
                                .arg(mdlBytes.size()));
                    }
                    return;
                }
                if (m_statusLabel) {
                    m_statusLabel->setText(
                        QStringLiteral("Exported puppet MDL (object %1, %2 bytes)")
                            .arg(resp.object_id)
                            .arg(resp.mdl_byte_size));
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

void MainWindow::handleExportScenePackage()
{
    const QString destPath = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export Scene Package"), m_activeWallpaperPath,
        QStringLiteral("Scene Packages (*.pkg *.wwpkg);;All Files (*)"));
    if (destPath.isEmpty()) {
        return;
    }

    const QString lowerDest = destPath.toLower();
    const QFileInfo srcInfo(m_activeWallpaperPath);
    const QString lowerSrc = m_activeWallpaperPath.toLower();

    // Landlock-confined wwald can only write exports under the cache staging root,
    // never directly into user wallpaper or config directories.
    if (!lowerDest.endsWith(QStringLiteral(".wwpkg")) && m_ipcClient &&
        m_ipcClient->isConnected()) {
        QString cacheRoot = qEnvironmentVariable("XDG_CACHE_HOME");
        if (cacheRoot.isEmpty()) {
            cacheRoot = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
        }
        if (cacheRoot.isEmpty()) {
            cacheRoot = QDir::homePath() + QStringLiteral("/.cache");
        }
        const QString exportStageRoot = cacheRoot + QStringLiteral("/wwal/exports");
        if (!QDir().mkpath(exportStageRoot)) {
            if (m_statusLabel) {
                m_statusLabel->setText(
                    QStringLiteral("Failed to create export staging directory: %1")
                        .arg(exportStageRoot));
            }
            return;
        }

        static uint64_t s_exportSeq = 0;
        const bool isPkgFile = lowerDest.endsWith(QStringLiteral(".pkg"));
        const QString tag = QStringLiteral("scene_%1_%2")
                                .arg(QCoreApplication::applicationPid())
                                .arg(++s_exportSeq);
        const QString stagePath =
            isPkgFile ? (exportStageRoot + QLatin1Char('/') + tag + QStringLiteral(".pkg"))
                      : (exportStageRoot + QLatin1Char('/') + tag);

        const uint32_t exportFlags =
            isPkgFile ? (WE_EXPORT_PKG_FLAG_PACK_SCENE_PKG | WE_EXPORT_PKG_FLAG_DIRECT_PKG_FILE)
                      : 0u;

        m_pendingExportStagePath = stagePath;
        m_pendingExportDestPath = destPath;
        m_pendingExportIsPkgFile = isPkgFile;

        const QString outName =
            m_previewViewport ? m_previewViewport->selectedMonitor() : QStringLiteral("*");
        if (!m_ipcClient->sendExportScenePkg(outName, stagePath, exportFlags)) {
            m_pendingExportStagePath.clear();
            m_pendingExportDestPath.clear();
            m_pendingExportIsPkgFile = false;
            if (m_statusLabel) {
                m_statusLabel->setText(QStringLiteral("Failed to dispatch scene package export."));
            }
            return;
        }
        if (m_statusLabel) {
            m_statusLabel->setText(QStringLiteral("Exporting scene package: %1").arg(destPath));
        }
        return;
    }

    auto *wizard = new CreatorPackagingWizard(this);
    wizard->setAttribute(Qt::WA_DeleteOnClose);
    if (!m_activeWallpaperPath.isEmpty() &&
        (srcInfo.isDir() || lowerSrc.endsWith(QStringLiteral(".pkg")) ||
         lowerSrc.endsWith(QStringLiteral(".tex")) ||
         lowerSrc.endsWith(QStringLiteral("project.json")))) {
        wizard->setSourcePath(m_activeWallpaperPath);
    }
    wizard->show();
}

void MainWindow::handleViewportToolModeChanged(int index)
{
    if (!m_previewViewport || index < 0 || index > 11) {
        return;
    }
    const auto mode = static_cast<ViewportToolMode>(index);
    m_previewViewport->setToolMode(mode);
    if (!m_layoutManager) {
        return;
    }
    if (index >= static_cast<int>(ViewportToolMode::BrushMask) &&
        index <= static_cast<int>(ViewportToolMode::BrushBlurSmudge)) {
        m_layoutManager->realizeLazyPanel(QStringLiteral("GpuBrushPainter"));
    } else if (mode == ViewportToolMode::ContourPin) {
        m_layoutManager->realizeLazyPanel(QStringLiteral("PuppetMeshPanel"));
    } else if (mode == ViewportToolMode::BonePlace || mode == ViewportToolMode::IkDrag) {
        m_layoutManager->realizeLazyPanel(QStringLiteral("BoneRiggingPanel"));
    } else if (mode == ViewportToolMode::WeightBrush) {
        m_layoutManager->realizeLazyPanel(QStringLiteral("WeightPaintPanel"));
    }
}

void MainWindow::setActivePuppetObjectContext(int32_t objectId, uint32_t texWidth,
                                              uint32_t texHeight)
{
    if (objectId < 0) {
        return;
    }
    const bool objectChanged = (m_activePuppetObjectId != objectId);
    m_activePuppetObjectId = objectId;
    if (texWidth > 0u) {
        m_activePuppetTexWidth = texWidth;
    }
    if (texHeight > 0u) {
        m_activePuppetTexHeight = texHeight;
    }
    if (m_previewViewport) {
        m_previewViewport->setPuppetObjectId(objectId);
    }
    if (m_puppetMeshPanel) {
        m_puppetMeshPanel->setTargetObjectId(objectId, m_activePuppetTexWidth,
                                             m_activePuppetTexHeight);
    }
    if (objectChanged) {
        m_timelineHydratedObjectId = -1;
        if (m_weightPaintPanel) {
            hydrateWeightPaintPanel(m_weightPaintPanel);
        }
        if (m_dopeSheetTimeline) {
            hydrateDopeSheetTimeline(m_dopeSheetTimeline);
        }
    }
}

void MainWindow::hydrateWeightPaintPanel(WeightPaintPanel *panel) const
{
    if (!panel || m_activePuppetObjectId < 0) {
        return;
    }

    std::vector<we_mdl_vertex_t> verts;
    std::vector<uint16_t> indices;
    if (m_previewViewport && m_previewViewport->puppetObjectId() == m_activePuppetObjectId &&
        !m_previewViewport->gizmoOverlay().meshWireframe().vertices.empty()) {
        verts = m_previewViewport->gizmoOverlay().meshWireframe().vertices;
        indices = m_previewViewport->gizmoOverlay().meshWireframe().indices;
    } else if (m_cachedPuppetMesh.objectId == m_activePuppetObjectId) {
        verts = m_cachedPuppetMesh.vertices;
        indices = m_cachedPuppetMesh.indices;
    }

    we_puppet_t puppet{};
    puppet.vertex_count = static_cast<uint32_t>(verts.size());
    puppet.vertices = verts.empty() ? nullptr : verts.data();
    puppet.triangle_count = static_cast<uint32_t>(indices.size() / 3u);
    puppet.indices = indices.empty() ? nullptr : indices.data();

    std::vector<we_mdl_bone_t> bones;
    if (m_hasCachedPuppetRig && m_cachedPuppetRig.objectId == m_activePuppetObjectId) {
        bones = m_cachedPuppetRig.bones;
        puppet.bone_count = static_cast<uint32_t>(bones.size());
        puppet.bones = bones.empty() ? nullptr : bones.data();
    }

    panel->setPuppetContext(m_activePuppetObjectId, &puppet);
    if (m_boneRiggingPanel &&
        m_boneRiggingPanel->rigSnapshot().objectId == m_activePuppetObjectId &&
        m_boneRiggingPanel->selectedBoneIndex() >= 0) {
        panel->setActiveBone(m_boneRiggingPanel->selectedBoneIndex());
    } else if (puppet.bone_count > 0u) {
        panel->setActiveBone(0);
    }
}

void MainWindow::hydrateDopeSheetTimeline(DopeSheetTimelineWidget *timeline)
{
    if (!timeline || m_activePuppetObjectId < 0) {
        return;
    }

    const bool sameObject = (m_timelineHydratedObjectId == m_activePuppetObjectId);
    const auto &existingClips = timeline->clips();
    const bool hasEditedChannels =
        sameObject && std::any_of(existingClips.begin(), existingClips.end(),
                                  [](const TimelineAnimClip &c) { return !c.channels.empty(); });
    if (hasEditedChannels) {
        return;
    }

    we_puppet_t puppet{};
    std::vector<we_mdl_bone_t> bones;
    if (m_hasCachedPuppetRig && m_cachedPuppetRig.objectId == m_activePuppetObjectId) {
        bones = m_cachedPuppetRig.bones;
        puppet.bone_count = static_cast<uint32_t>(bones.size());
        puppet.bones = bones.empty() ? nullptr : bones.data();
    }
    timeline->loadPuppetAnimations(&puppet, m_activePuppetObjectId);
    m_timelineHydratedObjectId = m_activePuppetObjectId;
}

void MainWindow::refreshOnionSkinFrames()
{
    if (!m_previewViewport) {
        return;
    }
    if (!m_onionSkinEnabled || !m_dopeSheetTimeline || !m_hasCachedPuppetRig ||
        m_cachedPuppetRig.bones.empty() || m_dopeSheetTimeline->clips().empty()) {
        m_previewViewport->setOnionSkinFrames({});
        return;
    }

    const auto &clips = m_dopeSheetTimeline->clips();
    const TimelineAnimClip *activeClip = &clips.front();
    for (const auto &c : clips) {
        if (c.layerVisible && !c.channels.empty()) {
            activeClip = &c;
            break;
        }
    }

    const int32_t lenFrames = std::max(1, activeClip->lengthFrames);
    const double maxFrame = static_cast<double>(lenFrames - 1);
    const double curFrame = m_dopeSheetTimeline->currentFrame();
    const size_t boneCount = m_cachedPuppetRig.bones.size();

    auto wrapSampleFrame = [activeClip, lenFrames, maxFrame](double f) -> double {
        if (maxFrame <= 0.0) {
            return 0.0;
        }
        if (activeClip->mode == WE_ANIM_LOOP) {
            double wrapped = std::fmod(f, static_cast<double>(lenFrames));
            if (wrapped < 0.0) {
                wrapped += static_cast<double>(lenFrames);
            }
            return std::clamp(wrapped, 0.0, maxFrame);
        }
        if (activeClip->mode == WE_ANIM_MIRROR) {
            const double span = maxFrame * 2.0;
            double wrapped = std::fmod(f, span);
            if (wrapped < 0.0) {
                wrapped += span;
            }
            return (wrapped > maxFrame) ? (span - wrapped) : wrapped;
        }
        return std::clamp(f, 0.0, maxFrame);
    };

    std::vector<GizmoOnionGhostFrame> frames;
    frames.reserve(static_cast<size_t>(m_onionBeforeFrames + m_onionAfterFrames));
    for (int delta = -m_onionBeforeFrames; delta <= m_onionAfterFrames; ++delta) {
        if (delta == 0) {
            continue;
        }
        const double sampleFrame = wrapSampleFrame(curFrame + static_cast<double>(delta));
        std::vector<std::array<float, 16>> localMats(boneCount);
        for (size_t bIdx = 0; bIdx < boneCount; ++bIdx) {
            const auto &srcBone = m_cachedPuppetRig.bones[bIdx];
            float pos[3] = {srcBone.bind_pos[0], srcBone.bind_pos[1], srcBone.bind_pos[2]};
            float angle[3] = {srcBone.bind_angle[0], srcBone.bind_angle[1], srcBone.bind_angle[2]};
            float scale[3] = {srcBone.bind_scale[0], srcBone.bind_scale[1], srcBone.bind_scale[2]};
            for (const auto &ch : activeClip->channels) {
                if (ch.boneIndex != static_cast<int32_t>(bIdx)) {
                    continue;
                }
                const double val = DopeSheetTimelineWidget::evaluateChannelAtFrame(ch, sampleFrame);
                if (activeClip->blendMode == WE_ANIM_BLEND_ADDITIVE) {
                    if (ch.channelIndex < 3u) {
                        pos[ch.channelIndex] += static_cast<float>(val);
                    } else if (ch.channelIndex < 6u) {
                        angle[ch.channelIndex - 3u] += static_cast<float>(val);
                    } else if (ch.channelIndex < 9u) {
                        scale[ch.channelIndex - 6u] *= static_cast<float>(val);
                    }
                } else {
                    if (ch.channelIndex < 3u) {
                        pos[ch.channelIndex] = static_cast<float>(val);
                    } else if (ch.channelIndex < 6u) {
                        angle[ch.channelIndex - 3u] = static_cast<float>(val);
                    } else if (ch.channelIndex < 9u) {
                        scale[ch.channelIndex - 6u] = static_cast<float>(val);
                    }
                }
            }
            composeBoneLocalMat4(pos, angle, scale, localMats[bIdx].data());
        }

        const auto worldMats = resolveBoneWorldMatrices(m_cachedPuppetRig.bones, localMats);
        std::vector<std::pair<QVector3D, QVector3D>> segments(boneCount);
        std::vector<bool> hasChild(boneCount, false);
        for (size_t i = 0; i < boneCount; ++i) {
            segments[i].first = QVector3D(worldMats[i][12], worldMats[i][13], worldMats[i][14]);
        }
        for (size_t i = 0; i < boneCount; ++i) {
            const uint32_t p = m_cachedPuppetRig.bones[i].parent;
            if (p != 0xFFFFFFFFu && p < boneCount) {
                hasChild[p] = true;
                segments[p].second = segments[i].first;
            }
        }
        for (size_t i = 0; i < boneCount; ++i) {
            if (!hasChild[i]) {
                const QVector3D axisX(worldMats[i][0], worldMats[i][1], worldMats[i][2]);
                const QVector3D dir = (axisX.lengthSquared() > 1e-5f) ? axisX.normalized()
                                                                      : QVector3D(1.0f, 0.0f, 0.0f);
                segments[i].second = segments[i].first + dir * 28.0f;
            }
        }

        GizmoOnionGhostFrame ghost{};
        ghost.frameDelta = delta;
        ghost.opacity =
            std::clamp(m_onionOpacity / static_cast<float>(std::abs(delta)), 0.08f, 0.85f);
        ghost.boneSegments = std::move(segments);
        frames.push_back(std::move(ghost));
    }
    m_previewViewport->setOnionSkinFrames(std::move(frames));
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
