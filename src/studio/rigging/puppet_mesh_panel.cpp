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
#include <QMetaObject>
#include <QPointer>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QThreadPool>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>

namespace waywal::studio {

namespace {

constexpr uint32_t kMaxConstraintPins = 4096u;
constexpr uint32_t kMinMeshVertices = 3u;
constexpr uint32_t kMaxMeshVertices = WE_MDL_MAX_VERTICES;
constexpr float kTargetMinAngleDeg = 25.0f;

struct MeshObjectState {
    uint32_t texWidth{0u};
    uint32_t texHeight{0u};
    std::vector<uint8_t> alphaBuffer;
    std::vector<we_cdt_pin_t> pins;
    std::vector<we_mdl_vertex_t> vertices;
    std::vector<uint16_t> indices;
    we_mesh_gen_cfg_t appliedConfig{};
    std::vector<we_cdt_pin_t> appliedPins;
    std::vector<ContourPreviewLoop> contourPreview;
    float cdtMinAngle{0.0f};
    float cdtMeanQuality{0.0f};
    bool hitBudget{false};
    uint32_t generatedVertCount{0u};
};

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

void remapVertexListBlendIndices(std::vector<we_mdl_vertex_t> &vertices,
                                 const std::vector<int32_t> &oldToNew) noexcept
{
    const bool anySurviving =
        std::any_of(oldToNew.begin(), oldToNew.end(), [](int32_t mapped) { return mapped >= 0; });

    for (we_mdl_vertex_t &vert : vertices) {
        float weightSum = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const uint32_t oldBone = vert.blend_indices[k];
            const float w = vert.weight[k];
            if (oldBone < oldToNew.size() && oldToNew[oldBone] >= 0 && w > 0.0f) {
                vert.blend_indices[k] = static_cast<uint32_t>(oldToNew[oldBone]);
                weightSum += w;
            } else {
                vert.blend_indices[k] = 0u;
                vert.weight[k] = 0.0f;
            }
        }

        if (weightSum > 1e-6f) {
            for (int k = 0; k < 4; ++k) {
                vert.weight[k] /= weightSum;
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

} // namespace

struct PuppetMeshPanel::Impl {
    QThreadPool workerPool;
    std::shared_ptr<std::atomic<bool>> cancelFlag{std::make_shared<std::atomic<bool>>(false)};
    uint64_t activeGeneration{0u};
    bool taskRunning{false};

    std::vector<we_mdl_vertex_t> vertices;
    std::vector<uint16_t> indices;
    we_mesh_gen_cfg_t appliedConfig{};
    std::vector<we_cdt_pin_t> appliedPins;
    std::vector<ContourPreviewLoop> contourPreview;
    std::unordered_map<int32_t, MeshObjectState> objectStates;

    QDoubleSpinBox *minIslandAreaSpin{nullptr};
    QLabel *triBadge{nullptr};
    QLabel *angleBadge{nullptr};
    QPushButton *extractBtn{nullptr};
    QPushButton *removePinBtn{nullptr};
    QPushButton *clearPinsBtn{nullptr};
};

PuppetMeshPanel::PuppetMeshPanel(QWidget *parent)
    : QWidget(parent), m_impl(std::make_unique<Impl>())
{
    m_impl->workerPool.setMaxThreadCount(1);
    we_mesh_gen_cfg_default(&m_impl->appliedConfig);

    setObjectName(QStringLiteral("PuppetMeshPanel"));
    setAccessibleName(tr("Mesh and Cutout"));
    setAccessibleDescription(
        tr("Alpha silhouette contour extraction, constraint pins, and Constrained Delaunay mesh "
           "generation"));
    setupUi();
    updateMeshStatsLabel();
}

PuppetMeshPanel::~PuppetMeshPanel()
{
    cancelPendingTasks();
    if (m_impl) {
        m_impl->workerPool.waitForDone(5000);
    }
    m_lastResult = we_mesh_gen_result_t{};
}

void PuppetMeshPanel::setupUi()
{
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

    m_impl->minIslandAreaSpin = new QDoubleSpinBox(cdtGroup);
    configureDoubleSpin(
        m_impl->minIslandAreaSpin, 0.0, 4096.0, 1.0, 1,
        static_cast<double>(defaults.min_island_area), QStringLiteral(" px^2"),
        tr("Minimum Island Area"),
        tr("Speckle contour discard area threshold in square pixels [0.0..4096.0]"));
    addRowLabel(5, tr("Min Island Area:"), m_impl->minIslandAreaSpin->toolTip());
    cdtLayout->addWidget(m_impl->minIslandAreaSpin, 5, 1);

    m_maxVertsSpin = new QSpinBox(cdtGroup);
    m_maxVertsSpin->setRange(static_cast<int>(kMinMeshVertices),
                             static_cast<int>(kMaxMeshVertices));
    m_maxVertsSpin->setSingleStep(256);
    m_maxVertsSpin->setValue(
        std::clamp(static_cast<int>(defaults.max_vertices), 3, static_cast<int>(kMaxMeshVertices)));
    m_maxVertsSpin->setKeyboardTracking(false);
    m_maxVertsSpin->setAccessibleName(tr("Maximum Vertex Cap"));
    m_maxVertsSpin->setToolTip(
        tr("Hard vertex cap [3..65536] enforced during Steiner insertion and Chew/Ruppert "
           "refinement"));
    m_maxVertsSpin->setAccessibleDescription(m_maxVertsSpin->toolTip());
    addRowLabel(6, tr("Max Vertex Cap:"), m_maxVertsSpin->toolTip());
    cdtLayout->addWidget(m_maxVertsSpin, 6, 1);

    m_wireframeCheck = new QCheckBox(tr("Show CDT Wireframe Overlay"), cdtGroup);
    m_wireframeCheck->setAccessibleName(tr("Show CDT Wireframe Overlay"));
    m_wireframeCheck->setToolTip(
        tr("Overlay generated Constrained Delaunay mesh edges inside the viewport"));
    m_wireframeCheck->setAccessibleDescription(m_wireframeCheck->toolTip());
    connect(m_wireframeCheck, &QCheckBox::toggled, this,
            [this](bool checked) { emit wireframeOverlayRequested(checked, m_lastResult); });
    cdtLayout->addWidget(m_wireframeCheck, 7, 0, 1, 2);

    contentLayout->addWidget(cdtGroup);

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

    m_impl->extractBtn = new QPushButton(tr("Preview Contours"), pinGroup);
    m_impl->extractBtn->setAccessibleName(tr("Preview Extracted Contours"));
    m_impl->extractBtn->setToolTip(
        tr("Run Marching Squares and TP-RDP contour extraction on the loaded alpha channel"));
    connect(m_impl->extractBtn, &QPushButton::clicked, this,
            &PuppetMeshPanel::onExtractContoursClicked);

    m_impl->removePinBtn = new QPushButton(tr("Remove Pin"), pinGroup);
    m_impl->removePinBtn->setAccessibleName(tr("Remove Selected Constraint Pin"));
    m_impl->removePinBtn->setToolTip(tr("Remove the currently selected Steiner constraint pin"));
    connect(m_impl->removePinBtn, &QPushButton::clicked, this, [this]() {
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

    m_impl->clearPinsBtn = new QPushButton(tr("Clear Pins"), pinGroup);
    m_impl->clearPinsBtn->setAccessibleName(tr("Clear All Constraint Pins"));
    m_impl->clearPinsBtn->setToolTip(tr("Remove all interior Steiner constraint pins"));
    connect(m_impl->clearPinsBtn, &QPushButton::clicked, this,
            &PuppetMeshPanel::onClearPinsClicked);

    pinBtnRow->addWidget(m_impl->extractBtn);
    pinBtnRow->addWidget(m_impl->removePinBtn);
    pinBtnRow->addWidget(m_impl->clearPinsBtn);
    pinLayout->addLayout(pinBtnRow);

    contentLayout->addWidget(pinGroup);

    auto *actionRow = new QHBoxLayout();
    actionRow->setSpacing(6);

    m_generateBtn = new QPushButton(tr("Generate Cutout Mesh"), container);
    m_generateBtn->setObjectName(QStringLiteral("generateMeshBtn"));
    m_generateBtn->setAccessibleName(tr("Generate Cutout Mesh"));
    m_generateBtn->setToolTip(
        tr("Generate Constrained Delaunay cutout mesh on a worker thread and push snapshot"));
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

    auto *badgeRow = new QHBoxLayout();
    badgeRow->setContentsMargins(0, 0, 0, 0);
    badgeRow->setSpacing(6);

    m_impl->triBadge = new QLabel(tr("TRIS: 0"), this);
    m_impl->triBadge->setAlignment(Qt::AlignCenter);
    m_impl->triBadge->setAccessibleName(tr("Generated Triangle Count Badge"));
    m_impl->triBadge->setToolTip(tr("Total triangles in active puppet mesh"));

    m_impl->angleBadge = new QLabel(tr("MIN ANGLE: -- deg"), this);
    m_impl->angleBadge->setAlignment(Qt::AlignCenter);
    m_impl->angleBadge->setAccessibleName(tr("Minimum Triangle Angle Telemetry Badge"));
    m_impl->angleBadge->setToolTip(
        tr("Achieved minimum interior triangle angle (target >= 25.0 deg)"));

    badgeRow->addWidget(m_impl->triBadge, 1);
    badgeRow->addWidget(m_impl->angleBadge, 1);
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
    setTabOrder(m_minAngleSpin, m_impl->minIslandAreaSpin);
    setTabOrder(m_impl->minIslandAreaSpin, m_maxVertsSpin);
    setTabOrder(m_maxVertsSpin, m_wireframeCheck);
    setTabOrder(m_wireframeCheck, m_pinTree);
    setTabOrder(m_pinTree, m_impl->extractBtn);
    setTabOrder(m_impl->extractBtn, m_impl->removePinBtn);
    setTabOrder(m_impl->removePinBtn, m_impl->clearPinsBtn);
    setTabOrder(m_impl->clearPinsBtn, m_generateBtn);
    setTabOrder(m_generateBtn, m_exportBtn);
}

void PuppetMeshPanel::saveActiveObjectState()
{
    if (!m_impl || m_objectId < 0) {
        return;
    }
    MeshObjectState &st = m_impl->objectStates[m_objectId];
    st.texWidth = m_texWidth;
    st.texHeight = m_texHeight;
    st.alphaBuffer = m_alphaBuffer;
    st.pins = m_pins;
    st.vertices = m_impl->vertices;
    st.indices = m_impl->indices;
    st.appliedConfig = m_impl->appliedConfig;
    st.appliedPins = m_impl->appliedPins;
    st.contourPreview = m_impl->contourPreview;
    st.cdtMinAngle = m_lastResult.achieved_min_angle_deg;
    st.cdtMeanQuality = m_lastResult.achieved_mean_quality;
    st.hitBudget = m_lastResult.hit_vertex_budget;
    st.generatedVertCount = m_lastResult.vertex_count;
}

void PuppetMeshPanel::restoreObjectState(int32_t objectId, uint32_t texWidth, uint32_t texHeight)
{
    m_objectId = objectId;
    const auto it = m_impl->objectStates.find(objectId);
    if (it != m_impl->objectStates.end()) {
        const MeshObjectState &st = it->second;
        m_texWidth = (texWidth > 0u) ? texWidth : st.texWidth;
        m_texHeight = (texHeight > 0u) ? texHeight : st.texHeight;
        m_alphaBuffer = st.alphaBuffer;
        m_pins = st.pins;
        m_impl->vertices = st.vertices;
        m_impl->indices = st.indices;
        m_impl->appliedConfig = st.appliedConfig;
        m_impl->appliedPins = st.appliedPins;
        m_impl->contourPreview = st.contourPreview;
        refreshLastResultView(st.cdtMinAngle, st.cdtMeanQuality, st.hitBudget,
                              st.generatedVertCount);
    } else {
        m_texWidth = texWidth;
        m_texHeight = texHeight;
        m_alphaBuffer.clear();
        m_pins.clear();
        m_impl->vertices.clear();
        m_impl->indices.clear();
        we_mesh_gen_cfg_default(&m_impl->appliedConfig);
        m_impl->appliedPins.clear();
        m_impl->contourPreview.clear();
        m_lastResult = we_mesh_gen_result_t{};
    }
    rebuildPinTreeUi(m_pinTree, m_pins);
    updateMeshStatsLabel();
}

void PuppetMeshPanel::refreshLastResultView(float cdtMinAngle, float cdtMeanQuality, bool hitBudget,
                                            uint32_t generatedVertCount)
{
    float computedMinAngle = 0.0f;
    float computedMeanQ = 0.0f;
    computeMeshQuality(m_impl->vertices, m_impl->indices, computedMinAngle, computedMeanQ);

    const uint32_t vertCount = static_cast<uint32_t>(m_impl->vertices.size());
    const uint32_t idxCount = static_cast<uint32_t>(m_impl->indices.size());
    const uint32_t triCount = idxCount / 3u;

    m_lastResult = we_mesh_gen_result_t{};
    m_lastResult.vertices = m_impl->vertices.empty() ? nullptr : m_impl->vertices.data();
    m_lastResult.vertex_count = vertCount;
    m_lastResult.indices = m_impl->indices.empty() ? nullptr : m_impl->indices.data();
    m_lastResult.index_count = idxCount;
    m_lastResult.triangle_count = triCount;
    m_lastResult.achieved_min_angle_deg =
        (computedMinAngle > 0.0f || triCount == 0u) ? computedMinAngle : cdtMinAngle;
    m_lastResult.achieved_mean_quality =
        (computedMeanQ > 0.0f || triCount == 0u) ? computedMeanQ : cdtMeanQuality;
    m_lastResult.hit_vertex_budget =
        (vertCount > 0u && vertCount == generatedVertCount && hitBudget) ||
        (vertCount > 0u && m_impl->appliedConfig.max_vertices > 0u &&
         vertCount >= m_impl->appliedConfig.max_vertices);
}

void PuppetMeshPanel::updateAsyncUiState()
{
    const bool busy = m_impl && m_impl->taskRunning;
    if (m_generateBtn) {
        m_generateBtn->setText(busy ? tr("Restart Cutout Mesh") : tr("Generate Cutout Mesh"));
    }
    if (m_impl && m_impl->extractBtn) {
        m_impl->extractBtn->setText(busy ? tr("Re-extract Contours") : tr("Preview Contours"));
    }
}

void PuppetMeshPanel::cancelPendingTasks()
{
    if (!m_impl) {
        return;
    }
    if (m_impl->cancelFlag) {
        m_impl->cancelFlag->store(true, std::memory_order_release);
    }
    ++m_impl->activeGeneration;
    m_impl->workerPool.clear();
    m_impl->taskRunning = false;
    updateAsyncUiState();
}

const std::vector<ContourPreviewLoop> &PuppetMeshPanel::contourPreview() const noexcept
{
    return m_impl->contourPreview;
}

bool PuppetMeshPanel::isTaskRunning() const noexcept { return m_impl && m_impl->taskRunning; }

void PuppetMeshPanel::setTargetObjectId(int32_t objectId, uint32_t texWidth, uint32_t texHeight)
{
    if (objectId == m_objectId) {
        if (texWidth > 0u) {
            m_texWidth = texWidth;
        }
        if (texHeight > 0u) {
            m_texHeight = texHeight;
        }
        saveActiveObjectState();
        updateMeshStatsLabel();
        return;
    }

    cancelPendingTasks();
    saveActiveObjectState();
    restoreObjectState(objectId, texWidth, texHeight);
    emit contourPinsChanged(m_objectId, m_pins);
    emit contoursExtracted(m_objectId, m_impl->contourPreview);
    if (m_wireframeCheck && m_wireframeCheck->isChecked()) {
        emit wireframeOverlayRequested(true, m_lastResult);
    }
}

void PuppetMeshPanel::setSourceAlpha(std::vector<uint8_t> alphaBuffer, uint32_t width,
                                     uint32_t height)
{
    cancelPendingTasks();

    const uint64_t pixelCount = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (width == 0u || height == 0u || width > WE_CDT_MAX_IMAGE_DIM ||
        height > WE_CDT_MAX_IMAGE_DIM || pixelCount > WE_CDT_MAX_IMAGE_PIXELS ||
        alphaBuffer.size() < static_cast<size_t>(pixelCount)) {
        m_alphaBuffer.clear();
        m_texWidth = 0u;
        m_texHeight = 0u;
        saveActiveObjectState();
        updateMeshStatsLabel();
        emit statusMessage(tr("Rejected invalid or oversized source alpha buffer"));
        return;
    }

    m_alphaBuffer = std::move(alphaBuffer);
    m_texWidth = width;
    m_texHeight = height;

    // A fresh silhouette invalidates old pins, triangles, and contour previews.
    m_pins.clear();
    m_impl->vertices.clear();
    m_impl->indices.clear();
    m_impl->appliedPins.clear();
    m_impl->contourPreview.clear();
    m_lastResult = we_mesh_gen_result_t{};
    rebuildPinTreeUi(m_pinTree, m_pins);
    saveActiveObjectState();
    updateMeshStatsLabel();

    emit contourPinsChanged(m_objectId, m_pins);
    emit contoursExtracted(m_objectId, m_impl->contourPreview);
    if (m_wireframeCheck && m_wireframeCheck->isChecked()) {
        emit wireframeOverlayRequested(true, m_lastResult);
    }
    emit statusMessage(
        tr("Loaded %1x%2 source alpha buffer for cutout CDT").arg(width).arg(height));
}

void PuppetMeshPanel::loadExistingMesh(const we_puppet_t *puppet)
{
    cancelPendingTasks();

    // Replacing the active mesh invalidates prior alpha silhouettes, pins, and previews.
    m_alphaBuffer.clear();
    m_pins.clear();
    m_impl->appliedPins.clear();
    m_impl->contourPreview.clear();
    rebuildPinTreeUi(m_pinTree, m_pins);

    if (!puppet || puppet->vertex_count < kMinMeshVertices || !puppet->vertices ||
        puppet->index_count < 3u || !puppet->indices) {
        m_impl->vertices.clear();
        m_impl->indices.clear();
        m_lastResult = we_mesh_gen_result_t{};
        saveActiveObjectState();
        updateMeshStatsLabel();
        emit contourPinsChanged(m_objectId, m_pins);
        emit contoursExtracted(m_objectId, m_impl->contourPreview);
        emit wireframeOverlayRequested(isWireframeVisible(), m_lastResult);
        return;
    }

    const uint32_t vertCount = std::min<uint32_t>(puppet->vertex_count, WE_MDL_MAX_VERTICES);
    const uint32_t triCount = std::min<uint32_t>(puppet->index_count / 3u, WE_MDL_MAX_TRIANGLES);
    const uint32_t idxCount = triCount * 3u;

    m_impl->vertices.assign(puppet->vertices, puppet->vertices + vertCount);
    m_impl->indices.assign(puppet->indices, puppet->indices + idxCount);
    m_impl->appliedConfig = currentConfig();

    refreshLastResultView(0.0f, 0.0f,
                          vertCount >= static_cast<uint32_t>(m_impl->appliedConfig.max_vertices),
                          vertCount);
    saveActiveObjectState();
    updateMeshStatsLabel();

    emit contourPinsChanged(m_objectId, m_pins);
    emit contoursExtracted(m_objectId, m_impl->contourPreview);
    emit meshGenerated(m_objectId, m_lastResult.vertex_count, m_lastResult.triangle_count,
                       m_lastResult.achieved_min_angle_deg);
    emit wireframeOverlayRequested(isWireframeVisible(), m_lastResult);
}

void PuppetMeshPanel::applyPinSnapshot(const PuppetMeshSnapshot &snap, size_t expectedPinCount,
                                       const QString &applyMsg)
{
    if (snap.objectId >= 0 && snap.objectId != m_objectId) {
        cancelPendingTasks();
        saveActiveObjectState();
        restoreObjectState(snap.objectId, 0u, 0u);
    }

    m_pins = snap.pins;
    m_impl->appliedPins = snap.pins;
    saveActiveObjectState();
    rebuildPinTreeUi(m_pinTree, m_pins);
    emit contourPinsChanged(m_objectId, m_pins);
    emit statusMessage(snap.pins.size() == expectedPinCount
                           ? applyMsg
                           : tr("Restored %1 constraint pin(s)").arg(snap.pins.size()));
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

    PuppetMeshSnapshot oldSnap;
    oldSnap.objectId = m_objectId;
    oldSnap.vertices = m_impl->vertices;
    oldSnap.indices = m_impl->indices;
    oldSnap.cdtConfig = m_impl->vertices.empty() ? currentConfig() : m_impl->appliedConfig;
    oldSnap.pins = m_pins;

    PuppetMeshSnapshot newSnap = oldSnap;
    newSnap.pins = nextPins;

    QPointer<PuppetMeshPanel> self(this);
    const size_t newSize = nextPins.size();
    const QString applyMsg =
        tr("Added Steiner pin (%1, %2)").arg(clampedX, 0, 'f', 1).arg(clampedY, 0, 'f', 1);
    auto applyPins = [self, newSize, applyMsg](const PuppetMeshSnapshot &snap) {
        if (self) {
            self->applyPinSnapshot(snap, newSize, applyMsg);
        }
    };

    if (m_commandBus) {
        m_commandBus->execute(
            std::make_unique<MeshEditCommand>(tr("Add constraint pin"), std::move(oldSnap),
                                              std::move(newSnap), std::move(applyPins)));
    } else {
        applyPins(newSnap);
    }
}

void PuppetMeshPanel::removeConstraintPin(uint32_t index)
{
    if (index >= m_pins.size()) {
        return;
    }

    std::vector<we_cdt_pin_t> nextPins = m_pins;
    nextPins.erase(nextPins.begin() + static_cast<std::ptrdiff_t>(index));

    PuppetMeshSnapshot oldSnap;
    oldSnap.objectId = m_objectId;
    oldSnap.vertices = m_impl->vertices;
    oldSnap.indices = m_impl->indices;
    oldSnap.cdtConfig = m_impl->vertices.empty() ? currentConfig() : m_impl->appliedConfig;
    oldSnap.pins = m_pins;

    PuppetMeshSnapshot newSnap = oldSnap;
    newSnap.pins = nextPins;

    QPointer<PuppetMeshPanel> self(this);
    const size_t newSize = nextPins.size();
    const QString applyMsg = tr("Removed constraint pin #%1").arg(index);
    auto applyPins = [self, newSize, applyMsg](const PuppetMeshSnapshot &snap) {
        if (self) {
            self->applyPinSnapshot(snap, newSize, applyMsg);
        }
    };

    if (m_commandBus) {
        m_commandBus->execute(
            std::make_unique<MeshEditCommand>(tr("Remove constraint pin"), std::move(oldSnap),
                                              std::move(newSnap), std::move(applyPins)));
    } else {
        applyPins(newSnap);
    }
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
    if (m_impl && m_impl->minIslandAreaSpin) {
        cfg.min_island_area =
            std::clamp(static_cast<float>(m_impl->minIslandAreaSpin->value()), 0.0f, 4096.0f);
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

    cancelPendingTasks();
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    m_impl->cancelFlag = cancel;
    const uint64_t gen = ++m_impl->activeGeneration;
    m_impl->taskRunning = true;
    updateAsyncUiState();
    emit statusMessage(tr("Extracting silhouette contours..."));

    std::vector<uint8_t> alphaCopy = m_alphaBuffer;
    const uint32_t texW = m_texWidth;
    const uint32_t texH = m_texHeight;
    const we_mesh_gen_cfg_t cfg = currentConfig();
    const int32_t targetObjectId = m_objectId;
    QPointer<PuppetMeshPanel> self(this);

    m_impl->workerPool.start([self, gen, cancel, targetObjectId, texW, texH, cfg,
                              alpha = std::move(alphaCopy)]() mutable {
        if (cancel->load(std::memory_order_acquire)) {
            return;
        }

        we_cdt_loop_t *loops = nullptr;
        uint32_t loopCount = 0u;
        const bool ok =
            we_mesh_extract_contours(alpha.data(), texW, texH, texW, &cfg, &loops, &loopCount);

        std::vector<ContourPreviewLoop> preview;
        uint64_t totalPoints = 0u;
        uint32_t holeCount = 0u;
        if (ok && loops && loopCount > 0u) {
            preview.reserve(loopCount);
            for (uint32_t i = 0u; i < loopCount; ++i) {
                ContourPreviewLoop outLoop;
                outLoop.isHole = loops[i].is_hole;
                outLoop.signedAreaPx2 = loops[i].signed_area_px2;
                if (loops[i].points && loops[i].point_count > 0u) {
                    outLoop.points.assign(loops[i].points, loops[i].points + loops[i].point_count);
                    totalPoints += loops[i].point_count;
                }
                if (loops[i].is_hole) {
                    ++holeCount;
                }
                preview.push_back(std::move(outLoop));
            }
        }
        we_mesh_loops_free(loops, loopCount);

        if (!self || cancel->load(std::memory_order_acquire)) {
            return;
        }

        QMetaObject::invokeMethod(
            self.data(),
            [self, gen, cancel, targetObjectId, ok, loopCount, holeCount, totalPoints,
             preview = std::move(preview)]() mutable {
                if (!self || cancel->load(std::memory_order_acquire) ||
                    gen != self->m_impl->activeGeneration) {
                    return;
                }
                self->m_impl->taskRunning = false;
                self->updateAsyncUiState();
                if (!ok) {
                    self->m_impl->contourPreview.clear();
                    self->saveActiveObjectState();
                    emit self->contoursExtracted(targetObjectId, self->m_impl->contourPreview);
                    emit self->statusMessage(
                        tr("Contour extraction found no silhouette above alpha threshold"));
                    return;
                }

                self->m_impl->contourPreview = std::move(preview);
                self->saveActiveObjectState();
                emit self->contoursExtracted(targetObjectId, self->m_impl->contourPreview);
                emit self->statusMessage(
                    tr("Extracted %1 contour loop(s) (%2 hole(s), %3 simplified vertices)")
                        .arg(loopCount)
                        .arg(holeCount)
                        .arg(totalPoints));
            },
            Qt::QueuedConnection);
    });
}

void PuppetMeshPanel::onClearPinsClicked()
{
    if (m_pins.empty()) {
        return;
    }

    PuppetMeshSnapshot oldSnap;
    oldSnap.objectId = m_objectId;
    oldSnap.vertices = m_impl->vertices;
    oldSnap.indices = m_impl->indices;
    oldSnap.cdtConfig = m_impl->vertices.empty() ? currentConfig() : m_impl->appliedConfig;
    oldSnap.pins = m_pins;

    PuppetMeshSnapshot newSnap = oldSnap;
    newSnap.pins.clear();

    QPointer<PuppetMeshPanel> self(this);
    const QString applyMsg = tr("Cleared all interior constraint pins");
    auto applyPins = [self, applyMsg](const PuppetMeshSnapshot &snap) {
        if (self) {
            self->applyPinSnapshot(snap, 0u, applyMsg);
        }
    };

    if (m_commandBus) {
        m_commandBus->execute(
            std::make_unique<MeshEditCommand>(tr("Clear constraint pins"), std::move(oldSnap),
                                              std::move(newSnap), std::move(applyPins)));
    } else {
        applyPins(newSnap);
    }
}

void PuppetMeshPanel::applyMeshSnapshot(const PuppetMeshSnapshot &snap, float cdtMinAngle,
                                        float cdtMeanQuality, bool hitBudget,
                                        uint32_t generatedVertCount)
{
    if (snap.objectId >= 0 && snap.objectId != m_objectId) {
        cancelPendingTasks();
        saveActiveObjectState();
        restoreObjectState(snap.objectId, 0u, 0u);
    }

    m_impl->vertices = snap.vertices;
    m_impl->indices = snap.indices;
    m_impl->appliedConfig = snap.cdtConfig;
    m_impl->appliedPins = snap.pins;
    m_pins = snap.pins;

    rebuildPinTreeUi(m_pinTree, m_pins);

    if (snap.cdtConfig.max_vertices >= kMinMeshVertices) {
        if (m_alphaThreshSpin) {
            const QSignalBlocker b(m_alphaThreshSpin);
            m_alphaThreshSpin->setValue(snap.cdtConfig.alpha_threshold);
        }
        if (m_dilationPxSpin) {
            const QSignalBlocker b(m_dilationPxSpin);
            m_dilationPxSpin->setValue(snap.cdtConfig.dilation_px);
        }
        if (m_simplifyEpsSpin) {
            const QSignalBlocker b(m_simplifyEpsSpin);
            m_simplifyEpsSpin->setValue(snap.cdtConfig.simplify_eps_px);
        }
        if (m_interiorSpacingSpin) {
            const QSignalBlocker b(m_interiorSpacingSpin);
            m_interiorSpacingSpin->setValue(snap.cdtConfig.interior_spacing);
        }
        if (m_minAngleSpin) {
            const QSignalBlocker b(m_minAngleSpin);
            m_minAngleSpin->setValue(snap.cdtConfig.min_angle_deg);
        }
        if (m_impl->minIslandAreaSpin) {
            const QSignalBlocker b(m_impl->minIslandAreaSpin);
            m_impl->minIslandAreaSpin->setValue(snap.cdtConfig.min_island_area);
        }
        if (m_maxVertsSpin) {
            const QSignalBlocker b(m_maxVertsSpin);
            m_maxVertsSpin->setValue(static_cast<int>(
                std::min<uint32_t>(snap.cdtConfig.max_vertices, kMaxMeshVertices)));
        }
    }

    refreshLastResultView(cdtMinAngle, cdtMeanQuality, hitBudget, generatedVertCount);
    saveActiveObjectState();
    updateMeshStatsLabel();
    syncMeshToDaemon(snap);

    emit contourPinsChanged(m_objectId, m_pins);
    emit meshGenerated(m_objectId, m_lastResult.vertex_count, m_lastResult.triangle_count,
                       m_lastResult.achieved_min_angle_deg);
    emit wireframeOverlayRequested(isWireframeVisible(), m_lastResult);
    emit statusMessage(
        m_lastResult.vertex_count == 0u
            ? tr("Cleared cutout mesh for object #%1").arg(m_objectId)
            : tr("CDT mesh ready: %1 verts, %2 tris, min angle %3 deg")
                  .arg(m_lastResult.vertex_count)
                  .arg(m_lastResult.triangle_count)
                  .arg(static_cast<double>(m_lastResult.achieved_min_angle_deg), 0, 'f', 1));
}

void PuppetMeshPanel::onGenerateCutoutMeshClicked()
{
    const size_t requiredBytes = static_cast<size_t>(m_texWidth) * static_cast<size_t>(m_texHeight);
    if (m_alphaBuffer.empty() || m_texWidth == 0u || m_texHeight == 0u ||
        m_alphaBuffer.size() < requiredBytes) {
        emit statusMessage(tr("Load a valid source alpha buffer before generating mesh"));
        return;
    }

    cancelPendingTasks();
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    m_impl->cancelFlag = cancel;
    const uint64_t gen = ++m_impl->activeGeneration;
    m_impl->taskRunning = true;
    updateAsyncUiState();
    emit statusMessage(tr("Generating CDT cutout mesh..."));

    std::vector<uint8_t> alphaCopy = m_alphaBuffer;
    std::vector<we_cdt_pin_t> pinsCopy = m_pins;
    const uint32_t texW = m_texWidth;
    const uint32_t texH = m_texHeight;
    const we_mesh_gen_cfg_t cfg = currentConfig();
    const int32_t targetObjectId = m_objectId;
    QPointer<PuppetMeshPanel> self(this);

    m_impl->workerPool.start([self, gen, cancel, targetObjectId, texW, texH, cfg,
                              alpha = std::move(alphaCopy), pins = std::move(pinsCopy)]() mutable {
        if (cancel->load(std::memory_order_acquire)) {
            return;
        }

        we_mesh_gen_result_t rawResult{};
        const bool ok = we_mesh_gen_from_alpha(alpha.data(), texW, texH, texW, &cfg,
                                               pins.empty() ? nullptr : pins.data(),
                                               static_cast<uint32_t>(pins.size()), &rawResult);

        const bool valid = ok && rawResult.vertex_count >= kMinMeshVertices &&
                           rawResult.triangle_count >= 1u && rawResult.vertices &&
                           rawResult.indices &&
                           rawResult.index_count >= rawResult.triangle_count * 3u;

        std::vector<we_mdl_vertex_t> genVertices;
        std::vector<uint16_t> genIndices;
        float cdtMinAngle = 0.0f;
        float cdtMeanQuality = 0.0f;
        bool hitBudget = false;
        uint32_t generatedVertCount = 0u;

        if (valid && !cancel->load(std::memory_order_acquire)) {
            genVertices.assign(rawResult.vertices, rawResult.vertices + rawResult.vertex_count);
            genIndices.assign(rawResult.indices, rawResult.indices + rawResult.triangle_count * 3u);
            cdtMinAngle = rawResult.achieved_min_angle_deg;
            cdtMeanQuality = rawResult.achieved_mean_quality;
            hitBudget = rawResult.hit_vertex_budget;
            generatedVertCount = rawResult.vertex_count;
        }
        we_mesh_gen_result_free(&rawResult);

        if (!self || cancel->load(std::memory_order_acquire)) {
            return;
        }

        QMetaObject::invokeMethod(
            self.data(),
            [self, gen, cancel, targetObjectId, valid, cfg, cdtMinAngle, cdtMeanQuality, hitBudget,
             generatedVertCount, genVertices = std::move(genVertices),
             genIndices = std::move(genIndices), pins = std::move(pins)]() mutable {
                if (!self || cancel->load(std::memory_order_acquire) ||
                    gen != self->m_impl->activeGeneration) {
                    return;
                }
                self->m_impl->taskRunning = false;
                self->updateAsyncUiState();

                if (!valid) {
                    emit self->statusMessage(
                        tr("CDT mesh generation failed for current cutout settings"));
                    return;
                }

                PuppetMeshSnapshot oldMesh;
                oldMesh.objectId = targetObjectId;
                if (self->m_objectId == targetObjectId) {
                    oldMesh.vertices = self->m_impl->vertices;
                    oldMesh.indices = self->m_impl->indices;
                    oldMesh.cdtConfig = self->m_impl->appliedConfig;
                    oldMesh.pins = pins;
                } else if (const auto it = self->m_impl->objectStates.find(targetObjectId);
                           it != self->m_impl->objectStates.end()) {
                    oldMesh.vertices = it->second.vertices;
                    oldMesh.indices = it->second.indices;
                    oldMesh.cdtConfig = it->second.appliedConfig;
                    oldMesh.pins = it->second.pins;
                } else {
                    oldMesh.cdtConfig = cfg;
                    oldMesh.pins = pins;
                }

                PuppetMeshSnapshot newMesh;
                newMesh.objectId = targetObjectId;
                newMesh.vertices = std::move(genVertices);
                newMesh.indices = std::move(genIndices);
                newMesh.cdtConfig = cfg;
                newMesh.pins = std::move(pins);

                auto applyFn = [self, cdtMinAngle, cdtMeanQuality, hitBudget,
                                generatedVertCount](const PuppetMeshSnapshot &snap) {
                    if (self) {
                        self->applyMeshSnapshot(snap, cdtMinAngle, cdtMeanQuality, hitBudget,
                                                generatedVertCount);
                    }
                };

                if (self->m_commandBus) {
                    self->m_commandBus->execute(std::make_unique<MeshEditCommand>(
                        tr("Generate cutout mesh"), std::move(oldMesh), std::move(newMesh),
                        std::move(applyFn)));
                } else {
                    applyFn(newMesh);
                }
            },
            Qt::QueuedConnection);
    });
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

    auto finalizeExport = [this](int32_t objId, QString destPath) {
        if (destPath.isEmpty()) {
            emit statusMessage(tr("Canceled .mdl export for object #%1").arg(objId));
            return;
        }
        if (QFileInfo(destPath).suffix().isEmpty()) {
            destPath.append(QStringLiteral(".mdl"));
        }
        emit exportMdlRequested(objId, destPath);
        emit statusMessage(
            tr("Queued .mdl export request for object #%1 to %2").arg(objId).arg(destPath));
    };

    const QVariant headlessProp = property("headlessExportPath");
    if (headlessProp.isValid()) {
        finalizeExport(m_objectId, headlessProp.toString().trimmed());
        return;
    }
    if (QGuiApplication::platformName() == QLatin1String("offscreen")) {
        finalizeExport(m_objectId, defaultFile);
        return;
    }

    auto *dialog = new QFileDialog(this, tr("Export Puppet .mdl"), defaultFile,
                                   tr("Wallpaper Engine Model (*.mdl);;All Files (*)"));
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setFileMode(QFileDialog::AnyFile);
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    QPointer<PuppetMeshPanel> self(this);
    const int32_t exportObjId = m_objectId;
    connect(dialog, &QDialog::finished, this, [self, dialog, exportObjId](int result) {
        if (!self) {
            return;
        }
        if (result != QDialog::Accepted || dialog->selectedFiles().isEmpty()) {
            emit self->statusMessage(tr("Canceled .mdl export for object #%1").arg(exportObjId));
            return;
        }
        QString destPath = dialog->selectedFiles().constFirst().trimmed();
        if (destPath.isEmpty()) {
            emit self->statusMessage(tr("Canceled .mdl export for object #%1").arg(exportObjId));
            return;
        }
        if (QFileInfo(destPath).suffix().isEmpty()) {
            destPath.append(QStringLiteral(".mdl"));
        }
        emit self->exportMdlRequested(exportObjId, destPath);
        emit self->statusMessage(
            tr("Queued .mdl export request for object #%1 to %2").arg(exportObjId).arg(destPath));
    });
    dialog->open();
}

void PuppetMeshPanel::remapVertexBlendIndices(int32_t objectId,
                                              const std::vector<int32_t> &oldToNew)
{
    if (!m_impl || objectId < 0 || oldToNew.empty()) {
        return;
    }

    if (objectId == m_objectId) {
        remapVertexListBlendIndices(m_impl->vertices, oldToNew);
        m_lastResult.vertices = m_impl->vertices.empty() ? nullptr : m_impl->vertices.data();
        saveActiveObjectState();
        if (m_ipcClient && m_impl->vertices.size() >= kMinMeshVertices &&
            m_impl->indices.size() >= 3u) {
            m_ipcClient->sendPuppetSetMesh(static_cast<uint32_t>(objectId), m_impl->vertices,
                                           m_impl->indices, 0x3u);
        }
        return;
    }

    const auto it = m_impl->objectStates.find(objectId);
    if (it != m_impl->objectStates.end()) {
        remapVertexListBlendIndices(it->second.vertices, oldToNew);
    }
}

void PuppetMeshPanel::updateMeshStatsLabel()
{
    const uint32_t vCount = m_lastResult.vertex_count;
    const uint32_t tCount = m_lastResult.triangle_count;
    const float minAngle = m_lastResult.achieved_min_angle_deg;
    const float meanQ = m_lastResult.achieved_mean_quality;
    const bool hasMesh = (vCount >= kMinMeshVertices && tCount > 0u);

    if (m_impl && m_impl->triBadge) {
        m_impl->triBadge->setText(tr("TRIS: %1").arg(tCount));
        const QString triColor = hasMesh ? QStringLiteral("#f1f5f9") : QStringLiteral("#8b9bb4");
        m_impl->triBadge->setStyleSheet(
            QStringLiteral("QLabel { font-family: 'JetBrains Mono', monospace; font-size: 11px; "
                           "font-weight: 600; color: %1; background-color: #0b0c10; "
                           "border: 1px solid #2a2f3d; border-radius: 4px; padding: 4px 6px; }")
                .arg(triColor));
    }

    if (m_impl && m_impl->angleBadge) {
        if (!hasMesh) {
            m_impl->angleBadge->setText(tr("MIN ANGLE: -- deg"));
            m_impl->angleBadge->setStyleSheet(QStringLiteral(
                "QLabel { font-family: 'JetBrains Mono', monospace; font-size: 11px; "
                "font-weight: 600; color: #8b9bb4; background-color: #0b0c10; "
                "border: 1px solid #2a2f3d; border-radius: 4px; padding: 4px 6px; }"));
        } else {
            const bool meetsTarget = (minAngle >= kTargetMinAngleDeg);
            const QString angleColor =
                meetsTarget ? QStringLiteral("#a6e3a1") : QStringLiteral("#f9e2af");
            const QString borderCol =
                meetsTarget ? QStringLiteral("#2e5a44") : QStringLiteral("#5c4b23");
            m_impl->angleBadge->setText(
                tr("MIN ANGLE: %1 deg").arg(static_cast<double>(minAngle), 0, 'f', 1));
            m_impl->angleBadge->setStyleSheet(
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
    if (!m_ipcClient || snapshot.objectId < 0) {
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
    m_ipcClient->sendPuppetSetMesh(objId, verts, indices, flags);
}

} // namespace waywal::studio
