// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/graph/node_graph_editor_widget.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFile>
#include <QGraphicsPathItem>
#include <QGraphicsSceneMouseEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScreen>
#include <QScrollBar>
#include <QStyleOptionGraphicsItem>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>

namespace waywal::studio {

namespace {

/* ========================================================================= */
/* Design Tokens & Theme Colors                                              */
/* ========================================================================= */

constexpr QColor kColorCanvasBg(0x0E, 0x11, 0x17);     // primitive.color.slate.950
constexpr QColor kColorGridMinor(0x18, 0x1B, 0x22);    // Grid minor
constexpr QColor kColorGridMajor(0x1E, 0x23, 0x2E);    // Grid major
constexpr QColor kColorNodeSurface(0x12, 0x15, 0x1C);  // semantic.studio.node.surface
constexpr QColor kColorNodeHeader(0x1E, 0x23, 0x2E);   // primitive.color.slate.800
constexpr QColor kColorFocusRing(0x38, 0xBD, 0xF8);    // semantic.studio.focus.ring
constexpr QColor kColorWireActive(0x22, 0xD3, 0xEE);   // component.nodegraph.wire.active
constexpr QColor kColorWireCycleErr(0xF8, 0x71, 0x71); // component.nodegraph.wire.cycle_err
constexpr QColor kColorSnapHalo(0x4E, 0xCD, 0xC4, 90); // component.nodegraph.snap.halo

constexpr float kNodeWidth = kGraphNodeWidth;
constexpr float kHeaderHeight = kGraphHeaderHeight;
constexpr float kPinRowHeight = kGraphPinRowHeight;
constexpr float kPinRadius = 6.0f;

/* ========================================================================= */
/* Dual-Coded WCAG AAA Pin Colors & Geometric Silhouettes                    */
/* ========================================================================= */

QColor pinTypeColor(uint32_t pinType) noexcept
{
    switch (pinType) {
    case WAYWAL_PIN_FLOAT:
        return QColor(0xFF, 0x6B, 0x6B); // Coral #FF6B6B
    case WAYWAL_PIN_VEC2:
        return QColor(0x4E, 0xCD, 0xC4); // Mint #4ECDC4
    case WAYWAL_PIN_VEC3:
        return QColor(0xFF, 0xD1, 0x66); // Amber #FFD166
    case WAYWAL_PIN_VEC4:
        return QColor(0xC0, 0x84, 0xFC); // Lavender #C084FC
    case WAYWAL_PIN_INT:
        return QColor(0x38, 0xBD, 0xF8); // Sky Blue #38BDF8
    case WAYWAL_PIN_BOOL:
        return QColor(0xA3, 0xE6, 0x35); // Lime #A3E635
    case WAYWAL_PIN_SAMPLER2D:
        return QColor(0xFB, 0x92, 0x3C); // Orange #FB923C
    case WAYWAL_PIN_AUDIO_SPECTRUM:
        return QColor(0xF4, 0x72, 0xB6); // Magenta #F472B6
    default:
        return QColor(0x94, 0xA3, 0xB8);
    }
}

const char *pinTypeName(uint32_t pinType) noexcept
{
    switch (pinType) {
    case WAYWAL_PIN_FLOAT:
        return "float";
    case WAYWAL_PIN_VEC2:
        return "vec2";
    case WAYWAL_PIN_VEC3:
        return "vec3";
    case WAYWAL_PIN_VEC4:
        return "vec4";
    case WAYWAL_PIN_INT:
        return "int";
    case WAYWAL_PIN_BOOL:
        return "bool";
    case WAYWAL_PIN_SAMPLER2D:
        return "sampler2D";
    case WAYWAL_PIN_AUDIO_SPECTRUM:
        return "audio";
    default:
        return "unknown";
    }
}

void drawPinSilhouette(QPainter *painter, uint32_t pinType, const QPointF &center, float radius,
                       const QColor &color, bool connected, float opacity = 1.0f)
{
    painter->save();
    painter->setOpacity(opacity);
    QPen pen(color, 1.5f);
    QBrush brush = connected ? QBrush(color) : QBrush(kColorNodeSurface);
    painter->setPen(pen);
    painter->setBrush(brush);

    switch (pinType) {
    case WAYWAL_PIN_FLOAT: // Circle (float)
        painter->drawEllipse(center, radius, radius);
        break;

    case WAYWAL_PIN_VEC2: { // Horizontal Capsule (vec2)
        QRectF cap(center.x() - radius * 1.2f, center.y() - radius * 0.7f, radius * 2.4f,
                   radius * 1.4f);
        painter->drawRoundedRect(cap, radius * 0.7f, radius * 0.7f);
        break;
    }

    case WAYWAL_PIN_VEC3: { // Upward Equilateral Triangle (vec3)
        QPolygonF tri;
        tri << QPointF(center.x(), center.y() - radius * 1.15f)
            << QPointF(center.x() + radius * 1.0f, center.y() + radius * 0.75f)
            << QPointF(center.x() - radius * 1.0f, center.y() + radius * 0.75f);
        painter->drawPolygon(tri);
        break;
    }

    case WAYWAL_PIN_VEC4: { // Diamond Rhombus (vec4)
        QPolygonF dia;
        dia << QPointF(center.x(), center.y() - radius * 1.15f)
            << QPointF(center.x() + radius * 1.15f, center.y())
            << QPointF(center.x(), center.y() + radius * 1.15f)
            << QPointF(center.x() - radius * 1.15f, center.y());
        painter->drawPolygon(dia);
        break;
    }

    case WAYWAL_PIN_INT: { // Rounded Square (int)
        QRectF sq(center.x() - radius * 0.9f, center.y() - radius * 0.9f, radius * 1.8f,
                  radius * 1.8f);
        painter->drawRoundedRect(sq, 2.5f, 2.5f);
        break;
    }

    case WAYWAL_PIN_BOOL: { // Hexagon (bool)
        QPolygonF hex;
        for (int i = 0; i < 6; ++i) {
            float a = static_cast<float>(i) * 3.14159265f / 3.0f;
            hex << QPointF(center.x() + radius * std::cos(a), center.y() + radius * std::sin(a));
        }
        painter->drawPolygon(hex);
        break;
    }

    case WAYWAL_PIN_SAMPLER2D: { // Double-Bordered Square (sampler2D)
        QRectF sq(center.x() - radius * 0.9f, center.y() - radius * 0.9f, radius * 1.8f,
                  radius * 1.8f);
        painter->drawRect(sq);
        QRectF inSq(center.x() - radius * 0.45f, center.y() - radius * 0.45f, radius * 0.9f,
                    radius * 0.9f);
        painter->drawRect(inSq);
        break;
    }

    case WAYWAL_PIN_AUDIO_SPECTRUM: { // Concentric Waveform Ring (audio)
        painter->drawEllipse(center, radius, radius);
        painter->drawEllipse(center, radius * 0.45f, radius * 0.45f);
        break;
    }

    default:
        painter->drawEllipse(center, radius, radius);
        break;
    }
    painter->restore();
}

QColor nodeCategoryColor(uint32_t opcode) noexcept
{
    if (opcode <= 9) {
        return QColor(0x89, 0xB4, 0xFA); // Inputs & Sensors (Blue)
    }
    if (opcode <= 29) {
        return QColor(0xCB, 0xA6, 0xF7); // Math & Vector (Mauve)
    }
    if (opcode <= 39) {
        return QColor(0xA6, 0xE3, 0xA1); // Noise & UV (Green)
    }
    if (opcode <= 47) {
        return QColor(0xF9, 0xE2, 0xAF); // Color & Lighting (Yellow)
    }
    if (opcode == 48) {
        return QColor(0xFA, 0xB3, 0x87); // Built-In Effect (Peach)
    }
    return QColor(0xF3, 0x8B, 0xA8); // Output Pass Sinks (Red)
}

void resyncPinConnectedFlags(waywal_shader_graph_t *graph) noexcept
{
    if (!graph) {
        return;
    }
    for (uint32_t i = 0; i < graph->node_count; ++i) {
        auto &n = graph->nodes[i];
        for (uint32_t p = 0; p < n.input_pin_count && p < WAYWAL_GRAPH_MAX_PINS_PER_NODE; ++p) {
            n.inputs[p].flags &= ~WAYWAL_PIN_FLAG_CONNECTED;
        }
        for (uint32_t p = 0; p < n.output_pin_count && p < WAYWAL_GRAPH_MAX_PINS_PER_NODE; ++p) {
            n.outputs[p].flags &= ~WAYWAL_PIN_FLAG_CONNECTED;
        }
    }
    for (uint32_t eIdx = 0; eIdx < graph->edge_count; ++eIdx) {
        const auto &e = graph->edges[eIdx];
        if (auto *src = waywal_shader_graph_find_node(graph, e.src_node_id)) {
            if (e.src_pin_index < src->output_pin_count &&
                e.src_pin_index < WAYWAL_GRAPH_MAX_PINS_PER_NODE) {
                src->outputs[e.src_pin_index].flags |= WAYWAL_PIN_FLAG_CONNECTED;
            }
        }
        if (auto *dst = waywal_shader_graph_find_node(graph, e.dst_node_id)) {
            if (e.dst_pin_index < dst->input_pin_count &&
                e.dst_pin_index < WAYWAL_GRAPH_MAX_PINS_PER_NODE) {
                dst->inputs[e.dst_pin_index].flags |= WAYWAL_PIN_FLAG_CONNECTED;
            }
        }
    }
}

} // namespace

/* ========================================================================= */
/* Forward Declarations for Graphics Items                                   */
/* ========================================================================= */

class NodeItem;
class WireItem;

/* ========================================================================= */
/* Custom Interactive QGraphicsView Canvas                                   */
/* ========================================================================= */

class GraphCanvasView final : public QGraphicsView {
  public:
    explicit GraphCanvasView(NodeGraphEditorWidget *editor, QWidget *parent = nullptr)
        : QGraphicsView(parent), m_editor(editor)
    {
        setRenderHint(QPainter::Antialiasing, true);
        setRenderHint(QPainter::TextAntialiasing, true);
        setRenderHint(QPainter::SmoothPixmapTransform, true);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        setResizeAnchor(QGraphicsView::AnchorUnderMouse);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setDragMode(QGraphicsView::NoDrag);
        setFocusPolicy(Qt::StrongFocus);
        setAcceptDrops(false);
        if (viewport()) {
            viewport()->setAcceptDrops(false);
        }
        setMouseTracking(true);
    }

    void startDraggingWire(uint32_t nodeId, uint32_t pinIdx, uint32_t pinType, uint32_t pinFlags,
                           bool isOutput, const QPointF &startScenePos)
    {
        m_wireDragging = true;
        m_dragSrcNodeId = nodeId;
        m_dragSrcPinIdx = pinIdx;
        m_dragPinType = pinType;
        m_dragPinFlags = pinFlags;
        m_dragIsOutput = isOutput;
        m_dragStartPos = startScenePos;
        m_dragCurrentPos = startScenePos;
        m_snappedPinValid = false;
        viewport()->update();
    }

    void stopDraggingWire()
    {
        m_wireDragging = false;
        m_snappedPinValid = false;
        viewport()->update();
    }

    [[nodiscard]] bool isDraggingWire() const noexcept { return m_wireDragging; }

  protected:
    void drawBackground(QPainter *painter, const QRectF &rect) override
    {
        painter->fillRect(rect, kColorCanvasBg);

        const qreal minorGrid = 25.0;
        const qreal majorGrid = 100.0;

        qreal left = std::floor(rect.left() / minorGrid) * minorGrid;
        qreal top = std::floor(rect.top() / minorGrid) * minorGrid;

        // Minor grid lines
        painter->setPen(QPen(kColorGridMinor, 1.0));
        QVarLengthArray<QLineF, 128> minorLines;
        for (qreal x = left; x < rect.right(); x += minorGrid) {
            if (std::abs(std::remainder(x, majorGrid)) > 1.0) {
                minorLines.append(QLineF(x, rect.top(), x, rect.bottom()));
            }
        }
        for (qreal y = top; y < rect.bottom(); y += minorGrid) {
            if (std::abs(std::remainder(y, majorGrid)) > 1.0) {
                minorLines.append(QLineF(rect.left(), y, rect.right(), y));
            }
        }
        painter->drawLines(minorLines.data(), minorLines.size());

        // Major grid lines
        painter->setPen(QPen(kColorGridMajor, 1.5));
        QVarLengthArray<QLineF, 64> majorLines;
        qreal majorLeft = std::floor(rect.left() / majorGrid) * majorGrid;
        qreal majorTop = std::floor(rect.top() / majorGrid) * majorGrid;
        for (qreal x = majorLeft; x < rect.right(); x += majorGrid) {
            majorLines.append(QLineF(x, rect.top(), x, rect.bottom()));
        }
        for (qreal y = majorTop; y < rect.bottom(); y += majorGrid) {
            majorLines.append(QLineF(rect.left(), y, rect.right(), y));
        }
        painter->drawLines(majorLines.data(), majorLines.size());
    }

    void drawForeground(QPainter *painter, const QRectF &rect) override
    {
        QGraphicsView::drawForeground(painter, rect);

        if (!m_wireDragging) {
            return;
        }

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        QPointF p0 = m_dragStartPos;
        QPointF p3 = m_dragCurrentPos;
        if (!m_dragIsOutput) {
            std::swap(p0, p3);
        }

        float dx = static_cast<float>(p3.x() - p0.x());
        float tangentDist = std::max(60.0f, 0.45f * std::abs(dx));
        QPointF p1 = p0 + QPointF(tangentDist, 0.0);
        QPointF p2 = p3 - QPointF(tangentDist, 0.0);

        QPainterPath path;
        path.moveTo(p0);
        path.cubicTo(p1, p2, p3);

        QPen pen(kColorWireActive, 2.5f, Qt::DashLine);
        painter->setPen(pen);
        painter->strokePath(path, pen);

        // Render magnetic snap halo scaled consistently in screen space
        if (m_snappedPinValid) {
            const float invScale = 1.0f / std::max(0.15f, m_editor->zoomScale());
            const qreal haloRadius = 16.0 * static_cast<qreal>(invScale);
            painter->setPen(QPen(QColor(0x4E, 0xCD, 0xC4), 1.5f * invScale));
            painter->setBrush(QBrush(kColorSnapHalo));
            painter->drawEllipse(m_snappedPinPos, haloRadius, haloRadius);
        }

        painter->restore();
    }

    bool focusNextPrevChild(bool /*next*/) override { return false; }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (m_editor &&
            ((event->key() == Qt::Key_A && (event->modifiers() & Qt::ShiftModifier)) ||
             event->key() == Qt::Key_Tab || event->key() == Qt::Key_Space ||
             event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace ||
             event->matches(QKeySequence::Undo) || event->matches(QKeySequence::Redo))) {
            m_editor->keyPressEvent(event);
            return;
        }
        QGraphicsView::keyPressEvent(event);
    }

    void wheelEvent(QWheelEvent *event) override
    {
        const float zoomFactor = event->angleDelta().y() > 0 ? 1.15f : (1.0f / 1.15f);
        m_editor->setZoomScale(m_editor->zoomScale() * zoomFactor);
        event->accept();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::MiddleButton ||
            (event->button() == Qt::LeftButton && (event->modifiers() & Qt::AltModifier))) {
            m_isPanning = true;
            m_panLastPos = event->pos();
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }

        QGraphicsView::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_isPanning) {
            QPoint delta = event->pos() - m_panLastPos;
            m_panLastPos = event->pos();
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
            verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
            event->accept();
            return;
        }

        if (m_wireDragging) {
            const QPointF scenePos = mapToScene(event->pos());
            m_dragCurrentPos = scenePos;
            m_snappedPinValid = false;

            const float sceneSnapRadius = NodeGraphEditorWidget::kMagneticSnapRadiusPx /
                                          std::max(0.15f, m_editor->zoomScale());
            const auto nearest = m_editor->pinSpatialGrid().findNearestCompatible(
                scenePos, !m_dragIsOutput, m_dragPinType, sceneSnapRadius, m_dragSrcNodeId,
                m_dragPinFlags);
            if (nearest.has_value()) {
                m_snappedPinPos = nearest->scenePos;
                m_dragCurrentPos = nearest->scenePos;
                m_snappedDstNodeId = nearest->nodeId;
                m_snappedDstPinIdx = nearest->pinIdx;
                m_snappedPinValid = true;
            }

            viewport()->update();
            event->accept();
            return;
        }

        QGraphicsView::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (m_isPanning &&
            (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton)) {
            m_isPanning = false;
            setCursor(Qt::ArrowCursor);
            event->accept();
            return;
        }

        if (m_wireDragging && event->button() == Qt::LeftButton) {
            if (m_snappedPinValid) {
                if (m_dragIsOutput) {
                    m_editor->connectPins(m_dragSrcNodeId, m_dragSrcPinIdx, m_snappedDstNodeId,
                                          m_snappedDstPinIdx);
                } else {
                    m_editor->connectPins(m_snappedDstNodeId, m_snappedDstPinIdx, m_dragSrcNodeId,
                                          m_dragSrcPinIdx);
                }
            } else {
                // Released on empty canvas -> open quick connect popup
                QPointF dropPos = mapToScene(event->pos());
                m_editor->openQuickConnectPopup(dropPos, static_cast<int32_t>(m_dragSrcNodeId),
                                                static_cast<int32_t>(m_dragSrcPinIdx),
                                                m_dragIsOutput);
            }
            stopDraggingWire();
            event->accept();
            return;
        }

        QGraphicsView::mouseReleaseEvent(event);
    }

  private:
    NodeGraphEditorWidget *m_editor{nullptr};
    bool m_isPanning{false};
    QPoint m_panLastPos;

    bool m_wireDragging{false};
    uint32_t m_dragSrcNodeId{0};
    uint32_t m_dragSrcPinIdx{0};
    uint32_t m_dragPinType{0};
    uint32_t m_dragPinFlags{0};
    bool m_dragIsOutput{true};
    QPointF m_dragStartPos;
    QPointF m_dragCurrentPos;

    bool m_snappedPinValid{false};
    QPointF m_snappedPinPos;
    uint32_t m_snappedDstNodeId{0};
    uint32_t m_snappedDstPinIdx{0};
};

/* ========================================================================= */
/* Node Graphics Item                                                        */
/* ========================================================================= */

class NodeItem final : public QGraphicsItem {
  public:
    NodeItem(uint32_t nodeId, NodeGraphEditorWidget *editor) : m_nodeId(nodeId), m_editor(editor)
    {
        setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
        setAcceptHoverEvents(true);
        updateFromModel();
    }

    [[nodiscard]] uint32_t nodeId() const noexcept { return m_nodeId; }

    void updateFromModel()
    {
        const auto &graph = m_editor->graph();
        const waywal_graph_node_t *node = waywal_shader_graph_find_node_const(&graph, m_nodeId);
        if (!node) {
            return;
        }

        setPos(node->canvas_pos[0], node->canvas_pos[1]);

        uint32_t maxPins =
            std::max(std::min<uint32_t>(node->input_pin_count, WAYWAL_GRAPH_MAX_PINS_PER_NODE),
                     std::min<uint32_t>(node->output_pin_count, WAYWAL_GRAPH_MAX_PINS_PER_NODE));
        if (maxPins == 0) {
            maxPins = 1;
        }

        const bool collapsed = (node->flags & WAYWAL_NODE_FLAG_COLLAPSED) != 0;
        float h = collapsed ? (kHeaderHeight + 10.0f)
                            : (kHeaderHeight + 16.0f + maxPins * kPinRowHeight + 10.0f);
        if (!collapsed && (node->flags & WAYWAL_NODE_FLAG_PREVIEW_EXPANDED) &&
            m_editor->currentLod() == GraphSemanticZoomLod::FullDetail) {
            h += 74.0f; // 64x64 thumbnail + padding
        }
        m_height = h;

        prepareGeometryChange();
    }

    [[nodiscard]] QRectF boundingRect() const override
    {
        return QRectF(-4.0, -4.0, kNodeWidth + 8.0, m_height + 8.0);
    }

    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override
    {
        (void)option;
        (void)widget;

        const auto &graph = m_editor->graph();
        const waywal_graph_node_t *node = waywal_shader_graph_find_node_const(&graph, m_nodeId);
        if (!node) {
            return;
        }

        GraphSemanticZoomLod lod = m_editor->currentLod();
        painter->setRenderHint(QPainter::Antialiasing, true);

        const auto &schedule = m_editor->lastSchedule();
        const bool hasCycleHighlight =
            (schedule.has_cycle_error != 0 && schedule.cycle_node_id == m_nodeId) ||
            (m_editor->m_cycleHighlightSrcNodeId != 0 &&
             (m_nodeId == m_editor->m_cycleHighlightSrcNodeId ||
              m_nodeId == m_editor->m_cycleHighlightDstNodeId));

        // 1. Draw Card Surface
        QRectF cardRect(0.0, 0.0, kNodeWidth, m_height);
        painter->setBrush(QBrush(kColorNodeSurface));
        QPen borderPen = hasCycleHighlight ? QPen(kColorWireCycleErr, 2.5f)
                                           : (isSelected() ? QPen(kColorFocusRing, 2.0f)
                                                           : QPen(kColorNodeHeader, 1.0f));
        painter->setPen(borderPen);
        painter->drawRoundedRect(cardRect, 8.0, 8.0);

        // 2. Category Color Header Strip
        QColor catCol = nodeCategoryColor(node->opcode);
        painter->setBrush(QBrush(catCol));
        painter->setPen(Qt::NoPen);
        painter->drawRoundedRect(QRectF(0.0, 0.0, kNodeWidth, 4.0), 2.0, 2.0);

        // 3. Header Background
        painter->setBrush(QBrush(kColorNodeHeader));
        painter->drawRoundedRect(QRectF(0.0, 4.0, kNodeWidth, kHeaderHeight), 4.0, 4.0);

        // Header Title
        painter->setPen(QColor(0xCD, 0xD6, 0xF4));
        QFont font = painter->font();
        font.setBold(true);
        font.setPointSize(lod == GraphSemanticZoomLod::MacroOverview ? 11 : 9);
        painter->setFont(font);
        painter->drawText(QRectF(12.0, 4.0, kNodeWidth - 24.0, kHeaderHeight),
                          Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8(node->title));

        // Cull pins and preview thumbnails when collapsed or in MacroOverview
        if ((node->flags & WAYWAL_NODE_FLAG_COLLAPSED) != 0 ||
            lod == GraphSemanticZoomLod::MacroOverview) {
            return;
        }

        // 4. Pin Sockets and Labels
        font.setBold(false);
        font.setPointSize(8);
        painter->setFont(font);

        const uint32_t inCount =
            std::min<uint32_t>(node->input_pin_count, WAYWAL_GRAPH_MAX_PINS_PER_NODE);
        for (uint32_t i = 0; i < inCount; ++i) {
            const auto &pin = node->inputs[i];
            if ((pin.flags & WAYWAL_PIN_FLAG_HIDDEN) != 0) {
                continue;
            }
            const QPointF center(0.0, graphPinCenterY(i, kHeaderHeight, kPinRowHeight));
            const bool connected = (pin.flags & WAYWAL_PIN_FLAG_CONNECTED) != 0;
            drawPinSilhouette(painter, pin.pin_type, center, kPinRadius, pinTypeColor(pin.pin_type),
                              connected);

            painter->setPen(QColor(0xA6, 0xAD, 0xC8));
            painter->drawText(QRectF(10.0, center.y() - 10.0, kNodeWidth * 0.45, 20.0),
                              Qt::AlignVCenter | Qt::AlignLeft,
                              QString::fromUtf8(pin.label[0] ? pin.label : pin.name));
        }

        const uint32_t outCount =
            std::min<uint32_t>(node->output_pin_count, WAYWAL_GRAPH_MAX_PINS_PER_NODE);
        for (uint32_t i = 0; i < outCount; ++i) {
            const auto &pin = node->outputs[i];
            if ((pin.flags & WAYWAL_PIN_FLAG_HIDDEN) != 0) {
                continue;
            }
            const QPointF center(kNodeWidth, graphPinCenterY(i, kHeaderHeight, kPinRowHeight));
            const bool connected = (pin.flags & WAYWAL_PIN_FLAG_CONNECTED) != 0;
            drawPinSilhouette(painter, pin.pin_type, center, kPinRadius, pinTypeColor(pin.pin_type),
                              connected);

            painter->setPen(QColor(0xA6, 0xAD, 0xC8));
            painter->drawText(
                QRectF(kNodeWidth * 0.5, center.y() - 10.0, kNodeWidth * 0.5 - 10.0, 20.0),
                Qt::AlignVCenter | Qt::AlignRight,
                QString::fromUtf8(pin.label[0] ? pin.label : pin.name));
        }

        // 5. FullDetail Controls & Live Thumbnail Preview
        if (lod == GraphSemanticZoomLod::FullDetail) {
            if (node->flags & WAYWAL_NODE_FLAG_PREVIEW_EXPANDED) {
                float thumbY = m_height - 70.0f;
                QRectF thumbRect((kNodeWidth - 64.0f) * 0.5f, thumbY, 64.0f, 64.0f);
                painter->setBrush(QBrush(QColor(0x0E, 0x11, 0x17)));
                painter->setPen(QPen(QColor(0x31, 0x32, 0x44), 1.0f));
                painter->drawRect(thumbRect);

                const auto thumbIt = m_editor->m_nodeThumbnails.constFind(m_nodeId);
                if (thumbIt != m_editor->m_nodeThumbnails.constEnd() && !thumbIt->isNull()) {
                    painter->drawImage(thumbRect.adjusted(1.0, 1.0, -1.0, -1.0), *thumbIt);
                } else {
                    painter->setPen(QColor(0x58, 0x5B, 0x70));
                    painter->drawText(thumbRect, Qt::AlignCenter, QStringLiteral("64x64"));
                }
            }
        }
    }

  protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override
    {
        if (change == ItemPositionHasChanged && m_editor && !m_editor->m_rebuildingScene &&
            !m_editor->m_applyingMoveInPlace) {
            m_editor->updateWiresFromLiveItemPositions();
        }
        return QGraphicsItem::itemChange(change, value);
    }

    void mousePressEvent(QGraphicsSceneMouseEvent *event) override
    {
        const auto &graph = m_editor->graph();
        const waywal_graph_node_t *node = waywal_shader_graph_find_node_const(&graph, m_nodeId);
        const bool pinsInteractive =
            node && ((node->flags & WAYWAL_NODE_FLAG_COLLAPSED) == 0) &&
            (m_editor->currentLod() != GraphSemanticZoomLod::MacroOverview);
        if (pinsInteractive) {
            const QPointF local = event->pos();

            const uint32_t inCount =
                std::min<uint32_t>(node->input_pin_count, WAYWAL_GRAPH_MAX_PINS_PER_NODE);
            for (uint32_t i = 0; i < inCount; ++i) {
                if ((node->inputs[i].flags & WAYWAL_PIN_FLAG_HIDDEN) != 0) {
                    continue;
                }
                const QPointF center(0.0, graphPinCenterY(i, kHeaderHeight, kPinRowHeight));
                if (std::hypot(local.x() - center.x(), local.y() - center.y()) <= 12.0) {
                    if (auto *view = dynamic_cast<GraphCanvasView *>(scene()->views().value(0))) {
                        view->startDraggingWire(m_nodeId, i, node->inputs[i].pin_type,
                                                node->inputs[i].flags, false, mapToScene(center));
                        event->accept();
                        return;
                    }
                }
            }

            const uint32_t outCount =
                std::min<uint32_t>(node->output_pin_count, WAYWAL_GRAPH_MAX_PINS_PER_NODE);
            for (uint32_t i = 0; i < outCount; ++i) {
                if ((node->outputs[i].flags & WAYWAL_PIN_FLAG_HIDDEN) != 0) {
                    continue;
                }
                const QPointF center(kNodeWidth, graphPinCenterY(i, kHeaderHeight, kPinRowHeight));
                if (std::hypot(local.x() - center.x(), local.y() - center.y()) <= 12.0) {
                    if (auto *view = dynamic_cast<GraphCanvasView *>(scene()->views().value(0))) {
                        view->startDraggingWire(m_nodeId, i, node->outputs[i].pin_type,
                                                node->outputs[i].flags, true, mapToScene(center));
                        event->accept();
                        return;
                    }
                }
            }
        }

        m_editor->nodeSelected(m_nodeId);
        QGraphicsItem::mousePressEvent(event);

        // Snapshot positions after base press updates multi-selection so co-selected nodes
        // dragged together by Qt record their true starting positions.
        m_groupDragStartPositions.clear();
        for (const auto &[nid, nItem] : m_editor->m_nodeItems) {
            if (nItem && (nItem == this || nItem->isSelected())) {
                m_groupDragStartPositions.emplace_back(nid, nItem->pos());
            }
        }
        std::sort(m_groupDragStartPositions.begin(), m_groupDragStartPositions.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });
    }

    void mouseReleaseEvent(QGraphicsSceneMouseEvent *event) override
    {
        QGraphicsItem::mouseReleaseEvent(event);

        if (m_groupDragStartPositions.empty()) {
            return;
        }

        std::vector<MoveNodesCommand::NodeMoveDelta> deltas;
        deltas.reserve(m_groupDragStartPositions.size());
        for (const auto &[nid, startPos] : m_groupDragStartPositions) {
            const auto it = m_editor->m_nodeItems.find(nid);
            if (it != m_editor->m_nodeItems.end() && it->second) {
                const QPointF endPos = it->second->pos();
                if (endPos != startPos) {
                    deltas.push_back({nid, startPos, endPos});
                }
            }
        }
        m_groupDragStartPositions.clear();

        if (!deltas.empty()) {
            m_editor->moveNodes(deltas);
        }
    }

  private:
    uint32_t m_nodeId{0};
    NodeGraphEditorWidget *m_editor{nullptr};
    float m_height{120.0f};
    std::vector<std::pair<uint32_t, QPointF>> m_groupDragStartPositions;
};

/* ========================================================================= */
/* Cubic Bézier Wire Graphics Item                                           */
/* ========================================================================= */

class WireItem final : public QGraphicsPathItem {
  public:
    WireItem(uint32_t edgeId, NodeGraphEditorWidget *editor) : m_edgeId(edgeId), m_editor(editor)
    {
        setZValue(-1.0);
        setAcceptHoverEvents(true);
        updatePath();
    }

    [[nodiscard]] uint32_t edgeId() const noexcept { return m_edgeId; }
    [[nodiscard]] const CachedWireGeometry &cachedGeometry() const noexcept { return m_geom; }

    [[nodiscard]] QRectF boundingRect() const override { return m_geom.cachedBounds; }

    void updatePath()
    {
        const auto &graph = m_editor->graph();
        const waywal_graph_edge_t *edge = nullptr;
        for (uint32_t i = 0; i < graph.edge_count; ++i) {
            if (graph.edges[i].edge_id == m_edgeId) {
                edge = &graph.edges[i];
                break;
            }
        }

        if (!edge) {
            return;
        }

        const auto *src = waywal_shader_graph_find_node_const(&graph, edge->src_node_id);
        const auto *dst = waywal_shader_graph_find_node_const(&graph, edge->dst_node_id);
        if (!src || !dst) {
            return;
        }
        if (edge->src_pin_index >= src->output_pin_count ||
            edge->src_pin_index >= WAYWAL_GRAPH_MAX_PINS_PER_NODE ||
            edge->dst_pin_index >= dst->input_pin_count ||
            edge->dst_pin_index >= WAYWAL_GRAPH_MAX_PINS_PER_NODE) {
            return;
        }

        m_srcNodeId = edge->src_node_id;
        m_dstNodeId = edge->dst_node_id;
        m_srcPinIdx = edge->src_pin_index;
        m_dstPinIdx = edge->dst_pin_index;
        m_srcType = src->outputs[edge->src_pin_index].pin_type;
        m_dstType = dst->inputs[edge->dst_pin_index].pin_type;

        const QPointF srcNodePos(src->canvas_pos[0], src->canvas_pos[1]);
        const QPointF dstNodePos(dst->canvas_pos[0], dst->canvas_pos[1]);
        const QPointF p0 =
            (src->flags & WAYWAL_NODE_FLAG_COLLAPSED) != 0
                ? QPointF(srcNodePos.x() + kNodeWidth, srcNodePos.y() + kHeaderHeight * 0.5 + 2.0)
                : graphPinScenePos(srcNodePos, m_srcPinIdx, true, kNodeWidth, kHeaderHeight,
                                   kPinRowHeight);
        const QPointF p3 = (dst->flags & WAYWAL_NODE_FLAG_COLLAPSED) != 0
                               ? QPointF(dstNodePos.x(), dstNodePos.y() + kHeaderHeight * 0.5 + 2.0)
                               : graphPinScenePos(dstNodePos, m_dstPinIdx, false, kNodeWidth,
                                                  kHeaderHeight, kPinRowHeight);
        commitEndpoints(p0, p3);
    }

    void updateEndpointsFromLiveNodes()
    {
        const auto srcIt = m_editor->m_nodeItems.find(m_srcNodeId);
        const auto dstIt = m_editor->m_nodeItems.find(m_dstNodeId);
        if (srcIt == m_editor->m_nodeItems.end() || dstIt == m_editor->m_nodeItems.end() ||
            !srcIt->second || !dstIt->second) {
            return;
        }
        const auto &graph = m_editor->graph();
        const auto *src = waywal_shader_graph_find_node_const(&graph, m_srcNodeId);
        const auto *dst = waywal_shader_graph_find_node_const(&graph, m_dstNodeId);
        const QPointF srcPos = srcIt->second->pos();
        const QPointF dstPos = dstIt->second->pos();
        const QPointF p0 =
            (src && (src->flags & WAYWAL_NODE_FLAG_COLLAPSED) != 0)
                ? QPointF(srcPos.x() + kNodeWidth, srcPos.y() + kHeaderHeight * 0.5 + 2.0)
                : graphPinScenePos(srcPos, m_srcPinIdx, true, kNodeWidth, kHeaderHeight,
                                   kPinRowHeight);
        const QPointF p3 = (dst && (dst->flags & WAYWAL_NODE_FLAG_COLLAPSED) != 0)
                               ? QPointF(dstPos.x(), dstPos.y() + kHeaderHeight * 0.5 + 2.0)
                               : graphPinScenePos(dstPos, m_dstPinIdx, false, kNodeWidth,
                                                  kHeaderHeight, kPinRowHeight);
        commitEndpoints(p0, p3);
    }

    [[nodiscard]] uint32_t srcNodeId() const noexcept { return m_srcNodeId; }
    [[nodiscard]] uint32_t dstNodeId() const noexcept { return m_dstNodeId; }

    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override
    {
        (void)option;
        (void)widget;

        m_geom.ensureUpdated();
        painter->setRenderHint(QPainter::Antialiasing, true);

        const auto &schedule = m_editor->lastSchedule();
        const bool isCycleEdge =
            ((schedule.has_cycle_error != 0) &&
             (m_srcNodeId == schedule.cycle_node_id || m_dstNodeId == schedule.cycle_node_id)) ||
            (m_editor->m_cycleHighlightSrcNodeId != 0 &&
             m_srcNodeId == m_editor->m_cycleHighlightSrcNodeId &&
             m_dstNodeId == m_editor->m_cycleHighlightDstNodeId);

        if (isCycleEdge) {
            QPen pen(kColorWireCycleErr, 3.0f, Qt::DashLine);
            painter->setPen(pen);
            painter->strokePath(m_geom.cachedPath, pen);
        } else if (m_hovered || isSelected()) {
            QPen pen(kColorWireActive, 3.0f);
            painter->setPen(pen);
            painter->strokePath(m_geom.cachedPath, pen);
        } else if (m_srcType == m_dstType) {
            QPen pen(pinTypeColor(m_srcType), 2.5f);
            painter->setPen(pen);
            painter->strokePath(m_geom.cachedPath, pen);
        } else {
            QLinearGradient grad(m_geom.p0, m_geom.p1);
            grad.setColorAt(0.0, pinTypeColor(m_srcType));
            grad.setColorAt(1.0, pinTypeColor(m_dstType));
            QPen pen(QBrush(grad), 2.5f);
            painter->setPen(pen);
            painter->strokePath(m_geom.cachedPath, pen);
        }

        if (m_srcType != m_dstType &&
            m_editor->currentLod() != GraphSemanticZoomLod::MacroOverview) {
            const QPointF mid = m_geom.cachedMidpoint;
            const QString text =
                QStringLiteral("%1 -> %2").arg(pinTypeName(m_srcType), pinTypeName(m_dstType));

            QFont font = painter->font();
            font.setPointSize(7);
            font.setBold(true);
            painter->setFont(font);

            QFontMetrics fm(font);
            const int textW = fm.horizontalAdvance(text);
            const QRectF pill(mid.x() - textW * 0.5 - 6.0, mid.y() - 8.0, textW + 12.0, 16.0);

            painter->setBrush(QBrush(QColor(0x1E, 0x23, 0x2E)));
            painter->setPen(QPen(QColor(0x38, 0xBD, 0xF8), 1.0f));
            painter->drawRoundedRect(pill, 4.0, 4.0);

            painter->setPen(QColor(0xCD, 0xD6, 0xF4));
            painter->drawText(pill, Qt::AlignCenter, text);
        }
    }

  protected:
    void hoverEnterEvent(QGraphicsSceneHoverEvent *event) override
    {
        m_hovered = true;
        update();
        QGraphicsPathItem::hoverEnterEvent(event);
    }

    void hoverLeaveEvent(QGraphicsSceneHoverEvent *event) override
    {
        m_hovered = false;
        update();
        QGraphicsPathItem::hoverLeaveEvent(event);
    }

    void mousePressEvent(QGraphicsSceneMouseEvent *event) override
    {
        if (event->button() == Qt::RightButton) {
            const uint32_t edgeId = m_edgeId;
            QPointer<NodeGraphEditorWidget> safeEditor(m_editor);
            event->accept();
            if (m_editor) {
                // Defer scene rebuild until Qt finishes dispatching mousePressEvent on `this`.
                QMetaObject::invokeMethod(
                    m_editor,
                    [safeEditor, edgeId]() {
                        if (safeEditor) {
                            safeEditor->disconnectWire(edgeId);
                        }
                    },
                    Qt::QueuedConnection);
            }
            return;
        }
        QGraphicsPathItem::mousePressEvent(event);
    }

  private:
    void commitEndpoints(const QPointF &p0, const QPointF &p3)
    {
        m_geom.setEndpoints(p0, p3);
        if (m_geom.dirty) {
            prepareGeometryChange();
            m_geom.ensureUpdated();
            setPath(m_geom.cachedPath);
        }
    }

    uint32_t m_edgeId{0};
    NodeGraphEditorWidget *m_editor{nullptr};
    CachedWireGeometry m_geom;
    uint32_t m_srcNodeId{0};
    uint32_t m_dstNodeId{0};
    uint32_t m_srcPinIdx{0};
    uint32_t m_dstPinIdx{0};
    uint32_t m_srcType{0};
    uint32_t m_dstType{0};
    bool m_hovered{false};
};

/* ========================================================================= */
/* NodeGraphEditorWidget Implementation                                      */
/* ========================================================================= */

NodeGraphEditorWidget::NodeGraphEditorWidget(QWidget *parent)
    : QWidget(parent), m_graph(waywal_shader_graph_create(), &waywal_shader_graph_destroy)
{
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    setupUi();
    newEmptyGraph();
}

NodeGraphEditorWidget::~NodeGraphEditorWidget() = default;

void NodeGraphEditorWidget::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Top Control / Status Bar
    auto *toolBar = new QToolBar(this);
    toolBar->setStyleSheet(QStringLiteral(
        "QToolBar { background: #12151C; border-bottom: 1px solid #1E232E; padding: 4px; }"
        "QLineEdit { background: #181B22; color: #CDD6F4; border: 1px solid #313244; "
        "border-radius: 4px; padding: 4px 8px; }"
        "QPushButton { background: #1E232E; color: #CDD6F4; border: 1px solid #313244; "
        "border-radius: 4px; padding: 4px 12px; }"
        "QPushButton:hover { background: #313244; }"
        "QLabel { color: #A6ADC8; font-family: monospace; font-size: 11px; padding: 0 8px; }"));

    m_searchBox = new QLineEdit(toolBar);
    m_searchBox->setPlaceholderText(QStringLiteral("Filter nodes (Shift+A or Tab to spawn)..."));
    m_searchBox->setFixedWidth(260);
    connect(m_searchBox, &QLineEdit::textChanged, this, &NodeGraphEditorWidget::setSearchFilter);
    toolBar->addWidget(m_searchBox);

    auto *zoomInBtn = new QPushButton(QStringLiteral("+"), toolBar);
    connect(zoomInBtn, &QPushButton::clicked, this, [this] { setZoomScale(m_zoomScale * 1.2f); });
    toolBar->addWidget(zoomInBtn);

    auto *zoomOutBtn = new QPushButton(QStringLiteral("-"), toolBar);
    connect(zoomOutBtn, &QPushButton::clicked, this, [this] { setZoomScale(m_zoomScale / 1.2f); });
    toolBar->addWidget(zoomOutBtn);

    auto *zoomResetBtn = new QPushButton(QStringLiteral("1:1"), toolBar);
    connect(zoomResetBtn, &QPushButton::clicked, this, [this] { setZoomScale(1.0f); });
    toolBar->addWidget(zoomResetBtn);

    m_costBadgeLabel = new QLabel(QStringLiteral("ALU: 0 | TEX: 0 | Passes: 1"), toolBar);
    toolBar->addWidget(m_costBadgeLabel);

    auto *spacer = new QWidget(toolBar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolBar->addWidget(spacer);

    m_compileBtn = new QPushButton(QStringLiteral("Compile & Link"), toolBar);
    m_compileBtn->setStyleSheet(
        QStringLiteral("QPushButton { background: #22D3EE; color: #0E1117; font-weight: bold; }"));
    connect(m_compileBtn, &QPushButton::clicked, this, &NodeGraphEditorWidget::compileToGlslSource);
    toolBar->addWidget(m_compileBtn);

    mainLayout->addWidget(toolBar);

    // Canvas Scene & View
    m_scene = new QGraphicsScene(this);
    m_scene->setSceneRect(-50000.0, -50000.0, 100000.0, 100000.0);

    m_view = new GraphCanvasView(this, this);
    m_view->setScene(m_scene);
    mainLayout->addWidget(m_view);
}

void NodeGraphEditorWidget::newEmptyGraph(const QString &name)
{
    waywal_shader_graph_init(m_graph.get(), name.toUtf8().constData());
    m_nodeThumbnails.clear();
    m_cycleHighlightSrcNodeId = 0;
    m_cycleHighlightDstNodeId = 0;
    m_nextEdgeId = 1;

    const int32_t uvRes =
        waywal_shader_graph_add_node(m_graph.get(), WAYWAL_NODE_IN_UV, "", -250.0f, 0.0f);
    const int32_t outRes =
        waywal_shader_graph_add_node(m_graph.get(), WAYWAL_NODE_OUT_IMAGE_PASS, "", 250.0f, 0.0f);
    if (uvRes > 0) {
        if (auto *n = waywal_shader_graph_find_node(m_graph.get(), static_cast<uint32_t>(uvRes))) {
            std::strncpy(n->prop_or_asset_key, "UV Coordinates", sizeof(n->prop_or_asset_key) - 1);
        }
    }
    if (outRes > 0) {
        if (auto *n = waywal_shader_graph_find_node(m_graph.get(), static_cast<uint32_t>(outRes))) {
            std::strncpy(n->prop_or_asset_key, "Output Surface", sizeof(n->prop_or_asset_key) - 1);
        }
    }
    if (uvRes > 0 && outRes > 0) {
        const int32_t eid = waywal_shader_graph_connect(m_graph.get(), static_cast<uint32_t>(uvRes),
                                                        0, static_cast<uint32_t>(outRes), 0);
        if (eid > 0) {
            m_nextEdgeId = std::max(m_nextEdgeId, static_cast<uint32_t>(eid) + 1);
        }
    }

    resyncPinConnectedFlags(m_graph.get());
    rebuildSceneFromModel();
    validateAndSchedule();
    compileToGlslSource();
    emit graphModified();
}

bool NodeGraphEditorWidget::loadGraphFromFile(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit statusMessage(QStringLiteral("Failed to open graph file: %1").arg(filePath));
        return false;
    }

    QByteArray data = file.readAll();
    char errBuf[256] = {0};
    auto tempGraph = std::unique_ptr<waywal_shader_graph_t, void (*)(waywal_shader_graph_t *)>(
        waywal_shader_graph_create(), &waywal_shader_graph_destroy);
    if (!tempGraph) {
        emit statusMessage(QStringLiteral("Out of memory allocating shader graph"));
        return false;
    }

    if (!waywal_shader_graph_load_json(data.constData(), static_cast<size_t>(data.size()),
                                       tempGraph.get(), errBuf)) {
        emit statusMessage(
            QStringLiteral("JSON graph parse error: %1").arg(QString::fromUtf8(errBuf)));
        return false;
    }

    m_graph = std::move(tempGraph);
    m_nodeThumbnails.clear();
    m_cycleHighlightSrcNodeId = 0;
    m_cycleHighlightDstNodeId = 0;
    resyncPinConnectedFlags(m_graph.get());

    m_nextEdgeId = 1;
    for (uint32_t i = 0; i < m_graph->edge_count; ++i) {
        if (m_graph->edges[i].edge_id >= m_nextEdgeId) {
            m_nextEdgeId = m_graph->edges[i].edge_id + 1;
        }
    }

    rebuildSceneFromModel();
    validateAndSchedule();
    compileToGlslSource();
    emit graphModified();
    return true;
}

bool NodeGraphEditorWidget::saveGraphToFile(const QString &filePath) const
{
    char *json = waywal_shader_graph_save_json(m_graph.get());
    if (!json) {
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::free(json);
        return false;
    }

    file.write(json);
    file.close();
    std::free(json);
    return true;
}

uint32_t NodeGraphEditorWidget::addNodeInternal(waywal_node_op_t op, const QPointF &scenePos,
                                                const QString &propOrEffectSlug,
                                                const QString &titleOverride,
                                                const float *defaultConstVal)
{
    const float sx = std::isfinite(scenePos.x()) ? static_cast<float>(scenePos.x()) : 0.0f;
    const float sy = std::isfinite(scenePos.y()) ? static_cast<float>(scenePos.y()) : 0.0f;
    const uint32_t prevNodeCount = m_graph->node_count;
    const int32_t res =
        waywal_shader_graph_add_node(m_graph.get(), static_cast<uint32_t>(op), "", sx, sy);
    if (res < 0) {
        return 0;
    }

    const uint32_t nid = static_cast<uint32_t>(res);
    waywal_graph_node_t *added = waywal_shader_graph_find_node(m_graph.get(), nid);
    if (!added) {
        return 0;
    }

    waywal_graph_node_t snapshot = *added;
    // Revert the provisional C insertion so CommandBus::execute owns the sole mutation.
    m_graph->node_count = prevNodeCount;

    if (!propOrEffectSlug.isEmpty()) {
        const QByteArray slugUtf8 = propOrEffectSlug.toUtf8();
        std::memset(snapshot.prop_or_asset_key, 0, sizeof(snapshot.prop_or_asset_key));
        std::strncpy(snapshot.prop_or_asset_key, slugUtf8.constData(),
                     sizeof(snapshot.prop_or_asset_key) - 1);
    }
    if (!titleOverride.isEmpty()) {
        const QByteArray titleUtf8 = titleOverride.toUtf8();
        std::memset(snapshot.title, 0, sizeof(snapshot.title));
        std::strncpy(snapshot.title, titleUtf8.constData(), sizeof(snapshot.title) - 1);
    }
    if (defaultConstVal) {
        for (int c = 0; c < 4; ++c) {
            snapshot.const_val[c] = std::isfinite(defaultConstVal[c]) ? defaultConstVal[c] : 0.0f;
        }
    }

    QPointer<NodeGraphEditorWidget> self(this);
    auto cmd = std::make_unique<AddNodeCommand>(snapshot,
                                                [self](const waywal_graph_node_t &n, bool isAdd) {
                                                    if (self) {
                                                        self->applyNodeMutation(n, isAdd);
                                                    }
                                                });
    if (m_commandBus) {
        m_commandBus->execute(std::move(cmd));
    } else {
        cmd->execute();
    }
    return nid;
}

uint32_t NodeGraphEditorWidget::addNode(waywal_node_op_t op, const QPointF &scenePos,
                                        const QString &propOrEffectSlug)
{
    return addNodeInternal(op, scenePos, propOrEffectSlug, QString(), nullptr);
}

uint32_t NodeGraphEditorWidget::addBuiltinEffectNode(we_builtin_effect_id_t effectId,
                                                     const QPointF &scenePos)
{
    const we_builtin_effect_desc_t *desc = we_builtin_effect_get_by_id(effectId);
    if (!desc) {
        desc = we_builtin_effect_find_by_id(effectId);
    }

    const QString title =
        desc ? QString::fromUtf8(desc->display_name) : QStringLiteral("Built-In Effect");
    const QString slug = desc ? QString::fromUtf8(desc->slug) : QString();
    return addNodeInternal(WAYWAL_NODE_BUILTIN_EFFECT, scenePos, slug, title,
                           desc ? desc->default_params : nullptr);
}

void NodeGraphEditorWidget::removeSelectedNodes()
{
    std::vector<uint32_t> toRemove;
    for (const auto &[nid, nodeItem] : m_nodeItems) {
        if (nodeItem && nodeItem->isSelected()) {
            toRemove.push_back(nid);
        }
    }
    std::sort(toRemove.begin(), toRemove.end());
    if (toRemove.empty()) {
        return;
    }

    std::vector<waywal_graph_node_t> removedNodes;
    std::unordered_set<uint32_t> removedSet;
    removedNodes.reserve(toRemove.size());
    for (uint32_t nid : toRemove) {
        if (const waywal_graph_node_t *node =
                waywal_shader_graph_find_node_const(m_graph.get(), nid)) {
            removedNodes.push_back(*node);
            removedSet.insert(nid);
        }
    }
    if (removedNodes.empty()) {
        return;
    }

    std::vector<waywal_graph_edge_t> detachedEdges;
    for (uint32_t i = 0; i < m_graph->edge_count; ++i) {
        const auto &e = m_graph->edges[i];
        if (removedSet.count(e.src_node_id) || removedSet.count(e.dst_node_id)) {
            detachedEdges.push_back(e);
        }
    }

    class RemoveNodesCompositeCommand final : public ICommand {
      public:
        RemoveNodesCompositeCommand(QPointer<NodeGraphEditorWidget> widget,
                                    std::vector<waywal_graph_node_t> nodes,
                                    std::vector<waywal_graph_edge_t> edges)
            : m_widget(std::move(widget)), m_nodes(std::move(nodes)), m_edges(std::move(edges))
        {
        }

        void execute() override
        {
            if (!m_widget) {
                return;
            }
            for (const auto &n : m_nodes) {
                waywal_shader_graph_remove_node(m_widget->m_graph.get(), n.node_id);
                m_widget->m_nodeThumbnails.remove(n.node_id);
            }
            finishMutation();
        }

        void undo() override
        {
            if (!m_widget) {
                return;
            }
            for (const auto &n : m_nodes) {
                if (m_widget->m_graph->node_count < WAYWAL_GRAPH_MAX_NODES &&
                    !waywal_shader_graph_find_node_const(m_widget->m_graph.get(), n.node_id)) {
                    m_widget->m_graph->nodes[m_widget->m_graph->node_count++] = n;
                }
            }
            for (const auto &e : m_edges) {
                bool exists = false;
                for (uint32_t i = 0; i < m_widget->m_graph->edge_count; ++i) {
                    if (m_widget->m_graph->edges[i].edge_id == e.edge_id) {
                        exists = true;
                        break;
                    }
                }
                if (!exists && m_widget->m_graph->edge_count < WAYWAL_GRAPH_MAX_EDGES) {
                    m_widget->m_graph->edges[m_widget->m_graph->edge_count++] = e;
                }
            }
            finishMutation();
        }

        [[nodiscard]] QString describe() const override
        {
            return QStringLiteral("Remove %1 node(s)").arg(m_nodes.size());
        }

      private:
        void finishMutation()
        {
            resyncPinConnectedFlags(m_widget->m_graph.get());
            m_widget->rebuildSceneFromModel();
            m_widget->validateAndSchedule();
            if (m_widget->m_graph->flags & WAYWAL_GRAPH_FLAG_AUTO_COMPILE) {
                m_widget->compileToGlslSource();
            }
            emit m_widget->graphModified();
        }

        QPointer<NodeGraphEditorWidget> m_widget;
        std::vector<waywal_graph_node_t> m_nodes;
        std::vector<waywal_graph_edge_t> m_edges;
    };

    auto cmd = std::make_unique<RemoveNodesCompositeCommand>(
        QPointer<NodeGraphEditorWidget>(this), std::move(removedNodes), std::move(detachedEdges));
    if (m_commandBus) {
        m_commandBus->execute(std::move(cmd));
    } else {
        cmd->execute();
    }
}

bool NodeGraphEditorWidget::connectPins(uint32_t srcNodeId, uint32_t srcPinIdx, uint32_t dstNodeId,
                                        uint32_t dstPinIdx)
{
    if (srcNodeId == dstNodeId) {
        emit statusMessage(QStringLiteral("Connection rejected: self-loop not allowed"));
        return false;
    }

    const waywal_graph_node_t *src = waywal_shader_graph_find_node_const(m_graph.get(), srcNodeId);
    const waywal_graph_node_t *dst = waywal_shader_graph_find_node_const(m_graph.get(), dstNodeId);
    if (!src || !dst) {
        return false;
    }

    if (srcPinIdx >= src->output_pin_count || srcPinIdx >= WAYWAL_GRAPH_MAX_PINS_PER_NODE ||
        dstPinIdx >= dst->input_pin_count || dstPinIdx >= WAYWAL_GRAPH_MAX_PINS_PER_NODE) {
        emit statusMessage(QStringLiteral("Connection rejected: pin index out of bounds"));
        return false;
    }

    const auto &srcPin = src->outputs[srcPinIdx];
    const auto &dstPin = dst->inputs[dstPinIdx];
    const bool nonCoercible = ((srcPin.flags | dstPin.flags) & WAYWAL_PIN_FLAG_NON_COERCIBLE) != 0;
    const bool compat = nonCoercible
                            ? (srcPin.pin_type == dstPin.pin_type)
                            : waywal_pin_types_compatible(srcPin.pin_type, dstPin.pin_type);
    if (!compat) {
        emit statusMessage(QStringLiteral("Connection rejected: incompatible pin types"));
        return false;
    }

    if (waywal_shader_graph_would_create_cycle(m_graph.get(), srcNodeId, dstNodeId)) {
        m_cycleHighlightSrcNodeId = srcNodeId;
        m_cycleHighlightDstNodeId = dstNodeId;
        if (m_scene) {
            m_scene->update();
        }
        QPointer<NodeGraphEditorWidget> self(this);
        QTimer::singleShot(1200, this, [self]() {
            if (self) {
                self->m_cycleHighlightSrcNodeId = 0;
                self->m_cycleHighlightDstNodeId = 0;
                if (self->m_scene) {
                    self->m_scene->update();
                }
            }
        });
        emit statusMessage(QStringLiteral("Connection rejected: cycle detected"));
        return false;
    }

    std::optional<waywal_graph_edge_t> replacedEdge;
    for (uint32_t i = 0; i < m_graph->edge_count; ++i) {
        const auto &existing = m_graph->edges[i];
        if (existing.dst_node_id == dstNodeId && existing.dst_pin_index == dstPinIdx) {
            replacedEdge = existing;
            break;
        }
    }

    if (!replacedEdge.has_value() && m_graph->edge_count >= WAYWAL_GRAPH_MAX_EDGES) {
        emit statusMessage(QStringLiteral("Connection rejected: maximum edge count reached"));
        return false;
    }

    uint32_t eid = std::max(1u, m_nextEdgeId);
    auto edgeIdExists = [this](uint32_t candidate) {
        for (uint32_t i = 0; i < m_graph->edge_count; ++i) {
            if (m_graph->edges[i].edge_id == candidate) {
                return true;
            }
        }
        return false;
    };
    while (edgeIdExists(eid)) {
        ++eid;
    }
    m_nextEdgeId = eid + 1;

    waywal_graph_edge_t edge{};
    edge.edge_id = eid;
    edge.src_node_id = srcNodeId;
    edge.src_pin_index = srcPinIdx;
    edge.dst_node_id = dstNodeId;
    edge.dst_pin_index = dstPinIdx;

    QPointer<NodeGraphEditorWidget> self(this);
    auto cmd = std::make_unique<ConnectWireCommand>(
        edge,
        [self](const waywal_graph_edge_t &e, bool isConnect) {
            if (self) {
                self->applyWireMutation(e, isConnect);
            }
        },
        replacedEdge);

    if (m_commandBus) {
        m_commandBus->execute(std::move(cmd));
    } else {
        cmd->execute();
    }
    return true;
}

void NodeGraphEditorWidget::disconnectWire(uint32_t edgeId)
{
    for (uint32_t i = 0; i < m_graph->edge_count; ++i) {
        if (m_graph->edges[i].edge_id == edgeId) {
            const waywal_graph_edge_t edge = m_graph->edges[i];
            QPointer<NodeGraphEditorWidget> self(this);
            auto cmd = std::make_unique<DisconnectWireCommand>(
                edge, [self](const waywal_graph_edge_t &e, bool isConnect) {
                    if (self) {
                        self->applyWireMutation(e, isConnect);
                    }
                });
            if (m_commandBus) {
                m_commandBus->execute(std::move(cmd));
            } else {
                cmd->execute();
            }
            break;
        }
    }
}

void NodeGraphEditorWidget::setNodeConstant(uint32_t nodeId, const std::array<float, 4> &val)
{
    waywal_graph_node_t *node = waywal_shader_graph_find_node(m_graph.get(), nodeId);
    if (!node) {
        return;
    }

    const std::array<float, 4> oldVal = {node->const_val[0], node->const_val[1], node->const_val[2],
                                         node->const_val[3]};

    std::array<float, 4> safeVal{};
    for (size_t i = 0; i < 4; ++i) {
        safeVal[i] = std::isfinite(val[i]) ? val[i] : oldVal[i];
    }

    QPointer<NodeGraphEditorWidget> self(this);
    auto cmd = std::make_unique<SetNodeConstantCommand>(
        nodeId, QString::fromUtf8(node->title), oldVal, safeVal,
        [self](uint32_t nid, const std::array<float, 4> &v) {
            if (!self) {
                return;
            }
            if (auto *n = waywal_shader_graph_find_node(self->m_graph.get(), nid)) {
                n->const_val[0] = v[0];
                n->const_val[1] = v[1];
                n->const_val[2] = v[2];
                n->const_val[3] = v[3];
                emit self->nodeConstantScrubbed(nid, 0, v[0]);
                self->validateAndSchedule();
                if (self->m_graph->flags & WAYWAL_GRAPH_FLAG_AUTO_COMPILE) {
                    self->compileToGlslSource();
                }
                emit self->graphModified();
                self->m_scene->update();
            }
        });

    if (m_commandBus) {
        m_commandBus->execute(std::move(cmd));
    } else {
        cmd->execute();
    }
}

void NodeGraphEditorWidget::moveNodes(const std::vector<MoveNodesCommand::NodeMoveDelta> &deltas)
{
    if (deltas.empty()) {
        return;
    }
    QPointer<NodeGraphEditorWidget> self(this);
    auto cmd = std::make_unique<MoveNodesCommand>(
        deltas, [self](const std::vector<MoveNodesCommand::NodeMoveDelta> &d, bool useNewPos) {
            if (self) {
                self->applyMoveDeltasInPlace(d, useNewPos);
            }
        });
    if (m_commandBus) {
        m_commandBus->execute(std::move(cmd));
    } else {
        cmd->execute();
    }
}

void NodeGraphEditorWidget::applyMoveDeltasInPlace(
    const std::vector<MoveNodesCommand::NodeMoveDelta> &deltas, bool useNewPos)
{
    const QScopedValueRollback<bool> guard(m_applyingMoveInPlace, true);
    for (const auto &delta : deltas) {
        const QPointF p = useNewPos ? delta.newPos : delta.oldPos;
        if (auto *n = waywal_shader_graph_find_node(m_graph.get(), delta.nodeId)) {
            n->canvas_pos[0] = static_cast<float>(p.x());
            n->canvas_pos[1] = static_cast<float>(p.y());
        }
        const auto it = m_nodeItems.find(delta.nodeId);
        if (it != m_nodeItems.end() && it->second && it->second->pos() != p) {
            it->second->setPos(p);
        }
    }

    for (WireItem *wire : m_wireItems) {
        if (wire) {
            wire->updateEndpointsFromLiveNodes();
        }
    }
    rebuildPinSpatialGrid();
    m_scene->update();
    emit graphModified();
}

bool NodeGraphEditorWidget::validateAndSchedule()
{
    std::memset(&m_schedule, 0, sizeof(m_schedule));
    bool ok = waywal_shader_graph_analyze(m_graph.get(), &m_schedule);

    if (m_costBadgeLabel) {
        if (!ok || m_schedule.has_cycle_error) {
            m_costBadgeLabel->setText(
                QStringLiteral("Tarjan SCC Cycle Error at Node #%1").arg(m_schedule.cycle_node_id));
            m_costBadgeLabel->setStyleSheet(QStringLiteral("color: #F87171; font-weight: bold;"));
        } else {
            uint32_t passCount = 0;
            for (int p = 0; p < 5; ++p) {
                if (m_schedule.active_pass_mask & (1u << p)) {
                    passCount++;
                }
            }
            m_costBadgeLabel->setText(QStringLiteral("ALU: %1 | TEX: %2 | Passes: %3")
                                          .arg(m_schedule.estimated_alu_ops)
                                          .arg(m_schedule.estimated_tex_fetches)
                                          .arg(passCount));
            m_costBadgeLabel->setStyleSheet(QStringLiteral("color: #A6ADC8;"));
        }
    }

    m_scene->update();
    return ok && (m_schedule.has_cycle_error == 0);
}

QString NodeGraphEditorWidget::compileToGlslSource()
{
    const QScopedValueRollback<bool> compileGuard(m_inCompileToGlsl, true);

    if (!validateAndSchedule()) {
        emit graphCompiled(false, QString(), QString::fromUtf8(m_schedule.error_msg), 0, 0);
        return QString();
    }

    size_t glslLen = 0;
    char *glsl = waywal_shader_graph_compile_glsl(m_graph.get(), &m_schedule, &glslLen);
    if (!glsl) {
        emit graphCompiled(false, QString(), QStringLiteral("Compilation failed"), 0, 0);
        return QString();
    }

    const QString code = QString::fromUtf8(glsl, static_cast<int>(glslLen));
    std::free(glsl);

    emit graphCompiled(true, code, QStringLiteral("OK"), m_schedule.estimated_alu_ops,
                       m_schedule.estimated_tex_fetches);
    emit graphCompiled(code, m_schedule.active_pass_mask);
    emit statusMessage(
        QStringLiteral("Shader graph compiled successfully (%1 bytes)").arg(code.size()));
    return code;
}

bool NodeGraphEditorWidget::dispatchGraphJsonHotSwap()
{
    if (!m_ipcClient) {
        return true;
    }
    char *raw = waywal_shader_graph_save_json(m_graph.get());
    if (!raw) {
        return false;
    }
    const QString jsonStr = QString::fromUtf8(raw);
    std::free(raw);

    const QString outName = m_targetOutput.isEmpty() ? QStringLiteral("*") : m_targetOutput;
    return m_ipcClient->requestCompileShaderGraph(outName, QString(), jsonStr,
                                                  WAYWAL_COMPILE_FLAG_HOT_SWAP);
}

bool NodeGraphEditorWidget::compileAndHotSwapDaemon()
{
    // If invoked from a slot connected to graphCompiled(), GLSL was just validated and compiled on
    // this stack frame; serialize and send the .wwgraph JSON without re-compiling GLSL.
    if (m_inCompileToGlsl) {
        m_hotSwapDispatchedInCurrentCompile = true;
        return dispatchGraphJsonHotSwap();
    }

    m_hotSwapDispatchedInCurrentCompile = false;
    const QString code = compileToGlslSource();
    if (code.isEmpty()) {
        return false;
    }
    if (!m_hotSwapDispatchedInCurrentCompile) {
        return dispatchGraphJsonHotSwap();
    }
    return true;
}

void NodeGraphEditorWidget::setZoomScale(float scale)
{
    if (!std::isfinite(scale)) {
        return;
    }
    const GraphSemanticZoomLod prevLod = currentLod();
    m_zoomScale = std::clamp(scale, 0.15f, 3.00f);

    QTransform t;
    t.scale(m_zoomScale, m_zoomScale);
    m_view->setTransform(t);

    if (currentLod() != prevLod) {
        const QScopedValueRollback<bool> guard(m_rebuildingScene, true);
        for (const auto &[nid, nodeItem] : m_nodeItems) {
            (void)nid;
            if (nodeItem) {
                nodeItem->updateFromModel();
            }
        }
    }
    m_scene->update();
}

void NodeGraphEditorWidget::setSearchFilter(const QString &query)
{
    m_activeSearchFilter = query;
    applySearchFilter(query);
}

GraphSemanticZoomLod NodeGraphEditorWidget::currentLod() const noexcept
{
    if (m_zoomScale < kLodMacroThreshold) {
        return GraphSemanticZoomLod::MacroOverview;
    }
    if (m_zoomScale < kLodCompactThreshold) {
        return GraphSemanticZoomLod::CompactPins;
    }
    return GraphSemanticZoomLod::FullDetail;
}

void NodeGraphEditorWidget::openQuickConnectPopup(const QPointF &scenePos, int32_t fromNodeId,
                                                  int32_t fromPinIdx, bool fromOutput)
{
    auto *popup = new QDialog(this, Qt::Popup | Qt::FramelessWindowHint);
    popup->setAttribute(Qt::WA_DeleteOnClose, true);
    popup->setStyleSheet(QStringLiteral(
        "QDialog { background: #181B22; border: 1px solid #38BDF8; border-radius: 6px; padding: "
        "4px; }"
        "QLineEdit { background: #12151C; color: #CDD6F4; border: 1px solid #313244; "
        "border-radius: 4px; padding: 6px; font-size: 12px; }"
        "QListWidget { background: #12151C; color: #CDD6F4; border: 1px solid #313244; "
        "border-radius: 4px; font-size: 11px; }"
        "QListWidget::item { padding: 4px; }"
        "QListWidget::item:selected { background: #38BDF8; color: #0E1117; font-weight: bold; }"));

    auto *layout = new QVBoxLayout(popup);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    auto *searchEdit = new QLineEdit(popup);
    searchEdit->setPlaceholderText(QStringLiteral("Search node or effect..."));
    layout->addWidget(searchEdit);

    auto *listWidget = new QListWidget(popup);
    layout->addWidget(listWidget);

    struct NodeEntry {
        QString title;
        bool isBuiltin;
        uint32_t id;
    };
    auto entries = std::make_shared<std::vector<NodeEntry>>();

    *entries = {
        {QStringLiteral("Input: UV Coordinates"), false, WAYWAL_NODE_IN_UV},
        {QStringLiteral("Input: Time Uniform"), false, WAYWAL_NODE_IN_TIME},
        {QStringLiteral("Input: Resolution Uniform"), false, WAYWAL_NODE_IN_RESOLUTION},
        {QStringLiteral("Input: Pointer Coordinates"), false, WAYWAL_NODE_IN_POINTER},
        {QStringLiteral("Input: Audio FFT Spectrum"), false, WAYWAL_NODE_IN_AUDIO_FFT},
        {QStringLiteral("Input: Texture2D Asset"), false, WAYWAL_NODE_IN_TEXTURE2D},
        {QStringLiteral("Input: Previous Frame Buffer"), false, WAYWAL_NODE_IN_PASS_BUFFER},
        {QStringLiteral("Input: Depth Map"), false, WAYWAL_NODE_IN_DEPTH_MAP},
        {QStringLiteral("Input: User Property"), false, WAYWAL_NODE_IN_USER_PROP},
        {QStringLiteral("Input: Constant Literal"), false, WAYWAL_NODE_IN_CONSTANT},
        {QStringLiteral("Math: Add"), false, WAYWAL_NODE_MATH_ADD},
        {QStringLiteral("Math: Subtract"), false, WAYWAL_NODE_MATH_SUB},
        {QStringLiteral("Math: Multiply"), false, WAYWAL_NODE_MATH_MUL},
        {QStringLiteral("Math: Divide"), false, WAYWAL_NODE_MATH_DIV},
        {QStringLiteral("Math: FMA"), false, WAYWAL_NODE_MATH_FMA},
        {QStringLiteral("Math: Mix / Lerp"), false, WAYWAL_NODE_MATH_MIX},
        {QStringLiteral("Math: Smoothstep"), false, WAYWAL_NODE_MATH_SMOOTHSTEP},
        {QStringLiteral("Math: Clamp"), false, WAYWAL_NODE_MATH_CLAMP},
        {QStringLiteral("Math: Sin"), false, WAYWAL_NODE_MATH_SIN},
        {QStringLiteral("Math: Cos"), false, WAYWAL_NODE_MATH_COS},
        {QStringLiteral("Math: Pow"), false, WAYWAL_NODE_MATH_POW},
        {QStringLiteral("Math: Dot"), false, WAYWAL_NODE_MATH_DOT},
        {QStringLiteral("Math: Cross"), false, WAYWAL_NODE_MATH_CROSS},
        {QStringLiteral("Math: Normalize"), false, WAYWAL_NODE_MATH_NORMALIZE},
        {QStringLiteral("Math: Length"), false, WAYWAL_NODE_MATH_LENGTH},
        {QStringLiteral("Math: Distance"), false, WAYWAL_NODE_MATH_DISTANCE},
        {QStringLiteral("Math: Fract"), false, WAYWAL_NODE_MATH_FRACT},
        {QStringLiteral("Math: Floor"), false, WAYWAL_NODE_MATH_FLOOR},
        {QStringLiteral("Math: Abs"), false, WAYWAL_NODE_MATH_ABS},
        {QStringLiteral("Vector: Swizzle / Combine"), false, WAYWAL_NODE_VEC_SWIZZLE_COMBINE},
        {QStringLiteral("Noise: 2D Simplex"), false, WAYWAL_NODE_NOISE_SIMPLEX2D},
        {QStringLiteral("Noise: 2D Curl"), false, WAYWAL_NODE_NOISE_CURL2D},
        {QStringLiteral("Noise: 2D Voronoi"), false, WAYWAL_NODE_NOISE_VORONOI2D},
        {QStringLiteral("Noise: 2D FBM"), false, WAYWAL_NODE_NOISE_FBM2D},
        {QStringLiteral("SDF: Circle"), false, WAYWAL_NODE_SDF_CIRCLE},
        {QStringLiteral("SDF: Box"), false, WAYWAL_NODE_SDF_BOX},
        {QStringLiteral("SDF: Ring"), false, WAYWAL_NODE_SDF_RING},
        {QStringLiteral("UV: Polar Coordinates"), false, WAYWAL_NODE_UV_POLAR},
        {QStringLiteral("UV: Twirl"), false, WAYWAL_NODE_UV_TWIRL},
        {QStringLiteral("UV: Tile"), false, WAYWAL_NODE_UV_TILE},
        {QStringLiteral("Color: OKLCH to sRGB"), false, WAYWAL_NODE_COLOR_OKLCH_TO_SRGB},
        {QStringLiteral("Color: sRGB to OKLCH"), false, WAYWAL_NODE_COLOR_SRGB_TO_OKLCH},
        {QStringLiteral("Color: Hue Shift"), false, WAYWAL_NODE_COLOR_HUE_SHIFT},
        {QStringLiteral("Color: Contrast & Saturation"), false, WAYWAL_NODE_COLOR_CONTRAST_SAT},
        {QStringLiteral("Filter: Dual Kawase Blur"), false, WAYWAL_NODE_FILTER_DUAL_KAWASE},
        {QStringLiteral("Filter: Chromatic Aberration"), false, WAYWAL_NODE_FILTER_CHROMATIC_ABERR},
        {QStringLiteral("Light: Normal From Height"), false, WAYWAL_NODE_LIGHT_NORMAL_FROM_HEIGHT},
        {QStringLiteral("Light: Blinn-Phong"), false, WAYWAL_NODE_LIGHT_BLINN_PHONG},
        {QStringLiteral("Output: Buffer Pass"), false, WAYWAL_NODE_OUT_BUFFER_PASS},
        {QStringLiteral("Output: Image Pass"), false, WAYWAL_NODE_OUT_IMAGE_PASS},
    };

    size_t effectCount = 0;
    const we_builtin_effect_desc_t *effects = we_builtin_effects_get_table(&effectCount);
    for (size_t i = 0; i < effectCount; ++i) {
        entries->push_back(
            {QStringLiteral("Effect: %1").arg(QString::fromUtf8(effects[i].display_name)), true,
             effects[i].id});
    }

    auto populateList = [listWidget, entries](const QString &filter) {
        listWidget->clear();
        for (const auto &entry : *entries) {
            if (filter.isEmpty() || entry.title.contains(filter, Qt::CaseInsensitive)) {
                auto *item = new QListWidgetItem(entry.title, listWidget);
                item->setData(Qt::UserRole, entry.isBuiltin);
                item->setData(Qt::UserRole + 1, entry.id);
            }
        }
        if (listWidget->count() > 0) {
            listWidget->setCurrentRow(0);
        }
    };

    populateList(QString());

    connect(searchEdit, &QLineEdit::textChanged, popup, populateList);

    QPointer<NodeGraphEditorWidget> self(this);
    QPointer<QDialog> popupPtr(popup);
    auto selectCurrent = [self, popupPtr, listWidget, scenePos, fromNodeId, fromPinIdx,
                          fromOutput]() {
        if (!self || !popupPtr) {
            return;
        }
        QListWidgetItem *item = listWidget->currentItem();
        if (!item) {
            return;
        }

        const bool isBuiltin = item->data(Qt::UserRole).toBool();
        const uint32_t id = item->data(Qt::UserRole + 1).toUInt();

        uint32_t newId = 0;
        if (isBuiltin) {
            newId = self->addBuiltinEffectNode(static_cast<we_builtin_effect_id_t>(id), scenePos);
        } else {
            newId = self->addNode(static_cast<waywal_node_op_t>(id), scenePos);
        }

        if (newId > 0 && fromNodeId >= 0 && fromPinIdx >= 0) {
            const auto &g = self->graph();
            const auto *fromN =
                waywal_shader_graph_find_node_const(&g, static_cast<uint32_t>(fromNodeId));
            const auto *toN = waywal_shader_graph_find_node_const(&g, newId);
            if (fromN && toN) {
                const uint32_t srcIdx = static_cast<uint32_t>(fromPinIdx);
                if (fromOutput && srcIdx < fromN->output_pin_count &&
                    srcIdx < WAYWAL_GRAPH_MAX_PINS_PER_NODE) {
                    const uint32_t srcType = fromN->outputs[srcIdx].pin_type;
                    const uint32_t srcFlags = fromN->outputs[srcIdx].flags;
                    const uint32_t dstCount =
                        std::min<uint32_t>(toN->input_pin_count, WAYWAL_GRAPH_MAX_PINS_PER_NODE);
                    for (uint32_t p = 0; p < dstCount; ++p) {
                        const bool nc = ((srcFlags | toN->inputs[p].flags) &
                                         WAYWAL_PIN_FLAG_NON_COERCIBLE) != 0;
                        const bool compat =
                            nc ? (srcType == toN->inputs[p].pin_type)
                               : waywal_pin_types_compatible(srcType, toN->inputs[p].pin_type);
                        if (compat) {
                            self->connectPins(static_cast<uint32_t>(fromNodeId), srcIdx, newId, p);
                            break;
                        }
                    }
                } else if (!fromOutput && srcIdx < fromN->input_pin_count &&
                           srcIdx < WAYWAL_GRAPH_MAX_PINS_PER_NODE) {
                    const uint32_t dstType = fromN->inputs[srcIdx].pin_type;
                    const uint32_t dstFlags = fromN->inputs[srcIdx].flags;
                    const uint32_t srcCount =
                        std::min<uint32_t>(toN->output_pin_count, WAYWAL_GRAPH_MAX_PINS_PER_NODE);
                    for (uint32_t p = 0; p < srcCount; ++p) {
                        const bool nc = ((dstFlags | toN->outputs[p].flags) &
                                         WAYWAL_PIN_FLAG_NON_COERCIBLE) != 0;
                        const bool compat =
                            nc ? (toN->outputs[p].pin_type == dstType)
                               : waywal_pin_types_compatible(toN->outputs[p].pin_type, dstType);
                        if (compat) {
                            self->connectPins(newId, p, static_cast<uint32_t>(fromNodeId), srcIdx);
                            break;
                        }
                    }
                }
            }
        }

        popupPtr->close();
    };

    connect(searchEdit, &QLineEdit::returnPressed, popup, selectCurrent);
    connect(listWidget, &QListWidget::itemDoubleClicked, popup, selectCurrent);

    constexpr int popupW = 260;
    constexpr int popupH = 300;
    QPoint globalPos = m_view->mapToGlobal(m_view->mapFromScene(scenePos));
    const QScreen *screen = QGuiApplication::screenAt(globalPos);
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen) {
        const QRect avail = screen->availableGeometry();
        globalPos.setX(std::clamp(globalPos.x(), avail.left(),
                                  std::max(avail.left(), avail.right() - popupW)));
        globalPos.setY(
            std::clamp(globalPos.y(), avail.top(), std::max(avail.top(), avail.bottom() - popupH)));
    }
    popup->resize(popupW, popupH);
    popup->move(globalPos);
    popup->show();
    searchEdit->setFocus();
}

bool NodeGraphEditorWidget::focusNextPrevChild(bool next)
{
    if (hasFocus() || (m_view && m_view->hasFocus())) {
        return false;
    }
    return QWidget::focusNextPrevChild(next);
}

void NodeGraphEditorWidget::keyPressEvent(QKeyEvent *event)
{
    if ((event->key() == Qt::Key_A && (event->modifiers() & Qt::ShiftModifier)) ||
        event->key() == Qt::Key_Tab || event->key() == Qt::Key_Space) {
        QPoint mouseViewPos = m_view->mapFromGlobal(QCursor::pos());
        QPointF scenePos = m_view->mapToScene(mouseViewPos);
        openQuickConnectPopup(scenePos, -1, -1, true);
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        removeSelectedNodes();
        event->accept();
        return;
    }

    if (event->matches(QKeySequence::Undo)) {
        if (m_commandBus) {
            m_commandBus->undo();
        }
        event->accept();
        return;
    }

    if (event->matches(QKeySequence::Redo)) {
        if (m_commandBus) {
            m_commandBus->redo();
        }
        event->accept();
        return;
    }

    QWidget::keyPressEvent(event);
}

void NodeGraphEditorWidget::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasFormat(QStringLiteral("application/x-waywal-effect-id")) ||
        event->mimeData()->hasUrls() || event->mimeData()->hasText()) {
        event->acceptProposedAction();
    }
}

void NodeGraphEditorWidget::dragMoveEvent(QDragMoveEvent *event)
{
    if (event->mimeData()->hasFormat(QStringLiteral("application/x-waywal-effect-id")) ||
        event->mimeData()->hasUrls() || event->mimeData()->hasText()) {
        event->acceptProposedAction();
    }
}

void NodeGraphEditorWidget::dropEvent(QDropEvent *event)
{
    const QMimeData *mime = event->mimeData();
    if (!mime) {
        return;
    }

    const QPoint viewPos = m_view->viewport()->mapFrom(this, event->position().toPoint());
    const QPointF scenePos = m_view->mapToScene(viewPos);

    if (mime->hasFormat(QStringLiteral("application/x-waywal-effect-id"))) {
        const QByteArray data = mime->data(QStringLiteral("application/x-waywal-effect-id"));
        bool ok = false;
        uint32_t effectId = 0;
        const QString text = QString::fromUtf8(data);
        const int colonIdx = text.indexOf(QLatin1Char(':'));
        if (colonIdx >= 0) {
            effectId = text.left(colonIdx).toUInt(&ok);
        } else {
            effectId = text.toUInt(&ok);
        }
        if (!ok && data.size() >= static_cast<int>(sizeof(uint32_t))) {
            std::memcpy(&effectId, data.constData(), sizeof(effectId));
            ok = (we_builtin_effect_get_by_id(static_cast<we_builtin_effect_id_t>(effectId)) !=
                  nullptr);
        }
        if (ok &&
            addBuiltinEffectNode(static_cast<we_builtin_effect_id_t>(effectId), scenePos) > 0) {
            event->acceptProposedAction();
            return;
        }
    }

    if (mime->hasUrls()) {
        for (const QUrl &url : mime->urls()) {
            if (!url.isLocalFile()) {
                continue;
            }
            const QString localPath = url.toLocalFile();
            if (localPath.endsWith(QStringLiteral(".wwgraph"), Qt::CaseInsensitive) ||
                localPath.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
                if (loadGraphFromFile(localPath)) {
                    event->acceptProposedAction();
                    return;
                }
            }
        }
    }

    if (mime->hasText()) {
        const QString txt = mime->text().trimmed();
        bool ok = false;
        uint32_t effectId = 0;
        const int colonIdx = txt.indexOf(QLatin1Char(':'));
        if (colonIdx >= 0) {
            effectId = txt.left(colonIdx).toUInt(&ok);
        } else {
            effectId = txt.toUInt(&ok);
        }
        if (!ok || !we_builtin_effect_get_by_id(static_cast<we_builtin_effect_id_t>(effectId))) {
            size_t count = 0;
            const we_builtin_effect_desc_t *table = we_builtin_effects_get_table(&count);
            for (size_t i = 0; i < count; ++i) {
                if (txt.compare(QString::fromUtf8(table[i].slug), Qt::CaseInsensitive) == 0 ||
                    txt.compare(QString::fromUtf8(table[i].display_name), Qt::CaseInsensitive) ==
                        0) {
                    effectId = table[i].id;
                    ok = true;
                    break;
                }
            }
        }
        if (ok &&
            addBuiltinEffectNode(static_cast<we_builtin_effect_id_t>(effectId), scenePos) > 0) {
            event->acceptProposedAction();
            return;
        }
    }
}

void NodeGraphEditorWidget::rebuildSceneFromModel()
{
    const QScopedValueRollback<bool> guard(m_rebuildingScene, true);
    resyncPinConnectedFlags(m_graph.get());

    m_nodeItems.clear();
    m_wireItems.clear();
    m_scene->clear();

    m_nodeItems.reserve(m_graph->node_count);
    for (uint32_t i = 0; i < m_graph->node_count; ++i) {
        const uint32_t nid = m_graph->nodes[i].node_id;
        auto *item = new NodeItem(nid, this);
        m_scene->addItem(item);
        m_nodeItems[nid] = item;
    }

    m_wireItems.reserve(m_graph->edge_count);
    for (uint32_t i = 0; i < m_graph->edge_count; ++i) {
        auto *item = new WireItem(m_graph->edges[i].edge_id, this);
        m_scene->addItem(item);
        m_wireItems.push_back(item);
    }

    rebuildPinSpatialGrid();
    if (!m_activeSearchFilter.isEmpty()) {
        applySearchFilter(m_activeSearchFilter);
    }
    m_scene->update();
}

void NodeGraphEditorWidget::rebuildPinSpatialGrid()
{
    m_pinSpatialGrid.rebuildFromGraph(*m_graph, kNodeWidth, kHeaderHeight, kPinRowHeight);
}

void NodeGraphEditorWidget::updateWiresFromLiveItemPositions()
{
    for (WireItem *wire : m_wireItems) {
        if (wire) {
            wire->updateEndpointsFromLiveNodes();
        }
    }
}

void NodeGraphEditorWidget::applySearchFilter(const QString &query)
{
    const QString trimmed = query.trimmed();
    std::unordered_set<uint32_t> matchedNodes;
    for (const auto &[nid, item] : m_nodeItems) {
        if (!item) {
            continue;
        }
        if (trimmed.isEmpty()) {
            item->setOpacity(1.0);
            continue;
        }
        const auto *node = waywal_shader_graph_find_node_const(m_graph.get(), nid);
        bool matches = false;
        if (node) {
            matches =
                QString::fromUtf8(node->title).contains(trimmed, Qt::CaseInsensitive) ||
                QString::fromUtf8(node->prop_or_asset_key).contains(trimmed, Qt::CaseInsensitive) ||
                QString::fromUtf8(node->custom_label).contains(trimmed, Qt::CaseInsensitive);
        }
        if (matches) {
            matchedNodes.insert(nid);
        }
        item->setOpacity(matches ? 1.0 : 0.25);
    }
    for (WireItem *wire : m_wireItems) {
        if (!wire) {
            continue;
        }
        if (trimmed.isEmpty()) {
            wire->setOpacity(1.0);
        } else {
            const bool touchesMatch = matchedNodes.count(wire->srcNodeId()) > 0 ||
                                      matchedNodes.count(wire->dstNodeId()) > 0;
            wire->setOpacity(touchesMatch ? 1.0 : 0.18);
        }
    }
    m_scene->update();
}

void NodeGraphEditorWidget::applyNodeMutation(const waywal_graph_node_t &node, bool isAdd)
{
    if (isAdd) {
        bool exists = false;
        for (uint32_t i = 0; i < m_graph->node_count; ++i) {
            if (m_graph->nodes[i].node_id == node.node_id) {
                m_graph->nodes[i] = node;
                exists = true;
                break;
            }
        }
        if (!exists && m_graph->node_count < WAYWAL_GRAPH_MAX_NODES) {
            m_graph->nodes[m_graph->node_count++] = node;
        }
    } else {
        waywal_shader_graph_remove_node(m_graph.get(), node.node_id);
        m_nodeThumbnails.remove(node.node_id);
    }

    resyncPinConnectedFlags(m_graph.get());
    rebuildSceneFromModel();
    validateAndSchedule();
    if (m_graph->flags & WAYWAL_GRAPH_FLAG_AUTO_COMPILE) {
        compileToGlslSource();
    }
    emit graphModified();
}

void NodeGraphEditorWidget::applyWireMutation(const waywal_graph_edge_t &edge, bool isConnect)
{
    if (isConnect) {
        for (uint32_t i = 0; i < m_graph->edge_count;) {
            const auto &e = m_graph->edges[i];
            if (e.dst_node_id == edge.dst_node_id && e.dst_pin_index == edge.dst_pin_index &&
                e.edge_id != edge.edge_id) {
                waywal_shader_graph_disconnect(m_graph.get(), e.edge_id);
            } else {
                ++i;
            }
        }

        bool exists = false;
        for (uint32_t i = 0; i < m_graph->edge_count; ++i) {
            if (m_graph->edges[i].edge_id == edge.edge_id) {
                exists = true;
                break;
            }
        }
        if (!exists && m_graph->edge_count < WAYWAL_GRAPH_MAX_EDGES) {
            m_graph->edges[m_graph->edge_count++] = edge;
        }
    } else {
        if (!waywal_shader_graph_disconnect(m_graph.get(), edge.edge_id)) {
            return;
        }
    }

    resyncPinConnectedFlags(m_graph.get());
    rebuildSceneFromModel();
    validateAndSchedule();
    if (m_graph->flags & WAYWAL_GRAPH_FLAG_AUTO_COMPILE) {
        compileToGlslSource();
    }
    emit graphModified();
}

} // namespace waywal::studio
