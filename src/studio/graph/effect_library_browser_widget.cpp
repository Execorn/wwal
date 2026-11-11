// SPDX-License-Identifier: GPL-3.0-or-later
#include "effect_library_browser_widget.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDataStream>
#include <QDrag>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPixmapCache>
#include <QPushButton>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace waywal::studio {

namespace {

enum EffectItemRoles {
    RoleEffectId = Qt::UserRole,
    RoleSlug = Qt::UserRole + 1,
    RoleDisplayName = Qt::UserRole + 2,
    RoleCategory = Qt::UserRole + 3,
    RolePassCount = Qt::UserRole + 4,
    RoleAluOps = Qt::UserRole + 5,
    RoleTexFetches = Qt::UserRole + 6,
    RoleFlags = Qt::UserRole + 7,
};

static inline QColor getCategoryColor(uint32_t category)
{
    switch (category) {
    case WE_EFFECT_CAT_MOTION_DISTORTION: // 0
        return QColor(56, 189, 248);      // #38BDF8 (Sky / Cyan)
    case WE_EFFECT_CAT_LIGHTING_POST:     // 1
        return QColor(251, 191, 36);      // #FBBF24 (Amber / Gold)
    case WE_EFFECT_CAT_COLOR_ATMOSPHERE:  // 2
        return QColor(192, 132, 252);     // #C084FC (Purple / Violet)
    case WE_EFFECT_CAT_PARTICLE_TRAIL:    // 3
        return QColor(52, 211, 153);      // #34D399 (Emerald / Mint)
    default:
        return QColor(148, 163, 184); // #94A3B8 (Slate)
    }
}

static inline QString getCategoryName(uint32_t category)
{
    switch (category) {
    case WE_EFFECT_CAT_MOTION_DISTORTION:
        return QStringLiteral("Motion & Distortion");
    case WE_EFFECT_CAT_LIGHTING_POST:
        return QStringLiteral("Lighting & Post");
    case WE_EFFECT_CAT_COLOR_ATMOSPHERE:
        return QStringLiteral("Color & Atmosphere");
    case WE_EFFECT_CAT_PARTICLE_TRAIL:
        return QStringLiteral("Particles & Trails");
    default:
        return QStringLiteral("Other");
    }
}

static QPixmap generateProceduralIcon(uint32_t category, int size = 28)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    const float s = static_cast<float>(size);
    const QColor col = getCategoryColor(category);

    switch (category) {
    case WE_EFFECT_CAT_MOTION_DISTORTION: {
        // Dual sine waves with centered ripple ring
        p.setPen(QPen(col, s * 0.08f + 0.5f, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);

        QPainterPath w1;
        w1.moveTo(s * 0.12f, s * 0.35f);
        w1.cubicTo(s * 0.35f, s * 0.12f, s * 0.65f, s * 0.58f, s * 0.88f, s * 0.35f);
        p.drawPath(w1);

        QPainterPath w2;
        w2.moveTo(s * 0.12f, s * 0.65f);
        w2.cubicTo(s * 0.35f, s * 0.42f, s * 0.65f, s * 0.88f, s * 0.88f, s * 0.65f);
        p.drawPath(w2);

        p.setPen(QPen(col, 1.2f));
        p.setBrush(col);
        p.drawEllipse(QPointF(s * 0.5f, s * 0.5f), s * 0.07f, s * 0.07f);
        break;
    }
    case WE_EFFECT_CAT_LIGHTING_POST: {
        // Radiant 8-ray sunburst flare
        const float c = s * 0.5f;
        const float r1 = s * 0.14f;
        const float r2 = s * 0.40f;
        p.setPen(QPen(col, s * 0.08f + 0.5f, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);

        // 4 cardinal rays
        p.drawLine(QPointF(c, c - r2), QPointF(c, c - r1));
        p.drawLine(QPointF(c, c + r1), QPointF(c, c + r2));
        p.drawLine(QPointF(c - r2, c), QPointF(c - r1, c));
        p.drawLine(QPointF(c + r1, c), QPointF(c + r2, c));

        // 4 diagonal rays
        const float d1 = r1 * 0.85f;
        const float d2 = r2 * 0.70f;
        p.drawLine(QPointF(c - d2, c - d2), QPointF(c - d1, c - d1));
        p.drawLine(QPointF(c + d1, c - d1), QPointF(c + d2, c - d2));
        p.drawLine(QPointF(c - d2, c + d2), QPointF(c - d1, c + d1));
        p.drawLine(QPointF(c + d1, c + d1), QPointF(c + d2, c + d2));

        // Core glow dot
        p.setBrush(col);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(c, c), r1 * 0.8f, r1 * 0.8f);
        break;
    }
    case WE_EFFECT_CAT_COLOR_ATMOSPHERE: {
        // Triple chromatic RGB / CMY circle blend
        const float r = s * 0.22f;
        const float c = s * 0.5f;
        p.setPen(Qt::NoPen);

        p.setBrush(QColor(192, 132, 252, 220)); // Violet
        p.drawEllipse(QPointF(c, c - r * 0.55f), r, r);

        p.setBrush(QColor(56, 189, 248, 190)); // Cyan
        p.drawEllipse(QPointF(c - r * 0.55f, c + r * 0.45f), r, r);

        p.setBrush(QColor(244, 114, 182, 190)); // Pink
        p.drawEllipse(QPointF(c + r * 0.55f, c + r * 0.45f), r, r);
        break;
    }
    case WE_EFFECT_CAT_PARTICLE_TRAIL: {
        // Dynamic sparkle trail with diamond star
        p.setPen(QPen(col, s * 0.08f + 0.5f, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);

        QPainterPath trail;
        trail.moveTo(s * 0.18f, s * 0.82f);
        trail.quadTo(s * 0.42f, s * 0.65f, s * 0.75f, s * 0.25f);
        p.drawPath(trail);

        // 4-point sparkle at tip
        const float sx = s * 0.75f, sy = s * 0.25f;
        const float sr = s * 0.17f;
        p.drawLine(QPointF(sx - sr, sy), QPointF(sx + sr, sy));
        p.drawLine(QPointF(sx, sy - sr), QPointF(sx, sy + sr));

        // Smaller ember dots
        p.setBrush(col);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(s * 0.45f, s * 0.56f), s * 0.06f, s * 0.06f);
        p.drawEllipse(QPointF(s * 0.24f, s * 0.76f), s * 0.045f, s * 0.045f);
        break;
    }
    default: {
        p.setBrush(col);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF(s * 0.2f, s * 0.2f, s * 0.6f, s * 0.6f), s * 0.1f, s * 0.1f);
        break;
    }
    }

    return pm;
}

static QPixmap cachedProceduralIcon(uint32_t category, int size = 28)
{
    const QString key = QStringLiteral("wwal_eff_cat_%1_%2").arg(category).arg(size);
    QPixmap pm;
    if (!QPixmapCache::find(key, &pm)) {
        pm = generateProceduralIcon(category, size);
        QPixmapCache::insert(key, pm);
    }
    return pm;
}

class EffectCardDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        const QRect r = option.rect.adjusted(6, 3, -6, -3);

        const bool isSelected = (option.state & QStyle::State_Selected);
        const bool isHovered = (option.state & QStyle::State_MouseOver);

        const uint32_t category = index.data(RoleCategory).toUInt();
        const QString displayName = index.data(RoleDisplayName).toString();
        const QString slug = index.data(RoleSlug).toString();
        const uint32_t aluOps = index.data(RoleAluOps).toUInt();
        const uint32_t passCount = index.data(RolePassCount).toUInt();
        const uint32_t flags = index.data(RoleFlags).toUInt();

        const QColor catColor = getCategoryColor(category);

        // 1. Background fill & border
        if (isSelected) {
            painter->setBrush(QColor(30, 41, 59, 240));       // #1E293B
            painter->setPen(QPen(QColor(56, 189, 248), 1.5)); // #38BDF8 focus ring
        } else if (isHovered) {
            painter->setBrush(QColor(24, 30, 42, 240));
            painter->setPen(QPen(QColor(56, 189, 248, 120), 1.0));
        } else {
            painter->setBrush(QColor(18, 21, 28, 230));     // #12151C
            painter->setPen(QPen(QColor(33, 38, 46), 1.0)); // #21262D
        }
        painter->drawRoundedRect(r, 6.0, 6.0);

        // 2. Left category vertical accent bar
        QRectF accentRect(r.left() + 2.0, r.top() + 6.0, 3.5, r.height() - 12.0);
        painter->setBrush(catColor);
        painter->setPen(Qt::NoPen);
        painter->drawRoundedRect(accentRect, 1.5, 1.5);

        // 3. Category icon (26x26)
        const int iconSize = 26;
        const int iconX = r.left() + 11;
        const int iconY = r.top() + (r.height() - iconSize) / 2;
        QPixmap iconPm = cachedProceduralIcon(category, iconSize);
        painter->drawPixmap(iconX, iconY, iconPm);

        // 4. Badges (right side)
        // ALU badge
        const QString aluText = QString::asprintf("%u ALU", aluOps);
        QFont badgeFont = painter->font();
        badgeFont.setPixelSize(10);
        badgeFont.setBold(true);
        painter->setFont(badgeFont);

        QFontMetrics fmBadge(badgeFont);
        const int aluW = fmBadge.horizontalAdvance(aluText) + 10;
        const int aluH = 16;
        const int aluX = r.right() - 8 - aluW;
        const int aluY = r.top() + 8;
        const QRect aluRect(aluX, aluY, aluW, aluH);

        QColor aluColor;
        if (aluOps < 50) {
            aluColor = QColor(52, 211, 153); // #34D399 (Green)
        } else if (aluOps <= 150) {
            aluColor = QColor(251, 191, 36); // #FBBF24 (Amber)
        } else {
            aluColor = QColor(248, 113, 113); // #F87171 (Red)
        }

        painter->setBrush(QColor(30, 41, 59));
        painter->setPen(QPen(aluColor.darker(120), 1.0));
        painter->drawRoundedRect(aluRect, 4.0, 4.0);

        painter->setPen(aluColor);
        painter->drawText(aluRect, Qt::AlignCenter, aluText);

        // Passes badge & Flag pills
        const QString passText = (passCount == 1) ? QStringLiteral("1 Pass")
                                                  : QStringLiteral("%1 Passes").arg(passCount);
        const int passW = fmBadge.horizontalAdvance(passText) + 8;
        const int passH = 15;
        const int passX = r.right() - 8 - passW;
        const int passY = r.top() + 28;
        const QRect passRect(passX, passY, passW, passH);

        painter->setBrush(QColor(24, 28, 38));
        painter->setPen(QPen(QColor(167, 139, 250, 160), 1.0)); // Violet
        painter->drawRoundedRect(passRect, 3.5, 3.5);

        painter->setPen(QColor(196, 181, 253));
        painter->drawText(passRect, Qt::AlignCenter, passText);

        int currentRightX = passX - 4;

        // Capability flag badges (FBO, DEPTH, AUDIO, MOUSE)
        auto drawFlagBadge = [&](const QString &label, const QColor &color) {
            const int fw = fmBadge.horizontalAdvance(label) + 6;
            const int fx = currentRightX - fw;
            const QRect fRect(fx, passY, fw, passH);
            painter->setBrush(QColor(color.red(), color.green(), color.blue(), 35));
            painter->setPen(QPen(color, 0.8));
            painter->drawRoundedRect(fRect, 3.0, 3.0);
            painter->setPen(color);
            painter->drawText(fRect, Qt::AlignCenter, label);
            currentRightX = fx - 4;
        };

        if (flags & WE_EFFECT_FLAG_MULTIPASS_FBO) {
            drawFlagBadge(QStringLiteral("FBO"), QColor(192, 132, 252));
        }
        if (flags & WE_EFFECT_FLAG_NEEDS_DEPTH) {
            drawFlagBadge(QStringLiteral("DEPTH"), QColor(96, 165, 250));
        }
        if (flags & WE_EFFECT_FLAG_NEEDS_AUDIO) {
            drawFlagBadge(QStringLiteral("AUDIO"), QColor(244, 114, 182));
        }
        if (flags & WE_EFFECT_FLAG_NEEDS_CURSOR) {
            drawFlagBadge(QStringLiteral("MOUSE"), QColor(251, 146, 60));
        }

        // 5. Title & Slug text
        const int textLeft = iconX + iconSize + 10;
        const int textRight = std::min(aluX - 8, currentRightX - 8);
        const int maxTextW = std::max(60, textRight - textLeft);

        // Title
        QFont titleFont = painter->font();
        titleFont.setPixelSize(12);
        titleFont.setBold(true);
        painter->setFont(titleFont);
        QFontMetrics fmTitle(titleFont);
        const QString elidedTitle = fmTitle.elidedText(displayName, Qt::ElideRight, maxTextW);

        painter->setPen(isSelected ? QColor(255, 255, 255) : QColor(241, 245, 249));
        painter->drawText(QRect(textLeft, r.top() + 8, maxTextW, 18),
                          Qt::AlignLeft | Qt::AlignVCenter, elidedTitle);

        // Slug
        QFont slugFont = painter->font();
        slugFont.setPixelSize(10);
        slugFont.setBold(false);
        painter->setFont(slugFont);
        QFontMetrics fmSlug(slugFont);
        const QString elidedSlug = fmSlug.elidedText(slug, Qt::ElideRight, maxTextW);

        painter->setPen(QColor(148, 163, 184));
        painter->drawText(QRect(textLeft, r.top() + 27, maxTextW, 16),
                          Qt::AlignLeft | Qt::AlignVCenter, elidedSlug);

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        Q_UNUSED(index);
        return QSize(option.rect.width(), 52);
    }
};

class EffectListWidget : public QListWidget {
  public:
    explicit EffectListWidget(QWidget *parent = nullptr) : QListWidget(parent)
    {
        setDragEnabled(true);
        setViewMode(QListView::ListMode);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setAutoScroll(true);
        setItemDelegate(new EffectCardDelegate(this));
    }

  protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_dragStartPos = event->pos();
        }
        QListWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!(event->buttons() & Qt::LeftButton)) {
            QListWidget::mouseMoveEvent(event);
            return;
        }

        if ((event->pos() - m_dragStartPos).manhattanLength() < QApplication::startDragDistance()) {
            QListWidget::mouseMoveEvent(event);
            return;
        }

        QListWidgetItem *item = itemAt(m_dragStartPos);
        if (!item) {
            item = currentItem();
        }
        if (!item) {
            QListWidget::mouseMoveEvent(event);
            return;
        }

        executeDrag(item, Qt::CopyAction);
    }

    void startDrag(Qt::DropActions supportedActions) override
    {
        QListWidgetItem *item = currentItem();
        if (item) {
            executeDrag(item, supportedActions);
        }
    }

  private:
    void executeDrag(QListWidgetItem *item, Qt::DropActions supportedActions = Qt::CopyAction)
    {
        const uint32_t effectId = item->data(RoleEffectId).toUInt();
        const QString slug = item->data(RoleSlug).toString();
        const QString displayName = item->data(RoleDisplayName).toString();
        const uint32_t category = item->data(RoleCategory).toUInt();

        auto *drag = new QDrag(this);
        auto *mime = new QMimeData();

        const QString payload = QStringLiteral("%1:%2").arg(effectId).arg(slug);
        const QByteArray payloadBytes = payload.toUtf8();
        mime->setData(QStringLiteral("application/x-waywal-effect-id"), payloadBytes);

        // 2. Auxiliary formats for maximum interoperability
        mime->setText(payload);
        mime->setData(QStringLiteral("application/x-waywal-effect-slug"), slug.toUtf8());
        mime->setData(QStringLiteral("application/x-waywal-effect-id-raw"),
                      QByteArray::number(effectId));

        QByteArray binaryData;
        {
            QDataStream stream(&binaryData, QIODevice::WriteOnly);
            stream << static_cast<quint32>(effectId) << slug;
        }
        mime->setData(QStringLiteral("application/x-waywal-effect-stream"), binaryData);

        drag->setMimeData(mime);

        // Visual drag pixmap card
        const int dragW = 210;
        const int dragH = 38;
        QPixmap dragPixmap(dragW, dragH);
        dragPixmap.fill(Qt::transparent);
        {
            QPainter p(&dragPixmap);
            p.setRenderHint(QPainter::Antialiasing);

            p.setBrush(QColor(18, 21, 28, 235));
            p.setPen(QPen(QColor(56, 189, 248), 1.5));
            p.drawRoundedRect(QRectF(1, 1, dragW - 2, dragH - 2), 6.0, 6.0);

            const QColor catColor = getCategoryColor(category);
            p.setBrush(catColor);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(QRectF(3, 4, 3.5, dragH - 8), 1.5, 1.5);

            QPixmap iconPm = cachedProceduralIcon(category, 22);
            p.drawPixmap(10, (dragH - 22) / 2, iconPm);

            QFont font = p.font();
            font.setPixelSize(11);
            font.setBold(true);
            p.setFont(font);
            p.setPen(QColor(241, 245, 249));
            p.drawText(QRect(36, 3, dragW - 42, 18), Qt::AlignLeft | Qt::AlignVCenter, displayName);

            font.setPixelSize(9);
            font.setBold(false);
            p.setFont(font);
            p.setPen(QColor(148, 163, 184));
            p.drawText(QRect(36, 19, dragW - 42, 15), Qt::AlignLeft | Qt::AlignVCenter, slug);
        }

        drag->setPixmap(dragPixmap);
        drag->setHotSpot(QPoint(dragW / 2, dragH / 2));

        drag->exec(supportedActions, Qt::CopyAction);
    }

    QPoint m_dragStartPos;
};

} // namespace

EffectLibraryBrowserWidget::EffectLibraryBrowserWidget(QWidget *parent) : QWidget(parent)
{
    setupUi();
    populateEffectList();
}

void EffectLibraryBrowserWidget::setupUi()
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(8, 8, 8, 8);
    rootLayout->setSpacing(6);

    // 1. Search Box
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(
        QStringLiteral("Search 50 built-in effects... (e.g. water, bloom)"));
    m_searchEdit->setClearButtonEnabled(true);
    connect(m_searchEdit, &QLineEdit::textChanged, this,
            &EffectLibraryBrowserWidget::onSearchTextChanged);
    rootLayout->addWidget(m_searchEdit);

    // 2. Category Filter Combo
    m_categoryCombo = new QComboBox(this);
    m_categoryCombo->addItem(QStringLiteral("All Categories (50)"));
    m_categoryCombo->addItem(QStringLiteral("Motion & Distortion (16)"));
    m_categoryCombo->addItem(QStringLiteral("Lighting & Post (16)"));
    m_categoryCombo->addItem(QStringLiteral("Color & Atmosphere (12)"));
    m_categoryCombo->addItem(QStringLiteral("Particles & Trails (6)"));
    connect(m_categoryCombo, &QComboBox::currentIndexChanged, this,
            &EffectLibraryBrowserWidget::onCategoryComboChanged);
    rootLayout->addWidget(m_categoryCombo);

    // 3. Card List View
    m_listWidget = new EffectListWidget(this);
    connect(m_listWidget, &QListWidget::itemDoubleClicked, this,
            &EffectLibraryBrowserWidget::onItemDoubleClicked);
    connect(m_listWidget, &QListWidget::itemSelectionChanged, this, [this]() {
        const bool hasSelection = (selectedEffectId() != WE_EFFECT_NONE);
        if (m_btnAddToGraph)
            m_btnAddToGraph->setEnabled(hasSelection);
        if (m_btnApplyToScene)
            m_btnApplyToScene->setEnabled(hasSelection);
    });
    rootLayout->addWidget(m_listWidget, 1);

    // 4. Action Buttons Bar
    auto *btnLayout = new QHBoxLayout();
    btnLayout->setContentsMargins(0, 2, 0, 0);
    btnLayout->setSpacing(6);

    m_btnAddToGraph = new QPushButton(QStringLiteral("Insert into Graph"), this);
    m_btnAddToGraph->setObjectName(QStringLiteral("btnAddToGraph"));
    m_btnAddToGraph->setToolTip(QStringLiteral(
        "Insert selected built-in effect as a node in the Shader Graph (or drag & drop)"));
    connect(m_btnAddToGraph, &QPushButton::clicked, this,
            &EffectLibraryBrowserWidget::onAddToGraphClicked);

    m_btnApplyToScene = new QPushButton(QStringLiteral("Apply to Scene"), this);
    m_btnApplyToScene->setObjectName(QStringLiteral("btnApplyToScene"));
    m_btnApplyToScene->setToolTip(
        QStringLiteral("Append selected effect to the active layer pass stack"));
    connect(m_btnApplyToScene, &QPushButton::clicked, this,
            &EffectLibraryBrowserWidget::onApplyToLayerClicked);

    btnLayout->addWidget(m_btnAddToGraph, 1);
    btnLayout->addWidget(m_btnApplyToScene, 1);
    rootLayout->addLayout(btnLayout);

    // Stylesheet matching W3C design tokens
    setStyleSheet(QStringLiteral("QWidget {"
                                 "    background-color: #0E1117;"
                                 "    color: #F1F5F9;"
                                 "}"
                                 "QLineEdit {"
                                 "    background-color: #12151C;"
                                 "    border: 1px solid #21262D;"
                                 "    border-radius: 6px;"
                                 "    padding: 6px 10px;"
                                 "    color: #F1F5F9;"
                                 "    font-size: 12px;"
                                 "    selection-background-color: #0284C7;"
                                 "}"
                                 "QLineEdit:focus {"
                                 "    border: 1.5px solid #38BDF8;"
                                 "}"
                                 "QComboBox {"
                                 "    background-color: #12151C;"
                                 "    border: 1px solid #21262D;"
                                 "    border-radius: 6px;"
                                 "    padding: 5px 10px;"
                                 "    color: #F1F5F9;"
                                 "    font-size: 12px;"
                                 "}"
                                 "QComboBox:focus {"
                                 "    border: 1.5px solid #38BDF8;"
                                 "}"
                                 "QComboBox::drop-down {"
                                 "    subcontrol-origin: padding;"
                                 "    subcontrol-position: top right;"
                                 "    width: 20px;"
                                 "    border-left: none;"
                                 "}"
                                 "QComboBox QAbstractItemView {"
                                 "    background-color: #12151C;"
                                 "    border: 1px solid #21262D;"
                                 "    color: #F1F5F9;"
                                 "    selection-background-color: #1E293B;"
                                 "    selection-color: #38BDF8;"
                                 "    padding: 4px;"
                                 "}"
                                 "QListWidget {"
                                 "    background-color: #0E1117;"
                                 "    border: 1px solid #1E232E;"
                                 "    border-radius: 6px;"
                                 "    outline: none;"
                                 "    padding: 2px;"
                                 "}"
                                 "QPushButton#btnAddToGraph {"
                                 "    background-color: #0284C7;"
                                 "    color: #FFFFFF;"
                                 "    font-weight: bold;"
                                 "    font-size: 12px;"
                                 "    border: none;"
                                 "    border-radius: 6px;"
                                 "    padding: 7px 12px;"
                                 "}"
                                 "QPushButton#btnAddToGraph:hover {"
                                 "    background-color: #0369A1;"
                                 "}"
                                 "QPushButton#btnAddToGraph:pressed {"
                                 "    background-color: #075985;"
                                 "}"
                                 "QPushButton#btnAddToGraph:disabled {"
                                 "    background-color: #1E293B;"
                                 "    color: #64748B;"
                                 "}"
                                 "QPushButton#btnApplyToScene {"
                                 "    background-color: #1E232E;"
                                 "    color: #F1F5F9;"
                                 "    font-size: 12px;"
                                 "    border: 1px solid #334155;"
                                 "    border-radius: 6px;"
                                 "    padding: 7px 12px;"
                                 "}"
                                 "QPushButton#btnApplyToScene:hover {"
                                 "    background-color: #334155;"
                                 "}"
                                 "QPushButton#btnApplyToScene:pressed {"
                                 "    background-color: #0F172A;"
                                 "}"
                                 "QPushButton#btnApplyToScene:disabled {"
                                 "    background-color: #12151C;"
                                 "    color: #475569;"
                                 "    border-color: #1E293B;"
                                 "}"));
}

void EffectLibraryBrowserWidget::populateEffectList()
{
    m_listWidget->clear();

    size_t count = 0;
    const we_builtin_effect_desc_t *table = we_builtin_effects_get_table(&count);

    if (!table || count == 0) {
        count = WE_BUILTIN_EFFECT_COUNT;
    }

    for (size_t i = 0; i < count; ++i) {
        const we_builtin_effect_desc_t *desc = nullptr;
        if (table && i < count) {
            desc = &table[i];
        } else {
            desc = we_builtin_effect_get_by_id(static_cast<we_builtin_effect_id_t>(i + 1));
        }

        if (!desc)
            continue;

        auto *item = new QListWidgetItem(m_listWidget);
        item->setData(RoleEffectId, desc->id);
        item->setData(RoleSlug, QString::fromUtf8(desc->slug));
        item->setData(RoleDisplayName, QString::fromUtf8(desc->display_name));
        item->setData(RoleCategory, desc->category);
        item->setData(RolePassCount, desc->pass_count);
        item->setData(RoleAluOps, desc->alu_ops);
        item->setData(RoleTexFetches, desc->tex_fetches);
        item->setData(RoleFlags, desc->flags);

        // Detailed rich tooltip
        QString tip = QStringLiteral("<div style='font-family: sans-serif; min-width: 220px;'>"
                                     "<b style='font-size: 13px; color: #F1F5F9;'>%1</b> "
                                     "<span style='color: #94A3B8;'>(effects/%2)</span><br/>"
                                     "<span style='color: %3;'>Category: %4</span><br/>"
                                     "<hr style='border: 0.5px solid #334155; margin: 4px 0;'/>"
                                     "<b>Complexity:</b> %5 ALU ops, %6 texture fetches<br/>"
                                     "<b>Passes:</b> %7<br/>"
                                     "<b>Default Uniforms:</b> %8 active parameters")
                          .arg(QString::fromUtf8(desc->display_name))
                          .arg(QString::fromUtf8(desc->slug))
                          .arg(getCategoryColor(desc->category).name())
                          .arg(getCategoryName(desc->category))
                          .arg(desc->alu_ops)
                          .arg(desc->tex_fetches)
                          .arg(desc->pass_count)
                          .arg(desc->param_count);

        if (desc->flags != 0) {
            QStringList flagNames;
            if (desc->flags & WE_EFFECT_FLAG_MULTIPASS_FBO)
                flagNames << QStringLiteral("Multi-Pass FBO");
            if (desc->flags & WE_EFFECT_FLAG_NEEDS_DEPTH)
                flagNames << QStringLiteral("Depth Buffer");
            if (desc->flags & WE_EFFECT_FLAG_NEEDS_AUDIO)
                flagNames << QStringLiteral("Audio Spectrum");
            if (desc->flags & WE_EFFECT_FLAG_NEEDS_CURSOR)
                flagNames << QStringLiteral("Cursor Force");
            tip +=
                QStringLiteral("<br/><b>Capabilities:</b> ") + flagNames.join(QStringLiteral(", "));
        }
        tip += QStringLiteral("</div>");
        item->setToolTip(tip);
    }

    applyFilter();
}

void EffectLibraryBrowserWidget::setFilterText(const QString &query)
{
    m_currentFilter = query.trimmed();
    if (m_searchEdit && m_searchEdit->text() != m_currentFilter) {
        m_searchEdit->blockSignals(true);
        m_searchEdit->setText(m_currentFilter);
        m_searchEdit->blockSignals(false);
    }
    applyFilter();
}

void EffectLibraryBrowserWidget::setCategoryFilter(int32_t categoryOrNegativeOneForAll)
{
    m_currentCategory = categoryOrNegativeOneForAll;
    int comboIndex = 0;
    if (categoryOrNegativeOneForAll >= 0 && categoryOrNegativeOneForAll <= 3) {
        comboIndex = categoryOrNegativeOneForAll + 1;
    }
    if (m_categoryCombo && m_categoryCombo->currentIndex() != comboIndex) {
        m_categoryCombo->blockSignals(true);
        m_categoryCombo->setCurrentIndex(comboIndex);
        m_categoryCombo->blockSignals(false);
    }
    applyFilter();
}

we_builtin_effect_id_t EffectLibraryBrowserWidget::selectedEffectId() const noexcept
{
    if (!m_listWidget)
        return WE_EFFECT_NONE;
    const QListWidgetItem *item = m_listWidget->currentItem();
    if (!item || item->isHidden())
        return WE_EFFECT_NONE;
    return static_cast<we_builtin_effect_id_t>(item->data(RoleEffectId).toUInt());
}

size_t EffectLibraryBrowserWidget::visibleEffectCount() const noexcept
{
    if (!m_listWidget)
        return 0;
    size_t count = 0;
    for (int i = 0; i < m_listWidget->count(); ++i) {
        const QListWidgetItem *item = m_listWidget->item(i);
        if (item && !item->isHidden()) {
            ++count;
        }
    }
    return count;
}

void EffectLibraryBrowserWidget::onSearchTextChanged(const QString &text)
{
    m_currentFilter = text.trimmed();
    applyFilter();
}

void EffectLibraryBrowserWidget::onCategoryComboChanged(int index)
{
    if (index <= 0) {
        m_currentCategory = -1;
    } else {
        m_currentCategory = index - 1;
    }
    applyFilter();
}

void EffectLibraryBrowserWidget::onItemDoubleClicked(QListWidgetItem *item)
{
    if (!item)
        return;
    const uint32_t rawId = item->data(RoleEffectId).toUInt();
    const auto effectId = static_cast<we_builtin_effect_id_t>(rawId);
    const QString slug = item->data(RoleSlug).toString();

    emit effectActivated(effectId, slug);
    emit effectInsertIntoGraphRequested(effectId);
    emit effectInsertRequested(rawId);
}

void EffectLibraryBrowserWidget::onAddToGraphClicked()
{
    const we_builtin_effect_id_t id = selectedEffectId();
    if (id == WE_EFFECT_NONE)
        return;

    const we_builtin_effect_desc_t *desc = we_builtin_effect_get_by_id(id);
    const QString slug = desc ? QString::fromUtf8(desc->slug) : QString();
    const uint32_t rawId = static_cast<uint32_t>(id);

    emit effectActivated(id, slug);
    emit effectInsertIntoGraphRequested(id);
    emit effectInsertRequested(rawId);
}

void EffectLibraryBrowserWidget::onApplyToLayerClicked()
{
    const we_builtin_effect_id_t id = selectedEffectId();
    if (id == WE_EFFECT_NONE)
        return;

    const we_builtin_effect_desc_t *desc = we_builtin_effect_get_by_id(id);
    const QString slug = desc ? QString::fromUtf8(desc->slug) : QString();
    const uint32_t rawId = static_cast<uint32_t>(id);

    emit effectActivated(id, slug);
    emit effectApplyToActiveSceneRequested(id);
    emit effectAppendToStackRequested(rawId);
}

void EffectLibraryBrowserWidget::applyFilter()
{
    if (!m_listWidget)
        return;

    const QString filterLower = m_currentFilter.toLower();

    for (int i = 0; i < m_listWidget->count(); ++i) {
        QListWidgetItem *item = m_listWidget->item(i);
        if (!item)
            continue;

        const int32_t cat = static_cast<int32_t>(item->data(RoleCategory).toUInt());
        const QString displayName = item->data(RoleDisplayName).toString();
        const QString slug = item->data(RoleSlug).toString();

        const bool matchCat = (m_currentCategory < 0) || (cat == m_currentCategory);
        const bool matchText = filterLower.isEmpty() ||
                               displayName.toLower().contains(filterLower) ||
                               slug.toLower().contains(filterLower);

        item->setHidden(!(matchCat && matchText));
    }

    // Ensure valid selection if current selection became hidden
    QListWidgetItem *cur = m_listWidget->currentItem();
    if (!cur || cur->isHidden()) {
        for (int i = 0; i < m_listWidget->count(); ++i) {
            QListWidgetItem *it = m_listWidget->item(i);
            if (!it->isHidden()) {
                m_listWidget->setCurrentItem(it);
                break;
            }
        }
    }

    const bool hasSelection = (selectedEffectId() != WE_EFFECT_NONE);
    if (m_btnAddToGraph)
        m_btnAddToGraph->setEnabled(hasSelection);
    if (m_btnApplyToScene)
        m_btnApplyToScene->setEnabled(hasSelection);
}

} // namespace waywal::studio
