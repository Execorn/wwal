// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/app/dock_layout_manager.hpp"
#include "studio/app/main_window.hpp"
#include "studio/audio/audio_binding_wizard.hpp"
#include "studio/core/command_bus.hpp"
#include "studio/core/command_bus_authoring.hpp"
#include "studio/editor/glsl_scenescript_editor_widget.hpp"
#include "studio/inspector/property_inspector_widget.hpp"
#include "studio/particle/particle_designer_widget.hpp"
#include "studio/scene/scene_layer_tree_widget.hpp"
#include "studio/wizard/creator_packaging_wizard.hpp"
#include "waywal/shader_parser.h"
#include "waywal/we_asset_writer.h"

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QTreeView>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace waywal::studio;

namespace {

#if defined(__SANITIZE_ADDRESS__)
#define WWAL_TEST_WITH_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define WWAL_TEST_WITH_ASAN 1
#else
#define WWAL_TEST_WITH_ASAN 0
#endif
#else
#define WWAL_TEST_WITH_ASAN 0
#endif

double readPrivateRssAnonMiB()
{
    std::ifstream statusFile("/proc/self/status");
    std::string line;
    while (std::getline(statusFile, line)) {
        if (line.rfind("RssAnon:", 0) == 0) {
            long kb = 0;
            if (std::sscanf(line.c_str(), "RssAnon: %ld kB", &kb) == 1) {
                return static_cast<double>(kb) / 1024.0;
            }
        }
    }
    return -1.0;
}

void test_dock_layout_revision6_and_v5_migration()
{
    std::cout << "[Studio Authoring] Testing DockLayoutManager revision 6, 6 perspectives & v5->v6 "
                 "migration...\n";

    QMainWindow mw;
    DockLayoutManager mgr(&mw);

    const QJsonObject root = mgr.serializeLayout();
    assert(root.value(QStringLiteral("layout_revision")).toInt() == 6);

    const QJsonObject perspectives = root.value(QStringLiteral("perspectives")).toObject();
    const QStringList expectedPerspectives = {
        QStringLiteral("Default"),        QStringLiteral("Shader Coding"),
        QStringLiteral("Scene Composer"), QStringLiteral("Particle & Audio"),
        QStringLiteral("Theme Studio"),   QStringLiteral("Multi-Monitor"),
    };
    for (const QString &name : expectedPerspectives) {
        assert(perspectives.contains(name));
    }

    /* Verify lazy registration defers construction until realized */
    int factoryCalls = 0;
    mgr.registerLazyPanel(
        QStringLiteral("GpuBrushPainter"), QStringLiteral("GPU Paint Brush"),
        [&](QWidget *parent) -> QWidget * {
            ++factoryCalls;
            auto *w = new QWidget(parent);
            w->setObjectName(QStringLiteral("TestBrushWidget"));
            return w;
        },
        Qt::RightDockWidgetArea);

    assert(!mgr.isPanelRealized(QStringLiteral("GpuBrushPainter")));
    assert(mgr.panelWidget(QStringLiteral("GpuBrushPainter")) == nullptr);
    assert(factoryCalls == 0);

    QWidget *realized = mgr.realizeLazyPanel(QStringLiteral("GpuBrushPainter"));
    assert(realized != nullptr);
    assert(mgr.isPanelRealized(QStringLiteral("GpuBrushPainter")));
    assert(mgr.panelWidget(QStringLiteral("GpuBrushPainter")) == realized);
    assert(factoryCalls == 1);

    /* Calling realize a second time must be idempotent */
    assert(mgr.realizeLazyPanel(QStringLiteral("GpuBrushPainter")) == realized);
    assert(factoryCalls == 1);

    /* Simulate legacy v5 config with a user-defined custom perspective containing legacy aliases */
    QJsonObject v5CustomLeft;
    v5CustomLeft.insert(QStringLiteral("type"), QStringLiteral("DockNode"));
    v5CustomLeft.insert(QStringLiteral("content"), QStringLiteral("Grid"));

    QJsonObject v5CustomRight;
    v5CustomRight.insert(QStringLiteral("type"), QStringLiteral("TabGroup"));
    v5CustomRight.insert(QStringLiteral("active_tab"), QStringLiteral("CodeEditor"));
    QJsonArray customTabs;
    customTabs.append(QStringLiteral("Viewport"));
    customTabs.append(QStringLiteral("CodeEditor"));
    customTabs.append(QStringLiteral("Layers"));
    v5CustomRight.insert(QStringLiteral("tabs"), customTabs);

    QJsonObject v5CustomTree;
    v5CustomTree.insert(QStringLiteral("split_direction"), QStringLiteral("Horizontal"));
    v5CustomTree.insert(QStringLiteral("split_ratio"), 0.30);
    v5CustomTree.insert(QStringLiteral("left"), v5CustomLeft);
    v5CustomTree.insert(QStringLiteral("right"), v5CustomRight);

    QJsonObject v5Persps;
    v5Persps.insert(QStringLiteral("Default"), QJsonObject()); // Built-in gets refreshed
    v5Persps.insert(QStringLiteral("My Custom Lab"), v5CustomTree);

    QJsonObject v5Root;
    v5Root.insert(QStringLiteral("version"), 1);
    v5Root.insert(QStringLiteral("layout_revision"), 5);
    v5Root.insert(QStringLiteral("active_perspective"), QStringLiteral("Default"));
    v5Root.insert(QStringLiteral("perspectives"), v5Persps);

    assert(mgr.deserializeLayout(v5Root));
    const QJsonObject migrated = mgr.serializeLayout();
    assert(migrated.value(QStringLiteral("layout_revision")).toInt() == 6);

    const QJsonObject migratedPersps = migrated.value(QStringLiteral("perspectives")).toObject();
    for (const QString &name : expectedPerspectives) {
        assert(migratedPersps.contains(name));
    }
    assert(migratedPersps.contains(QStringLiteral("My Custom Lab")));
    const QJsonObject migratedCustom =
        migratedPersps.value(QStringLiteral("My Custom Lab")).toObject();
    assert(migratedCustom.value(QStringLiteral("left"))
               .toObject()
               .value(QStringLiteral("content"))
               .toString() == QStringLiteral("Wallpapers"));
    const QJsonObject migratedRight = migratedCustom.value(QStringLiteral("right")).toObject();
    assert(migratedRight.value(QStringLiteral("active_tab")).toString() ==
           QStringLiteral("GlslSceneScriptEditor"));
    const QJsonArray normTabs = migratedRight.value(QStringLiteral("tabs")).toArray();
    assert(normTabs.at(0).toString() == QStringLiteral("GpuPreviewViewport"));
    assert(normTabs.at(1).toString() == QStringLiteral("GlslSceneScriptEditor"));
    assert(normTabs.at(2).toString() == QStringLiteral("SceneLayerTree"));

    /* Verify Default perspective contains SceneLayerTree in left.tabs */
    const QJsonArray defLeftTabs = migratedPersps.value(QStringLiteral("Default"))
                                       .toObject()
                                       .value(QStringLiteral("left"))
                                       .toObject()
                                       .value(QStringLiteral("tabs"))
                                       .toArray();
    bool hasSceneTree = false;
    for (const auto &v : defLeftTabs) {
        if (v.toString() == QStringLiteral("SceneLayerTree")) {
            hasSceneTree = true;
        }
    }
    assert(hasSceneTree);

    std::cout << "  -> PASS: DockLayoutManager v6, lazy panels, and v5->v6 migration verified.\n";
}

void test_mainwindow_lazy_panels_and_rss_budget()
{
    std::cout << "[Studio Authoring] Testing MainWindow lazy docks, audio badge wiring & RssAnon "
                 "< 80 MB...\n";

    StudioOptions opts;
    opts.headlessCheck = true;
    MainWindow win(opts);

    DockLayoutManager *mgr = win.dockLayoutManager();
    assert(mgr != nullptr);

    /* All 5 authoring docks stay deferred on startup so private RSS stays minimal */
    assert(!mgr->isPanelRealized(QStringLiteral("SceneLayerTree")));
    assert(!mgr->isPanelRealized(QStringLiteral("GpuBrushPainter")));
    assert(!mgr->isPanelRealized(QStringLiteral("ParticleDesigner")));
    assert(!mgr->isPanelRealized(QStringLiteral("AudioBindingWizard")));
    assert(!mgr->isPanelRealized(QStringLiteral("GlslSceneScriptEditor")));

    const double rssAnonMb = readPrivateRssAnonMiB();
#if !WWAL_TEST_WITH_ASAN
    assert(rssAnonMb > 0.0 && rssAnonMb < 80.0);
#else
    assert(rssAnonMb > 0.0);
#endif

    /* Trigger PropertyInspectorWidget audio badge -> lazily realizes AudioBindingWizard */
    PropertyInspectorWidget *inspector = win.propertyInspector();
    assert(inspector != nullptr);
    inspector->setTargetObjectId(12, 0);
    emit inspector->audioBindingRequested(QStringLiteral("u_glow"), 0.45f);
    QApplication::processEvents();

    assert(mgr->isPanelRealized(QStringLiteral("AudioBindingWizard")));
    assert(win.audioBindingWizard() != nullptr);

    std::cout << "  -> PASS: MainWindow lazy docks & startup RssAnon (" << rssAnonMb
              << " MiB < 80 MiB) verified.\n";
}

void test_inspector_combo_and_code_editor_sync()
{
    std::cout << "[Studio Authoring] Testing // [COMBO] inspector controls & cursor-preserving "
                 "editor patching...\n";

    PropertyInspectorWidget inspector;
    inspector.setTargetObjectId(3, 1);

    waywal_shader_meta_t meta{};
    meta.uniform_count = 1;
    std::snprintf(meta.uniforms[0].name, sizeof(meta.uniforms[0].name), "u_speed");
    std::snprintf(meta.uniforms[0].label, sizeof(meta.uniforms[0].label), "Speed");
    meta.uniforms[0].type = WAYWAL_UNIFORM_TYPE_FLOAT;
    meta.uniforms[0].default_val[0] = 1.0f;
    meta.uniforms[0].min_val[0] = 0.0f;
    meta.uniforms[0].max_val[0] = 5.0f;
    meta.uniforms[0].step = 0.1f;

    const QString comboJson = QStringLiteral(
        R"([{"name":"MODE","label":"Render Mode","default":0,"options":[{"value":0,"label":"Off"},{"value":2,"label":"Neon"}]}])");
    inspector.setShaderMetadata(meta, comboJson);

    PropertyRowWidget *comboRow = inspector.propertyRow(QStringLiteral("MODE"));
    assert(comboRow != nullptr);
    assert(comboRow->property().isShaderCombo);

    bool comboSignalSeen = false;
    int32_t emittedComboVal = -1;
    QObject::connect(
        &inspector, &PropertyInspectorWidget::shaderComboChanged,
        [&](const QString &, int32_t objId, uint32_t passIdx, const QString &name, int32_t val) {
            if (objId == 3 && passIdx == 1 && name == QStringLiteral("MODE")) {
                comboSignalSeen = true;
                emittedComboVal = val;
            }
        });

    auto *comboBox = comboRow->findChild<QComboBox *>();
    assert(comboBox != nullptr && comboBox->count() == 2);
    comboBox->setCurrentIndex(1);
    QApplication::processEvents();
    assert(comboSignalSeen && emittedComboVal == 2);

    /* Verify cursor preservation during bidirectional source patching */
    GlslSceneScriptEditorWidget editor;
    const QString glslSrc = QStringLiteral(
        "uniform float u_speed; // {\"material\":\"Speed\",\"default\":1.0,\"range\":[0.0,5.0]}\n"
        "void main() {\n"
        "    gl_FragColor = vec4(u_speed);\n"
        "}\n");
    editor.openDocument(EditorLanguageMode::GlslEs310Fragment, -1, 0, QStringLiteral("test.frag"),
                        glslSrc);
    auto *plainEdit = editor.findChild<QPlainTextEdit *>();
    assert(plainEdit != nullptr);
    QTextCursor cursor = plainEdit->textCursor();
    const int savedPos = glslSrc.indexOf(QStringLiteral("gl_FragColor"));
    cursor.setPosition(savedPos);
    plainEdit->setTextCursor(cursor);

    const bool patched = editor.patchUniformOrComboDefaultInSource(QStringLiteral("u_speed"),
                                                                   QStringLiteral("2.50"));
    assert(patched);
    assert(editor.sourceText().contains(QStringLiteral("2.50")));
    assert(plainEdit->textCursor().position() >= savedPos);

    std::cout << "  -> PASS: Inspector // [COMBO] + cursor-preserving code patch verified.\n";
}

void test_packaging_wizard_pkg_and_tex_handoff()
{
    std::cout << "[Studio Authoring] Testing CreatorPackagingWizard .pkg and .tex -> .wwpkg "
                 "handoff...\n";

    QTemporaryDir tmpDir;
    assert(tmpDir.isValid());

    /* Build a minimal valid PKGV0022 scene package and TEXV0005 texture */
    const QByteArray sceneJson =
        R"({"general":{"orthogonalprojection":{"width":1920,"height":1080}},"objects":[{"id":1,"name":"bg"}]})";
    const QByteArray fragSrc = "void main() { gl_FragColor = vec4(0.1, 0.2, 0.3, 1.0); }\n";
    we_pkg_write_entry_t entries[2] = {
        {.path = "scene.json",
         .data = reinterpret_cast<const uint8_t *>(sceneJson.constData()),
         .size = static_cast<size_t>(sceneJson.size())},
        {.path = "shaders/test.frag",
         .data = reinterpret_cast<const uint8_t *>(fragSrc.constData()),
         .size = static_cast<size_t>(fragSrc.size())},
    };

    const QString pkgPath = tmpDir.filePath(QStringLiteral("sample_scene.pkg"));
    we_asset_writer_err_t werr = WE_WRITER_OK;
    assert(we_pkg_write_file(pkgPath.toUtf8().constData(), entries, 2u, &werr));

    wwal::studio::CreatorPackagingWizard pkgWizard;
    pkgWizard.setSourcePath(pkgPath);
    QApplication::processEvents();
    assert(pkgWizard.context()->sourcePath == pkgPath);
    assert(pkgWizard.context()->projectType == WWPKG_TYPE_SCENE_2D);
    assert(pkgWizard.assetBundlingPage()->validatePage());
    assert(!pkgWizard.context()->stagingDir.isEmpty());
    assert(QFile::exists(pkgWizard.context()->stagingDir + QStringLiteral("/scene.json")));

    /* Single .tex texture handoff */
    const uint8_t px[16] = {255, 0, 128, 255, 255, 0, 128, 255, 255, 0, 128, 255, 255, 0, 128, 255};
    we_tex_encode_mip_t mip = {.width = 2u, .height = 2u, .pixels = px, .byte_size = 16u};
    we_tex_encode_image_t img = {.mip_count = 1u, .mips = &mip};
    we_tex_encode_spec_t texSpec{};
    texSpec.format = WE_TEX_FMT_RGBA8888;
    texSpec.flags = WE_TEX_FLAG_NONE;
    texSpec.texture_width = 2u;
    texSpec.texture_height = 2u;
    texSpec.image_width = 2u;
    texSpec.image_height = 2u;
    texSpec.enable_lz4 = true;
    texSpec.image_count = 1u;
    texSpec.images = &img;

    const QString texPath = tmpDir.filePath(QStringLiteral("albedo.tex"));
    assert(we_tex_write_v5_file(texPath.toUtf8().constData(), &texSpec, &werr));

    wwal::studio::CreatorPackagingWizard texWizard;
    texWizard.setSourcePath(texPath);
    QApplication::processEvents();
    assert(texWizard.context()->projectType == WWPKG_TYPE_SCENE_2D);
    assert(texWizard.assetBundlingPage()->validatePage());
    assert(QFile::exists(texWizard.context()->stagingDir + QStringLiteral("/scene.json")));

    std::cout << "  -> PASS: .pkg and .tex -> .wwpkg wizard staging verified.\n";
}

void test_all_six_perspectives_switching()
{
    std::cout << "[Studio Authoring] Testing switching across all 6 perspectives...\n";

    StudioOptions opts;
    opts.headlessCheck = true;
    MainWindow win(opts);
    win.show();
    QApplication::processEvents();
    DockLayoutManager *mgr = win.dockLayoutManager();
    assert(mgr != nullptr);

    const QStringList perspectives = {
        QStringLiteral("Default"),        QStringLiteral("Shader Coding"),
        QStringLiteral("Scene Composer"), QStringLiteral("Particle & Audio"),
        QStringLiteral("Theme Studio"),   QStringLiteral("Multi-Monitor"),
    };
    for (const QString &p : perspectives) {
        assert(mgr->switchPerspective(p));
        QApplication::processEvents();
        assert(mgr->activePerspective() == p);
        if (p == QStringLiteral("Scene Composer")) {
            mgr->setPanelVisible(QStringLiteral("SceneLayerTree"), true);
            QApplication::processEvents();
            assert(mgr->isPanelRealized(QStringLiteral("SceneLayerTree")));
        } else if (p == QStringLiteral("Particle & Audio")) {
            mgr->setPanelVisible(QStringLiteral("ParticleDesigner"), true);
            mgr->setPanelVisible(QStringLiteral("AudioBindingWizard"), true);
            QApplication::processEvents();
            assert(mgr->isPanelRealized(QStringLiteral("ParticleDesigner")));
            assert(mgr->isPanelRealized(QStringLiteral("AudioBindingWizard")));
        }
    }

    assert(mgr->isPanelRealized(QStringLiteral("SceneLayerTree")));
    assert(mgr->isPanelRealized(QStringLiteral("ParticleDesigner")));
    assert(mgr->isPanelRealized(QStringLiteral("AudioBindingWizard")));

    std::cout << "  -> PASS: All 6 perspectives switched and lazy panels realized cleanly.\n";
}

void test_scene_layer_tree_cascade_undo_and_z_reorder()
{
    std::cout << "[Studio Authoring] Testing SceneLayerTree cascade-delete undo & Z-stack "
                 "reorder math...\n";

    /* 1. Verify FlatZOrderStack post-erase drop target index */
    SceneLayerTreeModel zModel;
    SceneLayerNode n10;
    n10.id = 10;
    n10.name = QStringLiteral("Bottom");
    n10.zOrderIndex = 0;
    SceneLayerNode n20;
    n20.id = 20;
    n20.name = QStringLiteral("Mid");
    n20.zOrderIndex = 1;
    SceneLayerNode n30;
    n30.id = 30;
    n30.name = QStringLiteral("Top");
    n30.zOrderIndex = 2;
    zModel.setNodes({n10, n20, n30});
    zModel.setViewMode(SceneLayerViewMode::FlatZOrderStack);

    std::vector<std::pair<int32_t, uint32_t>> zMoves;
    QObject::connect(&zModel, &SceneLayerTreeModel::reorderZRequested,
                     [&](int32_t objId, uint32_t newZ) { zMoves.emplace_back(objId, newZ); });

    /* Move Top (id=30, flat row 0) to row 2 (between Mid id=20 and Bottom id=10): z becomes 1 */
    {
        const QModelIndex idxTop = zModel.index(0, 0);
        assert(idxTop.isValid() &&
               idxTop.data(Qt::DisplayRole).toString() == QStringLiteral("Top"));
        std::unique_ptr<QMimeData> mime(zModel.mimeData({idxTop}));
        assert(mime != nullptr);
        assert(zModel.dropMimeData(mime.get(), Qt::MoveAction, 2, 0, QModelIndex()));
        assert(!zMoves.empty() && zMoves.back().first == 30 && zMoves.back().second == 1u);
    }

    /* Move Bottom (id=10, flat row 2) to row 0 (above Top): z becomes 2 */
    {
        const QModelIndex idxBot = zModel.index(2, 0);
        std::unique_ptr<QMimeData> mime(zModel.mimeData({idxBot}));
        assert(mime != nullptr);
        assert(zModel.dropMimeData(mime.get(), Qt::MoveAction, 0, 0, QModelIndex()));
        assert(zMoves.size() == 2 && zMoves.back().first == 10 && zMoves.back().second == 2u);
    }

    /* 2. Verify cascade delete + undo restores entire parent-before-child subtree */
    CommandBus bus;
    SceneLayerTreeWidget tree;
    tree.setCommandBus(&bus);
    tree.show();
    const QString sceneJson = QStringLiteral(R"({
        "objects": [
            {"id": 1, "name": "RootLayer", "parentId": -1, "origin": "0 0 0"},
            {"id": 2, "name": "ChildLayer", "parentId": 1, "origin": "10 10 0"},
            {"id": 3, "name": "GrandChild", "parentId": 2, "origin": "20 20 0"}
        ]
    })");
    tree.loadFromSceneJson(sceneJson);
    tree.selectObject(2);
    assert(tree.selectedObjectId() == 2);

    QKeyEvent shiftDel(QEvent::KeyPress, Qt::Key_Delete, Qt::ShiftModifier);
    QApplication::sendEvent(&tree, &shiftDel);
    QApplication::processEvents();
    auto *treeView = tree.findChild<QTreeView *>();
    assert(treeView != nullptr && treeView->model() != nullptr);
    /* RootLayer (id=1) remains without ChildLayer/GrandChild */
    assert(treeView->model()->rowCount() == 1);
    const QModelIndex rootIdx = treeView->model()->index(0, 0);
    assert(treeView->model()->rowCount(rootIdx) == 0);

    assert(bus.canUndo());
    bus.undo();
    QApplication::processEvents();
    const QModelIndex rootIdxAfter = treeView->model()->index(0, 0);
    assert(rootIdxAfter.isValid());
    assert(treeView->model()->rowCount(rootIdxAfter) == 1);
    const QModelIndex childIdx = treeView->model()->index(0, 0, rootIdxAfter);
    assert(childIdx.isValid());
    assert(treeView->model()->rowCount(childIdx) == 1);
    assert(tree.selectedObjectId() == 2);

    std::cout << "  -> PASS: SceneLayerTree cascade-delete undo & descending-Z reorder verified.\n";
}

void test_particle_curve_ramp_roundtrip_and_cross_object_undo()
{
    std::cout << "[Studio Authoring] Testing Particle curve/ramp round-trip undo & cross-object "
                 "switch guard...\n";

    CommandBus bus;
    ParticleDesignerWidget widget;
    widget.setCommandBus(&bus);
    widget.show();
    QApplication::processEvents();

    widget.loadParticleSystem(101, nullptr);
    const QString baseline101Json = widget.exportSystemJson();
    const QJsonObject doc101 = QJsonDocument::fromJson(baseline101Json.toUtf8()).object();
    assert(doc101.contains(QStringLiteral("alphaknots")));
    assert(doc101.contains(QStringLiteral("sizeknots")));
    assert(doc101.contains(QStringLiteral("colorstops")));

    /* Edit alpha curve and color ramp -> triggers EditParticleSystemCommand */
    const auto curves = widget.findChildren<ScalarCurveEditorWidget *>();
    assert(!curves.isEmpty());
    std::vector<we_particle_curve_knot_t> customAlpha = {
        {0.0f, 0.25f, 0.0f, 0.0f, 0u, 0u},
        {0.5f, 0.90f, 0.0f, 0.0f, 0u, 0u},
        {1.0f, 0.10f, 0.0f, 0.0f, 0u, 0u},
    };
    emit curves[0]->curveEdited(customAlpha);
    QApplication::processEvents();

    auto *ramp = widget.findChild<OklabColorRampWidget *>();
    assert(ramp != nullptr);
    std::vector<we_particle_color_stop_t> customStops = {
        {0.0f, {1.0f, 0.2f, 0.1f}},
        {0.5f, {0.1f, 0.8f, 0.3f}},
        {1.0f, {0.2f, 0.4f, 1.0f}},
    };
    ramp->setStops(customStops);
    emit ramp->rampEdited(customStops);
    QApplication::processEvents();

    const QJsonObject editedDoc =
        QJsonDocument::fromJson(widget.exportSystemJson().toUtf8()).object();
    assert(editedDoc.value(QStringLiteral("alphaknots")).toArray().size() == 3);
    assert(editedDoc.value(QStringLiteral("colorstops")).toArray().size() == 3);

    /* Switch to another object (202) and trigger Undo: must NOT clobber 202 */
    we_particle_sys_t sys202{};
    sys202.max_count = 777u;
    widget.loadParticleSystem(202, &sys202);
    const QString beforeUndo202 = widget.exportSystemJson();

    assert(bus.canUndo());
    bus.undo();
    QApplication::processEvents();
    const QString afterUndo202 = widget.exportSystemJson();
    assert(afterUndo202 == beforeUndo202);
    assert(QJsonDocument::fromJson(afterUndo202.toUtf8())
               .object()
               .value(QStringLiteral("maxcount"))
               .toInt() == 777);

    std::cout
        << "  -> PASS: Particle curve/ramp JSON round-trip & cross-object undo guard verified.\n";
}

void test_audio_clear_all_undo_and_preselect_preservation()
{
    std::cout << "[Studio Authoring] Testing AudioBindingWizard Clear All undo & preselect "
                 "preservation...\n";

    CommandBus bus;
    AudioBindingWizard wizard;
    wizard.setCommandBus(&bus);
    wizard.preselectTargetUniform(777, QStringLiteral("u_glow"), 1u, 0.35f);
    wizard.applyMusicalBandPreset(AudioBindingWizard::MusicalBandPreset::Bass80To250Hz);
    wizard.show();
    QApplication::processEvents();

    /* Unknown objectId 777 & preselected Bark preset must survive setSceneLayers + UI init */
    SceneLayerNode node10;
    node10.id = 10;
    node10.name = QStringLiteral("Background");
    wizard.setSceneLayers({node10});
    QApplication::processEvents();

    /* Silence must NOT spin the 60 Hz oscilloscope idle timer */
    auto *idleTimer = wizard.findChild<QTimer *>();
    assert(idleTimer == nullptr || !idleTimer->isActive());

    QPushButton *createBtn = nullptr;
    QPushButton *clearBtn = nullptr;
    for (QPushButton *b : wizard.findChildren<QPushButton *>()) {
        if (b->text() == QStringLiteral("Create Binding")) {
            createBtn = b;
        } else if (b->text() == QStringLiteral("Clear All")) {
            clearBtn = b;
        }
    }
    assert(createBtn != nullptr && clearBtn != nullptr);

    createBtn->click();
    QApplication::processEvents();
    assert(wizard.activeBindings().size() == 1);
    assert(wizard.activeBindings().front().target_object_id == 777);
    assert(wizard.activeBindings().front().band_start == 2u &&
           wizard.activeBindings().front().band_end == 6u);

    createBtn->click();
    QApplication::processEvents();
    assert(wizard.activeBindings().size() == 2);

    clearBtn->click();
    QApplication::processEvents();
    assert(wizard.activeBindings().empty());

    assert(bus.canUndo());
    bus.undo();
    QApplication::processEvents();
    assert(wizard.activeBindings().size() == 2);

    bus.redo();
    QApplication::processEvents();
    assert(wizard.activeBindings().empty());

    std::cout << "  -> PASS: Audio Clear All undo, unknown target preservation, and idle timer "
                 "verified.\n";
}

void test_editor_uniform_trailing_comment_and_inspector_preservation()
{
    std::cout << "[Studio Authoring] Testing GLSL [UNIFORM] trailing comment & inspector "
                 "preservation...\n";

    GlslSceneScriptEditorWidget editor;
    const QString src = QStringLiteral("// [UNIFORM]\n"
                                       "// type: float\n"
                                       "// name: u_gain\n"
                                       "// default: 0.75 // keep trailing comment\n"
                                       "uniform float u_gain;\n"
                                       "void main() {\n"
                                       "    gl_FragColor = vec4(u_gain);\n"
                                       "}\n");
    editor.openDocument(EditorLanguageMode::GlslEs310Fragment, 4, 0, QStringLiteral("u.frag"), src);
    assert(editor.patchUniformOrComboDefaultInSource(QStringLiteral("u_gain"),
                                                     QStringLiteral("1.25")));
    const QString patched = editor.sourceText();
    assert(patched.contains(QStringLiteral("// default: 1.25 // keep trailing comment")));

    /* PropertyInspectorWidget schema-match keeps existing audio-bound badges intact */
    PropertyInspectorWidget inspector;
    waywal_shader_meta_t meta{};
    std::snprintf(meta.title, sizeof(meta.title), "Plasma");
    meta.uniform_count = 1;
    std::snprintf(meta.uniforms[0].name, sizeof(meta.uniforms[0].name), "u_gain");
    std::snprintf(meta.uniforms[0].label, sizeof(meta.uniforms[0].label), "Gain");
    meta.uniforms[0].type = WAYWAL_UNIFORM_TYPE_FLOAT;
    meta.uniforms[0].num_components = 1;
    meta.uniforms[0].default_val[0] = 0.75f;
    meta.uniforms[0].has_range = true;
    meta.uniforms[0].min_val[0] = 0.0f;
    meta.uniforms[0].max_val[0] = 2.0f;
    meta.uniforms[0].step = 0.05f;

    inspector.setShaderMetadata(meta, QString());
    PropertyRowWidget *row = inspector.propertyRow(QStringLiteral("u_gain"));
    assert(row != nullptr);
    row->setAudioBound(true);
    assert(row->isAudioBound());

    /* Re-applying same schema must NOT destroy/recreate row or drop audio badge */
    inspector.setShaderMetadata(meta, QString());
    PropertyRowWidget *sameRow = inspector.propertyRow(QStringLiteral("u_gain"));
    assert(sameRow == row && sameRow->isAudioBound());

    std::cout << "  -> PASS: GLSL [UNIFORM] trailing comment & inspector state preservation "
                 "verified.\n";
}

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qunsetenv("DISPLAY");
    qunsetenv("WAYLAND_DISPLAY");
    QApplication app(argc, argv);

    std::cout << "=================================================================\n";
    std::cout << "Starting Studio Authoring Integration Suite\n";
    std::cout << "=================================================================\n";

    test_dock_layout_revision6_and_v5_migration();
    test_mainwindow_lazy_panels_and_rss_budget();
    test_inspector_combo_and_code_editor_sync();
    test_packaging_wizard_pkg_and_tex_handoff();
    test_all_six_perspectives_switching();
    test_scene_layer_tree_cascade_undo_and_z_reorder();
    test_particle_curve_ramp_roundtrip_and_cross_object_undo();
    test_audio_clear_all_undo_and_preselect_preservation();
    test_editor_uniform_trailing_comment_and_inspector_preservation();

    std::cout << "=================================================================\n";
    std::cout << "All Studio Authoring Integration Tests PASSED!\n";
    std::cout << "=================================================================\n";
    return 0;
}
