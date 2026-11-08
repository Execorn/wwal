// SPDX-License-Identifier: GPL-3.0-or-later
#include "virtualized_grid_widget.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QFileDialog>
#include <QGridLayout>
#include <QListWidget>
#include <QMenu>
#include <QMetaObject>
#include <QResizeEvent>
#include <QScrollArea>
#include <QLinearGradient>
#include <QPainterPath>

namespace wwal::studio {

namespace {

QColor badgeColor(WallpaperBadge badge)
{
    switch (badge) {
    case WallpaperBadge::Image:
        return QColor(14, 165, 233); // Sky blue
    case WallpaperBadge::Video:
        return QColor(168, 85, 247); // Purple
    case WallpaperBadge::Shader:
        return QColor(16, 185, 129); // Emerald green
    case WallpaperBadge::Scene:
        return QColor(249, 115, 22); // Orange
    case WallpaperBadge::GnomeXml:
        return QColor(234, 179, 8); // Yellow
    case WallpaperBadge::Favorite:
        return QColor(236, 72, 153); // Pink
    }
    return QColor(100, 116, 139);
}

const char *badgeText(WallpaperBadge badge)
{
    switch (badge) {
    case WallpaperBadge::Image:
        return "IMAGE";
    case WallpaperBadge::Video:
        return "VIDEO";
    case WallpaperBadge::Shader:
        return "SHADER";
    case WallpaperBadge::Scene:
        return "WE_SCENE";
    case WallpaperBadge::GnomeXml:
        return "XML";
    case WallpaperBadge::Favorite:
        return "FAV";
    }
    return "WALL";
}

} // namespace

// ============================================================================
// GridGlCanvas Implementation
// ============================================================================

GridGlCanvas::GridGlCanvas(FuzzyCatalogIndex &index, GpuTexturePool &pool,
                           HoverPreviewController &hover, QWidget *parent)
    : QOpenGLWidget(parent), m_index(index), m_pool(pool), m_hover(hover)
{

    setAutoFillBackground(false);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    m_fallbackCanvas = new QLabel(this);
    m_fallbackCanvas->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_fallbackCanvas->setVisible(false);

    // Repaint when background worker threads finish decoding a thumbnail
    m_pool.setThumbnailReadyCallback([this]() {
        QMetaObject::invokeMethod(this, [this]() { update(); }, Qt::QueuedConnection);
    });

    // Repaint on hover preview frame updates
    connect(&m_hover, &HoverPreviewController::previewFrameReady, this,
            [this](uint32_t, GLuint) { update(); });
    connect(&m_hover, &HoverPreviewController::previewStopped, this,
            [this](uint32_t) { update(); });
}

GridGlCanvas::~GridGlCanvas()
{
    if (isValid()) {
        makeCurrent();
        m_pool.destroyGl(this);
        m_hover.destroyGl(this);
        doneCurrent();
    }
}

int GridGlCanvas::computeColumns(int container_width) const noexcept
{
    return std::max(1, (container_width + SPACING_X) / (ITEM_WIDTH + SPACING_X));
}

int GridGlCanvas::computeTotalRows(size_t total_items, int columns) const noexcept
{
    if (total_items == 0 || columns <= 0)
        return 0;
    return static_cast<int>((total_items + static_cast<size_t>(columns) - 1) /
                            static_cast<size_t>(columns));
}

int GridGlCanvas::computeVirtualHeight(size_t total_items, int columns) const noexcept
{
    // If N_total = 0, H_virtual = 0, preventing negative scroll offsets
    if (total_items == 0 || columns <= 0) {
        return 0;
    }
    int r_total = computeTotalRows(total_items, columns);
    return r_total * ROW_HEIGHT - SPACING_Y;
}

GridVisibleSlice GridGlCanvas::computeVisibleSlice(size_t total_items, int columns, int scroll_y,
                                                   int viewport_h) const noexcept
{
    // Empty Grid Guard:
    if (total_items == 0 || columns <= 0) {
        return GridVisibleSlice{
            .is_empty = true, .row_first = 0, .row_last = 0, .index_first = 0, .index_last = 0};
    }

    int r_total = computeTotalRows(total_items, columns);
    int r_first = std::max(0, (scroll_y / ROW_HEIGHT) - OVERSCAN_ROWS);
    int r_last = std::min(r_total - 1,
                          ((scroll_y + viewport_h + ROW_HEIGHT - 1) / ROW_HEIGHT) + OVERSCAN_ROWS);

    size_t idx_first = static_cast<size_t>(columns * r_first);
    size_t idx_last = std::min(total_items - 1, static_cast<size_t>(columns * (r_last + 1) - 1));

    return GridVisibleSlice{.is_empty = false,
                            .row_first = r_first,
                            .row_last = r_last,
                            .index_first = idx_first,
                            .index_last = idx_last};
}

QRect GridGlCanvas::itemRect(size_t filtered_idx, int columns, int scroll_y) const
{
    if (columns <= 0)
        return QRect();

    int r = static_cast<int>(filtered_idx / static_cast<size_t>(columns));
    int c = static_cast<int>(filtered_idx % static_cast<size_t>(columns));

    const int card_w =
        (columns == 1) ? std::min(ITEM_WIDTH, std::max(120, width() - 16)) : ITEM_WIDTH;
    int total_grid_width = columns * card_w + (columns - 1) * SPACING_X;
    int margin_x = std::max(8, (width() - total_grid_width) / 2);

    int x = margin_x + c * (card_w + SPACING_X);
    int y = SPACING_Y + r * ROW_HEIGHT - scroll_y;

    return QRect(x, y, card_w, ITEM_HEIGHT);
}

std::optional<size_t> GridGlCanvas::itemAtPoint(const QPoint &pos) const
{
    size_t total = m_index.filteredCount();
    if (total == 0)
        return std::nullopt;

    int cols = computeColumns(width());
    const int card_w = (cols == 1) ? std::min(ITEM_WIDTH, std::max(120, width() - 16)) : ITEM_WIDTH;
    int total_grid_width = cols * card_w + (cols - 1) * SPACING_X;
    int margin_x = std::max(8, (width() - total_grid_width) / 2);

    int rel_x = pos.x() - margin_x;
    int rel_y = pos.y() + m_scrollY - SPACING_Y;

    if (rel_x < 0 || rel_y < 0)
        return std::nullopt;

    int c = rel_x / (card_w + SPACING_X);
    int r = rel_y / ROW_HEIGHT;

    if (c >= cols)
        return std::nullopt;

    int in_card_x = rel_x % (card_w + SPACING_X);
    int in_card_y = rel_y % ROW_HEIGHT;

    if (in_card_x > card_w || in_card_y > ITEM_HEIGHT) {
        return std::nullopt; // In spacing gap
    }

    size_t idx = static_cast<size_t>(r * cols + c);
    if (idx < total) {
        return idx;
    }
    return std::nullopt;
}

void GridGlCanvas::setScrollY(int scroll_y)
{
    int max_scroll = std::max(
        0, computeVirtualHeight(m_index.filteredCount(), computeColumns(width())) - height());
    int clamped = std::clamp(scroll_y, 0, max_scroll);
    if (m_scrollY != clamped) {
        m_scrollY = clamped;
        update();
    }
}

void GridGlCanvas::toggleSelection(uint32_t item_id)
{
    if (m_selectedIds.contains(item_id)) {
        m_selectedIds.erase(item_id);
    } else {
        m_selectedIds.insert(item_id);
    }
    m_lastSelectedId = item_id;

    std::vector<uint32_t> list(m_selectedIds.begin(), m_selectedIds.end());
    emit selectionChanged(list);
    update();
}

void GridGlCanvas::selectRange(uint32_t from_id, uint32_t to_id)
{
    size_t total = m_index.filteredCount();
    bool in_range = false;
    for (size_t i = 0; i < total; ++i) {
        uint32_t id = m_index.filteredIdAt(i);
        if (id == from_id || id == to_id) {
            m_selectedIds.insert(id);
            if (!in_range) {
                in_range = true;
            } else {
                break; // End of range
            }
        } else if (in_range) {
            m_selectedIds.insert(id);
        }
    }
    m_lastSelectedId = to_id;

    std::vector<uint32_t> list(m_selectedIds.begin(), m_selectedIds.end());
    emit selectionChanged(list);
    update();
}

void GridGlCanvas::clearSelection()
{
    if (!m_selectedIds.empty()) {
        m_selectedIds.clear();
        m_lastSelectedId = std::nullopt;
        emit selectionChanged({});
        update();
    }
}

void GridGlCanvas::resetHoverState()
{
    m_hover.stop();
    if (m_hoveredFilteredIdx.has_value()) {
        m_hoveredFilteredIdx = std::nullopt;
        emit hoverChanged(std::nullopt);
    }
}

void GridGlCanvas::initializeGL()
{
    initializeOpenGLFunctions();

    // Initialize 3-Level LRU Texture Pool & Dual PBO Uploads
    m_pool.initGl(this);

    // Initialize Hover Preview Controller
    m_hover.initGl(this);

    m_glInitialized = true;
}

void GridGlCanvas::resizeGL(int w, int h)
{
    glViewport(0, 0, w, h);
    int vh = computeVirtualHeight(m_index.filteredCount(), computeColumns(w));
    emit virtualHeightChanged(vh);
}

void GridGlCanvas::resizeEvent(QResizeEvent *event)
{
    QOpenGLWidget::resizeEvent(event);
    int vh = computeVirtualHeight(m_index.filteredCount(), computeColumns(width()));
    emit virtualHeightChanged(vh);
}

void GridGlCanvas::paintGL()
{
    if (isValid()) {
        m_pool.processPendingUploads(this, 12);
        glClearColor(0.08f, 0.09f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    } else {
        m_pool.processPendingUploads(nullptr, 12);
    }

    QPainter painter(this);
    renderCards(painter);
}

void GridGlCanvas::paintEvent(QPaintEvent *event)
{
    if (isValid()) {
        if (m_fallbackCanvas && m_fallbackCanvas->isVisible()) {
            m_fallbackCanvas->hide();
        }
        QOpenGLWidget::paintEvent(event);
        return;
    }
    if (!m_fallbackCanvas || width() <= 0 || height() <= 0) {
        return;
    }

    m_pool.processPendingUploads(nullptr, 12);

    const qreal dpr = devicePixelRatioF();
    QImage frame(std::max(1, static_cast<int>(width() * dpr)),
                 std::max(1, static_cast<int>(height() * dpr)),
                 QImage::Format_ARGB32_Premultiplied);
    frame.setDevicePixelRatio(dpr);
    frame.fill(QColor(20, 23, 28));

    QPainter painter(&frame);
    renderCards(painter);
    painter.end();

    m_fallbackCanvas->setGeometry(rect());
    m_fallbackCanvas->setPixmap(QPixmap::fromImage(frame));
    if (!m_fallbackCanvas->isVisible()) {
        m_fallbackCanvas->show();
    }
}

void GridGlCanvas::renderCards(QPainter &painter)
{
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    size_t total = m_index.filteredCount();
    int cols = computeColumns(width());
    GridVisibleSlice slice = computeVisibleSlice(total, cols, m_scrollY, height());

    if (slice.is_empty) {
        painter.setPen(QColor(148, 163, 184));
        QFont font = painter.font();
        font.setPointSize(10);
        font.setWeight(QFont::Medium);
        painter.setFont(font);
        painter.drawText(rect().adjusted(14, 14, -14, -14), Qt::AlignCenter | Qt::TextWordWrap,
                         "No wallpapers found matching query or tags.");
        return;
    }

    for (size_t i = slice.index_first; i <= slice.index_last; ++i) {
        const auto &item = m_index.filteredItemAt(i);
        QRect card = itemRect(i, cols, m_scrollY);
        if (card.bottom() < 0 || card.top() > height()) {
            continue;
        }

        bool is_hovered = (m_hoveredFilteredIdx.has_value() && *m_hoveredFilteredIdx == i);
        bool is_selected = m_selectedIds.contains(item.id);

        // Hover Zoom: 1.03x scale centered
        QRect render_rect = card;
        if (is_hovered) {
            int dw = static_cast<int>(card.width() * 0.03f);
            int dh = static_cast<int>(card.height() * 0.03f);
            render_rect.adjust(-dw / 2, -dh / 2, dw / 2, dh / 2);
        }

        painter.save();
        QPainterPath card_clip;
        card_clip.addRoundedRect(QRectF(render_rect), 8.0, 8.0);
        painter.setClipPath(card_clip);

        // 1. Draw Card Background (Rich gradient from dominant color while texture loads)
        uint32_t dc = item.dominant_color;
        QColor base_color((dc >> 16) & 0xFF, (dc >> 8) & 0xFF, dc & 0xFF);
        QLinearGradient bg_grad(render_rect.topLeft(), render_rect.bottomRight());
        bg_grad.setColorAt(0.0, base_color.lighter(120));
        bg_grad.setColorAt(0.55, base_color.darker(115));
        bg_grad.setColorAt(1.0, base_color.darker(175));
        painter.fillRect(render_rect, bg_grad);

        // Subtle horizon/wave silhouette inside the card
        QPainterPath wave;
        const qreal wl = render_rect.left();
        const qreal wr = render_rect.right();
        const qreal wb = render_rect.bottom();
        const qreal wy = render_rect.top() + render_rect.height() * 0.56;
        wave.moveTo(wl, wb);
        wave.lineTo(wl, wy);
        wave.cubicTo(wl + render_rect.width() * 0.35, wy - 18.0, wl + render_rect.width() * 0.65,
                     wy + 16.0, wr, wy - 6.0);
        wave.lineTo(wr, wb);
        wave.closeSubpath();
        QColor wave_col = base_color.darker(210);
        wave_col.setAlpha(110);
        painter.fillPath(wave, wave_col);

        // 2. Request thumbnail from 3-Level LRU Texture Pool and draw when ready
        (void)m_pool.requestTexture(item.id, item.path, item.preview_path);
        QImage thumb = m_pool.getThumbnailImage(item.id);
        if (!thumb.isNull()) {
            painter.drawImage(render_rect, thumb);
        }

        // 3. Hover Preview Override: If hover preview active for this item and has a frame
        if (m_hover.isPreviewActive() && m_hover.activeItemId() == item.id) {
            const QImage &previewImg = m_hover.currentPreviewImage();
            if (!previewImg.isNull()) {
                painter.drawImage(render_rect, previewImg);
            }
        }

        // 4. Source Type Badge Pill (Top-Left)
        drawBadgePill(painter, render_rect, item.badge);

        // 5. Selection Checkbox (Top-Right)
        drawCardSelectionIndicator(painter, render_rect, is_selected);

        // 6. Title Banner Drop Shadow (Bottom, clipped cleanly to rounded card corners)
        drawCardTitleOverlay(painter, render_rect, item.title);

        painter.restore();

        // 7. Border & Hover Glow (drawn unclipped for crisp anti-aliased stroke)
        if (is_selected) {
            QPen sel_pen(QColor(56, 189, 248), 2.0); // Sky cyan active border
            painter.setPen(sel_pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(render_rect.adjusted(1, 1, -1, -1), 7.5, 7.5);
        } else if (is_hovered) {
            QPen hover_pen(QColor(129, 140, 248, 220), 2.0); // Indigo glow
            painter.setPen(hover_pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(render_rect.adjusted(1, 1, -1, -1), 7.5, 7.5);
        } else {
            QPen border_pen(QColor(255, 255, 255, 28), 1.0);
            painter.setPen(border_pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(render_rect, 8.0, 8.0);
        }
    }
}

void GridGlCanvas::drawBadgePill(QPainter &painter, const QRect &card_rect, WallpaperBadge badge)
{
    QFont font = painter.font();
    font.setPixelSize(9);
    font.setWeight(QFont::Bold);
    painter.setFont(font);

    QString label = QString::fromUtf8(badgeText(badge));
    QFontMetrics fm(font);
    int pill_w =
        std::clamp(fm.horizontalAdvance(label) + 12, 42, std::max(42, card_rect.width() - 44));
    QRect pill_rect(card_rect.left() + 8, card_rect.top() + 8, pill_w, 19);

    QColor pill_bg = badgeColor(badge);
    pill_bg.setAlpha(215);

    painter.setPen(Qt::NoPen);
    painter.setBrush(pill_bg);
    painter.drawRoundedRect(pill_rect, 4.0, 4.0);

    painter.setPen(Qt::white);
    painter.drawText(pill_rect, Qt::AlignCenter, label);
}

void GridGlCanvas::drawCardSelectionIndicator(QPainter &painter, const QRect &card_rect,
                                              bool is_selected)
{
    QRect box_rect(card_rect.right() - 26, card_rect.top() + 8, 18, 18);

    if (is_selected) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(56, 189, 248)); // Cyan checked
        painter.drawRoundedRect(box_rect, 4.0, 4.0);

        painter.setPen(QPen(Qt::black, 2.0));
        // Checkmark tick
        painter.drawLine(box_rect.left() + 4, box_rect.top() + 9, box_rect.left() + 7,
                         box_rect.top() + 13);
        painter.drawLine(box_rect.left() + 7, box_rect.top() + 13, box_rect.left() + 14,
                         box_rect.top() + 5);
    } else {
        painter.setPen(QPen(QColor(255, 255, 255, 130), 1.5));
        painter.setBrush(QColor(15, 23, 42, 110));
        painter.drawRoundedRect(box_rect, 4.0, 4.0);
    }
}

void GridGlCanvas::drawCardTitleOverlay(QPainter &painter, const QRect &card_rect,
                                        const std::string &title)
{
    int banner_h = 34;
    QRect banner_rect(card_rect.left(), card_rect.bottom() - banner_h + 1, card_rect.width(),
                      banner_h);

    QLinearGradient grad(banner_rect.topLeft(), banner_rect.bottomLeft());
    grad.setColorAt(0.0, QColor(10, 12, 16, 0));
    grad.setColorAt(0.35, QColor(10, 12, 16, 175));
    grad.setColorAt(1.0, QColor(10, 12, 16, 238));

    painter.fillRect(banner_rect, grad);

    QRect text_rect(card_rect.left() + 9, card_rect.bottom() - 22,
                    std::max(20, card_rect.width() - 18), 18);

    painter.setPen(QColor(241, 245, 249));
    QFont font = painter.font();
    font.setPixelSize(10);
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);

    QString qtitle = QString::fromStdString(title);
    QFontMetrics fm(font);
    QString elided = fm.elidedText(qtitle, Qt::ElideRight, text_rect.width());
    painter.drawText(text_rect, Qt::AlignLeft | Qt::AlignVCenter, elided);
}

void GridGlCanvas::mouseMoveEvent(QMouseEvent *event)
{
    auto hit = itemAtPoint(event->pos());
    if (hit != m_hoveredFilteredIdx) {
        if (m_hoveredFilteredIdx.has_value()) {
            if (*m_hoveredFilteredIdx < m_index.filteredCount()) {
                uint32_t prev_id = m_index.filteredIdAt(*m_hoveredFilteredIdx);
                m_hover.onCellHoverExited(prev_id);
            } else {
                m_hover.stop();
            }
        }

        m_hoveredFilteredIdx = hit;
        emit hoverChanged(hit);

        if (hit.has_value() && *hit < m_index.filteredCount()) {
            const auto &item = m_index.filteredItemAt(*hit);
            m_hover.onCellHoverEntered(item.id, item.path, item.badge, item.preview_path);
        }

        update();
    }
}

void GridGlCanvas::mousePressEvent(QMouseEvent *event)
{
    auto hit = itemAtPoint(event->pos());
    if (!hit.has_value()) {
        if (event->button() == Qt::RightButton) {
            QMenu menu(this);
            menu.setStyleSheet("QMenu { background: #1e222b; color: #f1f5f9; border: 1px solid "
                               "#334155; padding: 4px; }"
                               "QMenu::item { padding: 5px 14px; border-radius: 4px; }"
                               "QMenu::item:selected { background: #0284c7; color: white; }");
            QAction *cfgAct = menu.addAction("Configure Wallpaper Folders && Exclusions...");
            connect(cfgAct, &QAction::triggered, this, &GridGlCanvas::configureFoldersRequested);
            menu.exec(event->globalPosition().toPoint());
        }
        return;
    }

    const auto &item = m_index.filteredItemAt(*hit);

    if (event->button() == Qt::RightButton) {
        clearSelection();
        toggleSelection(item.id);
        emit itemSelected(item);

        std::filesystem::path item_p(item.path);
        std::filesystem::path parent_dir = item_p.parent_path();
        QString parent_qstr = QString::fromStdString(parent_dir.string());
        QString parent_name = QString::fromStdString(parent_dir.filename().string());

        QMenu menu(this);
        menu.setStyleSheet(
            "QMenu { background: #1e222b; color: #f1f5f9; border: 1px solid #334155; padding: 4px; "
            "}"
            "QMenu::item { padding: 5px 14px; border-radius: 4px; }"
            "QMenu::item:selected { background: #0284c7; color: white; }"
            "QMenu::separator { height: 1px; background: #334155; margin: 4px 2px; }");
        QAction *applyAct = menu.addAction("Apply Wallpaper Now");
        connect(applyAct, &QAction::triggered, this,
                [this, item]() { emit itemDoubleClicked(item); });
        menu.addSeparator();
        if (!parent_qstr.isEmpty()) {
            QAction *exclAct = menu.addAction(
                QStringLiteral("Exclude Folder '%1' (%2)").arg(parent_name, parent_qstr));
            connect(exclAct, &QAction::triggered, this,
                    [this, parent_qstr]() { emit excludeFolderRequested(parent_qstr); });
        }
        QAction *cfgAct = menu.addAction("Configure Wallpaper Folders && Exclusions...");
        connect(cfgAct, &QAction::triggered, this, &GridGlCanvas::configureFoldersRequested);
        menu.exec(event->globalPosition().toPoint());
        return;
    }

    if (event->modifiers() & Qt::ShiftModifier) {
        if (m_lastSelectedId.has_value()) {
            selectRange(*m_lastSelectedId, item.id);
        } else {
            toggleSelection(item.id);
        }
    } else if (event->modifiers() & Qt::ControlModifier) {
        toggleSelection(item.id);
    } else {
        clearSelection();
        toggleSelection(item.id);
    }

    emit itemSelected(item);
}

void GridGlCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    auto hit = itemAtPoint(event->pos());
    if (hit.has_value()) {
        const auto &item = m_index.filteredItemAt(*hit);
        emit itemDoubleClicked(item);
    }
}

void GridGlCanvas::leaveEvent(QEvent *event)
{
    Q_UNUSED(event);
    if (m_hoveredFilteredIdx.has_value()) {
        if (*m_hoveredFilteredIdx < m_index.filteredCount()) {
            uint32_t id = m_index.filteredIdAt(*m_hoveredFilteredIdx);
            m_hover.onCellHoverExited(id);
        } else {
            m_hover.stop();
        }
        m_hoveredFilteredIdx = std::nullopt;
        emit hoverChanged(std::nullopt);
        update();
    }
}

void GridGlCanvas::wheelEvent(QWheelEvent *event)
{
    int delta = event->angleDelta().y();
    setScrollY(m_scrollY - delta);
    event->accept();
}

// ============================================================================
// VirtualizedGridWidget Implementation
// ============================================================================

VirtualizedGridWidget::VirtualizedGridWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumWidth(200);
    setupUi();
}

VirtualizedGridWidget::~VirtualizedGridWidget()
{
    delete m_canvas;
    m_canvas = nullptr;
}

void VirtualizedGridWidget::setupUi()
{
    auto *root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(6, 6, 6, 6);
    root_layout->setSpacing(5);

    // 1. Top Search Bar (full width so it never crushes in narrow docks)
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText("Search wallpapers (Fuzzy Myers)...");
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setStyleSheet(
        "QLineEdit { background: #1e222b; color: #f1f5f9; border: 1px solid #334155; "
        "border-radius: 5px; padding: 4px 8px; font-size: 11px; }"
        "QLineEdit:focus { border: 1px solid #38bdf8; }");
    connect(m_searchEdit, &QLineEdit::textChanged, this,
            &VirtualizedGridWidget::onSearchTextChanged);
    root_layout->addWidget(m_searchEdit);

    // 1b. Sort, Folders & Status Row
    auto *top_bar = new QHBoxLayout();
    top_bar->setContentsMargins(0, 0, 0, 0);
    top_bar->setSpacing(4);

    m_sortCombo = new QComboBox(this);
    m_sortCombo->setMaximumWidth(92);
    m_sortCombo->addItem("Name (A-Z)", static_cast<int>(SortCriteria::NameAsc));
    m_sortCombo->addItem("Name (Z-A)", static_cast<int>(SortCriteria::NameDesc));
    m_sortCombo->addItem("Date (Newest)", static_cast<int>(SortCriteria::DateNewest));
    m_sortCombo->addItem("Date (Oldest)", static_cast<int>(SortCriteria::DateOldest));
    m_sortCombo->addItem("Size (Largest)", static_cast<int>(SortCriteria::SizeLargest));
    m_sortCombo->addItem("Size (Smallest)", static_cast<int>(SortCriteria::SizeSmallest));
    m_sortCombo->addItem("Type (Badge)", static_cast<int>(SortCriteria::Type));
    m_sortCombo->addItem("Dominant Hue", static_cast<int>(SortCriteria::DominantHue));
    m_sortCombo->setStyleSheet(
        "QComboBox { background: #1e222b; color: #f1f5f9; border: 1px solid #334155; "
        "border-radius: 5px; padding: 2px 5px; font-size: 10px; }");
    connect(m_sortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &VirtualizedGridWidget::onSortChanged);
    top_bar->addWidget(m_sortCombo);

    m_foldersButton = new QPushButton("Folders", this);
    m_foldersButton->setToolTip(
        "Configure included & excluded wallpaper folders (~/.config/wwal/studio_catalog.json)");
    m_foldersButton->setStyleSheet(
        "QPushButton { background: #1e222b; color: #38bdf8; border: 1px solid #334155; "
        "border-radius: 5px; padding: 2px 7px; font-size: 10px; font-weight: 600; }"
        "QPushButton:hover { background: #283040; border-color: #38bdf8; color: #f1f5f9; }");
    connect(m_foldersButton, &QPushButton::clicked, this,
            &VirtualizedGridWidget::openFolderSettingsDialog);
    top_bar->addWidget(m_foldersButton);

    m_statusLabel = new QLabel("0 wallpapers", this);
    m_statusLabel->setStyleSheet("color: #94a3b8; font-size: 9.5px;");
    m_statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_statusLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    top_bar->addWidget(m_statusLabel, 1);

    root_layout->addLayout(top_bar);

    // 2. Tag Filter Chips Grid (4 columns so all tags are visible without clipping)
    m_tagChipContainer = new QWidget(this);
    auto *chip_grid = new QGridLayout(m_tagChipContainer);
    chip_grid->setContentsMargins(0, 0, 0, 0);
    chip_grid->setHorizontalSpacing(3);
    chip_grid->setVerticalSpacing(3);
    for (int col = 0; col < 4; ++col) {
        chip_grid->setColumnStretch(col, 1);
    }

    auto configureChipBtn = [](QPushButton *btn, const QString &tooltip) {
        btn->setCheckable(true);
        btn->setFixedHeight(20);
        btn->setMinimumWidth(34);
        btn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        btn->setToolTip(tooltip);
    };

    // "All" chip
    auto *all_btn = new QPushButton("All", m_tagChipContainer);
    configureChipBtn(all_btn, QStringLiteral("Show all wallpapers"));
    all_btn->setChecked(true);
    connect(all_btn, &QPushButton::clicked, this,
            [this]() { onTagChipClicked(WallpaperTags::None); });
    m_tagChips.push_back(all_btn);
    chip_grid->addWidget(all_btn, 0, 0);

    // Individual tag chips
    int chipIdx = 1;
    for (const auto &tag : WallpaperTags::allKnownTags()) {
        QString label = QString::fromUtf8(tag.display_name);
        if (label == QStringLiteral("Audio-Reactive")) {
            label = QStringLiteral("Audio");
        } else if (label == QStringLiteral("Cyberpunk")) {
            label = QStringLiteral("Cyber");
        } else if (label == QStringLiteral("Architecture")) {
            label = QStringLiteral("Arch");
        } else if (label == QStringLiteral("Photography")) {
            label = QStringLiteral("Photo");
        } else if (label == QStringLiteral("Digital Art")) {
            label = QStringLiteral("Digital");
        } else if (label == QStringLiteral("Landscape")) {
            label = QStringLiteral("Scenic");
        }
        auto *btn = new QPushButton(label, m_tagChipContainer);
        configureChipBtn(
            btn, QStringLiteral("Filter by tag: %1").arg(QString::fromUtf8(tag.display_name)));
        uint64_t mask = tag.mask;
        connect(btn, &QPushButton::clicked, this, [this, mask]() { onTagChipClicked(mask); });
        m_tagChips.push_back(btn);
        chip_grid->addWidget(btn, chipIdx / 4, chipIdx % 4);
        ++chipIdx;
    }
    updateTagChipStyles();
    root_layout->addWidget(m_tagChipContainer);

    // 3. Central Grid Canvas + Vertical Scroll Bar
    auto *grid_layout = new QHBoxLayout();
    grid_layout->setContentsMargins(0, 0, 0, 0);
    grid_layout->setSpacing(2);

    m_canvas = new GridGlCanvas(m_index, m_pool, m_hover, this);
    m_scrollBar = new QScrollBar(Qt::Vertical, this);
    m_scrollBar->setVisible(false);
    m_scrollBar->setStyleSheet(
        "QScrollBar:vertical { background: #14171d; width: 10px; margin: 0; }"
        "QScrollBar::handle:vertical { background: #334155; min-height: 24px; border-radius: 4px; }"
        "QScrollBar::handle:vertical:hover { background: #475569; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");

    connect(m_scrollBar, &QScrollBar::valueChanged, this,
            &VirtualizedGridWidget::onScrollValueChanged);
    connect(m_canvas, &GridGlCanvas::virtualHeightChanged, this,
            &VirtualizedGridWidget::onVirtualHeightChanged);

    connect(m_canvas, &GridGlCanvas::itemSelected, this, &VirtualizedGridWidget::wallpaperSelected);
    connect(m_canvas, &GridGlCanvas::itemDoubleClicked, this,
            &VirtualizedGridWidget::wallpaperDoubleClicked);
    connect(m_canvas, &GridGlCanvas::configureFoldersRequested, this,
            &VirtualizedGridWidget::openFolderSettingsDialog);
    connect(m_canvas, &GridGlCanvas::excludeFolderRequested, this,
            [this](const QString &folder_path) {
                m_index.addExcludedDirectory(folder_path.toStdString());
                (void)m_index.saveConfigToFile();
                scanWallpapers();
                selectFirstItemIfAvailable();
            });
    connect(m_canvas, &GridGlCanvas::selectionChanged, this, [this](const auto &ids) {
        emit playlistSelectionChanged(ids);
        if (m_playlistBar) {
            m_playlistBar->setVisible(!ids.empty());
            if (m_playlistSelectionLabel) {
                m_playlistSelectionLabel->setText(QString("%1 selected").arg(ids.size()));
            }
        }
    });

    grid_layout->addWidget(m_canvas, 1);
    grid_layout->addWidget(m_scrollBar);
    root_layout->addLayout(grid_layout, 1);

    // 4. Multi-Select Playlist Bar (shown when items are selected)
    m_playlistBar = new QWidget(this);
    m_playlistBar->setVisible(false);
    m_playlistBar->setStyleSheet("QWidget { background: #1e222b; border: 1px solid #334155; "
                                 "border-radius: 5px; padding: 2px; }");
    auto *pl_layout = new QHBoxLayout(m_playlistBar);
    pl_layout->setContentsMargins(6, 4, 6, 4);
    pl_layout->setSpacing(4);

    m_playlistSelectionLabel = new QLabel("0 selected", m_playlistBar);
    m_playlistSelectionLabel->setStyleSheet(
        "color: #f1f5f9; font-weight: bold; font-size: 9.5px; border: none;");
    pl_layout->addWidget(m_playlistSelectionLabel);

    pl_layout->addStretch();

    m_playlistAddButton = new QPushButton("+ Playlist", m_playlistBar);
    m_playlistAddButton->setStyleSheet(
        "QPushButton { background: #0284c7; color: white; border: none; border-radius: 4px; "
        "padding: 3px 8px; font-weight: bold; font-size: 9.5px; }"
        "QPushButton:hover { background: #0369a1; }");
    connect(m_playlistAddButton, &QPushButton::clicked, this,
            &VirtualizedGridWidget::onPlaylistAddClicked);
    pl_layout->addWidget(m_playlistAddButton);

    m_playlistClearButton = new QPushButton("Clear", m_playlistBar);
    m_playlistClearButton->setStyleSheet("QPushButton { background: transparent; color: #94a3b8; "
                                         "border: none; padding: 3px 5px; font-size: 9.5px; }"
                                         "QPushButton:hover { color: #f1f5f9; }");
    connect(m_playlistClearButton, &QPushButton::clicked, this,
            &VirtualizedGridWidget::clearSelection);
    pl_layout->addWidget(m_playlistClearButton);

    root_layout->addWidget(m_playlistBar);
}

void VirtualizedGridWidget::scanWallpapers(const std::vector<std::string> &extra_paths)
{
    if (m_canvas) {
        m_canvas->resetHoverState();
        m_canvas->clearSelection();
    }
    size_t count = m_index.scanAll(extra_paths);
    int cols = m_canvas->computeColumns(m_canvas->width());
    int vh = m_canvas->computeVirtualHeight(m_index.filteredCount(), cols);
    onVirtualHeightChanged(vh);
    updateStatusText();
    m_canvas->update();
    emit catalogScanned(count);
}

void VirtualizedGridWidget::populateSampleCatalogIfEmpty(bool force_showcase_cards)
{
    if (!force_showcase_cards && m_index.totalCount() > 0) {
        return;
    }

    struct SampleDef {
        const char *title;
        const char *path;
        WallpaperBadge badge;
        uint32_t color;
        uint64_t tags;
    };

    const SampleDef samples[] = {
        {"01 Cyberpunk Neon Rainstorm", "/usr/share/wwal/samples/cyberpunk_neon.glsl",
         WallpaperBadge::Shader, 0x0ea5e9,
         WallpaperTags::Cyberpunk | WallpaperTags::Dark | WallpaperTags::Urban},
        {"02 Abyssal Bioluminescence", "/usr/share/wwal/samples/abyssal_reef.json",
         WallpaperBadge::Scene, 0x6366f1,
         WallpaperTags::Nature | WallpaperTags::Dark | WallpaperTags::Fantasy},
        {"03 Solarized Horizon 4K", "/usr/share/wwal/samples/solarized_horizon.png",
         WallpaperBadge::Image, 0xf97316,
         WallpaperTags::Landscape | WallpaperTags::Minimal | WallpaperTags::Nature},
        {"04 Quantum Fluid Vortex", "/usr/share/wwal/samples/quantum_fluid.glsl",
         WallpaperBadge::Shader, 0x10b981,
         WallpaperTags::Abstract | WallpaperTags::SciFi | WallpaperTags::Dark},
        {"05 Tokyo Midnight Shinjuku", "/usr/share/wwal/samples/tokyo_shinjuku.mp4",
         WallpaperBadge::Video, 0xa855f7,
         WallpaperTags::Urban | WallpaperTags::Cyberpunk | WallpaperTags::Photography},
        {"06 Celestial Nebula Drift", "/usr/share/wwal/samples/celestial_nebula.json",
         WallpaperBadge::Scene, 0xec4899,
         WallpaperTags::Space | WallpaperTags::SciFi | WallpaperTags::DigitalArt},
        {"07 Nordic Fjord Minimal", "/usr/share/wwal/samples/nordic_fjord.png",
         WallpaperBadge::Image, 0x3b82f6,
         WallpaperTags::Landscape | WallpaperTags::Minimal | WallpaperTags::Light},
        {"08 Synthwave Outrun Grid", "/usr/share/wwal/samples/synthwave_grid.glsl",
         WallpaperBadge::Shader, 0xd946ef,
         WallpaperTags::Retro | WallpaperTags::Cyberpunk | WallpaperTags::Vehicles},
    };

    int64_t base_time = 1710000000;
    for (size_t i = 0; i < std::size(samples); ++i) {
        CatalogItem item;
        item.title = samples[i].title;
        item.path = samples[i].path;
        item.badge = samples[i].badge;
        item.dominant_color = samples[i].color;
        item.tag_mask = samples[i].tags;
        item.file_size = (2 + i * 3) * 1024 * 1024;
        item.date_modified = base_time + static_cast<int64_t>(i * 3600);
        m_index.addItem(std::move(item));
    }

    m_index.refilter();
    int cols = m_canvas->computeColumns(m_canvas->width());
    int vh = m_canvas->computeVirtualHeight(m_index.filteredCount(), cols);
    onVirtualHeightChanged(vh);
    updateStatusText();
    m_canvas->update();
    emit catalogScanned(m_index.totalCount());
}

void VirtualizedGridWidget::selectFirstItemIfAvailable()
{
    if (m_index.filteredCount() > 0 && m_canvas) {
        const CatalogItem &first = m_index.filteredItemAt(0);
        m_canvas->clearSelection();
        m_canvas->toggleSelection(first.id);
        emit wallpaperSelected(first);
    }
}

void VirtualizedGridWidget::openFolderSettingsDialog()
{
    QDialog dlg(this);
    dlg.setWindowTitle("Wallpaper Folders & Exclusions");
    dlg.setMinimumSize(540, 480);
    dlg.setStyleSheet(
        "QDialog { background: #14171d; color: #f1f5f9; }"
        "QLabel { color: #e2e8f0; font-size: 11px; }"
        "QListWidget { background: #1e222b; color: #f1f5f9; border: 1px solid #334155; "
        "border-radius: 5px; padding: 4px; font-size: 11px; }"
        "QLineEdit { background: #1e222b; color: #f1f5f9; border: 1px solid #334155; "
        "border-radius: 5px; padding: 4px 8px; font-size: 11px; }"
        "QPushButton { background: #1e222b; color: #f1f5f9; border: 1px solid #334155; "
        "border-radius: 5px; padding: 5px 10px; font-size: 11px; font-weight: 600; }"
        "QPushButton:hover { background: #283040; border-color: #38bdf8; }"
        "QCheckBox { color: #cbd5e1; font-size: 11px; spacing: 6px; }");

    auto *layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    // 1. Included Directories Section
    auto *incHeader =
        new QLabel("<b>Included Wallpaper Folders</b> (only these folders are scanned):", &dlg);
    layout->addWidget(incHeader);

    auto *incList = new QListWidget(&dlg);
    for (const auto &dir : m_index.includedDirectories()) {
        incList->addItem(QString::fromStdString(dir));
    }
    layout->addWidget(incList, 1);

    auto *incBtnRow = new QHBoxLayout();
    auto *addIncBtn = new QPushButton("+ Add Folder...", &dlg);
    auto *onlyWallpapersBtn = new QPushButton("Only ~/Pictures/Wallpapers", &dlg);
    auto *remIncBtn = new QPushButton("Remove Selected", &dlg);
    incBtnRow->addWidget(addIncBtn);
    incBtnRow->addWidget(onlyWallpapersBtn);
    incBtnRow->addWidget(remIncBtn);
    incBtnRow->addStretch();
    layout->addLayout(incBtnRow);

    connect(addIncBtn, &QPushButton::clicked, &dlg, [&dlg, incList]() {
        QString dir = QFileDialog::getExistingDirectory(&dlg, "Select Wallpaper Folder to Include");
        if (!dir.isEmpty()) {
            incList->addItem(dir);
        }
    });
    connect(onlyWallpapersBtn, &QPushButton::clicked, &dlg, [incList]() {
        incList->clear();
        incList->addItem(
            QString::fromStdString(FuzzyCatalogIndex::expandUserPath("~/Pictures/Wallpapers")));
    });
    connect(remIncBtn, &QPushButton::clicked, &dlg,
            [incList]() { qDeleteAll(incList->selectedItems()); });

    // 2. Excluded Directories / Subfolder Names Section
    auto *excHeader =
        new QLabel("<b>Excluded Folders or Folder Names</b> (e.g. <code>Screenshots</code>, "
                   "<code>~/Pictures/Camera</code>):",
                   &dlg);
    layout->addWidget(excHeader);

    auto *excList = new QListWidget(&dlg);
    for (const auto &dir : m_index.excludedDirectories()) {
        excList->addItem(QString::fromStdString(dir));
    }
    layout->addWidget(excList, 1);

    auto *excInputRow = new QHBoxLayout();
    auto *excPatternEdit = new QLineEdit(&dlg);
    excPatternEdit->setPlaceholderText(
        "Folder name or path to exclude (e.g. Screenshots, Memes)...");
    auto *addExcPatternBtn = new QPushButton("+ Exclude Name/Path", &dlg);
    auto *addExcDirBtn = new QPushButton("Browse Folder...", &dlg);
    auto *remExcBtn = new QPushButton("Remove Selected", &dlg);
    excInputRow->addWidget(excPatternEdit, 1);
    excInputRow->addWidget(addExcPatternBtn);
    excInputRow->addWidget(addExcDirBtn);
    excInputRow->addWidget(remExcBtn);
    layout->addLayout(excInputRow);

    connect(addExcPatternBtn, &QPushButton::clicked, &dlg, [excPatternEdit, excList]() {
        QString txt = excPatternEdit->text().trimmed();
        if (!txt.isEmpty()) {
            excList->addItem(txt);
            excPatternEdit->clear();
        }
    });
    connect(excPatternEdit, &QLineEdit::returnPressed, addExcPatternBtn, &QPushButton::click);
    connect(addExcDirBtn, &QPushButton::clicked, &dlg, [&dlg, excList]() {
        QString dir = QFileDialog::getExistingDirectory(&dlg, "Select Folder to Exclude");
        if (!dir.isEmpty()) {
            excList->addItem(dir);
        }
    });
    connect(remExcBtn, &QPushButton::clicked, &dlg,
            [excList]() { qDeleteAll(excList->selectedItems()); });

    // 3. System & Steam Workshop Checkbox
    auto *sysCheck =
        new QCheckBox("Also scan system (/usr/share/backgrounds) and Steam Workshop folders", &dlg);
    sysCheck->setChecked(m_index.scanSystemAndWorkshopPaths());
    layout->addWidget(sysCheck);

    auto *cfgPathNote = new QLabel(
        QStringLiteral("Saved to: <code>%1</code> (or set <code>WWAL_WALLPAPER_DIRS</code> / "
                       "<code>WWAL_EXCLUDE_DIRS</code>)")
            .arg(QString::fromStdString(FuzzyCatalogIndex::defaultConfigFilePath())),
        &dlg);
    cfgPathNote->setStyleSheet("color: #64748b; font-size: 10px;");
    cfgPathNote->setWordWrap(true);
    layout->addWidget(cfgPathNote);

    // 4. Bottom Action Row
    auto *bottomRow = new QHBoxLayout();
    auto *resetBtn = new QPushButton("Reset Defaults", &dlg);
    bottomRow->addWidget(resetBtn);
    bottomRow->addStretch();

    auto *cancelBtn = new QPushButton("Cancel", &dlg);
    auto *saveBtn = new QPushButton("Save && Rescan", &dlg);
    saveBtn->setStyleSheet(
        "QPushButton { background: #0284c7; color: white; border: 1px solid #38bdf8; "
        "border-radius: 5px; padding: 6px 14px; font-weight: bold; }"
        "QPushButton:hover { background: #0369a1; }");
    bottomRow->addWidget(cancelBtn);
    bottomRow->addWidget(saveBtn);
    layout->addLayout(bottomRow);

    connect(resetBtn, &QPushButton::clicked, &dlg, [incList, excList, sysCheck]() {
        FuzzyCatalogIndex defaultsIdx;
        defaultsIdx.resetFolderConfigToDefaults();
        incList->clear();
        for (const auto &d : defaultsIdx.includedDirectories()) {
            incList->addItem(QString::fromStdString(d));
        }
        excList->clear();
        for (const auto &d : defaultsIdx.excludedDirectories()) {
            excList->addItem(QString::fromStdString(d));
        }
        sysCheck->setChecked(defaultsIdx.scanSystemAndWorkshopPaths());
    });
    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(saveBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    if (dlg.exec() == QDialog::Accepted) {
        std::vector<std::string> newInc;
        newInc.reserve(static_cast<size_t>(incList->count()));
        for (int i = 0; i < incList->count(); ++i) {
            newInc.push_back(incList->item(i)->text().toStdString());
        }
        std::vector<std::string> newExc;
        newExc.reserve(static_cast<size_t>(excList->count()));
        for (int i = 0; i < excList->count(); ++i) {
            newExc.push_back(excList->item(i)->text().toStdString());
        }

        m_index.setIncludedDirectories(newInc);
        m_index.setExcludedDirectories(newExc);
        m_index.setScanSystemAndWorkshopPaths(sysCheck->isChecked());
        (void)m_index.saveConfigToFile();

        scanWallpapers();
        selectFirstItemIfAvailable();
    }
}

void VirtualizedGridWidget::onSearchTextChanged(const QString &text)
{
    if (m_canvas) {
        m_canvas->resetHoverState();
    }
    m_index.setSearchQuery(text.toStdString());
    int cols = m_canvas->computeColumns(m_canvas->width());
    int vh = m_canvas->computeVirtualHeight(m_index.filteredCount(), cols);
    onVirtualHeightChanged(vh);
    updateStatusText();
    m_canvas->update();
}

void VirtualizedGridWidget::onTagChipClicked(uint64_t tag_mask)
{
    if (m_canvas) {
        m_canvas->resetHoverState();
    }
    if (tag_mask == WallpaperTags::None) {
        m_index.setTagMask(0);
    } else {
        uint64_t current = m_index.activeTagMask();
        if (current & tag_mask) {
            current &= ~tag_mask;
        } else {
            current |= tag_mask;
        }
        m_index.setTagMask(current, false); // Match any
    }

    updateTagChipStyles();
    int cols = m_canvas->computeColumns(m_canvas->width());
    int vh = m_canvas->computeVirtualHeight(m_index.filteredCount(), cols);
    onVirtualHeightChanged(vh);
    updateStatusText();
    m_canvas->update();
}

void VirtualizedGridWidget::onSortChanged(int combo_index)
{
    if (m_canvas) {
        m_canvas->resetHoverState();
    }
    auto criteria = static_cast<SortCriteria>(m_sortCombo->itemData(combo_index).toInt());
    m_index.setSorting(criteria);
    m_canvas->update();
}

void VirtualizedGridWidget::onScrollValueChanged(int value) { m_canvas->setScrollY(value); }

void VirtualizedGridWidget::onVirtualHeightChanged(int virtual_height)
{
    int max_val = std::max(0, virtual_height - m_canvas->height());
    m_scrollBar->setRange(0, max_val);
    m_scrollBar->setPageStep(m_canvas->height());
    m_scrollBar->setVisible(max_val > 0);
}

void VirtualizedGridWidget::onPlaylistAddClicked()
{
    auto ids = selectedIds();
    if (!ids.empty()) {
        emit playlistSelectionChanged(ids);
    }
}

void VirtualizedGridWidget::clearSelection() { m_canvas->clearSelection(); }

std::vector<uint32_t> VirtualizedGridWidget::selectedIds() const
{
    const auto &set = m_canvas->selectedIds();
    return std::vector<uint32_t>(set.begin(), set.end());
}

void VirtualizedGridWidget::updateTagChipStyles()
{
    uint64_t active = m_index.activeTagMask();
    for (size_t i = 0; i < m_tagChips.size(); ++i) {
        auto *btn = m_tagChips[i];
        bool is_selected = false;
        if (i == 0) {
            is_selected = (active == 0);
        } else {
            uint64_t mask = WallpaperTags::allKnownTags()[i - 1].mask;
            is_selected = (active & mask) != 0;
        }

        btn->setChecked(is_selected);
        if (is_selected) {
            btn->setStyleSheet(
                "QPushButton { background: #0284c7; color: white; border: 1px solid #38bdf8; "
                "border-radius: 9px; padding: 1px 2px; font-size: 9px; font-weight: bold; }");
        } else {
            btn->setStyleSheet(
                "QPushButton { background: #1e222b; color: #94a3b8; border: 1px solid #334155; "
                "border-radius: 9px; padding: 1px 2px; font-size: 9px; }"
                "QPushButton:hover { background: #282e3a; color: #f1f5f9; }");
        }
    }
}

void VirtualizedGridWidget::updateStatusText()
{
    size_t filtered = m_index.filteredCount();
    size_t total = m_index.totalCount();
    double dur_us = m_index.lastFilterDurationUs();
    double dur_ms = dur_us / 1000.0;

    QString text =
        QString("%1 / %2 (%3 ms)").arg(filtered).arg(total).arg(QString::number(dur_ms, 'f', 2));
    m_statusLabel->setText(text);
}

} // namespace wwal::studio
