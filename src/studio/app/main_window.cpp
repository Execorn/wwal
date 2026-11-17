// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/app/main_window.hpp"
#include "studio/app/dock_layout_manager.hpp"
#include "studio/app/studio_session_context.hpp"
#include "studio/app/studio_tokens.hpp"
#include "waywal/ipc_proto_authoring.h"
#include "waywal/we_mesh_cdt.h"
#include "waywal/we_mesh_rig.h"
#include "waywal/we_puppet.h"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenuBar>
#include <QMenu>
#include <QSlider>
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
#include <QImage>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QStandardPaths>

#include <algorithm>
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
        btnRefresh->setAccessibleName(QStringLiteral("Refresh displays"));
        btnRefresh->setToolTip(QStringLiteral("Refresh connected Wayland outputs"));
        btnRefresh->setMinimumHeight(wwal::studio::tokens::kTouchTargetMinPx);
        connect(btnRefresh, &QPushButton::clicked, this, [this]() {
            if (m_canvas) {
                m_canvas->refreshFromWayland();
            }
        });
        topBtnRow->addWidget(btnRefresh, 1);

        auto *btnSpan = new QPushButton(QStringLiteral("Span"), controlsBox);
        btnSpan->setAccessibleName(QStringLiteral("Span displays"));
        btnSpan->setCheckable(true);
        btnSpan->setMinimumHeight(wwal::studio::tokens::kTouchTargetMinPx);
        btnSpan->setToolTip(QStringLiteral("Span wallpaper across all displays"));
        topBtnRow->addWidget(btnSpan, 1);
        ctrlLayout->addLayout(topBtnRow);

        auto *scaleRow = new QHBoxLayout();
        scaleRow->setContentsMargins(0, 0, 0, 0);
        scaleRow->setSpacing(8);
        auto *scaleLbl = new QLabel(QStringLiteral("Scale"), controlsBox);
        scaleLbl->setObjectName(QStringLiteral("StudioMutedLabel"));
        wwal::studio::tokens::applySecondaryTextRole(scaleLbl, QColor(0xcb, 0xd5, 0xe1));
        scaleRow->addWidget(scaleLbl);

        auto *comboScaling = new QComboBox(controlsBox);
        comboScaling->setAccessibleName(QStringLiteral("Wallpaper scaling mode"));
        comboScaling->setMinimumHeight(wwal::studio::tokens::kTouchTargetMinPx);
        comboScaling->addItem(QStringLiteral("Fill"), static_cast<int>(WAYWAL_SCALING_FILL));
        comboScaling->addItem(QStringLiteral("Fit"), static_cast<int>(WAYWAL_SCALING_FIT));
        comboScaling->addItem(QStringLiteral("Stretch"), static_cast<int>(WAYWAL_SCALING_STRETCH));
        comboScaling->addItem(QStringLiteral("Center"), static_cast<int>(WAYWAL_SCALING_CENTER));
        comboScaling->addItem(QStringLiteral("Tile"), static_cast<int>(WAYWAL_SCALING_TILE));
        comboScaling->addItem(QStringLiteral("Span"), static_cast<int>(WAYWAL_SCALING_SPAN));

        connect(btnSpan, &QPushButton::toggled, this, [this, comboScaling](bool checked) {
            if (comboScaling) {
                const QSignalBlocker blocker(comboScaling);
                const int targetIdx = comboScaling->findData(static_cast<int>(
                    checked ? WAYWAL_SCALING_SPAN : WAYWAL_SCALING_FILL));
                if (targetIdx >= 0) {
                    comboScaling->setCurrentIndex(targetIdx);
                }
            }
            if (m_canvas) {
                m_canvas->setDesktopSpanMode(checked);
            }
        });
        connect(comboScaling, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, comboScaling, btnSpan](int idx) {
                    const QVariant data = comboScaling->itemData(idx);
                    if (!data.isValid()) {
                        return;
                    }
                    const int mode = data.toInt();
                    if (btnSpan) {
                        const QSignalBlocker blocker(btnSpan);
                        btnSpan->setChecked(mode == WAYWAL_SCALING_SPAN);
                    }
                    if (m_canvas) {
                        m_canvas->setDesktopSpanMode(mode == WAYWAL_SCALING_SPAN);
                        m_canvas->setScalingMode(mode);
                    }
                });
        if (m_canvas) {
            connect(m_canvas, &MonitorTopologyCanvas::desktopSpanModeToggled, this,
                    [btnSpan, comboScaling](bool spanEnabled) {
                        if (btnSpan) {
                            const QSignalBlocker b1(btnSpan);
                            btnSpan->setChecked(spanEnabled);
                        }
                        if (comboScaling) {
                            const QSignalBlocker b2(comboScaling);
                            const int idx = comboScaling->findData(static_cast<int>(
                                spanEnabled ? WAYWAL_SCALING_SPAN : WAYWAL_SCALING_FILL));
                            if (idx >= 0) {
                                comboScaling->setCurrentIndex(idx);
                            }
                        }
                    });
            connect(m_canvas, &MonitorTopologyCanvas::scalingModeChanged, this,
                    [btnSpan, comboScaling](const QStringList &, int mode) {
                        if (comboScaling) {
                            const QSignalBlocker b1(comboScaling);
                            const int idx = comboScaling->findData(mode);
                            if (idx >= 0) {
                                comboScaling->setCurrentIndex(idx);
                            }
                        }
                        if (btnSpan) {
                            const QSignalBlocker b2(btnSpan);
                            btnSpan->setChecked(mode == WAYWAL_SCALING_SPAN);
                        }
                    });
        }

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
#include <QMatrix4x4>
#include <QSet>
#include <algorithm>
#include <array>
#include <limits>
#include <unordered_set>

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

void resolveBoneWorldMatricesInto(const std::vector<we_mdl_bone_t> &bones,
                                  const std::vector<std::array<float, 16>> &localMats,
                                  std::vector<std::array<float, 16>> &worldMats,
                                  std::vector<uint8_t> &visitState, std::vector<size_t> &stack)
{
    const size_t boneCount = std::min(bones.size(), localMats.size());
    worldMats.resize(boneCount);
    auto mulColMajor = [](const float *a, const float *b, float *out) {
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                out[col * 4 + row] =
                    a[0 * 4 + row] * b[col * 4 + 0] + a[1 * 4 + row] * b[col * 4 + 1] +
                    a[2 * 4 + row] * b[col * 4 + 2] + a[3 * 4 + row] * b[col * 4 + 3];
            }
        }
    };

    visitState.assign(boneCount, 0u);
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
}

std::vector<std::array<float, 16>>
resolveBoneWorldMatrices(const std::vector<we_mdl_bone_t> &bones,
                         const std::vector<std::array<float, 16>> &localMats)
{
    std::vector<std::array<float, 16>> worldMats;
    std::vector<uint8_t> visitState;
    std::vector<size_t> stack;
    resolveBoneWorldMatricesInto(bones, localMats, worldMats, visitState, stack);
    return worldMats;
}

[[nodiscard]] we_scene_node_kind_t parseSceneNodeKindJson(const QJsonValue &val)
{
    if (val.isDouble()) {
        const int raw = val.toInt(0);
        if (raw >= 0 && raw <= static_cast<int>(WE_NODE_CAMERA3D)) {
            return static_cast<we_scene_node_kind_t>(raw);
        }
    }
    const QString s = val.toString().trimmed().toLower();
    if (s.contains(QStringLiteral("particle"))) {
        return WE_NODE_PARTICLE;
    }
    if (s.contains(QStringLiteral("sound")) || s.contains(QStringLiteral("audio"))) {
        return WE_NODE_SOUND;
    }
    if (s.contains(QStringLiteral("text"))) {
        return WE_NODE_TEXT;
    }
    if (s.contains(QStringLiteral("light"))) {
        return WE_NODE_LIGHT;
    }
    if (s.contains(QStringLiteral("camera"))) {
        return WE_NODE_CAMERA3D;
    }
    return WE_NODE_IMAGE;
}

[[nodiscard]] std::vector<SceneLayerNode> parseSceneLayerNodesJson(const QString &sceneTreeJson)
{
    std::vector<SceneLayerNode> parsedNodes;
    const QJsonDocument doc = QJsonDocument::fromJson(sceneTreeJson.toUtf8());
    QJsonArray arr;
    if (doc.isArray()) {
        arr = doc.array();
    } else if (doc.isObject()) {
        const QJsonObject root = doc.object();
        if (root.contains(QStringLiteral("objects"))) {
            arr = root.value(QStringLiteral("objects")).toArray();
        } else if (root.contains(QStringLiteral("layers"))) {
            arr = root.value(QStringLiteral("layers")).toArray();
        } else if (root.contains(QStringLiteral("nodes"))) {
            arr = root.value(QStringLiteral("nodes")).toArray();
        }
    }
    std::unordered_set<int32_t> explicitIds;
    for (const QJsonValue &entry : arr) {
        if (!entry.isObject()) {
            continue;
        }
        const int rawId = entry.toObject().value(QStringLiteral("id")).toInt(-1);
        if (rawId >= 0) {
            explicitIds.insert(rawId);
        }
    }
    std::unordered_set<int32_t> assignedIds;
    int32_t nextFallbackId = 0;
    parsedNodes.reserve(static_cast<size_t>(arr.size()));
    uint32_t fallbackZ = 0u;
    for (const QJsonValue &entry : arr) {
        if (!entry.isObject()) {
            continue;
        }
        const QJsonObject obj = entry.toObject();
        SceneLayerNode node;
        const int rawId = obj.value(QStringLiteral("id")).toInt(-1);
        if (rawId >= 0 && !assignedIds.contains(rawId)) {
            node.id = rawId;
        } else {
            while (explicitIds.contains(nextFallbackId) || assignedIds.contains(nextFallbackId)) {
                ++nextFallbackId;
            }
            node.id = nextFallbackId++;
        }
        assignedIds.insert(node.id);
        node.parentId = obj.contains(QStringLiteral("parentId"))
                            ? obj.value(QStringLiteral("parentId")).toInt(-1)
                            : obj.value(QStringLiteral("parent_id")).toInt(-1);
        const int zRaw =
            obj.contains(QStringLiteral("zOrderIndex"))
                ? obj.value(QStringLiteral("zOrderIndex")).toInt(static_cast<int>(fallbackZ))
                : obj.value(QStringLiteral("z_order")).toInt(static_cast<int>(fallbackZ));
        node.zOrderIndex = static_cast<uint32_t>(std::max(0, zRaw));
        node.kind = obj.contains(QStringLiteral("kind"))
                        ? parseSceneNodeKindJson(obj.value(QStringLiteral("kind")))
                        : parseSceneNodeKindJson(obj.value(QStringLiteral("type")));
        node.visible = obj.value(QStringLiteral("visible")).toBool(true);
        node.locked = obj.value(QStringLiteral("locked")).toBool(false);
        node.name = obj.value(QStringLiteral("name")).toString();
        parsedNodes.push_back(std::move(node));
        ++fallbackZ;
    }
    return parsedNodes;
}

[[nodiscard]] uint32_t resolveActivePropertyComponentIndex(const PropertyRowWidget *row,
                                                           float baseline)
{
    if (!row) {
        return 0u;
    }
    const auto spinBoxes = row->findChildren<QDoubleSpinBox *>();
    for (int i = 0; i < spinBoxes.size(); ++i) {
        if (spinBoxes[i] && spinBoxes[i]->hasFocus()) {
            return static_cast<uint32_t>(i);
        }
    }
    const auto sliders = row->findChildren<QSlider *>();
    for (int i = 0; i < sliders.size(); ++i) {
        if (sliders[i] && sliders[i]->hasFocus()) {
            return static_cast<uint32_t>(i);
        }
    }
    const auto &vals = row->property().currentValues;
    uint32_t bestIdx = 0u;
    float bestDiff = std::numeric_limits<float>::max();
    for (size_t i = 0; i < vals.size(); ++i) {
        const float diff = std::fabs(vals[i] - baseline);
        if (diff < bestDiff) {
            bestDiff = diff;
            bestIdx = static_cast<uint32_t>(i);
        }
    }
    return bestIdx;
}

template <typename PanelT>
PanelT *ensureLazyPanel(const QPointer<PanelT> &member, const char *panelId,
                        DockLayoutManager *layoutMgr)
{
    if (!member && layoutMgr) {
        layoutMgr->realizeLazyPanel(QLatin1String(panelId));
    }
    return member.data();
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

void hydrateBoneRiggingPanelFromSnapshot(BoneRiggingPanel *panel,
                                         const BoneHierarchySnapshot &rigSnap, CommandBus *bus,
                                         IpcClient *ipc, bool &syncingDriveMode,
                                         int32_t selectedBoneIndex = -1)
{
    if (!panel) {
        return;
    }
    we_puppet_t puppet{};
    std::vector<we_mdl_bone_t> bones = rigSnap.bones;
    puppet.bone_count = static_cast<uint32_t>(bones.size());
    puppet.bones = bones.empty() ? nullptr : bones.data();

    const QSignalBlocker sigGuard(panel);
    const QScopedValueRollback<bool> modeGuard(syncingDriveMode, true);
    panel->setCommandBus(nullptr);
    panel->setIpcClient(nullptr);
    panel->loadPuppetRig(&puppet, rigSnap.objectId);
    for (const we_ik_chain_t &ch : rigSnap.ikChains) {
        panel->createTwoBoneIkChain(ch.root_bone, ch.mid_bone, ch.end_bone);
    }
    const size_t maxBones = std::min<size_t>(rigSnap.bones.size(), rigSnap.driveModes.size());
    for (size_t b = 0; b < maxBones; ++b) {
        if (rigSnap.driveModes[b] != WE_BONE_DRIVE_FK) {
            panel->setBoneDriveMode(rigSnap.objectId, static_cast<int32_t>(b),
                                    rigSnap.driveModes[b]);
        }
    }
    if (selectedBoneIndex >= 0 && selectedBoneIndex < static_cast<int32_t>(rigSnap.bones.size())) {
        panel->selectBone(selectedBoneIndex);
    }
    panel->setCommandBus(bus);
    panel->setIpcClient(ipc);
}

void remapCachedVertexBlendIndices(std::vector<we_mdl_vertex_t> &vertices,
                                   const std::vector<int32_t> &oldToNew)
{
    if (vertices.empty() || oldToNew.empty()) {
        return;
    }
    const bool anySurviving =
        std::any_of(oldToNew.begin(), oldToNew.end(), [](int32_t mapped) { return mapped >= 0; });
    for (we_mdl_vertex_t &vert : vertices) {
        float sum = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const uint32_t oldB = vert.blend_indices[k];
            const float w = std::isfinite(vert.weight[k]) ? vert.weight[k] : 0.0f;
            if (oldB < oldToNew.size() && oldToNew[oldB] >= 0 && w > 0.0f) {
                vert.blend_indices[k] = static_cast<uint32_t>(oldToNew[oldB]);
                sum += w;
            } else {
                vert.blend_indices[k] = 0u;
                vert.weight[k] = 0.0f;
            }
        }
        if (sum > 1e-6f) {
            for (int k = 0; k < 4; ++k) {
                vert.weight[k] /= sum;
            }
        } else {
            vert.blend_indices[0] = 0u;
            vert.blend_indices[1] = 0u;
            vert.blend_indices[2] = 0u;
            vert.blend_indices[3] = 0u;
            vert.weight[0] = anySurviving ? 1.0f : 0.0f;
            vert.weight[1] = 0.0f;
            vert.weight[2] = 0.0f;
            vert.weight[3] = 0.0f;
        }
    }
}

void buildBoneNamesAndParents(const std::vector<we_mdl_bone_t> &bones, std::vector<QString> &names,
                              std::vector<int32_t> &parents)
{
    names.clear();
    parents.clear();
    names.reserve(bones.size());
    parents.reserve(bones.size());
    for (size_t i = 0; i < bones.size(); ++i) {
        QString bName = QString::fromUtf8(bones[i].name).trimmed();
        if (bName.isEmpty()) {
            bName = QStringLiteral("Bone %1").arg(i);
        }
        names.push_back(std::move(bName));
        parents.push_back(bones[i].parent == 0xFFFFFFFFu ? -1
                                                         : static_cast<int32_t>(bones[i].parent));
    }
}

bool extractImageAlphaBuffer(const QImage &image, uint32_t &outWidth, uint32_t &outHeight,
                             std::vector<uint8_t> &outAlpha)
{
    if (image.isNull()) {
        return false;
    }
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull() || rgba.width() <= 0 || rgba.height() <= 0) {
        return false;
    }
    const uint32_t w = static_cast<uint32_t>(rgba.width());
    const uint32_t h = static_cast<uint32_t>(rgba.height());
    const uint64_t pixelCount = static_cast<uint64_t>(w) * static_cast<uint64_t>(h);
    if (w > WE_CDT_MAX_IMAGE_DIM || h > WE_CDT_MAX_IMAGE_DIM ||
        pixelCount > WE_CDT_MAX_IMAGE_PIXELS) {
        return false;
    }
    outAlpha.resize(static_cast<size_t>(pixelCount));
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t *row = rgba.constScanLine(static_cast<int>(y));
        const size_t dstRowBase = static_cast<size_t>(y) * w;
        for (uint32_t x = 0; x < w; ++x) {
            outAlpha[dstRowBase + x] = row[static_cast<size_t>(x) * 4u + 3u];
        }
    }
    outWidth = w;
    outHeight = h;
    return true;
}

QString formatAspectRatioGuideLabel(AspectRatioGuide guide)
{
    switch (guide) {
    case AspectRatioGuide::None:
        return QStringLiteral("Off");
    case AspectRatioGuide::Ratio16_9:
        return QStringLiteral("16:9");
    case AspectRatioGuide::Ratio16_10:
        return QStringLiteral("16:10");
    case AspectRatioGuide::Ratio21_9:
        return QStringLiteral("21:9");
    case AspectRatioGuide::Ratio32_9:
        return QStringLiteral("32:9");
    case AspectRatioGuide::Ratio4_3:
        return QStringLiteral("4:3");
    case AspectRatioGuide::MatchActiveDisplay:
        return QStringLiteral("Display");
    }
    return QStringLiteral("Off");
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

    // 3. Initialize Session Context (unparented CommandBus, PresetStore, IpcClient & Catalog)
    const bool interactiveMode = !m_options.headlessCheck && !m_options.benchmarkGrid;
    m_session = std::make_unique<StudioSessionContext>();
    m_session->initializeCoreServices(m_options.namespaceStr, m_options.socketPath, interactiveMode);

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
    if (auto *bus = commandBus()) {
        connect(bus, &CommandBus::historyChanged, this,
                [this](bool canUndo, bool canRedo, const QString &undoText,
                       const QString &redoText) {
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
    }

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

    // Defer directory scanning past the initial paint frame so startup latency stays bounded.
    if (!m_options.headlessCheck && !m_options.benchmarkGrid && m_gridWidget) {
        QTimer::singleShot(120, this, [this]() {
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

    // 12. Setup Cooperative Catalog Reconciliation & Recursive FS Watcher
    if (!m_options.headlessCheck && !m_options.benchmarkGrid && m_session) {
        QPointer<MainWindow> self(this);
        m_session->startWatchersAndReconcile([self]() {
            if (!self) {
                return;
            }
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
    }
}

MainWindow::~MainWindow()
{
    // Stop background workers and detach raw service pointers before destroying child docks,
    // ensuring ~CommandBus / ~IpcClient / ~PresetStore strictly outlive every panel (AUD-053).
    if (m_session) {
        m_session->shutdownAsyncWorkers();
    }
    detachPanelServicePointers();

    if (m_previewViewport && !m_previewViewport->parent()) {
        delete m_previewViewport;
        m_previewViewport = nullptr;
    }

    const auto directDocks = findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly);
    for (QDockWidget *dock : directDocks) {
        delete dock;
    }
    delete m_layoutManager;
    m_layoutManager = nullptr;

    m_session.reset();
}

void MainWindow::detachPanelServicePointers() noexcept
{
    if (m_previewViewport) {
        m_previewViewport->setCommandBus(nullptr);
    }
    if (m_bezierEditor) {
        m_bezierEditor->setCommandBus(nullptr);
    }
    if (m_propertyInspector) {
        m_propertyInspector->setCommandBus(nullptr);
        m_propertyInspector->setIpcClient(nullptr);
        m_propertyInspector->setPresetStore(nullptr);
    }
    if (m_nodeGraphEditor) {
        m_nodeGraphEditor->setCommandBus(nullptr);
        m_nodeGraphEditor->setIpcClient(nullptr);
    }
    if (m_cameraLightingInspector) {
        m_cameraLightingInspector->setCommandBus(nullptr);
        m_cameraLightingInspector->setIpcClient(nullptr);
    }
    if (m_physicsSpringInspector) {
        m_physicsSpringInspector->setCommandBus(nullptr);
        m_physicsSpringInspector->setIpcClient(nullptr);
    }
    if (m_sceneLayerTree) {
        m_sceneLayerTree->setCommandBus(nullptr);
        m_sceneLayerTree->setIpcClient(nullptr);
    }
    if (m_brushPainter) {
        m_brushPainter->setCommandBus(nullptr);
        m_brushPainter->setIpcClient(nullptr);
    }
    if (m_particleDesigner) {
        m_particleDesigner->setCommandBus(nullptr);
        m_particleDesigner->setIpcClient(nullptr);
    }
    if (m_audioBindingWizard) {
        m_audioBindingWizard->setCommandBus(nullptr);
        m_audioBindingWizard->setIpcClient(nullptr);
    }
    if (m_codeEditor) {
        m_codeEditor->setIpcClient(nullptr);
        m_codeEditor->setSharedOpenGLContext(nullptr);
    }
    if (m_puppetMeshPanel) {
        m_puppetMeshPanel->setCommandBus(nullptr);
        m_puppetMeshPanel->setIpcClient(nullptr);
    }
    if (m_boneRiggingPanel) {
        m_boneRiggingPanel->setCommandBus(nullptr);
        m_boneRiggingPanel->setIpcClient(nullptr);
    }
    if (m_weightPaintPanel) {
        m_weightPaintPanel->setCommandBus(nullptr);
        m_weightPaintPanel->setIpcClient(nullptr);
    }
    if (m_dopeSheetTimeline) {
        m_dopeSheetTimeline->setCommandBus(nullptr);
        m_dopeSheetTimeline->setIpcClient(nullptr);
    }
}

CommandBus *MainWindow::commandBus() const
{
    return m_session ? m_session->commandBus() : nullptr;
}
PresetStore *MainWindow::presetStore() const
{
    return m_session ? m_session->presetStore() : nullptr;
}
CatalogIndexManager *MainWindow::catalogIndexManager() const
{
    return m_session ? m_session->catalogIndexManager().get() : nullptr;
}
DownloadManager *MainWindow::downloadManager() const
{
    return m_session ? m_session->downloadManager().get() : nullptr;
}
RecursiveFsWatcher *MainWindow::fsWatcher() const
{
    return m_session ? m_session->fsWatcher() : nullptr;
}
IpcClient *MainWindow::ipcClient() const
{
    return m_session ? m_session->ipcClient() : nullptr;
}

MonitorTopologyBar *MainWindow::topologyBar() const { return m_topologyBar; }
VirtualizedGridWidget *MainWindow::gridWidget() const { return m_gridWidget; }
GpuPreviewViewport *MainWindow::previewViewport() const { return m_previewViewport; }
CubicBezierEditorWidget *MainWindow::bezierEditor() const { return m_bezierEditor; }
PropertyInspectorWidget *MainWindow::propertyInspector() const { return m_propertyInspector; }
ThemeStudioWidget *MainWindow::themeStudio() const
{
    return ensureLazyPanel(m_themeStudio, "ThemeStudioWidget", m_layoutManager);
}
StudioFooterBar *MainWindow::footerBar() const { return m_footerBar; }
WorkshopBrowserWidget *MainWindow::workshopBrowser() const
{
    return ensureLazyPanel(m_workshopBrowser, "WorkshopBrowser", m_layoutManager);
}
OpenHubBrowserWidget *MainWindow::openHubBrowser() const
{
    return ensureLazyPanel(m_openHubBrowser, "OpenHubBrowser", m_layoutManager);
}
NodeGraphEditorWidget *MainWindow::nodeGraphEditor() const
{
    return ensureLazyPanel(m_nodeGraphEditor, "NodeGraphEditorWidget", m_layoutManager);
}
EffectLibraryBrowserWidget *MainWindow::effectLibraryBrowser() const
{
    return ensureLazyPanel(m_effectLibraryBrowser, "EffectLibraryBrowserWidget", m_layoutManager);
}
CameraLightingInspectorWidget *MainWindow::cameraLightingInspector() const
{
    return ensureLazyPanel(m_cameraLightingInspector, "CameraLightingInspectorWidget",
                           m_layoutManager);
}
PhysicsSpringInspectorWidget *MainWindow::physicsSpringInspector() const
{
    return ensureLazyPanel(m_physicsSpringInspector, "PhysicsSpringInspectorWidget",
                           m_layoutManager);
}
SceneLayerTreeWidget *MainWindow::sceneLayerTree() const
{
    return ensureLazyPanel(m_sceneLayerTree, "SceneLayerTree", m_layoutManager);
}
GpuBrushPainterWidget *MainWindow::brushPainter() const
{
    return ensureLazyPanel(m_brushPainter, "GpuBrushPainter", m_layoutManager);
}
ParticleDesignerWidget *MainWindow::particleDesigner() const
{
    return ensureLazyPanel(m_particleDesigner, "ParticleDesigner", m_layoutManager);
}
AudioBindingWizard *MainWindow::audioBindingWizard() const
{
    return ensureLazyPanel(m_audioBindingWizard, "AudioBindingWizard", m_layoutManager);
}
GlslSceneScriptEditorWidget *MainWindow::codeEditor() const
{
    return ensureLazyPanel(m_codeEditor, "GlslSceneScriptEditor", m_layoutManager);
}
PuppetMeshPanel *MainWindow::puppetMeshPanel() const
{
    return ensureLazyPanel(m_puppetMeshPanel, "PuppetMeshPanel", m_layoutManager);
}
BoneRiggingPanel *MainWindow::boneRiggingPanel() const
{
    return ensureLazyPanel(m_boneRiggingPanel, "BoneRiggingPanel", m_layoutManager);
}
WeightPaintPanel *MainWindow::weightPaintPanel() const
{
    return ensureLazyPanel(m_weightPaintPanel, "WeightPaintPanel", m_layoutManager);
}
DopeSheetTimelineWidget *MainWindow::dopeSheetTimeline() const
{
    return ensureLazyPanel(m_dopeSheetTimeline, "DopeSheetTimelineWidget", m_layoutManager);
}

void MainWindow::setupPanels()
{
    // Panel 1 (Top Bar): Monitor Topology & Span Mode
    m_topologyBar = new MonitorTopologyBar(this);
    m_layoutManager->registerPanel(QStringLiteral("MonitorTopologyBar"), m_topologyBar,
                                   QStringLiteral("Displays"), Qt::TopDockWidgetArea);

    const auto catalogMgr = m_session ? m_session->catalogIndexManager()
                                      : std::shared_ptr<CatalogIndexManager>();
    const auto dlMgr = m_session ? m_session->downloadManager()
                                 : std::shared_ptr<DownloadManager>();

    // Panel 2 (Left Dock Tab 1): Virtualized Wallpaper Grid (eager core panel)
    m_gridWidget = new VirtualizedGridWidget(this);
    m_layoutManager->registerPanel(QStringLiteral("Wallpapers"), m_gridWidget,
                                   QStringLiteral("Library"), Qt::LeftDockWidgetArea);

    m_layoutManager->registerLazyPanel(
        QStringLiteral("WorkshopBrowser"), QStringLiteral("Workshop"),
        [this, catalogMgr, dlMgr](QWidget *parent) -> QWidget * {
            auto *ws = new WorkshopBrowserWidget(catalogMgr, dlMgr, parent);
            m_workshopBrowser = ws;
            wireWorkshopBrowserSignals(ws);
            return ws;
        },
        Qt::LeftDockWidgetArea);

    m_layoutManager->registerLazyPanel(
        QStringLiteral("OpenHubBrowser"), QStringLiteral("Open Hub"),
        [this, dlMgr](QWidget *parent) -> QWidget * {
            auto *hub = new OpenHubBrowserWidget(dlMgr, parent);
            m_openHubBrowser = hub;
            wireOpenHubBrowserSignals(hub);
            return hub;
        },
        Qt::LeftDockWidgetArea);

    // Panel 3 (Center Dock Top): GPU Preview Viewport (eager core panel)
    // Always avoid parenting QOpenGLWidget directly under a dock in headless offscreen mode
    // across every Qt 6 release, keeping previewViewport() non-null for tests.
    if (QGuiApplication::platformName() == QLatin1String("offscreen")) {
        m_previewViewport = new GpuPreviewViewport(nullptr);
        m_previewViewport->setCommandBus(commandBus());
        auto *offscreenPlaceholder = new QWidget(this);
        offscreenPlaceholder->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        m_layoutManager->registerPanel(QStringLiteral("GpuPreviewViewport"), offscreenPlaceholder,
                                       QStringLiteral("Preview"), Qt::RightDockWidgetArea);
    } else {
        m_previewViewport = new GpuPreviewViewport(this);
        m_previewViewport->setCommandBus(commandBus());
        m_layoutManager->registerPanel(QStringLiteral("GpuPreviewViewport"), m_previewViewport,
                                       QStringLiteral("Preview"), Qt::RightDockWidgetArea);
    }

    // Panel 4 (Center Dock Bottom): Transition Cubic Bezier Editor (eager core panel)
    m_bezierEditor = new CubicBezierEditorWidget(this);
    m_bezierEditor->setCommandBus(commandBus());
    for (QPushButton *btn : m_bezierEditor->findChildren<QPushButton *>()) {
        if (btn && btn->maximumHeight() > 0 && btn->maximumHeight() < 22) {
            btn->setFixedHeight(22);
        }
    }
    m_layoutManager->registerPanel(QStringLiteral("CubicBezier"), m_bezierEditor,
                                   QStringLiteral("Transitions"), Qt::RightDockWidgetArea);

    // Panel 5 (Right Dock Tab 1): Live Scene & Shader Inspector (eager core panel)
    m_propertyInspector = new PropertyInspectorWidget(this);
    m_propertyInspector->setCommandBus(commandBus());
    m_propertyInspector->setIpcClient(ipcClient());
    m_propertyInspector->setPresetStore(presetStore());
    m_layoutManager->registerPanel(QStringLiteral("PropertyInspectorWidget"), m_propertyInspector,
                                   QStringLiteral("Inspector"), Qt::RightDockWidgetArea);

    m_layoutManager->registerLazyPanel(
        QStringLiteral("ThemeStudioWidget"), QStringLiteral("Theme"),
        [this](QWidget *parent) -> QWidget * {
            auto *theme = new ThemeStudioWidget(parent);
            m_themeStudio = theme;
            wireThemeStudioSignals(theme);
            if (!m_activeWallpaperPath.isEmpty()) {
                if (m_previewViewport && !m_previewViewport->previewImage().isNull() &&
                    !m_activeWallpaperPath.endsWith(QStringLiteral(".glsl"), Qt::CaseInsensitive)) {
                    theme->setWallpaperImage(m_previewViewport->previewImage(),
                                             m_activeWallpaperPath);
                } else {
                    theme->loadWallpaper(m_activeWallpaperPath);
                }
            }
            return theme;
        },
        Qt::RightDockWidgetArea);

    m_layoutManager->registerLazyPanel(
        QStringLiteral("NodeGraphEditorWidget"), QStringLiteral("Shader Graph"),
        [this](QWidget *parent) -> QWidget * {
            auto *graph = new NodeGraphEditorWidget(parent);
            graph->setCommandBus(commandBus());
            graph->setIpcClient(ipcClient());
            graph->setTargetOutput(resolveSelectedOutputsCsv());
            m_nodeGraphEditor = graph;
            wireNodeGraphEditorSignals(graph);
            return graph;
        },
        Qt::RightDockWidgetArea);

    m_layoutManager->registerLazyPanel(
        QStringLiteral("EffectLibraryBrowserWidget"), QStringLiteral("Effect Library"),
        [this](QWidget *parent) -> QWidget * {
            auto *browser = new EffectLibraryBrowserWidget(parent);
            m_effectLibraryBrowser = browser;
            wireEffectLibrarySignals(browser);
            return browser;
        },
        Qt::LeftDockWidgetArea);

    m_layoutManager->registerLazyPanel(
        QStringLiteral("CameraLightingInspectorWidget"), QStringLiteral("Camera & Lights"),
        [this](QWidget *parent) -> QWidget * {
            auto *camLights = new CameraLightingInspectorWidget(parent);
            camLights->setCommandBus(commandBus());
            camLights->setIpcClient(ipcClient());
            camLights->setTargetOutput(resolveSelectedOutputsCsv());
            if (m_previewViewport) {
                m_previewViewport->setGizmoCameraAndLights(camLights->camera(),
                                                           camLights->lighting());
            }
            m_cameraLightingInspector = camLights;
            wireCameraLightingSignals(camLights);
            return camLights;
        },
        Qt::RightDockWidgetArea);

    m_layoutManager->registerLazyPanel(
        QStringLiteral("PhysicsSpringInspectorWidget"), QStringLiteral("Puppet Physics"),
        [this](QWidget *parent) -> QWidget * {
            auto *phys = new PhysicsSpringInspectorWidget(parent);
            phys->setCommandBus(commandBus());
            phys->setIpcClient(ipcClient());
            phys->setTargetOutput(resolveSelectedOutputsCsv());
            m_physicsSpringInspector = phys;
            wirePhysicsSpringSignals(phys);
            if (m_hasCachedPuppetRig) {
                const QScopedValueRollback<bool> guard(m_syncingBoneDriveMode, true);
                std::vector<QString> names;
                std::vector<int32_t> parents;
                buildBoneNamesAndParents(m_cachedPuppetRig.bones, names, parents);
                we_puppet_t physPuppet{};
                std::vector<we_mdl_bone_t> physBones = m_cachedPuppetRig.bones;
                physPuppet.bone_count = static_cast<uint32_t>(physBones.size());
                physPuppet.bones = physBones.empty() ? nullptr : physBones.data();
                phys->loadPuppetSkeleton(&physPuppet, m_cachedPuppetRig.objectId);
                phys->setBoneList(names, parents, m_cachedPuppetRig.objectId);
                phys->setBoneDriveModes(m_cachedPuppetRig.driveModes);
            }
            return phys;
        },
        Qt::RightDockWidgetArea);

    // Panel 6 (Bottom Footer): Audio FFT Spectrum, Power Governor & Telemetry HUD
    m_footerBar = new StudioFooterBar(this);
    m_layoutManager->registerPanel(QStringLiteral("StudioFooterBar"), m_footerBar,
                                   QStringLiteral("Telemetry"), Qt::BottomDockWidgetArea);

    // Register secondary authoring panels lazily to bound initial footprint and startup time.
    m_layoutManager->registerLazyPanel(
        QStringLiteral("SceneLayerTree"), QStringLiteral("Scene Hierarchy"),
        [this](QWidget *parent) -> QWidget * {
            auto *tree = new SceneLayerTreeWidget(parent);
            const QString targetOut = resolveSelectedOutputsCsv();
            tree->setCommandBus(commandBus());
            tree->setIpcClient(ipcClient());
            tree->setTargetOutput(targetOut);
            if (!m_lastSceneTreeJson.isEmpty()) {
                tree->loadFromSceneJson(m_lastSceneTreeJson);
            } else if (auto *ipc = ipcClient(); ipc && ipc->isConnected()) {
                ipc->sendQuerySceneTree(targetOut);
            }
            connect(tree, &SceneLayerTreeWidget::objectSelected, this,
                    [this, tree](int32_t objectId, we_scene_node_kind_t kind) {
                        const SceneLayerNode *node = tree->selectedNode();
                        const QString activeOut = resolveSelectedOutputsCsv();
                        if (m_previewViewport) {
                            m_previewViewport->setGizmoActiveNode(node);
                        }
                        if (m_propertyInspector) {
                            m_propertyInspector->setTargetOutput(activeOut);
                            m_propertyInspector->setTargetObjectId(objectId, 0);
                        }
                        if (m_audioBindingWizard) {
                            m_audioBindingWizard->setTargetOutput(activeOut);
                        }
                        if (objectId < 0) {
                            return;
                        }
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
                        if (kind == WE_NODE_IMAGE) {
                            setActivePuppetObjectContext(objectId, texW, texH);
                            if (m_brushPainter) {
                                m_brushPainter->setTargetOutput(activeOut);
                                m_brushPainter->setTargetTextureSlot(objectId, 0u, 0u, texW, texH,
                                                                     0u);
                            }
                            if (m_physicsSpringInspector && m_hasCachedPuppetRig &&
                                m_cachedPuppetRig.objectId == objectId) {
                                std::vector<QString> names;
                                std::vector<int32_t> parents;
                                buildBoneNamesAndParents(m_cachedPuppetRig.bones, names, parents);
                                m_physicsSpringInspector->setTargetOutput(activeOut);
                                const bool needsInit =
                                    !m_physicsSpringInspector->hasObjectState(objectId) ||
                                    (m_physicsSpringInspector->objectId() == objectId &&
                                     m_physicsSpringInspector->boneCount() == 0u && !names.empty());
                                if (needsInit) {
                                    we_puppet_t physPuppet{};
                                    std::vector<we_mdl_bone_t> bones = m_cachedPuppetRig.bones;
                                    physPuppet.bone_count = static_cast<uint32_t>(bones.size());
                                    physPuppet.bones = bones.empty() ? nullptr : bones.data();
                                    m_physicsSpringInspector->loadPuppetSkeleton(&physPuppet,
                                                                                 objectId);
                                }
                                m_physicsSpringInspector->setBoneList(names, parents, objectId);
                            }
                            if (auto *ipc = ipcClient();
                                ipc && ipc->isConnected() && objectId >= 0 &&
                                m_cachedObjectAlphas.find(objectId) == m_cachedObjectAlphas.end() &&
                                !(m_cachedPuppetMesh.objectId == objectId &&
                                  m_cachedPuppetMesh.vertices.size() >= 3u) &&
                                m_pendingAlphaReadbackObjectIds.size() < 4u) {
                                if (ipc->sendReadbackTexture(activeOut, objectId, 0u, 0u, 0u)) {
                                    m_pendingAlphaReadbackObjectIds.push_back(objectId);
                                }
                            }
                        } else if (kind == WE_NODE_PARTICLE) {
                            if (m_particleDesigner) {
                                m_particleDesigner->setTargetOutput(activeOut);
                                m_particleDesigner->loadParticleSystem(objectId, nullptr);
                            }
                        } else if (kind == WE_NODE_CAMERA3D || kind == WE_NODE_LIGHT) {
                            if (m_cameraLightingInspector) {
                                m_cameraLightingInspector->setTargetOutput(activeOut);
                                m_cameraLightingInspector->setCameraAndLights(
                                    m_cameraLightingInspector->camera(),
                                    m_cameraLightingInspector->lighting());
                            }
                        }
                    });
            connect(tree, &SceneLayerTreeWidget::layerMutated, this,
                    [this](int32_t) { syncCachedSceneLayersToAudioWizard(); });
            connect(tree, &SceneLayerTreeWidget::statusMessage, this, [this](const QString &msg) {
                if (m_statusLabel) {
                    m_statusLabel->setText(msg);
                }
            });
            m_sceneLayerTree = tree;
            if (!m_lastSceneTreeJson.isEmpty()) {
                syncCachedSceneLayersToAudioWizard();
            }
            return tree;
        },
        Qt::LeftDockWidgetArea);

    m_layoutManager->registerLazyPanel(
        QStringLiteral("GpuBrushPainter"), QStringLiteral("GPU Paint Brush"),
        [this](QWidget *parent) -> QWidget * {
            auto *brush = new GpuBrushPainterWidget(parent);
            brush->setCommandBus(commandBus());
            brush->setIpcClient(ipcClient());
            brush->setTargetOutput(resolveSelectedOutputsCsv());
            if (m_sceneLayerTree && m_sceneLayerTree->selectedObjectId() >= 0) {
                const SceneLayerNode *node = m_sceneLayerTree->selectedNode();
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
                brush->setTargetTextureSlot(m_sceneLayerTree->selectedObjectId(), 0u, 0u, texW,
                                            texH, 0u);
            }
            connect(brush, &GpuBrushPainterWidget::targetChannelOverlayRequested, this,
                    [this](we_brush_channel_t channel, bool showOverlay) {
                        if (m_previewViewport) {
                            m_previewViewport->setShaderUniform(QStringLiteral("u_brushOverlay"),
                                                                showOverlay ? 1 : 0);
                            m_previewViewport->setShaderUniform(QStringLiteral("u_brushChannel"),
                                                                static_cast<int>(channel));
                        }
                        if (m_statusLabel) {
                            m_statusLabel->setText(
                                QStringLiteral("Brush overlay channel %1 %2")
                                    .arg(static_cast<int>(channel))
                                    .arg(showOverlay ? QStringLiteral("enabled")
                                                     : QStringLiteral("disabled")));
                        }
                    });
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
            designer->setCommandBus(commandBus());
            designer->setIpcClient(ipcClient());
            designer->setTargetOutput(resolveSelectedOutputsCsv());
            if (m_sceneLayerTree && m_sceneLayerTree->selectedObjectId() >= 0) {
                const SceneLayerNode *node = m_sceneLayerTree->selectedNode();
                if (node && node->kind == WE_NODE_PARTICLE) {
                    designer->loadParticleSystem(node->id, nullptr);
                }
            }
            connect(designer, &ParticleDesignerWidget::particleSystemModified, this,
                    [this](int32_t objectId, const QString &jsonDefinition) {
                        if (m_sceneLayerTree) {
                            if (auto *ipc = ipcClient(); ipc && ipc->isConnected()) {
                                ipc->sendQuerySceneTree(resolveSelectedOutputsCsv());
                            }
                        }
                        if (m_statusLabel && !jsonDefinition.isEmpty()) {
                            m_statusLabel->setText(
                                QStringLiteral("Updated particle system (object %1)").arg(objectId));
                        }
                    });
            connect(designer, &ParticleDesignerWidget::controlPointOverlayUpdated, this,
                    [this](int32_t objectId) {
                        if (m_previewViewport) {
                            m_previewViewport->setShaderUniform(
                                QStringLiteral("u_particleControlObjectId"), objectId);
                        }
                    });
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
            wiz->setCommandBus(commandBus());
            wiz->setIpcClient(ipcClient());
            wiz->setTargetOutput(resolveSelectedOutputsCsv());
            if (m_sceneLayerTree) {
                if (auto *model = m_sceneLayerTree->findChild<SceneLayerTreeModel *>()) {
                    m_cachedSceneLayers = model->nodes();
                }
            } else if (m_cachedSceneLayers.empty() && !m_lastSceneTreeJson.isEmpty()) {
                m_cachedSceneLayers = parseSceneLayerNodesJson(m_lastSceneTreeJson);
            }
            if (!m_cachedSceneLayers.empty()) {
                wiz->setSceneLayers(m_cachedSceneLayers);
            }
            if (!m_cachedAudioBindings.empty()) {
                wiz->setActiveBindings(m_cachedAudioBindings);
            }
            connect(wiz, &AudioBindingWizard::bindingsUpdated, this,
                    [this](const std::vector<we_audio_binding_t> &bindings) {
                        m_cachedAudioBindings = bindings;
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
            editor->setIpcClient(ipcClient());
            m_codeEditor = editor;
            updateGpuShaderEditorContext();
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
                    QString source;
                    QString readErr;
                    if (StudioSessionContext::readBoundedUtf8File(m_activeWallpaperPath, source,
                                                                  readErr)) {
                        editor->openDocument(EditorLanguageMode::GlslEs310Fragment, -1, 0,
                                             QFileInfo(m_activeWallpaperPath).fileName(), source);
                    } else if (m_statusLabel && !readErr.isEmpty()) {
                        m_statusLabel->setText(readErr);
                    }
                }
            }
            return editor;
        },
        Qt::BottomDockWidgetArea);

    m_layoutManager->registerLazyPanel(
        QStringLiteral("PuppetMeshPanel"), QStringLiteral("Puppet Mesh"),
        [this](QWidget *parent) -> QWidget * {
            auto *panel = new PuppetMeshPanel(parent);
            panel->setCommandBus(commandBus());
            panel->setIpcClient(ipcClient());
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
            connect(panel, &PuppetMeshPanel::contoursExtracted, this,
                    [this](int32_t objectId, const std::vector<ContourPreviewLoop> &loops) {
                        if (m_previewViewport &&
                            (objectId < 0 || m_previewViewport->puppetObjectId() < 0 ||
                             m_previewViewport->puppetObjectId() == objectId)) {
                            m_previewViewport->setPuppetContourPreviewLoops(loops);
                        }
                    });
            connect(
                panel, &PuppetMeshPanel::exportMdlRequested, this,
                [this](int32_t objectId, const QString &destPath) {
                    auto *ipc = ipcClient();
                    if (!ipc || objectId < 0 || destPath.isEmpty()) {
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
                    if (!ipc->sendExportPuppetMdl(static_cast<uint32_t>(objectId), flags,
                                                  inAllowedDir ? destPath : QString())) {
                        m_pendingMdlExportPaths.erase(objectId);
                    }
                });
            connect(panel, &PuppetMeshPanel::statusMessage, this, [this](const QString &msg) {
                if (m_statusLabel) {
                    m_statusLabel->setText(msg);
                }
            });
            m_puppetMeshPanel = panel;
            if (m_activePuppetObjectId >= 0) {
                panel->setTargetObjectId(m_activePuppetObjectId, m_activePuppetTexWidth,
                                         m_activePuppetTexHeight);
            }
            if (auto it = m_cachedObjectAlphas.find(m_activePuppetObjectId);
                it != m_cachedObjectAlphas.end()) {
                it->second.pushed = false;
            }
            m_cachedPreviewAlpha.pushed = false;
            hydratePuppetMeshPanel(panel);
            return panel;
        },
        Qt::LeftDockWidgetArea);

    m_layoutManager->registerLazyPanel(
        QStringLiteral("BoneRiggingPanel"), QStringLiteral("Bone Rigging"),
        [this](QWidget *parent) -> QWidget * {
            auto *panel = new BoneRiggingPanel(parent);
            panel->setCommandBus(commandBus());
            panel->setIpcClient(ipcClient());
            connect(panel, &BoneRiggingPanel::boneSelected, this, [this](int32_t boneIndex) {
                if (m_previewViewport) {
                    m_previewViewport->setSelectedPuppetBone(boneIndex);
                    if (boneIndex >= 0) {
                        m_previewViewport->setWeightBrushActiveBone(
                            static_cast<uint32_t>(boneIndex));
                    }
                }
                if (m_weightPaintPanel) {
                    m_weightPaintPanel->setActiveBone(boneIndex);
                }
                if (m_physicsSpringInspector) {
                    m_physicsSpringInspector->selectBoneIndex(boneIndex);
                }
            });
            connect(panel, &BoneRiggingPanel::bonesRemapped, this,
                    [this](int32_t objectId, const std::vector<int32_t> &oldToNew) {
                        if (oldToNew.empty()) {
                            return;
                        }
                        if (m_cachedPuppetMesh.objectId == objectId) {
                            remapCachedVertexBlendIndices(m_cachedPuppetMesh.vertices, oldToNew);
                        }
                        if (const auto modeIt = m_pendingDriveModes.find(objectId);
                            modeIt != m_pendingDriveModes.end()) {
                            std::unordered_map<int32_t, we_bone_drive_mode_t> remapped;
                            for (const auto &[oldBoneIdx, mode] : modeIt->second) {
                                if (oldBoneIdx >= 0 &&
                                    static_cast<size_t>(oldBoneIdx) < oldToNew.size() &&
                                    oldToNew[static_cast<size_t>(oldBoneIdx)] >= 0) {
                                    remapped[oldToNew[static_cast<size_t>(oldBoneIdx)]] = mode;
                                }
                            }
                            modeIt->second = std::move(remapped);
                        }
                        if (m_puppetMeshPanel) {
                            m_puppetMeshPanel->remapVertexBlendIndices(objectId, oldToNew);
                        }
                        if (m_weightPaintPanel) {
                            m_weightPaintPanel->remapBoneIndices(objectId, oldToNew);
                        }
                        if (m_dopeSheetTimeline) {
                            m_dopeSheetTimeline->remapBoneIndices(objectId, oldToNew);
                        }
                        if (m_physicsSpringInspector) {
                            m_physicsSpringInspector->remapBoneIndices(objectId, oldToNew);
                        }
                        if (m_previewViewport) {
                            m_previewViewport->remapPuppetBoneIndices(objectId, oldToNew);
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
                        buildBoneNamesAndParents(snapshot.bones, names, parents);
                        m_physicsSpringInspector->setTargetOutput(resolveSelectedOutputsCsv());
                        // Preserve existing or remapped spring configs when only topology updates.
                        const bool needsInit =
                            !m_physicsSpringInspector->hasObjectState(objectId) ||
                            (m_physicsSpringInspector->objectId() == objectId &&
                             m_physicsSpringInspector->boneCount() == 0u && !names.empty());
                        if (needsInit) {
                            we_puppet_t physPuppet{};
                            std::vector<we_mdl_bone_t> physBones = snapshot.bones;
                            physPuppet.bone_count = static_cast<uint32_t>(physBones.size());
                            physPuppet.bones = physBones.empty() ? nullptr : physBones.data();
                            m_physicsSpringInspector->loadPuppetSkeleton(&physPuppet, objectId);
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
                        auto *ipc = ipcClient();
                        if (ipc && objectId >= 0) {
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
                            ipc->sendPuppetPreviewIk(req);
                        }
                    });
            connect(panel, &BoneRiggingPanel::statusMessage, this, [this](const QString &msg) {
                if (m_statusLabel) {
                    m_statusLabel->setText(msg);
                }
            });
            if (m_hasCachedPuppetRig && (m_activePuppetObjectId < 0 ||
                                         m_cachedPuppetRig.objectId == m_activePuppetObjectId)) {
                // Hydrate cached bones, IK chains, and non-FK drive modes without generating
                // synthetic undo entries or clobbering m_cachedPuppetRig (AUD-180).
                const BoneHierarchySnapshot cachedRig = m_cachedPuppetRig;
                hydrateBoneRiggingPanelFromSnapshot(panel, cachedRig, commandBus(), ipcClient(),
                                                    m_syncingBoneDriveMode);
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
            panel->setCommandBus(commandBus());
            panel->setIpcClient(ipcClient());
            connect(panel, &WeightPaintPanel::heatmapModeChanged, this,
                    [this](int32_t /*objectId*/, bool enabled, uint32_t activeBoneIdx,
                           float opacity, we_heatmap_palette_t palette) {
                        if (m_previewViewport) {
                            m_previewViewport->setWeightHeatmapMode(enabled, activeBoneIdx, opacity,
                                                                    palette);
                        }
                    });
            connect(panel, &WeightPaintPanel::normalizeAllWeightsRequested, this,
                    [this, panel](int32_t objectId, uint64_t lockMaskLo, uint64_t lockMaskHi) {
                        if (m_previewViewport && objectId >= 0) {
                            static const std::vector<uint64_t> emptyLocks;
                            m_previewViewport->setWeightLockedVertices(
                                objectId, panel->respectLockedVertices() ? panel->lockedVertexBits()
                                                                         : emptyLocks);
                            m_previewViewport->normalizeAllVertexWeights(
                                objectId, std::array<uint64_t, 2>{lockMaskLo, lockMaskHi});
                        }
                    });
            connect(
                panel, &WeightPaintPanel::vertexLocksChanged, this,
                [this, panel](int32_t objectId, const std::vector<uint64_t> &lockedVertexBitset) {
                    if (m_previewViewport) {
                        static const std::vector<uint64_t> emptyLocks;
                        m_previewViewport->setWeightLockedVertices(
                            objectId,
                            panel->respectLockedVertices() ? lockedVertexBitset : emptyLocks);
                    }
                });
            connect(panel, &WeightPaintPanel::brushParamsChanged, this,
                    [this, panel]() { syncWeightPaintBrushAndLocks(panel); });
            if (auto *boneList = panel->findChild<QListWidget *>()) {
                if (auto *selModel = boneList->selectionModel()) {
                    connect(selModel, &QItemSelectionModel::currentRowChanged, this,
                            [this](const QModelIndex &current, const QModelIndex &) {
                                if (m_previewViewport && current.isValid() && current.row() >= 0) {
                                    m_previewViewport->setWeightBrushActiveBone(
                                        static_cast<uint32_t>(current.row()));
                                }
                            });
                }
            }
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
            timeline->setCommandBus(commandBus());
            timeline->setIpcClient(ipcClient());
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
            if (auto *clipCombo = timeline->findChild<QComboBox *>(QStringLiteral("timelineClipCombo"))) {
                connect(clipCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                        [this](int /*idx*/) {
                            if (m_onionSkinEnabled) {
                                refreshOnionSkinFrames();
                            }
                        });
            }
            m_dopeSheetTimeline = timeline;
            return timeline;
        },
        Qt::BottomDockWidgetArea);

    // Remove standard status bar to let custom FooterBar shine
    setStatusBar(nullptr);

    if (m_previewViewport) {
        QPointer<QOpenGLContext> lastBoundCtx;
        connect(m_previewViewport, &QOpenGLWidget::frameSwapped, this,
                [this, lastBoundCtx]() mutable {
                    if (!m_previewViewport || !m_codeEditor) {
                        return;
                    }
                    QOpenGLContext *cur = m_previewViewport->context();
                    if (cur != lastBoundCtx.data()) {
                        lastBoundCtx = cur;
                        updateGpuShaderEditorContext();
                    }
                });
    }
    syncMonitorCatalogToPanels();
}

QString MainWindow::resolveSelectedOutputsCsv() const
{
    if (!m_topologyBar || !m_topologyBar->canvas()) {
        return QStringLiteral("*");
    }
    auto *canvas = m_topologyBar->canvas();
    if (canvas->isDesktopSpanMode()) {
        return QStringLiteral("*");
    }
    const QStringList selected = canvas->selectedMonitorNames();
    if (selected.isEmpty()) {
        if (m_previewViewport) {
            const QString vpSel = m_previewViewport->selectedMonitor().trimmed();
            if (!vpSel.isEmpty()) {
                return normalizeTargetOutputSpec(vpSel);
            }
        }
        return QStringLiteral("*");
    }
    return selected.join(QLatin1Char(','));
}

void MainWindow::syncMonitorCatalogToPanels()
{
    if (!m_topologyBar || !m_topologyBar->canvas()) {
        return;
    }
    auto *canvas = m_topologyBar->canvas();
    QStringList names;
    const auto &monitors = canvas->monitors();
    names.reserve(monitors.size());
    for (const MonitorInfo &info : monitors) {
        if (!info.name.isEmpty()) {
            names.push_back(info.name);
        }
    }
    if (m_previewViewport) {
        const QSignalBlocker vpBlocker(m_previewViewport);
        m_previewViewport->setAvailableMonitors(names);
        const QStringList selected = canvas->selectedMonitorNames();
        if (!selected.isEmpty()) {
            m_previewViewport->setSelectedMonitor(selected.first());
        } else if (!names.isEmpty()) {
            m_previewViewport->setSelectedMonitor(names.first());
        }
    }
    const QString targetOut = resolveSelectedOutputsCsv();
    if (m_propertyInspector) {
        m_propertyInspector->setTargetOutput(targetOut);
    }
    if (m_nodeGraphEditor) {
        m_nodeGraphEditor->setTargetOutput(targetOut);
    }
    if (m_cameraLightingInspector) {
        m_cameraLightingInspector->setTargetOutput(targetOut);
    }
    if (m_physicsSpringInspector) {
        m_physicsSpringInspector->setTargetOutput(targetOut);
    }
    if (m_sceneLayerTree) {
        m_sceneLayerTree->setTargetOutput(targetOut);
    }
    if (m_brushPainter) {
        m_brushPainter->setTargetOutput(targetOut);
    }
    if (m_particleDesigner) {
        m_particleDesigner->setTargetOutput(targetOut);
    }
    if (m_audioBindingWizard) {
        m_audioBindingWizard->setTargetOutput(targetOut);
    }
    if (m_codeEditor) {
        m_codeEditor->setTargetOutput(targetOut);
    }
}

void MainWindow::updateGpuShaderEditorContext()
{
    if (!m_previewViewport || !m_codeEditor) {
        return;
    }
    QOpenGLContext *ctx = m_previewViewport->context();
    m_codeEditor->setSharedOpenGLContext(ctx);
    if (ctx) {
        QPointer<GlslSceneScriptEditorWidget> safeEditor(m_codeEditor);
        connect(
            ctx, &QOpenGLContext::aboutToBeDestroyed, this,
            [safeEditor]() {
                if (safeEditor) {
                    safeEditor->setSharedOpenGLContext(nullptr);
                }
            },
            Qt::UniqueConnection);
    }
}

void MainWindow::syncThemeStateToIpcOrActiveWallpaper(bool includeApplyHint)
{
    if (!m_themeStudio) {
        return;
    }
    IIpcClientService *ipc =
        m_session ? m_session->activeIpcService() : Services::instance().ipcClient();
    if (ipc && ipc->isConnected()) {
        ipc->sendAudioPowerConfig(m_audioVolume, m_muteOnAudio ? 1 : 0, 0, m_batteryPolicy);
    }
    if (includeApplyHint && !m_activeWallpaperPath.isEmpty() &&
        QFileInfo::exists(m_activeWallpaperPath)) {
        applyWallpaperPath(m_activeWallpaperPath, resolveSelectedOutputsCsv());
    }
}

void MainWindow::setupMenuBar()
{
    auto *mb = menuBar();

    // --- File Menu ---
    auto *fileMenu = mb->addMenu(QStringLiteral("&File"));

    auto *openWallpaperAction = fileMenu->addAction(QStringLiteral("Open &Wallpaper..."));
    openWallpaperAction->setShortcut(QKeySequence::Open);
    connect(openWallpaperAction, &QAction::triggered, this, [this]() {
        auto *dlg = new QFileDialog(
            this, QStringLiteral("Open Wallpaper"), QString(),
            QStringLiteral("Wallpaper Media (*.png *.jpg *.jpeg *.webp *.jxl *.avif *.svg *.mp4 "
                           "*.webm *.gif *.glsl *.frag *.mdl);;All Files (*)"));
        dlg->setFileMode(QFileDialog::ExistingFile);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        QPointer<MainWindow> self(this);
        connect(dlg, &QFileDialog::fileSelected, this, [self](const QString &file) {
            if (!self || file.isEmpty()) {
                return;
            }
            self->m_activeWallpaperPath = file;
            const QString ext = QFileInfo(file).suffix().toLower();
            if (ext == QStringLiteral("mdl")) {
                self->loadPuppetFromMdlFile(file,
                                            std::max<int32_t>(0, self->m_activePuppetObjectId));
                return;
            }
            const bool isShader = (ext == QStringLiteral("glsl") || ext == QStringLiteral("frag") ||
                                   ext == QStringLiteral("comp"));
            QString boundedSource;
            QString readErr;
            if (isShader &&
                !StudioSessionContext::readBoundedUtf8File(file, boundedSource, readErr)) {
                if (self->m_statusLabel && !readErr.isEmpty()) {
                    self->m_statusLabel->setText(readErr);
                }
                return;
            }
            if (self->m_previewViewport) {
                if (isShader) {
                    self->m_previewViewport->setPreviewShader(boundedSource);
                } else {
                    self->m_previewViewport->setPreviewImagePath(file);
                }
            }
            if (isShader && self->m_propertyInspector) {
                self->m_propertyInspector->loadShaderFile(file);
            }
            if (isShader && self->m_codeEditor) {
                self->m_codeEditor->openDocument(EditorLanguageMode::GlslEs310Fragment, -1, 0,
                                                 QFileInfo(file).fileName(), boundedSource);
            }
            if (!isShader && self->m_themeStudio) {
                if (self->m_previewViewport && !self->m_previewViewport->previewImage().isNull()) {
                    self->m_themeStudio->setWallpaperImage(
                        self->m_previewViewport->previewImage(), file);
                } else {
                    self->m_themeStudio->loadWallpaper(file);
                }
            }
            if (self->m_statusLabel) {
                self->m_statusLabel->setText(QStringLiteral("Loaded: %1").arg(file));
            }
        });
        dlg->open();
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
        auto *dlg = new QFileDialog(
            this, QStringLiteral("Open GLSL Shader"), QString(),
            QStringLiteral("GLSL Shader (*.glsl *.frag *.comp);;All Files (*)"));
        dlg->setFileMode(QFileDialog::ExistingFile);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        QPointer<MainWindow> self(this);
        connect(dlg, &QFileDialog::fileSelected, this, [self](const QString &file) {
            if (!self || file.isEmpty()) {
                return;
            }
            QString boundedSource;
            QString readErr;
            if (!StudioSessionContext::readBoundedUtf8File(file, boundedSource, readErr)) {
                if (self->m_statusLabel && !readErr.isEmpty()) {
                    self->m_statusLabel->setText(readErr);
                }
                return;
            }
            self->m_activeWallpaperPath = file;
            if (self->m_previewViewport) {
                self->m_previewViewport->setPreviewShader(boundedSource);
            }
            if (self->m_propertyInspector) {
                self->m_propertyInspector->loadShaderFile(file);
            }
            if (self->m_codeEditor) {
                self->m_codeEditor->openDocument(EditorLanguageMode::GlslEs310Fragment, -1, 0,
                                                 QFileInfo(file).fileName(), boundedSource);
            }
            if (self->m_statusLabel) {
                self->m_statusLabel->setText(QStringLiteral("Loaded shader: %1").arg(file));
            }
        });
        dlg->open();
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
    m_playPauseAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (m_previewViewport && m_previewViewport->parent() == this) {
        m_previewViewport->addAction(m_playPauseAction);
    }
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
    perspLabel->setObjectName(QStringLiteral("StudioToolbarLabel"));
    wwal::studio::tokens::applySecondaryTextRole(perspLabel);
    toolbar->addWidget(perspLabel);

    m_perspectiveCombo = new QComboBox(this);
    m_perspectiveCombo->setObjectName(QStringLiteral("PerspectiveComboBox"));
    m_perspectiveCombo->setFixedWidth(148);
    m_perspectiveCombo->setFixedHeight(wwal::studio::tokens::kTouchTargetMinPx);
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
    toolLabel->setObjectName(QStringLiteral("StudioToolbarLabel"));
    wwal::studio::tokens::applySecondaryTextRole(toolLabel);
    toolbar->addWidget(toolLabel);

    m_toolModeCombo = new QComboBox(this);
    m_toolModeCombo->setObjectName(QStringLiteral("ViewportToolModeCombo"));
    m_toolModeCombo->setFixedWidth(136);
    m_toolModeCombo->setFixedHeight(wwal::studio::tokens::kTouchTargetMinPx);
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
    m_pauseBtn->setFixedHeight(wwal::studio::tokens::kTouchTargetMinPx);
    connect(m_pauseBtn, &QPushButton::clicked, this, &MainWindow::handlePlayPauseToggle);
    toolbar->addWidget(m_pauseBtn);

    // Fullscreen Preview button
    auto *btnFullscreen = new QPushButton(QStringLiteral("Fullscreen"), this);
    btnFullscreen->setToolTip(QStringLiteral("Toggle fullscreen window (F11)"));
    btnFullscreen->setFixedHeight(wwal::studio::tokens::kTouchTargetMinPx);
    connect(btnFullscreen, &QPushButton::clicked, this, &MainWindow::handleFullscreenToggle);
    toolbar->addWidget(btnFullscreen);

    // Creator Packaging Wizard button
    auto *btnWizard = new QPushButton(QStringLiteral("Package..."), this);
    btnWizard->setToolTip(
        QStringLiteral("Create a signed .wwpkg wallpaper package (Ctrl+Shift+P)"));
    btnWizard->setFixedHeight(wwal::studio::tokens::kTouchTargetMinPx);
    connect(btnWizard, &QPushButton::clicked, this, &MainWindow::handleOpenPackagingWizard);
    toolbar->addWidget(btnWizard);

    // Spacer
    auto *spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    // Status label
    m_statusLabel = new QLabel(QStringLiteral("Ready"), this);
    m_statusLabel->setObjectName(QStringLiteral("studioStatusLabel"));
    wwal::studio::tokens::applySecondaryTextRole(m_statusLabel, QColor(0xcb, 0xd5, 0xe1));
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
    QFont appFont = QApplication::font();
    appFont.setPixelSize(wwal::studio::tokens::kFontSizeBasePx);
    QApplication::setFont(appFont);
    if (!dynamic_cast<const wwal::studio::tokens::StudioProxyStyle *>(QApplication::style())) {
        QApplication::setStyle(new wwal::studio::tokens::StudioProxyStyle());
    }
    QApplication::setPalette(wwal::studio::tokens::buildProStudioPalette());
}

void MainWindow::syncCachedSceneLayersToAudioWizard()
{
    if (m_sceneLayerTree) {
        if (auto *model = m_sceneLayerTree->findChild<SceneLayerTreeModel *>()) {
            m_cachedSceneLayers = model->nodes();
        }
    } else if (m_cachedSceneLayers.empty() && !m_lastSceneTreeJson.isEmpty()) {
        m_cachedSceneLayers = parseSceneLayerNodesJson(m_lastSceneTreeJson);
    }
    if (m_audioBindingWizard) {
        m_audioBindingWizard->setSceneLayers(m_cachedSceneLayers);
    }
}

void MainWindow::wireThemeStudioSignals(ThemeStudioWidget *theme)
{
    if (!theme) {
        return;
    }
    connect(theme, &ThemeStudioWidget::paletteChanged, this,
            [this](const waywal_palette_t &palette) {
                if (m_statusLabel) {
                    const QString hex =
                        QStringLiteral("#%1")
                            .arg(palette.source_color & 0x00FFFFFFu, 6, 16, QLatin1Char('0'))
                            .toUpper();
                    m_statusLabel->setText(QStringLiteral("Theme Palette: source %1 (seed #%2)")
                                               .arg(hex)
                                               .arg(palette.source_color_index));
                }
            });
    connect(theme, &ThemeStudioWidget::modeChanged, this, [this](waywal_theme_mode_t mode) {
        syncThemeStateToIpcOrActiveWallpaper(true);
        if (m_statusLabel) {
            m_statusLabel->setText(QStringLiteral("Theme Mode: %1")
                                       .arg(mode == WAYWAL_THEME_LIGHT ? QStringLiteral("Light")
                                                                       : QStringLiteral("Dark")));
        }
    });
    connect(theme, &ThemeStudioWidget::variantChanged, this,
            [this](waywal_scheme_variant_t variant) {
                syncThemeStateToIpcOrActiveWallpaper(false);
                if (m_statusLabel) {
                    m_statusLabel->setText(
                        QStringLiteral("Theme Variant: %1").arg(static_cast<int>(variant)));
                }
            });
    connect(theme, &ThemeStudioWidget::contrastChanged, this, [this](float contrast) {
        syncThemeStateToIpcOrActiveWallpaper(false);
        if (m_statusLabel) {
            m_statusLabel->setText(QStringLiteral("Theme Contrast: %1").arg(contrast, 0, 'f', 2));
        }
    });
    connect(theme, &ThemeStudioWidget::seedIndexChanged, this, [this](uint8_t index) {
        syncThemeStateToIpcOrActiveWallpaper(true);
        if (m_statusLabel) {
            m_statusLabel->setText(QStringLiteral("Theme Seed Index: %1").arg(index));
        }
    });
    connect(theme, &ThemeStudioWidget::statusMessageRequested, this, [this](const QString &msg) {
        if (m_statusLabel && !msg.isEmpty()) {
            m_statusLabel->setText(msg);
        }
    });
}

void MainWindow::wireWorkshopBrowserSignals(WorkshopBrowserWidget *workshop)
{
    if (!workshop) {
        return;
    }
    connect(workshop, &WorkshopBrowserWidget::wallpaperSelected, this,
            [this](uint64_t /*id*/, const QString &filePath) {
                applyWallpaperPath(filePath, resolveSelectedOutputsCsv());
            });
    connect(workshop, &WorkshopBrowserWidget::wallpaperApplyRequested, this,
            [this](uint64_t publishedFileId, const QString &targetOutput) {
                QString path;
                if (auto *idxMgr = catalogIndexManager()) {
                    if (const auto rec = idxMgr->getItem(publishedFileId); rec.has_value()) {
                        path = QString::fromStdString(rec->file_path);
                    }
                }
                if (path.isEmpty()) {
                    if (m_statusLabel) {
                        m_statusLabel->setText(
                            QStringLiteral("Workshop item %1 is not downloaded locally yet")
                                .arg(publishedFileId));
                    }
                    return;
                }
                const QString targetSpec = targetOutput.trimmed().isEmpty()
                                               ? resolveSelectedOutputsCsv()
                                               : normalizeTargetOutputSpec(targetOutput);
                applyWallpaperPath(path, targetSpec);
            });
}

void MainWindow::wireOpenHubBrowserSignals(OpenHubBrowserWidget *openHub)
{
    if (!openHub) {
        return;
    }
    connect(openHub, &OpenHubBrowserWidget::packageSelected, this,
            [this](const QString & /*pkgId*/, const QString &cid) {
                if (m_statusLabel) {
                    m_statusLabel->setText(QStringLiteral("Selected Hub Package: %1").arg(cid));
                }
            });
    connect(openHub, &OpenHubBrowserWidget::packageInstallRequested, this,
            [this](const QString &packageId, const QString &targetOutput) {
                installPackageToDaemon(packageId, targetOutput);
            });
}

void MainWindow::wireEffectLibrarySignals(EffectLibraryBrowserWidget *browser)
{
    if (!browser) {
        return;
    }
    connect(browser, &EffectLibraryBrowserWidget::effectInsertIntoGraphRequested, this,
            [this](we_builtin_effect_id_t effectId) {
                if (!m_nodeGraphEditor && m_layoutManager) {
                    m_layoutManager->realizeLazyPanel(QStringLiteral("NodeGraphEditor"));
                }
                if (m_nodeGraphEditor) {
                    m_nodeGraphEditor->addBuiltinEffectNode(effectId, QPointF(100.0, 100.0));
                }
            });
    connect(browser, &EffectLibraryBrowserWidget::effectInsertRequested, this,
            [this](uint32_t /*effectId*/) {
                if (m_layoutManager) {
                    m_layoutManager->setPanelVisible(QStringLiteral("NodeGraphEditor"), true);
                }
            });
    connect(browser, &EffectLibraryBrowserWidget::effectApplyToActiveSceneRequested, this,
            [this](we_builtin_effect_id_t effectId) {
                if (!m_nodeGraphEditor && m_layoutManager) {
                    m_layoutManager->realizeLazyPanel(QStringLiteral("NodeGraphEditor"));
                }
                if (m_nodeGraphEditor) {
                    m_nodeGraphEditor->addBuiltinEffectNode(effectId, QPointF(140.0, 140.0));
                    m_nodeGraphEditor->compileAndHotSwapDaemon();
                }
            });
    connect(
        browser, &EffectLibraryBrowserWidget::effectAppendToStackRequested, this,
        [this](uint32_t effectId) {
            if (m_statusLabel) {
                m_statusLabel->setText(
                    QStringLiteral("Applied built-in effect #%1 to active scene").arg(effectId));
            }
        });
    connect(browser, &EffectLibraryBrowserWidget::effectActivated, this,
            [this](we_builtin_effect_id_t /*effectId*/, const QString &slug) {
                if (m_statusLabel && !slug.isEmpty()) {
                    m_statusLabel->setText(QStringLiteral("Effect: %1").arg(slug));
                }
            });
}

void MainWindow::wireNodeGraphEditorSignals(NodeGraphEditorWidget *graph)
{
    if (!graph) {
        return;
    }
    connect(graph, qOverload<const QString &, uint32_t>(&NodeGraphEditorWidget::graphCompiled),
            this, [this](const QString &glslCode, uint32_t /*activePassMask*/) {
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

void MainWindow::wireCameraLightingSignals(CameraLightingInspectorWidget *camLights)
{
    if (!camLights) {
        return;
    }
    connect(camLights, &CameraLightingInspectorWidget::cameraOrLightsChanged, this,
            [this](const QString & /*output*/, const we_camera3d_t &cam,
                   const we_lighting_uniforms_t &lights) {
                if (m_previewViewport) {
                    QMatrix4x4 vpMat;
                    std::memcpy(vpMat.data(), cam.view_proj_matrix, 16 * sizeof(float));
                    m_previewViewport->setShaderUniform(QStringLiteral("g_ViewProjectionMatrix"),
                                                        vpMat);
                    m_previewViewport->setGizmoCameraAndLights(cam, lights);
                }
            });
    connect(camLights, &CameraLightingInspectorWidget::camera3DChanged, this,
            [this](const QString & /*output*/, const we_camera3d_t &cam) {
                if (m_previewViewport && m_cameraLightingInspector) {
                    QMatrix4x4 vpMat;
                    std::memcpy(vpMat.data(), cam.view_proj_matrix, 16 * sizeof(float));
                    m_previewViewport->setShaderUniform(QStringLiteral("g_ViewProjectionMatrix"),
                                                        vpMat);
                    m_previewViewport->setGizmoCameraAndLights(
                        cam, m_cameraLightingInspector->lighting());
                }
            });
    connect(camLights, &CameraLightingInspectorWidget::lightingChanged, this,
            [this](const QString & /*output*/, const we_lighting_uniforms_t &lights) {
                if (m_previewViewport && m_cameraLightingInspector) {
                    m_previewViewport->setGizmoCameraAndLights(m_cameraLightingInspector->camera(),
                                                               lights);
                }
            });
}

void MainWindow::wirePhysicsSpringSignals(PhysicsSpringInspectorWidget *phys)
{
    if (!phys) {
        return;
    }
    connect(phys, &PhysicsSpringInspectorWidget::physicsParamsChanged, this,
            [this](const QString & /*output*/, int32_t /*objectId*/, int32_t boneIndex,
                   const we_bone_sim_config_t &cfg, const we_wind_field_t & /*wind*/,
                   const we_cursor_force_t & /*cursor*/) {
                if (m_previewViewport && boneIndex >= 0) {
                    m_previewViewport->setSelectedPuppetBone(boneIndex);
                }
                if (m_statusLabel) {
                    m_statusLabel->setText(QStringLiteral("Physics Bone %1 (%2): damping=%3")
                                               .arg(boneIndex)
                                               .arg((cfg.flags & WE_BONE_PHYS_ENABLED) != 0u
                                                        ? QStringLiteral("XPBD")
                                                        : QStringLiteral("Off"))
                                               .arg(cfg.damping_ratio, 0, 'f', 2));
                }
            });
    connect(phys, &PhysicsSpringInspectorWidget::boneDriveModeChanged, this,
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

void MainWindow::wireInterPanelSignals()
{
    // 1. Connect VirtualizedGrid wallpaper selection to Preview Viewport, Inspector & ThemeStudio
    if (m_gridWidget) {
        connect(m_gridWidget, &VirtualizedGridWidget::wallpaperSelected, this,
                [this](const wwal::studio::CatalogItem &item) {
                    const QString path = QString::fromStdString(item.path);
                    m_activeWallpaperPath = path;
                    if (m_statusLabel) {
                        m_statusLabel->setText(QStringLiteral("Selected: %1").arg(path));
                    }
                    if (m_previewViewport) {
                        if (item.badge == wwal::studio::WallpaperBadge::Shader) {
                            QString shaderSrc;
                            QString readErr;
                            if (StudioSessionContext::readBoundedUtf8File(path, shaderSrc, readErr)) {
                                m_previewViewport->setPreviewShader(shaderSrc);
                            } else if (m_statusLabel && !readErr.isEmpty()) {
                                m_statusLabel->setText(readErr);
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
                        const QString previewPath =
                            !item.preview_path.empty() ? QString::fromStdString(item.preview_path)
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
                    applyWallpaperPath(QString::fromStdString(item.path),
                                       resolveSelectedOutputsCsv());
                });
        connect(
            m_gridWidget, &VirtualizedGridWidget::playlistSelectionChanged, this,
            [this](const std::vector<uint32_t> &selectedIds) {
                if (m_statusLabel && !selectedIds.empty()) {
                    m_statusLabel->setText(
                        QStringLiteral("Playlist selection: %1 item(s)").arg(selectedIds.size()));
                }
            });
        connect(
            m_gridWidget, &VirtualizedGridWidget::playlistAddRequested, this,
            [this](const std::vector<uint32_t> &addedIds) {
                if (m_gridWidget) {
                    m_playlistQueue = m_gridWidget->playlistQueue();
                } else {
                    m_playlistQueue.insert(m_playlistQueue.end(), addedIds.begin(), addedIds.end());
                }
                if (m_statusLabel) {
                    m_statusLabel->setText(QStringLiteral("Playlist updated: %1 queued (%2 added)")
                                               .arg(m_playlistQueue.size())
                                               .arg(addedIds.size()));
                }
            });
    }

    if (m_previewViewport) {
        connect(m_previewViewport, &GpuPreviewViewport::applyRequested, this,
                [this](const QString &monitorSpec) {
                    if (!m_activeWallpaperPath.isEmpty()) {
                        applyWallpaperPath(m_activeWallpaperPath, monitorSpec);
                    }
                });
        connect(m_previewViewport, &GpuPreviewViewport::monitorSelectionChanged, this,
                [this](const QString &monitorName) {
                    if (m_topologyBar && m_topologyBar->canvas() && !monitorName.isEmpty()) {
                        const QSignalBlocker topoBlocker(m_topologyBar->canvas());
                        m_topologyBar->canvas()->setSelectedMonitors({monitorName});
                    }
                    syncMonitorCatalogToPanels();
                    if (auto *ipc = ipcClient(); ipc && ipc->isConnected()) {
                        ipc->sendQuerySceneTree(normalizeTargetOutputSpec(monitorName));
                    }
                    if (m_statusLabel && !monitorName.isEmpty()) {
                        m_statusLabel->setText(
                            QStringLiteral("Active Output: %1").arg(monitorName));
                    }
                });
        connect(m_previewViewport, &GpuPreviewViewport::zoomChanged, this, [this](float newZoom) {
            if (m_statusLabel) {
                m_statusLabel->setText(
                    QStringLiteral("Viewport Zoom: %1%").arg(qRound(newZoom * 100.0f)));
            }
        });
        connect(m_previewViewport, &GpuPreviewViewport::panChanged, this,
                [this](const QPointF &newOffset) {
                    if (m_statusLabel) {
                        m_statusLabel->setText(
                            QStringLiteral("Viewport Pan: (%1, %2)")
                                .arg(newOffset.x(), 0, 'f', 0)
                                .arg(newOffset.y(), 0, 'f', 0));
                    }
                });
        connect(m_previewViewport, &GpuPreviewViewport::parallaxToggled, this,
                [this](bool enabled) {
                    if (m_statusLabel) {
                        m_statusLabel->setText(
                            QStringLiteral("Preview Parallax: %1")
                                .arg(enabled ? QStringLiteral("Enabled")
                                             : QStringLiteral("Disabled")));
                    }
                });
        connect(m_previewViewport, &GpuPreviewViewport::aspectRatioGuideChanged, this,
                [this](AspectRatioGuide guide) {
                    if (m_statusLabel) {
                        m_statusLabel->setText(
                            QStringLiteral("Safe Frame Guide: %1")
                                .arg(formatAspectRatioGuideLabel(guide)));
                    }
                });
    }

    if (m_topologyBar && m_topologyBar->canvas()) {
        auto *canvas = m_topologyBar->canvas();
        auto *gravityDebounceTimer = new QTimer(this);
        gravityDebounceTimer->setSingleShot(true);
        gravityDebounceTimer->setInterval(120);
        auto pendingGravityTargets = std::make_shared<QString>();
        connect(gravityDebounceTimer, &QTimer::timeout, this, [this, pendingGravityTargets]() {
            if (!m_activeWallpaperPath.isEmpty()) {
                applyWallpaperPath(m_activeWallpaperPath, *pendingGravityTargets);
            }
        });

        connect(canvas, &MonitorTopologyCanvas::monitorSelectionChanged, this,
                [this](const QStringList &selectedOutputs) {
                    syncMonitorCatalogToPanels();
                    if (auto *ipc = ipcClient(); ipc && ipc->isConnected()) {
                        ipc->sendQuerySceneTree(resolveSelectedOutputsCsv());
                    }
                    if (m_statusLabel) {
                        m_statusLabel->setText(
                            selectedOutputs.isEmpty()
                                ? QStringLiteral("Target Displays: All")
                                : QStringLiteral("Target Displays: %1")
                                      .arg(selectedOutputs.join(QStringLiteral(", "))));
                    }
                });
        connect(canvas, &MonitorTopologyCanvas::wallpaperDropped, this,
                [this](const QStringList &targetOutputs, const QString &wallpaperPath) {
                    const QString targetSpec = targetOutputs.isEmpty()
                                                   ? resolveSelectedOutputsCsv()
                                                   : targetOutputs.join(QLatin1Char(','));
                    applyWallpaperPath(wallpaperPath, targetSpec);
                });
        connect(canvas, &MonitorTopologyCanvas::scalingModeChanged, this,
                [this](const QStringList &targetOutputs, int mode) {
                    const QString targetSpec = targetOutputs.isEmpty()
                                                   ? resolveSelectedOutputsCsv()
                                                   : targetOutputs.join(QLatin1Char(','));
                    if (!m_activeWallpaperPath.isEmpty()) {
                        applyWallpaperPath(m_activeWallpaperPath, targetSpec);
                    } else if (m_statusLabel) {
                        m_statusLabel->setText(
                            QStringLiteral("Scaling Mode: %1")
                                .arg(StudioSessionContext::scalingModeCliName(
                                    static_cast<uint32_t>(std::max(0, mode)))));
                    }
                });
        connect(canvas, &MonitorTopologyCanvas::cropGravityChanged, this,
                [this, gravityDebounceTimer, pendingGravityTargets](
                    const QStringList &targetOutputs, float gx, float gy) {
                    *pendingGravityTargets = targetOutputs.isEmpty()
                                                 ? resolveSelectedOutputsCsv()
                                                 : targetOutputs.join(QLatin1Char(','));
                    if (m_statusLabel) {
                        m_statusLabel->setText(QStringLiteral("Crop Gravity: (%1, %2)")
                                                   .arg(gx, 0, 'f', 2)
                                                   .arg(gy, 0, 'f', 2));
                    }
                    if (!m_activeWallpaperPath.isEmpty()) {
                        gravityDebounceTimer->start();
                    }
                });
        connect(canvas, &MonitorTopologyCanvas::desktopSpanModeToggled, this,
                [this](bool enabled) {
                    syncMonitorCatalogToPanels();
                    if (!m_activeWallpaperPath.isEmpty()) {
                        applyWallpaperPath(m_activeWallpaperPath,
                                           enabled ? QStringLiteral("*")
                                                   : resolveSelectedOutputsCsv());
                    } else if (m_statusLabel) {
                        m_statusLabel->setText(QStringLiteral("Desktop Span Mode: %1")
                                                   .arg(enabled ? QStringLiteral("Enabled")
                                                                : QStringLiteral("Per-Display")));
                    }
                });
        connect(canvas, &MonitorTopologyCanvas::spanRectsCalculated, this,
                [this](const QMap<QString, QRectF> &normalizedSpanRects) {
                    if (m_statusLabel && !normalizedSpanRects.isEmpty() && m_topologyBar &&
                        m_topologyBar->canvas() && m_topologyBar->canvas()->isDesktopSpanMode()) {
                        m_statusLabel->setText(
                            QStringLiteral("Span Geometry: %1 outputs calibrated")
                                .arg(normalizedSpanRects.size()));
                    }
                });
        connect(canvas, &MonitorTopologyCanvas::outputsRefreshed, this, [this](int count) {
            syncMonitorCatalogToPanels();
            if (m_statusLabel) {
                m_statusLabel->setText(QStringLiteral("Detected %1 display output(s)").arg(count));
            }
        });
    }

    // 2. Connect Cubic Bezier easing and transition config changes
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
        connect(m_bezierEditor, &CubicBezierEditorWidget::transitionConfigChanged, this,
                [this](const TransitionConfig &cfg) {
                    if (m_statusLabel) {
                        m_statusLabel->setText(
                            QStringLiteral("Transition: %1 (%2 ms @ %3 FPS)")
                                .arg(StudioSessionContext::transitionTypeCliName(cfg.type))
                                .arg(cfg.durationMs)
                                .arg(cfg.fps));
                    }
                });
        connect(m_bezierEditor, &CubicBezierEditorWidget::presetApplied, this,
                [this](const QString &presetName) {
                    if (m_statusLabel && !presetName.isEmpty()) {
                        m_statusLabel->setText(
                            QStringLiteral("Easing Preset: %1").arg(presetName));
                    }
                });
    }

    // 3. Connect Property Inspector signals
    if (m_propertyInspector) {
        connect(m_propertyInspector, &PropertyInspectorWidget::statusMessage, this,
                [this](const QString &msg) {
                    if (m_statusLabel) {
                        m_statusLabel->setText(msg);
                    }
                });
    }

    if (m_footerBar) {
        connect(m_footerBar, &StudioFooterBar::audioSensitivityGainChanged, this,
                [this](float gain) {
                    m_audioVolume = gain;
                    if (auto *ipc = ipcClient(); ipc && ipc->isConnected()) {
                        ipc->sendAudioPowerConfig(m_audioVolume, m_muteOnAudio ? 1 : 0, -1,
                                                  m_batteryPolicy);
                    }
                    if (m_statusLabel) {
                        m_statusLabel->setText(
                            QStringLiteral("Audio Gain: %1%").arg(qRound(gain * 100.0f)));
                    }
                });
        connect(m_footerBar, &StudioFooterBar::muteOnAudioToggled, this, [this](bool enabled) {
            m_muteOnAudio = enabled;
            if (auto *ipc = ipcClient(); ipc && ipc->isConnected()) {
                ipc->sendAudioPowerConfig(m_audioVolume, m_muteOnAudio ? 1 : 0, -1,
                                          m_batteryPolicy);
            }
            if (m_statusLabel) {
                m_statusLabel->setText(QStringLiteral("Mute On Audio: %1")
                                           .arg(enabled ? QStringLiteral("On")
                                                        : QStringLiteral("Off")));
            }
        });
        connect(m_footerBar, &StudioFooterBar::fpsCapChanged, this, [this](uint32_t fps) {
            if (m_previewViewport && fps > 0u) {
                m_previewViewport->setTargetAnimationFps(
                    static_cast<int>(std::clamp(fps, 15u, 360u)));
            }
            if (m_statusLabel) {
                m_statusLabel->setText(fps == 0u
                                           ? QStringLiteral("FPS Cap: VSync")
                                           : QStringLiteral("FPS Cap: %1").arg(fps));
            }
        });
        connect(m_footerBar, &StudioFooterBar::batteryPolicyChanged, this,
                [this](waywal_battery_policy_t policy) {
                    const uint32_t oldPolicy = m_batteryPolicy;
                    const uint32_t newPolicy = static_cast<uint32_t>(policy);
                    auto applyPolicy = [this](uint32_t pol) {
                        m_batteryPolicy = static_cast<uint8_t>(std::min<uint32_t>(pol, 2u));
                        if (auto *ipc = ipcClient(); ipc && ipc->isConnected()) {
                            ipc->sendAudioPowerConfig(m_audioVolume, m_muteOnAudio ? 1 : 0, -1,
                                                      m_batteryPolicy);
                        }
                        if (m_footerBar && m_footerBar->powerGovernor()) {
                            const QSignalBlocker blocker(m_footerBar->powerGovernor());
                            m_footerBar->powerGovernor()->setBatteryPolicy(
                                static_cast<waywal_battery_policy_t>(m_batteryPolicy));
                        }
                        if (m_statusLabel) {
                            const QString label = m_batteryPolicy == 0u
                                                      ? QStringLiteral("Ignore")
                                                  : m_batteryPolicy == 1u
                                                      ? QStringLiteral("Reduce FPS")
                                                      : QStringLiteral("Pause Video");
                            m_statusLabel->setText(QStringLiteral("Battery Policy: %1").arg(label));
                        }
                    };
                    if (auto *bus = commandBus(); bus && oldPolicy != newPolicy) {
                        bus->execute(std::make_unique<SetPowerPolicyCommand>(oldPolicy, newPolicy,
                                                                             applyPolicy));
                    } else {
                        applyPolicy(newPolicy);
                    }
                });
        connect(m_footerBar, &StudioFooterBar::autoPauseFlagsChanged, this,
                [this](bool pauseFullscreen, bool pauseMaximized, bool pauseOccluded) {
                    if (m_statusLabel) {
                        QStringList active;
                        if (pauseFullscreen) {
                            active << QStringLiteral("FS");
                        }
                        if (pauseMaximized) {
                            active << QStringLiteral("Max");
                        }
                        if (pauseOccluded) {
                            active << QStringLiteral("Occl");
                        }
                        m_statusLabel->setText(
                            QStringLiteral("Auto-Pause: %1")
                                .arg(active.isEmpty() ? QStringLiteral("None")
                                                      : active.join(QStringLiteral(", "))));
                    }
                });
        connect(m_footerBar, &StudioFooterBar::outputSelected, this,
                [this](const QString &outputName) {
                    if (outputName.isEmpty()) {
                        return;
                    }
                    if (m_topologyBar && m_topologyBar->canvas()) {
                        const QSignalBlocker topoBlocker(m_topologyBar->canvas());
                        m_topologyBar->canvas()->setSelectedMonitors({outputName});
                    }
                    syncMonitorCatalogToPanels();
                    if (auto *ipc = ipcClient(); ipc && ipc->isConnected()) {
                        ipc->sendQuerySceneTree(normalizeTargetOutputSpec(outputName));
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
                    if (auto *ipc = ipcClient()) {
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
                        ipc->sendSceneMutate(req);
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
                if (auto *bus = commandBus()) {
                    bus->execute(std::make_unique<SetLayerTransformCommand>(
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
                    const uint32_t compIdx = resolveActivePropertyComponentIndex(
                        m_propertyInspector->propertyRow(uniformName), baseline);
                    if (m_layoutManager) {
                        m_layoutManager->realizeLazyPanel(QStringLiteral("AudioBindingWizard"));
                        m_layoutManager->setPanelVisible(QStringLiteral("AudioBindingWizard"),
                                                         true);
                    }
                    if (m_audioBindingWizard) {
                        m_audioBindingWizard->preselectTargetUniform(
                            m_propertyInspector->targetObjectId(), uniformName, compIdx, baseline);
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
                    if (auto *ipc = ipcClient()) {
                        ipc->sendSetShaderCombo(output, objectId, passIndex, comboName, comboValue);
                    }
                });
    }

    if (auto *ipc = ipcClient()) {
        connect(ipc, &IpcClient::connectionStateChanged, this, [this](bool connected) {
            if (connected) {
                if (auto *activeIpc = ipcClient()) {
                    activeIpc->subscribeEvents();
                    activeIpc->requestOutputsJson();
                    activeIpc->sendQuerySceneTree(resolveSelectedOutputsCsv());
                }
            }
            if (m_statusLabel) {
                m_statusLabel->setText(connected ? QStringLiteral("Daemon IPC: Connected")
                                                 : QStringLiteral("Daemon IPC: Disconnected"));
            }
        });
        connect(ipc, &IpcClient::outputsJsonReceived, this, [this](const QString &json) {
            if (m_topologyBar && m_topologyBar->canvas() && !json.isEmpty()) {
                m_topologyBar->canvas()->updateOutputsFromJson(json.toUtf8());
            }
            syncMonitorCatalogToPanels();
        });
        connect(ipc, &IpcClient::sceneTreeQueried, this,
                [this](const waywal_ipc_resp_query_scene_tree_t &resp, const QString &treeJson) {
                    if (resp.status_code == 0 && !treeJson.isEmpty()) {
                        m_lastSceneTreeJson = treeJson;
                        m_cachedSceneLayers = parseSceneLayerNodesJson(treeJson);
                        if (m_sceneLayerTree) {
                            m_sceneLayerTree->loadFromSceneJson(treeJson);
                        }
                        syncCachedSceneLayersToAudioWizard();
                    }
                });
        connect(ipc, &IpcClient::particleConfigUpdated, this,
                [this](const waywal_ipc_resp_particle_config_t &resp) {
                    if (m_particleDesigner) {
                        m_particleDesigner->updateTelemetry(resp.alive_count, resp.max_count,
                                                            resp.sim_time_us);
                    }
                });
        connect(ipc, &IpcClient::physicsParamsUpdated, this,
                [this](const waywal_ipc_resp_set_physics_params_t &resp) {
                    if (m_physicsSpringInspector) {
                        m_physicsSpringInspector->updateSolverTelemetry(resp.active_sim_bones,
                                                                        resp.last_step_time_us);
                    }
                });
        connect(ipc, &IpcClient::telemetryReceived, this,
                [this](const TelemetrySnapshot &snapshot) {
                    waywal_telemetry_snapshot_t cSnap{};
                    cSnap.version = 1u;
                    cSnap.flags = WAYWAL_TELEMETRY_FORMAT_BINARY;
                    cSnap.timestamp_ns = snapshot.timestampNs;
                    cSnap.rss_kib = snapshot.rssKiB;
                    cSnap.pss_kib = snapshot.pssKiB;
                    cSnap.total_vram_kib = snapshot.totalVramKiB;
                    cSnap.gpu_active = snapshot.gpuActive ? 1u : 0u;
                    cSnap.on_battery = snapshot.onBattery ? 1u : 0u;
                    cSnap.battery_policy = snapshot.batteryPolicy;
                    cSnap.global_pause_mask = snapshot.globalPauseMask;

                    std::array<float, 64> bands{};
                    const size_t n = std::min<size_t>(snapshot.barkBands.size(), 64);
                    for (size_t i = 0; i < n; ++i) {
                        bands[i] = snapshot.barkBands[i];
                        cSnap.bark_bands[i] = snapshot.barkBands[i];
                    }
                    cSnap.rms_level = snapshot.rmsLevel;
                    cSnap.beat_pulse = snapshot.beatPulse;
                    cSnap.beat_onset = snapshot.beatOnset ? 1u : 0u;
                    cSnap.audio_active = snapshot.audioActive ? 1u : 0u;
                    cSnap.pipewire_active = snapshot.pipewireActive ? 1u : 0u;
                    cSnap.mute_on_audio = snapshot.muteOnAudio ? 1u : 0u;
                    cSnap.audio_volume = snapshot.audioVolume;

                    const size_t outCount =
                        std::min<size_t>(snapshot.outputs.size(), WAYWAL_MAX_TELEMETRY_OUTPUTS);
                    cSnap.num_outputs = static_cast<uint32_t>(outCount);
                    for (size_t i = 0; i < outCount; ++i) {
                        const OutputTelemetry &src = snapshot.outputs[i];
                        waywal_output_telemetry_t &dst = cSnap.outputs[i];
                        const QByteArray nameUtf8 = src.name.toUtf8();
                        const size_t copyLen = std::min<size_t>(
                            static_cast<size_t>(nameUtf8.size()), sizeof(dst.name) - 1u);
                        if (copyLen > 0) {
                            std::copy_n(nameUtf8.constData(), copyLen, dst.name);
                        }
                        dst.name[copyLen] = '\0';
                        dst.scanout_fps = src.scanoutFps;
                        dst.last_render_us = src.lastRenderUs;
                        dst.vram_kib = src.vramKiB;
                        for (size_t b = 0; b < 4; ++b) {
                            dst.bo_states[b] = src.boStates[b];
                        }
                        dst.engine_kind = src.engineKind;
                        dst.is_paused = src.isPaused ? 1u : 0u;
                        dst.pause_mask = src.pauseMask;
                        dst.width = src.width;
                        dst.height = src.height;
                        dst.logical_width = src.logicalWidth;
                        dst.logical_height = src.logicalHeight;
                        dst.scale_120 = static_cast<int32_t>(std::llround(src.scale * 120.0));
                    }

                    if (m_footerBar) {
                        m_footerBar->updateTelemetry(cSnap);
                    }
                    if (!m_audioBindingWizard) {
                        return;
                    }
                    float bassEnergy = 0.0f;
                    for (size_t i = 0; i < 7; ++i) {
                        bassEnergy += bands[i];
                    }
                    bassEnergy /= 7.0f;
                    const float dtSec = m_session ? m_session->sampleAudioTelemetryDeltaSec(
                                                        snapshot.audioActive ||
                                                        snapshot.pipewireActive)
                                                  : (1.0f / 60.0f);
                    m_audioBindingWizard->updateLiveBarkTelemetry(
                        bands, snapshot.rmsLevel, bassEnergy, snapshot.beatPulse, dtSec);
                });
        connect(ipc, &IpcClient::scenePkgExported, this,
                [this](const waywal_ipc_resp_export_scene_pkg_t &resp) {
                    PendingSceneExport pending{};
                    if (!m_session || !m_session->popPendingExport(pending) ||
                        pending.stagePath.isEmpty() || pending.destPath.isEmpty()) {
                        return;
                    }
                    const QString &stagePath = pending.stagePath;
                    const QString &destPath = pending.destPath;
                    const bool isPkgFile = pending.isPkgFile;

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
                                srcFs, tmpFs, std::filesystem::copy_options::overwrite_existing,
                                ec);
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
        connect(m_previewViewport, &GpuPreviewViewport::contourPinMoved, this,
                [this](int32_t objectId, uint32_t pinIndex, double px, double py) {
                    if (m_layoutManager && !m_puppetMeshPanel) {
                        m_layoutManager->realizeLazyPanel(QStringLiteral("PuppetMeshPanel"));
                    }
                    if (!m_puppetMeshPanel) {
                        return;
                    }
                    PuppetMeshSnapshot oldSnap = m_cachedPuppetMesh;
                    oldSnap.objectId = objectId >= 0 ? objectId : m_cachedPuppetMesh.objectId;
                    oldSnap.pins = m_puppetMeshPanel->pins();
                    if (pinIndex >= oldSnap.pins.size()) {
                        return;
                    }
                    PuppetMeshSnapshot newSnap = oldSnap;
                    newSnap.pins[pinIndex].px = px;
                    newSnap.pins[pinIndex].py = py;

                    auto applyPins = [this, pinIndex](const PuppetMeshSnapshot &snap) {
                        if (snap.objectId >= 0 && m_cachedPuppetMesh.objectId == snap.objectId) {
                            m_cachedPuppetMesh.pins = snap.pins;
                        }
                        if (m_puppetMeshPanel) {
                            CommandBus *savedBus = m_puppetMeshPanel->commandBus();
                            const QSignalBlocker panelBlocker(m_puppetMeshPanel.data());
                            m_puppetMeshPanel->setCommandBus(nullptr);
                            m_puppetMeshPanel->onClearPinsClicked();
                            for (const we_cdt_pin_t &p : snap.pins) {
                                m_puppetMeshPanel->addConstraintPin(p.px, p.py);
                            }
                            m_puppetMeshPanel->setCommandBus(savedBus);
                        }
                        if (m_previewViewport &&
                            (snap.objectId < 0 ||
                             m_previewViewport->puppetObjectId() == snap.objectId)) {
                            m_previewViewport->setPuppetContourPins(snap.pins,
                                                                    static_cast<int32_t>(pinIndex));
                        }
                    };
                    if (auto *bus = commandBus()) {
                        bus->execute(std::make_unique<MeshEditCommand>(
                            QStringLiteral("Move Contour Pin #%1").arg(pinIndex),
                            std::move(oldSnap), std::move(newSnap), std::move(applyPins)));
                    } else {
                        applyPins(newSnap);
                    }
                    if (m_statusLabel) {
                        m_statusLabel->setText(QStringLiteral("Moved contour pin #%1 (%2, %3)")
                                                   .arg(pinIndex)
                                                   .arg(px, 0, 'f', 1)
                                                   .arg(py, 0, 'f', 1));
                    }
                });
        connect(m_previewViewport, &GpuPreviewViewport::contourPinSelected, this,
                [this](int32_t objectId, int32_t pinIndex) {
                    if (m_puppetMeshPanel) {
                        if (auto *pinTree = m_puppetMeshPanel->findChild<QTreeWidget *>()) {
                            if (pinIndex >= 0 && pinIndex < pinTree->topLevelItemCount()) {
                                const QSignalBlocker treeBlocker(pinTree);
                                pinTree->setCurrentItem(pinTree->topLevelItem(pinIndex));
                            } else {
                                pinTree->clearSelection();
                            }
                        }
                    }
                    if (m_statusLabel && pinIndex >= 0) {
                        m_statusLabel->setText(QStringLiteral("Selected contour pin #%1 (object %2)")
                                                   .arg(pinIndex)
                                                   .arg(objectId));
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

        connect(
            m_previewViewport, &GpuPreviewViewport::puppetBonePivotChanged, this,
            [this](int32_t /*objectId*/, int32_t boneIndex, const QPointF &newLocalPos,
                   float newAngleZRad, bool committed) {
                if (!committed) {
                    if (m_statusLabel && boneIndex >= 0) {
                        m_statusLabel->setText(
                            QStringLiteral("Bone #%1 pivot: (%2, %3) @ %4 rad")
                                .arg(boneIndex)
                                .arg(newLocalPos.x(), 0, 'f', 1)
                                .arg(newLocalPos.y(), 0, 'f', 1)
                                .arg(newAngleZRad, 0, 'f', 2));
                    }
                    return;
                }
                if (m_layoutManager && !m_boneRiggingPanel) {
                    m_layoutManager->realizeLazyPanel(QStringLiteral("BoneRiggingPanel"));
                }
                if (!m_boneRiggingPanel || boneIndex < 0) {
                    return;
                }
                const BoneHierarchySnapshot &currentRig = m_boneRiggingPanel->rigSnapshot();
                if (static_cast<size_t>(boneIndex) >= currentRig.bones.size()) {
                    return;
                }

                BoneHierarchySnapshot oldRig = currentRig;
                BoneHierarchySnapshot newRig = currentRig;
                we_mdl_bone_t &nb = newRig.bones[static_cast<size_t>(boneIndex)];
                if (nb.parent != 0xFFFFFFFFu && nb.parent < newRig.bones.size()) {
                    std::vector<std::array<float, 16>> localMats(newRig.bones.size());
                    for (size_t i = 0; i < newRig.bones.size(); ++i) {
                        std::copy_n(newRig.bones[i].local_trans, 16, localMats[i].data());
                    }
                    const auto worldMats = resolveBoneWorldMatrices(newRig.bones, localMats);
                    QMatrix4x4 parentMat;
                    std::memcpy(parentMat.data(), worldMats[nb.parent].data(), 16 * sizeof(float));
                    const QVector3D localVec = parentMat.inverted().map(
                        QVector3D(static_cast<float>(newLocalPos.x()),
                                  static_cast<float>(newLocalPos.y()), nb.bind_pos[2]));
                    nb.bind_pos[0] = localVec.x();
                    nb.bind_pos[1] = localVec.y();
                } else {
                    nb.bind_pos[0] = static_cast<float>(newLocalPos.x());
                    nb.bind_pos[1] = static_cast<float>(newLocalPos.y());
                }
                nb.bind_angle[2] = newAngleZRad;
                composeBoneLocalMat4(nb.bind_pos, nb.bind_angle, nb.bind_scale, nb.local_trans);

                auto applyRig = [this, boneIndex](const BoneHierarchySnapshot &snap) {
                    m_cachedPuppetRig = snap;
                    m_hasCachedPuppetRig = true;
                    if (m_boneRiggingPanel) {
                        hydrateBoneRiggingPanelFromSnapshot(m_boneRiggingPanel, snap, commandBus(),
                                                            ipcClient(), m_syncingBoneDriveMode,
                                                            boneIndex);
                    }
                    if (m_previewViewport &&
                        (snap.objectId < 0 ||
                         m_previewViewport->puppetObjectId() == snap.objectId)) {
                        m_previewViewport->setPuppetSkeleton(snap, boneIndex);
                    }
                };
                if (auto *bus = commandBus()) {
                    bus->execute(std::make_unique<BoneEditCommand>(
                        QStringLiteral("Transform Bone #%1").arg(boneIndex), std::move(oldRig),
                        std::move(newRig), std::move(applyRig)));
                } else {
                    applyRig(newRig);
                }
            });

        connect(m_previewViewport, &GpuPreviewViewport::puppetIkTargetChanged, this,
                [this](int32_t objectId, int32_t chainIndex, const QVector3D &targetWorld,
                       bool committed) {
                    auto *ipc = ipcClient();
                    if (ipc && objectId >= 0 && chainIndex >= 0) {
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
                        ipc->sendPuppetPreviewIk(req);
                    }
                    if (committed && m_statusLabel) {
                        m_statusLabel->setText(
                            QStringLiteral("IK chain %1 target updated").arg(chainIndex));
                    }
                });

        connect(m_previewViewport, &GpuPreviewViewport::puppetIkPoleChanged, this,
                [this](int32_t objectId, int32_t chainIndex, const QVector3D & /*poleWorld*/,
                       bool poleFlip, bool committed) {
                    auto *ipc = ipcClient();
                    if (ipc && objectId >= 0 && chainIndex >= 0) {
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
                        ipc->sendPuppetPreviewIk(req);
                    }
                    if (committed && m_statusLabel) {
                        m_statusLabel->setText(
                            QStringLiteral("IK chain %1 pole updated").arg(chainIndex));
                    }
                });

        connect(m_previewViewport, &GpuPreviewViewport::weightBrushDabBatchReady, this,
                [this](int32_t objectId, const std::vector<WeightBrushDab> &dabs) {
                    auto *ipc = ipcClient();
                    if (!ipc || objectId < 0 || dabs.empty()) {
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
                                ipc->sendPuppetPaintWeights(static_cast<uint32_t>(objectId), 1u,
                                                            syncEntries);
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
                auto *bus = commandBus();
                if (bus && (!preBlocks.empty() || !postBlocks.empty())) {
                    QPointer<GpuPreviewViewport> viewportPtr(m_previewViewport);
                    auto cmd = std::make_unique<WeightPaintStrokeCommand>(
                        objectId, QStringLiteral("Weight Paint Stroke"), preBlocks, postBlocks,
                        [this, viewportPtr](int32_t oid,
                                            const std::vector<VertexWeightBlockSnapshot> &blocks) {
                            if (!viewportPtr || oid < 0 || viewportPtr->puppetObjectId() != oid) {
                                return;
                            }
                            viewportPtr->applyVertexWeightBlocks(oid, blocks);
                            auto *activeIpc = ipcClient();
                            if (!activeIpc) {
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
                                activeIpc->sendPuppetPaintWeights(static_cast<uint32_t>(oid), 1u,
                                                                  syncEntries);
                            }
                        });
                    bus->execute(std::move(cmd));
                } else if (auto *ipc = ipcClient();
                           ipc && objectId >= 0 && !dirtyEntries.empty()) {
                    ipc->sendPuppetPaintWeights(static_cast<uint32_t>(objectId), 1u, dirtyEntries);
                }
            });

        connect(m_previewViewport, &GpuPreviewViewport::previewImageChanged, this,
                [this](const QImage &image) { syncSourceAlphaFromPreviewImage(image); });
    }

    if (auto *ipc = ipcClient()) {
        connect(ipc, &IpcClient::textureReadbackReceived, this,
                [this](const waywal_ipc_resp_readback_texture_t &resp, const QByteArray &data) {
                    if (m_pendingAlphaReadbackObjectIds.empty()) {
                        return;
                    }
                    const int32_t targetObjectId = m_pendingAlphaReadbackObjectIds.front();
                    m_pendingAlphaReadbackObjectIds.pop_front();
                    if (resp.status_code != 0u || resp.width == 0u || resp.height == 0u ||
                        resp.width > WE_CDT_MAX_IMAGE_DIM || resp.height > WE_CDT_MAX_IMAGE_DIM ||
                        data.isEmpty()) {
                        return;
                    }
                    const uint64_t pixelCount =
                        static_cast<uint64_t>(resp.width) * static_cast<uint64_t>(resp.height);
                    if (pixelCount == 0u || pixelCount > WE_CDT_MAX_IMAGE_PIXELS ||
                        static_cast<uint64_t>(data.size()) < pixelCount) {
                        return;
                    }
                    const uint32_t bpp =
                        static_cast<uint32_t>(static_cast<uint64_t>(data.size()) / pixelCount);
                    if (bpp != 1u && bpp != 2u && bpp != 4u) {
                        return;
                    }
                    std::vector<uint8_t> alpha(static_cast<size_t>(pixelCount));
                    const auto *src = reinterpret_cast<const uint8_t *>(data.constData());
                    for (uint64_t p = 0; p < pixelCount; ++p) {
                        if (bpp == 4u) {
                            alpha[p] = src[p * 4u + 3u];
                        } else if (bpp == 2u) {
                            alpha[p] = src[p * 2u + 1u];
                        } else {
                            alpha[p] = src[p];
                        }
                    }
                    CachedAlphaBuffer &entry = m_cachedObjectAlphas[targetObjectId];
                    entry.width = resp.width;
                    entry.height = resp.height;
                    entry.alpha = std::move(alpha);
                    entry.pushed = false;
                    if (m_puppetMeshPanel && m_activePuppetObjectId == targetObjectId &&
                        m_puppetMeshPanel->lastResult().vertex_count == 0u) {
                        m_puppetMeshPanel->setSourceAlpha(entry.alpha, entry.width, entry.height);
                        entry.pushed = true;
                    }
                });
        connect(ipc, &IpcClient::puppetMeshAckReceived, this,
                [this](const waywal_ipc_puppet_mesh_ack_t &resp) {
                    if (m_statusLabel) {
                        m_statusLabel->setText(
                            QStringLiteral("Puppet mesh synced: %1 verts, %2 tris")
                                .arg(resp.vertex_count)
                                .arg(resp.triangle_count));
                    }
                });
        connect(ipc, &IpcClient::puppetWeightsSolved, this,
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
        connect(ipc, &IpcClient::puppetIkPoseReceived, this,
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
            ipc, &IpcClient::puppetMdlExported, this,
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

void MainWindow::applyWallpaperPath(const QString &path, const QString &explicitMonitorSpec)
{
    if (path.isEmpty()) {
        return;
    }
    const QString prevActivePath = m_activeWallpaperPath;
    m_activeWallpaperPath = path;

    const QFileInfo fi(path);
    if (!fi.exists()) {
        if (m_statusLabel) {
            m_statusLabel->setText(QStringLiteral("Preview Active: %1").arg(path));
        }
        return;
    }

    WallpaperApplyRequest req;
    req.namespaceStr = m_options.namespaceStr;
    req.path = path;

    QString targetOutputs;
    if (m_topologyBar && m_topologyBar->canvas() &&
        m_topologyBar->canvas()->isDesktopSpanMode()) {
        targetOutputs = QStringLiteral("*");
    } else if (!explicitMonitorSpec.trimmed().isEmpty()) {
        targetOutputs = normalizeTargetOutputSpec(explicitMonitorSpec);
    } else {
        targetOutputs = resolveSelectedOutputsCsv();
    }
    req.targetOutputsCsv = targetOutputs;

    if (m_bezierEditor) {
        const TransitionConfig cfg = m_bezierEditor->transitionConfig();
        req.transitionType = cfg.type;
        req.transitionDurationMs = cfg.durationMs;
        req.transitionFps = cfg.fps;
        req.angleDeg = cfg.angleDeg;
        req.waveFreq = cfg.waveFreq;
        req.waveAmp = cfg.waveAmp;
        req.centerX = cfg.centerX;
        req.centerY = cfg.centerY;
        req.bezier = cfg.bezier;
        req.hasCustomBezier = cfg.hasCustomBezier;
    }

    if (m_topologyBar && m_topologyBar->canvas()) {
        auto *canvas = m_topologyBar->canvas();
        req.scalingMode = canvas->isDesktopSpanMode()
                              ? static_cast<uint32_t>(WAYWAL_SCALING_SPAN)
                              : static_cast<uint32_t>(canvas->currentScalingMode());
        QString gravMonitor = (targetOutputs != QStringLiteral("*"))
                                  ? targetOutputs.section(QLatin1Char(','), 0, 0).trimmed()
                                  : QString();
        if (gravMonitor.isEmpty() && !canvas->selectedMonitorNames().isEmpty()) {
            gravMonitor = canvas->selectedMonitorNames().first();
        }
        const QPointF grav = canvas->cropGravity(gravMonitor);
        req.cropGravityX = static_cast<float>(grav.x());
        req.cropGravityY = static_cast<float>(grav.y());
        req.hasCropGravity = true;
    }

    if (m_themeStudio) {
        req.hasThemeMode = true;
        req.themeLightMode = (m_themeStudio->themeMode() == WAYWAL_THEME_LIGHT);
        req.hasSourceColorIndex = true;
        req.sourceColorIndex = m_themeStudio->seedIndex();
    }
    req.batteryPolicy = StudioSessionContext::batteryPolicyCliName(m_batteryPolicy);

    const WallpaperApplyRequest oldReq =
        m_session ? m_session->lastAppliedRequest() : WallpaperApplyRequest{};
    const waywal_img_metadata_t oldMeta = StudioSessionContext::buildImgMetadata(oldReq);
    const waywal_img_metadata_t newMeta = StudioSessionContext::buildImgMetadata(req);

    const QString oldOutSpec = oldReq.targetOutputsCsv;
    const QString oldPath = oldReq.path;
    const uint32_t oldScaling = oldMeta.scaling_mode;
    const uint32_t oldTrans = oldMeta.transition_type;
    const uint32_t newScaling = newMeta.scaling_mode;
    const uint32_t newTrans = newMeta.transition_type;

    auto applyFn = [this, prevActivePath, oldOutSpec, oldPath, oldScaling, oldTrans, newScaling,
                    newTrans, path](const QString &outSpec, const waywal_img_metadata_t &meta,
                                    const QString &targetPath) {
        if (targetPath.isEmpty()) {
            m_activeWallpaperPath.clear();
            if (m_statusLabel) {
                m_statusLabel->setText(QStringLiteral("Cleared active wallpaper selection"));
            }
            return;
        }
        // ApplyWallpaperCommand stores a single m_output string; restore the prior target
        // display routing on undo when scaling, transition, or path differs from redo state.
        const bool isUndoState =
            !oldOutSpec.isEmpty() && targetPath == oldPath && meta.scaling_mode == oldScaling &&
            meta.transition_type == oldTrans &&
            (oldPath != path || oldScaling != newScaling || oldTrans != newTrans);
        const QString effectiveOutSpec = isUndoState ? oldOutSpec : outSpec;
        const WallpaperApplyRequest effReq = StudioSessionContext::requestFromMetadata(
            m_options.namespaceStr, effectiveOutSpec, meta, targetPath);
        const bool pathChanged = (targetPath != prevActivePath);

        IIpcClientService *activeIpc =
            m_session ? m_session->activeIpcService() : Services::instance().ipcClient();
        const bool useCliFallback =
            (activeIpc == nullptr) ||
            StudioSessionContext::requiresCliFallbackMediaPath(targetPath);

        bool dispatchedViaIpc = false;
        bool dispatchedViaCli = false;
        if (!useCliFallback && activeIpc != nullptr) {
            const WallpaperApplyOptions opts =
                StudioSessionContext::toWallpaperApplyOptions(effReq);
            activeIpc->setWallpaper(opts);
            dispatchedViaIpc = true;
        } else {
            // Non-image assets (video/shader/workshop) and IPC-less contexts dispatch via CLI.
            dispatchedViaCli = StudioSessionContext::dispatchWwalCli(
                StudioSessionContext::buildWallpaperApplyArgs(effReq));
        }

        if (m_session) {
            m_session->setLastAppliedRequest(effReq);
        }
        m_activeWallpaperPath = targetPath;

        if (pathChanged && m_previewViewport) {
            const QString lower = targetPath.toLower();
            if (lower.endsWith(QStringLiteral(".glsl")) ||
                lower.endsWith(QStringLiteral(".frag")) ||
                lower.endsWith(QStringLiteral(".comp")) ||
                lower.endsWith(QStringLiteral(".shader"))) {
                QString shaderSrc;
                QString readErr;
                if (StudioSessionContext::readBoundedUtf8File(targetPath, shaderSrc, readErr)) {
                    m_previewViewport->setPreviewShader(shaderSrc);
                }
            } else {
                m_previewViewport->setPreviewImagePath(targetPath);
            }
        }
        if (auto *ipc = ipcClient(); ipc && ipc->isConnected()) {
            ipc->sendQuerySceneTree(normalizeTargetOutputSpec(effectiveOutSpec));
        }
        if (m_statusLabel) {
            const QString fileName = QFileInfo(targetPath).fileName();
            const QString modeName = StudioSessionContext::scalingModeCliName(effReq.scalingMode);
            if (dispatchedViaIpc || dispatchedViaCli) {
                m_statusLabel->setText(
                    QStringLiteral("Applied Wallpaper (%1): %2").arg(modeName, fileName));
            } else {
                m_statusLabel->setText(
                    QStringLiteral("wwal CLI unavailable; updated preview for %1")
                        .arg(fileName));
            }
        }
    };

    if (auto *bus = commandBus()) {
        bus->execute(std::make_unique<ApplyWallpaperCommand>(
            targetOutputs, oldMeta, newMeta, oldReq.path, path, std::move(applyFn)));
    } else {
        applyFn(targetOutputs, newMeta, path);
    }
}

void MainWindow::installPackageToDaemon(const QString &packageIdOrPath, const QString &targetOutput)
{
    const QString resolvedPkgPath =
        resolveLocalWwpkgFile(packageIdOrPath, m_session ? m_session->downloadManager() : nullptr);
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

    auto *ipc = ipcClient();
    if (!ipc) {
        return;
    }

    QPointer<MainWindow> self(this);
    ipc->installWwpkg(
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
    if (auto *bus = commandBus(); bus && bus->canUndo()) {
        bus->undo();
    }
}

void MainWindow::handleRedo()
{
    if (auto *bus = commandBus(); bus && bus->canRedo()) {
        bus->redo();
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
    if (m_activePackagingWizard) {
        m_activePackagingWizard->raise();
        m_activePackagingWizard->activateWindow();
        return;
    }

    auto *wizard = new CreatorPackagingWizard(this);
    wizard->setAttribute(Qt::WA_DeleteOnClose);
    m_activePackagingWizard = wizard;
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
    auto *dlg = new QFileDialog(this, QStringLiteral("Export Scene Package"), m_activeWallpaperPath,
                                QStringLiteral("Scene Packages (*.pkg *.wwpkg);;All Files (*)"));
    dlg->setAcceptMode(QFileDialog::AcceptSave);
    dlg->setDefaultSuffix(QStringLiteral("pkg"));
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    QPointer<MainWindow> self(this);
    connect(dlg, &QFileDialog::fileSelected, this, [self](const QString &destPath) {
        if (self && !destPath.isEmpty()) {
            self->dispatchScenePackageExport(destPath);
        }
    });
    dlg->open();
}

void MainWindow::dispatchScenePackageExport(const QString &destPath)
{
    if (destPath.isEmpty()) {
        return;
    }

    const QString lowerDest = destPath.toLower();
    const QFileInfo srcInfo(m_activeWallpaperPath);
    const QString lowerSrc = m_activeWallpaperPath.toLower();
    auto *ipc = ipcClient();

    // Landlock-confined wwald can only write exports under the cache staging root,
    // never directly into user wallpaper or config directories.
    if (!lowerDest.endsWith(QStringLiteral(".wwpkg")) && ipc && ipc->isConnected() && m_session) {
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

        const QString outName =
            m_previewViewport ? m_previewViewport->selectedMonitor() : QStringLiteral("*");
        m_session->enqueuePendingExport(PendingSceneExport{stagePath, destPath, outName, isPkgFile});

        if (!ipc->sendExportScenePkg(outName, stagePath, exportFlags)) {
            m_session->discardLastPendingExport();
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

    if (m_activePackagingWizard) {
        m_activePackagingWizard->raise();
        m_activePackagingWizard->activateWindow();
        return;
    }
    auto *wizard = new CreatorPackagingWizard(this);
    wizard->setAttribute(Qt::WA_DeleteOnClose);
    m_activePackagingWizard = wizard;
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
        if (objectChanged) {
            m_previewViewport->setPuppetContourPreviewLoops({});
        }
        m_previewViewport->setPuppetObjectId(objectId);
    }
    if (m_puppetMeshPanel) {
        m_puppetMeshPanel->setTargetObjectId(objectId, m_activePuppetTexWidth,
                                             m_activePuppetTexHeight);
        hydratePuppetMeshPanel(m_puppetMeshPanel);
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

void MainWindow::syncSourceAlphaFromPreviewImage(const QImage &image)
{
    if (image.isNull()) {
        m_cachedPreviewAlpha = CachedAlphaBuffer{};
        return;
    }

    CachedAlphaBuffer extracted;
    if (!extractImageAlphaBuffer(image, extracted.width, extracted.height, extracted.alpha)) {
        return;
    }
    extracted.pushed = false;
    m_cachedPreviewAlpha = extracted;

    if (m_activePuppetObjectId >= 0) {
        CachedAlphaBuffer &objAlpha = m_cachedObjectAlphas[m_activePuppetObjectId];
        objAlpha = extracted;
        objAlpha.pushed = false;
    }

    if (m_puppetMeshPanel && m_puppetMeshPanel->lastResult().vertex_count == 0u) {
        m_puppetMeshPanel->setSourceAlpha(m_cachedPreviewAlpha.alpha, m_cachedPreviewAlpha.width,
                                          m_cachedPreviewAlpha.height);
        m_cachedPreviewAlpha.pushed = true;
        if (auto it = m_cachedObjectAlphas.find(m_activePuppetObjectId);
            it != m_cachedObjectAlphas.end()) {
            it->second.pushed = true;
        }
    }
}

void MainWindow::hydratePuppetMeshPanel(PuppetMeshPanel *panel)
{
    if (!panel || panel->lastResult().vertex_count > 0u) {
        return;
    }

    std::vector<we_mdl_vertex_t> verts;
    std::vector<uint16_t> indices;
    if (m_cachedPuppetMesh.objectId == m_activePuppetObjectId &&
        m_cachedPuppetMesh.vertices.size() >= 3u && m_cachedPuppetMesh.indices.size() >= 3u) {
        verts = m_cachedPuppetMesh.vertices;
        indices = m_cachedPuppetMesh.indices;
    } else if (m_previewViewport && m_previewViewport->puppetObjectId() == m_activePuppetObjectId &&
               m_previewViewport->gizmoOverlay().meshWireframe().vertices.size() >= 3u &&
               m_previewViewport->gizmoOverlay().meshWireframe().indices.size() >= 3u) {
        verts = m_previewViewport->gizmoOverlay().meshWireframe().vertices;
        indices = m_previewViewport->gizmoOverlay().meshWireframe().indices;
    }

    if (!verts.empty() && !indices.empty()) {
        we_puppet_t existing{};
        existing.vertex_count = static_cast<uint32_t>(verts.size());
        existing.vertices = verts.data();
        existing.index_count = static_cast<uint32_t>(indices.size());
        existing.triangle_count = static_cast<uint32_t>(indices.size() / 3u);
        existing.indices = indices.data();
        if (m_hasCachedPuppetRig && m_cachedPuppetRig.objectId == m_activePuppetObjectId &&
            !m_cachedPuppetRig.bones.empty()) {
            existing.bone_count = static_cast<uint32_t>(m_cachedPuppetRig.bones.size());
            existing.bones = m_cachedPuppetRig.bones.data();
        }
        panel->loadExistingMesh(&existing);
        return;
    }

    if (auto it = m_cachedObjectAlphas.find(m_activePuppetObjectId);
        it != m_cachedObjectAlphas.end() && !it->second.alpha.empty()) {
        if (!it->second.pushed) {
            panel->setSourceAlpha(it->second.alpha, it->second.width, it->second.height);
            it->second.pushed = true;
        }
        return;
    }

    if (!m_cachedPreviewAlpha.alpha.empty()) {
        if (!m_cachedPreviewAlpha.pushed) {
            panel->setSourceAlpha(m_cachedPreviewAlpha.alpha, m_cachedPreviewAlpha.width,
                                  m_cachedPreviewAlpha.height);
            m_cachedPreviewAlpha.pushed = true;
        }
        return;
    }

    if (m_previewViewport && !m_previewViewport->previewImage().isNull() &&
        extractImageAlphaBuffer(m_previewViewport->previewImage(), m_cachedPreviewAlpha.width,
                                m_cachedPreviewAlpha.height, m_cachedPreviewAlpha.alpha)) {
        panel->setSourceAlpha(m_cachedPreviewAlpha.alpha, m_cachedPreviewAlpha.width,
                              m_cachedPreviewAlpha.height);
        m_cachedPreviewAlpha.pushed = true;
    }
}

void MainWindow::syncWeightPaintBrushAndLocks(WeightPaintPanel *panel) const
{
    if (!panel || !m_previewViewport) {
        return;
    }
    if (const auto *boneList = panel->findChild<QListWidget *>()) {
        const int row = boneList->currentRow();
        if (row >= 0) {
            m_previewViewport->setWeightBrushActiveBone(static_cast<uint32_t>(row));
        }
    }
    m_previewViewport->setWeightBrushParameters(panel->activeBrushOp(), panel->brushRadiusPx(),
                                                panel->brushStrength(), panel->targetWeightValue(),
                                                panel->lockedBoneMask(), panel->brushFalloff());
    static const std::vector<uint64_t> emptyLocks;
    m_previewViewport->setWeightLockedVertices(
        m_activePuppetObjectId,
        panel->respectLockedVertices() ? panel->lockedVertexBits() : emptyLocks);
}

bool MainWindow::loadPuppetFromMdlFile(const QString &filePath, int32_t objectId)
{
    if (filePath.isEmpty()) {
        return false;
    }
    QFile f(filePath);
    constexpr qint64 kMaxMdlBytes = 64LL * 1024LL * 1024LL;
    if (!f.open(QIODevice::ReadOnly) || f.size() <= 0 || f.size() > kMaxMdlBytes) {
        if (m_statusLabel) {
            m_statusLabel->setText(QStringLiteral("Failed to open puppet MDL: %1").arg(filePath));
        }
        return false;
    }
    const QByteArray bytes = f.readAll();
    we_puppet_t *puppet = we_puppet_parse_cpu(reinterpret_cast<const uint8_t *>(bytes.constData()),
                                              static_cast<size_t>(bytes.size()));
    if (!puppet) {
        if (m_statusLabel) {
            m_statusLabel->setText(QStringLiteral("Failed to parse puppet MDL: %1").arg(filePath));
        }
        return false;
    }
    loadExistingPuppetIntoPanels(objectId, puppet);
    we_puppet_destroy(puppet);
    if (m_statusLabel) {
        m_statusLabel->setText(QStringLiteral("Loaded puppet MDL: %1").arg(filePath));
    }
    return true;
}

void MainWindow::loadExistingPuppetIntoPanels(int32_t objectId, const we_puppet_t *puppet,
                                              uint32_t texWidth, uint32_t texHeight)
{
    if (!puppet || objectId < 0) {
        return;
    }

    m_activePuppetObjectId = objectId;
    if (texWidth > 0u) {
        m_activePuppetTexWidth = texWidth;
    }
    if (texHeight > 0u) {
        m_activePuppetTexHeight = texHeight;
    }

    m_cachedPuppetMesh = PuppetMeshSnapshot{};
    m_cachedPuppetMesh.objectId = objectId;
    if (puppet->vertices && puppet->vertex_count > 0u) {
        const uint32_t vc = std::min<uint32_t>(puppet->vertex_count, WE_MDL_MAX_VERTICES);
        m_cachedPuppetMesh.vertices.assign(puppet->vertices, puppet->vertices + vc);
    }
    if (puppet->indices && puppet->index_count >= 3u) {
        const uint32_t ic = std::min<uint32_t>(puppet->index_count, WE_MDL_MAX_TRIANGLES * 3u);
        m_cachedPuppetMesh.indices.assign(puppet->indices, puppet->indices + ic);
    }

    m_cachedPuppetRig = BoneHierarchySnapshot{};
    m_cachedPuppetRig.objectId = objectId;
    m_cachedPuppetRig.driveModes.fill(WE_BONE_DRIVE_FK);
    if (puppet->rig_ctx) {
        const auto *rigCtx = static_cast<const we_rig_pose_ctx_t *>(puppet->rig_ctx);
        for (uint32_t b = 0u; b < WE_MDL_MAX_BONES; ++b) {
            m_cachedPuppetRig.driveModes[b] = rigCtx->bone_drive_modes[b];
        }
        const uint32_t chainCount =
            std::min<uint32_t>(rigCtx->ik_chain_count, WE_RIG_MAX_IK_CHAINS);
        if (chainCount > 0u) {
            m_cachedPuppetRig.ikChains.assign(rigCtx->ik_chains, rigCtx->ik_chains + chainCount);
        }
    }
    if (puppet->bones && puppet->bone_count > 0u) {
        const uint32_t bc = std::min<uint32_t>(puppet->bone_count, WE_MDL_MAX_BONES);
        m_cachedPuppetRig.bones.assign(puppet->bones, puppet->bones + bc);
        if (puppet->physics_ctx) {
            for (uint32_t b = 0u; b < bc; ++b) {
                if ((puppet->physics_ctx->configs[b].flags & WE_BONE_PHYS_ENABLED) != 0u) {
                    m_cachedPuppetRig.driveModes[b] = WE_BONE_DRIVE_XPBD;
                }
            }
        }
    }
    m_hasCachedPuppetRig = !m_cachedPuppetRig.bones.empty();

    if (m_previewViewport) {
        m_previewViewport->setPuppetContourPreviewLoops({});
        m_previewViewport->setPuppetObjectId(objectId);
        m_previewViewport->setPuppetMeshData(!m_cachedPuppetMesh.vertices.empty(),
                                             m_cachedPuppetMesh.vertices,
                                             m_cachedPuppetMesh.indices);
        m_previewViewport->setPuppetSkeleton(m_cachedPuppetRig,
                                             m_cachedPuppetRig.bones.empty() ? -1 : 0);
    }
    if (m_puppetMeshPanel) {
        m_puppetMeshPanel->setTargetObjectId(objectId, m_activePuppetTexWidth,
                                             m_activePuppetTexHeight);
        m_puppetMeshPanel->loadExistingMesh(puppet);
    }
    if (m_boneRiggingPanel) {
        hydrateBoneRiggingPanelFromSnapshot(m_boneRiggingPanel, m_cachedPuppetRig, commandBus(),
                                            ipcClient(), m_syncingBoneDriveMode,
                                            m_cachedPuppetRig.bones.empty() ? -1 : 0);
    }
    if (m_physicsSpringInspector) {
        const QScopedValueRollback<bool> guard(m_syncingBoneDriveMode, true);
        m_physicsSpringInspector->setTargetOutput(resolveSelectedOutputsCsv());
        m_physicsSpringInspector->loadPuppetSkeleton(puppet, objectId);
    }
    if (m_weightPaintPanel) {
        hydrateWeightPaintPanel(m_weightPaintPanel);
    }
    if (m_dopeSheetTimeline) {
        if (puppet->anims && puppet->anim_count > 0u) {
            m_dopeSheetTimeline->loadPuppetAnimations(puppet, objectId);
            m_timelineHydratedObjectId = objectId;
        } else {
            m_timelineHydratedObjectId = -1;
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
    syncWeightPaintBrushAndLocks(panel);
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
    if (auto *clipCombo =
            m_dopeSheetTimeline->findChild<QComboBox *>(QStringLiteral("timelineClipCombo"))) {
        const int idx = clipCombo->currentIndex();
        if (idx >= 0 && static_cast<size_t>(idx) < clips.size()) {
            activeClip = &clips[static_cast<size_t>(idx)];
        }
    } else {
        for (const auto &c : clips) {
            if (c.layerVisible && !c.channels.empty()) {
                activeClip = &c;
                break;
            }
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
    m_onionScratch.localMats.resize(boneCount);
    for (int delta = -m_onionBeforeFrames; delta <= m_onionAfterFrames; ++delta) {
        if (delta == 0) {
            continue;
        }
        const double sampleFrame = wrapSampleFrame(curFrame + static_cast<double>(delta));
        auto &localMats = m_onionScratch.localMats;
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

        resolveBoneWorldMatricesInto(m_cachedPuppetRig.bones, localMats, m_onionScratch.worldMats,
                                     m_onionScratch.visitState, m_onionScratch.dfsStack);
        const auto &worldMats = m_onionScratch.worldMats;
        std::vector<std::pair<QVector3D, QVector3D>> segments(boneCount);
        m_onionScratch.hasChild.assign(boneCount, 0u);
        for (size_t i = 0; i < boneCount; ++i) {
            segments[i].first = QVector3D(worldMats[i][12], worldMats[i][13], worldMats[i][14]);
        }
        for (size_t i = 0; i < boneCount; ++i) {
            const uint32_t p = m_cachedPuppetRig.bones[i].parent;
            if (p != 0xFFFFFFFFu && p < boneCount) {
                m_onionScratch.hasChild[p] = 1u;
                segments[p].second = segments[i].first;
            }
        }
        for (size_t i = 0; i < boneCount; ++i) {
            if (m_onionScratch.hasChild[i] == 0u) {
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
