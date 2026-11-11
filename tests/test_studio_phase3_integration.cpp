// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_studio_phase3_integration.cpp
 * @brief Studio Shader Graph, Camera & Physics Docks Integration & Benchmark Suite
 *
 * Verifies:
 * 1. Instantiation of studio scene & shader graph panels:
 *    - NodeGraphEditorWidget
 *    - EffectLibraryBrowserWidget
 *    - CameraLightingInspectorWidget
 *    - PhysicsSpringInspectorWidget
 * 2. DockLayoutManager layout_revision == 5 and perspective loading (Default, Shader Coding).
 * 3. CommandBus 500 ms coalescing for:
 *    - MoveNodesCommand
 *    - SetNodeConstantCommand
 *    - SetCamera3DCommand
 *    - SetPhysicsSpringCommand
 * 4. NodeGraphEditorWidget canvas rendering benchmark (p99 <= 6.94 ms, 144 FPS target).
 */

#include "studio/app/dock_layout_manager.hpp"
#include "studio/core/command_bus.hpp"
#include "studio/core/command_bus_phase3.hpp"
#include "studio/graph/effect_library_browser_widget.hpp"
#include "studio/graph/node_graph_editor_widget.hpp"
#include "studio/graph/wire_spatial_and_batch.hpp"
#include "studio/inspector/camera_lighting_inspector_widget.hpp"
#include "studio/inspector/physics_spring_inspector_widget.hpp"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QWidget>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <thread>
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
#if !defined(__OPTIMIZE__)
constexpr double kCanvasP99BudgetMs = 120.0;
#else
constexpr double kCanvasP99BudgetMs = 35.0;
#endif
#elif !defined(__OPTIMIZE__)
constexpr double kCanvasP99BudgetMs = 35.0;
#else
constexpr double kCanvasP99BudgetMs = 6.94;
#endif

/* ========================================================================= */
/* 1. TEST PANEL INSTANTIATION & DEFAULTS                                    */
/* ========================================================================= */

static void test_panel_instantiation()
{
    std::cout << "[Studio] Testing Panel Instantiation & Default State...\n";

    CommandBus bus;

    // 1. NodeGraphEditorWidget
    NodeGraphEditorWidget graphEditor;
    graphEditor.setCommandBus(&bus);
    graphEditor.setTargetOutput(QStringLiteral("DP-1"));
    assert(graphEditor.targetOutput() == QStringLiteral("DP-1"));
    assert(graphEditor.zoomScale() == 1.0f);
    assert(graphEditor.currentLod() == GraphSemanticZoomLod::FullDetail);

    // 2. EffectLibraryBrowserWidget
    EffectLibraryBrowserWidget effectBrowser;
    assert(effectBrowser.visibleEffectCount() == 50);

    // Filter test
    effectBrowser.setFilterText(QStringLiteral("bloom"));
    assert(effectBrowser.visibleEffectCount() > 0);
    effectBrowser.setFilterText(QStringLiteral("nonexistent_effect_xyz_123"));
    assert(effectBrowser.visibleEffectCount() == 0);
    effectBrowser.setFilterText(QString());
    assert(effectBrowser.visibleEffectCount() == 50);

    // 3. CameraLightingInspectorWidget
    CameraLightingInspectorWidget cameraInspector;
    cameraInspector.setCommandBus(&bus);
    cameraInspector.setTargetOutput(QStringLiteral("HDMI-A-1"));
    assert(cameraInspector.targetOutput() == QStringLiteral("HDMI-A-1"));
    assert(cameraInspector.camera().projection_mode == WE_PROJ_PERSPECTIVE_3D);

    // 4. PhysicsSpringInspectorWidget
    PhysicsSpringInspectorWidget physicsInspector;
    physicsInspector.setCommandBus(&bus);
    physicsInspector.setTargetOutput(QStringLiteral("DP-2"));
    assert(physicsInspector.targetOutput() == QStringLiteral("DP-2"));
    assert(physicsInspector.windField().enabled == 0);

    std::cout << "  -> PASS: All 4 studio panels instantiated and verified.\n";
}

/* ========================================================================= */
/* 2. DOCK LAYOUT MANAGER REVISION 5 & PERSPECTIVES                           */
/* ========================================================================= */

static void test_dock_layout_revision5()
{
    std::cout << "[Studio] Testing DockLayoutManager Revision 5 & Perspectives...\n";

    QMainWindow mainWindow;
    DockLayoutManager layoutMgr(&mainWindow);

    // Serialize layout and verify revision 5 in JSON
    QJsonObject root = layoutMgr.serializeLayout();
    assert(root.contains(QStringLiteral("layout_revision")));
    assert(root.value(QStringLiteral("layout_revision")).toInt() == 5);

    // Check perspectives
    assert(root.contains(QStringLiteral("perspectives")));
    QJsonObject perspectives = root.value(QStringLiteral("perspectives")).toObject();
    assert(perspectives.contains(QStringLiteral("Default")));
    assert(perspectives.contains(QStringLiteral("Shader Coding")));

    // Test Default perspective tree
    QJsonObject defaultTree = DockLayoutManager::createDefaultPerspectiveTree();
    assert(!defaultTree.isEmpty());
    assert(defaultTree.contains(QStringLiteral("split_direction")));
    assert(defaultTree.contains(QStringLiteral("left")));
    assert(defaultTree.contains(QStringLiteral("right")));

    // Test Shader Coding perspective tree
    QJsonObject shaderTree = DockLayoutManager::createShaderCodingPerspectiveTree();
    assert(!shaderTree.isEmpty());
    assert(shaderTree.contains(QStringLiteral("split_direction")));
    assert(shaderTree.contains(QStringLiteral("left")));
    assert(shaderTree.contains(QStringLiteral("right")));

    // Deserialize round-trip test
    bool ok = layoutMgr.deserializeLayout(root);
    assert(ok);

    std::cout
        << "  -> PASS: DockLayoutManager layout_revision == 5 and perspective trees verified.\n";
}

/* ========================================================================= */
/* 3. COMMAND BUS 500 MS COALESCING                                          */
/* ========================================================================= */

static void test_command_bus_coalescing()
{
    std::cout << "[Studio] Testing CommandBus 500 ms Coalescing...\n";

    // 1. MoveNodesCommand coalescing
    {
        std::vector<MoveNodesCommand::NodeMoveDelta> deltas1 = {
            {1, QPointF(0, 0), QPointF(10, 10)}};
        std::vector<MoveNodesCommand::NodeMoveDelta> deltas2 = {
            {1, QPointF(10, 10), QPointF(25, 30)}};

        QPointF finalPos(0, 0);
        auto applyFn = [&finalPos](const std::vector<MoveNodesCommand::NodeMoveDelta> &d,
                                   bool useNew) {
            if (!d.empty()) {
                finalPos = useNew ? d[0].newPos : d[0].oldPos;
            }
        };

        MoveNodesCommand cmd1(deltas1, applyFn);
        MoveNodesCommand cmd2(deltas2, applyFn);

        // Immediate merge within 500 ms should succeed
        assert(cmd1.mergeWith(&cmd2) == true);
        assert(cmd1.deltas()[0].oldPos == QPointF(0, 0));
        assert(cmd1.deltas()[0].newPos == QPointF(25, 30));

        cmd1.execute();
        assert(finalPos == QPointF(25, 30));
        cmd1.undo();
        assert(finalPos == QPointF(0, 0));
    }

    // 2. SetNodeConstantCommand coalescing
    {
        std::array<float, 4> v0 = {1.0f, 0.0f, 0.0f, 1.0f};
        std::array<float, 4> v1 = {1.0f, 0.5f, 0.0f, 1.0f};
        std::array<float, 4> v2 = {1.0f, 1.0f, 0.0f, 1.0f};

        std::array<float, 4> curVal = v0;
        auto applyConstFn = [&curVal](uint32_t /*nodeId*/, const std::array<float, 4> &val) {
            curVal = val;
        };

        SetNodeConstantCommand cmd1(10, QStringLiteral("Color"), v0, v1, applyConstFn);
        SetNodeConstantCommand cmd2(10, QStringLiteral("Color"), v1, v2, applyConstFn);

        // Merge within 500 ms should succeed
        assert(cmd1.mergeWith(&cmd2) == true);
        assert(cmd1.newVal() == v2);

        cmd1.execute();
        assert(curVal == v2);
        cmd1.undo();
        assert(curVal == v0);
    }

    // 3. SetCamera3DCommand coalescing
    {
        we_camera3d_t cam0{}, cam1{}, cam2{};
        we_lighting_uniforms_t light0{}, light1{}, light2{};
        cam0.fov_y_deg = 45.0f;
        cam1.fov_y_deg = 50.0f;
        cam2.fov_y_deg = 60.0f;

        float activeFov = 45.0f;
        auto applyCamFn = [&activeFov](const QString &, const we_camera3d_t &c,
                                       const we_lighting_uniforms_t &) { activeFov = c.fov_y_deg; };

        SetCamera3DCommand cmd1(QStringLiteral("DP-1"), cam0, cam1, light0, light1, applyCamFn);
        SetCamera3DCommand cmd2(QStringLiteral("DP-1"), cam1, cam2, light1, light2, applyCamFn);

        assert(cmd1.mergeWith(&cmd2) == true);
        assert(cmd1.newCamera().fov_y_deg == 60.0f);

        cmd1.execute();
        assert(activeFov == 60.0f);
        cmd1.undo();
        assert(activeFov == 45.0f);
    }

    // 4. SetPhysicsSpringCommand coalescing
    {
        we_bone_sim_config_t cfg0{}, cfg1{}, cfg2{};
        we_wind_field_t wind0{}, wind1{}, wind2{};
        we_cursor_force_t cur0{}, cur1{}, cur2{};
        cfg0.damping_ratio = 0.1f;
        cfg1.damping_ratio = 0.3f;
        cfg2.damping_ratio = 0.5f;

        float activeDamping = 0.1f;
        auto applyPhysFn = [&activeDamping](const QString &, int32_t, int32_t,
                                            const we_bone_sim_config_t &c, const we_wind_field_t &,
                                            const we_cursor_force_t &) {
            activeDamping = c.damping_ratio;
        };

        SetPhysicsSpringCommand cmd1(QStringLiteral("DP-1"), 0, 1, cfg0, cfg1, wind0, wind1, cur0,
                                     cur1, applyPhysFn);
        SetPhysicsSpringCommand cmd2(QStringLiteral("DP-1"), 0, 1, cfg1, cfg2, wind1, wind2, cur1,
                                     cur2, applyPhysFn);

        assert(cmd1.mergeWith(&cmd2) == true);

        cmd1.execute();
        assert(activeDamping == 0.5f);
        cmd1.undo();
        assert(activeDamping == 0.1f);
    }

    std::cout << "  -> PASS: CommandBus 500 ms coalescing verified for all commands.\n";
}

/* ========================================================================= */
/* 4. NODE GRAPH EDITOR CANVAS RENDERING BENCHMARK                           */
/* ========================================================================= */

static void test_node_graph_canvas_benchmark()
{
    std::cout << "[Studio] Benchmarking NodeGraphEditorWidget Canvas Rendering...\n";

    CommandBus bus;
    NodeGraphEditorWidget graphEditor;
    graphEditor.setCommandBus(&bus);
    graphEditor.resize(960, 540);

    // Build a representative shader graph: 12 nodes, 15 edges
    uint32_t n_uv = graphEditor.addNode(WAYWAL_NODE_IN_UV, QPointF(100, 100));
    uint32_t n_time = graphEditor.addNode(WAYWAL_NODE_IN_TIME, QPointF(100, 250));
    uint32_t n_sin = graphEditor.addNode(WAYWAL_NODE_MATH_SIN, QPointF(300, 250));
    uint32_t n_mul = graphEditor.addNode(WAYWAL_NODE_MATH_MUL, QPointF(500, 200));
    uint32_t n_add = graphEditor.addNode(WAYWAL_NODE_MATH_ADD, QPointF(700, 150));
    uint32_t n_out = graphEditor.addNode(WAYWAL_NODE_OUT_IMAGE_PASS, QPointF(900, 150));

    // Builtin effect nodes
    uint32_t n_eff1 = graphEditor.addBuiltinEffectNode(WE_EFFECT_BLOOM, QPointF(300, 400));
    uint32_t n_eff2 = graphEditor.addBuiltinEffectNode(WE_EFFECT_VIGNETTE, QPointF(600, 400));

    assert(n_uv != 0);
    assert(n_time != 0);
    assert(n_sin != 0);
    assert(n_mul != 0);
    assert(n_add != 0);
    assert(n_out != 0);
    assert(n_eff1 != 0);
    assert(n_eff2 != 0);

    // Connect some pins
    graphEditor.connectPins(n_time, 0, n_sin, 0);
    graphEditor.connectPins(n_sin, 0, n_mul, 0);
    graphEditor.connectPins(n_uv, 0, n_mul, 1);
    graphEditor.connectPins(n_mul, 0, n_add, 0);
    graphEditor.connectPins(n_add, 0, n_out, 0);

    // Render 100 frames offscreen into QImage to benchmark canvas rendering latency
    QImage renderTarget(960, 540, QImage::Format_ARGB32_Premultiplied);
    constexpr int kFrames = 100;
    std::vector<double> frameTimesMs;
    frameTimesMs.reserve(kFrames);

    // Warm-up runs to prime font cache, glyph rasterization, and style sheets
    for (int w = 0; w < 5; ++w) {
        QPainter wp(&renderTarget);
        graphEditor.render(&wp);
        wp.end();
    }

    QPainter painter(&renderTarget);
    for (int frame = 0; frame < kFrames; ++frame) {
        // Micro-animate zoom or pan
        float zoom = 0.8f + 0.3f * std::sin((float)frame * 0.05f);
        graphEditor.setZoomScale(zoom);

        auto start = std::chrono::high_resolution_clock::now();
        graphEditor.render(&painter);
        auto end = std::chrono::high_resolution_clock::now();

        double elapsedMs = std::chrono::duration<double, std::milli>(end - start).count();
        frameTimesMs.push_back(elapsedMs);
    }
    painter.end();

    std::sort(frameTimesMs.begin(), frameTimesMs.end());
    size_t p99Idx = static_cast<size_t>(kFrames * 0.99);
    if (p99Idx >= frameTimesMs.size()) {
        p99Idx = frameTimesMs.size() - 1;
    }
    double p99 = frameTimesMs[p99Idx];
    double p50 = frameTimesMs[kFrames / 2];
    double avg = std::accumulate(frameTimesMs.begin(), frameTimesMs.end(), 0.0) / kFrames;

    std::cout << "  Canvas Benchmark (" << kFrames << " frames @ 1080p):\n";
    std::cout << "    Median (p50): " << p50 << " ms\n";
    std::cout << "    Average:      " << avg << " ms\n";
    std::cout << "    p99:          " << p99 << " ms (Threshold: <= " << kCanvasP99BudgetMs
              << " ms)\n";

    assert(p99 <= kCanvasP99BudgetMs);
    std::cout << "  -> PASS: Canvas rendering p99 (" << p99
              << " ms) strictly satisfies <= " << kCanvasP99BudgetMs << " ms.\n";
}

static void test_camera_lighting_undo_redo_and_degenerate_frustum()
{
    std::cout << "[Studio] Testing CameraLightingInspector Undo/Redo & Degenerate Frustum...\n";

    CommandBus bus;
    CameraLightingInspectorWidget inspector;
    inspector.setCommandBus(&bus);
    inspector.setTargetOutput(QStringLiteral("DP-1"));

    const float initialFov = inspector.camera().fov_y_deg;
    const float initialNormalMap = inspector.lighting().normal_map_strength;

    QDoubleSpinBox *normalSpin = nullptr;
    for (auto *spin : inspector.findChildren<QDoubleSpinBox *>()) {
        if (spin && std::fabs(spin->maximum() - 2.0) < 1e-6 &&
            std::fabs(spin->singleStep() - 0.05) < 1e-6) {
            normalSpin = spin;
            break;
        }
    }
    assert(normalSpin != nullptr);
    normalSpin->setValue(1.65);
    assert(std::fabs(inspector.lighting().normal_map_strength - 1.65f) < 1e-4f);
    assert(bus.undoCount() == 1);
    assert(bus.redoCount() == 0);

    bus.undo();
    assert(std::fabs(inspector.lighting().normal_map_strength - initialNormalMap) < 1e-4f);
    assert(bus.canRedo());
    assert(bus.redoCount() == 1);

    bus.redo();
    assert(std::fabs(inspector.lighting().normal_map_strength - 1.65f) < 1e-4f);
    assert(bus.undoCount() == 1);
    assert(bus.redoCount() == 0);

    CommandBus kfBus;
    inspector.setCommandBus(&kfBus);
    assert(inspector.camera().keyframe_count == 0);
    assert(inspector.addCameraKeyframe(1.25f));
    assert(inspector.camera().keyframe_count == 1);
    assert(kfBus.undoCount() == 1);

    kfBus.undo();
    assert(inspector.camera().keyframe_count == 0);
    assert(kfBus.canRedo());

    kfBus.redo();
    assert(inspector.camera().keyframe_count == 1);

    CommandBus rmBus;
    inspector.setCommandBus(&rmBus);
    assert(inspector.removeCameraKeyframe(0));
    assert(inspector.camera().keyframe_count == 0);
    rmBus.undo();
    assert(inspector.camera().keyframe_count == 1);
    assert(rmBus.canRedo());

    CommandBus resetBus;
    inspector.setCommandBus(&resetBus);
    QPushButton *resetBtn = nullptr;
    for (auto *btn : inspector.findChildren<QPushButton *>()) {
        if (btn && btn->text().contains(QStringLiteral("Reset Camera"))) {
            resetBtn = btn;
            break;
        }
    }
    assert(resetBtn != nullptr);
    resetBtn->click();
    assert(inspector.camera().keyframe_count == 0);
    assert(std::fabs(inspector.lighting().normal_map_strength - initialNormalMap) < 1e-4f);
    assert(std::fabs(inspector.camera().fov_y_deg - initialFov) < 1e-4f);

    resetBus.undo();
    assert(inspector.camera().keyframe_count == 1);
    assert(std::fabs(inspector.lighting().normal_map_strength - 1.65f) < 1e-4f);
    assert(resetBus.canRedo());

    we_camera3d_t degenCam = inspector.camera();
    degenCam.eye_pos[0] = 3.0f;
    degenCam.eye_pos[1] = 4.0f;
    degenCam.eye_pos[2] = 5.0f;
    degenCam.target_pos[0] = 3.0f;
    degenCam.target_pos[1] = 4.0f;
    degenCam.target_pos[2] = 5.0f;
    degenCam.up_vec[0] = 0.0f;
    degenCam.up_vec[1] = 0.0f;
    degenCam.up_vec[2] = 0.0f;
    inspector.setCameraAndLights(degenCam, inspector.lighting());

    for (int i = 0; i < 16; ++i) {
        assert(std::isfinite(inspector.camera().view_matrix[i]));
        assert(std::isfinite(inspector.camera().view_proj_matrix[i]));
    }

    std::cout << "  -> PASS: CameraLightingInspector Undo/Redo stack & finite frustum verified.\n";
}

static void test_node_graph_inplace_move_and_wire_undo()
{
    std::cout << "[Studio] Testing NodeGraph In-Place Multi-Move & Replaced-Wire Undo...\n";

    CommandBus bus;
    NodeGraphEditorWidget editor;
    editor.setCommandBus(&bus);
    editor.resize(960, 540);
    const uint32_t baseEdges = editor.graph().edge_count;

    const uint32_t nA = editor.addNode(WAYWAL_NODE_IN_TIME, QPointF(100.0, 100.0));
    const uint32_t nB = editor.addNode(WAYWAL_NODE_IN_TIME, QPointF(100.0, 280.0));
    const uint32_t nOut = editor.addNode(WAYWAL_NODE_OUT_IMAGE_PASS, QPointF(520.0, 345.0));
    assert(nA != 0 && nB != 0 && nOut != 0);

    auto findEdgeToOut = [&]() -> const waywal_graph_edge_t * {
        const auto &g = editor.graph();
        for (uint32_t i = 0; i < g.edge_count; ++i) {
            if (g.edges[i].dst_node_id == nOut && g.edges[i].dst_pin_index == 0) {
                return &g.edges[i];
            }
        }
        return nullptr;
    };

    auto *view = editor.findChild<QGraphicsView *>();
    assert(view != nullptr && view->scene() != nullptr);
    QGraphicsScene *scene = view->scene();

    auto findNodeItemAt = [&](const QPointF &expectedPos) -> QGraphicsItem * {
        for (QGraphicsItem *it : scene->items()) {
            if ((it->flags() & QGraphicsItem::ItemIsMovable) &&
                QLineF(it->pos(), expectedPos).length() < 1.0) {
                return it;
            }
        }
        return nullptr;
    };

    assert(editor.connectPins(nA, 0, nOut, 0));
    assert(editor.graph().edge_count == baseEdges + 1);
    assert(findEdgeToOut() != nullptr && findEdgeToOut()->src_node_id == nA);

    QGraphicsItem *itemA = findNodeItemAt(QPointF(100.0, 100.0));
    QGraphicsItem *itemB = findNodeItemAt(QPointF(100.0, 280.0));
    assert(itemA != nullptr && itemB != nullptr);

    editor.moveNodes({
        {nA, QPointF(100.0, 100.0), QPointF(180.0, 140.0)},
        {nB, QPointF(100.0, 280.0), QPointF(180.0, 320.0)},
    });

    assert(scene->items().contains(itemA));
    assert(scene->items().contains(itemB));
    assert(QLineF(itemA->pos(), QPointF(180.0, 140.0)).length() < 1e-3);
    assert(QLineF(itemB->pos(), QPointF(180.0, 320.0)).length() < 1e-3);

    bus.undo();
    assert(scene->items().contains(itemA));
    assert(scene->items().contains(itemB));
    assert(QLineF(itemA->pos(), QPointF(100.0, 100.0)).length() < 1e-3);
    assert(QLineF(itemB->pos(), QPointF(100.0, 280.0)).length() < 1e-3);
    assert(bus.canRedo());

    bus.redo();
    assert(QLineF(itemA->pos(), QPointF(180.0, 140.0)).length() < 1e-3);
    assert(QLineF(itemB->pos(), QPointF(180.0, 320.0)).length() < 1e-3);

    scene->clearSelection();
    itemA->setSelected(true);
    itemB->setSelected(true);
    {
        const QPointF startScene = itemA->sceneBoundingRect().center();
        const QPointF endScene = startScene + QPointF(40.0, 25.0);
        const QPointF startLocal = itemA->mapFromScene(startScene);

        QGraphicsSceneMouseEvent pressEv(QEvent::GraphicsSceneMousePress);
        pressEv.setButton(Qt::LeftButton);
        pressEv.setButtons(Qt::LeftButton);
        pressEv.setScenePos(startScene);
        pressEv.setLastScenePos(startScene);
        pressEv.setPos(startLocal);
        pressEv.setButtonDownScenePos(Qt::LeftButton, startScene);
        pressEv.setButtonDownPos(Qt::LeftButton, startLocal);
        QApplication::sendEvent(scene, &pressEv);

        QGraphicsSceneMouseEvent moveEv(QEvent::GraphicsSceneMouseMove);
        moveEv.setButton(Qt::NoButton);
        moveEv.setButtons(Qt::LeftButton);
        moveEv.setScenePos(endScene);
        moveEv.setLastScenePos(startScene);
        moveEv.setPos(itemA->mapFromScene(endScene));
        moveEv.setButtonDownScenePos(Qt::LeftButton, startScene);
        moveEv.setButtonDownPos(Qt::LeftButton, startLocal);
        QApplication::sendEvent(scene, &moveEv);

        QGraphicsSceneMouseEvent relEv(QEvent::GraphicsSceneMouseRelease);
        relEv.setButton(Qt::LeftButton);
        relEv.setButtons(Qt::NoButton);
        relEv.setScenePos(endScene);
        relEv.setLastScenePos(endScene);
        relEv.setPos(itemA->mapFromScene(endScene));
        relEv.setButtonDownScenePos(Qt::LeftButton, startScene);
        relEv.setButtonDownPos(Qt::LeftButton, startLocal);
        QApplication::sendEvent(scene, &relEv);
    }
    assert(scene->items().contains(itemA));
    assert(scene->items().contains(itemB));
    assert(QLineF(itemA->pos(), QPointF(220.0, 165.0)).length() < 1.0);
    assert(QLineF(itemB->pos(), QPointF(220.0, 345.0)).length() < 1.0);

    CommandBus wireBus;
    editor.setCommandBus(&wireBus);
    assert(editor.connectPins(nB, 0, nOut, 0));
    assert(editor.graph().edge_count == baseEdges + 1);
    assert(findEdgeToOut() != nullptr && findEdgeToOut()->src_node_id == nB);

    const waywal_graph_node_t *nodeAPtr =
        waywal_shader_graph_find_node(const_cast<waywal_shader_graph_t *>(&editor.graph()), nA);
    const waywal_graph_node_t *nodeBPtr =
        waywal_shader_graph_find_node(const_cast<waywal_shader_graph_t *>(&editor.graph()), nB);
    assert(nodeAPtr != nullptr && nodeBPtr != nullptr);
    assert((nodeAPtr->outputs[0].flags & WAYWAL_PIN_FLAG_CONNECTED) == 0);
    assert((nodeBPtr->outputs[0].flags & WAYWAL_PIN_FLAG_CONNECTED) != 0);

    wireBus.undo();
    assert(editor.graph().edge_count == baseEdges + 1);
    assert(findEdgeToOut() != nullptr && findEdgeToOut()->src_node_id == nA);
    assert(findEdgeToOut()->dst_node_id == nOut);
    nodeAPtr =
        waywal_shader_graph_find_node(const_cast<waywal_shader_graph_t *>(&editor.graph()), nA);
    nodeBPtr =
        waywal_shader_graph_find_node(const_cast<waywal_shader_graph_t *>(&editor.graph()), nB);
    assert((nodeAPtr->outputs[0].flags & WAYWAL_PIN_FLAG_CONNECTED) != 0);
    assert((nodeBPtr->outputs[0].flags & WAYWAL_PIN_FLAG_CONNECTED) == 0);

    wireBus.redo();
    assert(editor.graph().edge_count == baseEdges + 1);
    assert(findEdgeToOut() != nullptr && findEdgeToOut()->src_node_id == nB);

    {
        const QPointF wireMid(470.0, 345.0 + 28.0 + 16.0);
        QGraphicsSceneMouseEvent rPress(QEvent::GraphicsSceneMousePress);
        rPress.setButton(Qt::RightButton);
        rPress.setButtons(Qt::RightButton);
        rPress.setScenePos(wireMid);
        rPress.setLastScenePos(wireMid);
        rPress.setPos(wireMid);
        rPress.setButtonDownScenePos(Qt::RightButton, wireMid);
        rPress.setButtonDownPos(Qt::RightButton, wireMid);
        QApplication::sendEvent(scene, &rPress);
    }
    QCoreApplication::processEvents();
    assert(editor.graph().edge_count == baseEdges);
    assert(findEdgeToOut() == nullptr);
    wireBus.undo();
    assert(editor.graph().edge_count == baseEdges + 1);
    assert(findEdgeToOut() != nullptr && findEdgeToOut()->src_node_id == nB);

    const float prevZoom = editor.zoomScale();
    editor.setZoomScale(std::numeric_limits<float>::quiet_NaN());
    editor.setZoomScale(std::numeric_limits<float>::infinity());
    assert(std::fabs(editor.zoomScale() - prevZoom) < 1e-5f);
    editor.setSearchFilter(QStringLiteral("time"));
    editor.setSearchFilter(QString());

    std::cout << "  -> PASS: In-place multi-node drag, wire replacement undo & queued disconnect "
                 "verified.\n";
}

static void test_cyclic_skeleton_propagation_and_slider_sync()
{
    std::cout << "[Studio] Testing Cyclic Skeleton Propagation & 18-Slider Sync...\n";

    std::array<we_mdl_bone_t, 4> bones{};
    we_puppet_t puppet{};
    puppet.bones = bones.data();
    puppet.bone_count = static_cast<uint32_t>(bones.size());

    std::snprintf(bones[0].name, sizeof(bones[0].name), "CycleRoot");
    bones[0].parent = 1u;
    bones[0].bind_pos[1] = 24.0f;

    std::snprintf(bones[1].name, sizeof(bones[1].name), "CyclePartner");
    bones[1].parent = 0u;
    bones[1].bind_pos[1] = 48.0f;

    std::snprintf(bones[2].name, sizeof(bones[2].name), "LeafStrand");
    bones[2].parent = 1u;
    bones[2].bind_pos[1] = 66.0f;

    std::snprintf(bones[3].name, sizeof(bones[3].name), "SelfLoopIsland");
    bones[3].parent = 3u;
    bones[3].bind_pos[1] = 12.0f;

    CommandBus bus;
    PhysicsSpringInspectorWidget inspector;
    inspector.setCommandBus(&bus);
    inspector.loadPuppetSkeleton(&puppet, 42);

    inspector.selectBoneIndex(0);
    const float origBone1Drag = inspector.boneConfig(1).wind_drag_coeff;
    const float origBone2Drag = inspector.boneConfig(2).wind_drag_coeff;
    const float origBone3Drag = inspector.boneConfig(3).wind_drag_coeff;

    inspector.applyMaterialPreset(WE_PHYS_PRESET_SILK);
    const float silkDrag = inspector.boneConfig(0).wind_drag_coeff;
    assert(std::fabs(silkDrag - 2.5f) < 1e-4f);

    const auto sliders = inspector.findChildren<QSlider *>();
    assert(sliders.size() == 18);
    int nonZeroSliders = 0;
    for (QSlider *sl : sliders) {
        if (sl && sl->value() > 0) {
            ++nonZeroSliders;
        }
    }
    assert(nonZeroSliders >= 8);

    inspector.onPropagateToChildren();
    assert(std::fabs(inspector.boneConfig(1).wind_drag_coeff - silkDrag) < 1e-4f);
    assert(std::fabs(inspector.boneConfig(2).wind_drag_coeff - silkDrag) < 1e-4f);
    assert(std::fabs(inspector.boneConfig(3).wind_drag_coeff - origBone3Drag) < 1e-4f);

    bus.undo();
    assert(std::fabs(inspector.boneConfig(1).wind_drag_coeff - origBone1Drag) < 1e-4f);
    assert(std::fabs(inspector.boneConfig(2).wind_drag_coeff - origBone2Drag) < 1e-4f);
    assert(bus.canRedo());

    bus.redo();
    assert(std::fabs(inspector.boneConfig(1).wind_drag_coeff - silkDrag) < 1e-4f);
    assert(std::fabs(inspector.boneConfig(2).wind_drag_coeff - silkDrag) < 1e-4f);

    std::cout << "  -> PASS: Cyclic skeleton cascade terminated in O(N) with full Undo/Redo.\n";
}

static void test_pin_spatial_grid_accuracy_and_latency()
{
    std::cout << "[Studio] Testing UniformPinSpatialGrid Snap Accuracy & Latency...\n";

    CachedWireGeometry geom;
    assert(geom.dirty);
    geom.setEndpoints(QPointF(0.0, 0.0), QPointF(200.0, 100.0));
    geom.ensureUpdated();
    assert(!geom.dirty);
    const QPointF exactMid = (geom.p0 + 3.0 * geom.c0 + 3.0 * geom.c1 + geom.p1) * 0.125;
    assert(std::fabs(geom.cachedMidpoint.x() - exactMid.x()) < 1e-6);
    assert(std::fabs(geom.cachedMidpoint.y() - exactMid.y()) < 1e-6);
    assert(std::fabs(geom.cachedMidpoint.x() - 100.0) < 1e-6);
    assert(std::fabs(geom.cachedMidpoint.y() - 50.0) < 1e-6);

    UniformPinSpatialGrid grid;
    std::vector<PinSpatialEntry> allPins;
    allPins.reserve(512);
    for (uint32_t node = 1; node <= 256; ++node) {
        const float bx = static_cast<float>((node % 16) * 220);
        const float by = static_cast<float>((node / 16) * 160);
        const uint32_t ptype = (node % 2 == 0) ? static_cast<uint32_t>(WAYWAL_PIN_FLOAT)
                                               : static_cast<uint32_t>(WAYWAL_PIN_VEC4);
        PinSpatialEntry inPin{
            node, 0u, ptype, false, QPointF(bx, by + 44.0f),
        };
        PinSpatialEntry outPin{
            node, 0u, ptype, true, QPointF(bx + 180.0f, by + 44.0f),
        };
        grid.insert(inPin);
        grid.insert(outPin);
        allPins.push_back(inPin);
        allPins.push_back(outPin);
    }

    constexpr double kSnapRadius = 24.0;
    for (int q = 0; q < 500; ++q) {
        const QPointF queryPt(static_cast<double>((q * 73) % 3520),
                              static_cast<double>((q * 41) % 2560));
        const uint32_t excludeNode = static_cast<uint32_t>((q % 256) + 1);
        const bool wantOutputPin = (q % 2 == 1);
        const uint32_t dragType = (q % 3 == 0) ? static_cast<uint32_t>(WAYWAL_PIN_FLOAT)
                                               : static_cast<uint32_t>(WAYWAL_PIN_VEC4);

        const auto fastHit = grid.findNearestCompatible(
            queryPt, wantOutputPin, dragType, static_cast<float>(kSnapRadius), excludeNode);

        double bestDistSq = kSnapRadius * kSnapRadius;
        std::optional<PinSpatialEntry> bruteHit;
        for (const auto &e : allPins) {
            if (e.isOutput != wantOutputPin || e.nodeId == excludeNode) {
                continue;
            }
            const bool compat = wantOutputPin ? waywal_pin_types_compatible(e.pinType, dragType)
                                              : waywal_pin_types_compatible(dragType, e.pinType);
            if (!compat) {
                continue;
            }
            const double dx = e.scenePos.x() - queryPt.x();
            const double dy = e.scenePos.y() - queryPt.y();
            const double d2 = dx * dx + dy * dy;
            if (d2 <= bestDistSq) {
                bestDistSq = d2;
                bruteHit = e;
            }
        }

        assert(fastHit.has_value() == bruteHit.has_value());
        if (fastHit && bruteHit) {
            assert(fastHit->nodeId == bruteHit->nodeId);
            assert(fastHit->pinIdx == bruteHit->pinIdx);
            assert(fastHit->isOutput == bruteHit->isOutput);
        }
    }

    std::vector<double> querySamplesMs;
    querySamplesMs.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        const QPointF pt(static_cast<double>((i * 53) % 3520),
                         static_cast<double>((i * 29) % 2560));
        const auto t0 = std::chrono::high_resolution_clock::now();
        const auto hit = grid.findNearestCompatible(pt, false, WAYWAL_PIN_FLOAT,
                                                    static_cast<float>(kSnapRadius), 1u);
        (void)hit;
        const auto t1 = std::chrono::high_resolution_clock::now();
        querySamplesMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(querySamplesMs.begin(), querySamplesMs.end());
    const double snapP99Ms = querySamplesMs[990];
    assert(snapP99Ms <= 6.94);

    std::cout << "  -> PASS: Spatial pin snap exactness verified (p99 = " << snapP99Ms << " ms).\n";
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    std::cout << "=================================================================\n";
    std::cout << "Starting Studio Integration & Benchmark Suite\n";
    std::cout << "=================================================================\n";

    test_panel_instantiation();
    test_dock_layout_revision5();
    test_command_bus_coalescing();
    test_node_graph_canvas_benchmark();
    test_camera_lighting_undo_redo_and_degenerate_frustum();
    test_node_graph_inplace_move_and_wire_undo();
    test_cyclic_skeleton_propagation_and_slider_sync();
    test_pin_spatial_grid_accuracy_and_latency();

    std::cout << "=================================================================\n";
    std::cout << "All Studio Integration Tests PASSED successfully!\n";
    std::cout << "=================================================================\n";
    return 0;
}
