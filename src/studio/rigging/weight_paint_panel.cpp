// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/rigging/weight_paint_panel.hpp"

#include "waywal/ipc_proto_rigging.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <vector>

namespace waywal::studio {

namespace {

float clamp01(float v) noexcept { return std::clamp(v, 0.0f, 1.0f); }

/* Polynomial colormaps matching the GLES 3.10 weight heatmap fragment shader. */
QColor evalHeatmapColor(we_heatmap_palette_t palette, float t)
{
    t = clamp01(t);
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;

    if (palette == WE_HEATMAP_PALETTE_VIRIDIS) {
        const float t2 = t * t;
        const float t3 = t2 * t;
        const float t4 = t3 * t;
        const float t5 = t4 * t;
        const float t6 = t5 * t;
        r = 0.277727327f + 0.105093043f * t - 0.330861829f * t2 - 4.634230499f * t3 +
            6.228269936f * t4 + 4.776384998f * t5 - 5.435455856f * t6;
        g = 0.005407345f + 1.404613529f * t + 0.214847559f * t2 - 5.799100973f * t3 +
            14.179933367f * t4 - 13.745145377f * t5 + 4.645852612f * t6;
        b = 0.334099805f + 1.384590163f * t + 0.095095163f * t2 - 19.332440956f * t3 +
            56.690552601f * t4 - 65.353032633f * t5 + 26.312435249f * t6;
    } else if (palette == WE_HEATMAP_PALETTE_INFERNO) {
        const float t2 = t * t;
        const float t3 = t2 * t;
        const float t4 = t3 * t;
        const float t5 = t4 * t;
        const float t6 = t5 * t;
        r = 0.000218940f + 0.106513419f * t + 11.60249310f * t2 - 41.7039961f * t3 +
            77.16293570f * t4 - 71.3194282f * t5 + 25.13112620f * t6;
        g = 0.001651005f + 0.563956437f * t - 3.972853960f * t2 + 17.43639890f * t3 -
            33.40235890f * t4 + 32.62606430f * t5 - 12.24266890f * t6;
        b = -0.019480898f + 3.932712390f * t - 15.94239410f * t2 + 44.35414520f * t3 -
            81.80730930f * t4 + 73.20951990f * t5 - 23.07032500f * t6;
    } else {
        const float t2 = t * t;
        const float t3 = t2 * t;
        const float t4 = t3 * t;
        const float t5 = t4 * t;
        r = 0.13572138f + 4.61539260f * t - 42.66032258f * t2 + 132.13108234f * t3 -
            152.94239396f * t4 + 59.28637943f * t5;
        g = 0.09140261f + 2.19418839f * t + 4.84296658f * t2 - 14.18503333f * t3 +
            4.27729857f * t4 + 2.82956604f * t5;
        b = 0.10667330f + 12.64194608f * t - 60.58204836f * t2 + 110.36276771f * t3 -
            89.90310912f * t4 + 27.34824973f * t5;
    }

    QColor c;
    c.setRgbF(clamp01(r), clamp01(g), clamp01(b), 1.0f);
    return c;
}

class HeatmapLegendWidget final : public QWidget {
  public:
    explicit HeatmapLegendWidget(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumHeight(46);
        setMaximumHeight(52);
        setAccessibleName(QStringLiteral("Weight Heatmap Palette Legend"));
        setAccessibleDescription(
            QStringLiteral("Displays perceptual color ramp and 0.25 isoline ticks from 0.00 to "
                           "1.00"));
    }

    void setPalette(we_heatmap_palette_t palette)
    {
        if (m_palette != palette) {
            m_palette = palette;
            update();
        }
    }

  protected:
    void paintEvent(QPaintEvent *event) override
    {
        (void)event;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QRectF bounds = rect().adjusted(2.0, 2.0, -2.0, -2.0);
        p.setPen(QPen(QColor(QStringLiteral("#2a2f3d")), 1.0));
        p.setBrush(QColor(QStringLiteral("#0b0c10")));
        p.drawRoundedRect(bounds, 4.0, 4.0);

        const QRectF bar = bounds.adjusted(8.0, 6.0, -8.0, -18.0);
        if (bar.width() <= 4.0 || bar.height() <= 4.0) {
            return;
        }

        QLinearGradient grad(bar.topLeft(), bar.topRight());
        constexpr int kStops = 16;
        for (int i = 0; i <= kStops; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(kStops);
            QColor c = evalHeatmapColor(m_palette, u);
            if (!isEnabled()) {
                const int lum = qGray(c.rgb());
                c = QColor(lum, lum, lum, 128);
            }
            grad.setColorAt(static_cast<qreal>(u), c);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(grad);
        p.drawRoundedRect(bar, 2.0, 2.0);

        p.setPen(
            QPen(QColor(isEnabled() ? QStringLiteral("#f1f5f9") : QStringLiteral("#788496")), 1.0));
        for (int tick = 0; tick <= 4; ++tick) {
            const float u = static_cast<float>(tick) * 0.25f;
            const double x = bar.left() + bar.width() * u;
            p.drawLine(QPointF(x, bar.top()), QPointF(x, bar.bottom() + 2.0));
        }

        p.setFont(QFont(QStringLiteral("JetBrains Mono"), 8));
        p.setPen(QColor(isEnabled() ? QStringLiteral("#f1f5f9") : QStringLiteral("#788496")));
        const QRectF textRow(bar.left(), bar.bottom() + 2.0, bar.width(), 14.0);
        p.drawText(textRow, Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("0.00"));
        p.drawText(textRow, Qt::AlignHCenter | Qt::AlignVCenter, QStringLiteral("0.50"));
        p.drawText(textRow, Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("1.00"));
    }

  private:
    we_heatmap_palette_t m_palette{WE_HEATMAP_PALETTE_TURBO};
};

/* Stores per-vertex lock bitset and dominant bone indices inside the QObject child tree
 * so vertex locks stay decoupled from raw puppet pointers without expanding header state. */
class WeightLockState final : public QObject {
  public:
    explicit WeightLockState(QObject *parent = nullptr) : QObject(parent)
    {
        setObjectName(QStringLiteral("weightLockState"));
    }

    uint32_t vertexCount{0u};
    std::vector<uint64_t> lockedVertBits;
    std::vector<int16_t> dominantBones;
    bool updatingUi{false};

    void resetForPuppet(const we_puppet_t *puppet)
    {
        lockedVertBits.clear();
        dominantBones.clear();
        vertexCount = 0u;
        if (!puppet || puppet->vertex_count == 0u || !puppet->vertices) {
            return;
        }
        vertexCount = std::min<uint32_t>(puppet->vertex_count, WE_MDL_MAX_VERTICES);
        const size_t words = (static_cast<size_t>(vertexCount) + 63u) / 64u;
        lockedVertBits.assign(words, 0ull);
        dominantBones.assign(vertexCount, -1);

        for (uint32_t v = 0; v < vertexCount; ++v) {
            const we_mdl_vertex_t &vert = puppet->vertices[v];
            float bestW = 0.0f;
            int16_t bestBone = -1;
            for (int k = 0; k < 4; ++k) {
                if (vert.weight[k] > bestW) {
                    bestW = vert.weight[k];
                    bestBone = static_cast<int16_t>(vert.blend_indices[k]);
                }
            }
            dominantBones[v] = bestBone;
        }
    }

    [[nodiscard]] uint32_t lockedVertexCount() const noexcept
    {
        uint32_t count = 0u;
        for (uint64_t w : lockedVertBits) {
            count += static_cast<uint32_t>(std::popcount(w));
        }
        return std::min(count, vertexCount);
    }

    void setVerticesForBoneLocked(int32_t boneIndex, bool locked)
    {
        if (boneIndex < 0 || vertexCount == 0u || lockedVertBits.empty()) {
            return;
        }
        for (uint32_t v = 0; v < vertexCount; ++v) {
            if (dominantBones[v] == static_cast<int16_t>(boneIndex)) {
                const size_t word = v >> 6u;
                const uint64_t bit = 1ull << (v & 63u);
                if (locked) {
                    lockedVertBits[word] |= bit;
                } else {
                    lockedVertBits[word] &= ~bit;
                }
            }
        }
    }

    void unlockAllVertices() noexcept
    {
        std::fill(lockedVertBits.begin(), lockedVertBits.end(), 0ull);
    }
};

WeightLockState *findLockState(const QObject *owner)
{
    return static_cast<WeightLockState *>(
        owner->findChild<QObject *>(QStringLiteral("weightLockState")));
}

QDoubleSpinBox *makeWeightSpin(double minVal, double maxVal, double step, int decimals,
                               const QString &suffix, const QString &accessibleName,
                               QWidget *parent)
{
    auto *spin = new QDoubleSpinBox(parent);
    spin->setRange(minVal, maxVal);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    if (!suffix.isEmpty()) {
        spin->setSuffix(suffix);
    }
    spin->setKeyboardTracking(false);
    spin->setAccessibleName(accessibleName);
    return spin;
}

template <typename Client = IpcClient>
void dispatchHeatSolveIpc(Client *client, int32_t objectId, const we_heat_skin_cfg_t &cfg,
                          bool respectLockedVerts, const std::vector<uint64_t> &lockedVertBits)
{
    if (!client || objectId < 0) {
        return;
    }

    waywal_ipc_puppet_solve_weights_t req{};
    req.object_id = static_cast<uint32_t>(objectId);
    req.falloff_power = cfg.falloff_power;
    req.smoothness = cfg.smooth_factor;
    req.max_influences = cfg.max_influences;
    req.flags = 1u;
    if (respectLockedVerts && !lockedVertBits.empty()) {
        req.flags |= (1u << 1);
    }
    if (cfg.use_visibility_raycast) {
        req.flags |= (1u << 2);
    }
    if (cfg.use_intrinsic_delaunay) {
        req.flags |= (1u << 3);
    }
    req.locked_vert_word_count =
        ((req.flags & (1u << 1)) != 0u)
            ? static_cast<uint32_t>(std::min<size_t>(lockedVertBits.size(), 1024u))
            : 0u;

    if constexpr (requires(Client *c) { c->sendPuppetSolveWeights(req, lockedVertBits); }) {
        client->sendPuppetSolveWeights(req, lockedVertBits);
    } else if constexpr (requires(Client *c) { c->sendPuppetSolveWeights(req); }) {
        client->sendPuppetSolveWeights(req);
    } else if constexpr (requires(Client *c) { c->sendPuppetSolveWeights(objectId, cfg); }) {
        client->sendPuppetSolveWeights(objectId, cfg);
    } else {
        (void)req;
    }
}

template <typename Client = IpcClient>
void dispatchHeatmapModeIpc(Client *client, const waywal_ipc_puppet_heatmap_mode_t &req)
{
    if (!client) {
        return;
    }
    if constexpr (requires(Client *c) { c->sendPuppetHeatmapMode(req); }) {
        client->sendPuppetHeatmapMode(req);
    } else if constexpr (requires(Client *c) {
                             c->sendPuppetHeatmapMode(
                                 req.object_id, req.enabled != 0u, req.active_bone_idx, req.opacity,
                                 static_cast<we_heatmap_palette_t>(req.palette));
                         }) {
        client->sendPuppetHeatmapMode(req.object_id, req.enabled != 0u, req.active_bone_idx,
                                      req.opacity, static_cast<we_heatmap_palette_t>(req.palette));
    } else {
        (void)req;
    }
}

} // namespace

WeightPaintPanel::WeightPaintPanel(QWidget *parent) : QWidget(parent)
{
    new WeightLockState(this);
    setupUi();
}

void WeightPaintPanel::setupUi()
{
    setStyleSheet(QStringLiteral(
        "QWidget { background-color: #12141a; color: #f1f5f9; font-size: 12px; }"
        "QGroupBox#weightSectionGroup { background-color: #181b22; border: 1px solid #2a2f3d; "
        "border-radius: 6px; margin-top: 10px; padding-top: 10px; font-weight: 600; "
        "color: #f1f5f9; }"
        "QGroupBox#weightSectionGroup::title { subcontrol-origin: margin; left: 8px; padding: 0 "
        "4px; color: #94a3b8; }"
        "QListWidget { background-color: #0b0c10; color: #f1f5f9; border: 1px solid #2a2f3d; "
        "border-radius: 4px; alternate-background-color: #12141a; }"
        "QListWidget::item:selected { background-color: #1e3a5f; color: #f1f5f9; }"
        "QComboBox, QDoubleSpinBox, QSpinBox, QPushButton { background-color: #0b0c10; "
        "color: #f1f5f9; border: 1px solid #2a2f3d; border-radius: 4px; padding: 3px 6px; }"
        "QPushButton { background-color: #181b22; }"
        "QPushButton:hover, QComboBox:hover, QDoubleSpinBox:hover, QSpinBox:hover { "
        "background-color: #222631; border-color: #38bdf8; }"
        "QPushButton[activeToggle=\"true\"]:checked { border: 1px solid #38bdf8; "
        "background-color: #0c2d48; color: #38bdf8; }"
        "QComboBox:focus, QDoubleSpinBox:focus, QSpinBox:focus, QPushButton:focus, "
        "QCheckBox:focus, QSlider:focus, QListWidget:focus { border: 2px solid #38bdf8; outline: "
        "none; }"
        "QComboBox:disabled, QDoubleSpinBox:disabled, QSpinBox:disabled, QPushButton:disabled, "
        "QCheckBox:disabled, QSlider:disabled { color: #788496; background-color: #12141a; "
        "border-color: #222631; }"
        "QDoubleSpinBox, QSpinBox { font-family: 'JetBrains Mono', monospace; font-size: 12px; }"));

    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(6, 6, 6, 6);
    outerLayout->setSpacing(6);

    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto *container = new QWidget(scrollArea);
    auto *root = new QVBoxLayout(container);
    root->setContentsMargins(2, 2, 4, 2);
    root->setSpacing(8);

    // -------------------------------------------------------------------------
    // Section 1: Bone Influences & Bone / Vertex Locking
    // -------------------------------------------------------------------------
    auto *boneGroup = new QGroupBox(tr("Bone Influences & Vertex Locks"), container);
    boneGroup->setObjectName(QStringLiteral("weightSectionGroup"));
    auto *boneLayout = new QVBoxLayout(boneGroup);
    boneLayout->setContentsMargins(8, 8, 8, 8);
    boneLayout->setSpacing(6);

    m_boneWeightList = new QListWidget(boneGroup);
    m_boneWeightList->setAlternatingRowColors(true);
    m_boneWeightList->setMinimumHeight(110);
    m_boneWeightList->setMaximumHeight(160);
    m_boneWeightList->setAccessibleName(tr("Bone Influence List"));
    m_boneWeightList->setAccessibleDescription(
        tr("Select an active bone to paint weights, or check a bone to lock its weight sum"));
    connect(m_boneWeightList, &QListWidget::currentRowChanged, this, [this](int row) {
        auto *lockState = findLockState(this);
        if ((lockState && lockState->updatingUi) || row < 0) {
            return;
        }
        setActiveBone(row);
    });
    connect(m_boneWeightList, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        auto *lockState = findLockState(this);
        if (!item || (lockState && lockState->updatingUi)) {
            return;
        }
        const uint32_t boneIdx = item->data(Qt::UserRole).toUInt();
        setBoneLocked(boneIdx, item->checkState() == Qt::Checked);
    });
    boneLayout->addWidget(m_boneWeightList);

    auto *boneLockRow = new QHBoxLayout();
    boneLockRow->setSpacing(6);

    auto *lockBoneBtn = new QPushButton(tr("Lock Bone"), boneGroup);
    lockBoneBtn->setObjectName(QStringLiteral("lockBoneBtn"));
    lockBoneBtn->setCheckable(true);
    lockBoneBtn->setProperty("activeToggle", true);
    lockBoneBtn->setAccessibleName(tr("Toggle Active Bone Weight Lock"));
    lockBoneBtn->setToolTip(tr("Freeze weights on the active bone during brush & normalize ops"));
    connect(lockBoneBtn, &QPushButton::toggled, this, [this](bool checked) {
        if (m_activeBoneIndex >= 0) {
            setBoneLocked(static_cast<uint32_t>(m_activeBoneIndex), checked);
        }
    });

    auto *unlockBonesBtn = new QPushButton(tr("Unlock All Bones"), boneGroup);
    unlockBonesBtn->setObjectName(QStringLiteral("unlockBonesBtn"));
    unlockBonesBtn->setEnabled(false);
    unlockBonesBtn->setAccessibleName(tr("Unlock All Bones"));
    connect(unlockBonesBtn, &QPushButton::clicked, this, [this]() {
        m_lockedBoneMask = {0ull, 0ull};
        for (uint32_t i = 0; i < WE_MDL_MAX_BONES; ++i) {
            setBoneLocked(i, false);
        }
        emit statusMessage(tr("Unlocked all bone weights"));
    });

    boneLockRow->addWidget(lockBoneBtn);
    boneLockRow->addWidget(unlockBonesBtn);
    boneLayout->addLayout(boneLockRow);

    auto *vertLockRow = new QHBoxLayout();
    vertLockRow->setSpacing(6);

    auto *lockVertToggleBtn = new QPushButton(tr("Lock Bone Vertices"), boneGroup);
    lockVertToggleBtn->setObjectName(QStringLiteral("lockVertToggleBtn"));
    lockVertToggleBtn->setCheckable(true);
    lockVertToggleBtn->setProperty("activeToggle", true);
    lockVertToggleBtn->setAccessibleName(tr("Toggle Vertex Lock for Active Bone"));
    lockVertToggleBtn->setToolTip(
        tr("Lock or unlock vertices predominantly bound to the active bone"));

    auto *unlockAllVertsBtn = new QPushButton(tr("Unlock All Vertices"), boneGroup);
    unlockAllVertsBtn->setObjectName(QStringLiteral("unlockAllVertsBtn"));
    unlockAllVertsBtn->setEnabled(false);
    unlockAllVertsBtn->setAccessibleName(tr("Unlock All Vertices"));

    auto updateVertLockUi = [this, lockVertToggleBtn, unlockAllVertsBtn]() {
        auto *lockState = findLockState(this);
        const uint32_t lockedCount = lockState ? lockState->lockedVertexCount() : 0u;
        unlockAllVertsBtn->setEnabled(lockedCount > 0u);
        if (m_solveTelemetryLabel) {
            m_solveTelemetryLabel->setText(tr("Vertex Locks: %1 locked | Active Bone: #%2")
                                               .arg(lockedCount)
                                               .arg(m_activeBoneIndex));
        }
        (void)lockVertToggleBtn;
    };

    connect(lockVertToggleBtn, &QPushButton::toggled, this, [this, updateVertLockUi](bool checked) {
        auto *lockState = findLockState(this);
        if (!lockState) {
            return;
        }
        lockState->setVerticesForBoneLocked(m_activeBoneIndex, checked);
        updateVertLockUi();
        emit statusMessage(checked
                               ? tr("Locked vertices bound to bone #%1").arg(m_activeBoneIndex)
                               : tr("Unlocked vertices bound to bone #%1").arg(m_activeBoneIndex));
    });

    connect(unlockAllVertsBtn, &QPushButton::clicked, this,
            [this, lockVertToggleBtn, updateVertLockUi]() {
                auto *lockState = findLockState(this);
                if (lockState) {
                    lockState->unlockAllVertices();
                }
                {
                    const QSignalBlocker b(lockVertToggleBtn);
                    lockVertToggleBtn->setChecked(false);
                }
                updateVertLockUi();
                emit statusMessage(tr("Unlocked all mesh vertices"));
            });

    vertLockRow->addWidget(lockVertToggleBtn);
    vertLockRow->addWidget(unlockAllVertsBtn);
    boneLayout->addLayout(vertLockRow);
    root->addWidget(boneGroup);

    // -------------------------------------------------------------------------
    // Section 2: Harmonic Heat Diffusion Solver
    // -------------------------------------------------------------------------
    auto *heatGroup = new QGroupBox(tr("Harmonic Heat Diffusion Solver"), container);
    heatGroup->setObjectName(QStringLiteral("weightSectionGroup"));
    auto *heatGrid = new QGridLayout(heatGroup);
    heatGrid->setContentsMargins(8, 8, 8, 8);
    heatGrid->setSpacing(6);

    m_heatFalloffSpin =
        makeWeightSpin(0.5, 8.0, 0.25, 2, QString(), tr("Heat Distance Falloff Power"), heatGroup);
    m_heatFalloffSpin->setValue(2.0);
    m_heatSmoothSpin = makeWeightSpin(0.0, 1.0, 0.05, 2, QString(),
                                      tr("Post-Solve Cotangent Smoothing"), heatGroup);
    m_heatSmoothSpin->setValue(0.25);

    auto *conductivitySpin =
        makeWeightSpin(0.01, 10.0, 0.05, 3, QString(),
                       tr("Heat Conductivity and Tikhonov Regularization Scale"), heatGroup);
    conductivitySpin->setObjectName(QStringLiteral("conductivitySpin"));
    conductivitySpin->setValue(1.0);
    conductivitySpin->setToolTip(
        tr("Scales heat diffusion conductivity (paired with 1e-3 Tikhonov island "
           "regularization)"));

    auto *maxInfSpin = new QSpinBox(heatGroup);
    maxInfSpin->setObjectName(QStringLiteral("maxInfSpin"));
    maxInfSpin->setRange(1, 4);
    maxInfSpin->setValue(4);
    maxInfSpin->setKeyboardTracking(false);
    maxInfSpin->setAccessibleName(tr("Maximum Bone Influences Per Vertex"));

    heatGrid->addWidget(new QLabel(tr("Falloff Power:"), heatGroup), 0, 0);
    heatGrid->addWidget(m_heatFalloffSpin, 0, 1);
    heatGrid->addWidget(new QLabel(tr("Smoothing:"), heatGroup), 0, 2);
    heatGrid->addWidget(m_heatSmoothSpin, 0, 3);

    heatGrid->addWidget(new QLabel(tr("Conductivity:"), heatGroup), 1, 0);
    heatGrid->addWidget(conductivitySpin, 1, 1);
    heatGrid->addWidget(new QLabel(tr("Max Influences:"), heatGroup), 1, 2);
    heatGrid->addWidget(maxInfSpin, 1, 3);

    m_visibilityRaycastCheck =
        new QCheckBox(tr("Visibility Raycast (Prevent Cross-Gap Bleed)"), heatGroup);
    m_visibilityRaycastCheck->setChecked(true);
    m_visibilityRaycastCheck->setAccessibleName(tr("Use Visibility Raycast"));
    heatGrid->addWidget(m_visibilityRaycastCheck, 2, 0, 1, 4);

    auto *idtCheck = new QCheckBox(tr("Intrinsic Delaunay (iDT M-Matrix Guarantee)"), heatGroup);
    idtCheck->setObjectName(QStringLiteral("idtCheck"));
    idtCheck->setChecked(true);
    idtCheck->setAccessibleName(tr("Use Intrinsic Delaunay Triangulation"));
    heatGrid->addWidget(idtCheck, 3, 0, 1, 4);

    auto *respectLockedVertsCheck =
        new QCheckBox(tr("Respect Locked Vertices During Heat Solve"), heatGroup);
    respectLockedVertsCheck->setObjectName(QStringLiteral("respectLockedVertsCheck"));
    respectLockedVertsCheck->setChecked(true);
    respectLockedVertsCheck->setAccessibleName(tr("Respect Locked Vertices"));
    heatGrid->addWidget(respectLockedVertsCheck, 4, 0, 1, 4);

    m_solveHeatBtn = new QPushButton(tr("Solve Harmonic Heat Weights"), heatGroup);
    m_solveHeatBtn->setAccessibleName(tr("Solve Harmonic Heat Weights"));
    m_solveHeatBtn->setToolTip(
        tr("Compute automatic visibility-aware cotangent heat skinning weights"));
    connect(m_solveHeatBtn, &QPushButton::clicked, this,
            &WeightPaintPanel::onSolveHeatWeightsClicked);
    heatGrid->addWidget(m_solveHeatBtn, 5, 0, 1, 4);

    root->addWidget(heatGroup);

    // -------------------------------------------------------------------------
    // Section 3: Weight Paint Brush Controls
    // -------------------------------------------------------------------------
    auto *brushGroup = new QGroupBox(tr("Weight Paint Brush"), container);
    brushGroup->setObjectName(QStringLiteral("weightSectionGroup"));
    auto *brushGrid = new QGridLayout(brushGroup);
    brushGrid->setContentsMargins(8, 8, 8, 8);
    brushGrid->setSpacing(6);

    m_brushOpCombo = new QComboBox(brushGroup);
    m_brushOpCombo->addItem(tr("Add"), static_cast<uint32_t>(WE_WEIGHT_OP_ADD));
    m_brushOpCombo->addItem(tr("Subtract"), static_cast<uint32_t>(WE_WEIGHT_OP_SUBTRACT));
    m_brushOpCombo->addItem(tr("Replace"), static_cast<uint32_t>(WE_WEIGHT_OP_REPLACE));
    m_brushOpCombo->addItem(tr("Smooth"), static_cast<uint32_t>(WE_WEIGHT_OP_SMOOTH));
    m_brushOpCombo->addItem(tr("Normalize"), static_cast<uint32_t>(WE_WEIGHT_OP_NORMALIZE));
    m_brushOpCombo->setAccessibleName(tr("Weight Brush Mode"));
    brushGrid->addWidget(new QLabel(tr("Brush Mode:"), brushGroup), 0, 0);
    brushGrid->addWidget(m_brushOpCombo, 0, 1, 1, 3);

    m_radiusSpin = makeWeightSpin(1.0, 512.0, 2.0, 1, QStringLiteral(" px"),
                                  tr("Weight Brush Radius"), brushGroup);
    m_radiusSpin->setValue(32.0);
    m_strengthSpin =
        makeWeightSpin(0.0, 1.0, 0.05, 2, QString(), tr("Weight Brush Strength"), brushGroup);
    m_strengthSpin->setValue(0.50);

    brushGrid->addWidget(new QLabel(tr("Radius:"), brushGroup), 1, 0);
    brushGrid->addWidget(m_radiusSpin, 1, 1);
    brushGrid->addWidget(new QLabel(tr("Strength:"), brushGroup), 1, 2);
    brushGrid->addWidget(m_strengthSpin, 1, 3);

    auto *brushFalloffSpin =
        makeWeightSpin(0.25, 4.0, 0.1, 2, QString(), tr("Brush Edge Falloff"), brushGroup);
    brushFalloffSpin->setObjectName(QStringLiteral("brushFalloffSpin"));
    brushFalloffSpin->setValue(1.50);

    m_targetWeightSpin =
        makeWeightSpin(0.0, 1.0, 0.05, 2, QString(), tr("Replace Target Weight"), brushGroup);
    m_targetWeightSpin->setValue(1.00);
    m_targetWeightSpin->setEnabled(false);

    brushGrid->addWidget(new QLabel(tr("Falloff:"), brushGroup), 2, 0);
    brushGrid->addWidget(brushFalloffSpin, 2, 1);
    brushGrid->addWidget(new QLabel(tr("Target Weight:"), brushGroup), 2, 2);
    brushGrid->addWidget(m_targetWeightSpin, 2, 3);

    m_normalizeBtn = new QPushButton(tr("Normalize All Weights"), brushGroup);
    m_normalizeBtn->setAccessibleName(tr("Normalize All Unlocked Weights"));
    connect(m_normalizeBtn, &QPushButton::clicked, this, &WeightPaintPanel::onNormalizeAllClicked);
    brushGrid->addWidget(m_normalizeBtn, 3, 0, 1, 4);

    connect(m_brushOpCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        if (m_targetWeightSpin) {
            m_targetWeightSpin->setEnabled(activeBrushOp() == WE_WEIGHT_OP_REPLACE);
        }
        emit brushParamsChanged();
    });
    for (QDoubleSpinBox *s : {m_radiusSpin, m_strengthSpin, brushFalloffSpin, m_targetWeightSpin}) {
        connect(s, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                &WeightPaintPanel::brushParamsChanged);
    }

    root->addWidget(brushGroup);

    // -------------------------------------------------------------------------
    // Section 4: Perceptual Weight Heatmap Overlay & Swatch Legend
    // -------------------------------------------------------------------------
    auto *heatmapGroup = new QGroupBox(tr("Weight Heatmap Overlay"), container);
    heatmapGroup->setObjectName(QStringLiteral("weightSectionGroup"));
    auto *heatmapGrid = new QGridLayout(heatmapGroup);
    heatmapGrid->setContentsMargins(8, 8, 8, 8);
    heatmapGrid->setSpacing(6);

    m_heatmapEnableCheck = new QCheckBox(tr("Enable Heatmap Overlay"), heatmapGroup);
    m_heatmapEnableCheck->setChecked(false);
    m_heatmapEnableCheck->setAccessibleName(tr("Enable Weight Heatmap Overlay"));
    connect(m_heatmapEnableCheck, &QCheckBox::toggled, this,
            &WeightPaintPanel::onHeatmapToggleChanged);
    heatmapGrid->addWidget(m_heatmapEnableCheck, 0, 0, 1, 2);

    m_paletteCombo = new QComboBox(heatmapGroup);
    m_paletteCombo->addItem(tr("Turbo (Rainbow)"), static_cast<uint32_t>(WE_HEATMAP_PALETTE_TURBO));
    m_paletteCombo->addItem(tr("Viridis (Perceptual)"),
                            static_cast<uint32_t>(WE_HEATMAP_PALETTE_VIRIDIS));
    m_paletteCombo->addItem(tr("Inferno (High Contrast)"),
                            static_cast<uint32_t>(WE_HEATMAP_PALETTE_INFERNO));
    m_paletteCombo->setAccessibleName(tr("Heatmap Color Palette"));
    m_paletteCombo->setEnabled(false);

    m_heatmapOpacitySlider = new QSlider(Qt::Horizontal, heatmapGroup);
    m_heatmapOpacitySlider->setRange(0, 100);
    m_heatmapOpacitySlider->setValue(75);
    m_heatmapOpacitySlider->setAccessibleName(tr("Heatmap Overlay Opacity"));
    m_heatmapOpacitySlider->setEnabled(false);

    auto *legend = new HeatmapLegendWidget(heatmapGroup);
    legend->setObjectName(QStringLiteral("heatmapLegend"));
    legend->setEnabled(false);

    connect(
        m_paletteCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, legend](int) {
            legend->setPalette(activePalette());
            sendHeatmapModeIpc();
            const float opacity = m_heatmapOpacitySlider
                                      ? static_cast<float>(m_heatmapOpacitySlider->value()) / 100.0f
                                      : 0.75f;
            const uint32_t boneIdx =
                (m_activeBoneIndex >= 0) ? static_cast<uint32_t>(m_activeBoneIndex) : 0xFFFFFFFFu;
            emit heatmapModeChanged(m_objectId,
                                    m_heatmapEnableCheck && m_heatmapEnableCheck->isChecked(),
                                    boneIdx, opacity, activePalette());
        });

    connect(m_heatmapOpacitySlider, &QSlider::valueChanged, this, [this](int val) {
        sendHeatmapModeIpc();
        const uint32_t boneIdx =
            (m_activeBoneIndex >= 0) ? static_cast<uint32_t>(m_activeBoneIndex) : 0xFFFFFFFFu;
        emit heatmapModeChanged(m_objectId,
                                m_heatmapEnableCheck && m_heatmapEnableCheck->isChecked(), boneIdx,
                                static_cast<float>(val) / 100.0f, activePalette());
    });

    heatmapGrid->addWidget(new QLabel(tr("Palette:"), heatmapGroup), 1, 0);
    heatmapGrid->addWidget(m_paletteCombo, 1, 1);
    heatmapGrid->addWidget(new QLabel(tr("Opacity:"), heatmapGroup), 2, 0);
    heatmapGrid->addWidget(m_heatmapOpacitySlider, 2, 1);
    heatmapGrid->addWidget(legend, 3, 0, 1, 2);

    root->addWidget(heatmapGroup);
    root->addStretch(1);

    scrollArea->setWidget(container);
    outerLayout->addWidget(scrollArea, 1);

    m_solveTelemetryLabel = new QLabel(tr("Ready: no puppet loaded"), this);
    m_solveTelemetryLabel->setWordWrap(true);
    m_solveTelemetryLabel->setAccessibleName(tr("Weight Solver Telemetry"));
    m_solveTelemetryLabel->setStyleSheet(
        QStringLiteral("QLabel { font-family: 'JetBrains Mono', monospace; font-size: 11px; "
                       "color: #94a3b8; background-color: #181b22; border: 1px solid #2a2f3d; "
                       "border-radius: 4px; padding: 6px; }"));
    outerLayout->addWidget(m_solveTelemetryLabel);

    setTabOrder(m_boneWeightList, lockBoneBtn);
    setTabOrder(lockBoneBtn, unlockBonesBtn);
    setTabOrder(unlockBonesBtn, lockVertToggleBtn);
    setTabOrder(lockVertToggleBtn, unlockAllVertsBtn);
    setTabOrder(unlockAllVertsBtn, m_heatFalloffSpin);
    setTabOrder(m_heatFalloffSpin, m_heatSmoothSpin);
    setTabOrder(m_heatSmoothSpin, conductivitySpin);
    setTabOrder(conductivitySpin, maxInfSpin);
    setTabOrder(maxInfSpin, m_visibilityRaycastCheck);
    setTabOrder(m_visibilityRaycastCheck, idtCheck);
    setTabOrder(idtCheck, respectLockedVertsCheck);
    setTabOrder(respectLockedVertsCheck, m_solveHeatBtn);
    setTabOrder(m_solveHeatBtn, m_brushOpCombo);
    setTabOrder(m_brushOpCombo, m_radiusSpin);
    setTabOrder(m_radiusSpin, m_strengthSpin);
    setTabOrder(m_strengthSpin, brushFalloffSpin);
    setTabOrder(brushFalloffSpin, m_targetWeightSpin);
    setTabOrder(m_targetWeightSpin, m_normalizeBtn);
    setTabOrder(m_normalizeBtn, m_heatmapEnableCheck);
    setTabOrder(m_heatmapEnableCheck, m_paletteCombo);
    setTabOrder(m_paletteCombo, m_heatmapOpacitySlider);
}

void WeightPaintPanel::setPuppetContext(int32_t objectId, const we_puppet_t *puppet)
{
    m_objectId = objectId;
    m_lockedBoneMask = {0ull, 0ull};

    auto *lockState = findLockState(this);
    if (lockState) {
        lockState->resetForPuppet(puppet);
    }

    if (m_boneWeightList) {
        bool fallbackUpdating = false;
        const QScopedValueRollback<bool> guard(lockState ? lockState->updatingUi : fallbackUpdating,
                                               true);
        const QSignalBlocker blocker(m_boneWeightList);
        m_boneWeightList->clear();

        if (puppet && puppet->bone_count > 0u && puppet->bones) {
            const uint32_t count = std::min<uint32_t>(puppet->bone_count, WE_MDL_MAX_BONES);
            for (uint32_t i = 0; i < count; ++i) {
                QString name = QString::fromUtf8(puppet->bones[i].name).trimmed();
                if (name.isEmpty()) {
                    name = QStringLiteral("Bone_%1").arg(i);
                }
                auto *item = new QListWidgetItem(QStringLiteral("[%1] %2").arg(i).arg(name));
                item->setData(Qt::UserRole, static_cast<uint32_t>(i));
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(Qt::Unchecked);
                m_boneWeightList->addItem(item);
            }
            m_activeBoneIndex = 0;
            m_boneWeightList->setCurrentRow(0);
        } else {
            m_activeBoneIndex = 0;
        }
    }

    if (auto *unlockBonesBtn = findChild<QPushButton *>(QStringLiteral("unlockBonesBtn"))) {
        unlockBonesBtn->setEnabled(false);
    }
    if (auto *unlockAllVertsBtn = findChild<QPushButton *>(QStringLiteral("unlockAllVertsBtn"))) {
        unlockAllVertsBtn->setEnabled(false);
    }
    if (auto *lockBoneBtn = findChild<QPushButton *>(QStringLiteral("lockBoneBtn"))) {
        const QSignalBlocker b(lockBoneBtn);
        lockBoneBtn->setChecked(false);
    }
    if (m_solveTelemetryLabel) {
        if (puppet) {
            m_solveTelemetryLabel->setText(tr("Puppet #%1: %2 verts, %3 bones")
                                               .arg(objectId)
                                               .arg(puppet->vertex_count)
                                               .arg(puppet->bone_count));
        } else {
            m_solveTelemetryLabel->setText(tr("Ready: no puppet loaded"));
        }
    }

    sendHeatmapModeIpc();
}

void WeightPaintPanel::setActiveBone(int32_t boneIndex)
{
    if (boneIndex < 0 || boneIndex >= static_cast<int32_t>(WE_MDL_MAX_BONES)) {
        return;
    }
    m_activeBoneIndex = boneIndex;

    auto *lockState = findLockState(this);
    if (m_boneWeightList && boneIndex < m_boneWeightList->count()) {
        bool fallbackUpdating = false;
        const QScopedValueRollback<bool> guard(lockState ? lockState->updatingUi : fallbackUpdating,
                                               true);
        const QSignalBlocker blocker(m_boneWeightList);
        m_boneWeightList->setCurrentRow(boneIndex);
    }
    if (auto *lockBoneBtn = findChild<QPushButton *>(QStringLiteral("lockBoneBtn"))) {
        const QSignalBlocker b(lockBoneBtn);
        lockBoneBtn->setChecked(isBoneLocked(static_cast<uint32_t>(boneIndex)));
    }

    if (m_heatmapEnableCheck && m_heatmapEnableCheck->isChecked()) {
        sendHeatmapModeIpc();
        const float opacity = m_heatmapOpacitySlider
                                  ? static_cast<float>(m_heatmapOpacitySlider->value()) / 100.0f
                                  : 0.75f;
        emit heatmapModeChanged(m_objectId, true, static_cast<uint32_t>(m_activeBoneIndex), opacity,
                                activePalette());
    }
}

void WeightPaintPanel::setBoneLocked(uint32_t boneIndex, bool locked)
{
    if (boneIndex >= WE_MDL_MAX_BONES) {
        return;
    }
    const size_t word = boneIndex >> 6u;
    const uint64_t bit = 1ull << (boneIndex & 63u);
    if (locked) {
        m_lockedBoneMask[word] |= bit;
    } else {
        m_lockedBoneMask[word] &= ~bit;
    }

    auto *lockState = findLockState(this);
    if (m_boneWeightList && static_cast<int>(boneIndex) < m_boneWeightList->count()) {
        bool fallbackUpdating = false;
        const QScopedValueRollback<bool> guard(lockState ? lockState->updatingUi : fallbackUpdating,
                                               true);
        if (QListWidgetItem *item = m_boneWeightList->item(static_cast<int>(boneIndex))) {
            item->setCheckState(locked ? Qt::Checked : Qt::Unchecked);
        }
    }

    if (static_cast<int32_t>(boneIndex) == m_activeBoneIndex) {
        if (auto *lockBoneBtn = findChild<QPushButton *>(QStringLiteral("lockBoneBtn"))) {
            const QSignalBlocker b(lockBoneBtn);
            lockBoneBtn->setChecked(locked);
        }
    }
    if (auto *unlockBonesBtn = findChild<QPushButton *>(QStringLiteral("unlockBonesBtn"))) {
        unlockBonesBtn->setEnabled((m_lockedBoneMask[0] | m_lockedBoneMask[1]) != 0ull);
    }
}

bool WeightPaintPanel::isBoneLocked(uint32_t boneIndex) const noexcept
{
    if (boneIndex >= WE_MDL_MAX_BONES) {
        return false;
    }
    const size_t word = boneIndex >> 6u;
    const uint64_t bit = 1ull << (boneIndex & 63u);
    return (m_lockedBoneMask[word] & bit) != 0ull;
}

we_weight_brush_op_t WeightPaintPanel::activeBrushOp() const noexcept
{
    if (!m_brushOpCombo) {
        return WE_WEIGHT_OP_ADD;
    }
    const uint32_t v = m_brushOpCombo->currentData().toUInt();
    return (v <= static_cast<uint32_t>(WE_WEIGHT_OP_NORMALIZE))
               ? static_cast<we_weight_brush_op_t>(v)
               : WE_WEIGHT_OP_ADD;
}

we_heatmap_palette_t WeightPaintPanel::activePalette() const noexcept
{
    if (!m_paletteCombo) {
        return WE_HEATMAP_PALETTE_TURBO;
    }
    const uint32_t v = m_paletteCombo->currentData().toUInt();
    return (v <= static_cast<uint32_t>(WE_HEATMAP_PALETTE_INFERNO))
               ? static_cast<we_heatmap_palette_t>(v)
               : WE_HEATMAP_PALETTE_TURBO;
}

float WeightPaintPanel::brushRadiusPx() const noexcept
{
    return m_radiusSpin ? static_cast<float>(m_radiusSpin->value()) : 32.0f;
}

float WeightPaintPanel::brushStrength() const noexcept
{
    return m_strengthSpin ? static_cast<float>(m_strengthSpin->value()) : 0.5f;
}

float WeightPaintPanel::targetWeightValue() const noexcept
{
    return m_targetWeightSpin ? static_cast<float>(m_targetWeightSpin->value()) : 1.0f;
}

void WeightPaintPanel::onSolveHeatWeightsClicked()
{
    auto *lockState = findLockState(this);
    auto *conductivitySpin = findChild<QDoubleSpinBox *>(QStringLiteral("conductivitySpin"));
    auto *maxInfSpin = findChild<QSpinBox *>(QStringLiteral("maxInfSpin"));
    auto *idtCheck = findChild<QCheckBox *>(QStringLiteral("idtCheck"));
    auto *respectLockedVertsCheck =
        findChild<QCheckBox *>(QStringLiteral("respectLockedVertsCheck"));

    const bool respectLocked =
        respectLockedVertsCheck ? respectLockedVertsCheck->isChecked() : true;

    we_heat_skin_cfg_t cfg{};
    cfg.conductivity_scale =
        conductivitySpin ? static_cast<float>(conductivitySpin->value()) : 1.0f;
    cfg.falloff_power = m_heatFalloffSpin
                            ? std::clamp(static_cast<float>(m_heatFalloffSpin->value()), 0.5f, 8.0f)
                            : 2.0f;
    cfg.smooth_factor = m_heatSmoothSpin
                            ? std::clamp(static_cast<float>(m_heatSmoothSpin->value()), 0.0f, 1.0f)
                            : 0.25f;
    cfg.max_influences =
        maxInfSpin ? static_cast<uint32_t>(std::clamp(maxInfSpin->value(), 1, 4)) : 4u;
    cfg.prune_epsilon = 1e-4f;
    cfg.use_visibility_raycast =
        m_visibilityRaycastCheck ? m_visibilityRaycastCheck->isChecked() : true;
    cfg.use_intrinsic_delaunay = idtCheck ? idtCheck->isChecked() : true;
    cfg.locked_vert_bs = (respectLocked && lockState && !lockState->lockedVertBits.empty())
                             ? lockState->lockedVertBits.data()
                             : nullptr;

    if (m_solveTelemetryLabel) {
        m_solveTelemetryLabel->setText(tr("Heat Solve: falloff=%1, smooth=%2, iDT=%3, raycast=%4")
                                           .arg(static_cast<double>(cfg.falloff_power), 0, 'f', 2)
                                           .arg(static_cast<double>(cfg.smooth_factor), 0, 'f', 2)
                                           .arg(cfg.use_intrinsic_delaunay ? tr("on") : tr("off"))
                                           .arg(cfg.use_visibility_raycast ? tr("on") : tr("off")));
    }

    if (m_objectId >= 0) {
        const std::vector<uint64_t> emptyBits;
        dispatchHeatSolveIpc(m_ipcClient, m_objectId, cfg, respectLocked,
                             lockState ? lockState->lockedVertBits : emptyBits);
        emit heatSolveRequested(m_objectId, cfg);
        emit statusMessage(
            tr("Requested harmonic heat weight solve for puppet #%1").arg(m_objectId));
    } else {
        emit statusMessage(tr("No active puppet loaded for heat weight solve"));
    }
}

void WeightPaintPanel::onNormalizeAllClicked()
{
    if (m_objectId < 0) {
        emit statusMessage(tr("No active puppet loaded for weight normalization"));
        return;
    }

    emit normalizeAllWeightsRequested(m_objectId, m_lockedBoneMask[0], m_lockedBoneMask[1]);
    emit statusMessage(
        tr("Normalized unlocked vertex weights for puppet #%1 (respecting bone locks)")
            .arg(m_objectId));
}

void WeightPaintPanel::setSolveTelemetry(uint32_t vertexCount, uint32_t boneCount, float elapsedMs,
                                         bool usedIc0Fallback)
{
    if (!m_solveTelemetryLabel) {
        return;
    }

    const QString solverTag = usedIc0Fallback ? tr("IC(0) fallback") : tr("Cholesky");
    m_solveTelemetryLabel->setText(tr("Solved %1 verts / %2 bones in %3 ms (%4)")
                                       .arg(vertexCount)
                                       .arg(boneCount)
                                       .arg(static_cast<double>(elapsedMs), 0, 'f', 2)
                                       .arg(solverTag));
}

void WeightPaintPanel::onHeatmapToggleChanged(bool enabled)
{
    if (m_heatmapEnableCheck && m_heatmapEnableCheck->isChecked() != enabled) {
        const QSignalBlocker blocker(m_heatmapEnableCheck);
        m_heatmapEnableCheck->setChecked(enabled);
    }
    if (m_paletteCombo) {
        m_paletteCombo->setEnabled(enabled);
    }
    if (m_heatmapOpacitySlider) {
        m_heatmapOpacitySlider->setEnabled(enabled);
    }
    if (auto *legend = findChild<QWidget *>(QStringLiteral("heatmapLegend"))) {
        legend->setEnabled(enabled);
    }

    sendHeatmapModeIpc();

    const float opacity = m_heatmapOpacitySlider
                              ? static_cast<float>(m_heatmapOpacitySlider->value()) / 100.0f
                              : 0.75f;
    const uint32_t boneIdx =
        (m_activeBoneIndex >= 0) ? static_cast<uint32_t>(m_activeBoneIndex) : 0xFFFFFFFFu;
    emit heatmapModeChanged(m_objectId, enabled, boneIdx, opacity, activePalette());
}

void WeightPaintPanel::sendHeatmapModeIpc()
{
    if (!m_ipcClient || m_objectId < 0) {
        return;
    }

    waywal_ipc_puppet_heatmap_mode_t req{};
    req.object_id = static_cast<uint32_t>(m_objectId);
    req.enabled = (m_heatmapEnableCheck && m_heatmapEnableCheck->isChecked()) ? 1u : 0u;
    req.palette = static_cast<uint16_t>(activePalette());
    req.active_bone_idx =
        (m_activeBoneIndex >= 0) ? static_cast<uint32_t>(m_activeBoneIndex) : 0xFFFFFFFFu;
    req.opacity = m_heatmapOpacitySlider
                      ? static_cast<float>(m_heatmapOpacitySlider->value()) / 100.0f
                      : 0.75f;

    dispatchHeatmapModeIpc(m_ipcClient, req);
}

} // namespace waywal::studio
