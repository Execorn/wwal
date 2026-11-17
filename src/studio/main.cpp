// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/app/main_window.hpp"
#include "studio/app/dock_layout_manager.hpp"
#include "studio/app/studio_tokens.hpp"
#include "studio/grid/virtualized_grid_widget.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QLoggingCategory>
#include <QMouseEvent>
#include <QPixmapCache>
#include <QSurfaceFormat>
#include <QTabBar>
#include <QThread>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <malloc.h>
#include <string_view>
#include <unistd.h>

using namespace waywal::studio;

static std::atomic<bool> g_isOffscreenPlatform{false};

#if defined(__SANITIZE_ADDRESS__)
#define WAYWAL_STUDIO_ASAN_BUILD 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define WAYWAL_STUDIO_ASAN_BUILD 1
#endif
#endif

static bool readSmapsRollupRssAnonMiB(double &outRssAnonMb, double &outVmRssMb)
{
    outRssAnonMb = 0.0;
    outVmRssMb = 0.0;

    bool foundAnon = false;
    bool foundRss = false;
    {
        std::ifstream smapsFile("/proc/self/smaps_rollup");
        std::string line;
        while (std::getline(smapsFile, line)) {
            if (line.rfind("Anonymous:", 0) == 0) {
                long kb = 0;
                if (sscanf(line.c_str(), "Anonymous: %ld kB", &kb) == 1) {
                    outRssAnonMb = static_cast<double>(kb) / 1024.0;
                    foundAnon = true;
                }
            } else if (line.rfind("Rss:", 0) == 0) {
                long kb = 0;
                if (sscanf(line.c_str(), "Rss: %ld kB", &kb) == 1) {
                    outVmRssMb = static_cast<double>(kb) / 1024.0;
                    foundRss = true;
                }
            }
        }
    }

    if (!foundAnon || !foundRss) {
        std::ifstream statusFile("/proc/self/status");
        std::string line;
        while (std::getline(statusFile, line)) {
            if (!foundAnon && line.rfind("RssAnon:", 0) == 0) {
                long kb = 0;
                if (sscanf(line.c_str(), "RssAnon: %ld kB", &kb) == 1) {
                    outRssAnonMb = static_cast<double>(kb) / 1024.0;
                    foundAnon = true;
                }
            } else if (!foundRss && line.rfind("VmRSS:", 0) == 0) {
                long kb = 0;
                if (sscanf(line.c_str(), "VmRSS: %ld kB", &kb) == 1) {
                    outVmRssMb = static_cast<double>(kb) / 1024.0;
                    foundRss = true;
                }
            }
        }
    }
    return foundAnon;
}

static int runHeadlessCheck(const StudioOptions &options,
                            std::chrono::steady_clock::time_point startupT0)
{
    // Prevent secondary browser panels from launching disk/SQLite scan threads during headless
    // runs.
    qputenv("WWAL_STUDIO_HEADLESS_CHECK", "1");

    std::cout << "========================================================" << std::endl;
    std::cout << "[wwal-studio] Starting Headless Verification Suite ("
              << (options.realizeAllPanels ? "22-panel full realization" : "4-panel core launch")
              << ")" << std::endl;
    std::cout << "========================================================" << std::endl;

    // 1. Instantiate core MainWindow in offscreen mode
    MainWindow window(options);
    DockLayoutManager *layoutMgr = window.dockLayoutManager();
    if (!layoutMgr) {
        std::cerr << "[-] FAILED: DockLayoutManager is null!" << std::endl;
        return 1;
    }
    std::cout << "[+] MainWindow and DockLayoutManager successfully instantiated." << std::endl;

    // 2. Validate Panel Registration across all 22 registered IDs and confirm lazy deferral
    const QStringList panelIds = layoutMgr->registeredPanelIds();
    std::cout << "[+] Registered panels (" << panelIds.size()
              << "): " << panelIds.join(", ").toStdString() << std::endl;

    if (panelIds.size() != 22) {
        std::cerr << "[-] FAILED: Expected 22 registered panels, got " << panelIds.size()
                  << std::endl;
        return 1;
    }

    const QStringList corePanels = {
        QStringLiteral("Wallpapers"),         QStringLiteral("GpuPreviewViewport"),
        QStringLiteral("CubicBezier"),        QStringLiteral("PropertyInspectorWidget"),
        QStringLiteral("MonitorTopologyBar"), QStringLiteral("StudioFooterBar")};

    const QStringList lazyPanels = {QStringLiteral("WorkshopBrowser"),
                                    QStringLiteral("OpenHubBrowser"),
                                    QStringLiteral("ThemeStudioWidget"),
                                    QStringLiteral("NodeGraphEditorWidget"),
                                    QStringLiteral("EffectLibraryBrowserWidget"),
                                    QStringLiteral("CameraLightingInspectorWidget"),
                                    QStringLiteral("PhysicsSpringInspectorWidget"),
                                    QStringLiteral("SceneLayerTree"),
                                    QStringLiteral("GpuBrushPainter"),
                                    QStringLiteral("ParticleDesigner"),
                                    QStringLiteral("AudioBindingWizard"),
                                    QStringLiteral("GlslSceneScriptEditor"),
                                    QStringLiteral("PuppetMeshPanel"),
                                    QStringLiteral("BoneRiggingPanel"),
                                    QStringLiteral("WeightPaintPanel"),
                                    QStringLiteral("DopeSheetTimelineWidget")};

    for (const auto &expected : corePanels) {
        if (!panelIds.contains(expected) || !layoutMgr->dockWidget(expected)) {
            std::cerr << "[-] FAILED: Core panel '" << expected.toStdString() << "' is missing!"
                      << std::endl;
            return 1;
        }
    }
    for (const auto &lazyId : lazyPanels) {
        if (!panelIds.contains(lazyId) || !layoutMgr->dockWidget(lazyId)) {
            std::cerr << "[-] FAILED: Missing secondary lazy panel '" << lazyId.toStdString()
                      << "'!" << std::endl;
            return 1;
        }
        if (layoutMgr->isPanelRealized(lazyId)) {
            std::cerr << "[-] FAILED: Secondary panel '" << lazyId.toStdString()
                      << "' realized prematurely before window reveal!" << std::endl;
            return 1;
        }
    }
    std::cout << "[+] All 22 registered panels (4 core + 2 bars eager, 16 secondary lazy) verified."
              << std::endl;

    // 3. Validate Available Perspectives and JSON Serialization Schema
    const QStringList perspectives = layoutMgr->availablePerspectives();
    const QStringList requiredPerspectives = {QStringLiteral("Default"),
                                              QStringLiteral("Shader Coding"),
                                              QStringLiteral("Scene Composer"),
                                              QStringLiteral("Particle & Audio"),
                                              QStringLiteral("Theme Studio"),
                                              QStringLiteral("Multi-Monitor"),
                                              QStringLiteral("Rigging & Animation")};

    for (const auto &req : requiredPerspectives) {
        if (!perspectives.contains(req)) {
            std::cerr << "[-] FAILED: Missing required perspective: " << req.toStdString()
                      << std::endl;
            return 1;
        }
    }

    const QJsonObject layoutJson = layoutMgr->serializeLayout();
    if (layoutJson.value(QStringLiteral("version")).toInt() != 1 ||
        layoutJson.value(QStringLiteral("layout_revision")).toInt() != 7 ||
        layoutJson.value(QStringLiteral("active_perspective")).toString().isEmpty()) {
        std::cerr << "[-] FAILED: JSON layout metadata mismatch!" << std::endl;
        return 1;
    }

    const QJsonObject perspsObj = layoutJson.value(QStringLiteral("perspectives")).toObject();
    for (const auto &req : requiredPerspectives) {
        if (!perspsObj.contains(req)) {
            std::cerr << "[-] FAILED: Serialized JSON missing perspective: " << req.toStdString()
                      << std::endl;
            return 1;
        }
    }
    const QJsonObject defTree = perspsObj.value(QStringLiteral("Default")).toObject();
    if (!defTree.contains(QStringLiteral("split_direction")) ||
        !defTree.contains(QStringLiteral("split_ratio")) ||
        !defTree.contains(QStringLiteral("left")) || !defTree.contains(QStringLiteral("right"))) {
        std::cerr << "[-] FAILED: 'Default' perspective root does not match expected split schema!"
                  << std::endl;
        return 1;
    }
    std::cout << "[+] Perspectives (" << perspectives.size()
              << ") and JSON layout serialization verified." << std::endl;

    // 4. Test File Load/Save Round-Trip and Global Actions
    const QString testFilePath = QStringLiteral("/tmp/wwal_headless_layout_test_%1.json")
                                     .arg(QCoreApplication::applicationPid());
    if (!layoutMgr->saveLayoutToFile(testFilePath) || !QFile::exists(testFilePath) ||
        QFileInfo(testFilePath).size() <= 0) {
        QFile::remove(testFilePath);
        std::cerr << "[-] FAILED layout file save round-trip!" << std::endl;
        return 1;
    }
    const bool loaded = layoutMgr->loadLayoutFromFile(testFilePath);
    QFile::remove(testFilePath);
    if (!loaded) {
        std::cerr << "[-] FAILED: Unable to reload saved layout JSON file!" << std::endl;
        return 1;
    }

    if (!window.undoAction() || !window.redoAction() || !window.searchFocusAction() ||
        !window.playPauseAction() || !window.fullscreenAction()) {
        std::cerr << "[-] FAILED: One or more global studio actions are null!" << std::endl;
        return 1;
    }

    // 5. Optional directory scan (explicit -d) when user supplies wallpaper dirs.
    if (window.gridWidget() && !options.wallpaperDirs.isEmpty()) {
        auto &idx = window.gridWidget()->catalogIndex();
        (void)idx.loadConfigFromFile();
        std::vector<std::string> dirs;
        for (const auto &d : options.wallpaperDirs) {
            dirs.push_back(d.toStdString());
        }
        idx.setIncludedDirectories(dirs);
        idx.setScanSystemAndWorkshopPaths(false);
        if (!options.excludeDirs.isEmpty()) {
            for (const auto &ex : options.excludeDirs) {
                idx.addExcludedDirectory(ex.toStdString());
            }
        }
        window.gridWidget()->scanWallpapers();
        window.gridWidget()->populateSampleCatalogIfEmpty(false);
        window.gridWidget()->selectFirstItemIfAvailable();
    }

    // 6. Execute interactive window show path and measure cold startup latency + smaps_rollup
    // RssAnon.
    window.resize(1600, 960);
    window.show();
    for (int i = 0; i < 4; ++i) {
        QCoreApplication::processEvents();
    }
    const double startupMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startupT0)
            .count();

    auto *dWallpapers = layoutMgr->dockWidget(QStringLiteral("Wallpapers"));
    auto *dPreview = layoutMgr->dockWidget(QStringLiteral("GpuPreviewViewport"));
    auto *dBezier = layoutMgr->dockWidget(QStringLiteral("CubicBezier"));
    auto *dInspector = layoutMgr->dockWidget(QStringLiteral("PropertyInspectorWidget"));
    auto *dTheme = layoutMgr->dockWidget(QStringLiteral("ThemeStudioWidget"));
    auto *dTopo = layoutMgr->dockWidget(QStringLiteral("MonitorTopologyBar"));
    auto *dFooter = layoutMgr->dockWidget(QStringLiteral("StudioFooterBar"));

    const QStringList geometryReportPanels = {
        QStringLiteral("Wallpapers"),        QStringLiteral("GpuPreviewViewport"),
        QStringLiteral("CubicBezier"),       QStringLiteral("PropertyInspectorWidget"),
        QStringLiteral("ThemeStudioWidget"), QStringLiteral("MonitorTopologyBar"),
        QStringLiteral("StudioFooterBar")};
    std::cout << "[+] Default Perspective Geometries @ 1600x960:" << std::endl;
    for (const auto &id : geometryReportPanels) {
        if (auto *d = layoutMgr->dockWidget(id)) {
            const QRect g = d->geometry();
            std::cout << "  - " << id.toStdString() << ": (" << g.x() << ", " << g.y() << ", "
                      << g.width() << "x" << g.height() << ") visible=" << d->isVisible()
                      << " realized=" << layoutMgr->isPanelRealized(id) << std::endl;
        }
    }

    if (dWallpapers->width() < 220 || dPreview->width() < 500 || dInspector->width() < 300) {
        std::cerr
            << "[-] FAILED: Default perspective columns did not scale proportionally at 1600x960!"
            << std::endl;
        return 1;
    }
    if (dBezier->geometry().top() < dPreview->geometry().bottom()) {
        std::cerr << "[-] FAILED: CubicBezier overlaps GpuPreviewViewport vertically!" << std::endl;
        return 1;
    }
    if (dWallpapers->geometry().right() > dPreview->geometry().left() ||
        dPreview->geometry().right() > dInspector->geometry().left()) {
        std::cerr << "[-] FAILED: Horizontal dock columns overlap in Default perspective!"
                  << std::endl;
        return 1;
    }
    if (dTopo->width() < 1500 || dFooter->width() < 1500) {
        std::cerr << "[-] FAILED: Top/Bottom bars do not span full window width!" << std::endl;
        return 1;
    }

    auto checkMultiTabBarsVisible = [&window](const char *contextLabel,
                                              int expectedMinTabs = 2) -> bool {
        int activeTabGroups = 0;
        const auto tabBars = window.findChildren<QTabBar *>(QString(), Qt::FindDirectChildrenOnly);
        for (QTabBar *tb : tabBars) {
            if (!tb)
                continue;
            if (tb->y() < 45) {
                if (tb->isVisible()) {
                    std::cerr << "[-] FAILED (" << contextLabel << "): Parked QTabBar at ("
                              << tb->x() << ", " << tb->y() << ") overlaps menu bar!" << std::endl;
                    return false;
                }
                continue;
            }
            if (tb->count() >= expectedMinTabs) {
                ++activeTabGroups;
                if (!tb->isVisible() || tb->width() < 100 || tb->height() < 16) {
                    std::cerr << "[-] FAILED (" << contextLabel
                              << "): Multi-tab QTabBar (count=" << tb->count()
                              << ") is hidden or collapsed (" << tb->width() << "x" << tb->height()
                              << ")!" << std::endl;
                    return false;
                }
            }
        }
        if (activeTabGroups == 0) {
            std::cerr << "[-] FAILED (" << contextLabel
                      << "): Expected at least 1 multi-tab QTabBar (count >= " << expectedMinTabs
                      << ")!" << std::endl;
            return false;
        }
        return true;
    };

    if (!checkMultiTabBarsVisible("Default 1600x960 initial")) {
        return 1;
    }

    if (!options.realizeAllPanels) {
        for (const auto &lazyId : lazyPanels) {
            if (layoutMgr->isPanelRealized(lazyId)) {
                std::cerr << "[-] FAILED: Parked lazy panel '" << lazyId.toStdString()
                          << "' realized during Default show()!" << std::endl;
                return 1;
            }
        }
    }

    double baseRssAnonMb = 0.0;
    double baseVmRssMb = 0.0;
    readSmapsRollupRssAnonMiB(baseRssAnonMb, baseVmRssMb);
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "[+] Startup Latency: " << startupMs << " ms (Budget: <= 350.00 ms)" << std::endl;
    std::cout << "[+] Core 4-panel RssAnon (/proc/self/smaps_rollup): " << baseRssAnonMb
              << " MB (VmRSS: " << baseVmRssMb << " MB, Budget: < 55.00 MB)" << std::endl;

#if !defined(WAYWAL_STUDIO_ASAN_BUILD)
    if (baseRssAnonMb >= 55.0) {
        std::cerr << "[-] FAILED: 4-panel RssAnon (" << baseRssAnonMb
                  << " MB) exceeded 55.0 MB budget!" << std::endl;
        return 1;
    }
#endif

    const QString screenshotDir = qEnvironmentVariable("WWAL_STUDIO_SCREENSHOT_DIR");
    if (!screenshotDir.isEmpty()) {
        window.grab().save(screenshotDir + QStringLiteral("/default_1600x960.png"));
    }

    if (options.realizeAllPanels) {
        // Reveal inactive ThemeStudio tab, check dynamic 1024x720 scaling, sweep perspectives,
        // and realize every registered dock to measure worst-case 22-panel footprint.
        layoutMgr->setPanelVisible(QStringLiteral("ThemeStudioWidget"), true);
        for (int i = 0; i < 4; ++i)
            QCoreApplication::processEvents();
        if (dTheme->x() <= 0 || dTheme->y() <= 0 ||
            !layoutMgr->isPanelRealized(QStringLiteral("ThemeStudioWidget"))) {
            std::cerr << "[-] FAILED: ThemeStudioWidget tab did not raise or realize on screen: ("
                      << dTheme->x() << ", " << dTheme->y() << ")!" << std::endl;
            return 1;
        }
        {
            QMouseEvent releaseEv(QEvent::MouseButtonRelease, QPointF(100, 100), QPointF(100, 100),
                                  Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(&window, &releaseEv);
        }
        for (int i = 0; i < 2; ++i)
            QCoreApplication::processEvents();

        if (!checkMultiTabBarsVisible("Default 1600x960 after raising ThemeStudioWidget tab")) {
            return 1;
        }
        if (!screenshotDir.isEmpty()) {
            window.grab().save(screenshotDir +
                               QStringLiteral("/default_themestudio_tab_1600x960.png"));
        }

        window.resize(1024, 720);
        for (int i = 0; i < 4; ++i)
            QCoreApplication::processEvents();

        if (dWallpapers->width() < 160 || dPreview->width() < 350 || dTheme->width() < 220 ||
            dTheme->x() <= 0) {
            std::cerr << "[-] FAILED: Dock columns collapsed at 1024x720 with ThemeStudioWidget!"
                      << std::endl;
            return 1;
        }
        if (!checkMultiTabBarsVisible("Default 1024x720 with ThemeStudioWidget tab active")) {
            return 1;
        }
        if (!screenshotDir.isEmpty()) {
            window.grab().save(screenshotDir +
                               QStringLiteral("/default_themestudio_tab_1024x720.png"));
        }

        layoutMgr->setPanelVisible(QStringLiteral("PropertyInspectorWidget"), true);
        for (int i = 0; i < 4; ++i)
            QCoreApplication::processEvents();

        if (dWallpapers->width() < 160 || dPreview->width() < 350 || dInspector->width() < 220) {
            std::cerr << "[-] FAILED: Dock columns collapsed at 1024x720!" << std::endl;
            return 1;
        }
        if (!checkMultiTabBarsVisible("Default 1024x720 after resize")) {
            return 1;
        }

        window.resize(1600, 960);
        for (int i = 0; i < 4; ++i)
            QCoreApplication::processEvents();

        for (const QString &persp :
             {QStringLiteral("Shader Coding"), QStringLiteral("Scene Composer"),
              QStringLiteral("Particle & Audio"), QStringLiteral("Theme Studio"),
              QStringLiteral("Multi-Monitor"), QStringLiteral("Rigging & Animation"),
              QStringLiteral("Default")}) {
            if (!layoutMgr->switchPerspective(persp) || layoutMgr->activePerspective() != persp) {
                std::cerr << "[-] FAILED to switch to perspective: " << persp.toStdString()
                          << std::endl;
                return 1;
            }
            for (int i = 0; i < 4; ++i)
                QCoreApplication::processEvents();
        }

        int realizedCount = 0;
        for (const QString &id : panelIds) {
            if (layoutMgr->realizeLazyPanel(id) != nullptr && layoutMgr->isPanelRealized(id)) {
                ++realizedCount;
            }
        }
        for (int i = 0; i < 4; ++i)
            QCoreApplication::processEvents();

        if (realizedCount != panelIds.size()) {
            std::cerr << "[-] FAILED: Expected all 22 panels realized, got " << realizedCount
                      << std::endl;
            return 1;
        }

        double allRssAnonMb = 0.0;
        double allVmRssMb = 0.0;
        malloc_trim(0);
        readSmapsRollupRssAnonMiB(allRssAnonMb, allVmRssMb);
        std::cout << "[+] Realized 22-panel RssAnon (/proc/self/smaps_rollup): " << allRssAnonMb
                  << " MB (VmRSS: " << allVmRssMb << " MB, Budget: <= 62.00 MB)" << std::endl;
#if !defined(WAYWAL_STUDIO_ASAN_BUILD)
        if (allRssAnonMb > 62.0) {
            std::cerr << "[-] FAILED: Realized 22-panel RssAnon (" << allRssAnonMb
                      << " MB) exceeded 62.0 MB budget!" << std::endl;
            return 1;
        }
#endif
    }

    window.hide();
    QCoreApplication::processEvents();

    std::cout << "========================================================" << std::endl;
    std::cout << "[wwal-studio] Headless verification check PASSED cleanly (code 0)." << std::endl;
    std::cout << "========================================================" << std::endl;
    return 0;
}

static int runBenchmarkGrid(const StudioOptions & /*options*/)
{
    std::cout << "========================================================" << std::endl;
    std::cout << "[wwal-studio] 144 FPS Virtualization & Search Benchmark" << std::endl;
    std::cout << "========================================================" << std::endl;

    wwal::studio::FuzzyCatalogIndex index;
    constexpr size_t totalItems = 10000;
    for (size_t i = 0; i < totalItems; ++i) {
        wwal::studio::CatalogItem item;
        item.id = static_cast<uint32_t>(i);
        item.title = "Wallpaper Asset " + std::to_string(i + 1);
        item.path = "/usr/share/backgrounds/wwal/wallpaper_" + std::to_string(i + 1) + ".png";
        item.badge = static_cast<wwal::studio::WallpaperBadge>(i % 5);
        item.file_size = 1024 * 1024 * (1 + (i % 16));
        item.date_modified = 1700000000 + static_cast<int64_t>(i);
        item.aspect_ratio = 16.0f / 9.0f;
        index.addItem(std::move(item));
    }

    wwal::studio::GpuTexturePool pool(256);
    wwal::studio::HoverPreviewController hover;
    wwal::studio::GridGlCanvas canvas(index, pool, hover);

    const int cols = canvas.computeColumns(1920);
    const int virtualH = canvas.computeVirtualHeight(totalItems, cols);
    constexpr int viewportH = 1080;
    constexpr int frames = 2000;

    std::vector<double> frameUs;
    frameUs.reserve(frames);
    for (int f = 0; f < frames; ++f) {
        const int scrollY =
            static_cast<int>((static_cast<double>(f) / frames) * std::max(1, virtualH - viewportH));
        const auto t0 = std::chrono::steady_clock::now();
        const auto slice = canvas.computeVisibleSlice(totalItems, cols, scrollY, viewportH);
        const auto t1 = std::chrono::steady_clock::now();
        if (slice.is_empty || slice.index_last < slice.index_first ||
            slice.index_last >= totalItems) {
            std::cerr << "[-] FAILED: Invalid visible slice in scroll sweep!" << std::endl;
            return 1;
        }
        frameUs.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }

    std::sort(frameUs.begin(), frameUs.end());
    const double p50Us = frameUs[frameUs.size() * 50 / 100];
    const double p99Us = frameUs[frameUs.size() * 99 / 100];
    const double searchUs = index.benchmarkSearch("wallpaper", 50);

    std::cout << "[+] 10,000-item O(1) slice: p50=" << std::fixed << std::setprecision(3) << p50Us
              << " us, p99=" << p99Us << " us (144 FPS budget: < 6940.0 us)" << std::endl;
    std::cout << "[+] 10,000-item Myers/Bitap search: " << std::fixed << std::setprecision(1)
              << searchUs << " us" << std::endl;

    if (p99Us >= 6940.0) {
        std::cerr << "[-] FAILED: Grid virtualization p99 exceeded 144 FPS budget!" << std::endl;
        return 1;
    }
    std::cout << "========================================================" << std::endl;
    std::cout << "[wwal-studio] Grid benchmark PASSED cleanly (code 0)." << std::endl;
    std::cout << "========================================================" << std::endl;
    return 0;
}

static QtMessageHandler g_prevQtMessageHandler = nullptr;

static void waywalQtMessageFilter(QtMsgType type, const QMessageLogContext &context,
                                  const QString &msg)
{
    if (context.category && std::string_view(context.category) == "qt.qpa.wayland.textinput") {
        return;
    }
    if (msg.contains(QLatin1String("QWaylandTextInputv3::disableSurface")) ||
        (msg.contains(QLatin1String("Trying to disable")) &&
         msg.contains(QLatin1String("is focused")))) {
        return;
    }
    if (g_isOffscreenPlatform.load(std::memory_order_relaxed) &&
        (msg.contains(QLatin1String("QOpenGLWidget is not supported on this platform")) ||
         msg.contains(QLatin1String("QOpenGLWidget: Failed to create context")))) {
        return;
    }
    if (g_prevQtMessageHandler) {
        g_prevQtMessageHandler(type, context, msg);
    }
}

#ifndef WAYWAL_VERSION
#define WAYWAL_VERSION "3.0.0"
#endif

int main(int argc, char *argv[])
{
    mallopt(M_ARENA_MAX, 2);
    const auto startupT0 = std::chrono::steady_clock::now();
    g_prevQtMessageHandler = qInstallMessageHandler(waywalQtMessageFilter);
    if (qEnvironmentVariableIsEmpty("QT_LOGGING_RULES")) {
        QLoggingCategory::setFilterRules(QStringLiteral("qt.qpa.wayland.textinput.warning=false\n"
                                                        "qt.qpa.wayland.textinput=false"));
    }

    // High-DPI fractional scaling (wp_fractional_scale_v1) MUST be configured before Q*Application
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    // Unified OpenGL ES 3.1 surface format across all QOpenGLWidget viewports
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGLES);
    fmt.setVersion(3, 1);
    fmt.setRedBufferSize(8);
    fmt.setGreenBufferSize(8);
    fmt.setBlueBufferSize(8);
    fmt.setAlphaBufferSize(8);
    fmt.setDepthBufferSize(0);
    fmt.setStencilBufferSize(0);
    fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    fmt.setSwapInterval(0);
    QSurfaceFormat::setDefaultFormat(fmt);

    // Early scan for headless flags so we can switch to offscreen platform plugin
    bool isHeadless = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--headless-check" || arg == "--benchmark-grid") {
            isHeadless = true;
            break;
        }
    }

    // In headless environments without display servers, auto-enable offscreen platform
    if (isHeadless || (qEnvironmentVariableIsEmpty("DISPLAY") &&
                       qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        qunsetenv("DISPLAY");
        qunsetenv("WAYLAND_DISPLAY");
    }
    g_isOffscreenPlatform.store(qEnvironmentVariable("QT_QPA_PLATFORM") ==
                                    QLatin1String("offscreen"),
                                std::memory_order_relaxed);

    QApplication app(argc, argv);
    QPixmapCache::setCacheLimit(512);
    QApplication::setStyle(new wwal::studio::tokens::StudioProxyStyle());
    if (qEnvironmentVariableIsEmpty("QT_LOGGING_RULES")) {
        QLoggingCategory::setFilterRules(QStringLiteral("qt.qpa.wayland.textinput.warning=false\n"
                                                        "qt.qpa.wayland.textinput=false"));
    }
    app.setApplicationName(QStringLiteral("wwal-studio"));
    app.setApplicationVersion(QStringLiteral(WAYWAL_VERSION));
    app.setOrganizationName(QStringLiteral("waywal"));
    app.setOrganizationDomain(QStringLiteral("waywal.org"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("WayWal Studio wallpaper manager and shader editor"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption namespaceOption(
        QStringList() << QStringLiteral("n") << QStringLiteral("namespace"),
        QStringLiteral("IPC socket namespace for daemon connection (default: 'default')."),
        QStringLiteral("name"), QStringLiteral("default"));
    parser.addOption(namespaceOption);

    QCommandLineOption socketOption(
        QStringList() << QStringLiteral("s") << QStringLiteral("socket"),
        QStringLiteral("Direct path to wwald Unix domain socket override."),
        QStringLiteral("path"));
    parser.addOption(socketOption);

    QCommandLineOption wallpaperDirOption(
        QStringList() << QStringLiteral("d") << QStringLiteral("wallpaper-dir"),
        QStringLiteral("Wallpaper directory to scan (can be passed multiple times; overrides "
                       "default folders)."),
        QStringLiteral("dir"));
    parser.addOption(wallpaperDirOption);

    QCommandLineOption excludeDirOption(
        QStringList() << QStringLiteral("x") << QStringLiteral("exclude-dir"),
        QStringLiteral("Folder path or folder name to exclude from wallpaper scanning (can be "
                       "passed multiple times)."),
        QStringLiteral("dir_or_name"));
    parser.addOption(excludeDirOption);

    QCommandLineOption benchmarkGridOption(
        QStringList() << QStringLiteral("benchmark-grid"),
        QStringLiteral("Run automated 144 FPS virtualization scroll stress benchmark."));
    parser.addOption(benchmarkGridOption);

    QCommandLineOption headlessCheckOption(
        QStringList() << QStringLiteral("headless-check"),
        QStringLiteral(
            "Instantiate core structures, test layout serialization, and exit 0 without window."));
    parser.addOption(headlessCheckOption);

    QCommandLineOption realizeAllPanelsOption(
        QStringList() << QStringLiteral("realize-all-panels"),
        QStringLiteral(
            "Realize all 22 registered panels and multi-perspective layouts in headless checks."));
    parser.addOption(realizeAllPanelsOption);

    parser.process(app);

    StudioOptions options;
    options.namespaceStr = parser.value(namespaceOption);
    options.socketPath = parser.value(socketOption);
    options.wallpaperDirs = parser.values(wallpaperDirOption);
    options.excludeDirs = parser.values(excludeDirOption);
    options.benchmarkGrid = parser.isSet(benchmarkGridOption);
    options.headlessCheck = parser.isSet(headlessCheckOption);
    options.realizeAllPanels = parser.isSet(realizeAllPanelsOption);

    if (options.headlessCheck) {
        return runHeadlessCheck(options, startupT0);
    }
    if (options.benchmarkGrid) {
        return runBenchmarkGrid(options);
    }

    // Normal Interactive GUI Mode
    MainWindow window(options);
    window.show();

    return app.exec();
}
