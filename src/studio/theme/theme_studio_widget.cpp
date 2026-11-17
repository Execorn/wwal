// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/theme/theme_studio_widget.hpp"

#include "studio/inspector/oklch_color_dialog.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFont>
#include <QFontMetrics>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>

extern "C" {
#include <waywal/image_loader.h>
#include <waywal/toml_parser.h>
}

namespace waywal::studio {

namespace {

inline QString formatSwatchHex(const color_rgba_t &c)
{
    if (c.a < 255) {
        return QString::asprintf("#%02X%02X%02X%02X", c.r, c.g, c.b, c.a);
    }
    return QString::asprintf("#%02X%02X%02X", c.r, c.g, c.b);
}

inline QColor compositeSwatchOverDarkSurface(const color_rgba_t &c)
{
    constexpr double bgR = 24.0;
    constexpr double bgG = 27.0;
    constexpr double bgB = 34.0;
    const double alpha = static_cast<double>(c.a) / 255.0;
    const int r = static_cast<int>(std::lround(c.r * alpha + bgR * (1.0 - alpha)));
    const int g = static_cast<int>(std::lround(c.g * alpha + bgG * (1.0 - alpha)));
    const int b = static_cast<int>(std::lround(c.b * alpha + bgB * (1.0 - alpha)));
    return QColor(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255));
}

class SeedSwatchDot final : public QWidget {
  public:
    explicit SeedSwatchDot(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedSize(16, 16);
        setAttribute(Qt::WA_OpaquePaintEvent, false);
    }

    void setSwatchColor(const QColor &col, bool active)
    {
        if (m_color != col || m_active != active) {
            m_color = col;
            m_active = active;
            update();
        }
    }

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);
        if (m_active) {
            p.setPen(QPen(QColor(255, 255, 255, 110), 1.0));
            p.setBrush(m_color);
        } else {
            p.setPen(QPen(QColor(0x94, 0xA3, 0xB8), 1.0, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
        }
        p.drawRoundedRect(r, 3.0, 3.0);
    }

  private:
    QColor m_color;
    bool m_active{false};
};

} // namespace

// ============================================================================
// ColorSwatchChip Implementation
// ============================================================================

ColorSwatchChip::ColorSwatchChip(QWidget *parent) : QFrame(parent)
{
    setFrameShape(QFrame::NoFrame);
}

ColorSwatchChip::ColorSwatchChip(const QString &tokenName, const color_rgba_t &color,
                                 QWidget *parent)
    : QFrame(parent), m_tokenName(tokenName), m_color(color)
{
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setMinimumSize(88, 40);
    setAccessibleName(QStringLiteral("%1: %2").arg(m_tokenName, formatSwatchHex(m_color)));
    refreshUi();
}

void ColorSwatchChip::updateColor(const color_rgba_t &color)
{
    m_color = color;
    refreshUi();
}

void ColorSwatchChip::refreshUi()
{
    m_hexString = formatSwatchHex(m_color);
    setAccessibleName(QStringLiteral("%1: %2").arg(m_tokenName, m_hexString));
    setToolTip(QStringLiteral("%1: %2\nClick or press Enter/Space to copy hex")
                   .arg(m_tokenName, m_hexString));
    update();
}

void ColorSwatchChip::activateCopy(const QPoint &globalPos)
{
    QGuiApplication::clipboard()->setText(m_hexString);
    QToolTip::showText(globalPos, QStringLiteral("Copied %1 (%2)").arg(m_hexString, m_tokenName),
                       this);
    Q_EMIT copiedToClipboard(m_tokenName, m_hexString);
}

void ColorSwatchChip::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        setFocus(Qt::MouseFocusReason);
        activateCopy(event->globalPosition().toPoint());
    }
    QFrame::mousePressEvent(event);
}

void ColorSwatchChip::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
        event->key() == Qt::Key_Space) {
        activateCopy(mapToGlobal(rect().center()));
        event->accept();
        return;
    }
    QFrame::keyPressEvent(event);
}

void ColorSwatchChip::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF bounds = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);
    p.setBrush(QColor(0x18, 0x1B, 0x22));
    p.setPen(hasFocus() ? QPen(QColor(0x38, 0xBD, 0xF8), 2.0)
                        : QPen(QColor(0x26, 0x2B, 0x36), 1.0));
    p.drawRoundedRect(bounds, 5.0, 5.0);

    const QRectF swatchBox(bounds.x() + 6.0, bounds.center().y() - 10.0, 22.0, 20.0);
    p.setPen(QPen(QColor(255, 255, 255, 55), 1.0));
    p.setBrush(compositeSwatchOverDarkSurface(m_color));
    p.drawRoundedRect(swatchBox, 3.0, 3.0);

    const int textX = static_cast<int>(swatchBox.right()) + 8;
    const int textW = std::max(10, static_cast<int>(bounds.right()) - textX - 6);

    QFont nameFont = font();
    nameFont.setPixelSize(11);
    nameFont.setWeight(QFont::DemiBold);
    p.setFont(nameFont);
    p.setPen(QColor(0xF1, 0xF5, 0xF9));
    p.drawText(QRect(textX, static_cast<int>(bounds.y()) + 3, textW, 16),
               Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetrics(nameFont).elidedText(m_tokenName, Qt::ElideRight, textW));

    QFont hexFont(QStringLiteral("Monospace"));
    hexFont.setStyleHint(QFont::Monospace);
    hexFont.setPixelSize(11);
    p.setFont(hexFont);
    p.setPen(QColor(0x94, 0xA3, 0xB8));
    p.drawText(QRect(textX, static_cast<int>(bounds.y()) + 19, textW, 16),
               Qt::AlignLeft | Qt::AlignVCenter, m_hexString);
}

// ============================================================================
// ColorSwatchGridWidget Implementation
// ============================================================================

ColorSwatchGridWidget::ColorSwatchGridWidget(const QString &gridTitle, QWidget *parent)
    : ColorSwatchChip(parent), m_gridTitle(gridTitle)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    setAccessibleName(gridTitle);
    setAccessibleDescription(
        tr("Color swatch grid. Use arrow keys to navigate, Enter or Space to copy hex, "
           "and double-click or press E to edit seed color in OkLCH."));
}

void ColorSwatchGridWidget::setSwatches(const QVector<SwatchEntry> &entries)
{
    m_entries = entries;
    for (auto &e : m_entries) {
        if (e.hex.isEmpty()) {
            e.hex = formatSwatchHex(e.color);
        }
    }
    if (m_entries.isEmpty()) {
        m_focusedIndex = -1;
        m_hoveredIndex = -1;
    } else {
        m_focusedIndex = std::clamp(m_focusedIndex, 0, static_cast<int>(m_entries.size()) - 1);
    }
    updateLayoutMetrics();
    updateAccessibleFocus();
    update();
}

int ColorSwatchGridWidget::columnCountForWidth(int availWidth) const noexcept
{
    return (availWidth >= 290) ? 2 : 1;
}

QRect ColorSwatchGridWidget::tileRectForIndex(int index) const
{
    if (index < 0 || index >= m_entries.size()) {
        return {};
    }
    constexpr int kMargin = 6;
    constexpr int kGap = 6;
    const int w = std::max(100, width());
    const int cols = columnCountForWidth(w);
    const int usableW = std::max(60, w - kMargin * 2 - kGap * (cols - 1));
    const int tileW = usableW / cols;
    const int row = index / cols;
    const int col = index % cols;
    const int x = kMargin + col * (tileW + kGap);
    const int y = kMargin + row * (m_tileHeight + kGap);
    return QRect(x, y, tileW, m_tileHeight);
}

int ColorSwatchGridWidget::indexAtPos(const QPoint &pos) const noexcept
{
    for (int i = 0; i < m_entries.size(); ++i) {
        if (tileRectForIndex(i).contains(pos)) {
            return i;
        }
    }
    return -1;
}

void ColorSwatchGridWidget::updateLayoutMetrics()
{
    constexpr int kMargin = 6;
    constexpr int kGap = 6;
    const int w = std::max(120, width());
    m_columns = columnCountForWidth(w);
    const int count = static_cast<int>(m_entries.size());
    const int rows = (count > 0) ? ((count + m_columns - 1) / m_columns) : 1;
    const int neededH = kMargin * 2 + rows * m_tileHeight + std::max(0, rows - 1) * kGap;
    setMinimumHeight(neededH);
    updateGeometry();
}

QSize ColorSwatchGridWidget::sizeHint() const
{
    constexpr int kMargin = 6;
    constexpr int kGap = 6;
    const int w = std::max(200, width());
    const int cols = columnCountForWidth(w);
    const int count = static_cast<int>(m_entries.size());
    const int rows = (count > 0) ? ((count + cols - 1) / cols) : 1;
    return {240, kMargin * 2 + rows * m_tileHeight + std::max(0, rows - 1) * kGap};
}

QSize ColorSwatchGridWidget::minimumSizeHint() const { return {140, 64}; }

void ColorSwatchGridWidget::ensureFocusedVisible()
{
    if (m_focusedIndex < 0 || m_focusedIndex >= m_entries.size()) {
        return;
    }
    const QRect r = tileRectForIndex(m_focusedIndex);
    QWidget *p = parentWidget();
    while (p) {
        if (auto *scroll = qobject_cast<QScrollArea *>(p)) {
            scroll->ensureVisible(r.center().x(), r.center().y(), 12, 16);
            break;
        }
        p = p->parentWidget();
    }
}

void ColorSwatchGridWidget::updateAccessibleFocus()
{
    if (m_focusedIndex >= 0 && m_focusedIndex < m_entries.size()) {
        const auto &e = m_entries[m_focusedIndex];
        setAccessibleName(QStringLiteral("%1 - %2: %3").arg(m_gridTitle, e.name, e.hex));
    } else {
        setAccessibleName(m_gridTitle);
    }
}

void ColorSwatchGridWidget::triggerCopyAt(int index, const QPoint &globalPos)
{
    if (index < 0 || index >= m_entries.size()) {
        return;
    }
    const auto &e = m_entries[index];
    QGuiApplication::clipboard()->setText(e.hex);
    QToolTip::showText(globalPos, QStringLiteral("Copied %1 (%2)").arg(e.hex, e.name), this);
    Q_EMIT copiedToClipboard(e.name, e.hex);
}

void ColorSwatchGridWidget::paintEvent(QPaintEvent *event)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const QRect dirtyRect = event ? event->rect() : rect();

    QFont nameFont = font();
    nameFont.setPixelSize(12);
    nameFont.setWeight(QFont::DemiBold);
    const QFontMetrics nameFm(nameFont);

    QFont hexFont(QStringLiteral("Monospace"));
    hexFont.setStyleHint(QFont::Monospace);
    hexFont.setPixelSize(11);
    const QFontMetrics hexFm(hexFont);

    for (int i = 0; i < m_entries.size(); ++i) {
        const QRect tile = tileRectForIndex(i);
        if (!tile.intersects(dirtyRect)) {
            continue;
        }

        const auto &e = m_entries[i];
        const bool isHovered = (i == m_hoveredIndex);
        const bool isFocused = (hasFocus() && i == m_focusedIndex);

        const QRectF tileF = QRectF(tile).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setBrush(isHovered ? QColor(0x20, 0x24, 0x2E) : QColor(0x18, 0x1B, 0x22));
        if (isFocused) {
            p.setPen(QPen(QColor(0x38, 0xBD, 0xF8), 2.0));
        } else if (isHovered) {
            p.setPen(QPen(QColor(0x02, 0x84, 0xC7), 1.2));
        } else {
            p.setPen(QPen(QColor(0x26, 0x2B, 0x36), 1.0));
        }
        p.drawRoundedRect(tileF, 6.0, 6.0);

        const int swatchSize = 28;
        const QRectF swatchBox(tile.x() + 8, tile.y() + (tile.height() - swatchSize) / 2,
                               swatchSize, swatchSize);
        if (e.color.a < 255) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0x2D, 0x33, 0x40));
            p.drawRoundedRect(swatchBox, 4.0, 4.0);
        }
        p.setPen(QPen(QColor(255, 255, 255, 65), 1.0));
        p.setBrush(compositeSwatchOverDarkSurface(e.color));
        p.drawRoundedRect(swatchBox, 4.0, 4.0);

        const int textX = static_cast<int>(swatchBox.right()) + 8;
        const int textW = std::max(12, tile.right() - textX - 8);
        const int lineH = std::max(16, (tile.height() - 10) / 2);

        p.setFont(nameFont);
        p.setPen(QColor(0xF1, 0xF5, 0xF9));
        p.drawText(QRect(textX, tile.y() + 5, textW, lineH), Qt::AlignLeft | Qt::AlignVCenter,
                   nameFm.elidedText(e.name, Qt::ElideRight, textW));

        p.setFont(hexFont);
        p.setPen(QColor(0x94, 0xA3, 0xB8));
        p.drawText(QRect(textX, tile.y() + 5 + lineH, textW, lineH),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   hexFm.elidedText(e.hex, Qt::ElideRight, textW));
    }
}

void ColorSwatchGridWidget::resizeEvent(QResizeEvent *event)
{
    ColorSwatchChip::resizeEvent(event);
    updateLayoutMetrics();
}

void ColorSwatchGridWidget::mouseMoveEvent(QMouseEvent *event)
{
    const int idx = indexAtPos(event->position().toPoint());
    if (idx != m_hoveredIndex) {
        m_hoveredIndex = idx;
        update();
    }
}

void ColorSwatchGridWidget::leaveEvent(QEvent *event)
{
    ColorSwatchChip::leaveEvent(event);
    if (m_hoveredIndex != -1) {
        m_hoveredIndex = -1;
        update();
    }
}

void ColorSwatchGridWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        setFocus(Qt::MouseFocusReason);
        const int idx = indexAtPos(event->position().toPoint());
        if (idx >= 0) {
            m_focusedIndex = idx;
            updateAccessibleFocus();
            update();
            triggerCopyAt(idx, event->globalPosition().toPoint());
            event->accept();
            return;
        }
    }
    QFrame::mousePressEvent(event);
}

void ColorSwatchGridWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        const int idx = indexAtPos(event->position().toPoint());
        if (idx >= 0 && idx < m_entries.size()) {
            const auto &e = m_entries[idx];
            Q_EMIT swatchEditRequested(e.name, e.color);
            event->accept();
            return;
        }
    }
    QFrame::mouseDoubleClickEvent(event);
}

void ColorSwatchGridWidget::keyPressEvent(QKeyEvent *event)
{
    if (m_entries.isEmpty()) {
        QFrame::keyPressEvent(event);
        return;
    }
    if (m_focusedIndex < 0 || m_focusedIndex >= m_entries.size()) {
        m_focusedIndex = 0;
    }

    const int count = static_cast<int>(m_entries.size());
    const int cols = std::max(1, columnCountForWidth(width()));
    int next = m_focusedIndex;

    switch (event->key()) {
    case Qt::Key_Left:
        next = std::max(0, m_focusedIndex - 1);
        break;
    case Qt::Key_Right:
        next = std::min(count - 1, m_focusedIndex + 1);
        break;
    case Qt::Key_Up:
        if (m_focusedIndex - cols >= 0) {
            next = m_focusedIndex - cols;
        }
        break;
    case Qt::Key_Down:
        if (m_focusedIndex + cols < count) {
            next = m_focusedIndex + cols;
        }
        break;
    case Qt::Key_Home:
        next = 0;
        break;
    case Qt::Key_End:
        next = count - 1;
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        if ((event->modifiers() & Qt::ShiftModifier) != 0) {
            const auto &e = m_entries[m_focusedIndex];
            Q_EMIT swatchEditRequested(e.name, e.color);
        } else {
            triggerCopyAt(m_focusedIndex, mapToGlobal(tileRectForIndex(m_focusedIndex).center()));
        }
        event->accept();
        return;
    case Qt::Key_E:
    case Qt::Key_F2: {
        const auto &e = m_entries[m_focusedIndex];
        Q_EMIT swatchEditRequested(e.name, e.color);
        event->accept();
        return;
    }
    default:
        QFrame::keyPressEvent(event);
        return;
    }

    if (next != m_focusedIndex) {
        m_focusedIndex = next;
        updateAccessibleFocus();
        ensureFocusedVisible();
        update();
    }
    event->accept();
}

void ColorSwatchGridWidget::focusInEvent(QFocusEvent *event)
{
    ColorSwatchChip::focusInEvent(event);
    if (m_focusedIndex < 0 && !m_entries.isEmpty()) {
        m_focusedIndex = 0;
    }
    updateAccessibleFocus();
    update();
}

void ColorSwatchGridWidget::focusOutEvent(QFocusEvent *event)
{
    ColorSwatchChip::focusOutEvent(event);
    update();
}

bool ColorSwatchGridWidget::event(QEvent *event)
{
    if (event && event->type() == QEvent::ToolTip) {
        auto *he = static_cast<QHelpEvent *>(event);
        const int idx = indexAtPos(he->pos());
        if (idx >= 0 && idx < m_entries.size()) {
            const auto &e = m_entries[idx];
            QToolTip::showText(
                he->globalPos(),
                QStringLiteral(
                    "%1: %2\nClick to copy hex; double-click or press E to edit in OkLCH")
                    .arg(e.name, e.hex),
                this, tileRectForIndex(idx));
        } else {
            QToolTip::hideText();
            event->ignore();
        }
        return true;
    }
    return ColorSwatchChip::event(event);
}

class WcagStatusPill final : public QWidget {
  public:
    WcagStatusPill(const QString &text, const QSize &minSz, QWidget *parent = nullptr)
        : QWidget(parent), m_text(text), m_minSize(minSz)
    {
        setMinimumSize(m_minSize);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    void setColors(const QColor &bg, const QColor &fg)
    {
        if (m_bg != bg || m_fg != fg) {
            m_bg = bg;
            m_fg = fg;
            update();
        }
    }

    [[nodiscard]] QSize sizeHint() const override { return m_minSize; }
    [[nodiscard]] QSize minimumSizeHint() const override { return m_minSize; }

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(m_bg);
        p.drawRoundedRect(rect(), 3.0, 3.0);

        QFont f = font();
        f.setPixelSize(11);
        f.setBold(true);
        p.setFont(f);
        p.setPen(m_fg);
        p.drawText(rect(), Qt::AlignCenter, m_text);
    }

  private:
    QString m_text;
    QSize m_minSize;
    QColor m_bg{0x33, 0x41, 0x55};
    QColor m_fg{0xF1, 0xF5, 0xF9};
};

// ============================================================================
// WcagBadgeWidget Implementation
// ============================================================================

WcagBadgeWidget::WcagBadgeWidget(const QString &pairName, QWidget *parent)
    : QFrame(parent), m_pairName(pairName)
{
    setFrameShape(QFrame::StyledPanel);
    setToolTip(pairName);
    setMinimumHeight(30);
    setStyleSheet(
        QStringLiteral("WcagBadgeWidget { background-color: #181b22; border: 1px solid #262b36; "
                       "border-radius: 5px; padding: 0px; }"));

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 3, 6, 3);
    layout->setSpacing(4);

    QString shortLabel = pairName;
    const int slashIdx = pairName.indexOf(QLatin1Char('/'));
    if (slashIdx >= 0) {
        shortLabel = pairName.mid(slashIdx + 1).trimmed();
        shortLabel.replace(QLatin1String("primaryContainer"), QLatin1String("priCont"));
        shortLabel.replace(QLatin1String("secondaryContainer"), QLatin1String("secCont"));
        shortLabel.replace(QLatin1String("tertiaryContainer"), QLatin1String("terCont"));
        shortLabel.replace(QLatin1String("successContainer"), QLatin1String("sucCont"));
        shortLabel.replace(QLatin1String("surfaceVariant"), QLatin1String("surfVar"));
        shortLabel.replace(QLatin1String("background"), QLatin1String("bg"));
        shortLabel.replace(QLatin1String("secondary"), QLatin1String("second"));
        shortLabel.replace(QLatin1String("tertiary"), QLatin1String("tert"));
    }

    m_nameLabel = new QLabel(shortLabel, this);
    m_nameLabel->setMinimumWidth(20);
    m_nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_nameLabel->setStyleSheet(
        QStringLiteral("font-size: 11px; font-weight: 600; color: #f1f5f9; border: none;"));
    layout->addWidget(m_nameLabel, 1);

    m_ratioLabel = new QLabel(QStringLiteral("0.0:1"), this);
    m_ratioLabel->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 11px; "
                                               "font-weight: bold; color: #f1f5f9; border: none;"));
    layout->addWidget(m_ratioLabel, 0);

    m_aaBadge = new WcagStatusPill(QStringLiteral("AA"), QSize(28, 20), this);
    layout->addWidget(m_aaBadge, 0);

    m_aaaBadge = new WcagStatusPill(QStringLiteral("AAA"), QSize(34, 20), this);
    layout->addWidget(m_aaaBadge, 0);
}

color_rgba_t WcagBadgeWidget::compositeOver(const color_rgba_t &fg, const color_rgba_t &bg)
{
    if (fg.a == 255) {
        return fg;
    }
    const double alpha = static_cast<double>(fg.a) / 255.0;
    const double inv = 1.0 - alpha;
    color_rgba_t out{};
    out.r = static_cast<uint8_t>(
        std::clamp(static_cast<int>(std::lround(fg.r * alpha + bg.r * inv)), 0, 255));
    out.g = static_cast<uint8_t>(
        std::clamp(static_cast<int>(std::lround(fg.g * alpha + bg.g * inv)), 0, 255));
    out.b = static_cast<uint8_t>(
        std::clamp(static_cast<int>(std::lround(fg.b * alpha + bg.b * inv)), 0, 255));
    out.a = 255;
    return out;
}

double WcagBadgeWidget::relativeLuminance(const color_rgba_t &c)
{
    const auto toLinear = [](double channel) -> double {
        if (channel <= 0.04045) {
            return channel / 12.92;
        }
        return std::pow((channel + 0.055) / 1.055, 2.4);
    };

    const double r = toLinear(c.r / 255.0);
    const double g = toLinear(c.g / 255.0);
    const double b = toLinear(c.b / 255.0);
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

double WcagBadgeWidget::computeContrastRatio(const color_rgba_t &fg, const color_rgba_t &bg)
{
    constexpr color_rgba_t kBackdrop{{17, 24, 39, 255}};
    const color_rgba_t opaqueBg = compositeOver(bg, kBackdrop);
    const color_rgba_t opaqueFg = compositeOver(fg, opaqueBg);

    const double y1 = relativeLuminance(opaqueFg);
    const double y2 = relativeLuminance(opaqueBg);
    const double lighter = std::max(y1, y2);
    const double darker = std::min(y1, y2);
    return (lighter + 0.05) / (darker + 0.05);
}

void WcagBadgeWidget::updateRatio(const color_rgba_t &fg, const color_rgba_t &bg)
{
    const double ratio = computeContrastRatio(fg, bg);
    m_ratioLabel->setText(QString::asprintf("%.1f:1", ratio));

    const int passAa = (ratio >= 4.5) ? 1 : 0;
    const int passAaa = (ratio >= 7.0) ? 1 : 0;

    if (m_lastPassAa != passAa) {
        m_lastPassAa = passAa;
        m_aaBadge->setColors(passAa ? QColor(0x15, 0x80, 0x3D) : QColor(0xB9, 0x1C, 0x1C),
                             QColor(0xFF, 0xFF, 0xFF));
        m_aaBadge->setToolTip(
            passAa ? QStringLiteral("%1 - WCAG 2.1 AA (4.5:1) Pass").arg(m_pairName)
                   : QStringLiteral("%1 - WCAG 2.1 AA (4.5:1) Fail").arg(m_pairName));
    }

    if (m_lastPassAaa != passAaa) {
        m_lastPassAaa = passAaa;
        m_aaaBadge->setColors(passAaa ? QColor(0x15, 0x80, 0x3D) : QColor(0x33, 0x41, 0x55),
                              passAaa ? QColor(0xFF, 0xFF, 0xFF) : QColor(0xF1, 0xF5, 0xF9));
        m_aaaBadge->setToolTip(
            passAaa ? QStringLiteral("%1 - WCAG 2.1 AAA (7.0:1) Pass").arg(m_pairName)
                    : QStringLiteral("%1 - WCAG 2.1 AAA (7.0:1) Not met").arg(m_pairName));
    }
}

// ============================================================================
// ThemeStudioWidget Implementation
// ============================================================================

ThemeStudioWidget::ThemeStudioWidget(QWidget *parent)
    : QWidget(parent), m_solarTimer(this), m_toastTimer(this)
{
    setMinimumWidth(210);
    setStyleSheet(QStringLiteral(
        "ThemeStudioWidget { color: #f1f5f9; font-size: 12px; }"
        "ThemeStudioWidget QLabel { color: #f1f5f9; font-size: 11px; }"
        "ThemeStudioWidget QGroupBox { border: 1px solid #262b36; border-radius: 6px; "
        "margin-top: 10px; padding-top: 8px; font-size: 11px; font-weight: 600; color: #94a3b8; } "
        "ThemeStudioWidget QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top "
        "left; left: 8px; top: 0px; padding: 0 4px; }"
        "ThemeStudioWidget QPushButton { padding: 4px 8px; font-size: 11px; color: #f1f5f9; "
        "background-color: #20242e; border: 1px solid #2c3240; border-radius: 5px; }"
        "ThemeStudioWidget QPushButton:checked { background-color: #0284c7; border-color: #38bdf8; "
        "color: #ffffff; font-weight: 600; }"
        "ThemeStudioWidget QPushButton:hover { border-color: #38bdf8; }"
        "ThemeStudioWidget QPushButton:focus, ThemeStudioWidget QComboBox:focus, "
        "ThemeStudioWidget QCheckBox:focus, ThemeStudioWidget QRadioButton:focus, "
        "ThemeStudioWidget QSlider:focus, ThemeStudioWidget QTabBar::tab:focus { "
        "border: 2px solid #38bdf8; outline: none; }"
        "ThemeStudioWidget QComboBox { font-size: 11px; color: #f1f5f9; padding: 2px 6px; }"
        "ThemeStudioWidget QCheckBox { font-size: 11px; color: #f1f5f9; spacing: 5px; }"
        "ThemeStudioWidget QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid "
        "#475569; border-radius: 3px; background: #181b22; }"
        "ThemeStudioWidget QCheckBox::indicator:checked { background: #0284c7; "
        "border-color: #38bdf8; }"
        "ThemeStudioWidget QRadioButton { font-size: 11px; color: #f1f5f9; spacing: 3px; }"
        "ThemeStudioWidget QRadioButton::indicator { width: 13px; height: 13px; }"
        "ThemeStudioWidget QTabBar::tab { padding: 5px 9px; font-size: 11px; color: #94a3b8; "
        "min-height: 28px; }"
        "ThemeStudioWidget QTabBar::tab:selected { color: #f1f5f9; font-weight: 600; }"));

    setupUi();

    m_solarTimer.setObjectName(QStringLiteral("themeSolarTimer"));
    connect(&m_solarTimer, &QTimer::timeout, this, &ThemeStudioWidget::onSolarTimerTick);

    m_toastTimer.setObjectName(QStringLiteral("themeToastTimer"));
    m_toastTimer.setSingleShot(true);
    connect(&m_toastTimer, &QTimer::timeout, this, [this]() {
        if (m_clipboardToastLabel) {
            m_clipboardToastLabel->clear();
        }
    });

    reextract();
}

void ThemeStudioWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_mainSplitter) {
        const Qt::Orientation target = (width() >= 540) ? Qt::Horizontal : Qt::Vertical;
        const bool orientationChanged = (m_mainSplitter->orientation() != target);
        if (orientationChanged) {
            m_mainSplitter->setOrientation(target);
        }
        const bool userMoved = m_mainSplitter->property("wwal_user_moved").toBool();
        if (!userMoved || orientationChanged) {
            if (target == Qt::Vertical && height() > 240) {
                const int availH = std::max(240, height() - 32);
                const int bottomH = (availH >= 560) ? 300 : 140;
                const int topH = std::max(260, availH - bottomH - 4);
                m_mainSplitter->setSizes({topH, std::max(96, availH - topH)});
            } else if (target == Qt::Horizontal && width() > 300) {
                const int leftW = static_cast<int>(width() * 0.54);
                m_mainSplitter->setSizes({leftW, width() - leftW});
            }
        }
    }
}

void ThemeStudioWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_autoSolarSchedule) {
        applySolarScheduleEvaluation();
        if (!m_solarTimer.isActive()) {
            m_solarTimer.start(60000);
        }
    }
}

void ThemeStudioWidget::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    m_solarTimer.stop();
}

QColor ThemeStudioWidget::toQColor(const color_rgba_t &c) { return QColor(c.r, c.g, c.b, c.a); }

void ThemeStudioWidget::setupUi()
{
    auto *topLayout = new QVBoxLayout(this);
    topLayout->setContentsMargins(8, 6, 8, 6);
    topLayout->setSpacing(6);

    // Split header across two rows so 220px narrow docks keep >= 28px touch targets unclipped.
    auto *headerTopRow = new QHBoxLayout();
    headerTopRow->setContentsMargins(0, 0, 0, 0);
    headerTopRow->setSpacing(6);

    auto *titleLabel = new QLabel(QStringLiteral("<b>Color Palette</b>"), this);
    titleLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #f1f5f9;"));
    titleLabel->setMinimumWidth(60);
    titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    headerTopRow->addWidget(titleLabel, 1);

    m_perfLabel = new QLabel(QStringLiteral("Ready"), this);
    m_perfLabel->setToolTip(QStringLiteral("Palette extraction time"));
    m_perfLabel->setStyleSheet(
        QStringLiteral("color: #94a3b8; font-size: 11px; font-family: monospace;"));
    headerTopRow->addWidget(m_perfLabel, 0);

    topLayout->addLayout(headerTopRow);

    auto *headerActionRow = new QHBoxLayout();
    headerActionRow->setContentsMargins(0, 0, 0, 0);
    headerActionRow->setSpacing(6);

    m_toggleLockBtn = new QPushButton(QStringLiteral("Lockscreen"), this);
    m_toggleLockBtn->setCheckable(true);
    m_toggleLockBtn->setChecked(false);
    m_toggleLockBtn->setToolTip(QStringLiteral("Toggle lockscreen blur preview"));
    m_toggleLockBtn->setMinimumHeight(28);
    headerActionRow->addWidget(m_toggleLockBtn, 0);

    auto *loadWallBtn = new QPushButton(QStringLiteral("Open Image..."), this);
    loadWallBtn->setToolTip(QStringLiteral("Load an image to extract its color palette"));
    loadWallBtn->setMinimumHeight(28);
    loadWallBtn->setStyleSheet(QStringLiteral("font-weight: 600;"));
    connect(loadWallBtn, &QPushButton::clicked, this, &ThemeStudioWidget::openWallpaperDialog);
    headerActionRow->addWidget(loadWallBtn, 0);

    m_clipboardToastLabel = new QLabel(this);
    m_clipboardToastLabel->setStyleSheet(
        QStringLiteral("color: #38bdf8; font-size: 11px; font-weight: 600;"));
    m_clipboardToastLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    headerActionRow->addWidget(m_clipboardToastLabel, 1);

    topLayout->addLayout(headerActionRow);

    m_mainSplitter = new QSplitter(Qt::Horizontal, this);
    m_mainSplitter->setChildrenCollapsible(false);
    connect(m_mainSplitter, &QSplitter::splitterMoved, this, [this](int, int) {
        if (m_mainSplitter) {
            m_mainSplitter->setProperty("wwal_user_moved", true);
        }
    });

    auto *leftScroll = new QScrollArea(m_mainSplitter);
    leftScroll->setWidgetResizable(true);
    leftScroll->setFrameShape(QFrame::NoFrame);
    leftScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    leftScroll->setStyleSheet(
        QStringLiteral("QScrollArea { background: transparent; border: none; }"));

    auto *leftColWidget = new QWidget(leftScroll);
    auto *leftColLayout = new QVBoxLayout(leftColWidget);
    leftColLayout->setContentsMargins(0, 0, 4, 0);
    leftColLayout->setSpacing(6);

    setupControlsCard(leftColLayout);

    auto *previewCard = new QGroupBox(QStringLiteral("Desktop Preview"), leftColWidget);
    auto *previewLayout = new QVBoxLayout(previewCard);
    previewLayout->setContentsMargins(6, 6, 6, 6);
    previewLayout->setSpacing(4);

    m_mockup = new DesktopPreviewMockup(previewCard);
    m_mockup->setMinimumSize(170, 120);
    previewLayout->addWidget(m_mockup, 1);
    connect(m_toggleLockBtn, &QPushButton::toggled, m_mockup,
            &DesktopPreviewMockup::setLockscreenPreview);
    connect(m_mockup, &DesktopPreviewMockup::lockscreenPreviewChanged, this, [this](bool enabled) {
        if (m_toggleLockBtn && m_toggleLockBtn->isChecked() != enabled) {
            const QSignalBlocker blocker(m_toggleLockBtn);
            m_toggleLockBtn->setChecked(enabled);
        }
    });

    leftColLayout->addWidget(previewCard, 1);
    leftScroll->setWidget(leftColWidget);
    m_mainSplitter->addWidget(leftScroll);

    auto *rightColWidget = new QWidget(m_mainSplitter);
    auto *rightColLayout = new QVBoxLayout(rightColWidget);
    rightColLayout->setContentsMargins(0, 0, 0, 0);
    rightColLayout->setSpacing(4);

    auto *tabWidget = new QTabWidget(rightColWidget);
    setupSwatchesCard(tabWidget);

    rightColLayout->addWidget(tabWidget, 1);
    m_mainSplitter->addWidget(rightColWidget);

    m_mainSplitter->setStretchFactor(0, 6);
    m_mainSplitter->setStretchFactor(1, 4);

    topLayout->addWidget(m_mainSplitter, 1);
}

void ThemeStudioWidget::setupControlsCard(QVBoxLayout *parentLayout)
{
    auto *card = new QGroupBox(QStringLiteral("Theme Options"), this);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    // Row 1a: Exclusive Dark/Light mode buttons
    auto *modeRow = new QHBoxLayout();
    modeRow->setContentsMargins(0, 0, 0, 0);
    modeRow->setSpacing(6);

    auto *modeLabel = new QLabel(QStringLiteral("Mode:"), card);
    modeLabel->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 11px; color: #94a3b8;"));
    modeRow->addWidget(modeLabel);

    m_modeGroup = new QButtonGroup(this);
    m_modeGroup->setExclusive(true);

    m_modeDarkBtn = new QPushButton(QStringLiteral("Dark"), card);
    m_modeDarkBtn->setCheckable(true);
    m_modeDarkBtn->setChecked(true);
    m_modeDarkBtn->setMinimumHeight(28);
    m_modeGroup->addButton(m_modeDarkBtn, static_cast<int>(WAYWAL_THEME_DARK));
    modeRow->addWidget(m_modeDarkBtn);

    m_modeLightBtn = new QPushButton(QStringLiteral("Light"), card);
    m_modeLightBtn->setCheckable(true);
    m_modeLightBtn->setChecked(false);
    m_modeLightBtn->setMinimumHeight(28);
    m_modeGroup->addButton(m_modeLightBtn, static_cast<int>(WAYWAL_THEME_LIGHT));
    modeRow->addWidget(m_modeLightBtn);

    connect(m_modeGroup, &QButtonGroup::idClicked, this,
            [this](int id) { setThemeMode(static_cast<waywal_theme_mode_t>(id)); });

    modeRow->addStretch(1);
    layout->addLayout(modeRow);

    // Row 1b: Solar schedule toggle and status badge
    auto *solarRow = new QHBoxLayout();
    solarRow->setContentsMargins(0, 0, 0, 0);
    solarRow->setSpacing(6);

    m_autoSolarCheck = new QCheckBox(QStringLiteral("Solar"), card);
    m_autoSolarCheck->setMinimumHeight(28);
    m_autoSolarCheck->setToolTip(
        QStringLiteral("Switch between Dark and Light mode based on solar elevation"));
    connect(m_autoSolarCheck, &QCheckBox::toggled, this, &ThemeStudioWidget::setAutoSolarSchedule);
    solarRow->addWidget(m_autoSolarCheck);

    m_solarPhaseBadge = new QLabel(QStringLiteral("Manual"), card);
    m_solarPhaseBadge->setStyleSheet(
        QStringLiteral("color: #94a3b8; font-size: 11px; font-family: monospace;"));
    m_solarPhaseBadge->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    solarRow->addWidget(m_solarPhaseBadge, 1);

    layout->addLayout(solarRow);

    // Row 2: M3 Variant Combo
    auto *row2 = new QHBoxLayout();
    row2->setContentsMargins(0, 0, 0, 0);
    row2->setSpacing(6);

    auto *varLabel = new QLabel(QStringLiteral("Scheme:"), card);
    varLabel->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 11px; color: #94a3b8;"));
    row2->addWidget(varLabel);

    m_variantCombo = new QComboBox(card);
    m_variantCombo->setMinimumHeight(28);
    m_variantCombo->setMinimumWidth(80);
    m_variantCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_variantCombo->addItem(QStringLiteral("Tonal Spot (Balanced)"),
                            static_cast<int>(WAYWAL_SCHEME_TONAL_SPOT));
    m_variantCombo->addItem(QStringLiteral("Vibrant (High chroma)"),
                            static_cast<int>(WAYWAL_SCHEME_VIBRANT));
    m_variantCombo->addItem(QStringLiteral("Expressive (Shifted)"),
                            static_cast<int>(WAYWAL_SCHEME_EXPRESSIVE));
    m_variantCombo->addItem(QStringLiteral("Content (Source)"),
                            static_cast<int>(WAYWAL_SCHEME_CONTENT));
    m_variantCombo->addItem(QStringLiteral("Fidelity (Exact)"),
                            static_cast<int>(WAYWAL_SCHEME_FIDELITY));
    m_variantCombo->addItem(QStringLiteral("Neutral (Low chroma)"),
                            static_cast<int>(WAYWAL_SCHEME_NEUTRAL));
    m_variantCombo->addItem(QStringLiteral("Monochrome (Grey)"),
                            static_cast<int>(WAYWAL_SCHEME_MONOCHROME));
    m_variantCombo->addItem(QStringLiteral("Rainbow (Multi-hue)"),
                            static_cast<int>(WAYWAL_SCHEME_RAINBOW));
    m_variantCombo->addItem(QStringLiteral("Fruit Salad (Duo)"),
                            static_cast<int>(WAYWAL_SCHEME_FRUIT_SALAD));
    m_variantCombo->setCurrentIndex(0);
    connect(m_variantCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (idx >= 0) {
            setSchemeVariant(
                static_cast<waywal_scheme_variant_t>(m_variantCombo->itemData(idx).toInt()));
        }
    });
    row2->addWidget(m_variantCombo, 1);

    layout->addLayout(row2);

    // Row 2b: Contrast label + slider + Reset
    m_contrastValLabel = new QLabel(QStringLiteral("Contrast: +0.00"), card);
    m_contrastValLabel->setStyleSheet(QStringLiteral(
        "font-weight: 600; font-size: 11px; font-family: monospace; color: #94a3b8;"));
    layout->addWidget(m_contrastValLabel);

    auto *row2b = new QHBoxLayout();
    row2b->setContentsMargins(0, 0, 0, 0);
    row2b->setSpacing(6);

    m_contrastSlider = new QSlider(Qt::Horizontal, card);
    m_contrastSlider->setAccessibleName(QStringLiteral("Theme contrast slider"));
    m_contrastSlider->setMinimumHeight(28);
    m_contrastSlider->setRange(-100, 100);
    m_contrastSlider->setValue(0);
    m_contrastSlider->setSingleStep(5);
    m_contrastSlider->setPageStep(25);
    m_contrastSlider->setMinimumWidth(50);
    connect(m_contrastSlider, &QSlider::valueChanged, this,
            [this](int val) { setContrast(static_cast<float>(val) / 100.0f); });
    row2b->addWidget(m_contrastSlider, 1);

    m_resetContrastBtn = new QPushButton(QStringLiteral("Reset"), card);
    m_resetContrastBtn->setToolTip(QStringLiteral("Reset contrast to 0.0"));
    m_resetContrastBtn->setMinimumSize(52, 28);
    connect(m_resetContrastBtn, &QPushButton::clicked, this,
            [this]() { m_contrastSlider->setValue(0); });
    row2b->addWidget(m_resetContrastBtn);

    layout->addLayout(row2b);

    // Row 3a: Ranked Seeds 1..4
    auto *seedRow = new QHBoxLayout();
    seedRow->setContentsMargins(0, 0, 0, 0);
    seedRow->setSpacing(3);

    auto *seedLabel = new QLabel(QStringLiteral("Seed:"), card);
    seedLabel->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 11px; color: #94a3b8;"));
    seedRow->addWidget(seedLabel);

    m_seedGroup = new QButtonGroup(this);
    m_seedRadios.resize(4);
    m_seedSwatches.resize(4);

    const QString seedShortNames[4] = {QStringLiteral("1"), QStringLiteral("2"),
                                       QStringLiteral("3"), QStringLiteral("4")};
    const QString seedTooltips[4] = {
        QStringLiteral("Primary wallpaper accent"), QStringLiteral("Secondary wallpaper accent"),
        QStringLiteral("Tertiary wallpaper accent"), QStringLiteral("Neutral wallpaper accent")};

    for (int i = 0; i < 4; ++i) {
        auto *radio = new QRadioButton(seedShortNames[i], card);
        radio->setMinimumHeight(28);
        radio->setAccessibleName(seedTooltips[i]);
        radio->setToolTip(seedTooltips[i]);
        if (i == 0) {
            radio->setChecked(true);
        }
        m_seedGroup->addButton(radio, i);
        m_seedRadios[i] = radio;

        auto *swatch = new SeedSwatchDot(card);
        swatch->setToolTip(seedTooltips[i]);
        m_seedSwatches[i] = swatch;

        seedRow->addWidget(radio);
        seedRow->addWidget(swatch);
        seedRow->addSpacing(2);
    }

    connect(m_seedGroup, &QButtonGroup::idClicked, this,
            [this](int id) { setSeedIndex(static_cast<uint8_t>(id)); });

    seedRow->addStretch(1);
    layout->addLayout(seedRow);

    // Row 3b: Custom Seed Override
    auto *customRow = new QHBoxLayout();
    customRow->setContentsMargins(0, 0, 0, 0);
    customRow->setSpacing(6);

    m_customSeedBtn = new QPushButton(QStringLiteral("Pick Color..."), card);
    m_customSeedBtn->setToolTip(QStringLiteral("Choose a custom seed color"));
    m_customSeedBtn->setMinimumHeight(28);
    connect(m_customSeedBtn, &QPushButton::clicked, this, &ThemeStudioWidget::openCustomSeedDialog);
    customRow->addWidget(m_customSeedBtn);

    m_customSeedSwatch = new SeedSwatchDot(card);
    customRow->addWidget(m_customSeedSwatch);

    m_clearSeedOverrideBtn = new QPushButton(QStringLiteral("Clear"), card);
    m_clearSeedOverrideBtn->setToolTip(
        QStringLiteral("Clear custom seed and use wallpaper colors"));
    m_clearSeedOverrideBtn->setMinimumHeight(28);
    m_clearSeedOverrideBtn->setEnabled(false);
    connect(m_clearSeedOverrideBtn, &QPushButton::clicked, this,
            &ThemeStudioWidget::clearCustomSeed);
    customRow->addWidget(m_clearSeedOverrideBtn);

    customRow->addStretch(1);
    layout->addLayout(customRow);

    parentLayout->addWidget(card);
}

void ThemeStudioWidget::setupWcagBadgesCard(QVBoxLayout *parentLayout)
{
    auto *card = new QGroupBox(QStringLiteral("WCAG 2.1 Contrast (AA 4.5 / AAA 7.0)"), this);
    auto *grid = new QGridLayout(card);
    grid->setContentsMargins(6, 6, 6, 6);
    grid->setSpacing(4);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);

    const QString pairs[] = {QStringLiteral("onPrimary / primary"),
                             QStringLiteral("onPrimaryContainer / primaryContainer"),
                             QStringLiteral("onSecondary / secondary"),
                             QStringLiteral("onSecondaryContainer / secondaryContainer"),
                             QStringLiteral("onTertiary / tertiary"),
                             QStringLiteral("onTertiaryContainer / tertiaryContainer"),
                             QStringLiteral("onSurface / surface"),
                             QStringLiteral("onSurfaceVariant / surfaceVariant"),
                             QStringLiteral("onBackground / background"),
                             QStringLiteral("onError / error"),
                             QStringLiteral("onSuccess / success"),
                             QStringLiteral("onSuccessContainer / successContainer")};

    m_wcagBadges.reserve(12);
    for (int i = 0; i < 12; ++i) {
        auto *badge = new WcagBadgeWidget(pairs[i], card);
        m_wcagBadges.push_back(badge);
        grid->addWidget(badge, i / 2, i % 2);
    }

    parentLayout->addWidget(card);
}

void ThemeStudioWidget::setupSwatchesCard(QTabWidget *tabWidget)
{
    auto *m3Scroll = new QScrollArea(tabWidget);
    m3Scroll->setWidgetResizable(true);
    m3Scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m3Scroll->setFrameShape(QFrame::NoFrame);
    m_m3SwatchGrid = new ColorSwatchGridWidget(QStringLiteral("Material 3 Swatches"), m3Scroll);
    connect(m_m3SwatchGrid, &ColorSwatchChip::copiedToClipboard, this,
            &ThemeStudioWidget::onSwatchCopied);
    connect(m_m3SwatchGrid, &ColorSwatchGridWidget::swatchEditRequested, this,
            &ThemeStudioWidget::onSwatchEditRequested);
    m3Scroll->setWidget(m_m3SwatchGrid);
    tabWidget->addTab(m3Scroll, QStringLiteral("M3 (36)"));

    auto *ansiScroll = new QScrollArea(tabWidget);
    ansiScroll->setWidgetResizable(true);
    ansiScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ansiScroll->setFrameShape(QFrame::NoFrame);
    m_ansiSwatchGrid =
        new ColorSwatchGridWidget(QStringLiteral("ANSI Terminal Swatches"), ansiScroll);
    connect(m_ansiSwatchGrid, &ColorSwatchChip::copiedToClipboard, this,
            &ThemeStudioWidget::onSwatchCopied);
    connect(m_ansiSwatchGrid, &ColorSwatchGridWidget::swatchEditRequested, this,
            &ThemeStudioWidget::onSwatchEditRequested);
    ansiScroll->setWidget(m_ansiSwatchGrid);
    tabWidget->addTab(ansiScroll, QStringLiteral("ANSI (16)"));

    auto *catScroll = new QScrollArea(tabWidget);
    catScroll->setWidgetResizable(true);
    catScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    catScroll->setFrameShape(QFrame::NoFrame);
    m_catppuccinSwatchGrid =
        new ColorSwatchGridWidget(QStringLiteral("Catppuccin Swatches"), catScroll);
    connect(m_catppuccinSwatchGrid, &ColorSwatchChip::copiedToClipboard, this,
            &ThemeStudioWidget::onSwatchCopied);
    connect(m_catppuccinSwatchGrid, &ColorSwatchGridWidget::swatchEditRequested, this,
            &ThemeStudioWidget::onSwatchEditRequested);
    catScroll->setWidget(m_catppuccinSwatchGrid);
    tabWidget->addTab(catScroll, QStringLiteral("Catppuccin (14)"));

    auto *wcagScroll = new QScrollArea(tabWidget);
    wcagScroll->setWidgetResizable(true);
    wcagScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    wcagScroll->setFrameShape(QFrame::NoFrame);
    auto *wcagContainer = new QWidget();
    auto *wcagLayout = new QVBoxLayout(wcagContainer);
    wcagLayout->setContentsMargins(6, 6, 6, 6);
    wcagLayout->setSpacing(6);
    setupWcagBadgesCard(wcagLayout);
    wcagLayout->addStretch(1);
    wcagScroll->setWidget(wcagContainer);
    tabWidget->addTab(wcagScroll, QStringLiteral("Contrast"));

    auto *exporterScroll = new QScrollArea(tabWidget);
    exporterScroll->setWidgetResizable(true);
    exporterScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    exporterScroll->setFrameShape(QFrame::NoFrame);
    m_templateExporter = new TemplateExporter(exporterScroll);
    exporterScroll->setWidget(m_templateExporter);
    tabWidget->addTab(exporterScroll, QStringLiteral("Export"));
}

void ThemeStudioWidget::loadWallpaper(const QString &filePath)
{
    if (filePath.isEmpty() || !QFileInfo::exists(filePath)) {
        Q_EMIT statusMessageRequested(
            QStringLiteral("Failed to load wallpaper image: %1").arg(filePath));
        return;
    }

    QImage loaded(filePath);
    if (loaded.isNull()) {
        uint8_t *rgba = nullptr;
        uint32_t w = 0, h = 0;
        const std::string stdPath = filePath.toStdString();
        if (image_load_rgba(stdPath.c_str(), &rgba, &w, &h) && rgba && w > 0 && h > 0) {
            QImage raw(rgba, static_cast<int>(w), static_cast<int>(h), static_cast<int>(w * 4),
                       QImage::Format_RGBA8888);
            loaded = raw.copy();
            image_free(rgba);
        } else if (rgba) {
            image_free(rgba);
        }
    }
    if (!loaded.isNull()) {
        setWallpaperImage(loaded, filePath);
    } else {
        Q_EMIT statusMessageRequested(
            QStringLiteral("Failed to decode wallpaper image: %1").arg(filePath));
    }
}

void ThemeStudioWidget::setWallpaperImage(const QImage &image, const QString &path)
{
    m_sourceImage = image;
    m_wallpaperPath = path;
    if (m_mockup) {
        m_mockup->setWallpaperImage(m_sourceImage);
    }
    reextract();
}

void ThemeStudioWidget::reextract()
{
    const auto t0 = std::chrono::steady_clock::now();

    // Synthesize a temporary sample buffer when no image is loaded without overwriting
    // m_sourceImage or degrading DesktopPreviewMockup's full-resolution procedural wallpaper.
    QImage sample;
    if (m_sourceImage.isNull()) {
        constexpr int w = 128;
        constexpr int h = 128;
        sample = QImage(w, h, QImage::Format_RGBA8888);
        for (int y = 0; y < h; ++y) {
            auto *row = sample.scanLine(y);
            for (int x = 0; x < w; ++x) {
                const float t = static_cast<float>(x + y) / static_cast<float>(w + h - 2);
                float r = 0.0f, g = 0.0f, b = 0.0f;
                if (t < 0.5f) {
                    const float u = t * 2.0f;
                    r = 48.0f + u * (140.0f - 48.0f);
                    g = 20.0f + u * (60.0f - 20.0f);
                    b = 96.0f + u * (180.0f - 96.0f);
                } else {
                    const float u = (t - 0.5f) * 2.0f;
                    r = 140.0f + u * (240.0f - 140.0f);
                    g = 60.0f + u * (100.0f - 60.0f);
                    b = 180.0f + u * (80.0f - 180.0f);
                }
                row[x * 4 + 0] = static_cast<uint8_t>(std::lround(r));
                row[x * 4 + 1] = static_cast<uint8_t>(std::lround(g));
                row[x * 4 + 2] = static_cast<uint8_t>(std::lround(b));
                row[x * 4 + 3] = 255U;
            }
        }
    } else {
        sample = m_sourceImage;
        if (sample.width() > 128 || sample.height() > 128) {
            sample = sample.scaled(128, 128, Qt::KeepAspectRatio, Qt::FastTransformation);
        }
        if (sample.format() != QImage::Format_RGBA8888) {
            sample = sample.convertToFormat(QImage::Format_RGBA8888);
        }
    }

    const bool ok = waywal_palette_extract_seed_ex(
        sample.constBits(), static_cast<uint32_t>(sample.width()),
        static_cast<uint32_t>(sample.height()), static_cast<uint32_t>(sample.bytesPerLine()), false,
        m_mode, m_variant, m_contrast, m_seedIndex, m_hasSeedOverride, m_seedOverrideRgb,
        &m_palette);

    const auto t1 = std::chrono::steady_clock::now();
    const double elapsedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (m_perfLabel) {
        m_perfLabel->setText(ok ? QStringLiteral("%1 ms").arg(QString::number(elapsedMs, 'f', 2))
                                : QStringLiteral("Failed"));
    }

    if (m_mockup) {
        m_mockup->setPalette(m_palette);
    }
    if (m_templateExporter) {
        m_templateExporter->setPalette(m_palette);
    }

    updateSeedRadioSwatches();
    updateWcagBadges();
    updateSwatchGrids();

    Q_EMIT paletteChanged(m_palette);
}

void ThemeStudioWidget::updateSeedRadioSwatches()
{
    for (int i = 0; i < 4; ++i) {
        if (auto *dot = static_cast<SeedSwatchDot *>(m_seedSwatches[i])) {
            const uint32_t c = m_palette.seeds[i];
            dot->setSwatchColor(QColor((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF), true);
        }
    }

    if (auto *customDot = static_cast<SeedSwatchDot *>(m_customSeedSwatch)) {
        if (m_hasSeedOverride) {
            customDot->setSwatchColor(QColor((m_seedOverrideRgb >> 16) & 0xFF,
                                             (m_seedOverrideRgb >> 8) & 0xFF,
                                             m_seedOverrideRgb & 0xFF),
                                      true);
            if (m_clearSeedOverrideBtn) {
                m_clearSeedOverrideBtn->setEnabled(true);
            }
        } else {
            customDot->setSwatchColor(QColor(), false);
            if (m_clearSeedOverrideBtn) {
                m_clearSeedOverrideBtn->setEnabled(false);
            }
        }
    }
}

void ThemeStudioWidget::updateWcagBadges()
{
    if (m_wcagBadges.size() < 12) {
        return;
    }
    m_wcagBadges[0]->updateRatio(m_palette.onPrimary, m_palette.primary);
    m_wcagBadges[1]->updateRatio(m_palette.onPrimaryContainer, m_palette.primaryContainer);
    m_wcagBadges[2]->updateRatio(m_palette.onSecondary, m_palette.secondary);
    m_wcagBadges[3]->updateRatio(m_palette.onSecondaryContainer, m_palette.secondaryContainer);
    m_wcagBadges[4]->updateRatio(m_palette.onTertiary, m_palette.tertiary);
    m_wcagBadges[5]->updateRatio(m_palette.onTertiaryContainer, m_palette.tertiaryContainer);
    m_wcagBadges[6]->updateRatio(m_palette.onSurface, m_palette.surface);
    m_wcagBadges[7]->updateRatio(m_palette.onSurfaceVariant, m_palette.surfaceVariant);
    m_wcagBadges[8]->updateRatio(m_palette.onBackground, m_palette.background);
    m_wcagBadges[9]->updateRatio(m_palette.onError, m_palette.error);
    m_wcagBadges[10]->updateRatio(m_palette.onSuccess, m_palette.success);
    m_wcagBadges[11]->updateRatio(m_palette.onSuccessContainer, m_palette.successContainer);
}

void ThemeStudioWidget::updateSwatchGrids()
{
    if (m_m3SwatchGrid) {
        const QVector<ColorSwatchGridWidget::SwatchEntry> m3Entries = {
            {QStringLiteral("primary"), m_palette.primary, formatSwatchHex(m_palette.primary)},
            {QStringLiteral("onPrimary"), m_palette.onPrimary,
             formatSwatchHex(m_palette.onPrimary)},
            {QStringLiteral("primaryContainer"), m_palette.primaryContainer,
             formatSwatchHex(m_palette.primaryContainer)},
            {QStringLiteral("onPrimaryContainer"), m_palette.onPrimaryContainer,
             formatSwatchHex(m_palette.onPrimaryContainer)},
            {QStringLiteral("inversePrimary"), m_palette.inversePrimary,
             formatSwatchHex(m_palette.inversePrimary)},
            {QStringLiteral("secondary"), m_palette.secondary,
             formatSwatchHex(m_palette.secondary)},
            {QStringLiteral("onSecondary"), m_palette.onSecondary,
             formatSwatchHex(m_palette.onSecondary)},
            {QStringLiteral("secondaryContainer"), m_palette.secondaryContainer,
             formatSwatchHex(m_palette.secondaryContainer)},
            {QStringLiteral("onSecondaryContainer"), m_palette.onSecondaryContainer,
             formatSwatchHex(m_palette.onSecondaryContainer)},
            {QStringLiteral("tertiary"), m_palette.tertiary, formatSwatchHex(m_palette.tertiary)},
            {QStringLiteral("onTertiary"), m_palette.onTertiary,
             formatSwatchHex(m_palette.onTertiary)},
            {QStringLiteral("tertiaryContainer"), m_palette.tertiaryContainer,
             formatSwatchHex(m_palette.tertiaryContainer)},
            {QStringLiteral("onTertiaryContainer"), m_palette.onTertiaryContainer,
             formatSwatchHex(m_palette.onTertiaryContainer)},
            {QStringLiteral("error"), m_palette.error, formatSwatchHex(m_palette.error)},
            {QStringLiteral("onError"), m_palette.onError, formatSwatchHex(m_palette.onError)},
            {QStringLiteral("errorContainer"), m_palette.errorContainer,
             formatSwatchHex(m_palette.errorContainer)},
            {QStringLiteral("onErrorContainer"), m_palette.onErrorContainer,
             formatSwatchHex(m_palette.onErrorContainer)},
            {QStringLiteral("success"), m_palette.success, formatSwatchHex(m_palette.success)},
            {QStringLiteral("onSuccess"), m_palette.onSuccess,
             formatSwatchHex(m_palette.onSuccess)},
            {QStringLiteral("successContainer"), m_palette.successContainer,
             formatSwatchHex(m_palette.successContainer)},
            {QStringLiteral("onSuccessContainer"), m_palette.onSuccessContainer,
             formatSwatchHex(m_palette.onSuccessContainer)},
            {QStringLiteral("background"), m_palette.background,
             formatSwatchHex(m_palette.background)},
            {QStringLiteral("onBackground"), m_palette.onBackground,
             formatSwatchHex(m_palette.onBackground)},
            {QStringLiteral("surface"), m_palette.surface, formatSwatchHex(m_palette.surface)},
            {QStringLiteral("onSurface"), m_palette.onSurface,
             formatSwatchHex(m_palette.onSurface)},
            {QStringLiteral("surfaceDim"), m_palette.surfaceDim,
             formatSwatchHex(m_palette.surfaceDim)},
            {QStringLiteral("surfaceBright"), m_palette.surfaceBright,
             formatSwatchHex(m_palette.surfaceBright)},
            {QStringLiteral("surfaceVariant"), m_palette.surfaceVariant,
             formatSwatchHex(m_palette.surfaceVariant)},
            {QStringLiteral("onSurfaceVariant"), m_palette.onSurfaceVariant,
             formatSwatchHex(m_palette.onSurfaceVariant)},
            {QStringLiteral("surfaceContainerLowest"), m_palette.surfaceContainerLowest,
             formatSwatchHex(m_palette.surfaceContainerLowest)},
            {QStringLiteral("surfaceContainerLow"), m_palette.surfaceContainerLow,
             formatSwatchHex(m_palette.surfaceContainerLow)},
            {QStringLiteral("surfaceContainer"), m_palette.surfaceContainer,
             formatSwatchHex(m_palette.surfaceContainer)},
            {QStringLiteral("surfaceContainerHigh"), m_palette.surfaceContainerHigh,
             formatSwatchHex(m_palette.surfaceContainerHigh)},
            {QStringLiteral("surfaceContainerHighest"), m_palette.surfaceContainerHighest,
             formatSwatchHex(m_palette.surfaceContainerHighest)},
            {QStringLiteral("outline"), m_palette.outline, formatSwatchHex(m_palette.outline)},
            {QStringLiteral("outlineVariant"), m_palette.outlineVariant,
             formatSwatchHex(m_palette.outlineVariant)}};
        m_m3SwatchGrid->setSwatches(m3Entries);
    }

    if (m_ansiSwatchGrid) {
        const QString ansiNames[16] = {
            QStringLiteral("term0 (Black)"),         QStringLiteral("term1 (Red)"),
            QStringLiteral("term2 (Green)"),         QStringLiteral("term3 (Yellow)"),
            QStringLiteral("term4 (Blue)"),          QStringLiteral("term5 (Magenta)"),
            QStringLiteral("term6 (Cyan)"),          QStringLiteral("term7 (White)"),
            QStringLiteral("term8 (Bright Black)"),  QStringLiteral("term9 (Bright Red)"),
            QStringLiteral("term10 (Bright Green)"), QStringLiteral("term11 (Bright Yellow)"),
            QStringLiteral("term12 (Bright Blue)"),  QStringLiteral("term13 (Bright Magenta)"),
            QStringLiteral("term14 (Bright Cyan)"),  QStringLiteral("term15 (Bright White)")};
        QVector<ColorSwatchGridWidget::SwatchEntry> ansiEntries;
        ansiEntries.reserve(16);
        for (int i = 0; i < 16; ++i) {
            ansiEntries.push_back(
                {ansiNames[i], m_palette.term[i], formatSwatchHex(m_palette.term[i])});
        }
        m_ansiSwatchGrid->setSwatches(ansiEntries);
    }

    if (m_catppuccinSwatchGrid) {
        const QVector<ColorSwatchGridWidget::SwatchEntry> catEntries = {
            {QStringLiteral("rosewater"), m_palette.rosewater,
             formatSwatchHex(m_palette.rosewater)},
            {QStringLiteral("flamingo"), m_palette.flamingo, formatSwatchHex(m_palette.flamingo)},
            {QStringLiteral("pink"), m_palette.pink, formatSwatchHex(m_palette.pink)},
            {QStringLiteral("mauve"), m_palette.mauve, formatSwatchHex(m_palette.mauve)},
            {QStringLiteral("red"), m_palette.red, formatSwatchHex(m_palette.red)},
            {QStringLiteral("maroon"), m_palette.maroon, formatSwatchHex(m_palette.maroon)},
            {QStringLiteral("peach"), m_palette.peach, formatSwatchHex(m_palette.peach)},
            {QStringLiteral("yellow"), m_palette.yellow, formatSwatchHex(m_palette.yellow)},
            {QStringLiteral("green"), m_palette.green, formatSwatchHex(m_palette.green)},
            {QStringLiteral("teal"), m_palette.teal, formatSwatchHex(m_palette.teal)},
            {QStringLiteral("sky"), m_palette.sky, formatSwatchHex(m_palette.sky)},
            {QStringLiteral("sapphire"), m_palette.sapphire, formatSwatchHex(m_palette.sapphire)},
            {QStringLiteral("blue"), m_palette.blue, formatSwatchHex(m_palette.blue)},
            {QStringLiteral("lavender"), m_palette.lavender, formatSwatchHex(m_palette.lavender)}};
        m_catppuccinSwatchGrid->setSwatches(catEntries);
    }
}

void ThemeStudioWidget::setSolarCoordinates(double latitudeDeg, double longitudeDeg,
                                            bool hasCoordinates)
{
    if (!hasCoordinates || !std::isfinite(latitudeDeg) || !std::isfinite(longitudeDeg)) {
        m_hasSolarCoords = false;
        m_solarLatitude = 0.0;
        m_solarLongitude = 0.0;
    } else {
        m_hasSolarCoords = true;
        m_solarLatitude = std::clamp(latitudeDeg, -89.8, 89.8);
        m_solarLongitude = std::clamp(longitudeDeg, -180.0, 180.0);
    }
    m_solarConfigLoaded = true;
    if (m_autoSolarSchedule) {
        applySolarScheduleEvaluation();
    }
}

void ThemeStudioWidget::loadSolarConfigDefaultsIfNeeded()
{
    if (m_solarConfigLoaded) {
        return;
    }
    m_solarConfigLoaded = true;

    waywal_config_t cfg{};
    if (waywal_config_load_default(&cfg)) {
        if (cfg.solar.has_coordinates && std::isfinite(cfg.solar.latitude) &&
            std::isfinite(cfg.solar.longitude)) {
            m_hasSolarCoords = true;
            m_solarLatitude = std::clamp(cfg.solar.latitude, -89.8, 89.8);
            m_solarLongitude = std::clamp(cfg.solar.longitude, -180.0, 180.0);
        }
        if (cfg.solar.sunrise_fallback_min > 0 && cfg.solar.sunrise_fallback_min < 1440) {
            m_sunriseFallbackMin = cfg.solar.sunrise_fallback_min;
        }
        if (cfg.solar.sunset_fallback_min > 0 && cfg.solar.sunset_fallback_min < 1440) {
            m_sunsetFallbackMin = cfg.solar.sunset_fallback_min;
        }
    }
}

void ThemeStudioWidget::setThemeMode(waywal_theme_mode_t mode)
{
    if (mode != WAYWAL_THEME_DARK && mode != WAYWAL_THEME_LIGHT) {
        return;
    }
    if (m_modeDarkBtn && m_modeLightBtn) {
        const QSignalBlocker b1(m_modeDarkBtn);
        const QSignalBlocker b2(m_modeLightBtn);
        m_modeDarkBtn->setChecked(mode == WAYWAL_THEME_DARK);
        m_modeLightBtn->setChecked(mode == WAYWAL_THEME_LIGHT);
    }
    if (m_mode != mode) {
        m_mode = mode;
        reextract();
        Q_EMIT modeChanged(m_mode);
    }
}

void ThemeStudioWidget::setAutoSolarSchedule(bool enabled)
{
    m_autoSolarSchedule = enabled;
    if (m_autoSolarCheck && m_autoSolarCheck->isChecked() != enabled) {
        const QSignalBlocker blocker(m_autoSolarCheck);
        m_autoSolarCheck->setChecked(enabled);
    }
    if (m_modeDarkBtn) {
        m_modeDarkBtn->setEnabled(!m_autoSolarSchedule);
    }
    if (m_modeLightBtn) {
        m_modeLightBtn->setEnabled(!m_autoSolarSchedule);
    }

    if (m_autoSolarSchedule) {
        loadSolarConfigDefaultsIfNeeded();
        applySolarScheduleEvaluation();
        if (isVisible() && !m_solarTimer.isActive()) {
            m_solarTimer.start(60000);
        }
    } else {
        m_solarTimer.stop();
        if (m_solarPhaseBadge) {
            m_solarPhaseBadge->setText(QStringLiteral("Manual"));
        }
    }
}

void ThemeStudioWidget::applySolarScheduleEvaluation()
{
    const time_t utcNow = std::time(nullptr);
    struct tm localTm{};
    const struct tm *tmPtr = ::localtime_r(&utcNow, &localTm);
    if (!tmPtr) {
        if (m_solarPhaseBadge) {
            m_solarPhaseBadge->setText(QStringLiteral("Solar: Time Unavailable"));
        }
        Q_EMIT statusMessageRequested(
            QStringLiteral("Solar evaluation failed: localtime_r returned null"));
        return;
    }

    const bool coordsValid =
        m_hasSolarCoords && std::isfinite(m_solarLatitude) && std::isfinite(m_solarLongitude);
    waywal_solar_phase_t phase = WAYWAL_SOLAR_DAY;
    const bool isDaytime = waywal_solar_is_daytime_at_ex(
        coordsValid ? m_solarLatitude : 0.0, coordsValid ? m_solarLongitude : 0.0, coordsValid,
        m_sunriseFallbackMin, m_sunsetFallbackMin, utcNow, tmPtr, &phase);
    const char *phaseStr = waywal_solar_phase_to_string(phase);

    if (m_solarPhaseBadge) {
        m_solarPhaseBadge->setText(
            QStringLiteral("Solar: %1 (%2)")
                .arg(isDaytime ? QStringLiteral("Day") : QStringLiteral("Night"))
                .arg(QString::fromUtf8(phaseStr ? phaseStr : "unknown")));
    }

    const waywal_theme_mode_t desiredMode = isDaytime ? WAYWAL_THEME_LIGHT : WAYWAL_THEME_DARK;
    if (m_mode != desiredMode) {
        setThemeMode(desiredMode);
    }
}

void ThemeStudioWidget::onSolarTimerTick()
{
    if (m_autoSolarSchedule && isVisible()) {
        applySolarScheduleEvaluation();
    }
}

void ThemeStudioWidget::setSchemeVariant(waywal_scheme_variant_t variant)
{
    if (m_variant != variant) {
        m_variant = variant;
        if (m_variantCombo) {
            const int idx = m_variantCombo->findData(static_cast<int>(variant));
            if (idx >= 0 && m_variantCombo->currentIndex() != idx) {
                const QSignalBlocker blocker(m_variantCombo);
                m_variantCombo->setCurrentIndex(idx);
            }
        }
        reextract();
        Q_EMIT variantChanged(m_variant);
    }
}

void ThemeStudioWidget::setContrast(float contrast)
{
    if (!std::isfinite(contrast)) {
        return;
    }
    const float clamped = std::clamp(contrast, -1.0f, 1.0f);
    if (std::abs(m_contrast - clamped) > 0.005f) {
        m_contrast = clamped;
        if (m_contrastValLabel) {
            m_contrastValLabel->setText(
                QStringLiteral("Contrast: %1%2")
                    .arg(m_contrast >= 0.0f ? QStringLiteral("+") : QStringLiteral(""))
                    .arg(QString::number(m_contrast, 'f', 2)));
        }
        if (m_contrastSlider) {
            const int sliderVal = static_cast<int>(std::lround(m_contrast * 100.0f));
            if (m_contrastSlider->value() != sliderVal) {
                const QSignalBlocker blocker(m_contrastSlider);
                m_contrastSlider->setValue(sliderVal);
            }
        }
        reextract();
        Q_EMIT contrastChanged(m_contrast);
    }
}

void ThemeStudioWidget::setSeedIndex(uint8_t index)
{
    if (index >= 4) {
        return;
    }
    if (m_seedRadios.size() == 4 && m_seedRadios[index] && m_seedGroup) {
        const QSignalBlocker blocker(m_seedGroup);
        m_seedRadios[index]->setChecked(true);
    }
    if (m_seedIndex != index || m_hasSeedOverride) {
        m_seedIndex = index;
        m_hasSeedOverride = false;
        reextract();
        Q_EMIT seedIndexChanged(m_seedIndex);
    }
}

void ThemeStudioWidget::applyCustomSeedColor(const QColor &chosen, const QString &sourceContext)
{
    if (!chosen.isValid()) {
        return;
    }
    m_hasSeedOverride = true;
    m_seedOverrideRgb = (static_cast<uint32_t>(chosen.red()) << 16) |
                        (static_cast<uint32_t>(chosen.green()) << 8) |
                        static_cast<uint32_t>(chosen.blue());

    if (m_seedGroup) {
        const QSignalBlocker blocker(m_seedGroup);
        m_seedGroup->setExclusive(false);
        for (QRadioButton *r : m_seedRadios) {
            if (r) {
                r->setChecked(false);
            }
        }
        m_seedGroup->setExclusive(true);
    }

    reextract();

    const QString hexUpper = chosen.name(QColor::HexRgb).toUpper();
    if (sourceContext.isEmpty()) {
        Q_EMIT statusMessageRequested(QStringLiteral("Custom seed override: %1").arg(hexUpper));
    } else {
        Q_EMIT statusMessageRequested(
            QStringLiteral("Custom seed override from %1: %2").arg(sourceContext, hexUpper));
    }
}

void ThemeStudioWidget::openSeedColorDialog(const QColor &initial, const QString &title,
                                            const QString &sourceContext)
{
    if (m_customSeedDialog) {
        m_customSeedDialog->close();
    }

    QPointer<ThemeStudioWidget> self(this);
    m_customSeedDialog = OklchColorDialog::openNonModal(
        initial, this, title, this, [self, sourceContext](const QColor &chosen) {
            if (!self) {
                return;
            }
            self->applyCustomSeedColor(chosen, sourceContext);
        });
}

void ThemeStudioWidget::openCustomSeedDialog()
{
    const QColor current = m_hasSeedOverride
                               ? QColor((m_seedOverrideRgb >> 16) & 0xFF,
                                        (m_seedOverrideRgb >> 8) & 0xFF, m_seedOverrideRgb & 0xFF)
                               : toQColor(m_palette.primary);

    openSeedColorDialog(current, QStringLiteral("Select Custom Seed Color Override"), QString());
}

void ThemeStudioWidget::onSwatchEditRequested(const QString &name, const color_rgba_t &color)
{
    openSeedColorDialog(toQColor(color), QStringLiteral("Edit Seed from '%1' (OkLCH)").arg(name),
                        name);
}

void ThemeStudioWidget::clearCustomSeed()
{
    if (m_hasSeedOverride) {
        m_hasSeedOverride = false;
        m_seedOverrideRgb = 0;
        if (m_seedIndex < 4 && m_seedRadios.size() == 4 && m_seedRadios[m_seedIndex] &&
            m_seedGroup) {
            const QSignalBlocker blocker(m_seedGroup);
            m_seedRadios[m_seedIndex]->setChecked(true);
        }
        reextract();
        Q_EMIT statusMessageRequested(QStringLiteral("Cleared custom seed override"));
    }
}

void ThemeStudioWidget::openWallpaperDialog()
{
    if (m_activeWallpaperDialog) {
        m_activeWallpaperDialog->raise();
        m_activeWallpaperDialog->activateWindow();
        return;
    }

    auto *dlg = new QFileDialog(
        this, QStringLiteral("Select Wallpaper Image"), QDir::homePath(),
        QStringLiteral(
            "Images (*.png *.jpg *.jpeg *.webp *.bmp *.svg *.avif *.jxl *.qoi);;All Files (*)"));
    dlg->setFileMode(QFileDialog::ExistingFile);
    dlg->setAttribute(Qt::WA_DeleteOnClose, true);
    dlg->setWindowModality(Qt::NonModal);
    dlg->setModal(false);
    m_activeWallpaperDialog = dlg;

    QPointer<ThemeStudioWidget> self(this);
    connect(dlg, &QFileDialog::fileSelected, this, [self](const QString &filePath) {
        if (self && !filePath.isEmpty()) {
            self->loadWallpaper(filePath);
        }
    });
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

void ThemeStudioWidget::onSwatchCopied(const QString &name, const QString &hex)
{
    const QString msg = QStringLiteral("Copied %1 (%2) to clipboard!").arg(hex, name);
    if (m_clipboardToastLabel) {
        m_clipboardToastLabel->setText(msg);
        m_toastTimer.start(3500);
    }
    Q_EMIT statusMessageRequested(msg);
}

} // namespace waywal::studio
