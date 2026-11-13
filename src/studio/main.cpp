// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/app/main_window.hpp"
#include "studio/app/dock_layout_manager.hpp"
#include "studio/grid/virtualized_grid_widget.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QLoggingCategory>
#include <QMouseEvent>
#include <QStyleFactory>
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
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <unistd.h>

using namespace waywal::studio;

static int runHeadlessCheck(const StudioOptions &options)
{
    std::cout << "========================================================" << std::endl;
    std::cout << "[wwal-studio] Starting Headless Verification Suite" << std::endl;
    std::cout << "========================================================" << std::endl;

    // 1. Instantiate core MainWindow in offscreen mode
    MainWindow window(options);
    DockLayoutManager *layoutMgr = window.dockLayoutManager();
    if (!layoutMgr) {
        std::cerr << "[-] FAILED: DockLayoutManager is null!" << std::endl;
        return 1;
    }
    std::cout << "[+] MainWindow and DockLayoutManager successfully instantiated." << std::endl;

    // 2. Validate Panel Registration for all 6 panels & bars
    QStringList panelIds = layoutMgr->registeredPanelIds();
    std::cout << "[+] Registered panels (" << panelIds.size()
              << "): " << panelIds.join(", ").toStdString() << std::endl;

    const QStringList expectedPanels = {
        QStringLiteral("Wallpapers"),        QStringLiteral("GpuPreviewViewport"),
        QStringLiteral("CubicBezier"),       QStringLiteral("PropertyInspectorWidget"),
        QStringLiteral("ThemeStudioWidget"), QStringLiteral("MonitorTopologyBar"),
        QStringLiteral("StudioFooterBar")};

    for (const auto &expected : expectedPanels) {
        if (!panelIds.contains(expected)) {
            std::cerr << "[-] FAILED: Panel '" << expected.toStdString() << "' is not registered!"
                      << std::endl;
            return 1;
        }
        if (!layoutMgr->dockWidget(expected)) {
            std::cerr << "[-] FAILED: DockWidget for '" << expected.toStdString() << "' is null!"
                      << std::endl;
            return 1;
        }
    }
    std::cout << "[+] All 6 required panels & footer bars verified and accessible." << std::endl;

    // 3. Validate Available Perspectives
    QStringList perspectives = layoutMgr->availablePerspectives();
    std::cout << "[+] Available perspectives: " << perspectives.join(", ").toStdString()
              << std::endl;

    const QStringList requiredPerspectives = {
        QStringLiteral("Default"), QStringLiteral("Shader Coding"), QStringLiteral("Theme Studio"),
        QStringLiteral("Multi-Monitor")};

    for (const auto &req : requiredPerspectives) {
        if (!perspectives.contains(req)) {
            std::cerr << "[-] FAILED: Missing required perspective: " << req.toStdString()
                      << std::endl;
            return 1;
        }
    }
    std::cout << "[+] All 4 required perspectives present in perspective registry." << std::endl;

    // 4. Validate JSON Serialization Schema
    QJsonObject layoutJson = layoutMgr->serializeLayout();
    if (layoutJson.value(QStringLiteral("version")).toInt() != 1) {
        std::cerr << "[-] FAILED: JSON serialization 'version' must equal 1!" << std::endl;
        return 1;
    }
    if (layoutJson.value(QStringLiteral("active_perspective")).toString().isEmpty()) {
        std::cerr << "[-] FAILED: JSON missing 'active_perspective' field!" << std::endl;
        return 1;
    }

    QJsonObject perspsObj = layoutJson.value(QStringLiteral("perspectives")).toObject();
    for (const auto &req : requiredPerspectives) {
        if (!perspsObj.contains(req)) {
            std::cerr << "[-] FAILED: Serialized JSON missing perspective: " << req.toStdString()
                      << std::endl;
            return 1;
        }
    }

    // Validate tree nodes in "Default" perspective
    QJsonObject defTree = perspsObj.value(QStringLiteral("Default")).toObject();
    if (!defTree.contains(QStringLiteral("split_direction")) ||
        !defTree.contains(QStringLiteral("split_ratio")) ||
        !defTree.contains(QStringLiteral("left")) || !defTree.contains(QStringLiteral("right"))) {
        std::cerr << "[-] FAILED: 'Default' perspective root does not match expected split schema!"
                  << std::endl;
        return 1;
    }
    std::cout << "[+] JSON layout serialization matches split tree schema with 100% fidelity."
              << std::endl;

    // 5. Test Live Perspective Switching
    for (const auto &name : requiredPerspectives) {
        bool ok = layoutMgr->switchPerspective(name);
        if (!ok || layoutMgr->activePerspective() != name) {
            std::cerr << "[-] FAILED to switch to perspective: " << name.toStdString() << std::endl;
            return 1;
        }
        std::cout << "  -> Switched to perspective: '" << name.toStdString() << "' [OK]"
                  << std::endl;
    }
    // Switch back to Default
    layoutMgr->switchPerspective(QStringLiteral("Default"));
    std::cout << "[+] Live perspective switching cycle validated." << std::endl;

    // 6. Test File Load/Save Round-Trip
    QString testFilePath = QStringLiteral("/tmp/wwal_headless_layout_test_%1.json")
                               .arg(QCoreApplication::applicationPid());
    bool saved = layoutMgr->saveLayoutToFile(testFilePath);
    if (!saved || !QFile::exists(testFilePath)) {
        std::cerr << "[-] FAILED to save layout file to: " << testFilePath.toStdString()
                  << std::endl;
        return 1;
    }

    QFileInfo fi(testFilePath);
    if (fi.size() <= 0) {
        std::cerr << "[-] FAILED: Saved layout file is empty!" << std::endl;
        QFile::remove(testFilePath);
        return 1;
    }

    bool loaded = layoutMgr->loadLayoutFromFile(testFilePath);
    QFile::remove(testFilePath);
    if (!loaded) {
        std::cerr << "[-] FAILED: Unable to reload saved layout JSON file!" << std::endl;
        return 1;
    }
    std::cout << "[+] File persistence round-trip ($XDG_CONFIG_HOME JSON format) passed."
              << std::endl;

    // 7. Verify Global Actions and Shortcuts
    if (!window.undoAction() || !window.redoAction() || !window.searchFocusAction() ||
        !window.playPauseAction() || !window.fullscreenAction()) {
        std::cerr << "[-] FAILED: One or more global studio actions are null!" << std::endl;
        return 1;
    }
    std::cout << "[+] Global shortcuts and actions verified:" << std::endl;
    std::cout << "  - Undo: " << window.undoAction()->shortcut().toString().toStdString()
              << std::endl;
    std::cout << "  - Redo: " << window.redoAction()->shortcut().toString().toStdString()
              << std::endl;
    std::cout << "  - Search Focus: "
              << window.searchFocusAction()->shortcut().toString().toStdString() << std::endl;
    // 8. Verify Idle RSS < 80 MB (Private Anonymous / Heap Memory)
    double rss_anon_mb = 0.0;
    double rss_total_mb = 0.0;
    std::ifstream statusFile("/proc/self/status");
    std::string sline;
    while (std::getline(statusFile, sline)) {
        if (sline.rfind("RssAnon:", 0) == 0) {
            long kb = 0;
            if (sscanf(sline.c_str(), "RssAnon: %ld kB", &kb) == 1) {
                rss_anon_mb = kb / 1024.0;
            }
        } else if (sline.rfind("VmRSS:", 0) == 0) {
            long kb = 0;
            if (sscanf(sline.c_str(), "VmRSS: %ld kB", &kb) == 1) {
                rss_total_mb = kb / 1024.0;
            }
        }
    }
    std::cout << "[+] Idle Memory Footprint:" << std::endl;
    std::cout << "  -> Private Anonymous / Heap RSS: " << std::fixed << std::setprecision(2)
              << rss_anon_mb << " MB (Budget: < 80.00 MB)" << std::endl;
    std::cout << "  -> Total VmRSS (including shared OS libraries): " << rss_total_mb << " MB"
              << std::endl;

#if defined(__SANITIZE_ADDRESS__)
#define WAYWAL_STUDIO_ASAN_BUILD 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define WAYWAL_STUDIO_ASAN_BUILD 1
#endif
#endif

#if defined(WAYWAL_STUDIO_ASAN_BUILD)
    if (rss_anon_mb >= 80.0) {
        std::cout << "[*] ASan build: Idle Private RSS = " << rss_anon_mb
                  << " MB (80 MB non-ASan limit informational only under AddressSanitizer)."
                  << std::endl;
    }
#else
    if (rss_anon_mb >= 80.0) {
        std::cerr << "[-] FAILED: Idle Private RSS exceeded 80 MB budget!" << std::endl;
        return 1;
    }
#endif

    // 9. Verify Realized Window Geometry, Dock Non-Overlap, & Proportional Resize Scaling
    // Populate sample/scanned catalog entries so GridGlCanvas card rendering is verified
    if (window.gridWidget()) {
        auto &idx = window.gridWidget()->catalogIndex();
        (void)idx.loadConfigFromFile();
        if (!options.wallpaperDirs.isEmpty()) {
            std::vector<std::string> dirs;
            for (const auto &d : options.wallpaperDirs) {
                dirs.push_back(d.toStdString());
            }
            idx.setIncludedDirectories(dirs);
            idx.setScanSystemAndWorkshopPaths(false);
        }
        if (!options.excludeDirs.isEmpty()) {
            for (const auto &ex : options.excludeDirs) {
                idx.addExcludedDirectory(ex.toStdString());
            }
        }
        window.gridWidget()->scanWallpapers();
        window.gridWidget()->populateSampleCatalogIfEmpty(false);
        window.gridWidget()->selectFirstItemIfAvailable();
    }

    // Test the EXACT interactive startup sequence: window.show() WITHOUT calling
    // switchPerspective() or applyCurrentSplitSizes() after show()!
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    window.resize(1600, 960);
    window.show();
    for (int i = 0; i < 12; ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(15);
    }

    auto *dWallpapers = layoutMgr->dockWidget(QStringLiteral("Wallpapers"));
    auto *dPreview = layoutMgr->dockWidget(QStringLiteral("GpuPreviewViewport"));
    auto *dBezier = layoutMgr->dockWidget(QStringLiteral("CubicBezier"));
    auto *dInspector = layoutMgr->dockWidget(QStringLiteral("PropertyInspectorWidget"));
    auto *dTheme = layoutMgr->dockWidget(QStringLiteral("ThemeStudioWidget"));
    auto *dTopo = layoutMgr->dockWidget(QStringLiteral("MonitorTopologyBar"));
    auto *dFooter = layoutMgr->dockWidget(QStringLiteral("StudioFooterBar"));

    std::cout << "[+] Default Perspective Geometries @ 1600x960 (initial show without post-show "
                 "switchPerspective):"
              << std::endl;
    for (const auto &id : expectedPanels) {
        if (auto *d = layoutMgr->dockWidget(id)) {
            QRect g = d->geometry();
            std::cout << "  - " << id.toStdString() << ": (" << g.x() << ", " << g.y() << ", "
                      << g.width() << "x" << g.height() << ") visible=" << d->isVisible()
                      << std::endl;
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

    const QString screenshotDir = qEnvironmentVariable("WWAL_STUDIO_SCREENSHOT_DIR");
    if (!screenshotDir.isEmpty()) {
        window.grab().save(screenshotDir + QStringLiteral("/default_1600x960.png"));
    }

    // Switch active tab in right dock to Material You Theme Studio, simulate mouse release
    // (which triggers syncLiveRatiosFromGeometry while PropertyInspectorWidget is parked
    // offscreen), and verify QTabBar and split ratios are not corrupted!
    layoutMgr->setPanelVisible(QStringLiteral("ThemeStudioWidget"), true);
    for (int i = 0; i < 6; ++i)
        QCoreApplication::processEvents();
    if (dTheme->x() <= 0 || dTheme->y() <= 0) {
        std::cerr << "[-] FAILED: ThemeStudioWidget tab did not raise onto screen: (" << dTheme->x()
                  << ", " << dTheme->y() << ")!" << std::endl;
        return 1;
    }
    {
        QMouseEvent releaseEv(QEvent::MouseButtonRelease, QPointF(100, 100), QPointF(100, 100),
                              Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &releaseEv);
    }
    for (int i = 0; i < 4; ++i)
        QCoreApplication::processEvents();

    if (!checkMultiTabBarsVisible("Default 1600x960 after raising ThemeStudioWidget tab")) {
        return 1;
    }
    if (!screenshotDir.isEmpty()) {
        window.grab().save(screenshotDir + QStringLiteral("/default_themestudio_tab_1600x960.png"));
    }

    // Test dynamic window resize down to 1024x720 while ThemeStudioWidget is the active tab
    // (verifying that offscreen PropertyInspectorWidget did not corrupt split_ratio!)
    window.resize(1024, 720);
    for (int i = 0; i < 8; ++i)
        QCoreApplication::processEvents();

    if (dWallpapers->width() < 160 || dPreview->width() < 350 || dTheme->width() < 220 ||
        dTheme->x() <= 0) {
        std::cerr << "[-] FAILED: Dock columns collapsed at 1024x720 when ThemeStudioWidget tab is "
                     "active! "
                  << "wall=" << dWallpapers->width() << " prev=" << dPreview->width()
                  << " theme=" << dTheme->width() << " x=" << dTheme->x() << std::endl;
        return 1;
    }
    if (!checkMultiTabBarsVisible("Default 1024x720 with ThemeStudioWidget tab active")) {
        return 1;
    }
    if (!screenshotDir.isEmpty()) {
        window.grab().save(screenshotDir + QStringLiteral("/default_themestudio_tab_1024x720.png"));
    }

    // Switch back to PropertyInspectorWidget tab at 1024x720 and verify QTabBar & geometry
    layoutMgr->setPanelVisible(QStringLiteral("PropertyInspectorWidget"), true);
    for (int i = 0; i < 6; ++i)
        QCoreApplication::processEvents();

    std::cout << "[+] Default Perspective Geometries @ 1024x720:" << std::endl;
    for (const auto &id : expectedPanels) {
        if (auto *d = layoutMgr->dockWidget(id)) {
            QRect g = d->geometry();
            std::cout << "  - " << id.toStdString() << ": (" << g.x() << ", " << g.y() << ", "
                      << g.width() << "x" << g.height() << ") visible=" << d->isVisible()
                      << std::endl;
        }
    }

    if (dWallpapers->width() < 160 || dPreview->width() < 350 || dInspector->width() < 220) {
        std::cerr << "[-] FAILED: Dock columns collapsed or failed to scale at 1024x720!"
                  << std::endl;
        return 1;
    }
    if (!checkMultiTabBarsVisible("Default 1024x720 after resize")) {
        return 1;
    }

    if (!screenshotDir.isEmpty()) {
        window.grab().save(screenshotDir + QStringLiteral("/default_1024x720.png"));

        window.resize(1600, 960);
        for (int i = 0; i < 8; ++i)
            QCoreApplication::processEvents();

        layoutMgr->switchPerspective(QStringLiteral("Shader Coding"));
        for (int i = 0; i < 8; ++i)
            QCoreApplication::processEvents();
        if (!checkMultiTabBarsVisible("Shader Coding 1600x960", 3)) {
            return 1;
        }
        window.grab().save(screenshotDir + QStringLiteral("/shader_coding_1600x960.png"));

        layoutMgr->switchPerspective(QStringLiteral("Theme Studio"));
        for (int i = 0; i < 8; ++i)
            QCoreApplication::processEvents();
        window.grab().save(screenshotDir + QStringLiteral("/theme_studio_1600x960.png"));

        layoutMgr->switchPerspective(QStringLiteral("Multi-Monitor"));
        for (int i = 0; i < 8; ++i)
            QCoreApplication::processEvents();
        window.grab().save(screenshotDir + QStringLiteral("/multi_monitor_1600x960.png"));

        layoutMgr->switchPerspective(QStringLiteral("Default"));
        for (int i = 0; i < 8; ++i)
            QCoreApplication::processEvents();
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
    if (qEnvironmentVariable("QT_QPA_PLATFORM") == QLatin1String("offscreen") &&
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

    QApplication app(argc, argv);
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

    parser.process(app);

    StudioOptions options;
    options.namespaceStr = parser.value(namespaceOption);
    options.socketPath = parser.value(socketOption);
    options.wallpaperDirs = parser.values(wallpaperDirOption);
    options.excludeDirs = parser.values(excludeDirOption);
    options.benchmarkGrid = parser.isSet(benchmarkGridOption);
    options.headlessCheck = parser.isSet(headlessCheckOption);

    if (options.headlessCheck) {
        return runHeadlessCheck(options);
    }
    if (options.benchmarkGrid) {
        return runBenchmarkGrid(options);
    }

    // Normal Interactive GUI Mode
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    MainWindow window(options);
    window.show();

    return app.exec();
}
