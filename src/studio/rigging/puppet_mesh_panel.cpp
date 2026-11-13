// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/rigging/puppet_mesh_panel.hpp"

#include <QAbstractItemView>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPointer>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace waywal::studio {

namespace {

constexpr uint32_t kMaxConstraintPins = 4096u;
constexpr uint32_t kMinMeshVertices = 3u;
constexpr uint32_t kMaxMeshVertices = 65535u;
constexpr float kTargetMinAngleDeg = 25.0f;

// Owned by PuppetMeshPanel so m_lastResult pointers stay valid with a defaulted destructor.
class PuppetMeshStorage final : public QObject {
  public:
    explicit PuppetMeshStorage(QObject *parent = nullptr) : QObject(parent)
    {
        setObjectName(QStringLiteral("PuppetMeshStorage"));
    }

    std::vector<we_mdl_vertex_t> vertices;
    std::vector<uint16_t> indices;
    we_mesh_gen_cfg_t appliedConfig{};
    std::vector<we_cdt_pin_t> appliedPins;
    QLabel *triBadge{nullptr};
    QLabel *angleBadge{nullptr};
    QPushButton *extractBtn{nullptr};
    QPushButton *removePinBtn{nullptr};
    QPushButton *clearPinsBtn{nullptr};
};

[[nodiscard]] PuppetMeshStorage *ensureStorage(PuppetMeshPanel *panel)
{
    if (!panel) {
        return nullptr;
    }
    if (auto *existing = panel->findChild<QObject *>(QStringLiteral("PuppetMeshStorage"),
                                                     Qt::FindDirectChildrenOnly)) {
        return static_cast<PuppetMeshStorage *>(existing);
    }
    auto *storage = new PuppetMeshStorage(panel);
    we_mesh_gen_cfg_default(&storage->appliedConfig);
    return storage;
}

[[nodiscard]] const PuppetMeshStorage *findStorage(const PuppetMeshPanel *panel) noexcept
{
    if (!panel) {
        return nullptr;
    }
    const auto *existing = panel->findChild<const QObject *>(QStringLiteral("PuppetMeshStorage"),
                                                             Qt::FindDirectChildrenOnly);
    return static_cast<const PuppetMeshStorage *>(existing);
}

void computeMeshQuality(const std::vector<we_mdl_vertex_t> &vertices,
                        const std::vector<uint16_t> &indices, float &outMinAngleDeg,
                        float &outMeanQuality) noexcept
{
    outMinAngleDeg = 0.0f;
    outMeanQuality = 0.0f;

    const size_t triCount = indices.size() / 3u;
    if (triCount == 0u || vertices.size() < kMinMeshVertices) {
        return;
    }

    constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;
    constexpr double kTwoSqrt3 = 3.46410161513775458705;

    double minAngle = 180.0;
    double qualitySum = 0.0;
    size_t validTris = 0u;

    for (size_t t = 0u; t < triCount; ++t) {
        const uint16_t i0 = indices[t * 3u + 0u];
        const uint16_t i1 = indices[t * 3u + 1u];
        const uint16_t i2 = indices[t * 3u + 2u];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
            continue;
        }

        const double ax = vertices[i0].pos[0];
        const double ay = vertices[i0].pos[1];
        const double bx = vertices[i1].pos[0];
        const double by = vertices[i1].pos[1];
        const double cx = vertices[i2].pos[0];
        const double cy = vertices[i2].pos[1];

        const double abx = bx - ax;
        const double aby = by - ay;
        const double bcx = cx - bx;
        const double bcy = cy - by;
        const double cax = ax - cx;
        const double cay = ay - cy;

        const double la2 = abx * abx + aby * aby;
        const double lb2 = bcx * bcx + bcy * bcy;
        const double lc2 = cax * cax + cay * cay;
        const double lenSum2 = la2 + lb2 + lc2;
        if (la2 <= 1e-12 || lb2 <= 1e-12 || lc2 <= 1e-12 || lenSum2 <= 1e-12) {
            minAngle = 0.0;
            ++validTris;
            continue;
        }

        const double la = std::sqrt(la2);
        const double lb = std::sqrt(lb2);
        const double lc = std::sqrt(lc2);

        const double cosA = std::clamp((la2 + lc2 - lb2) / (2.0 * la * lc), -1.0, 1.0);
        const double cosB = std::clamp((la2 + lb2 - lc2) / (2.0 * la * lb), -1.0, 1.0);
        const double cosC = std::clamp((lb2 + lc2 - la2) / (2.0 * lb * lc), -1.0, 1.0);

        const double angA = std::acos(cosA) * kRadToDeg;
        const double angB = std::acos(cosB) * kRadToDeg;
        const double angC = std::acos(cosC) * kRadToDeg;

        minAngle = std::min({minAngle, angA, angB, angC});

        const double area2 = std::abs(abx * (-cay) - aby * (-cax));
        const double q = std::clamp((kTwoSqrt3 * area2) / lenSum2, 0.0, 1.0);
        qualitySum += q;
        ++validTris;
    }

    if (validTris > 0u) {
        outMinAngleDeg = static_cast<float>(minAngle);
        outMeanQuality = static_cast<float>(qualitySum / static_cast<double>(validTris));
    }
}

template <typename Client = IpcClient>
void dispatchPuppetSetMeshIpc(Client *client, const PuppetMeshSnapshot &snapshot)
{
    if (!client || snapshot.objectId < 0) {
        return;
    }

    const bool clearMesh = snapshot.vertices.empty() || snapshot.indices.empty();
    if (!clearMesh &&
        (snapshot.vertices.size() < kMinMeshVertices || snapshot.indices.size() < 3u)) {
        return;
    }

    const uint32_t objId = static_cast<uint32_t>(snapshot.objectId);
    // Clear requests pass flags = 0; active meshes normalize weights and preserve skeleton.
    const uint32_t flags = clearMesh ? 0u : 0x3u;
    const std::vector<we_mdl_vertex_t> emptyVerts;
    const std::vector<uint16_t> emptyIndices;
    const auto &verts = clearMesh ? emptyVerts : snapshot.vertices;
    const auto &indices = clearMesh ? emptyIndices : snapshot.indices;

    if constexpr (requires(Client *c) { c->sendPuppetSetMesh(objId, verts, indices, flags); }) {
        client->sendPuppetSetMesh(objId, verts, indices, flags);
    } else if constexpr (requires(Client *c) {
                             c->sendPuppetSetMesh(snapshot.objectId, verts, indices, flags);
                         }) {
        client->sendPuppetSetMesh(snapshot.objectId, verts, indices, flags);
    } else if constexpr (requires(Client *c, const waywal_ipc_puppet_set_mesh_t &req,
                                  const std::vector<waywal_ipc_mdl_vertex_t> &wireVerts) {
                             c->sendPuppetSetMesh(req, wireVerts, indices);
                         }) {
        waywal_ipc_puppet_set_mesh_t req{};
        req.object_id = objId;
        req.vertex_count = static_cast<uint32_t>(verts.size());
        req.triangle_count = static_cast<uint32_t>(indices.size() / 3u);
        req.flags = flags;
        std::vector<waywal_ipc_mdl_vertex_t> wireVerts(verts.size());
        for (size_t i = 0u; i < verts.size(); ++i) {
            std::memcpy(&wireVerts[i], &verts[i], sizeof(waywal_ipc_mdl_vertex_t));
        }
        client->sendPuppetSetMesh(req, wireVerts, indices);
    } else {
        (void)objId;
        (void)flags;
    }
}

void rebuildPinTreeUi(QTreeWidget *pinTree, const std::vector<we_cdt_pin_t> &pins)
{
    if (!pinTree) {
        return;
    }
    const QSignalBlocker blocker(pinTree);
    pinTree->clear();
    for (size_t i = 0u; i < pins.size(); ++i) {
        auto *item = new QTreeWidgetItem(pinTree);
        item->setText(0, QString::number(i));
        item->setText(1, QString::number(pins[i].px, 'f', 1));
        item->setText(2, QString::number(pins[i].py, 'f', 1));
        item->setText(3, (pins[i].flags & 1u) != 0u ? QObject::tr("Steiner") : QObject::tr("Soft"));
    }
}

[[nodiscard]] PuppetMeshSnapshot makePinSnapshot(int32_t objectId, const PuppetMeshStorage *st,
                                                 const we_mesh_gen_cfg_t &fallbackCfg,
                                                 const std::vector<we_cdt_pin_t> &pins)
{
    PuppetMeshSnapshot snap;
    snap.objectId = objectId;
    if (st) {
        snap.vertices = st->vertices;
        snap.indices = st->indices;
        snap.cdtConfig = st->vertices.empty() ? fallbackCfg : st->appliedConfig;
    } else {
        snap.cdtConfig = fallbackCfg;
    }
    snap.pins = pins;
    return snap;
}

void executePinEditCommand(PuppetMeshPanel *panel, int32_t &objectIdRef,
                           std::vector<we_cdt_pin_t> &pinsRef, QTreeWidget *pinTree,
                           CommandBus *bus, PuppetMeshStorage *storage,
                           const we_mesh_gen_cfg_t &cfg, const QString &cmdTitle,
                           std::vector<we_cdt_pin_t> nextPins, const QString &applyMsg)
{
    PuppetMeshSnapshot oldSnap = makePinSnapshot(objectIdRef, storage, cfg, pinsRef);
    PuppetMeshSnapshot newSnap = makePinSnapshot(objectIdRef, storage, cfg, nextPins);

    QPointer<PuppetMeshPanel> self(panel);
    QPointer<QTreeWidget> safeTree(pinTree);
    const int32_t targetObjectId = objectIdRef;
    const size_t newSize = nextPins.size();

    auto applyPins = [self, &objectIdRef, &pinsRef, safeTree, targetObjectId, newSize,
                      applyMsg](const PuppetMeshSnapshot &snap) {
        if (!self || objectIdRef != targetObjectId) {
            return;
        }
        pinsRef = snap.pins;
        if (auto *storagePtr = ensureStorage(self.data())) {
            storagePtr->appliedPins = snap.pins;
        }
        rebuildPinTreeUi(safeTree.data(), pinsRef);
        emit self->contourPinsChanged(objectIdRef, pinsRef);
        emit self->statusMessage(
            snap.pins.size() == newSize
                ? applyMsg
                : QObject::tr("Restored %1 constraint pin(s)").arg(snap.pins.size()));
    };

    if (bus) {
        bus->execute(std::make_unique<MeshEditCommand>(cmdTitle, std::move(oldSnap),
                                                       std::move(newSnap), std::move(applyPins)));
    } else {
        applyPins(newSnap);
    }
}

[[nodiscard]] QGroupBox *createStyledGroup(const QString &title, QWidget *parent)
{
    auto *group = new QGroupBox(title, parent);
    group->setObjectName(QStringLiteral("meshSectionGroup"));
    return group;
}

void configureDoubleSpin(QDoubleSpinBox *spin, double minVal, double maxVal, double step,
                         int decimals, double initialVal, const QString &suffix,
                         const QString &accName, const QString &tooltip)
{
    spin->setRange(minVal, maxVal);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    spin->setValue(initialVal);
    spin->setKeyboardTracking(false);
    if (!suffix.isEmpty()) {
        spin->setSuffix(suffix);
    }
    spin->setAccessibleName(accName);
    spin->setAccessibleDescription(tooltip);
    spin->setToolTip(tooltip);
}

} // namespace

PuppetMeshPanel::PuppetMeshPanel(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("PuppetMeshPanel"));
    setAccessibleName(tr("Mesh and Cutout"));
    setAccessibleDescription(
        tr("Alpha silhouette contour extraction, constraint pins, and Constrained Delaunay mesh "
           "generation"));
    setupUi();
    updateMeshStatsLabel();
}

void PuppetMeshPanel::setupUi()
{
    auto *storage = ensureStorage(this);

    setStyleSheet(QStringLiteral(
        "#PuppetMeshPanel { background-color: #12141a; color: #f1f5f9; }"
        "QScrollArea { background-color: transparent; }"
        "QGroupBox#meshSectionGroup { background-color: #181b22; border: 1px solid #2a2f3d; "
        "border-radius: 6px; margin-top: 10px; padding-top: 10px; font-weight: 600; "
        "color: #f1f5f9; }"
        "QGroupBox#meshSectionGroup::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; "
        "color: #8b9bb4; }"
        "QLabel { color: #cdd6f4; }"
        "QLabel#meshFieldLabel { color: #8b9bb4; font-size: 12px; }"
        "QDoubleSpinBox, QSpinBox, QPushButton { background-color: #181b22; color: #f1f5f9; "
        "border: 1px solid #2a2f3d; border-radius: 4px; padding: 4px 6px; }"
        "QDoubleSpinBox, QSpinBox { font-family: 'JetBrains Mono', monospace; font-size: 12px; "
        "background-color: #0b0c10; }"
        "QDoubleSpinBox:hover, QSpinBox:hover, QPushButton:hover { background-color: #222631; }"
        "QDoubleSpinBox:focus, QSpinBox:focus, QPushButton:focus, QCheckBox:focus, "
        "QTreeWidget:focus { border: 2px solid #38bdf8; outline: none; }"
        "QPushButton:disabled, QDoubleSpinBox:disabled, QSpinBox:disabled, QCheckBox:disabled { "
        "color: #8b9bb4; border-color: #222631; }"
        "QPushButton#generateMeshBtn { background-color: #0c4a6e; color: #e0f2fe; "
        "border: 1px solid #38bdf8; font-weight: 600; padding: 6px 10px; }"
        "QPushButton#generateMeshBtn:hover { background-color: #075985; }"
        "QTreeWidget { background-color: #0b0c10; color: #f1f5f9; border: 1px solid #2a2f3d; "
        "border-radius: 4px; alternate-background-color: #14171f; }"
        "QHeaderView::section { background-color: #181b22; color: #8b9bb4; "
        "border: none; border-bottom: 1px solid #2a2f3d; padding: 4px 6px; font-weight: 600; }"
        "QTreeWidget::item:selected { background-color: #1e3a5f; color: #f8fafc; }"
        "QCheckBox { color: #f1f5f9; spacing: 6px; }"));

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(4, 4, 4, 4);
    rootLayout->setSpacing(6);

    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto *container = new QWidget(scrollArea);
    auto *contentLayout = new QVBoxLayout(container);
    contentLayout->setContentsMargins(4, 4, 4, 4);
    contentLayout->setSpacing(8);

    // 1. Cutout & CDT Parameters
    auto *cdtGroup = createStyledGroup(tr("Alpha Cutout & CDT Quality"), container);
    auto *cdtLayout = new QGridLayout(cdtGroup);
    cdtLayout->setContentsMargins(8, 8, 8, 8);
    cdtLayout->setHorizontalSpacing(8);
    cdtLayout->setVerticalSpacing(6);

    we_mesh_gen_cfg_t defaults{};
    we_mesh_gen_cfg_default(&defaults);

    auto addRowLabel = [&](int row, const QString &text, const QString &tip) {
        auto *lbl = new QLabel(text, cdtGroup);
        lbl->setObjectName(QStringLiteral("meshFieldLabel"));
        lbl->setToolTip(tip);
        cdtLayout->addWidget(lbl, row, 0);
        return lbl;
    };

    m_alphaThreshSpin = new QDoubleSpinBox(cdtGroup);
    configureDoubleSpin(
        m_alphaThreshSpin, 1.0 / 255.0, 1.0, 0.02, 3, static_cast<double>(defaults.alpha_threshold),
        QString(), tr("Alpha Threshold"),
        tr("Normalized alpha cutoff [0.004..1.0] used to trace sub-pixel silhouette contours"));
    addRowLabel(0, tr("Alpha Threshold:"), m_alphaThreshSpin->toolTip());
    cdtLayout->addWidget(m_alphaThreshSpin, 0, 1);

    m_dilationPxSpin = new QDoubleSpinBox(cdtGroup);
    configureDoubleSpin(
        m_dilationPxSpin, 0.0, static_cast<double>(WE_CDT_MAX_DILATION_PX), 0.5, 1,
        static_cast<double>(defaults.dilation_px), QStringLiteral(" px"),
        tr("Dilation Padding Pixels"),
        tr("Separable alpha dilation padding in pixels [0.0..32.0] around semi-transparent edges"));
    addRowLabel(1, tr("Dilation Padding:"), m_dilationPxSpin->toolTip());
    cdtLayout->addWidget(m_dilationPxSpin, 1, 1);

    m_simplifyEpsSpin = new QDoubleSpinBox(cdtGroup);
    configureDoubleSpin(
        m_simplifyEpsSpin, 0.25, 64.0, 0.25, 2, static_cast<double>(defaults.simplify_eps_px),
        QStringLiteral(" px"), tr("TP-RDP Tolerance"),
        tr("Topology-preserving Ramer-Douglas-Peucker contour simplification tolerance in pixels"));
    addRowLabel(2, tr("RDP Tolerance:"), m_simplifyEpsSpin->toolTip());
    cdtLayout->addWidget(m_simplifyEpsSpin, 2, 1);

    m_interiorSpacingSpin = new QDoubleSpinBox(cdtGroup);
    configureDoubleSpin(
        m_interiorSpacingSpin, 4.0, 512.0, 2.0, 1, static_cast<double>(defaults.interior_spacing),
        QStringLiteral(" px"), tr("Poisson Steiner Spacing"),
        tr("Bridson Poisson-disk spacing in pixels [4.0..512.0] for interior Steiner vertices"));
    addRowLabel(3, tr("Steiner Spacing:"), m_interiorSpacingSpin->toolTip());
    cdtLayout->addWidget(m_interiorSpacingSpin, 3, 1);

    m_minAngleSpin = new QDoubleSpinBox(cdtGroup);
    configureDoubleSpin(
        m_minAngleSpin, static_cast<double>(WE_CDT_MIN_ANGLE_MIN_DEG),
        static_cast<double>(WE_CDT_MIN_ANGLE_MAX_DEG), 1.0, 1,
        static_cast<double>(defaults.min_angle_deg), QStringLiteral(" deg"),
        tr("Minimum Angle Target"),
        tr("Chew/Ruppert Delaunay quality target angle [5.0..32.0 deg] (>= 25 deg recommended)"));
    addRowLabel(4, tr("Min Angle Target:"), m_minAngleSpin->toolTip());
    cdtLayout->addWidget(m_minAngleSpin, 4, 1);

    m_maxVertsSpin = new QSpinBox(cdtGroup);
    m_maxVertsSpin->setRange(static_cast<int>(kMinMeshVertices),
                             static_cast<int>(kMaxMeshVertices));
    m_maxVertsSpin->setSingleStep(256);
    m_maxVertsSpin->setValue(
        std::clamp(static_cast<int>(defaults.max_vertices), 3, static_cast<int>(kMaxMeshVertices)));
    m_maxVertsSpin->setKeyboardTracking(false);
    m_maxVertsSpin->setAccessibleName(tr("Maximum Vertex Cap"));
    m_maxVertsSpin->setToolTip(
        tr("Hard vertex cap [3..65535] enforced during Steiner insertion and Chew/Ruppert "
           "refinement"));
    m_maxVertsSpin->setAccessibleDescription(m_maxVertsSpin->toolTip());
    addRowLabel(5, tr("Max Vertex Cap:"), m_maxVertsSpin->toolTip());
    cdtLayout->addWidget(m_maxVertsSpin, 5, 1);

    m_wireframeCheck = new QCheckBox(tr("Show CDT Wireframe Overlay"), cdtGroup);
    m_wireframeCheck->setAccessibleName(tr("Show CDT Wireframe Overlay"));
    m_wireframeCheck->setToolTip(
        tr("Overlay generated Constrained Delaunay mesh edges inside the viewport"));
    m_wireframeCheck->setAccessibleDescription(m_wireframeCheck->toolTip());
    connect(m_wireframeCheck, &QCheckBox::toggled, this,
            [this](bool checked) { emit wireframeOverlayRequested(checked, m_lastResult); });
    cdtLayout->addWidget(m_wireframeCheck, 6, 0, 1, 2);

    contentLayout->addWidget(cdtGroup);

    // 2. Interior Constraint Pins
    auto *pinGroup = createStyledGroup(tr("Interior Constraint Pins"), container);
    auto *pinLayout = new QVBoxLayout(pinGroup);
    pinLayout->setContentsMargins(8, 8, 8, 8);
    pinLayout->setSpacing(6);

    m_pinTree = new QTreeWidget(pinGroup);
    m_pinTree->setHeaderLabels({tr("#"), tr("X (px)"), tr("Y (px)"), tr("Constraint")});
    m_pinTree->setRootIsDecorated(false);
    m_pinTree->setAlternatingRowColors(true);
    m_pinTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_pinTree->setMinimumHeight(110);
    m_pinTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_pinTree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_pinTree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_pinTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_pinTree->setAccessibleName(tr("Interior Constraint Pins"));
    m_pinTree->setToolTip(
        tr("Hard Steiner points forced as vertices inside the Constrained Delaunay triangulation"));
    pinLayout->addWidget(m_pinTree);

    auto *pinBtnRow = new QHBoxLayout();
    pinBtnRow->setSpacing(6);

    storage->extractBtn = new QPushButton(tr("Preview Contours"), pinGroup);
    storage->extractBtn->setAccessibleName(tr("Preview Extracted Contours"));
    storage->extractBtn->setToolTip(
        tr("Run Marching Squares and TP-RDP contour extraction on the loaded alpha channel"));
    connect(storage->extractBtn, &QPushButton::clicked, this,
            &PuppetMeshPanel::onExtractContoursClicked);

    storage->removePinBtn = new QPushButton(tr("Remove Pin"), pinGroup);
    storage->removePinBtn->setAccessibleName(tr("Remove Selected Constraint Pin"));
    storage->removePinBtn->setToolTip(tr("Remove the currently selected Steiner constraint pin"));
    connect(storage->removePinBtn, &QPushButton::clicked, this, [this]() {
        if (!m_pinTree) {
            return;
        }
        auto *item = m_pinTree->currentItem();
        if (!item) {
            return;
        }
        const int idx = m_pinTree->indexOfTopLevelItem(item);
        if (idx >= 0) {
            removeConstraintPin(static_cast<uint32_t>(idx));
        }
    });

    storage->clearPinsBtn = new QPushButton(tr("Clear Pins"), pinGroup);
    storage->clearPinsBtn->setAccessibleName(tr("Clear All Constraint Pins"));
    storage->clearPinsBtn->setToolTip(tr("Remove all interior Steiner constraint pins"));
    connect(storage->clearPinsBtn, &QPushButton::clicked, this,
            &PuppetMeshPanel::onClearPinsClicked);

    pinBtnRow->addWidget(storage->extractBtn);
    pinBtnRow->addWidget(storage->removePinBtn);
    pinBtnRow->addWidget(storage->clearPinsBtn);
    pinLayout->addLayout(pinBtnRow);

    contentLayout->addWidget(pinGroup);

    // 3. Actions
    auto *actionRow = new QHBoxLayout();
    actionRow->setSpacing(6);

    m_generateBtn = new QPushButton(tr("Generate Cutout Mesh"), container);
    m_generateBtn->setObjectName(QStringLiteral("generateMeshBtn"));
    m_generateBtn->setAccessibleName(tr("Generate Cutout Mesh"));
    m_generateBtn->setToolTip(
        tr("Generate Constrained Delaunay cutout mesh locally and push undoable snapshot"));
    connect(m_generateBtn, &QPushButton::clicked, this,
            &PuppetMeshPanel::onGenerateCutoutMeshClicked);

    m_exportBtn = new QPushButton(tr("Export .mdl..."), container);
    m_exportBtn->setAccessibleName(tr("Export Puppet MDL"));
    m_exportBtn->setToolTip(tr("Export the rigged puppet mesh, skeleton, and animations to .mdl"));
    connect(m_exportBtn, &QPushButton::clicked, this, &PuppetMeshPanel::onExportMdlClicked);

    actionRow->addWidget(m_generateBtn, 2);
    actionRow->addWidget(m_exportBtn, 1);
    contentLayout->addLayout(actionRow);

    contentLayout->addStretch(1);
    scrollArea->setWidget(container);
    rootLayout->addWidget(scrollArea, 1);

    // 4. Telemetry Badges & Status Summary
    auto *badgeRow = new QHBoxLayout();
    badgeRow->setContentsMargins(0, 0, 0, 0);
    badgeRow->setSpacing(6);

    storage->triBadge = new QLabel(tr("TRIS: 0"), this);
    storage->triBadge->setAlignment(Qt::AlignCenter);
    storage->triBadge->setAccessibleName(tr("Generated Triangle Count Badge"));
    storage->triBadge->setToolTip(tr("Total triangles in active puppet mesh"));

    storage->angleBadge = new QLabel(tr("MIN ANGLE: -- deg"), this);
    storage->angleBadge->setAlignment(Qt::AlignCenter);
    storage->angleBadge->setAccessibleName(tr("Minimum Triangle Angle Telemetry Badge"));
    storage->angleBadge->setToolTip(
        tr("Achieved minimum interior triangle angle (target >= 25.0 deg)"));

    badgeRow->addWidget(storage->triBadge, 1);
    badgeRow->addWidget(storage->angleBadge, 1);
    rootLayout->addLayout(badgeRow);

    m_statsLabel = new QLabel(this);
    m_statsLabel->setWordWrap(true);
    m_statsLabel->setStyleSheet(
        QStringLiteral("QLabel { font-family: 'JetBrains Mono', monospace; font-size: 11px; "
                       "color: #8b9bb4; background-color: #181b22; border: 1px solid #2a2f3d; "
                       "border-radius: 4px; padding: 6px; }"));
    m_statsLabel->setAccessibleName(tr("Mesh Generation Telemetry"));
    m_statsLabel->setToolTip(tr("Active mesh vertex count, triangle count, and Delaunay quality"));
    rootLayout->addWidget(m_statsLabel);

    setTabOrder(m_alphaThreshSpin, m_dilationPxSpin);
    setTabOrder(m_dilationPxSpin, m_simplifyEpsSpin);
    setTabOrder(m_simplifyEpsSpin, m_interiorSpacingSpin);
    setTabOrder(m_interiorSpacingSpin, m_minAngleSpin);
    setTabOrder(m_minAngleSpin, m_maxVertsSpin);
    setTabOrder(m_maxVertsSpin, m_wireframeCheck);
    setTabOrder(m_wireframeCheck, m_pinTree);
    setTabOrder(m_pinTree, storage->extractBtn);
    setTabOrder(storage->extractBtn, storage->removePinBtn);
    setTabOrder(storage->removePinBtn, storage->clearPinsBtn);
    setTabOrder(storage->clearPinsBtn, m_generateBtn);
    setTabOrder(m_generateBtn, m_exportBtn);
}

void PuppetMeshPanel::setTargetObjectId(int32_t objectId, uint32_t texWidth, uint32_t texHeight)
{
    m_objectId = objectId;
    m_texWidth = texWidth;
    m_texHeight = texHeight;
    updateMeshStatsLabel();
}

void PuppetMeshPanel::setSourceAlpha(std::vector<uint8_t> alphaBuffer, uint32_t width,
                                     uint32_t height)
{
    const uint64_t pixelCount = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (width == 0u || height == 0u || width > WE_CDT_MAX_IMAGE_DIM ||
        height > WE_CDT_MAX_IMAGE_DIM || pixelCount > WE_CDT_MAX_IMAGE_PIXELS ||
        alphaBuffer.size() < static_cast<size_t>(pixelCount)) {
        m_alphaBuffer.clear();
        m_texWidth = 0u;
        m_texHeight = 0u;
        emit statusMessage(tr("Rejected invalid or oversized source alpha buffer"));
        return;
    }

    m_alphaBuffer = std::move(alphaBuffer);
    m_texWidth = width;
    m_texHeight = height;
    emit statusMessage(
        tr("Loaded %1x%2 source alpha buffer for cutout CDT").arg(width).arg(height));
}

void PuppetMeshPanel::loadExistingMesh(const we_puppet_t *puppet)
{
    auto *storage = ensureStorage(this);
    if (!storage) {
        return;
    }

    if (!puppet || puppet->vertex_count < kMinMeshVertices || !puppet->vertices ||
        puppet->index_count < 3u || !puppet->indices) {
        storage->vertices.clear();
        storage->indices.clear();
        m_lastResult = we_mesh_gen_result_t{};
        updateMeshStatsLabel();
        if (m_wireframeCheck && m_wireframeCheck->isChecked()) {
            emit wireframeOverlayRequested(true, m_lastResult);
        }
        return;
    }

    const uint32_t vertCount = std::min<uint32_t>(puppet->vertex_count, WE_MDL_MAX_VERTICES);
    const uint32_t triCount = std::min<uint32_t>(puppet->index_count / 3u, WE_MDL_MAX_TRIANGLES);
    const uint32_t idxCount = triCount * 3u;

    storage->vertices.assign(puppet->vertices, puppet->vertices + vertCount);
    storage->indices.assign(puppet->indices, puppet->indices + idxCount);
    storage->appliedConfig = currentConfig();
    storage->appliedPins = m_pins;

    float minAngleDeg = 0.0f;
    float meanQuality = 0.0f;
    computeMeshQuality(storage->vertices, storage->indices, minAngleDeg, meanQuality);

    m_lastResult = we_mesh_gen_result_t{};
    m_lastResult.vertices = storage->vertices.data();
    m_lastResult.vertex_count = vertCount;
    m_lastResult.indices = storage->indices.data();
    m_lastResult.index_count = idxCount;
    m_lastResult.triangle_count = triCount;
    m_lastResult.achieved_min_angle_deg = minAngleDeg;
    m_lastResult.achieved_mean_quality = meanQuality;
    m_lastResult.hit_vertex_budget =
        (vertCount >= static_cast<uint32_t>(currentConfig().max_vertices));

    updateMeshStatsLabel();
    if (m_wireframeCheck && m_wireframeCheck->isChecked()) {
        emit wireframeOverlayRequested(true, m_lastResult);
    }
}

void PuppetMeshPanel::addConstraintPin(double px, double py)
{
    if (!std::isfinite(px) || !std::isfinite(py)) {
        return;
    }
    if (m_pins.size() >= kMaxConstraintPins) {
        emit statusMessage(
            tr("Constraint pin cap (%1) reached").arg(static_cast<int>(kMaxConstraintPins)));
        return;
    }

    double clampedX = px;
    double clampedY = py;
    if (m_texWidth > 0u && m_texHeight > 0u) {
        clampedX = std::clamp(px, 0.0, static_cast<double>(m_texWidth));
        clampedY = std::clamp(py, 0.0, static_cast<double>(m_texHeight));
    }

    std::vector<we_cdt_pin_t> nextPins = m_pins;
    nextPins.push_back(we_cdt_pin_t{clampedX, clampedY, 1u});
    executePinEditCommand(
        this, m_objectId, m_pins, m_pinTree, m_commandBus, ensureStorage(this), currentConfig(),
        tr("Add constraint pin"), std::move(nextPins),
        tr("Added Steiner pin (%1, %2)").arg(clampedX, 0, 'f', 1).arg(clampedY, 0, 'f', 1));
}

void PuppetMeshPanel::removeConstraintPin(uint32_t index)
{
    if (index >= m_pins.size()) {
        return;
    }

    std::vector<we_cdt_pin_t> nextPins = m_pins;
    nextPins.erase(nextPins.begin() + static_cast<std::ptrdiff_t>(index));
    executePinEditCommand(this, m_objectId, m_pins, m_pinTree, m_commandBus, ensureStorage(this),
                          currentConfig(), tr("Remove constraint pin"), std::move(nextPins),
                          tr("Removed constraint pin #%1").arg(index));
}

bool PuppetMeshPanel::isWireframeVisible() const noexcept
{
    return m_wireframeCheck && m_wireframeCheck->isChecked();
}

we_mesh_gen_cfg_t PuppetMeshPanel::currentConfig() const noexcept
{
    we_mesh_gen_cfg_t cfg{};
    we_mesh_gen_cfg_default(&cfg);

    if (m_alphaThreshSpin) {
        cfg.alpha_threshold =
            std::clamp(static_cast<float>(m_alphaThreshSpin->value()), 1.0f / 255.0f, 1.0f);
    }
    if (m_dilationPxSpin) {
        cfg.dilation_px =
            std::clamp(static_cast<float>(m_dilationPxSpin->value()), 0.0f, WE_CDT_MAX_DILATION_PX);
    }
    if (m_simplifyEpsSpin) {
        cfg.simplify_eps_px =
            std::clamp(static_cast<float>(m_simplifyEpsSpin->value()), 0.25f, 64.0f);
    }
    if (m_interiorSpacingSpin) {
        cfg.interior_spacing =
            std::clamp(static_cast<float>(m_interiorSpacingSpin->value()), 4.0f, 512.0f);
    }
    if (m_minAngleSpin) {
        cfg.min_angle_deg = std::clamp(static_cast<float>(m_minAngleSpin->value()),
                                       WE_CDT_MIN_ANGLE_MIN_DEG, WE_CDT_MIN_ANGLE_MAX_DEG);
    }
    if (m_maxVertsSpin) {
        cfg.max_vertices = std::clamp(static_cast<uint32_t>(m_maxVertsSpin->value()),
                                      kMinMeshVertices, kMaxMeshVertices);
    }
    return cfg;
}

void PuppetMeshPanel::onExtractContoursClicked()
{
    const size_t requiredBytes = static_cast<size_t>(m_texWidth) * static_cast<size_t>(m_texHeight);
    if (m_alphaBuffer.empty() || m_texWidth == 0u || m_texHeight == 0u ||
        m_alphaBuffer.size() < requiredBytes) {
        emit statusMessage(tr("Load a valid source alpha buffer before extracting contours"));
        return;
    }

    const we_mesh_gen_cfg_t cfg = currentConfig();
    we_cdt_loop_t *loops = nullptr;
    uint32_t loopCount = 0u;
    if (!we_mesh_extract_contours(m_alphaBuffer.data(), m_texWidth, m_texHeight, m_texWidth, &cfg,
                                  &loops, &loopCount)) {
        emit statusMessage(tr("Contour extraction found no silhouette above alpha threshold"));
        return;
    }

    uint64_t totalPoints = 0u;
    uint32_t holeCount = 0u;
    for (uint32_t i = 0u; i < loopCount; ++i) {
        totalPoints += loops[i].point_count;
        if (loops[i].is_hole) {
            ++holeCount;
        }
    }
    we_mesh_loops_free(loops, loopCount);

    emit statusMessage(tr("Extracted %1 contour loop(s) (%2 hole(s), %3 simplified vertices)")
                           .arg(loopCount)
                           .arg(holeCount)
                           .arg(totalPoints));
}

void PuppetMeshPanel::onClearPinsClicked()
{
    if (m_pins.empty()) {
        return;
    }

    executePinEditCommand(this, m_objectId, m_pins, m_pinTree, m_commandBus, ensureStorage(this),
                          currentConfig(), tr("Clear constraint pins"), {},
                          tr("Cleared all interior constraint pins"));
}

void PuppetMeshPanel::onGenerateCutoutMeshClicked()
{
    const size_t requiredBytes = static_cast<size_t>(m_texWidth) * static_cast<size_t>(m_texHeight);
    if (m_alphaBuffer.empty() || m_texWidth == 0u || m_texHeight == 0u ||
        m_alphaBuffer.size() < requiredBytes) {
        emit statusMessage(tr("Load a valid source alpha buffer before generating mesh"));
        return;
    }

    auto *storage = ensureStorage(this);
    if (!storage) {
        return;
    }

    const we_mesh_gen_cfg_t cfg = currentConfig();
    we_mesh_gen_result_t rawResult{};
    const bool ok = we_mesh_gen_from_alpha(
        m_alphaBuffer.data(), m_texWidth, m_texHeight, m_texWidth, &cfg,
        m_pins.empty() ? nullptr : m_pins.data(), static_cast<uint32_t>(m_pins.size()), &rawResult);

    if (!ok || rawResult.vertex_count < kMinMeshVertices || rawResult.triangle_count < 1u ||
        !rawResult.vertices || !rawResult.indices ||
        rawResult.index_count < rawResult.triangle_count * 3u) {
        we_mesh_gen_result_free(&rawResult);
        emit statusMessage(tr("CDT mesh generation failed for current cutout settings"));
        return;
    }

    PuppetMeshSnapshot oldMesh;
    oldMesh.objectId = m_objectId;
    oldMesh.vertices = storage->vertices;
    oldMesh.indices = storage->indices;
    oldMesh.cdtConfig = storage->appliedConfig;
    oldMesh.pins = m_pins;

    PuppetMeshSnapshot newMesh;
    newMesh.objectId = m_objectId;
    newMesh.vertices.assign(rawResult.vertices, rawResult.vertices + rawResult.vertex_count);
    newMesh.indices.assign(rawResult.indices, rawResult.indices + rawResult.triangle_count * 3u);
    newMesh.cdtConfig = cfg;
    newMesh.pins = m_pins;

    const float cdtMinAngle = rawResult.achieved_min_angle_deg;
    const float cdtMeanQuality = rawResult.achieved_mean_quality;
    const bool hitBudget = rawResult.hit_vertex_budget;
    const uint32_t generatedVertCount = rawResult.vertex_count;
    we_mesh_gen_result_free(&rawResult);

    QPointer<PuppetMeshPanel> self(this);
    const int32_t targetObjectId = m_objectId;

    auto applySnapshot = [self, targetObjectId, cdtMinAngle, cdtMeanQuality, hitBudget,
                          generatedVertCount](const PuppetMeshSnapshot &snap) {
        if (!self || self->m_objectId != targetObjectId) {
            return;
        }
        auto *st = ensureStorage(self.data());
        if (!st) {
            return;
        }

        st->vertices = snap.vertices;
        st->indices = snap.indices;
        st->appliedConfig = snap.cdtConfig;
        st->appliedPins = snap.pins;
        self->m_pins = snap.pins;

        rebuildPinTreeUi(self->m_pinTree, self->m_pins);

        if (snap.cdtConfig.max_vertices >= kMinMeshVertices) {
            if (self->m_alphaThreshSpin) {
                const QSignalBlocker b(self->m_alphaThreshSpin);
                self->m_alphaThreshSpin->setValue(snap.cdtConfig.alpha_threshold);
            }
            if (self->m_dilationPxSpin) {
                const QSignalBlocker b(self->m_dilationPxSpin);
                self->m_dilationPxSpin->setValue(snap.cdtConfig.dilation_px);
            }
            if (self->m_simplifyEpsSpin) {
                const QSignalBlocker b(self->m_simplifyEpsSpin);
                self->m_simplifyEpsSpin->setValue(snap.cdtConfig.simplify_eps_px);
            }
            if (self->m_interiorSpacingSpin) {
                const QSignalBlocker b(self->m_interiorSpacingSpin);
                self->m_interiorSpacingSpin->setValue(snap.cdtConfig.interior_spacing);
            }
            if (self->m_minAngleSpin) {
                const QSignalBlocker b(self->m_minAngleSpin);
                self->m_minAngleSpin->setValue(snap.cdtConfig.min_angle_deg);
            }
            if (self->m_maxVertsSpin) {
                const QSignalBlocker b(self->m_maxVertsSpin);
                self->m_maxVertsSpin->setValue(static_cast<int>(
                    std::min<uint32_t>(snap.cdtConfig.max_vertices, kMaxMeshVertices)));
            }
        }

        float computedMinAngle = 0.0f;
        float computedMeanQ = 0.0f;
        computeMeshQuality(st->vertices, st->indices, computedMinAngle, computedMeanQ);

        const uint32_t vertCount = static_cast<uint32_t>(st->vertices.size());
        const uint32_t idxCount = static_cast<uint32_t>(st->indices.size());
        const uint32_t triCount = idxCount / 3u;

        self->m_lastResult = we_mesh_gen_result_t{};
        self->m_lastResult.vertices = st->vertices.empty() ? nullptr : st->vertices.data();
        self->m_lastResult.vertex_count = vertCount;
        self->m_lastResult.indices = st->indices.empty() ? nullptr : st->indices.data();
        self->m_lastResult.index_count = idxCount;
        self->m_lastResult.triangle_count = triCount;
        self->m_lastResult.achieved_min_angle_deg =
            (computedMinAngle > 0.0f || triCount == 0u) ? computedMinAngle : cdtMinAngle;
        self->m_lastResult.achieved_mean_quality =
            (computedMeanQ > 0.0f || triCount == 0u) ? computedMeanQ : cdtMeanQuality;
        self->m_lastResult.hit_vertex_budget =
            (vertCount == generatedVertCount && hitBudget) ||
            (vertCount > 0u && vertCount >= snap.cdtConfig.max_vertices);

        self->updateMeshStatsLabel();
        self->syncMeshToDaemon(snap);

        emit self->contourPinsChanged(self->m_objectId, self->m_pins);
        emit self->meshGenerated(self->m_objectId, vertCount, triCount,
                                 self->m_lastResult.achieved_min_angle_deg);
        emit self->wireframeOverlayRequested(self->isWireframeVisible(), self->m_lastResult);
        emit self->statusMessage(
            vertCount == 0u
                ? tr("Cleared cutout mesh for object #%1").arg(self->m_objectId)
                : tr("CDT mesh ready: %1 verts, %2 tris, min angle %3 deg")
                      .arg(vertCount)
                      .arg(triCount)
                      .arg(static_cast<double>(self->m_lastResult.achieved_min_angle_deg), 0, 'f',
                           1));
    };

    if (m_commandBus) {
        m_commandBus->execute(
            std::make_unique<MeshEditCommand>(tr("Generate cutout mesh"), std::move(oldMesh),
                                              std::move(newMesh), std::move(applySnapshot)));
    } else {
        applySnapshot(newMesh);
    }
}

void PuppetMeshPanel::onExportMdlClicked()
{
    if (m_objectId < 0 || m_lastResult.vertex_count < kMinMeshVertices ||
        m_lastResult.triangle_count < 1u) {
        emit statusMessage(tr("Generate or load a valid puppet mesh before exporting .mdl"));
        return;
    }

    QString cacheBase = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheBase.isEmpty()) {
        cacheBase = QDir::tempPath();
    }
    const QString defaultFile = QDir(cacheBase + QStringLiteral("/wwal/exports"))
                                    .filePath(QStringLiteral("puppet_%1.mdl").arg(m_objectId));

    QString destPath;
    const QVariant headlessProp = property("headlessExportPath");
    if (headlessProp.isValid()) {
        destPath = headlessProp.toString().trimmed();
    } else if (QGuiApplication::platformName() == QLatin1String("offscreen")) {
        destPath = defaultFile;
    } else {
        destPath =
            QFileDialog::getSaveFileName(this, tr("Export Puppet .mdl"), defaultFile,
                                         tr("Wallpaper Engine Model (*.mdl);;All Files (*)"));
    }

    if (destPath.isEmpty()) {
        emit statusMessage(tr("Canceled .mdl export for object #%1").arg(m_objectId));
        return;
    }
    if (QFileInfo(destPath).suffix().isEmpty()) {
        destPath.append(QStringLiteral(".mdl"));
    }

    emit exportMdlRequested(m_objectId, destPath);
    emit statusMessage(
        tr("Requested .mdl export for object #%1 to %2").arg(m_objectId).arg(destPath));
}

void PuppetMeshPanel::updateMeshStatsLabel()
{
    const auto *storage = findStorage(this);

    const uint32_t vCount = m_lastResult.vertex_count;
    const uint32_t tCount = m_lastResult.triangle_count;
    const float minAngle = m_lastResult.achieved_min_angle_deg;
    const float meanQ = m_lastResult.achieved_mean_quality;
    const bool hasMesh = (vCount >= kMinMeshVertices && tCount > 0u);

    if (storage && storage->triBadge) {
        storage->triBadge->setText(tr("TRIS: %1").arg(tCount));
        const QString triColor = hasMesh ? QStringLiteral("#f1f5f9") : QStringLiteral("#8b9bb4");
        storage->triBadge->setStyleSheet(
            QStringLiteral("QLabel { font-family: 'JetBrains Mono', monospace; font-size: 11px; "
                           "font-weight: 600; color: %1; background-color: #0b0c10; "
                           "border: 1px solid #2a2f3d; border-radius: 4px; padding: 4px 6px; }")
                .arg(triColor));
    }

    if (storage && storage->angleBadge) {
        if (!hasMesh) {
            storage->angleBadge->setText(tr("MIN ANGLE: -- deg"));
            storage->angleBadge->setStyleSheet(QStringLiteral(
                "QLabel { font-family: 'JetBrains Mono', monospace; font-size: 11px; "
                "font-weight: 600; color: #8b9bb4; background-color: #0b0c10; "
                "border: 1px solid #2a2f3d; border-radius: 4px; padding: 4px 6px; }"));
        } else {
            const bool meetsTarget = (minAngle >= kTargetMinAngleDeg);
            const QString angleColor =
                meetsTarget ? QStringLiteral("#a6e3a1") : QStringLiteral("#f9e2af");
            const QString borderCol =
                meetsTarget ? QStringLiteral("#2e5a44") : QStringLiteral("#5c4b23");
            storage->angleBadge->setText(
                tr("MIN ANGLE: %1 deg").arg(static_cast<double>(minAngle), 0, 'f', 1));
            storage->angleBadge->setStyleSheet(
                QStringLiteral(
                    "QLabel { font-family: 'JetBrains Mono', monospace; font-size: 11px; "
                    "font-weight: 600; color: %1; background-color: #0b0c10; "
                    "border: 1px solid %2; border-radius: 4px; padding: 4px 6px; }")
                    .arg(angleColor, borderCol));
        }
    }

    if (!m_statsLabel) {
        return;
    }

    if (!hasMesh) {
        m_statsLabel->setText(tr("Object #%1 | No mesh generated (ready: %2x%3)")
                                  .arg(m_objectId)
                                  .arg(m_texWidth)
                                  .arg(m_texHeight));
        return;
    }

    const QString budgetNote = m_lastResult.hit_vertex_budget ? tr(" [CAP HIT]") : QString();
    m_statsLabel->setText(tr("Object #%1 | Verts: %2 | Tris: %3 | Mean Q: %4%5")
                              .arg(m_objectId)
                              .arg(vCount)
                              .arg(tCount)
                              .arg(static_cast<double>(meanQ), 0, 'f', 3)
                              .arg(budgetNote));
}

void PuppetMeshPanel::syncMeshToDaemon(const PuppetMeshSnapshot &snapshot)
{
    dispatchPuppetSetMeshIpc(m_ipcClient, snapshot);
}

} // namespace waywal::studio
