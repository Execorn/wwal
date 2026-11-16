// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/rigging/weight_paint_panel.hpp"

#include "waywal/ipc_proto_rigging.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPointer>
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
#include <cstddef>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace waywal::studio {

namespace {

constexpr uint32_t kWeightBlockSize = 256u;

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

struct ObjectWeightState {
    std::array<uint64_t, 2> lockedBoneMask{0ull, 0ull};
    int32_t activeBoneIndex{-1};
    uint32_t boneCount{0u};
    uint32_t vertexCount{0u};
    std::vector<uint64_t> lockedVertBits;
    std::vector<int16_t> dominantBones;
    std::vector<we_mdl_vertex_t> vertices;
};

[[nodiscard]] int16_t computeDominantBone(const we_mdl_vertex_t &vert) noexcept
{
    float bestW = 0.0f;
    int16_t bestBone = -1;
    for (int k = 0; k < 4; ++k) {
        if (vert.weight[k] > bestW) {
            bestW = vert.weight[k];
            bestBone = static_cast<int16_t>(vert.blend_indices[k]);
        }
    }
    return bestBone;
}

[[nodiscard]] bool vertexWeightsDiffer(const we_mdl_vertex_t &a, const we_mdl_vertex_t &b) noexcept
{
    for (int k = 0; k < 4; ++k) {
        if (a.blend_indices[k] != b.blend_indices[k] ||
            std::abs(a.weight[k] - b.weight[k]) > 1e-6f) {
            return true;
        }
    }
    return false;
}

void remapVertexArrayWeights(std::vector<we_mdl_vertex_t> &vertices,
                             std::vector<int16_t> &dominantBones,
                             const std::vector<int32_t> &oldToNew)
{
    const bool anySurviving =
        std::any_of(oldToNew.begin(), oldToNew.end(), [](int32_t mapped) { return mapped >= 0; });

    dominantBones.resize(vertices.size(), -1);
    for (size_t v = 0u; v < vertices.size(); ++v) {
        we_mdl_vertex_t &vert = vertices[v];
        float sum = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const uint32_t oldB = vert.blend_indices[k];
            const float w = vert.weight[k];
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
        dominantBones[v] = computeDominantBone(vert);
    }
}

[[nodiscard]] std::array<uint64_t, 2> remapLockMask(const std::array<uint64_t, 2> &mask,
                                                    const std::vector<int32_t> &oldToNew) noexcept
{
    std::array<uint64_t, 2> next{0ull, 0ull};
    const size_t n = std::min<size_t>(oldToNew.size(), WE_MDL_MAX_BONES);
    for (size_t oldB = 0u; oldB < n; ++oldB) {
        const size_t oldW = oldB >> 6u;
        const uint64_t oldBit = 1ull << (oldB & 63u);
        if ((mask[oldW] & oldBit) == 0ull) {
            continue;
        }
        const int32_t newB = oldToNew[oldB];
        if (newB >= 0 && newB < static_cast<int32_t>(WE_MDL_MAX_BONES)) {
            const size_t newW = static_cast<size_t>(newB) >> 6u;
            const uint64_t newBit = 1ull << (static_cast<size_t>(newB) & 63u);
            next[newW] |= newBit;
        }
    }
    return next;
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

} // namespace

struct WeightPaintPanel::Impl {
    uint32_t vertexCount{0u};
    uint32_t boneCount{0u};
    std::vector<uint64_t> lockedVertBits;
    std::vector<int16_t> dominantBones;
    std::vector<we_mdl_vertex_t> vertices;
    std::unordered_map<int32_t, ObjectWeightState> objectStates;
    bool updatingUi{false};

    QMetaObject::Connection weightsSolvedConn;
    bool pendingHeatSolve{false};
    bool telemetryUpdatedForSolve{false};
    int32_t pendingHeatObjectId{-1};
    QElapsedTimer heatSolveTimer;
    std::vector<we_mdl_vertex_t> preSolveVertices;

    QPushButton *lockBoneBtn{nullptr};
    QPushButton *unlockBonesBtn{nullptr};
    QPushButton *lockVertToggleBtn{nullptr};
    QPushButton *unlockAllVertsBtn{nullptr};
    QLabel *vertLockStatusLabel{nullptr};
    QDoubleSpinBox *conductivitySpin{nullptr};
    QSpinBox *maxInfSpin{nullptr};
    QCheckBox *idtCheck{nullptr};
    QCheckBox *respectLockedVertsCheck{nullptr};
    QDoubleSpinBox *brushFalloffSpin{nullptr};
    HeatmapLegendWidget *legend{nullptr};

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
        for (uint32_t v = 0u; v < vertexCount; ++v) {
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

WeightPaintPanel::WeightPaintPanel(QWidget *parent)
    : QWidget(parent), m_impl(std::make_unique<Impl>())
{
    setupUi();
}

WeightPaintPanel::~WeightPaintPanel()
{
    if (m_impl && m_impl->weightsSolvedConn) {
        disconnect(m_impl->weightsSolvedConn);
    }
}

void WeightPaintPanel::setIpcClient(IpcClient *client)
{
    if (m_ipcClient == client) {
        return;
    }
    if (m_impl && m_impl->weightsSolvedConn) {
        disconnect(m_impl->weightsSolvedConn);
        m_impl->weightsSolvedConn = QMetaObject::Connection{};
    }
    m_ipcClient = client;
    if (m_ipcClient && m_impl) {
        m_impl->weightsSolvedConn =
            connect(m_ipcClient, &IpcClient::puppetWeightsSolved, this,
                    [this](const waywal_ipc_puppet_weights_resp_t &resp,
                           const std::vector<waywal_ipc_vertex_weight_t> &entries) {
                        onPuppetWeightsSolved(resp, entries);
                    });
    }
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
        if ((m_impl && m_impl->updatingUi) || row < 0) {
            return;
        }
        setActiveBone(row);
    });
    connect(m_boneWeightList, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        if (!item || (m_impl && m_impl->updatingUi)) {
            return;
        }
        const uint32_t boneIdx = item->data(Qt::UserRole).toUInt();
        setBoneLocked(boneIdx, item->checkState() == Qt::Checked);
    });
    boneLayout->addWidget(m_boneWeightList);

    auto *boneLockRow = new QHBoxLayout();
    boneLockRow->setSpacing(6);

    m_impl->lockBoneBtn = new QPushButton(tr("Lock Bone"), boneGroup);
    m_impl->lockBoneBtn->setObjectName(QStringLiteral("lockBoneBtn"));
    m_impl->lockBoneBtn->setCheckable(true);
    m_impl->lockBoneBtn->setEnabled(false);
    m_impl->lockBoneBtn->setProperty("activeToggle", true);
    m_impl->lockBoneBtn->setAccessibleName(tr("Toggle Active Bone Weight Lock"));
    m_impl->lockBoneBtn->setToolTip(
        tr("Freeze weights on the active bone during brush & normalize ops"));
    connect(m_impl->lockBoneBtn, &QPushButton::toggled, this, [this](bool checked) {
        if (m_activeBoneIndex >= 0) {
            setBoneLocked(static_cast<uint32_t>(m_activeBoneIndex), checked);
        }
    });

    m_impl->unlockBonesBtn = new QPushButton(tr("Unlock All Bones"), boneGroup);
    m_impl->unlockBonesBtn->setObjectName(QStringLiteral("unlockBonesBtn"));
    m_impl->unlockBonesBtn->setEnabled(false);
    m_impl->unlockBonesBtn->setAccessibleName(tr("Unlock All Bones"));
    connect(m_impl->unlockBonesBtn, &QPushButton::clicked, this, [this]() {
        m_lockedBoneMask = {0ull, 0ull};
        if (m_impl) {
            const QScopedValueRollback<bool> guard(m_impl->updatingUi, true);
            if (m_boneWeightList) {
                for (int r = 0; r < m_boneWeightList->count(); ++r) {
                    if (QListWidgetItem *item = m_boneWeightList->item(r)) {
                        item->setCheckState(Qt::Unchecked);
                    }
                }
            }
        }
        syncBoneLockControls();
        saveActiveObjectState();
        emit brushParamsChanged();
        emit statusMessage(tr("Unlocked all bone weights"));
    });

    boneLockRow->addWidget(m_impl->lockBoneBtn);
    boneLockRow->addWidget(m_impl->unlockBonesBtn);
    boneLayout->addLayout(boneLockRow);

    auto *vertLockRow = new QHBoxLayout();
    vertLockRow->setSpacing(6);

    m_impl->lockVertToggleBtn = new QPushButton(tr("Lock Bone Vertices"), boneGroup);
    m_impl->lockVertToggleBtn->setObjectName(QStringLiteral("lockVertToggleBtn"));
    m_impl->lockVertToggleBtn->setCheckable(true);
    m_impl->lockVertToggleBtn->setEnabled(false);
    m_impl->lockVertToggleBtn->setProperty("activeToggle", true);
    m_impl->lockVertToggleBtn->setAccessibleName(tr("Toggle Vertex Lock for Active Bone"));
    m_impl->lockVertToggleBtn->setToolTip(
        tr("Lock or unlock vertices predominantly bound to the active bone"));

    m_impl->unlockAllVertsBtn = new QPushButton(tr("Unlock All Vertices"), boneGroup);
    m_impl->unlockAllVertsBtn->setObjectName(QStringLiteral("unlockAllVertsBtn"));
    m_impl->unlockAllVertsBtn->setEnabled(false);
    m_impl->unlockAllVertsBtn->setAccessibleName(tr("Unlock All Vertices"));

    connect(m_impl->lockVertToggleBtn, &QPushButton::toggled, this, [this](bool checked) {
        if (!m_impl || m_activeBoneIndex < 0) {
            return;
        }
        m_impl->setVerticesForBoneLocked(m_activeBoneIndex, checked);
        updateVertLockUi();
        saveActiveObjectState();
        emit vertexLocksChanged(m_objectId, m_impl->lockedVertBits);
        emit statusMessage(checked
                               ? tr("Locked vertices bound to bone #%1").arg(m_activeBoneIndex)
                               : tr("Unlocked vertices bound to bone #%1").arg(m_activeBoneIndex));
    });

    connect(m_impl->unlockAllVertsBtn, &QPushButton::clicked, this, [this]() {
        if (!m_impl) {
            return;
        }
        m_impl->unlockAllVertices();
        updateVertLockUi();
        saveActiveObjectState();
        emit vertexLocksChanged(m_objectId, m_impl->lockedVertBits);
        emit statusMessage(tr("Unlocked all mesh vertices"));
    });

    vertLockRow->addWidget(m_impl->lockVertToggleBtn);
    vertLockRow->addWidget(m_impl->unlockAllVertsBtn);
    boneLayout->addLayout(vertLockRow);

    m_impl->vertLockStatusLabel =
        new QLabel(tr("Vertex Locks: 0 locked | Active Bone: None"), boneGroup);
    m_impl->vertLockStatusLabel->setWordWrap(true);
    m_impl->vertLockStatusLabel->setAccessibleName(tr("Vertex Lock Telemetry"));
    m_impl->vertLockStatusLabel->setStyleSheet(
        QStringLiteral("QLabel { font-family: 'JetBrains Mono', monospace; font-size: 11px; "
                       "color: #94a3b8; background-color: #0b0c10; border: 1px solid #2a2f3d; "
                       "border-radius: 4px; padding: 4px 6px; }"));
    boneLayout->addWidget(m_impl->vertLockStatusLabel);
    root->addWidget(boneGroup);

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

    m_impl->conductivitySpin =
        makeWeightSpin(0.01, 10.0, 0.05, 3, QString(),
                       tr("Heat Conductivity and Tikhonov Regularization Scale"), heatGroup);
    m_impl->conductivitySpin->setObjectName(QStringLiteral("conductivitySpin"));
    m_impl->conductivitySpin->setValue(1.0);
    m_impl->conductivitySpin->setToolTip(
        tr("Scales heat diffusion conductivity (paired with 1e-3 Tikhonov island "
           "regularization)"));

    m_impl->maxInfSpin = new QSpinBox(heatGroup);
    m_impl->maxInfSpin->setObjectName(QStringLiteral("maxInfSpin"));
    m_impl->maxInfSpin->setRange(1, 4);
    m_impl->maxInfSpin->setValue(4);
    m_impl->maxInfSpin->setKeyboardTracking(false);
    m_impl->maxInfSpin->setAccessibleName(tr("Maximum Bone Influences Per Vertex"));

    heatGrid->addWidget(new QLabel(tr("Falloff Power:"), heatGroup), 0, 0);
    heatGrid->addWidget(m_heatFalloffSpin, 0, 1);
    heatGrid->addWidget(new QLabel(tr("Smoothing:"), heatGroup), 0, 2);
    heatGrid->addWidget(m_heatSmoothSpin, 0, 3);

    heatGrid->addWidget(new QLabel(tr("Conductivity:"), heatGroup), 1, 0);
    heatGrid->addWidget(m_impl->conductivitySpin, 1, 1);
    heatGrid->addWidget(new QLabel(tr("Max Influences:"), heatGroup), 1, 2);
    heatGrid->addWidget(m_impl->maxInfSpin, 1, 3);

    m_visibilityRaycastCheck =
        new QCheckBox(tr("Visibility Raycast (Prevent Cross-Gap Bleed)"), heatGroup);
    m_visibilityRaycastCheck->setChecked(true);
    m_visibilityRaycastCheck->setAccessibleName(tr("Use Visibility Raycast"));
    heatGrid->addWidget(m_visibilityRaycastCheck, 2, 0, 1, 4);

    m_impl->idtCheck = new QCheckBox(tr("Intrinsic Delaunay (iDT M-Matrix Guarantee)"), heatGroup);
    m_impl->idtCheck->setObjectName(QStringLiteral("idtCheck"));
    m_impl->idtCheck->setChecked(true);
    m_impl->idtCheck->setAccessibleName(tr("Use Intrinsic Delaunay Triangulation"));
    heatGrid->addWidget(m_impl->idtCheck, 3, 0, 1, 4);

    m_impl->respectLockedVertsCheck =
        new QCheckBox(tr("Respect Locked Vertices During Heat Solve"), heatGroup);
    m_impl->respectLockedVertsCheck->setObjectName(QStringLiteral("respectLockedVertsCheck"));
    m_impl->respectLockedVertsCheck->setChecked(true);
    m_impl->respectLockedVertsCheck->setAccessibleName(tr("Respect Locked Vertices"));
    heatGrid->addWidget(m_impl->respectLockedVertsCheck, 4, 0, 1, 4);

    m_solveHeatBtn = new QPushButton(tr("Solve Harmonic Heat Weights"), heatGroup);
    m_solveHeatBtn->setAccessibleName(tr("Solve Harmonic Heat Weights"));
    m_solveHeatBtn->setToolTip(
        tr("Compute automatic visibility-aware cotangent heat skinning weights"));
    connect(m_solveHeatBtn, &QPushButton::clicked, this,
            &WeightPaintPanel::onSolveHeatWeightsClicked);
    heatGrid->addWidget(m_solveHeatBtn, 5, 0, 1, 4);

    root->addWidget(heatGroup);

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

    m_impl->brushFalloffSpin =
        makeWeightSpin(0.25, 4.0, 0.1, 2, QString(), tr("Brush Edge Falloff"), brushGroup);
    m_impl->brushFalloffSpin->setObjectName(QStringLiteral("brushFalloffSpin"));
    m_impl->brushFalloffSpin->setValue(1.50);

    m_targetWeightSpin =
        makeWeightSpin(0.0, 1.0, 0.05, 2, QString(), tr("Replace Target Weight"), brushGroup);
    m_targetWeightSpin->setValue(1.00);
    m_targetWeightSpin->setEnabled(false);

    brushGrid->addWidget(new QLabel(tr("Falloff:"), brushGroup), 2, 0);
    brushGrid->addWidget(m_impl->brushFalloffSpin, 2, 1);
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
    for (QDoubleSpinBox *s :
         {m_radiusSpin, m_strengthSpin, m_impl->brushFalloffSpin, m_targetWeightSpin}) {
        connect(s, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                &WeightPaintPanel::brushParamsChanged);
    }

    root->addWidget(brushGroup);

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

    m_impl->legend = new HeatmapLegendWidget(heatmapGroup);
    m_impl->legend->setObjectName(QStringLiteral("heatmapLegend"));
    m_impl->legend->setEnabled(false);

    connect(m_paletteCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        if (m_impl && m_impl->legend) {
            m_impl->legend->setPalette(activePalette());
        }
        sendHeatmapModeIpc();
        const float opacity = m_heatmapOpacitySlider
                                  ? static_cast<float>(m_heatmapOpacitySlider->value()) / 100.0f
                                  : 0.75f;
        const uint32_t boneIdx =
            (m_activeBoneIndex >= 0) ? static_cast<uint32_t>(m_activeBoneIndex) : 0xFFFFFFFFu;
        emit heatmapModeChanged(m_objectId,
                                m_heatmapEnableCheck && m_heatmapEnableCheck->isChecked(), boneIdx,
                                opacity, activePalette());
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
    heatmapGrid->addWidget(m_impl->legend, 3, 0, 1, 2);

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

    setTabOrder(m_boneWeightList, m_impl->lockBoneBtn);
    setTabOrder(m_impl->lockBoneBtn, m_impl->unlockBonesBtn);
    setTabOrder(m_impl->unlockBonesBtn, m_impl->lockVertToggleBtn);
    setTabOrder(m_impl->lockVertToggleBtn, m_impl->unlockAllVertsBtn);
    setTabOrder(m_impl->unlockAllVertsBtn, m_heatFalloffSpin);
    setTabOrder(m_heatFalloffSpin, m_heatSmoothSpin);
    setTabOrder(m_heatSmoothSpin, m_impl->conductivitySpin);
    setTabOrder(m_impl->conductivitySpin, m_impl->maxInfSpin);
    setTabOrder(m_impl->maxInfSpin, m_visibilityRaycastCheck);
    setTabOrder(m_visibilityRaycastCheck, m_impl->idtCheck);
    setTabOrder(m_impl->idtCheck, m_impl->respectLockedVertsCheck);
    setTabOrder(m_impl->respectLockedVertsCheck, m_solveHeatBtn);
    setTabOrder(m_solveHeatBtn, m_brushOpCombo);
    setTabOrder(m_brushOpCombo, m_radiusSpin);
    setTabOrder(m_radiusSpin, m_strengthSpin);
    setTabOrder(m_strengthSpin, m_impl->brushFalloffSpin);
    setTabOrder(m_impl->brushFalloffSpin, m_targetWeightSpin);
    setTabOrder(m_targetWeightSpin, m_normalizeBtn);
    setTabOrder(m_normalizeBtn, m_heatmapEnableCheck);
    setTabOrder(m_heatmapEnableCheck, m_paletteCombo);
    setTabOrder(m_paletteCombo, m_heatmapOpacitySlider);
}

void WeightPaintPanel::saveActiveObjectState()
{
    if (!m_impl || m_objectId < 0) {
        return;
    }
    ObjectWeightState &st = m_impl->objectStates[m_objectId];
    st.lockedBoneMask = m_lockedBoneMask;
    st.activeBoneIndex = m_activeBoneIndex;
    st.boneCount = m_impl->boneCount;
    st.vertexCount = m_impl->vertexCount;
    st.lockedVertBits = m_impl->lockedVertBits;
    st.dominantBones = m_impl->dominantBones;
    st.vertices = m_impl->vertices;
}

void WeightPaintPanel::syncBoneLockControls()
{
    if (!m_impl) {
        return;
    }
    const bool hasActiveBone =
        (m_activeBoneIndex >= 0 && m_activeBoneIndex < static_cast<int32_t>(m_impl->boneCount));
    if (m_impl->lockBoneBtn) {
        const QSignalBlocker b(m_impl->lockBoneBtn);
        m_impl->lockBoneBtn->setEnabled(hasActiveBone);
        m_impl->lockBoneBtn->setChecked(hasActiveBone &&
                                        isBoneLocked(static_cast<uint32_t>(m_activeBoneIndex)));
    }
    if (m_impl->unlockBonesBtn) {
        m_impl->unlockBonesBtn->setEnabled((m_lockedBoneMask[0] | m_lockedBoneMask[1]) != 0ull);
    }
}

void WeightPaintPanel::updateVertLockUi()
{
    if (!m_impl) {
        return;
    }
    const uint32_t lockedCount = m_impl->lockedVertexCount();
    if (m_impl->unlockAllVertsBtn) {
        m_impl->unlockAllVertsBtn->setEnabled(lockedCount > 0u);
    }

    uint32_t boneTotal = 0u;
    uint32_t boneLocked = 0u;
    if (m_activeBoneIndex >= 0 && !m_impl->lockedVertBits.empty()) {
        for (uint32_t v = 0u; v < m_impl->vertexCount; ++v) {
            if (m_impl->dominantBones[v] == static_cast<int16_t>(m_activeBoneIndex)) {
                ++boneTotal;
                const size_t word = v >> 6u;
                const uint64_t bit = 1ull << (v & 63u);
                if (word < m_impl->lockedVertBits.size() &&
                    (m_impl->lockedVertBits[word] & bit) != 0ull) {
                    ++boneLocked;
                }
            }
        }
    }

    if (m_impl->lockVertToggleBtn) {
        const QSignalBlocker b(m_impl->lockVertToggleBtn);
        m_impl->lockVertToggleBtn->setEnabled(m_activeBoneIndex >= 0 && boneTotal > 0u);
        m_impl->lockVertToggleBtn->setChecked(boneTotal > 0u && boneLocked == boneTotal);
    }

    if (m_impl->vertLockStatusLabel) {
        const QString boneStr =
            (m_activeBoneIndex >= 0) ? QStringLiteral("#%1").arg(m_activeBoneIndex) : tr("None");
        m_impl->vertLockStatusLabel->setText(
            tr("Vertex Locks: %1 locked | Active Bone: %2").arg(lockedCount).arg(boneStr));
    }
}

void WeightPaintPanel::setPuppetContext(int32_t objectId, const we_puppet_t *puppet)
{
    if (!m_impl) {
        return;
    }

    const bool switchingObject = (objectId != m_objectId);
    if (switchingObject && m_objectId >= 0) {
        saveActiveObjectState();
    }

    m_objectId = objectId;

    if (switchingObject) {
        const auto it =
            (objectId >= 0) ? m_impl->objectStates.find(objectId) : m_impl->objectStates.end();
        if (it != m_impl->objectStates.end()) {
            m_lockedBoneMask = it->second.lockedBoneMask;
            m_activeBoneIndex = it->second.activeBoneIndex;
            m_impl->boneCount = it->second.boneCount;
            m_impl->vertexCount = it->second.vertexCount;
            m_impl->lockedVertBits = it->second.lockedVertBits;
            m_impl->dominantBones = it->second.dominantBones;
            m_impl->vertices = it->second.vertices;
        } else {
            m_lockedBoneMask = {0ull, 0ull};
            m_activeBoneIndex = -1;
            m_impl->boneCount = 0u;
            m_impl->vertexCount = 0u;
            m_impl->lockedVertBits.clear();
            m_impl->dominantBones.clear();
            m_impl->vertices.clear();
        }
    }

    const uint32_t newBoneCount =
        (puppet && puppet->bones) ? std::min<uint32_t>(puppet->bone_count, WE_MDL_MAX_BONES) : 0u;
    const uint32_t newVertCount =
        (puppet && puppet->vertices && puppet->vertex_count > 0u)
            ? std::min<uint32_t>(puppet->vertex_count, WE_MDL_MAX_VERTICES)
            : 0u;

    for (uint32_t i = newBoneCount; i < WE_MDL_MAX_BONES; ++i) {
        m_lockedBoneMask[i >> 6u] &= ~(1ull << (i & 63u));
    }
    m_impl->boneCount = newBoneCount;

    if (newVertCount == 0u) {
        m_impl->vertexCount = 0u;
        m_impl->lockedVertBits.clear();
        m_impl->dominantBones.clear();
        m_impl->vertices.clear();
    } else {
        const size_t words = (static_cast<size_t>(newVertCount) + 63u) / 64u;
        if (newVertCount != m_impl->vertexCount || m_impl->lockedVertBits.size() != words) {
            m_impl->lockedVertBits.assign(words, 0ull);
        }
        m_impl->vertexCount = newVertCount;
        m_impl->vertices.assign(puppet->vertices, puppet->vertices + newVertCount);
        m_impl->dominantBones.resize(newVertCount, -1);
        for (uint32_t v = 0u; v < newVertCount; ++v) {
            m_impl->dominantBones[v] = computeDominantBone(m_impl->vertices[v]);
        }
    }

    if (newBoneCount == 0u) {
        m_activeBoneIndex = -1;
    } else if (m_activeBoneIndex < 0 || m_activeBoneIndex >= static_cast<int32_t>(newBoneCount)) {
        m_activeBoneIndex = 0;
    }

    if (m_boneWeightList) {
        const QScopedValueRollback<bool> guard(m_impl->updatingUi, true);
        const QSignalBlocker blocker(m_boneWeightList);
        m_boneWeightList->clear();

        if (newBoneCount > 0u && puppet && puppet->bones) {
            for (uint32_t i = 0u; i < newBoneCount; ++i) {
                QString name = QString::fromUtf8(puppet->bones[i].name).trimmed();
                if (name.isEmpty()) {
                    name = QStringLiteral("Bone_%1").arg(i);
                }
                auto *item = new QListWidgetItem(QStringLiteral("[%1] %2").arg(i).arg(name));
                item->setData(Qt::UserRole, static_cast<uint32_t>(i));
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(isBoneLocked(i) ? Qt::Checked : Qt::Unchecked);
                m_boneWeightList->addItem(item);
            }
            m_boneWeightList->setCurrentRow(m_activeBoneIndex);
        }
    }

    syncBoneLockControls();
    updateVertLockUi();
    saveActiveObjectState();

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
    if (!m_impl || boneIndex < -1 || boneIndex >= static_cast<int32_t>(WE_MDL_MAX_BONES)) {
        return;
    }

    if (boneIndex >= 0 && m_boneWeightList && boneIndex >= m_boneWeightList->count() &&
        m_impl->boneCount > 0u) {
        return;
    }

    m_activeBoneIndex = boneIndex;

    if (m_boneWeightList) {
        const QScopedValueRollback<bool> guard(m_impl->updatingUi, true);
        const QSignalBlocker blocker(m_boneWeightList);
        if (m_activeBoneIndex < 0) {
            m_boneWeightList->setCurrentRow(-1);
            m_boneWeightList->clearSelection();
        } else if (m_activeBoneIndex < m_boneWeightList->count()) {
            m_boneWeightList->setCurrentRow(m_activeBoneIndex);
        }
    }

    syncBoneLockControls();
    updateVertLockUi();
    saveActiveObjectState();

    if (m_heatmapEnableCheck && m_heatmapEnableCheck->isChecked()) {
        sendHeatmapModeIpc();
        const float opacity = m_heatmapOpacitySlider
                                  ? static_cast<float>(m_heatmapOpacitySlider->value()) / 100.0f
                                  : 0.75f;
        const uint32_t activeIdx =
            (m_activeBoneIndex >= 0) ? static_cast<uint32_t>(m_activeBoneIndex) : 0xFFFFFFFFu;
        emit heatmapModeChanged(m_objectId, true, activeIdx, opacity, activePalette());
    }
}

void WeightPaintPanel::setBoneLocked(uint32_t boneIndex, bool locked)
{
    if (!m_impl || boneIndex >= WE_MDL_MAX_BONES) {
        return;
    }
    const size_t word = boneIndex >> 6u;
    const uint64_t bit = 1ull << (boneIndex & 63u);
    if (locked) {
        m_lockedBoneMask[word] |= bit;
    } else {
        m_lockedBoneMask[word] &= ~bit;
    }

    if (m_boneWeightList && static_cast<int>(boneIndex) < m_boneWeightList->count()) {
        const QScopedValueRollback<bool> guard(m_impl->updatingUi, true);
        if (QListWidgetItem *item = m_boneWeightList->item(static_cast<int>(boneIndex))) {
            item->setCheckState(locked ? Qt::Checked : Qt::Unchecked);
        }
    }

    syncBoneLockControls();
    saveActiveObjectState();
    emit brushParamsChanged();
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

float WeightPaintPanel::brushFalloff() const noexcept
{
    return (m_impl && m_impl->brushFalloffSpin)
               ? static_cast<float>(m_impl->brushFalloffSpin->value())
               : 1.5f;
}

float WeightPaintPanel::targetWeightValue() const noexcept
{
    return m_targetWeightSpin ? static_cast<float>(m_targetWeightSpin->value()) : 1.0f;
}

const std::vector<uint64_t> &WeightPaintPanel::lockedVertexBits() const noexcept
{
    return m_impl->lockedVertBits;
}

bool WeightPaintPanel::respectLockedVertices() const noexcept
{
    return !m_impl || !m_impl->respectLockedVertsCheck ||
           m_impl->respectLockedVertsCheck->isChecked();
}

void WeightPaintPanel::onSolveHeatWeightsClicked()
{
    if (!m_impl) {
        return;
    }

    const bool respectLocked = respectLockedVertices();

    we_heat_skin_cfg_t cfg{};
    cfg.conductivity_scale =
        m_impl->conductivitySpin ? static_cast<float>(m_impl->conductivitySpin->value()) : 1.0f;
    cfg.falloff_power = m_heatFalloffSpin
                            ? std::clamp(static_cast<float>(m_heatFalloffSpin->value()), 0.5f, 8.0f)
                            : 2.0f;
    cfg.smooth_factor = m_heatSmoothSpin
                            ? std::clamp(static_cast<float>(m_heatSmoothSpin->value()), 0.0f, 1.0f)
                            : 0.25f;
    cfg.max_influences = m_impl->maxInfSpin
                             ? static_cast<uint32_t>(std::clamp(m_impl->maxInfSpin->value(), 1, 4))
                             : 4u;
    cfg.prune_epsilon = 1e-4f;
    cfg.use_visibility_raycast =
        m_visibilityRaycastCheck ? m_visibilityRaycastCheck->isChecked() : true;
    cfg.use_intrinsic_delaunay = m_impl->idtCheck ? m_impl->idtCheck->isChecked() : true;
    cfg.locked_vert_bs = (respectLocked && !m_impl->lockedVertBits.empty())
                             ? m_impl->lockedVertBits.data()
                             : nullptr;

    if (m_solveTelemetryLabel) {
        m_solveTelemetryLabel->setText(tr("Heat Solve: falloff=%1, smooth=%2, iDT=%3, raycast=%4")
                                           .arg(static_cast<double>(cfg.falloff_power), 0, 'f', 2)
                                           .arg(static_cast<double>(cfg.smooth_factor), 0, 'f', 2)
                                           .arg(cfg.use_intrinsic_delaunay ? tr("on") : tr("off"))
                                           .arg(cfg.use_visibility_raycast ? tr("on") : tr("off")));
    }

    if (m_objectId >= 0) {
        m_impl->pendingHeatSolve = true;
        m_impl->telemetryUpdatedForSolve = false;
        m_impl->pendingHeatObjectId = m_objectId;
        m_impl->preSolveVertices = m_impl->vertices;
        m_impl->heatSolveTimer.restart();

        if (m_ipcClient) {
            waywal_ipc_puppet_solve_weights_t req{};
            req.object_id = static_cast<uint32_t>(m_objectId);
            req.falloff_power = cfg.falloff_power;
            req.smoothness = cfg.smooth_factor;
            req.max_influences = cfg.max_influences;
            req.flags = 1u;
            const bool sendLocks = respectLocked && !m_impl->lockedVertBits.empty();
            if (sendLocks) {
                req.flags |= (1u << 1);
            }
            if (cfg.use_visibility_raycast) {
                req.flags |= (1u << 2);
            }
            if (cfg.use_intrinsic_delaunay) {
                req.flags |= (1u << 3);
            }
            req.locked_vert_word_count =
                sendLocks
                    ? static_cast<uint32_t>(std::min<size_t>(m_impl->lockedVertBits.size(), 1024u))
                    : 0u;
            // Omit lock words when bit 1 is clear so IpcClient accepts the request cleanly.
            std::vector<uint64_t> sentBits;
            if (sendLocks && req.locked_vert_word_count > 0u) {
                sentBits.assign(m_impl->lockedVertBits.begin(),
                                m_impl->lockedVertBits.begin() + req.locked_vert_word_count);
            }
            m_ipcClient->sendPuppetSolveWeights(req, sentBits);
        }

        emit heatSolveRequested(m_objectId, cfg);
        emit statusMessage(
            tr("Requested harmonic heat weight solve for puppet #%1").arg(m_objectId));
    } else {
        emit statusMessage(tr("No active puppet loaded for heat weight solve"));
    }
}

void WeightPaintPanel::onPuppetWeightsSolved(const waywal_ipc_puppet_weights_resp_t &resp,
                                             const std::vector<waywal_ipc_vertex_weight_t> &entries)
{
    if (!m_impl || !m_impl->pendingHeatSolve ||
        static_cast<int32_t>(resp.object_id) != m_impl->pendingHeatObjectId) {
        return;
    }

    const float measuredMs =
        m_impl->heatSolveTimer.isValid()
            ? static_cast<float>(m_impl->heatSolveTimer.nsecsElapsed()) / 1.0e6f
            : 0.0f;
    m_impl->pendingHeatSolve = false;

    if (!m_impl->telemetryUpdatedForSolve) {
        setSolveTelemetry(resp.entry_count, m_impl->boneCount, measuredMs, resp.status != 0);
    }

    if (resp.status != 0 || entries.empty()) {
        m_impl->preSolveVertices.clear();
        return;
    }

    const int32_t targetObjectId = static_cast<int32_t>(resp.object_id);
    std::vector<we_mdl_vertex_t> preVerts;
    if (!m_impl->preSolveVertices.empty()) {
        preVerts = std::move(m_impl->preSolveVertices);
    } else if (targetObjectId == m_objectId) {
        preVerts = m_impl->vertices;
    } else if (const auto it = m_impl->objectStates.find(targetObjectId);
               it != m_impl->objectStates.end()) {
        preVerts = it->second.vertices;
    }
    m_impl->preSolveVertices.clear();

    if (preVerts.empty()) {
        return;
    }

    std::vector<we_mdl_vertex_t> postVerts = preVerts;
    for (const waywal_ipc_vertex_weight_t &e : entries) {
        if (e.vertex_index >= postVerts.size()) {
            continue;
        }
        we_mdl_vertex_t &v = postVerts[e.vertex_index];
        for (int k = 0; k < 4; ++k) {
            v.blend_indices[k] = e.blend_indices[k];
            v.weight[k] = e.weight[k];
        }
    }

    const uint32_t totalVerts = static_cast<uint32_t>(postVerts.size());
    const uint32_t blockCount = (totalVerts + kWeightBlockSize - 1u) / kWeightBlockSize;
    std::vector<VertexWeightBlockSnapshot> preBlocks;
    std::vector<VertexWeightBlockSnapshot> postBlocks;

    for (uint32_t b = 0u; b < blockCount; ++b) {
        const uint32_t start = b * kWeightBlockSize;
        const uint32_t end = std::min(start + kWeightBlockSize, totalVerts);
        bool dirty = false;
        for (uint32_t vi = start; vi < end; ++vi) {
            if (vertexWeightsDiffer(preVerts[vi], postVerts[vi])) {
                dirty = true;
                break;
            }
        }
        if (!dirty) {
            continue;
        }
        VertexWeightBlockSnapshot preBlk;
        preBlk.blockIndex = b;
        preBlk.slice.assign(preVerts.begin() + start, preVerts.begin() + end);
        VertexWeightBlockSnapshot postBlk;
        postBlk.blockIndex = b;
        postBlk.slice.assign(postVerts.begin() + start, postVerts.begin() + end);
        preBlocks.push_back(std::move(preBlk));
        postBlocks.push_back(std::move(postBlk));
    }

    if (preBlocks.empty()) {
        return;
    }

    if (targetObjectId == m_objectId) {
        m_impl->vertices = std::move(postVerts);
        m_impl->dominantBones.resize(m_impl->vertices.size(), -1);
        for (size_t i = 0u; i < m_impl->vertices.size(); ++i) {
            m_impl->dominantBones[i] = computeDominantBone(m_impl->vertices[i]);
        }
        updateVertLockUi();
        saveActiveObjectState();
    } else if (auto it = m_impl->objectStates.find(targetObjectId);
               it != m_impl->objectStates.end()) {
        it->second.vertices = std::move(postVerts);
        it->second.dominantBones.resize(it->second.vertices.size(), -1);
        for (size_t i = 0u; i < it->second.vertices.size(); ++i) {
            it->second.dominantBones[i] = computeDominantBone(it->second.vertices[i]);
        }
    }

    if (!m_commandBus) {
        return;
    }

    QPointer<WeightPaintPanel> self(this);
    auto firstApply = std::make_shared<bool>(true);
    auto applyBlocks = [self, firstApply](int32_t objId,
                                          const std::vector<VertexWeightBlockSnapshot> &blocks) {
        if (*firstApply) {
            *firstApply = false;
            return;
        }
        if (!self || !self->m_impl) {
            return;
        }
        std::vector<waywal_ipc_vertex_weight_t> ipcEntries;
        for (const VertexWeightBlockSnapshot &blk : blocks) {
            const uint32_t base = blk.blockIndex * kWeightBlockSize;
            for (size_t i = 0u; i < blk.slice.size(); ++i) {
                const uint32_t vIdx = base + static_cast<uint32_t>(i);
                waywal_ipc_vertex_weight_t ew{};
                ew.vertex_index = vIdx;
                for (int k = 0; k < 4; ++k) {
                    ew.blend_indices[k] = blk.slice[i].blend_indices[k];
                    ew.weight[k] = blk.slice[i].weight[k];
                }
                ipcEntries.push_back(ew);
                if (objId == self->m_objectId && vIdx < self->m_impl->vertices.size()) {
                    self->m_impl->vertices[vIdx] = blk.slice[i];
                    self->m_impl->dominantBones[vIdx] = computeDominantBone(blk.slice[i]);
                } else if (auto it = self->m_impl->objectStates.find(objId);
                           it != self->m_impl->objectStates.end() &&
                           vIdx < it->second.vertices.size()) {
                    it->second.vertices[vIdx] = blk.slice[i];
                    it->second.dominantBones[vIdx] = computeDominantBone(blk.slice[i]);
                }
            }
        }
        if (objId == self->m_objectId) {
            self->updateVertLockUi();
            self->saveActiveObjectState();
        }
        if (self->m_ipcClient && !ipcEntries.empty()) {
            self->m_ipcClient->sendPuppetPaintWeights(static_cast<uint32_t>(objId), 0u, ipcEntries);
        }
    };

    m_commandBus->execute(std::make_unique<WeightPaintStrokeCommand>(
        targetObjectId, tr("Harmonic Heat Solve"), std::move(preBlocks), std::move(postBlocks),
        std::move(applyBlocks)));
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

    float effectiveMs = elapsedMs;
    if (m_impl) {
        if (effectiveMs <= 0.0f && m_impl->heatSolveTimer.isValid()) {
            effectiveMs = static_cast<float>(m_impl->heatSolveTimer.nsecsElapsed()) / 1.0e6f;
        }
        m_impl->telemetryUpdatedForSolve = true;
    }

    if (vertexCount == 0u && usedIc0Fallback) {
        m_solveTelemetryLabel->setText(tr("Heat weight solve failed (0 vertices)"));
        return;
    }

    const QString solverTag = usedIc0Fallback ? tr("IC(0) / fallback") : tr("Cholesky");
    m_solveTelemetryLabel->setText(tr("Solved %1 verts / %2 bones in %3 ms (%4)")
                                       .arg(vertexCount)
                                       .arg(boneCount)
                                       .arg(static_cast<double>(effectiveMs), 0, 'f', 2)
                                       .arg(solverTag));
}

void WeightPaintPanel::remapBoneIndices(int32_t objectId, const std::vector<int32_t> &oldToNew)
{
    if (!m_impl || objectId < 0 || oldToNew.empty()) {
        return;
    }

    if (objectId == m_objectId) {
        m_lockedBoneMask = remapLockMask(m_lockedBoneMask, oldToNew);
        if (m_activeBoneIndex >= 0 && m_activeBoneIndex < static_cast<int32_t>(oldToNew.size())) {
            m_activeBoneIndex = oldToNew[static_cast<size_t>(m_activeBoneIndex)];
        }
        remapVertexArrayWeights(m_impl->vertices, m_impl->dominantBones, oldToNew);
        syncBoneLockControls();
        updateVertLockUi();
        saveActiveObjectState();
        emit brushParamsChanged();
        return;
    }

    const auto it = m_impl->objectStates.find(objectId);
    if (it != m_impl->objectStates.end()) {
        ObjectWeightState &st = it->second;
        st.lockedBoneMask = remapLockMask(st.lockedBoneMask, oldToNew);
        if (st.activeBoneIndex >= 0 && st.activeBoneIndex < static_cast<int32_t>(oldToNew.size())) {
            st.activeBoneIndex = oldToNew[static_cast<size_t>(st.activeBoneIndex)];
        }
        remapVertexArrayWeights(st.vertices, st.dominantBones, oldToNew);
    }
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
    if (m_impl && m_impl->legend) {
        m_impl->legend->setEnabled(enabled);
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

    m_ipcClient->sendPuppetHeatmapMode(req);
}

} // namespace waywal::studio
