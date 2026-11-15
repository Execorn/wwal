// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/scene/scene_layer_tree_widget.hpp"
#include "waywal/we_scene.h"

#include <QAbstractItemView>
#include <QColor>
#include <QDataStream>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIODevice>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMatrix4x4>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <queue>
#include <unordered_set>
#include <utility>

namespace waywal::studio {

namespace {

constexpr int kRoleObjectId = Qt::UserRole;
constexpr int kRoleKind = Qt::UserRole + 1;
constexpr int kRoleBlendMode = Qt::UserRole + 2;
constexpr int kRoleVisible = Qt::UserRole + 3;
constexpr int kRoleLocked = Qt::UserRole + 4;
constexpr int kRoleZOrder = Qt::UserRole + 5;
constexpr int kRoleHasScript = Qt::UserRole + 6;
constexpr int kRoleHasShader = Qt::UserRole + 7;
constexpr int kRoleHasAudio = Qt::UserRole + 8;

[[nodiscard]] QString kindTag(we_scene_node_kind_t kind)
{
    switch (kind) {
    case WE_NODE_IMAGE:
        return QStringLiteral("IMG");
    case WE_NODE_PARTICLE:
        return QStringLiteral("PTL");
    case WE_NODE_SOUND:
        return QStringLiteral("SND");
    case WE_NODE_TEXT:
        return QStringLiteral("TXT");
    case WE_NODE_LIGHT:
        return QStringLiteral("LGT");
    case WE_NODE_CAMERA3D:
        return QStringLiteral("CAM");
    }
    return QStringLiteral("OBJ");
}

[[nodiscard]] QString kindDisplayName(we_scene_node_kind_t kind)
{
    switch (kind) {
    case WE_NODE_IMAGE:
        return QStringLiteral("Image");
    case WE_NODE_PARTICLE:
        return QStringLiteral("Particle");
    case WE_NODE_SOUND:
        return QStringLiteral("Sound");
    case WE_NODE_TEXT:
        return QStringLiteral("Text");
    case WE_NODE_LIGHT:
        return QStringLiteral("Light");
    case WE_NODE_CAMERA3D:
        return QStringLiteral("3D Camera");
    }
    return QStringLiteral("Layer");
}

[[nodiscard]] QColor kindAccentColor(we_scene_node_kind_t kind)
{
    switch (kind) {
    case WE_NODE_IMAGE:
        return QColor(QStringLiteral("#38bdf8"));
    case WE_NODE_PARTICLE:
        return QColor(QStringLiteral("#22c55e"));
    case WE_NODE_SOUND:
        return QColor(QStringLiteral("#f59e0b"));
    case WE_NODE_TEXT:
        return QColor(QStringLiteral("#e2e8f0"));
    case WE_NODE_LIGHT:
        return QColor(QStringLiteral("#fbbf24"));
    case WE_NODE_CAMERA3D:
        return QColor(QStringLiteral("#a78bfa"));
    }
    return QColor(QStringLiteral("#94a3b8"));
}

[[nodiscard]] QString blendModeLabel(uint32_t blendMode)
{
    switch (static_cast<we_blend_mode_t>(blendMode)) {
    case WE_BLEND_NORMAL:
        return QStringLiteral("Normal");
    case WE_BLEND_ADDITIVE:
        return QStringLiteral("Add");
    case WE_BLEND_MULTIPLY:
        return QStringLiteral("Mul");
    case WE_BLEND_SCREEN:
        return QStringLiteral("Screen");
    case WE_BLEND_DISABLED:
        return QStringLiteral("Opaque");
    }
    return QStringLiteral("Normal");
}

[[nodiscard]] we_scene_node_kind_t parseNodeKind(const QJsonValue &val)
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

[[nodiscard]] uint32_t parseBlendMode(const QJsonValue &val)
{
    if (val.isDouble()) {
        const int raw = val.toInt(0);
        return (raw >= 0 && raw <= static_cast<int>(WE_BLEND_DISABLED)) ? static_cast<uint32_t>(raw)
                                                                        : 0u;
    }
    const QString s = val.toString().trimmed().toLower();
    if (s == QStringLiteral("additive") || s == QStringLiteral("add")) {
        return WE_BLEND_ADDITIVE;
    }
    if (s == QStringLiteral("multiply") || s == QStringLiteral("mul")) {
        return WE_BLEND_MULTIPLY;
    }
    if (s == QStringLiteral("screen")) {
        return WE_BLEND_SCREEN;
    }
    if (s == QStringLiteral("disabled") || s == QStringLiteral("opaque")) {
        return WE_BLEND_DISABLED;
    }
    return WE_BLEND_NORMAL;
}

template <size_t N>
[[nodiscard]] std::array<float, N> parseFloatArray(const QJsonValue &val,
                                                   std::array<float, N> fallback)
{
    if (val.isArray()) {
        const QJsonArray arr = val.toArray();
        for (size_t i = 0; i < N && std::cmp_less(i, arr.size()); ++i) {
            fallback[i] =
                static_cast<float>(arr.at(static_cast<qsizetype>(i)).toDouble(fallback[i]));
        }
        return fallback;
    }
    if (val.isString()) {
        const QStringList parts = val.toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (size_t i = 0; i < N && std::cmp_less(i, parts.size()); ++i) {
            bool ok = false;
            const float parsed = parts.at(static_cast<qsizetype>(i)).toFloat(&ok);
            if (ok) {
                fallback[i] = parsed;
            }
        }
    }
    return fallback;
}

void copyTruncatedString(char *dst, size_t cap, const QString &src)
{
    if (!dst || cap == 0) {
        return;
    }
    const QByteArray utf8 = src.toUtf8();
    const size_t n = std::min(cap - 1, static_cast<size_t>(utf8.size()));
    if (n > 0) {
        std::memcpy(dst, utf8.constData(), n);
    }
    dst[n] = '\0';
}

[[nodiscard]] waywal_ipc_req_scene_mutate_t
makeBaseMutateReq(const QString &output, we_scene_mutate_op_t op, const SceneLayerNode &node)
{
    waywal_ipc_req_scene_mutate_t req{};
    copyTruncatedString(req.target_output, sizeof(req.target_output), output);
    req.mutate_op = static_cast<uint32_t>(op);
    req.node_kind = static_cast<uint32_t>(node.kind);
    req.object_id = node.id;
    req.parent_id = node.parentId;
    req.target_z_index = static_cast<int32_t>(node.zOrderIndex);
    req.blend_mode = node.blendMode;
    req.flags = (node.visible ? 1u : 0u) | (node.locked ? 2u : 0u);
    std::copy(node.origin.begin(), node.origin.end(), req.origin);
    std::copy(node.scale.begin(), node.scale.end(), req.scale);
    std::copy(node.angles.begin(), node.angles.end(), req.angles);
    std::copy(node.size.begin(), node.size.end(), req.size);
    std::copy(node.parallaxDepth.begin(), node.parallaxDepth.end(), req.parallax_depth);
    std::copy(node.color.begin(), node.color.end(), req.color);
    copyTruncatedString(req.name, sizeof(req.name), node.name);
    return req;
}

// Column-major T * Rz(angles[2]) * Ry(angles[1]) * Rx(angles[0]) * S matching daemon we_scene.c.
[[nodiscard]] QMatrix4x4 buildLocalTrsMatrix(const SceneLayerNode &node) noexcept
{
    const float sx = std::isfinite(node.scale[0]) ? node.scale[0] : 1.0f;
    const float sy = std::isfinite(node.scale[1]) ? node.scale[1] : 1.0f;
    const float sz = std::isfinite(node.scale[2]) ? node.scale[2] : 1.0f;

    const float phi = std::isfinite(node.angles[0]) ? node.angles[0] : 0.0f;
    const float theta = std::isfinite(node.angles[1]) ? node.angles[1] : 0.0f;
    const float psi = std::isfinite(node.angles[2]) ? node.angles[2] : 0.0f;

    const float cp = std::cos(phi);
    const float sp = std::sin(phi);
    const float ct = std::cos(theta);
    const float st = std::sin(theta);
    const float cr = std::cos(psi);
    const float sr = std::sin(psi);

    const float ox = std::isfinite(node.origin[0]) ? node.origin[0] : 0.0f;
    const float oy = std::isfinite(node.origin[1]) ? node.origin[1] : 0.0f;
    const float oz = std::isfinite(node.origin[2]) ? node.origin[2] : 0.0f;

    return QMatrix4x4(
        sx * (cr * ct), sy * (cr * st * sp - sr * cp), sz * (cr * st * cp + sr * sp), ox,
        sx * (sr * ct), sy * (sr * st * sp + cr * cp), sz * (sr * st * cp - cr * sp), oy,
        -sx * st, sy * (ct * sp), sz * (ct * cp), oz, 0.0f, 0.0f, 0.0f, 1.0f);
}

[[nodiscard]] QMatrix4x4 worldTrsMatrixOf(const SceneLayerTreeModel &model, int32_t nodeId) noexcept
{
    std::vector<QMatrix4x4> chain;
    std::unordered_set<int32_t> visited;
    int32_t cur = nodeId;
    uint32_t guard = 0u;
    while (cur != -1 && guard++ <= SceneLayerTreeModel::kMaxParentDepth &&
           visited.insert(cur).second) {
        const SceneLayerNode *n = model.findNodeById(cur);
        if (!n) {
            break;
        }
        chain.push_back(buildLocalTrsMatrix(*n));
        cur = n->parentId;
    }
    QMatrix4x4 world;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        world = world * (*it);
    }
    return world;
}

void decomposeTrsMatrix(const QMatrix4x4 &m, std::array<float, 3> &outOrigin,
                        std::array<float, 3> &outScale, std::array<float, 3> &outAngles) noexcept
{
    outOrigin = {m(0, 3), m(1, 3), m(2, 3)};

    const QVector3D col0(m(0, 0), m(1, 0), m(2, 0));
    const QVector3D col1(m(0, 1), m(1, 1), m(2, 1));
    const QVector3D col2(m(0, 2), m(1, 2), m(2, 2));

    float sx = col0.length();
    float sy = col1.length();
    float sz = col2.length();

    const float det3 = QVector3D::dotProduct(col0, QVector3D::crossProduct(col1, col2));
    if (det3 < 0.0f) {
        sz = -sz;
    }

    outScale = {
        (sx > 1e-6f) ? sx : 1.0f,
        (sy > 1e-6f) ? sy : 1.0f,
        (std::abs(sz) > 1e-6f) ? sz : 1.0f,
    };

    const QVector3D r0 = (sx > 1e-6f) ? (col0 / sx) : QVector3D(1.0f, 0.0f, 0.0f);
    const QVector3D r1 = (sy > 1e-6f) ? (col1 / sy) : QVector3D(0.0f, 1.0f, 0.0f);
    const QVector3D r2 = (std::abs(sz) > 1e-6f) ? (col2 / sz) : QVector3D(0.0f, 0.0f, 1.0f);

    const float theta = std::asin(std::clamp(-r0.z(), -1.0f, 1.0f));
    float phi = 0.0f;
    float psi = 0.0f;
    if (std::abs(std::cos(theta)) > 1e-5f) {
        phi = std::atan2(r1.z(), r2.z());
        psi = std::atan2(r0.y(), r0.x());
    } else {
        phi = std::atan2(-r2.y(), r1.y());
        psi = 0.0f;
    }
    outAngles = {phi, theta, psi};
}

class MoveSceneLayerCommand final : public ICommand {
  public:
    using ApplyMoveFn = std::function<void(const SceneLayerMovePlan &plan, bool isUndo)>;

    MoveSceneLayerCommand(QString layerName, SceneLayerMovePlan plan,
                          std::array<float, 3> oldOrigin, std::array<float, 3> newOrigin,
                          std::array<float, 3> oldScale, std::array<float, 3> newScale,
                          std::array<float, 3> oldAngles, std::array<float, 3> newAngles,
                          ApplyMoveFn applyFn)
        : m_layerName(std::move(layerName)), m_plan(std::move(plan)), m_oldOrigin(oldOrigin),
          m_newOrigin(newOrigin), m_oldScale(oldScale), m_newScale(newScale), m_oldAngles(oldAngles),
          m_newAngles(newAngles), m_applyFn(std::move(applyFn))
    {
    }

    void execute() override
    {
        if (m_applyFn && isValid()) {
            m_applyFn(m_plan, false);
        }
    }

    void undo() override
    {
        if (m_applyFn && isValid()) {
            m_applyFn(m_plan, true);
        }
    }

    [[nodiscard]] QString describe() const override
    {
        const QString label = m_layerName.isEmpty() ? QStringLiteral("Layer #%1").arg(m_plan.objectId)
                                                    : m_layerName;
        if (m_plan.oldParentId != m_plan.newParentId && !m_plan.newZOrders.empty()) {
            return QStringLiteral("Move '%1'").arg(label);
        }
        if (m_plan.oldParentId != m_plan.newParentId) {
            return QStringLiteral("Reparent '%1'").arg(label);
        }
        return QStringLiteral("Reorder '%1' Z-order").arg(label);
    }

    [[nodiscard]] size_t memoryFootprint() const override
    {
        return sizeof(*this) + static_cast<size_t>(std::max<qsizetype>(0, m_layerName.capacity())) *
                                   sizeof(QChar) +
               (m_plan.oldZOrders.capacity() + m_plan.newZOrders.capacity()) *
                   sizeof(std::pair<int32_t, uint32_t>);
    }

    [[nodiscard]] bool isValid() const override
    {
        auto allFin = [](const std::array<float, 3> &a) {
            return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]);
        };
        return m_plan.objectId >= 0 && allFin(m_oldOrigin) && allFin(m_newOrigin) &&
               allFin(m_oldScale) && allFin(m_newScale) && allFin(m_oldAngles) &&
               allFin(m_newAngles);
    }

    [[nodiscard]] const std::array<float, 3> &originFor(bool isUndo) const noexcept
    {
        return isUndo ? m_oldOrigin : m_newOrigin;
    }
    [[nodiscard]] const std::array<float, 3> &scaleFor(bool isUndo) const noexcept
    {
        return isUndo ? m_oldScale : m_newScale;
    }
    [[nodiscard]] const std::array<float, 3> &anglesFor(bool isUndo) const noexcept
    {
        return isUndo ? m_oldAngles : m_newAngles;
    }

  private:
    QString m_layerName;
    SceneLayerMovePlan m_plan;
    std::array<float, 3> m_oldOrigin{};
    std::array<float, 3> m_newOrigin{};
    std::array<float, 3> m_oldScale{};
    std::array<float, 3> m_newScale{};
    std::array<float, 3> m_oldAngles{};
    std::array<float, 3> m_newAngles{};
    ApplyMoveFn m_applyFn;
};

[[nodiscard]] waywal_ipc_req_scene_mutate_t makeSnapshotMutateReq(const QString &output,
                                                                  we_scene_mutate_op_t op,
                                                                  const SceneLayerSnapshot &snap)
{
    waywal_ipc_req_scene_mutate_t req{};
    copyTruncatedString(req.target_output, sizeof(req.target_output), output);
    req.mutate_op = static_cast<uint32_t>(op);
    req.node_kind = static_cast<uint32_t>(snap.kind);
    req.object_id = snap.objectId;
    req.parent_id = snap.parentId;
    req.target_z_index = static_cast<int32_t>(snap.zOrderIndex);
    req.blend_mode = snap.blendMode;
    req.flags = (snap.visible ? 1u : 0u) | (snap.locked ? 2u : 0u) | (snap.fullscreen ? 4u : 0u) |
                (snap.passthrough ? 8u : 0u);
    std::copy(snap.origin.begin(), snap.origin.end(), req.origin);
    std::copy(snap.scale.begin(), snap.scale.end(), req.scale);
    std::copy(snap.angles.begin(), snap.angles.end(), req.angles);
    std::copy(snap.size.begin(), snap.size.end(), req.size);
    std::copy(snap.parallaxDepth.begin(), snap.parallaxDepth.end(), req.parallax_depth);
    std::copy(snap.color.begin(), snap.color.end(), req.color);
    copyTruncatedString(req.name, sizeof(req.name), snap.name);
    return req;
}

[[nodiscard]] SceneLayerSnapshot snapshotFromNode(const SceneLayerNode &node)
{
    SceneLayerSnapshot snap;
    snap.objectId = node.id;
    snap.parentId = node.parentId;
    snap.zOrderIndex = node.zOrderIndex;
    snap.kind = node.kind;
    snap.blendMode = node.blendMode;
    snap.visible = node.visible;
    snap.locked = node.locked;
    snap.origin = node.origin;
    snap.scale = node.scale;
    snap.angles = node.angles;
    snap.size = node.size;
    snap.parallaxDepth = node.parallaxDepth;
    snap.color = node.color;
    snap.name = node.name;
    return snap;
}

[[nodiscard]] SceneLayerNode nodeFromSnapshot(const SceneLayerSnapshot &snap)
{
    SceneLayerNode node;
    node.id = snap.objectId;
    node.parentId = snap.parentId;
    node.zOrderIndex = snap.zOrderIndex;
    node.kind = snap.kind;
    node.blendMode = snap.blendMode;
    node.visible = snap.visible;
    node.locked = snap.locked;
    node.origin = snap.origin;
    node.scale = snap.scale;
    node.angles = snap.angles;
    node.size = snap.size;
    node.parallaxDepth = snap.parallaxDepth;
    node.color = snap.color;
    node.name = snap.name;
    return node;
}

class LayerFilterProxyModel final : public QSortFilterProxyModel {
  public:
    explicit LayerFilterProxyModel(QObject *parent = nullptr) : QSortFilterProxyModel(parent)
    {
        setRecursiveFilteringEnabled(true);
        setFilterCaseSensitivity(Qt::CaseInsensitive);
    }

    void setQuery(const QString &query)
    {
        const QString trimmed = query.trimmed();
        if (m_query == trimmed) {
            return;
        }
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
        beginFilterChange();
        m_query = trimmed;
        endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
        m_query = trimmed;
        invalidateFilter();
#endif
    }

  protected:
    [[nodiscard]] bool filterAcceptsRow(int sourceRow,
                                        const QModelIndex &sourceParent) const override
    {
        if (m_query.isEmpty()) {
            return true;
        }
        const QModelIndex idx =
            sourceModel()->index(sourceRow, SceneLayerTreeModel::ColName, sourceParent);
        if (!idx.isValid()) {
            return false;
        }
        const QString name = idx.data(Qt::DisplayRole).toString();
        if (name.contains(m_query, Qt::CaseInsensitive)) {
            return true;
        }
        const auto kind = static_cast<we_scene_node_kind_t>(idx.data(kRoleKind).toUInt());
        if (kindDisplayName(kind).contains(m_query, Qt::CaseInsensitive) ||
            kindTag(kind).contains(m_query, Qt::CaseInsensitive)) {
            return true;
        }
        const uint32_t blend = idx.data(kRoleBlendMode).toUInt();
        return blendModeLabel(blend).contains(m_query, Qt::CaseInsensitive);
    }

  private:
    QString m_query;
};

class LayerItemDelegate final : public QStyledItemDelegate {
  public:
    explicit LayerItemDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem &option,
                                 const QModelIndex &index) const override
    {
        QSize sz = QStyledItemDelegate::sizeHint(option, index);
        sz.setHeight(std::max(sz.height(), 26));
        return sz;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        if (selected) {
            painter->fillRect(option.rect, QColor(0x1e, 0x3a, 0x5f, 180));
            painter->fillRect(QRect(option.rect.left(), option.rect.top(), 2, option.rect.height()),
                              QColor(QStringLiteral("#38bdf8")));
        } else if (hovered) {
            painter->fillRect(option.rect, QColor(QStringLiteral("#222631")));
        }

        const bool visible = index.data(kRoleVisible).toBool();
        const bool locked = index.data(kRoleLocked).toBool();
        const auto kind = static_cast<we_scene_node_kind_t>(index.data(kRoleKind).toUInt());
        const uint32_t blend = index.data(kRoleBlendMode).toUInt();

        QFont monoFont = option.font;
        monoFont.setFamily(QStringLiteral("JetBrains Mono"));
        monoFont.setPixelSize(11);

        if (index.column() == SceneLayerTreeModel::ColName) {
            const QRect r = option.rect.adjusted(4, 3, -4, -3);
            const QRect tagRect(r.left(), r.center().y() - 9, 30, 18);
            const QColor accent = kindAccentColor(kind);

            painter->setPen(QPen(accent.darker(130), 1.0));
            painter->setBrush(QColor(QStringLiteral("#181b22")));
            painter->drawRoundedRect(tagRect, 3.0, 3.0);

            painter->setFont(monoFont);
            painter->setPen(visible ? accent : QColor(QStringLiteral("#8b9bb4")));
            painter->drawText(tagRect, Qt::AlignCenter, kindTag(kind));

            int rightEdge = r.right();
            const auto drawChip = [&](const QString &chipText, const QColor &chipColor) {
                const int w = 26;
                const QRect chipRect(rightEdge - w, r.center().y() - 8, w, 16);
                painter->setPen(QPen(QColor(QStringLiteral("#2a2f3d")), 1.0));
                painter->setBrush(QColor(QStringLiteral("#181b22")));
                painter->drawRoundedRect(chipRect, 3.0, 3.0);
                painter->setFont(monoFont);
                painter->setPen(chipColor);
                painter->drawText(chipRect, Qt::AlignCenter, chipText);
                rightEdge -= (w + 3);
            };
            if (index.data(kRoleHasAudio).toBool()) {
                drawChip(QStringLiteral("AUD"), QColor(QStringLiteral("#f59e0b")));
            }
            if (index.data(kRoleHasScript).toBool()) {
                drawChip(QStringLiteral("JS"), QColor(QStringLiteral("#22c55e")));
            }
            if (index.data(kRoleHasShader).toBool()) {
                drawChip(QStringLiteral("FX"), QColor(QStringLiteral("#38bdf8")));
            }

            const QRect textRect(tagRect.right() + 6, r.top(),
                                 std::max(10, rightEdge - tagRect.right() - 8), r.height());
            painter->setFont(option.font);
            painter->setPen(visible ? QColor(QStringLiteral("#f1f5f9"))
                                    : QColor(QStringLiteral("#8b9bb4")));
            const QString elided = painter->fontMetrics().elidedText(
                index.data(Qt::DisplayRole).toString(), Qt::ElideRight, textRect.width());
            painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, elided);
        } else if (index.column() == SceneLayerTreeModel::ColBlendBadge) {
            const QRect pill = option.rect.adjusted(2, 4, -2, -4);
            painter->setPen(QPen(QColor(QStringLiteral("#2a2f3d")), 1.0));
            painter->setBrush(QColor(QStringLiteral("#181b22")));
            painter->drawRoundedRect(pill, 3.0, 3.0);
            painter->setFont(monoFont);
            painter->setPen(blend == WE_BLEND_NORMAL ? QColor(QStringLiteral("#94a3b8"))
                                                     : QColor(QStringLiteral("#38bdf8")));
            painter->drawText(pill, Qt::AlignCenter, blendModeLabel(blend));
        } else if (index.column() == SceneLayerTreeModel::ColLock) {
            painter->setFont(monoFont);
            painter->setPen(locked ? QColor(QStringLiteral("#f59e0b"))
                                   : QColor(QStringLiteral("#8b9bb4")));
            painter->drawText(option.rect, Qt::AlignCenter,
                              locked ? QStringLiteral("L") : QStringLiteral("·"));
        } else if (index.column() == SceneLayerTreeModel::ColVisible) {
            painter->setFont(monoFont);
            painter->setPen(visible ? QColor(QStringLiteral("#38bdf8"))
                                    : QColor(QStringLiteral("#8b9bb4")));
            painter->drawText(option.rect, Qt::AlignCenter,
                              visible ? QStringLiteral("V") : QStringLiteral("-"));
        }

        painter->restore();
    }
};

} // namespace

SceneLayerTreeModel::SceneLayerTreeModel(QObject *parent) : QAbstractItemModel(parent) {}

void SceneLayerTreeModel::setViewMode(SceneLayerViewMode mode)
{
    if (m_viewMode == mode) {
        return;
    }
    beginResetModel();
    m_viewMode = mode;
    rebuildIndexMaps();
    endResetModel();
}

void SceneLayerTreeModel::setNodes(std::vector<SceneLayerNode> nodes)
{
    beginResetModel();
    m_nodes = std::move(nodes);
    rebuildIndexMaps();
    endResetModel();
}

const SceneLayerNode *SceneLayerTreeModel::findNodeById(int32_t objectId) const noexcept
{
    const auto it = m_idToSlot.find(objectId);
    if (it == m_idToSlot.end() || it->second >= m_nodes.size()) {
        return nullptr;
    }
    return &m_nodes[it->second];
}

void SceneLayerTreeModel::rebuildIndexMaps()
{
    m_idToSlot.clear();
    m_childrenByParent.clear();
    m_flatZOrderIds.clear();

    m_idToSlot.reserve(m_nodes.size());
    m_flatZOrderIds.reserve(m_nodes.size());

    for (size_t i = 0; i < m_nodes.size(); ++i) {
        if (m_nodes[i].id >= 0 && !m_idToSlot.contains(m_nodes[i].id)) {
            m_idToSlot.emplace(m_nodes[i].id, i);
            m_flatZOrderIds.push_back(m_nodes[i].id);
        }
    }

    /* Break any cycle or over-deep chain in external JSON before building tree maps. */
    for (auto &node : m_nodes) {
        if (node.parentId < 0 || node.parentId == node.id || !m_idToSlot.contains(node.parentId)) {
            node.parentId = -1;
            continue;
        }
        std::unordered_set<int32_t> visited;
        visited.insert(node.id);
        int32_t cur = node.parentId;
        uint32_t depth = 1u;
        bool valid = true;
        while (cur != -1) {
            if (!visited.insert(cur).second || depth >= kMaxParentDepth) {
                valid = false;
                break;
            }
            const auto it = m_idToSlot.find(cur);
            if (it == m_idToSlot.end()) {
                valid = false;
                break;
            }
            cur = m_nodes[it->second].parentId;
            ++depth;
        }
        if (!valid) {
            node.parentId = -1;
        }
    }

    for (const auto &node : m_nodes) {
        if (node.id < 0) {
            continue;
        }
        m_childrenByParent[node.parentId].push_back(node.id);
    }

    const auto zOrderDesc = [this](int32_t a, int32_t b) {
        const SceneLayerNode *na = findNodeById(a);
        const SceneLayerNode *nb = findNodeById(b);
        if (!na || !nb) {
            return a < b;
        }
        if (na->zOrderIndex != nb->zOrderIndex) {
            return na->zOrderIndex > nb->zOrderIndex;
        }
        return na->id < nb->id;
    };

    std::sort(m_flatZOrderIds.begin(), m_flatZOrderIds.end(), zOrderDesc);
    for (auto &[parentId, children] : m_childrenByParent) {
        (void)parentId;
        std::sort(children.begin(), children.end(), zOrderDesc);
    }
}

const std::vector<int32_t> &SceneLayerTreeModel::childrenOf(int32_t parentId) const
{
    const auto it = m_childrenByParent.find(parentId);
    if (it == m_childrenByParent.end()) {
        return m_emptyChildren;
    }
    return it->second;
}

bool SceneLayerTreeModel::wouldCreateCycleOrExceedDepth(int32_t childId,
                                                        int32_t candidateParentId) const noexcept
{
    if (!m_idToSlot.contains(childId)) {
        return true;
    }

    /* Compute deepest subtree height rooted at childId so moving an entire branch enforces the cap.
     */
    uint32_t subtreeDepth = 1u;
    std::vector<std::pair<int32_t, uint32_t>> stack;
    stack.emplace_back(childId, 1u);
    std::unordered_set<int32_t> visited;
    while (!stack.empty()) {
        const auto [nodeId, depth] = stack.back();
        stack.pop_back();
        if (!visited.insert(nodeId).second) {
            return true;
        }
        subtreeDepth = std::max(subtreeDepth, depth);
        if (subtreeDepth > kMaxParentDepth) {
            return true;
        }
        const auto it = m_childrenByParent.find(nodeId);
        if (it != m_childrenByParent.end()) {
            for (int32_t descendant : it->second) {
                if (descendant == candidateParentId) {
                    return true;
                }
                stack.emplace_back(descendant, depth + 1u);
            }
        }
    }

    if (candidateParentId == -1) {
        return subtreeDepth > kMaxParentDepth;
    }
    if (candidateParentId == childId || !m_idToSlot.contains(candidateParentId)) {
        return true;
    }

    uint32_t parentChainDepth = 0u;
    int32_t cur = candidateParentId;
    std::unordered_set<int32_t> ancestors;
    while (cur != -1) {
        if (cur == childId || !ancestors.insert(cur).second) {
            return true;
        }
        ++parentChainDepth;
        if (parentChainDepth + subtreeDepth > kMaxParentDepth) {
            return true;
        }
        const auto it = m_idToSlot.find(cur);
        if (it == m_idToSlot.end()) {
            break;
        }
        cur = m_nodes[it->second].parentId;
    }

    return (parentChainDepth + subtreeDepth) > kMaxParentDepth;
}

QModelIndex SceneLayerTreeModel::indexForObjectId(int32_t objectId) const
{
    const auto it = m_idToSlot.find(objectId);
    if (it == m_idToSlot.end()) {
        return {};
    }
    const size_t slot = it->second;
    if (m_viewMode == SceneLayerViewMode::FlatZOrderStack) {
        const auto flatIt = std::find(m_flatZOrderIds.begin(), m_flatZOrderIds.end(), objectId);
        if (flatIt == m_flatZOrderIds.end()) {
            return {};
        }
        const int row = static_cast<int>(std::distance(m_flatZOrderIds.begin(), flatIt));
        return createIndex(row, ColName, static_cast<quintptr>(slot + 1));
    }

    const int32_t parentId = m_nodes[slot].parentId;
    const auto &siblings = childrenOf(parentId);
    const auto sibIt = std::find(siblings.begin(), siblings.end(), objectId);
    if (sibIt == siblings.end()) {
        return {};
    }
    const int row = static_cast<int>(std::distance(siblings.begin(), sibIt));
    return createIndex(row, ColName, static_cast<quintptr>(slot + 1));
}

QModelIndex SceneLayerTreeModel::index(int row, int column, const QModelIndex &parent) const
{
    if (row < 0 || column < 0 || column >= ColCount) {
        return {};
    }
    if (m_viewMode == SceneLayerViewMode::FlatZOrderStack) {
        if (parent.isValid() || std::cmp_greater_equal(row, m_flatZOrderIds.size())) {
            return {};
        }
        const int32_t id = m_flatZOrderIds[static_cast<size_t>(row)];
        const auto it = m_idToSlot.find(id);
        if (it == m_idToSlot.end()) {
            return {};
        }
        return createIndex(row, column, static_cast<quintptr>(it->second + 1));
    }

    int32_t parentId = -1;
    if (parent.isValid()) {
        const size_t parentSlot = static_cast<size_t>(parent.internalId() - 1);
        if (parentSlot >= m_nodes.size()) {
            return {};
        }
        parentId = m_nodes[parentSlot].id;
    }

    const auto &children = childrenOf(parentId);
    if (std::cmp_greater_equal(row, children.size())) {
        return {};
    }
    const int32_t childId = children[static_cast<size_t>(row)];
    const auto it = m_idToSlot.find(childId);
    if (it == m_idToSlot.end()) {
        return {};
    }
    return createIndex(row, column, static_cast<quintptr>(it->second + 1));
}

QModelIndex SceneLayerTreeModel::parent(const QModelIndex &child) const
{
    if (!child.isValid() || m_viewMode == SceneLayerViewMode::FlatZOrderStack) {
        return {};
    }
    const size_t slot = static_cast<size_t>(child.internalId() - 1);
    if (slot >= m_nodes.size()) {
        return {};
    }
    const int32_t pid = m_nodes[slot].parentId;
    if (pid < 0) {
        return {};
    }
    const auto pIt = m_idToSlot.find(pid);
    if (pIt == m_idToSlot.end()) {
        return {};
    }
    const size_t parentSlot = pIt->second;
    const auto &grandChildren = childrenOf(m_nodes[parentSlot].parentId);
    const auto rowIt = std::find(grandChildren.begin(), grandChildren.end(), pid);
    if (rowIt == grandChildren.end()) {
        return {};
    }
    const int row = static_cast<int>(std::distance(grandChildren.begin(), rowIt));
    return createIndex(row, ColName, static_cast<quintptr>(parentSlot + 1));
}

int SceneLayerTreeModel::rowCount(const QModelIndex &parent) const
{
    if (parent.column() > 0) {
        return 0;
    }
    if (m_viewMode == SceneLayerViewMode::FlatZOrderStack) {
        return parent.isValid() ? 0 : static_cast<int>(m_flatZOrderIds.size());
    }
    int32_t parentId = -1;
    if (parent.isValid()) {
        const size_t slot = static_cast<size_t>(parent.internalId() - 1);
        if (slot >= m_nodes.size()) {
            return 0;
        }
        parentId = m_nodes[slot].id;
    }
    return static_cast<int>(childrenOf(parentId).size());
}

int SceneLayerTreeModel::columnCount(const QModelIndex &parent) const
{
    (void)parent;
    return ColCount;
}

QVariant SceneLayerTreeModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) {
        return {};
    }
    const size_t slot = static_cast<size_t>(index.internalId() - 1);
    if (slot >= m_nodes.size()) {
        return {};
    }
    const SceneLayerNode &node = m_nodes[slot];

    switch (role) {
    case kRoleObjectId:
        return node.id;
    case kRoleKind:
        return static_cast<uint>(node.kind);
    case kRoleBlendMode:
        return static_cast<uint>(node.blendMode);
    case kRoleVisible:
        return node.visible;
    case kRoleLocked:
        return node.locked;
    case kRoleZOrder:
        return static_cast<uint>(node.zOrderIndex);
    case kRoleHasScript:
        return node.hasScript;
    case kRoleHasShader:
        return node.hasShader;
    case kRoleHasAudio:
        return node.hasAudioBinding;
    case Qt::DisplayRole:
        switch (index.column()) {
        case ColName:
            return node.name.isEmpty() ? QStringLiteral("Layer #%1").arg(node.id) : node.name;
        case ColBlendBadge:
            return blendModeLabel(node.blendMode);
        case ColLock:
            return node.locked ? QStringLiteral("L") : QString();
        case ColVisible:
            return node.visible ? QStringLiteral("V") : QStringLiteral("-");
        default:
            return {};
        }
    case Qt::ToolTipRole:
        if (index.column() == ColLock) {
            return node.locked ? QStringLiteral("Unlock layer (L)")
                               : QStringLiteral("Lock layer (L)");
        }
        if (index.column() == ColVisible) {
            return node.visible ? QStringLiteral("Hide layer (H)")
                                : QStringLiteral("Show layer (H)");
        }
        return QStringLiteral("%1 (%2) | Z=%3 | Blend=%4%5%6")
            .arg(node.name.isEmpty() ? QStringLiteral("Layer #%1").arg(node.id) : node.name)
            .arg(kindDisplayName(node.kind))
            .arg(node.zOrderIndex)
            .arg(blendModeLabel(node.blendMode))
            .arg(node.locked ? QStringLiteral(" | Locked") : QString())
            .arg(node.visible ? QString() : QStringLiteral(" | Hidden"));
    case Qt::AccessibleTextRole:
        if (index.column() == ColLock) {
            return node.locked ? QStringLiteral("Locked") : QStringLiteral("Unlocked");
        }
        if (index.column() == ColVisible) {
            return node.visible ? QStringLiteral("Visible") : QStringLiteral("Hidden");
        }
        return QStringLiteral("%1 layer %2, Z order %3, %4, %5")
            .arg(kindDisplayName(node.kind))
            .arg(node.name.isEmpty() ? QStringLiteral("#%1").arg(node.id) : node.name)
            .arg(node.zOrderIndex)
            .arg(node.visible ? QStringLiteral("visible") : QStringLiteral("hidden"))
            .arg(node.locked ? QStringLiteral("locked") : QStringLiteral("unlocked"));
    default:
        return {};
    }
}

Qt::ItemFlags SceneLayerTreeModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return Qt::ItemIsDropEnabled;
    }
    const auto id = static_cast<int32_t>(index.internalId() - 1);
    const SceneLayerNode *n =
        (id >= 0 && static_cast<size_t>(id) < m_nodes.size()) ? &m_nodes[static_cast<size_t>(id)]
                                                              : nullptr;
    Qt::ItemFlags base = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (n && !n->locked) {
        base |= Qt::ItemIsDragEnabled;
    }
    if (m_viewMode == SceneLayerViewMode::HierarchyTree && (!n || !n->locked)) {
        base |= Qt::ItemIsDropEnabled;
    }
    return base;
}

QStringList SceneLayerTreeModel::mimeTypes() const { return {QString::fromLatin1(kMimeType)}; }

QMimeData *SceneLayerTreeModel::mimeData(const QModelIndexList &indexes) const
{
    auto *mime = new QMimeData();
    QByteArray encoded;
    QDataStream stream(&encoded, QIODevice::WriteOnly);
    std::unordered_set<int32_t> seen;
    for (const QModelIndex &idx : indexes) {
        if (!idx.isValid() || idx.column() != ColName) {
            continue;
        }
        const int32_t id = idx.data(kRoleObjectId).toInt();
        if (id >= 0 && seen.insert(id).second) {
            stream << static_cast<qint32>(id);
        }
    }
    mime->setData(QString::fromLatin1(kMimeType), encoded);
    return mime;
}

bool SceneLayerTreeModel::dropMimeData(const QMimeData *data, Qt::DropAction action, int row,
                                       int column, const QModelIndex &parent)
{
    (void)column;
    if (action == Qt::IgnoreAction || !data || !data->hasFormat(QString::fromLatin1(kMimeType))) {
        return false;
    }

    QByteArray encoded = data->data(QString::fromLatin1(kMimeType));
    QDataStream stream(&encoded, QIODevice::ReadOnly);
    std::vector<int32_t> draggedIds;
    while (!stream.atEnd()) {
        qint32 id = -1;
        stream >> id;
        if (id >= 0) {
            draggedIds.push_back(id);
        }
    }
    if (draggedIds.empty()) {
        return false;
    }

    const bool preserveWorld = QGuiApplication::queryKeyboardModifiers().testFlag(Qt::AltModifier);
    int32_t targetParentId = -1;
    if (parent.isValid()) {
        const size_t pSlot = static_cast<size_t>(parent.internalId() - 1);
        if (pSlot >= m_nodes.size()) {
            return false;
        }
        targetParentId = m_nodes[pSlot].id;
    }

    if (m_viewMode == SceneLayerViewMode::HierarchyTree) {
        if (targetParentId != -1) {
            const SceneLayerNode *targetParent = findNodeById(targetParentId);
            if (!targetParent || targetParent->locked) {
                return false;
            }
        }
        for (int32_t id : draggedIds) {
            const SceneLayerNode *dragged = findNodeById(id);
            if (!dragged || dragged->locked) {
                return false;
            }
            if (dragged->parentId != targetParentId &&
                wouldCreateCycleOrExceedDepth(id, targetParentId)) {
                return false;
            }
        }
    } else {
        for (int32_t id : draggedIds) {
            const SceneLayerNode *dragged = findNodeById(id);
            if (!dragged || dragged->locked) {
                return false;
            }
        }
    }

    const int32_t primaryId = draggedIds.front();
    const SceneLayerNode *primaryNode = findNodeById(primaryId);
    if (!primaryNode) {
        return false;
    }
    uint32_t targetZ = primaryNode->zOrderIndex;
    size_t targetAscPos = 0u;
    bool haveTargetAscPos = false;

    std::vector<SceneLayerNode> sortedAsc = m_nodes;
    std::sort(sortedAsc.begin(), sortedAsc.end(), [](const SceneLayerNode &a, const SceneLayerNode &b) {
        if (a.zOrderIndex != b.zOrderIndex) {
            return a.zOrderIndex < b.zOrderIndex;
        }
        return a.id < b.id;
    });

    if (m_viewMode == SceneLayerViewMode::FlatZOrderStack && !m_flatZOrderIds.empty()) {
        const int32_t count = static_cast<int32_t>(m_flatZOrderIds.size());
        const int32_t rawInsertSlot = std::clamp(row < 0 ? count : row, 0, count);
        int32_t oldSlot = -1;
        for (int32_t i = 0; i < count; ++i) {
            if (m_flatZOrderIds[static_cast<size_t>(i)] == primaryId) {
                oldSlot = i;
                break;
            }
        }
        const int32_t insertIdx = (oldSlot >= 0 && oldSlot < rawInsertSlot)
                                      ? (rawInsertSlot - 1)
                                      : std::min(rawInsertSlot, std::max(0, count - 1));
        targetZ = static_cast<uint32_t>(std::max(0, count - 1 - insertIdx));
        targetAscPos = static_cast<size_t>(std::clamp(count - 1 - insertIdx, 0, count - 1));
        haveTargetAscPos = true;
    } else if (m_viewMode == SceneLayerViewMode::HierarchyTree) {
        const auto &siblings = childrenOf(targetParentId);
        auto ascRankWithout = [&](const SceneLayerNode *ref) -> size_t {
            size_t rank = 0u;
            for (const auto &n : sortedAsc) {
                if (n.id == primaryNode->id || n.id == ref->id) {
                    continue;
                }
                if (n.zOrderIndex < ref->zOrderIndex ||
                    (n.zOrderIndex == ref->zOrderIndex && n.id < ref->id)) {
                    ++rank;
                }
            }
            return rank;
        };

        if (!siblings.empty()) {
            const int32_t sibCount = static_cast<int32_t>(siblings.size());
            // row == -1 drops onto the parent/root container and appends after existing children.
            const int32_t rawInsertSlot = (row < 0) ? sibCount : std::clamp(row, 0, sibCount);
            if (rawInsertSlot < sibCount) {
                const SceneLayerNode *refNode =
                    findNodeById(siblings[static_cast<size_t>(rawInsertSlot)]);
                if (refNode && refNode->id != primaryNode->id) {
                    targetAscPos = ascRankWithout(refNode) + 1u;
                    targetZ = static_cast<uint32_t>(targetAscPos);
                    haveTargetAscPos = true;
                }
            } else {
                const SceneLayerNode *lastNode = findNodeById(siblings.back());
                if (lastNode && lastNode->id != primaryNode->id) {
                    targetAscPos = ascRankWithout(lastNode);
                    targetZ = static_cast<uint32_t>(targetAscPos);
                    haveTargetAscPos = true;
                }
            }
        } else {
            // Appending into an empty parent container keeps Z immediately after or at root end.
            targetAscPos = sortedAsc.size() > 0u ? (sortedAsc.size() - 1u) : 0u;
            targetZ = static_cast<uint32_t>(targetAscPos);
            haveTargetAscPos = true;
        }
    }

    auto buildZPlanForId = [&](int32_t id, uint32_t fallbackTargetZ, bool hasPos, size_t ascPos,
                               std::vector<std::pair<int32_t, uint32_t>> &outOldZ,
                               std::vector<std::pair<int32_t, uint32_t>> &outNewZ,
                               uint32_t &outCompatTargetZ) {
        std::vector<SceneLayerNode> seq = sortedAsc;
        const auto curIt =
            std::find_if(seq.begin(), seq.end(), [id](const auto &n) { return n.id == id; });
        if (curIt == seq.end()) {
            return;
        }
        std::vector<uint32_t> sparsePool;
        sparsePool.reserve(seq.size());
        bool strictSparse = true;
        for (size_t i = 0; i < seq.size(); ++i) {
            sparsePool.push_back(seq[i].zOrderIndex);
            if (i > 0u && sparsePool[i] <= sparsePool[i - 1u]) {
                strictSparse = false;
            }
        }

        const SceneLayerNode moved = *curIt;
        seq.erase(curIt);
        const size_t insertSlot =
            hasPos ? std::min<size_t>(ascPos, seq.size()) : std::min<size_t>(fallbackTargetZ, seq.size());
        seq.insert(seq.begin() + static_cast<ptrdiff_t>(insertSlot), moved);

        for (size_t i = 0; i < seq.size(); ++i) {
            const uint32_t newZ = strictSparse ? sparsePool[i] : static_cast<uint32_t>(i);
            if (seq[i].zOrderIndex != newZ) {
                outOldZ.emplace_back(seq[i].id, seq[i].zOrderIndex);
                outNewZ.emplace_back(seq[i].id, newZ);
            }
            if (seq[i].id == id) {
                outCompatTargetZ = newZ;
            }
        }
    };

    for (int32_t id : draggedIds) {
        const SceneLayerNode *dragged = findNodeById(id);
        if (!dragged) {
            continue;
        }
        SceneLayerMovePlan plan;
        plan.objectId = id;
        plan.oldParentId = dragged->parentId;
        plan.newParentId =
            (m_viewMode == SceneLayerViewMode::HierarchyTree) ? targetParentId : dragged->parentId;
        plan.preserveWorldTransform = preserveWorld;

        uint32_t compatTargetZ = targetZ;
        buildZPlanForId(id, targetZ, haveTargetAscPos, targetAscPos, plan.oldZOrders,
                        plan.newZOrders, compatTargetZ);

        emit layerMoveRequested(plan);
        if (m_viewMode == SceneLayerViewMode::HierarchyTree &&
            dragged->parentId != targetParentId) {
            emit reparentRequested(id, targetParentId, preserveWorld);
        }
        emit reorderZRequested(id, compatTargetZ);
    }
    return true;
}

Qt::DropActions SceneLayerTreeModel::supportedDropActions() const { return Qt::MoveAction; }

SceneLayerTreeWidget::SceneLayerTreeWidget(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("SceneLayerTreeWidget"));
    setAccessibleName(QStringLiteral("Scene Layer Tree"));
    setAccessibleDescription(QStringLiteral(
        "Hierarchical and Z-order scene layer navigator with visibility and lock toggles"));
    setFocusPolicy(Qt::StrongFocus);
}

void SceneLayerTreeWidget::showEvent(QShowEvent *event)
{
    ensureUiInitialized();
    QWidget::showEvent(event);
}

void SceneLayerTreeWidget::ensureUiInitialized()
{
    if (m_uiInitialized) {
        return;
    }
    m_uiInitialized = true;

    if (!m_model) {
        m_model = new SceneLayerTreeModel(this);
    }

    auto *proxy = new LayerFilterProxyModel(this);
    proxy->setSourceModel(m_model);
    m_proxyModel = proxy;

    setStyleSheet(QStringLiteral(
        "#SceneLayerTreeWidget { background-color: #12141a; color: #f1f5f9; }"
        "QLineEdit { background-color: #181b22; color: #f1f5f9; border: 1px solid #2a2f3d; "
        "border-radius: 4px; padding: 4px 8px; font-size: 12px; }"
        "QLineEdit:focus { border-color: #38bdf8; }"
        "QPushButton { background-color: #181b22; color: #f1f5f9; border: 1px solid #2a2f3d; "
        "border-radius: 4px; padding: 4px 8px; font-size: 12px; }"
        "QPushButton:hover { background-color: #222631; }"
        "QTreeView { background-color: #0b0c10; color: #f1f5f9; border: 1px solid #2a2f3d; "
        "outline: none; }"
        "QTreeView::item { height: 26px; }"
        "QTreeView::branch { background: #0b0c10; }"));

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(4, 4, 4, 4);
    rootLayout->setSpacing(4);

    auto *toolbarLayout = new QHBoxLayout();
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    toolbarLayout->setSpacing(4);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(QStringLiteral("Filter layers..."));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setAccessibleName(QStringLiteral("Filter scene layers"));
    m_searchEdit->setAccessibleDescription(
        QStringLiteral("Filter layers by name, node type, or blend mode"));

    m_viewModeBtn = new QPushButton(QStringLiteral("Tree"), this);
    m_viewModeBtn->setCheckable(true);
    m_viewModeBtn->setToolTip(QStringLiteral("Toggle Hierarchy Tree / Flat Z-Order Stack"));
    m_viewModeBtn->setAccessibleName(QStringLiteral("Toggle layer view mode"));

    m_addLayerBtn = new QPushButton(QStringLiteral("+"), this);
    m_addLayerBtn->setFixedWidth(28);
    m_addLayerBtn->setToolTip(QStringLiteral("Add scene layer"));
    m_addLayerBtn->setAccessibleName(QStringLiteral("Add scene layer"));

    toolbarLayout->addWidget(m_searchEdit, 1);
    toolbarLayout->addWidget(m_viewModeBtn, 0);
    toolbarLayout->addWidget(m_addLayerBtn, 0);
    rootLayout->addLayout(toolbarLayout);

    m_treeView = new QTreeView(this);
    m_treeView->setModel(m_proxyModel);
    m_treeView->setItemDelegate(new LayerItemDelegate(m_treeView));
    m_treeView->setHeaderHidden(true);
    m_treeView->setUniformRowHeights(true);
    m_treeView->setIndentation(16);
    m_treeView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_treeView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_treeView->setDragEnabled(true);
    m_treeView->setAcceptDrops(true);
    m_treeView->setDropIndicatorShown(true);
    m_treeView->setDragDropMode(QAbstractItemView::InternalMove);
    m_treeView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_treeView->setAccessibleName(QStringLiteral("Scene layer list"));
    m_treeView->header()->setStretchLastSection(false);
    m_treeView->header()->setSectionResizeMode(SceneLayerTreeModel::ColName, QHeaderView::Stretch);
    m_treeView->header()->setSectionResizeMode(SceneLayerTreeModel::ColBlendBadge,
                                               QHeaderView::Fixed);
    m_treeView->header()->setSectionResizeMode(SceneLayerTreeModel::ColLock, QHeaderView::Fixed);
    m_treeView->header()->setSectionResizeMode(SceneLayerTreeModel::ColVisible, QHeaderView::Fixed);
    m_treeView->setColumnWidth(SceneLayerTreeModel::ColBlendBadge, 54);
    m_treeView->setColumnWidth(SceneLayerTreeModel::ColLock, 24);
    m_treeView->setColumnWidth(SceneLayerTreeModel::ColVisible, 24);
    rootLayout->addWidget(m_treeView, 1);

    connect(m_searchEdit, &QLineEdit::textChanged, this,
            [proxy](const QString &text) { proxy->setQuery(text); });

    connect(m_viewModeBtn, &QPushButton::toggled, this, [this](bool flatChecked) {
        const int32_t keepId = selectedObjectId();
        const auto mode =
            flatChecked ? SceneLayerViewMode::FlatZOrderStack : SceneLayerViewMode::HierarchyTree;
        m_viewModeBtn->setText(flatChecked ? QStringLiteral("Z-Stack") : QStringLiteral("Tree"));
        if (m_treeView && m_treeView->selectionModel()) {
            const QSignalBlocker blocker(m_treeView->selectionModel());
            m_model->setViewMode(mode);
        } else {
            m_model->setViewMode(mode);
        }
        if (!flatChecked) {
            m_treeView->expandAll();
        }
        if (keepId >= 0 && m_model->findNodeById(keepId)) {
            selectObject(keepId);
        }
    });

    connect(m_addLayerBtn, &QPushButton::clicked, this, [this]() {
        QMenu menu(this);
        const std::array<we_scene_node_kind_t, 6> kinds{
            WE_NODE_IMAGE, WE_NODE_PARTICLE, WE_NODE_SOUND,
            WE_NODE_TEXT,  WE_NODE_LIGHT,    WE_NODE_CAMERA3D,
        };
        for (we_scene_node_kind_t k : kinds) {
            QAction *act = menu.addAction(QStringLiteral("Add %1").arg(kindDisplayName(k)));
            connect(act, &QAction::triggered, this, [this, k]() {
                int32_t maxId = 0;
                uint32_t maxZ = 0;
                for (const auto &n : m_model->nodes()) {
                    maxId = std::max(maxId, n.id);
                    maxZ = std::max(maxZ, n.zOrderIndex + 1u);
                }
                SceneLayerSnapshot snap;
                snap.objectId = maxId + 1;
                snap.parentId = -1;
                snap.zOrderIndex = maxZ;
                snap.kind = k;
                snap.name = QStringLiteral("%1 %2").arg(kindDisplayName(k)).arg(snap.objectId);

                QPointer<SceneLayerTreeWidget> self(this);
                const auto mutateFn = [self](const SceneLayerSnapshot &s, bool isAdd) {
                    if (!self || !self->m_model) {
                        return;
                    }
                    auto nodes = self->m_model->nodes();
                    if (isAdd) {
                        nodes.push_back(nodeFromSnapshot(s));
                        self->applyNodesPreservingSelection(std::move(nodes), s.objectId);
                        self->dispatchMutateIpc(
                            makeSnapshotMutateReq(self->m_targetOutput, WE_MUTATE_ADD_OBJECT, s));
                    } else {
                        std::erase_if(nodes,
                                      [&s](const SceneLayerNode &n) { return n.id == s.objectId; });
                        self->applyNodesPreservingSelection(std::move(nodes));
                        self->dispatchMutateIpc(makeSnapshotMutateReq(self->m_targetOutput,
                                                                      WE_MUTATE_REMOVE_OBJECT, s));
                    }
                    emit self->layerMutated(s.objectId);
                };

                if (m_commandBus) {
                    m_commandBus->execute(std::make_unique<AddSceneLayerCommand>(snap, mutateFn));
                } else {
                    mutateFn(snap, true);
                }
                selectObject(snap.objectId);
            });
        }
        menu.exec(m_addLayerBtn->mapToGlobal(QPoint(0, m_addLayerBtn->height())));
    });

    connect(m_treeView->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &current, const QModelIndex &previous) {
                (void)previous;
                if (!current.isValid()) {
                    m_selectedObjectId = -1;
                    setProperty("wwal_selected_object_id", -1);
                    return;
                }
                const QModelIndex src = m_proxyModel->mapToSource(current);
                if (!src.isValid()) {
                    return;
                }
                const int32_t id = src.data(kRoleObjectId).toInt();
                const auto kind = static_cast<we_scene_node_kind_t>(src.data(kRoleKind).toUInt());
                m_selectedObjectId = id;
                setProperty("wwal_selected_object_id", id);
                emit objectSelected(id, kind);
            });

    connect(m_treeView, &QTreeView::clicked, this, [this](const QModelIndex &idx) {
        if (!idx.isValid()) {
            return;
        }
        const QModelIndex src = m_proxyModel->mapToSource(idx);
        const int32_t id = src.data(kRoleObjectId).toInt();
        const SceneLayerNode *cur = m_model->findNodeById(id);
        if (!cur) {
            return;
        }
        if (idx.column() == SceneLayerTreeModel::ColLock ||
            idx.column() == SceneLayerTreeModel::ColVisible) {
            SetLayerFlagsCommand::LayerFlagState next;
            next.visible =
                (idx.column() == SceneLayerTreeModel::ColVisible) ? !cur->visible : cur->visible;
            next.locked =
                (idx.column() == SceneLayerTreeModel::ColLock) ? !cur->locked : cur->locked;
            next.blendMode = cur->blendMode;
            next.alpha = cur->color[3];
            next.color = {cur->color[0], cur->color[1], cur->color[2]};
            executeFlagEdit(id, next);
            selectObject(id);
        }
    });

    connect(m_treeView, &QTreeView::customContextMenuRequested, this, [this](const QPoint &pos) {
        const QModelIndex idx = m_treeView->indexAt(pos);
        if (!idx.isValid()) {
            return;
        }
        const QModelIndex src = m_proxyModel->mapToSource(idx);
        const int32_t targetId = src.data(kRoleObjectId).toInt();
        const SceneLayerNode *cur = m_model->findNodeById(targetId);
        if (!cur) {
            return;
        }

        QMenu menu(this);
        QMenu *blendMenu = menu.addMenu(QStringLiteral("Blend Mode"));
        for (uint32_t bm = 0; bm <= WE_BLEND_DISABLED; ++bm) {
            QAction *act = blendMenu->addAction(blendModeLabel(bm));
            act->setCheckable(true);
            act->setChecked(cur->blendMode == bm);
            connect(act, &QAction::triggered, this, [this, targetId, bm]() {
                if (!m_model) {
                    return;
                }
                const SceneLayerNode *latest = m_model->findNodeById(targetId);
                if (!latest) {
                    return;
                }
                SetLayerFlagsCommand::LayerFlagState next;
                next.visible = latest->visible;
                next.locked = latest->locked;
                next.blendMode = bm;
                next.alpha = latest->color[3];
                next.color = {latest->color[0], latest->color[1], latest->color[2]};
                executeFlagEdit(targetId, next);
            });
        }

        if (cur->parentId != -1 && !cur->locked) {
            QAction *unparentAct = menu.addAction(QStringLiteral("Unparent to Root"));
            connect(unparentAct, &QAction::triggered, this, [this, targetId]() {
                if (m_model) {
                    SceneLayerMovePlan plan;
                    if (const SceneLayerNode *n = m_model->findNodeById(targetId)) {
                        plan.objectId = targetId;
                        plan.oldParentId = n->parentId;
                        plan.newParentId = -1;
                        plan.preserveWorldTransform = true;
                        emit m_model->layerMoveRequested(plan);
                    }
                }
            });
        }
        menu.exec(m_treeView->viewport()->mapToGlobal(pos));
    });

    connect(
        m_model, &SceneLayerTreeModel::layerMoveRequested, this,
        [this](const SceneLayerMovePlan &plan) {
            const SceneLayerNode *target = m_model->findNodeById(plan.objectId);
            if (!target) {
                return;
            }
            if (target->locked) {
                emit statusMessage(
                    QStringLiteral("Move rejected: layer '%1' is locked").arg(target->name));
                return;
            }
            const bool parentChanged = (plan.oldParentId != plan.newParentId);
            if (parentChanged &&
                m_model->wouldCreateCycleOrExceedDepth(plan.objectId, plan.newParentId)) {
                emit statusMessage(QStringLiteral("Reparent rejected: cycle or parent depth > 16"));
                return;
            }
            if (parentChanged && plan.newParentId != -1) {
                const SceneLayerNode *newParentNode = m_model->findNodeById(plan.newParentId);
                if (!newParentNode || newParentNode->locked) {
                    emit statusMessage(
                        QStringLiteral("Reparent rejected: target parent is locked"));
                    return;
                }
            }
            if (!parentChanged && plan.newZOrders.empty()) {
                return;
            }

            std::array<float, 3> newOrigin = target->origin;
            std::array<float, 3> newScale = target->scale;
            std::array<float, 3> newAngles = target->angles;
            if (parentChanged && plan.preserveWorldTransform) {
                const QMatrix4x4 childWorld = worldTrsMatrixOf(*m_model, target->id);
                const QMatrix4x4 parentWorld = (plan.newParentId != -1)
                                                   ? worldTrsMatrixOf(*m_model, plan.newParentId)
                                                   : QMatrix4x4();
                bool invertible = false;
                const QMatrix4x4 invParent = parentWorld.inverted(&invertible);
                const QMatrix4x4 nextLocal = invertible ? (invParent * childWorld) : childWorld;
                decomposeTrsMatrix(nextLocal, newOrigin, newScale, newAngles);
            }

            QPointer<SceneLayerTreeWidget> self(this);
            const auto applyMove = [self, oldOrigin = target->origin, newOrigin,
                                    oldScale = target->scale, newScale, oldAngles = target->angles,
                                    newAngles](const SceneLayerMovePlan &p, bool isUndo) {
                if (!self || !self->m_model) {
                    return;
                }
                auto nodes = self->m_model->nodes();
                const auto &zUpdates = isUndo ? p.oldZOrders : p.newZOrders;
                std::unordered_map<int32_t, uint32_t> zById;
                zById.reserve(zUpdates.size());
                for (const auto &[id, z] : zUpdates) {
                    zById[id] = z;
                }

                std::vector<waywal_ipc_req_scene_mutate_t> pendingIpc;
                const bool hasParentChange = (p.oldParentId != p.newParentId);
                const int32_t nextParent = isUndo ? p.oldParentId : p.newParentId;
                const auto &nextOrigin = isUndo ? oldOrigin : newOrigin;
                const auto &nextScale = isUndo ? oldScale : newScale;
                const auto &nextAngles = isUndo ? oldAngles : newAngles;

                for (auto &n : nodes) {
                    if (n.id == p.objectId) {
                        if (hasParentChange) {
                            n.parentId = nextParent;
                            n.origin = nextOrigin;
                            n.scale = nextScale;
                            n.angles = nextAngles;
                        }
                        if (const auto zIt = zById.find(n.id); zIt != zById.end()) {
                            n.zOrderIndex = zIt->second;
                        }
                        if (hasParentChange) {
                            pendingIpc.push_back(
                                makeBaseMutateReq(self->m_targetOutput, WE_MUTATE_REPARENT, n));
                        }
                        if (zById.contains(n.id)) {
                            pendingIpc.push_back(
                                makeBaseMutateReq(self->m_targetOutput, WE_MUTATE_REORDER_Z, n));
                        }
                    } else if (const auto zIt = zById.find(n.id); zIt != zById.end()) {
                        n.zOrderIndex = zIt->second;
                        pendingIpc.push_back(
                            makeBaseMutateReq(self->m_targetOutput, WE_MUTATE_REORDER_Z, n));
                    }
                }

                self->applyNodesPreservingSelection(std::move(nodes), p.objectId);
                for (const auto &req : pendingIpc) {
                    self->dispatchMutateIpc(req);
                }
                emit self->layerMutated(p.objectId);
            };

            if (m_commandBus) {
                const auto execApply = [applyMove](const SceneLayerMovePlan &p, bool isUndo) {
                    applyMove(p, isUndo);
                };
                m_commandBus->execute(std::make_unique<MoveSceneLayerCommand>(
                    target->name, plan, target->origin, newOrigin, target->scale, newScale,
                    target->angles, newAngles, execApply));
            } else {
                applyMove(plan, false);
            }
            selectObject(plan.objectId);
        });

    connect(m_model, &QAbstractItemModel::modelReset, this, [this]() {
        if (m_treeView && m_model->viewMode() == SceneLayerViewMode::HierarchyTree) {
            m_treeView->expandAll();
        }
    });

    m_treeView->expandAll();
    const QVariant pendingId = property("wwal_selected_object_id");
    if (pendingId.isValid() && pendingId.toInt() >= 0) {
        selectObject(pendingId.toInt());
    }
}

void SceneLayerTreeWidget::applyNodesPreservingSelection(std::vector<SceneLayerNode> nodes,
                                                         int32_t preferredId)
{
    if (!m_model) {
        return;
    }
    const int32_t prevSelected = selectedObjectId();
    int32_t targetId = (preferredId >= 0) ? preferredId : prevSelected;

    if (m_uiInitialized && m_treeView && m_treeView->selectionModel()) {
        const QSignalBlocker blocker(m_treeView->selectionModel());
        m_model->setNodes(std::move(nodes));
    } else {
        m_model->setNodes(std::move(nodes));
    }

    if (m_treeView && m_model->viewMode() == SceneLayerViewMode::HierarchyTree) {
        m_treeView->expandAll();
    }

    // Default-select the top visible/root layer on initial population or when the old layer goes away.
    if ((targetId < 0 || !m_model->findNodeById(targetId)) && !m_model->nodes().empty()) {
        if (m_proxyModel && m_proxyModel->rowCount() > 0) {
            const QModelIndex topProxy = m_proxyModel->index(0, 0);
            const QModelIndex topSrc = m_proxyModel->mapToSource(topProxy);
            if (topSrc.isValid()) {
                targetId = topSrc.data(kRoleObjectId).toInt();
            }
        }
        if (targetId < 0 || !m_model->findNodeById(targetId)) {
            targetId = m_model->nodes().front().id;
        }
    }

    if (const SceneLayerNode *n = (targetId >= 0) ? m_model->findNodeById(targetId) : nullptr) {
        m_selectedObjectId = n->id;
        setProperty("wwal_selected_object_id", n->id);
        if (m_uiInitialized && m_treeView && m_proxyModel && m_treeView->selectionModel()) {
            const QModelIndex src = m_model->indexForObjectId(n->id);
            const QModelIndex proxyIdx =
                src.isValid() ? m_proxyModel->mapFromSource(src) : QModelIndex();
            if (proxyIdx.isValid()) {
                const QSignalBlocker blocker(m_treeView->selectionModel());
                m_treeView->selectionModel()->setCurrentIndex(
                    proxyIdx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                m_treeView->scrollTo(proxyIdx);
            }
        }
        if (n->id != prevSelected) {
            emit objectSelected(n->id, n->kind);
        }
    } else {
        m_selectedObjectId = -1;
        setProperty("wwal_selected_object_id", -1);
        if (m_uiInitialized && m_treeView && m_treeView->selectionModel()) {
            const QSignalBlocker blocker(m_treeView->selectionModel());
            m_treeView->selectionModel()->clearSelection();
            m_treeView->selectionModel()->clearCurrentIndex();
        }
        if (prevSelected >= 0) {
            emit objectSelected(-1, WE_NODE_IMAGE);
        }
    }
}

void SceneLayerTreeWidget::executeFlagEdit(int32_t objectId,
                                           const SetLayerFlagsCommand::LayerFlagState &nextFlags)
{
    if (!m_model) {
        return;
    }
    const SceneLayerNode *cur = m_model->findNodeById(objectId);
    if (!cur) {
        return;
    }
    SetLayerFlagsCommand::LayerFlagState oldFlags;
    oldFlags.visible = cur->visible;
    oldFlags.locked = cur->locked;
    oldFlags.blendMode = cur->blendMode;
    oldFlags.alpha = cur->color[3];
    oldFlags.color = {cur->color[0], cur->color[1], cur->color[2]};

    QPointer<SceneLayerTreeWidget> self(this);
    const auto applyFn = [self](int32_t id, const SetLayerFlagsCommand::LayerFlagState &st) {
        if (!self || !self->m_model) {
            return;
        }
        auto nodes = self->m_model->nodes();
        for (auto &n : nodes) {
            if (n.id == id) {
                n.visible = st.visible;
                n.locked = st.locked;
                n.blendMode = st.blendMode;
                n.color = {st.color[0], st.color[1], st.color[2], st.alpha};
                const auto req =
                    makeBaseMutateReq(self->m_targetOutput, WE_MUTATE_SET_BLEND_VIS, n);
                self->applyNodesPreservingSelection(std::move(nodes), id);
                self->dispatchMutateIpc(req);
                emit self->layerMutated(id);
                return;
            }
        }
    };

    if (m_commandBus) {
        m_commandBus->execute(std::make_unique<SetLayerFlagsCommand>(objectId, cur->name, oldFlags,
                                                                     nextFlags, applyFn));
    } else {
        applyFn(objectId, nextFlags);
    }
}

void SceneLayerTreeWidget::loadFromSceneJson(const QString &sceneTreeJson)
{
    ensureUiInitialized();
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

    // Reserve explicit valid IDs upfront so missing-ID fallbacks never collide with later entries.
    std::unordered_set<int32_t> explicitIds;
    for (const QJsonValue &entry : arr) {
        if (!entry.isObject()) {
            continue;
        }
        const QJsonObject obj = entry.toObject();
        const int rawId = obj.value(QStringLiteral("id")).toInt(-1);
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
                        ? parseNodeKind(obj.value(QStringLiteral("kind")))
                        : parseNodeKind(obj.value(QStringLiteral("type")));
        node.blendMode = obj.contains(QStringLiteral("blendMode"))
                             ? parseBlendMode(obj.value(QStringLiteral("blendMode")))
                             : parseBlendMode(obj.value(QStringLiteral("blend_mode")));
        node.visible = obj.value(QStringLiteral("visible")).toBool(true);
        node.locked = obj.value(QStringLiteral("locked")).toBool(false);
        node.hasScript = obj.value(QStringLiteral("hasScript")).toBool(false) ||
                         !obj.value(QStringLiteral("scripts")).toArray().isEmpty();
        node.hasShader = obj.value(QStringLiteral("hasShader")).toBool(false) ||
                         !obj.value(QStringLiteral("effects")).toArray().isEmpty();
        node.hasAudioBinding = obj.value(QStringLiteral("hasAudioBinding")).toBool(false);
        node.origin = parseFloatArray<3>(obj.value(QStringLiteral("origin")), node.origin);
        node.scale = parseFloatArray<3>(obj.value(QStringLiteral("scale")), node.scale);
        node.angles = parseFloatArray<3>(obj.value(QStringLiteral("angles")), node.angles);
        node.size = parseFloatArray<2>(obj.value(QStringLiteral("size")), node.size);
        node.parallaxDepth =
            parseFloatArray<2>(obj.value(QStringLiteral("parallaxDepth")), node.parallaxDepth);
        node.color = parseFloatArray<4>(obj.value(QStringLiteral("color")), node.color);
        node.name = obj.value(QStringLiteral("name")).toString();
        parsedNodes.push_back(std::move(node));
        ++fallbackZ;
    }

    applyNodesPreservingSelection(std::move(parsedNodes));
}

void SceneLayerTreeWidget::selectObject(int32_t objectId)
{
    ensureUiInitialized();
    const int32_t prevSelected = selectedObjectId();
    if (objectId < 0) {
        m_selectedObjectId = -1;
        setProperty("wwal_selected_object_id", -1);
        if (m_treeView && m_treeView->selectionModel()) {
            const QSignalBlocker blocker(m_treeView->selectionModel());
            m_treeView->selectionModel()->clearSelection();
            m_treeView->selectionModel()->clearCurrentIndex();
        }
        if (prevSelected >= 0) {
            emit objectSelected(-1, WE_NODE_IMAGE);
        }
        return;
    }

    const SceneLayerNode *node = m_model ? m_model->findNodeById(objectId) : nullptr;
    m_selectedObjectId = node ? node->id : objectId;
    setProperty("wwal_selected_object_id", m_selectedObjectId);
    if (m_treeView && m_proxyModel && m_treeView->selectionModel()) {
        const QModelIndex src = m_model->indexForObjectId(objectId);
        if (src.isValid()) {
            const QModelIndex proxyIdx = m_proxyModel->mapFromSource(src);
            if (proxyIdx.isValid()) {
                const QSignalBlocker blocker(m_treeView->selectionModel());
                m_treeView->selectionModel()->setCurrentIndex(
                    proxyIdx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                m_treeView->scrollTo(proxyIdx);
            }
        }
    }
    if (node && (node->id != prevSelected)) {
        emit objectSelected(node->id, node->kind);
    }
}

int32_t SceneLayerTreeWidget::selectedObjectId() const noexcept
{
    if (m_uiInitialized && m_treeView && m_proxyModel && m_treeView->selectionModel()) {
        const QModelIndex cur = m_treeView->currentIndex();
        if (cur.isValid()) {
            const QModelIndex src = m_proxyModel->mapToSource(cur);
            if (src.isValid()) {
                return src.data(kRoleObjectId).toInt();
            }
        }
    }
    if (m_selectedObjectId >= 0) {
        return m_selectedObjectId;
    }
    const QVariant stored = property("wwal_selected_object_id");
    return stored.isValid() ? stored.toInt() : -1;
}

const SceneLayerNode *SceneLayerTreeWidget::selectedNode() const noexcept
{
    if (!m_model) {
        return nullptr;
    }
    return m_model->findNodeById(selectedObjectId());
}

void SceneLayerTreeWidget::keyPressEvent(QKeyEvent *event)
{
    const SceneLayerNode *node = selectedNode();
    if (!node || !m_model) {
        QWidget::keyPressEvent(event);
        return;
    }

    const int key = event->key();
    if (key == Qt::Key_H || key == Qt::Key_L) {
        SetLayerFlagsCommand::LayerFlagState nextFlags;
        nextFlags.visible = (key == Qt::Key_H) ? !node->visible : node->visible;
        nextFlags.locked = (key == Qt::Key_L) ? !node->locked : node->locked;
        nextFlags.blendMode = node->blendMode;
        nextFlags.alpha = node->color[3];
        nextFlags.color = {node->color[0], node->color[1], node->color[2]};

        const int32_t id = node->id;
        executeFlagEdit(id, nextFlags);
        selectObject(id);
        event->accept();
        return;
    }

    if (key == Qt::Key_Delete) {
        const bool cascadeDelete = event->modifiers().testFlag(Qt::ShiftModifier);
        if (node->locked) {
            emit statusMessage(
                QStringLiteral("Cannot delete locked layer '%1'").arg(node->name));
            event->accept();
            return;
        }
        const SceneLayerSnapshot snap = snapshotFromNode(*node);
        std::vector<int32_t> childIds;
        std::vector<SceneLayerNode> removedSubtree;
        struct ReparentTrsBackup {
            int32_t id{-1};
            int32_t oldParent{-1};
            std::array<float, 3> oldOrigin{};
            std::array<float, 3> oldScale{};
            std::array<float, 3> oldAngles{};
            std::array<float, 3> newOrigin{};
            std::array<float, 3> newScale{};
            std::array<float, 3> newAngles{};
        };
        std::vector<ReparentTrsBackup> reparentBackups;

        if (cascadeDelete) {
            // Collect subtree in parent-before-child BFS order so undo re-adds parents before
            // children, and execute removes children before parents.
            std::unordered_set<int32_t> visited{snap.objectId};
            std::queue<int32_t> bfs;
            removedSubtree.push_back(*node);
            bfs.push(snap.objectId);
            const auto &allNodes = m_model->nodes();
            while (!bfs.empty()) {
                const int32_t pid = bfs.front();
                bfs.pop();
                for (const auto &n : allNodes) {
                    if (n.parentId == pid && visited.insert(n.id).second) {
                        if (n.locked) {
                            emit statusMessage(
                                QStringLiteral("Cannot cascade-delete subtree containing locked "
                                               "layer '%1'")
                                    .arg(n.name));
                            event->accept();
                            return;
                        }
                        removedSubtree.push_back(n);
                        childIds.push_back(n.id);
                        bfs.push(n.id);
                    }
                }
            }
        } else {
            const QMatrix4x4 newParentWorld = (node->parentId != -1)
                                                  ? worldTrsMatrixOf(*m_model, node->parentId)
                                                  : QMatrix4x4();
            bool invOk = false;
            const QMatrix4x4 invNewParent = newParentWorld.inverted(&invOk);
            for (const auto &n : m_model->nodes()) {
                if (n.parentId == snap.objectId) {
                    if (n.locked) {
                        emit statusMessage(
                            QStringLiteral("Cannot reparent locked child layer '%1'").arg(n.name));
                        event->accept();
                        return;
                    }
                    childIds.push_back(n.id);
                    ReparentTrsBackup bk;
                    bk.id = n.id;
                    bk.oldParent = n.parentId;
                    bk.oldOrigin = n.origin;
                    bk.oldScale = n.scale;
                    bk.oldAngles = n.angles;
                    const QMatrix4x4 childWorld = worldTrsMatrixOf(*m_model, n.id);
                    const QMatrix4x4 localNext = invOk ? (invNewParent * childWorld) : childWorld;
                    decomposeTrsMatrix(localNext, bk.newOrigin, bk.newScale, bk.newAngles);
                    reparentBackups.push_back(bk);
                }
            }
        }

        QPointer<SceneLayerTreeWidget> self(this);
        const auto restoreFn = [self, cascadeDelete, removedSubtree, reparentBackups](
                                   const SceneLayerSnapshot &layer,
                                   const std::vector<int32_t> &reparentedChildren, bool isRestore) {
            if (!self || !self->m_model) {
                return;
            }
            auto nodes = self->m_model->nodes();
            if (!isRestore) {
                std::vector<SceneLayerNode> reparentedNodes;
                if (cascadeDelete) {
                    std::unordered_set<int32_t> removeSet;
                    removeSet.reserve(removedSubtree.size() + 1u);
                    removeSet.insert(layer.objectId);
                    for (const auto &sn : removedSubtree) {
                        removeSet.insert(sn.id);
                    }
                    std::erase_if(nodes, [&removeSet](const SceneLayerNode &n) {
                        return removeSet.contains(n.id);
                    });
                    self->applyNodesPreservingSelection(std::move(nodes));
                    for (auto it = removedSubtree.rbegin(); it != removedSubtree.rend(); ++it) {
                        self->dispatchMutateIpc(makeSnapshotMutateReq(
                            self->m_targetOutput, WE_MUTATE_REMOVE_OBJECT, snapshotFromNode(*it)));
                    }
                } else {
                    std::unordered_set<int32_t> childSet(reparentedChildren.begin(),
                                                         reparentedChildren.end());
                    for (auto &n : nodes) {
                        if (childSet.contains(n.id)) {
                            n.parentId = layer.parentId;
                            for (const auto &bk : reparentBackups) {
                                if (bk.id == n.id) {
                                    n.origin = bk.newOrigin;
                                    n.scale = bk.newScale;
                                    n.angles = bk.newAngles;
                                    break;
                                }
                            }
                            reparentedNodes.push_back(n);
                        }
                    }
                    std::erase_if(nodes, [&layer](const SceneLayerNode &n) {
                        return n.id == layer.objectId;
                    });
                    self->applyNodesPreservingSelection(std::move(nodes));
                    for (const auto &rn : reparentedNodes) {
                        self->dispatchMutateIpc(
                            makeBaseMutateReq(self->m_targetOutput, WE_MUTATE_REPARENT, rn));
                    }
                    self->dispatchMutateIpc(makeSnapshotMutateReq(
                        self->m_targetOutput, WE_MUTATE_REMOVE_OBJECT, layer));
                }
            } else {
                if (cascadeDelete) {
                    std::unordered_set<int32_t> existingIds;
                    for (const auto &n : nodes) {
                        existingIds.insert(n.id);
                    }
                    for (const auto &sub : removedSubtree) {
                        if (existingIds.insert(sub.id).second) {
                            nodes.push_back(sub);
                        }
                    }
                    self->applyNodesPreservingSelection(std::move(nodes), layer.objectId);
                    for (const auto &sub : removedSubtree) {
                        self->dispatchMutateIpc(makeSnapshotMutateReq(
                            self->m_targetOutput, WE_MUTATE_ADD_OBJECT, snapshotFromNode(sub)));
                    }
                } else {
                    std::unordered_set<int32_t> childSet(reparentedChildren.begin(),
                                                         reparentedChildren.end());
                    std::vector<SceneLayerNode> reparentedNodes;
                    for (auto &n : nodes) {
                        if (childSet.contains(n.id)) {
                            n.parentId = layer.objectId;
                            for (const auto &bk : reparentBackups) {
                                if (bk.id == n.id) {
                                    n.origin = bk.oldOrigin;
                                    n.scale = bk.oldScale;
                                    n.angles = bk.oldAngles;
                                    break;
                                }
                            }
                            reparentedNodes.push_back(n);
                        }
                    }
                    if (std::ranges::none_of(nodes, [&layer](const SceneLayerNode &n) {
                            return n.id == layer.objectId;
                        })) {
                        nodes.push_back(nodeFromSnapshot(layer));
                    }
                    self->applyNodesPreservingSelection(std::move(nodes), layer.objectId);
                    self->dispatchMutateIpc(
                        makeSnapshotMutateReq(self->m_targetOutput, WE_MUTATE_ADD_OBJECT, layer));
                    for (const auto &rn : reparentedNodes) {
                        self->dispatchMutateIpc(
                            makeBaseMutateReq(self->m_targetOutput, WE_MUTATE_REPARENT, rn));
                    }
                }
            }
            emit self->layerMutated(layer.objectId);
        };

        if (m_commandBus) {
            m_commandBus->execute(
                std::make_unique<RemoveSceneLayerCommand>(snap, std::move(childIds), restoreFn));
        } else {
            restoreFn(snap, childIds, false);
        }
        event->accept();
        return;
    }

    QWidget::keyPressEvent(event);
}

void SceneLayerTreeWidget::dispatchMutateIpc(const waywal_ipc_req_scene_mutate_t &req)
{
    if (!m_ipcClient) {
        return;
    }
    const auto trySend = [&req](auto *clientPtr) {
        if constexpr (requires { clientPtr->sendSceneMutate(req); }) {
            clientPtr->sendSceneMutate(req);
        } else if constexpr (requires { clientPtr->requestSceneMutate(req); }) {
            clientPtr->requestSceneMutate(req);
        } else {
            (void)clientPtr;
            (void)req;
        }
    };
    trySend(m_ipcClient);
}

} // namespace waywal::studio
