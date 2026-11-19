// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_studio_command_and_dock.cpp
 * @brief Command Bus, Docking Persistence & Monitor Span Math Suite
 *
 * Verifies:
 * 1. CommandBus 1,000-step infinite undo/redo stack & memory capping.
 * 2. High-frequency micro-drag command coalescing via ICommand::mergeWith().
 * 3. DockLayoutManager JSON round-trip serialization/deserialization.
 * 4. MonitorTopologyCanvas normalized u_span_rect calculation across asymmetric 3-monitor setups.
 */

#include "studio/core/command_bus.hpp"
#include "studio/core/services.hpp"
#include "studio/app/dock_layout_manager.hpp"
#include "studio/app/main_window.hpp"
#include "studio/app/studio_session_context.hpp"
#include "studio/dock/openhub_browser_widget.hpp"
#include "studio/dock/workshop_browser_widget.hpp"
#include "studio/wizard/creator_packaging_wizard.hpp"
#include "studio/graph/effect_library_browser_widget.hpp"
#include "studio/graph/node_graph_editor_widget.hpp"
#include "studio/inspector/oklch_color_dialog.hpp"
#include "studio/inspector/property_inspector_widget.hpp"
#include "studio/theme/theme_studio_widget.hpp"
#include "studio/topology/cubic_bezier_editor.hpp"
#include "studio/topology/monitor_topology_canvas.hpp"
#include "studio/viewport/gpu_preview_viewport.hpp"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QGroupBox>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPushButton>
#include <QTabBar>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <QWidget>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalBlocker>

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

using namespace waywal::studio;

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
extern "C" __attribute__((visibility("default"))) const char *__lsan_default_suppressions(void)
{
    return "leak:<unknown module>\nleak:libnvidia\nleak:libEGL\nleak:libGLESv2\nleak:dri\n";
}
#endif

namespace {

class IncrementCommand : public ICommand {
  public:
    IncrementCommand(int *val, int delta) : m_target(val), m_delta(delta) {}

    void execute() override { *m_target += m_delta; }
    void undo() override { *m_target -= m_delta; }
    QString describe() const override { return QStringLiteral("Increment"); }

  private:
    int *m_target;
    int m_delta;
};

} // namespace

static void test_command_bus_1000_steps()
{
    std::cout << "[CommandBus] Testing CommandBus 1,000-step Undo/Redo & Capacity Capping..."
              << std::endl;

    CommandBus bus(1000);
    assert(bus.maxHistory() == 1000);
    assert(bus.undoCount() == 0);
    assert(!bus.canUndo());
    assert(!bus.canRedo());

    int counter = 0;

    // Push exactly 1,000 commands
    for (int i = 0; i < 1000; ++i) {
        bus.execute(std::make_unique<IncrementCommand>(&counter, 1));
    }

    assert(counter == 1000);
    assert(bus.undoCount() == 1000);
    assert(bus.canUndo());
    assert(!bus.canRedo());

    // Push 200 extra commands: undo history should remain bounded at 1,000
    for (int i = 0; i < 200; ++i) {
        bus.execute(std::make_unique<IncrementCommand>(&counter, 1));
    }

    assert(counter == 1200);
    assert(bus.undoCount() == 1000 && "History must be strictly capped at maxHistory = 1000");

    // Undo all 1,000 steps
    for (int i = 0; i < 1000; ++i) {
        assert(bus.canUndo());
        bus.undo();
    }

    assert(counter == 200 && "Counter must equal 200 after 1,000 undos from 1,200");
    assert(bus.undoCount() == 0);
    assert(!bus.canUndo());
    assert(bus.canRedo());
    assert(bus.redoCount() == 1000);

    // Redo all 1,000 steps
    for (int i = 0; i < 1000; ++i) {
        assert(bus.canRedo());
        bus.redo();
    }

    assert(counter == 1200);
    assert(bus.undoCount() == 1000);
    assert(bus.redoCount() == 0);
    assert(!bus.canRedo());

    std::cout << "  -> CommandBus 1,000-step undo/redo passed." << std::endl;
}

static void test_micro_drag_coalescing()
{
    std::cout << "[CommandBus] Testing Micro-Drag mergeWith() Coalescing..." << std::endl;

    CommandBus bus(100);
    float currentVal = 0.0f;

    auto applyFunc = [&currentVal](const QString & /*output*/, const QString & /*name*/,
                                   const float *vals, uint32_t count, uint32_t /*flags*/) {
        if (count > 0 && vals) {
            currentVal = vals[0];
        }
    };

    // Execute first drag event at t0: 0.0 -> 0.2
    bus.execute(std::make_unique<SetUniformCommand>(
        QStringLiteral("DP-1"), QStringLiteral("u_speed"), std::vector<float>{0.0f},
        std::vector<float>{0.2f}, 0, applyFunc));

    assert(std::fabs(currentVal - 0.2f) < 1e-5f);
    assert(bus.undoCount() == 1);

    // Rapid successive micro-drag updates within coalescing window (same output & uniform)
    bus.execute(std::make_unique<SetUniformCommand>(
        QStringLiteral("DP-1"), QStringLiteral("u_speed"), std::vector<float>{0.2f},
        std::vector<float>{0.5f}, 0, applyFunc));

    assert(std::fabs(currentVal - 0.5f) < 1e-5f);
    assert(bus.undoCount() == 1 && "Successive micro-drags must coalesce into a single undo step");

    bus.execute(std::make_unique<SetUniformCommand>(
        QStringLiteral("DP-1"), QStringLiteral("u_speed"), std::vector<float>{0.5f},
        std::vector<float>{1.8f}, 0, applyFunc));

    assert(std::fabs(currentVal - 1.8f) < 1e-5f);
    assert(bus.undoCount() == 1 && "Successive micro-drags must continue to coalesce");

    // Single undo must restore the initial pre-drag state (0.0f)
    bus.undo();
    assert(std::fabs(currentVal - 0.0f) < 1e-5f);
    assert(bus.undoCount() == 0);
    assert(bus.canRedo());

    // Single redo must restore the final dragged state (1.8f)
    bus.redo();
    assert(std::fabs(currentVal - 1.8f) < 1e-5f);
    assert(bus.undoCount() == 1);

    std::cout << "  -> Micro-drag coalescing passed." << std::endl;
}

static void test_dock_layout_manager_roundtrip()
{
    std::cout << "[DockLayout] Testing DockLayoutManager JSON Serialization Round-Trip..."
              << std::endl;

    QMainWindow mainWindow;
    DockLayoutManager layoutMgr(&mainWindow);

    // Register panels
    QWidget p1, p2, p3, p4, p5, p6;
    layoutMgr.registerPanel(QStringLiteral("Wallpapers"), &p1, QStringLiteral("Wallpaper Catalog"));
    layoutMgr.registerPanel(QStringLiteral("GpuPreviewViewport"), &p2,
                            QStringLiteral("GPU Preview"));
    layoutMgr.registerPanel(QStringLiteral("CubicBezier"), &p3, QStringLiteral("Transitions"));
    layoutMgr.registerPanel(QStringLiteral("PropertyInspectorWidget"), &p4,
                            QStringLiteral("Inspector"));
    layoutMgr.registerPanel(QStringLiteral("ThemeStudioWidget"), &p5,
                            QStringLiteral("Theme Studio"));
    layoutMgr.registerPanel(QStringLiteral("MonitorTopologyBar"), &p6, QStringLiteral("Topology"));

    // Switch perspective
    bool ok = layoutMgr.switchPerspective(QStringLiteral("Shader Coding"));
    assert(ok && "switchPerspective to 'Shader Coding' must succeed");
    assert(layoutMgr.activePerspective() == QStringLiteral("Shader Coding"));

    // Serialize to JSON
    QJsonObject json = layoutMgr.serializeLayout();
    assert(json.value(QStringLiteral("version")).toInt() == 1);
    assert(json.value(QStringLiteral("active_perspective")).toString() ==
           QStringLiteral("Shader Coding"));

    QJsonObject persps = json.value(QStringLiteral("perspectives")).toObject();
    assert(persps.contains(QStringLiteral("Default")));
    assert(persps.contains(QStringLiteral("Shader Coding")));
    assert(persps.contains(QStringLiteral("Theme Studio")));
    assert(persps.contains(QStringLiteral("Multi-Monitor")));

    // Change perspective to "Theme Studio"
    layoutMgr.switchPerspective(QStringLiteral("Theme Studio"));
    assert(layoutMgr.activePerspective() == QStringLiteral("Theme Studio"));

    // Deserialize previously serialized JSON: active perspective must restore to "Shader Coding"
    bool dOk = layoutMgr.deserializeLayout(json);
    assert(dOk && "deserializeLayout must succeed");
    assert(layoutMgr.activePerspective() == QStringLiteral("Shader Coding"));

    // Test file save/load round-trip
    QString tmpPath = QStringLiteral("/tmp/wwal_test_dock_roundtrip_%1.json")
                          .arg(QCoreApplication::applicationPid());
    bool sOk = layoutMgr.saveLayoutToFile(tmpPath);
    assert(sOk);

    QFileInfo fi(tmpPath);
    assert(fi.exists() && fi.size() > 0);

    bool lOk = layoutMgr.loadLayoutFromFile(tmpPath);
    assert(lOk);
    QFile::remove(tmpPath);

    // Test legacy pre-revision-2 JSON migration (missing layout_revision key)
    QJsonObject legacyJson = json;
    legacyJson.remove(QStringLiteral("layout_revision"));
    assert(layoutMgr.deserializeLayout(legacyJson));

    // Verify realized top-down dock geometries & non-overlapping splits when
    // switchPerspective("Default") is called BEFORE mainWindow.show() (exact interactive startup
    // sequence without post-show switchPerspective!)
    QWidget p7;
    layoutMgr.registerPanel(QStringLiteral("StudioFooterBar"), &p7, QStringLiteral("Footer"),
                            Qt::BottomDockWidgetArea);
    layoutMgr.switchPerspective(QStringLiteral("Default"));
    mainWindow.resize(1600, 960);
    mainWindow.show();
    for (int i = 0; i < 8; ++i)
        QCoreApplication::processEvents();

    auto *dWall = layoutMgr.dockWidget(QStringLiteral("Wallpapers"));
    auto *dPrev = layoutMgr.dockWidget(QStringLiteral("GpuPreviewViewport"));
    auto *dBez = layoutMgr.dockWidget(QStringLiteral("CubicBezier"));
    auto *dInsp = layoutMgr.dockWidget(QStringLiteral("PropertyInspectorWidget"));
    auto *dThem = layoutMgr.dockWidget(QStringLiteral("ThemeStudioWidget"));
    assert(dWall && dPrev && dBez && dInsp && dThem);
    assert(dWall->isVisible() && dPrev->isVisible() && dBez->isVisible() && dInsp->isVisible());
    assert(!dThem->isHidden() && "Tabified background dock must not be marked hidden");
    assert(dWall->geometry().right() <= dPrev->geometry().left());
    assert(dPrev->geometry().right() <= dInsp->geometry().left());
    assert(dPrev->geometry().bottom() <= dBez->geometry().top());
    assert(dWall->width() >= 220 && dPrev->width() >= 550 && dInsp->width() >= 300);

    auto hasVisibleMultiTabBar = [&mainWindow]() -> bool {
        bool foundActive = false;
        for (QTabBar *tb :
             mainWindow.findChildren<QTabBar *>(QString(), Qt::FindDirectChildrenOnly)) {
            if (!tb)
                continue;
            if (tb->y() < 45 && tb->isVisible()) {
                return false;
            }
            if (tb->y() >= 45 && tb->count() >= 2 && tb->isVisible()) {
                foundActive = true;
            }
        }
        return foundActive;
    };
    assert(hasVisibleMultiTabBar() && "Multi-tab QTabBar must be visible on initial show() without "
                                      "post-show switchPerspective()");

    // Raise second tab and send MouseButtonRelease directly to the QTabBar child widget
    // (simulating an interactive user tab click, where QTabBar accepts the mouse event so it does
    // not bubble to QMainWindow), verifying active_tab updates to 1 and split_ratio is not
    // corrupted on subsequent resize!
    dThem->show();
    dThem->raise();
    for (int i = 0; i < 6; ++i)
        QCoreApplication::processEvents();
    assert(hasVisibleMultiTabBar() &&
           "Multi-tab QTabBar must remain visible after switching active tab");
    {
        QTabBar *activeTb = nullptr;
        for (QTabBar *tb :
             mainWindow.findChildren<QTabBar *>(QString(), Qt::FindDirectChildrenOnly)) {
            if (tb && tb->count() >= 2 && tb->isVisible()) {
                activeTb = tb;
                break;
            }
        }
        assert(activeTb != nullptr);
        QMouseEvent releaseEv(QEvent::MouseButtonRelease, QPointF(10, 10), QPointF(10, 10),
                              Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(activeTb, &releaseEv);
    }
    for (int i = 0; i < 4; ++i)
        QCoreApplication::processEvents();

    {
        QJsonObject updatedJson = layoutMgr.serializeLayout();
        QJsonObject defaultTree = updatedJson.value(QStringLiteral("perspectives"))
                                      .toObject()
                                      .value(QStringLiteral("Default"))
                                      .toObject();
        QJsonObject rightSplit = defaultTree.value(QStringLiteral("right")).toObject();
        QJsonObject rightTabGroup = rightSplit.value(QStringLiteral("right")).toObject();
        assert(rightTabGroup.value(QStringLiteral("active_tab")).toString() ==
                   QStringLiteral("ThemeStudioWidget") &&
               "Clicking/releasing on QTabBar after switching to second tab must update active_tab "
               "to ThemeStudioWidget in DockLayoutManager");
    }

    mainWindow.resize(1024, 720);
    for (int i = 0; i < 8; ++i)
        QCoreApplication::processEvents();
    assert(dWall->geometry().right() <= dPrev->geometry().left());
    assert(dPrev->geometry().right() <= dThem->geometry().left());
    assert(dPrev->geometry().bottom() <= dBez->geometry().top());
    assert(dWall->width() >= 180 && dPrev->width() >= 380 && dThem->width() >= 220);
    assert(hasVisibleMultiTabBar() &&
           "Multi-tab QTabBar must remain visible after window resize with second tab active");

    dInsp->show();
    dInsp->raise();
    for (int i = 0; i < 6; ++i)
        QCoreApplication::processEvents();
    assert(dInsp->width() >= 220);

    std::cout << "  -> DockLayoutManager JSON round-trip, QTabBar preservation & proportional "
                 "geometry scaling passed."
              << std::endl;
}

static void test_monitor_topology_span_math()
{
    std::cout << "[Topology] Testing MonitorTopologyCanvas u_span_rect Across Asymmetric 3-Monitor "
                 "Setup..."
              << std::endl;

    MonitorTopologyCanvas canvas;

    // Construct an asymmetric 3-monitor configuration:
    // Monitor 1: "DP-1"     (0, 0, 1920, 1080)
    // Monitor 2: "DP-2"     (1920, 0, 2560, 1440)
    // Monitor 3: "HDMI-A-1" (4480, 200, 3840, 2160)
    QJsonObject root;
    QJsonArray outputs;

    QJsonObject m1;
    m1[QStringLiteral("name")] = QStringLiteral("DP-1");
    m1[QStringLiteral("make")] = QStringLiteral("Dell");
    m1[QStringLiteral("model")] = QStringLiteral("U2415");
    m1[QStringLiteral("x")] = 0;
    m1[QStringLiteral("y")] = 0;
    m1[QStringLiteral("width")] = 1920;
    m1[QStringLiteral("height")] = 1080;
    m1[QStringLiteral("scale")] = 1.0;
    m1[QStringLiteral("refresh")] = 60000;
    outputs.append(m1);

    QJsonObject m2;
    m2[QStringLiteral("name")] = QStringLiteral("DP-2");
    m2[QStringLiteral("make")] = QStringLiteral("LG");
    m2[QStringLiteral("model")] = QStringLiteral("27GL850");
    m2[QStringLiteral("x")] = 1920;
    m2[QStringLiteral("y")] = 0;
    m2[QStringLiteral("width")] = 2560;
    m2[QStringLiteral("height")] = 1440;
    m2[QStringLiteral("scale")] = 1.0;
    m2[QStringLiteral("refresh")] = 144000;
    outputs.append(m2);

    QJsonObject m3;
    m3[QStringLiteral("name")] = QStringLiteral("HDMI-A-1");
    m3[QStringLiteral("make")] = QStringLiteral("Asus");
    m3[QStringLiteral("model")] = QStringLiteral("PG32U");
    m3[QStringLiteral("x")] = 4480;
    m3[QStringLiteral("y")] = 200; // Asymmetric vertical offset
    m3[QStringLiteral("width")] = 3840;
    m3[QStringLiteral("height")] = 2160;
    m3[QStringLiteral("scale")] = 1.5;
    m3[QStringLiteral("refresh")] = 120000;
    outputs.append(m3);

    root[QStringLiteral("outputs")] = outputs;

    canvas.updateOutputsFromJson(root);
    assert(canvas.monitors().size() == 3);

    DesktopSpanCalculation span = canvas.calculateSpanRectangles();

    // Theoretical virtual bounding box:
    // minX = 0, minY = 0
    // maxX = 4480 + 3840 = 8320
    // maxY = 200 + 2160 = 2360
    // spanW = 8320, spanH = 2360
    assert(span.boundingBox.x() == 0);
    assert(span.boundingBox.y() == 0);
    assert(span.boundingBox.width() == 8320);
    assert(span.boundingBox.height() == 2360);

    const qreal spanW = 8320.0;
    const qreal spanH = 2360.0;

    // Check DP-1 normalized rect
    QRectF r1 = span.normalizedRects.value(QStringLiteral("DP-1"));
    assert(std::fabs(r1.x() - (0.0 / spanW)) < 1e-6);
    assert(std::fabs(r1.y() - (0.0 / spanH)) < 1e-6);
    assert(std::fabs(r1.width() - (1920.0 / spanW)) < 1e-6);
    assert(std::fabs(r1.height() - (1080.0 / spanH)) < 1e-6);

    // Check DP-2 normalized rect
    QRectF r2 = span.normalizedRects.value(QStringLiteral("DP-2"));
    assert(std::fabs(r2.x() - (1920.0 / spanW)) < 1e-6);
    assert(std::fabs(r2.y() - (0.0 / spanH)) < 1e-6);
    assert(std::fabs(r2.width() - (2560.0 / spanW)) < 1e-6);
    assert(std::fabs(r2.height() - (1440.0 / spanH)) < 1e-6);

    // Check HDMI-A-1 normalized rect
    QRectF r3 = span.normalizedRects.value(QStringLiteral("HDMI-A-1"));
    assert(std::fabs(r3.x() - (4480.0 / spanW)) < 1e-6);
    assert(std::fabs(r3.y() - (200.0 / spanH)) < 1e-6);
    assert(std::fabs(r3.width() - (3840.0 / spanW)) < 1e-6);
    assert(std::fabs(r3.height() - (2160.0 / spanH)) < 1e-6);

    // Seamless adjacent span invariant:
    // r1.right() == r2.left() and r2.right() == r3.left() and r3.right() == 1.0
    assert(std::fabs(r1.right() - r2.left()) < 1e-6);
    assert(std::fabs(r2.right() - r3.left()) < 1e-6);
    assert(std::fabs(r3.right() - 1.0) < 1e-6);

    std::cout << "  -> Asymmetric 3-monitor span math passed with 100% precision." << std::endl;
}

static void assert_ascii_only(const QString &str, const char *context)
{
    for (QChar ch : str) {
        if (ch.unicode() > 0x7F) {
            std::cerr << "[FAIL] Non-ASCII character U+" << std::hex
                      << static_cast<uint16_t>(ch.unicode()) << std::dec << " in " << context
                      << ": \"" << str.toStdString() << "\"" << std::endl;
            assert(false && "UI text must not contain emojis, em-dashes, or decorative Unicode");
        }
    }
}

static void audit_widget_tree(QWidget *root, const char *rootName)
{
    root->ensurePolished();
    if (root->layout()) {
        root->layout()->activate();
    }
    assert_ascii_only(root->windowTitle(), rootName);
    assert_ascii_only(root->toolTip(), rootName);

    for (QWidget *w : root->findChildren<QWidget *>()) {
        if (!w)
            continue;
        w->ensurePolished();
        if (w->layout()) {
            w->layout()->activate();
        }
        assert_ascii_only(w->windowTitle(), rootName);
        assert_ascii_only(w->toolTip(), rootName);

        if (auto *btn = qobject_cast<QAbstractButton *>(w)) {
            assert_ascii_only(btn->text(), rootName);
        }
        if (auto *pushBtn = qobject_cast<QPushButton *>(w)) {
            if (pushBtn->isVisibleTo(root)) {
                int effH = std::max(pushBtn->height(), pushBtn->sizeHint().height());
                if (effH > 0 && effH < 20) {
                    std::cerr << "[FAIL] Cramped QPushButton \"" << pushBtn->text().toStdString()
                              << "\" height=" << effH << " in " << rootName << std::endl;
                    assert(effH >= 20 && "Visible QPushButtons must have height >= 20px");
                }
            }
        }
        if (auto *lbl = qobject_cast<QLabel *>(w)) {
            assert_ascii_only(lbl->text(), rootName);
        }
        if (auto *grp = qobject_cast<QGroupBox *>(w)) {
            assert_ascii_only(grp->title(), rootName);
        }
        if (auto *edit = qobject_cast<QLineEdit *>(w)) {
            assert_ascii_only(edit->placeholderText(), rootName);
            assert_ascii_only(edit->text(), rootName);
        }
        if (auto *combo = qobject_cast<QComboBox *>(w)) {
            for (int i = 0; i < combo->count(); ++i) {
                assert_ascii_only(combo->itemText(i), rootName);
            }
        }
    }

    for (QAction *act : root->findChildren<QAction *>()) {
        if (!act)
            continue;
        assert_ascii_only(act->text(), rootName);
        assert_ascii_only(act->toolTip(), rootName);
    }
}

static void test_studio_ui_ergonomics_and_zero_emoji()
{
    std::cout << "[UI/UX] Auditing MainWindow, CreatorPackagingWizard & OklchColorDialog for zero "
                 "emojis/Unicode and button ergonomics..."
              << std::endl;

    StudioOptions opts;
    opts.headlessCheck = true;
    MainWindow mainWin(opts);
    mainWin.resize(1600, 960);
    audit_widget_tree(&mainWin, "MainWindow");

    // Raise WorkshopBrowser and OpenHubBrowser tabs and verify embedded VirtualizedGridWidget
    // does NOT show a duplicate search bar or local "Folders..." button
    auto *layoutMgr = mainWin.dockLayoutManager();
    assert(layoutMgr != nullptr);

    for (const QString &panelId :
         {QStringLiteral("WorkshopBrowser"), QStringLiteral("OpenHubBrowser")}) {
        layoutMgr->setPanelVisible(panelId, true);

        auto *dock = layoutMgr->dockWidget(panelId);
        assert(dock != nullptr && !dock->isHidden());
        audit_widget_tree(dock, panelId.toUtf8().constData());

        int visibleSearchBars = 0;
        for (auto *edit : dock->findChildren<QLineEdit *>()) {
            if (edit && edit->isVisibleTo(dock)) {
                ++visibleSearchBars;
            }
        }
        assert(visibleSearchBars == 1 &&
               "Browser panel must have exactly 1 visible search bar (no duplicate embedded grid "
               "search bar)");

        for (auto *btn : dock->findChildren<QPushButton *>()) {
            if (btn && btn->isVisibleTo(dock)) {
                assert(btn->text() != QStringLiteral("Folders...") &&
                       "Embedded grid inside Workshop/OpenHub must not show local 'Folders...' "
                       "button");
            }
        }
    }

    // Verify narrow-dock (220px width) ergonomics and download progress formatting
    if (auto *ws = mainWin.workshopBrowser()) {
        ws->resize(220, 600);
        audit_widget_tree(ws, "WorkshopBrowserWidget@220px");

        for (auto *btn : ws->findChildren<QPushButton *>()) {
            if (btn && btn->text() == QStringLiteral("Subscribe & Download")) {
                btn->click();
                break;
            }
        }
        QMetaObject::invokeMethod(ws, "onDownloadProgressUpdated", Qt::DirectConnection,
                                  Q_ARG(uint64_t, 2841902314ULL), Q_ARG(float, 50.0f),
                                  Q_ARG(float, 12.5f * 1024.0f * 1024.0f));
        for (auto *lbl : ws->findChildren<QLabel *>()) {
            if (lbl) {
                assert(!lbl->text().contains(QStringLiteral("%.1f")) &&
                       "Download telemetry must not contain literal %.1f format specifier");
                assert(!lbl->text().contains(QStringLiteral("%3")) &&
                       "Download telemetry must not contain unexpanded %3 placeholder");
            }
        }
    }

    if (auto *hub = mainWin.openHubBrowser()) {
        hub->resize(220, 600);
        audit_widget_tree(hub, "OpenHubBrowserWidget@220px");

        QMetaObject::invokeMethod(hub, "onDownloadProgressUpdated", Qt::DirectConnection,
                                  Q_ARG(QString, QStringLiteral("pkg-001")), Q_ARG(float, 50.0f),
                                  Q_ARG(float, 15.2f * 1024.0f * 1024.0f));
        for (auto *lbl : hub->findChildren<QLabel *>()) {
            if (lbl) {
                assert(!lbl->text().contains(QStringLiteral("%.1f")) &&
                       "OpenHub download telemetry must not contain literal %.1f format specifier");
                assert(!lbl->text().contains(QStringLiteral("%3")) &&
                       "OpenHub download telemetry must not contain unexpanded %3 placeholder");
            }
        }
    }

    if (auto *ts = mainWin.themeStudio()) {
        ts->resize(220, 600);
        audit_widget_tree(ts, "ThemeStudioWidget@220px");
    }

    CreatorPackagingWizard wizard;
    wizard.resize(840, 620);
    audit_widget_tree(&wizard, "CreatorPackagingWizard");

    OklchColorDialog colorDlg(QColor(56, 189, 248));
    audit_widget_tree(&colorDlg, "OklchColorDialog");

    std::cout << "  -> UI/UX zero-emoji, embedded grid chrome, download telemetry & 220px "
                 "narrow-dock ergonomics audit passed across all widgets."
              << std::endl;
}

static void test_property_inspector_oklch_and_topology_zoom()
{
    std::cout << "[Inspector] Testing PropertyInspector Undo/Redo, OkLCH Swatch Reset & Topology "
                 "Zoom..."
              << std::endl;

    QTemporaryDir tempXdg;
    assert(tempXdg.isValid());
    const QByteArray prevXdg = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", tempXdg.path().toUtf8());

    {
        CommandBus bus;
        PropertyInspectorWidget inspector;
        inspector.setCommandBus(&bus);

        QString savedPath;
        QObject::connect(
            &inspector, &PropertyInspectorWidget::presetSaved,
            [&savedPath](const QString &, const QString &filePath) { savedPath = filePath; });
        assert(inspector.savePreset(QStringLiteral("unit_check")));
        assert(savedPath.contains(QStringLiteral("/wwal/presets/")));
        assert(QFileInfo::exists(savedPath));

        PropertyRowWidget *speedRow = inspector.propertyRow(QStringLiteral("u_speed"));
        assert(speedRow != nullptr);
        const std::vector<float> origVals = inspector.propertyValues(QStringLiteral("u_speed"));
        assert(!origVals.empty() && std::fabs(origVals[0] - 1.0f) < 1e-4f);

        speedRow->updateValues({2.5f});
        assert(std::fabs(inspector.propertyValues(QStringLiteral("u_speed"))[0] - 2.5f) < 1e-4f);
        assert(bus.undoCount() == 1);

        bus.undo();
        assert(std::fabs(inspector.propertyValues(QStringLiteral("u_speed"))[0] - 1.0f) < 1e-4f);
        assert(std::fabs(speedRow->property().currentValues[0] - 1.0f) < 1e-4f);
        assert(bus.canRedo());

        bus.redo();
        assert(std::fabs(inspector.propertyValues(QStringLiteral("u_speed"))[0] - 2.5f) < 1e-4f);
        assert(std::fabs(speedRow->property().currentValues[0] - 2.5f) < 1e-4f);
    }

    if (prevXdg.isEmpty()) {
        qunsetenv("XDG_CONFIG_HOME");
    } else {
        qputenv("XDG_CONFIG_HOME", prevXdg);
    }

    {
        StudioOptions opts;
        opts.headlessCheck = true;
        MainWindow mainWin(opts);
        auto *browser = mainWin.effectLibraryBrowser();
        auto *graphEd = mainWin.nodeGraphEditor();
        assert(browser != nullptr && graphEd != nullptr);
        const uint32_t countBefore = graphEd->graph().node_count;
        QMetaObject::invokeMethod(browser, "onAddToGraphClicked", Qt::DirectConnection);
        assert(graphEd->graph().node_count == countBefore + 1);
    }

    {
        const QColor initCol(220, 60, 80);
        OklchColorDialog dlg(initCol);
        const QColor expectedReset = dlg.currentColor();
        dlg.setCurrentColor(QColor(40, 200, 120));
        assert(dlg.currentColor() != expectedReset);

        auto *initialSwatch = dlg.findChild<QWidget *>(QStringLiteral("oklchSwatchInitial"));
        assert(initialSwatch != nullptr);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(6.0, 6.0), QPointF(6.0, 6.0),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(initialSwatch, &press);
        assert(dlg.currentColor() == expectedReset);
    }

    {
        MonitorTopologyCanvas canvas;
        canvas.resize(640, 320);
        QJsonObject root;
        QJsonArray outputs;
        QJsonObject m1;
        m1[QStringLiteral("name")] = QStringLiteral("DP-1");
        m1[QStringLiteral("x")] = 0;
        m1[QStringLiteral("y")] = 0;
        m1[QStringLiteral("width")] = 1920;
        m1[QStringLiteral("height")] = 1080;
        m1[QStringLiteral("scale")] = 1.0;
        m1[QStringLiteral("refresh")] = 60000;
        outputs.append(m1);
        root[QStringLiteral("outputs")] = outputs;
        canvas.updateOutputsFromJson(root);

        QImage imgBefore(640, 320, QImage::Format_ARGB32_Premultiplied);
        imgBefore.fill(Qt::transparent);
        canvas.render(&imgBefore);

        QWheelEvent wheelOut(QPointF(320.0, 160.0), QPointF(320.0, 160.0), QPoint(0, 0),
                             QPoint(0, -480), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
                             false);
        QApplication::sendEvent(&canvas, &wheelOut);

        QImage imgAfter(640, 320, QImage::Format_ARGB32_Premultiplied);
        imgAfter.fill(Qt::transparent);
        canvas.render(&imgAfter);
        assert(imgAfter != imgBefore);
    }

    std::cout << "  -> PropertyInspector Undo/Redo, /wwal/presets path, OkLCH swatch reset & "
                 "MonitorTopologyCanvas zoom verified."
              << std::endl;
}

namespace {

class MockIpcClientService final : public IIpcClientService {
  public:
    struct AudioPowerCall {
        float volume{0.0f};
        int8_t muteOnAudio{0};
        int8_t themeFromMedia{0};
        uint8_t batteryPolicy{0};
    };

    bool connected{true};
    std::vector<WallpaperApplyOptions> setWallpaperCalls;
    std::vector<waywal_img_metadata_t> sendImageCalls;
    std::vector<AudioPowerCall> audioPowerCalls;

    bool isConnected() const override { return connected; }
    void sendSetUniform(const QString &, const QString &, const float *, uint32_t, bool) override {}
    void sendSetImage(const waywal_img_metadata_t &meta, int) override
    {
        sendImageCalls.push_back(meta);
    }
    void setWallpaper(const WallpaperApplyOptions &opts) override
    {
        setWallpaperCalls.push_back(opts);
    }
    void sendAudioPowerConfig(float volume, int8_t muteOnAudio, int8_t themeFromMedia,
                              uint8_t batteryPolicy = 0) override
    {
        audioPowerCalls.push_back({volume, muteOnAudio, themeFromMedia, batteryPolicy});
    }
    void requestOutputsJson(std::function<void(const QString &json)>) override {}
    void requestTelemetry(std::function<void(const TelemetrySnapshot &snapshot)>) override {}
    void requestOutputDmabuf(
        const QString &,
        std::function<void(const waywal_export_dmabuf_resp_t &desc, const QVector<int> &fds)>)
        override
    {
    }
    void subscribeEvents() override {}
};

class ScopedServicesIpcGuard {
  public:
    explicit ScopedServicesIpcGuard(IIpcClientService *svc)
    {
        Services::instance().setIpcClient(svc);
    }
    ~ScopedServicesIpcGuard() { Services::instance().setIpcClient(nullptr); }
    ScopedServicesIpcGuard(const ScopedServicesIpcGuard &) = delete;
    ScopedServicesIpcGuard &operator=(const ScopedServicesIpcGuard &) = delete;
};

} // namespace

static void test_mainwindow_apply_wallpaper_ipc_integration_and_command_bus_fallback()
{
    std::cout << "[E2E] Testing MainWindow::applyWallpaperPath -> IpcClient::setWallpaper & "
                 "ApplyWallpaperCommand fallback (E2E-F03/F04, AUD-151/420)..."
              << std::endl;

    QTemporaryDir tmpDir;
    assert(tmpDir.isValid());

    const QString pathA = tmpDir.filePath(QStringLiteral("center_wall.png"));
    const QString pathB = tmpDir.filePath(QStringLiteral("span_wall.png"));
    QImage imgA(8, 8, QImage::Format_RGBA8888);
    imgA.fill(QColor(30, 120, 210, 255));
    assert(imgA.save(pathA, "PNG"));
    QImage imgB(8, 8, QImage::Format_RGBA8888);
    imgB.fill(QColor(210, 90, 40, 255));
    assert(imgB.save(pathB, "PNG"));

    /* 1. CommandBus ApplyWallpaperCommand null-applyFunc fallback preserves m_output & path */
    {
        MockIpcClientService cmdMock;
        ScopedServicesIpcGuard guard(&cmdMock);
        CommandBus bus(64);

        waywal_img_metadata_t oldMeta{};
        oldMeta.scaling_mode = WAYWAL_SCALING_CENTER;
        waywal_img_metadata_t newMeta{};
        newMeta.scaling_mode = WAYWAL_SCALING_FIT;
        newMeta.transition_type = WAYWAL_TRANSITION_WIPE;

        bus.execute(std::make_unique<ApplyWallpaperCommand>(
            QStringLiteral("DP-1, HDMI-A-1"), oldMeta, newMeta, pathA, pathB, nullptr));
        assert(cmdMock.sendImageCalls.empty());
        assert(cmdMock.setWallpaperCalls.size() == 1u);
        assert((cmdMock.setWallpaperCalls[0].targetOutputs ==
                QStringList{QStringLiteral("DP-1"), QStringLiteral("HDMI-A-1")}));
        assert(cmdMock.setWallpaperCalls[0].filepath == pathB);
        assert(cmdMock.setWallpaperCalls[0].scaling_mode == WAYWAL_SCALING_FIT);

        bus.undo();
        assert(cmdMock.setWallpaperCalls.size() == 2u);
        assert((cmdMock.setWallpaperCalls[1].targetOutputs ==
                QStringList{QStringLiteral("DP-1"), QStringLiteral("HDMI-A-1")}));
        assert(cmdMock.setWallpaperCalls[1].filepath == pathA);
        assert(cmdMock.setWallpaperCalls[1].scaling_mode == WAYWAL_SCALING_CENTER);

        bus.redo();
        assert(cmdMock.setWallpaperCalls.size() == 3u);
        assert(cmdMock.setWallpaperCalls[2].filepath == pathB);

        bus.execute(std::make_unique<ApplyWallpaperCommand>(QStringLiteral("*"), oldMeta, newMeta,
                                                            pathA, pathB, nullptr));
        assert(cmdMock.setWallpaperCalls.size() == 4u);
        assert(cmdMock.setWallpaperCalls[3].targetOutputs.isEmpty());
    }

    /* 2. StudioSessionContext media classification & CLI fallback predicate */
    assert(!StudioSessionContext::requiresCliFallbackMediaPath(pathA));
    assert(StudioSessionContext::requiresCliFallbackMediaPath(
        tmpDir.filePath(QStringLiteral("loop.mp4"))));
    assert(StudioSessionContext::requiresCliFallbackMediaPath(
        tmpDir.filePath(QStringLiteral("fx.glsl"))));
    assert(StudioSessionContext::requiresCliFallbackMediaPath(tmpDir.path()));

    /* 3. Live MainWindow::applyWallpaperPath -> IIpcClientService::setWallpaper (Center vs Span) */
    MockIpcClientService mockIpc;
    StudioOptions opts;
    opts.headlessCheck = true;
    MainWindow win(opts);
    ScopedServicesIpcGuard guard(&mockIpc);

    assert(win.sessionContext() != nullptr);
    assert(win.sessionContext()->activeIpcService() == &mockIpc);

    auto *canvas = win.findChild<MonitorTopologyCanvas *>();
    assert(canvas != nullptr);

    const QJsonObject outputsObj{
        {QStringLiteral("outputs"),
         QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("DP-1")},
                                {QStringLiteral("x"), 0},
                                {QStringLiteral("y"), 0},
                                {QStringLiteral("width"), 1920},
                                {QStringLiteral("height"), 1080},
                                {QStringLiteral("scale"), 1.0},
                                {QStringLiteral("primary"), true}},
                    QJsonObject{{QStringLiteral("name"), QStringLiteral("HDMI-A-1")},
                                {QStringLiteral("x"), 1920},
                                {QStringLiteral("y"), 0},
                                {QStringLiteral("width"), 1920},
                                {QStringLiteral("height"), 1080},
                                {QStringLiteral("scale"), 1.0},
                                {QStringLiteral("primary"), false}}}}};
    canvas->updateOutputsFromJson(outputsObj);

    {
        const QSignalBlocker blocker(canvas);
        canvas->setSelectedMonitors({QStringLiteral("DP-1")});
        canvas->setDesktopSpanMode(false);
        canvas->setScalingMode(WAYWAL_SCALING_CENTER);
        canvas->setCropGravity(QStringLiteral("DP-1"), QPointF(0.20, 0.80));
    }

    assert(win.bezierEditor() != nullptr);
    TransitionConfig tcfg;
    tcfg.type = WAYWAL_TRANSITION_WIPE;
    tcfg.durationMs = 750u;
    tcfg.fps = 120u;
    tcfg.angleDeg = 45.0f;
    tcfg.waveFreq = 14.0f;
    tcfg.waveAmp = 0.25f;
    tcfg.centerX = 0.30f;
    tcfg.centerY = 0.70f;
    tcfg.hasCustomBezier = true;
    tcfg.bezier = bezier_curve_t{{0.15f}, {0.85f}, {0.70f}, {0.95f}};
    win.bezierEditor()->setTransitionConfig(tcfg);

    assert(win.themeStudio() != nullptr);
    win.themeStudio()->setThemeMode(WAYWAL_THEME_LIGHT);
    win.themeStudio()->setSeedIndex(2u);

    /* Apply via MonitorTopologyCanvas wallpaper drop signal -> Center scaling + per-monitor DP-1 */
    emit canvas->wallpaperDropped({QStringLiteral("DP-1")}, pathA);
    assert(mockIpc.setWallpaperCalls.size() == 1u);
    {
        const WallpaperApplyOptions &c = mockIpc.setWallpaperCalls[0];
        assert(c.filepath == pathA);
        assert((c.targetOutputs == QStringList{QStringLiteral("DP-1")}));
        assert(c.scaling_mode == WAYWAL_SCALING_CENTER);
        assert(c.transition_type == WAYWAL_TRANSITION_WIPE);
        assert(c.transition_duration_ms == 750u);
        assert(c.transition_fps == 120u);
        assert(c.has_crop_gravity);
        assert(std::fabs(c.crop_gravity_x - 0.20f) < 1e-4f);
        assert(std::fabs(c.crop_gravity_y - 0.80f) < 1e-4f);
        assert(c.has_custom_bezier);
        assert(std::fabs(c.bezier.p1x - 0.15f) < 1e-4f);
        assert(std::fabs(c.bezier.p1y - 0.85f) < 1e-4f);
        assert(std::fabs(c.bezier.p2x - 0.70f) < 1e-4f);
        assert(std::fabs(c.bezier.p2y - 0.95f) < 1e-4f);
        assert(c.has_theme_mode && c.theme_light_mode);
        assert(c.has_source_color_index && c.source_color_index == 2u);
        assert(c.batteryPolicy == QStringLiteral("ignore"));
    }

    /* Toggle Desktop Span mode -> Span scaling + wildcard outputs (empty targetOutputs) */
    canvas->setDesktopSpanMode(true);
    assert(mockIpc.setWallpaperCalls.size() == 2u);
    assert(mockIpc.setWallpaperCalls[1].scaling_mode == WAYWAL_SCALING_SPAN);
    assert(mockIpc.setWallpaperCalls[1].targetOutputs.isEmpty());
    assert(mockIpc.setWallpaperCalls[1].filepath == pathA);

    /* Undo/Redo through MainWindow CommandBus restores Center/DP-1 and re-applies Span */
    win.handleUndo();
    assert(mockIpc.setWallpaperCalls.size() == 3u);
    assert(mockIpc.setWallpaperCalls[2].scaling_mode == WAYWAL_SCALING_CENTER);
    assert((mockIpc.setWallpaperCalls[2].targetOutputs == QStringList{QStringLiteral("DP-1")}));

    win.handleRedo();
    assert(mockIpc.setWallpaperCalls.size() == 4u);
    assert(mockIpc.setWallpaperCalls[3].scaling_mode == WAYWAL_SCALING_SPAN);
    assert(mockIpc.setWallpaperCalls[3].targetOutputs.isEmpty());

    /* Exit span mode and re-apply explicitly to HDMI-A-1 via GpuPreviewViewport */
    {
        const QSignalBlocker blocker(canvas);
        canvas->setDesktopSpanMode(false);
        canvas->setScalingMode(WAYWAL_SCALING_FIT);
    }
    assert(win.previewViewport() != nullptr);
    emit win.previewViewport()->applyRequested(QStringLiteral("HDMI-A-1"));
    assert(mockIpc.setWallpaperCalls.size() == 5u);
    assert(mockIpc.setWallpaperCalls[4].scaling_mode == WAYWAL_SCALING_FIT);
    assert((mockIpc.setWallpaperCalls[4].targetOutputs == QStringList{QStringLiteral("HDMI-A-1")}));

    std::cout << "  -> MainWindow::applyWallpaperPath IPC integration & CommandBus fallback "
                 "verified cleanly."
              << std::endl;
}

static void test_theme_studio_to_mainwindow_live_signal_execution()
{
    std::cout << "[E2E] Testing live ThemeStudioWidget -> MainWindow signal execution (E2E-F08)..."
              << std::endl;

    QTemporaryDir tmpDir;
    assert(tmpDir.isValid());
    const QString wallPath = tmpDir.filePath(QStringLiteral("theme_source.png"));
    QImage wallImg(16, 16, QImage::Format_RGBA8888);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            wallImg.setPixelColor(x, y, QColor(40 + x * 10, 90 + y * 8, 220 - x * 6, 255));
        }
    }
    assert(wallImg.save(wallPath, "PNG"));

    MockIpcClientService mockIpc;
    StudioOptions opts;
    opts.headlessCheck = true;
    MainWindow win(opts);
    ScopedServicesIpcGuard guard(&mockIpc);

    ThemeStudioWidget *theme = win.themeStudio();
    assert(theme != nullptr);
    auto *canvas = win.findChild<MonitorTopologyCanvas *>();
    assert(canvas != nullptr);
    auto *statusLabel = win.findChild<QLabel *>(QStringLiteral("studioStatusLabel"));
    assert(statusLabel != nullptr);

    int paletteCount = 0;
    waywal_palette_t lastPalette{};
    std::vector<waywal_theme_mode_t> observedModes;
    std::vector<waywal_scheme_variant_t> observedVariants;
    std::vector<float> observedContrasts;
    std::vector<uint8_t> observedSeeds;
    QStringList statusMessages;

    QObject::connect(theme, &ThemeStudioWidget::paletteChanged, [&](const waywal_palette_t &p) {
        ++paletteCount;
        lastPalette = p;
    });
    QObject::connect(theme, &ThemeStudioWidget::modeChanged,
                     [&](waywal_theme_mode_t m) { observedModes.push_back(m); });
    QObject::connect(theme, &ThemeStudioWidget::variantChanged,
                     [&](waywal_scheme_variant_t v) { observedVariants.push_back(v); });
    QObject::connect(theme, &ThemeStudioWidget::contrastChanged,
                     [&](float c) { observedContrasts.push_back(c); });
    QObject::connect(theme, &ThemeStudioWidget::seedIndexChanged,
                     [&](uint8_t idx) { observedSeeds.push_back(idx); });
    QObject::connect(theme, &ThemeStudioWidget::statusMessageRequested,
                     [&](const QString &msg) { statusMessages.push_back(msg); });

    /* Activate a wallpaper on MainWindow so theme mutations re-sync active IPC state */
    emit canvas->wallpaperDropped({QStringLiteral("DP-1")}, wallPath);
    assert(mockIpc.setWallpaperCalls.size() == 1u);

    theme->setWallpaperImage(wallImg, wallPath);
    assert(paletteCount >= 1);

    auto *swatchChip = theme->findChild<ColorSwatchChip *>();
    assert(swatchChip != nullptr);
    emit swatchChip->copiedToClipboard(QStringLiteral("primary"), QStringLiteral("#4488ff"));
    assert(!statusMessages.isEmpty());
    assert(statusLabel->text() == QStringLiteral("Copied #4488ff (primary) to clipboard!"));

    theme->setThemeMode(WAYWAL_THEME_LIGHT);
    assert(!observedModes.empty() && observedModes.back() == WAYWAL_THEME_LIGHT);
    assert(statusLabel->text() == QStringLiteral("Theme Mode: Light"));
    assert(mockIpc.audioPowerCalls.size() >= 1u);
    assert(mockIpc.setWallpaperCalls.size() >= 2u);
    assert(mockIpc.setWallpaperCalls.back().has_theme_mode);
    assert(mockIpc.setWallpaperCalls.back().theme_light_mode);

    theme->setSchemeVariant(WAYWAL_SCHEME_VIBRANT);
    assert(!observedVariants.empty() && observedVariants.back() == WAYWAL_SCHEME_VIBRANT);
    assert(statusLabel->text() ==
           QStringLiteral("Theme Variant: %1").arg(static_cast<int>(WAYWAL_SCHEME_VIBRANT)));
    const size_t audioCountAfterVariant = mockIpc.audioPowerCalls.size();
    assert(audioCountAfterVariant >= 2u);

    theme->setContrast(0.45f);
    assert(!observedContrasts.empty() && std::fabs(observedContrasts.back() - 0.45f) < 1e-4f);
    assert(statusLabel->text() == QStringLiteral("Theme Contrast: 0.45"));
    assert(mockIpc.audioPowerCalls.size() > audioCountAfterVariant);

    const size_t wallpaperCountBeforeSeed = mockIpc.setWallpaperCalls.size();
    theme->setSeedIndex(3u);
    assert(!observedSeeds.empty() && observedSeeds.back() == 3u);
    assert(statusLabel->text() == QStringLiteral("Theme Seed Index: 3"));
    assert(mockIpc.setWallpaperCalls.size() > wallpaperCountBeforeSeed);
    assert(mockIpc.setWallpaperCalls.back().has_source_color_index);
    assert(mockIpc.setWallpaperCalls.back().source_color_index == 3u);
    assert(lastPalette.source_color_index == 3u);

    std::cout << "  -> Live ThemeStudioWidget -> MainWindow signal execution (E2E-F08) verified "
                 "cleanly."
              << std::endl;
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qunsetenv("DISPLAY");
    qunsetenv("WAYLAND_DISPLAY");
    QApplication app(argc, argv);

    std::cout << "==========================================================" << std::endl;
    std::cout << "  Studio Command Bus, Docking & Span Topology Test Suite  " << std::endl;
    std::cout << "==========================================================" << std::endl;

    test_command_bus_1000_steps();
    test_micro_drag_coalescing();
    test_dock_layout_manager_roundtrip();
    test_monitor_topology_span_math();
    test_studio_ui_ergonomics_and_zero_emoji();
    test_property_inspector_oklch_and_topology_zoom();
    test_mainwindow_apply_wallpaper_ipc_integration_and_command_bus_fallback();
    test_theme_studio_to_mainwindow_live_signal_execution();

    std::cout << "==========================================================" << std::endl;
    std::cout << "  >>> STUDIO COMMAND & DOCK SUITE PASSED CLEANLY (100%) <<<" << std::endl;
    std::cout << "==========================================================" << std::endl;

    return 0;
}
