// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/theme/desktop_preview_mockup.hpp"

#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QLinearGradient>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>

#include <algorithm>
#include <cmath>

namespace waywal::studio {

DesktopPreviewMockup::DesktopPreviewMockup(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void DesktopPreviewMockup::setPalette(const waywal_palette_t &palette)
{
    m_palette = palette;
    m_hasPalette = true;
    m_blurDirty = true;
    update();
}

void DesktopPreviewMockup::setWallpaperImage(const QImage &image)
{
    m_wallpaperImage = image;
    m_blurDirty = true;
    update();
}

void DesktopPreviewMockup::setLockscreenPreview(bool enabled)
{
    if (m_lockscreenPreview != enabled) {
        m_lockscreenPreview = enabled;
        update();
    }
}

void DesktopPreviewMockup::toggleLockscreenPreview()
{
    setLockscreenPreview(!m_lockscreenPreview);
}

QSize DesktopPreviewMockup::sizeHint() const
{
    return {640, 360};
}

QSize DesktopPreviewMockup::minimumSizeHint() const
{
    return {170, 120};
}

void DesktopPreviewMockup::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    m_blurDirty = true;
}

QColor DesktopPreviewMockup::toQColor(const color_rgba_t &c)
{
    return QColor(c.r, c.g, c.b, c.a);
}

QString DesktopPreviewMockup::toHex(const color_rgba_t &c)
{
    return QString::asprintf("#%02x%02x%02x", c.r, c.g, c.b);
}

void DesktopPreviewMockup::ensureWallpaperValid()
{
    if (!m_wallpaperImage.isNull()) {
        return;
    }

    // Synthesize an artistic procedural wallpaper using palette colors
    const int w = std::max(640, width());
    const int h = std::max(360, height());
    QImage synthetic(w, h, QImage::Format_ARGB32_Premultiplied);

    QColor base = m_hasPalette ? toQColor(m_palette.background) : QColor(18, 19, 24);
    QColor pri = m_hasPalette ? toQColor(m_palette.primary) : QColor(103, 80, 164);
    QColor sec = m_hasPalette ? toQColor(m_palette.secondary) : QColor(98, 91, 113);
    QColor ter = m_hasPalette ? toQColor(m_palette.tertiary) : QColor(125, 82, 96);

    QPainter sp(&synthetic);
    sp.setRenderHint(QPainter::Antialiasing);

    QLinearGradient bgGrad(0, 0, w, h);
    bgGrad.setColorAt(0.0, base.darker(120));
    bgGrad.setColorAt(0.5, base);
    bgGrad.setColorAt(1.0, base.lighter(110));
    sp.fillRect(0, 0, w, h, bgGrad);

    QRadialGradient g1(w * 0.75, h * 0.35, w * 0.45);
    g1.setColorAt(0.0, QColor(pri.red(), pri.green(), pri.blue(), 110));
    g1.setColorAt(0.6, QColor(sec.red(), sec.green(), sec.blue(), 45));
    g1.setColorAt(1.0, QColor(0, 0, 0, 0));
    sp.fillRect(0, 0, w, h, g1);

    QRadialGradient g2(w * 0.25, h * 0.75, w * 0.40);
    g2.setColorAt(0.0, QColor(ter.red(), ter.green(), ter.blue(), 90));
    g2.setColorAt(0.7, QColor(pri.red(), pri.green(), pri.blue(), 30));
    g2.setColorAt(1.0, QColor(0, 0, 0, 0));
    sp.fillRect(0, 0, w, h, g2);

    sp.end();
    m_wallpaperImage = synthetic;
}

void DesktopPreviewMockup::updateBlurredWallpaper()
{
    if (!m_blurDirty && !m_blurredWallpaper.isNull()) {
        return;
    }

    ensureWallpaperValid();

    const int targetW = std::max(200, width());
    const int targetH = std::max(120, height());

    // Multi-pass Kawase / Dual-Filter downsampling and upsampling simulation
    // Pass 1: Downsample to 1/2 size
    QImage pass1 = m_wallpaperImage.scaled(
        targetW / 2, targetH / 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    // Pass 2: Downsample to 1/4 size
    QImage pass2 = pass1.scaled(
        targetW / 4, targetH / 4, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    // Pass 3: Downsample to 1/8 size
    QImage pass3 = pass2.scaled(
        targetW / 8, targetH / 8, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    // Pass 4: Upsample back to 1/4 size with bilinear interpolation
    QImage pass4 = pass3.scaled(
        targetW / 4, targetH / 4, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    // Pass 5: Upsample back to target size with bilinear interpolation
    QImage blurred = pass4.scaled(
        targetW, targetH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    // Apply frosted surface color tint overlay
    QPainter bp(&blurred);
    QColor tint = m_hasPalette ? toQColor(m_palette.surface) : QColor(25, 28, 30);
    tint.setAlpha(120);
    bp.fillRect(blurred.rect(), tint);
    bp.end();

    m_blurredWallpaper = blurred;
    m_blurDirty = false;
}

void DesktopPreviewMockup::paintEvent(QPaintEvent * /*event*/)
{
    ensureWallpaperValid();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRect bounds = rect();

    // Clip to rounded container border
    QPainterPath containerClip;
    containerClip.addRoundedRect(bounds, 12.0, 12.0);
    p.setClipPath(containerClip);

    if (m_lockscreenPreview) {
        drawLockscreenView(p, bounds);
    } else {
        drawDesktopView(p, bounds);
    }

    // Outer subtle border
    p.setClipping(false);
    p.setPen(QPen(m_hasPalette ? toQColor(m_palette.outlineVariant) : QColor(80, 80, 90), 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(bounds.adjusted(1, 1, -1, -1), 12.0, 12.0);
}

void DesktopPreviewMockup::drawDesktopView(QPainter &p, const QRect &r)
{
    // 1. Wallpaper background
    if (!m_wallpaperImage.isNull()) {
        p.drawImage(r, m_wallpaperImage);
    }

    // 2. Waybar top bar
    drawWaybar(p, r);

    // 3. Hyprland Windows
    drawHyprlandWindows(p, r);

    // 4. Mako notification toast
    drawMakoToast(p, r);
}

void DesktopPreviewMockup::drawWaybar(QPainter &p, const QRect &desktopRect)
{
    const bool compact = desktopRect.width() < 380;
    const int barMargin = compact ? 6 : 10;
    const int barH = compact ? 22 : 28;
    const QRect barRect(desktopRect.x() + barMargin, desktopRect.y() + barMargin,
                        desktopRect.width() - (barMargin * 2), barH);

    // Waybar floating pill background
    QColor barBg = m_hasPalette ? toQColor(m_palette.surfaceContainerLowest) : QColor(20, 20, 26);
    barBg.setAlpha(215);

    p.setPen(QPen(m_hasPalette ? toQColor(m_palette.outlineVariant) : QColor(60, 60, 70), 1.0));
    p.setBrush(barBg);
    p.drawRoundedRect(barRect, 8.0, 8.0);

    // Left module: Workspaces [ 1 ] [ 2 ] [ 3 ] ...
    int wsX = barRect.x() + 6;
    const int wsY = barRect.y() + 3;
    const int wsH = barH - 6;
    const int wsW = compact ? 15 : 20;
    const int wsCount = compact ? 3 : 5;

    QFont wsFont = p.font();
    wsFont.setPointSize(compact ? 7 : 8);
    wsFont.setBold(true);
    p.setFont(wsFont);

    for (int i = 1; i <= wsCount; ++i) {
        QRect wsPill(wsX, wsY, wsW, wsH);
        if (i == 1) { // Active workspace
            QColor actBg = m_hasPalette ? toQColor(m_palette.primary) : QColor(103, 80, 164);
            QColor actFg = m_hasPalette ? toQColor(m_palette.onPrimary) : Qt::white;
            p.setPen(Qt::NoPen);
            p.setBrush(actBg);
            p.drawRoundedRect(wsPill, 5.0, 5.0);
            p.setPen(actFg);
        } else {
            QColor inactBg = m_hasPalette ? toQColor(m_palette.surfaceContainerHigh) : QColor(40, 40, 50);
            QColor inactFg = m_hasPalette ? toQColor(m_palette.onSurfaceVariant) : QColor(180, 180, 190);
            p.setPen(Qt::NoPen);
            p.setBrush(inactBg);
            p.drawRoundedRect(wsPill, 5.0, 5.0);
            p.setPen(inactFg);
        }
        p.drawText(wsPill, Qt::AlignCenter, QString::number(i));
        wsX += wsW + 3;
    }

    QFontMetrics fm(wsFont);

    // Right modules: System tray pills (drawn first so we know exact remaining center space)
    int trayX = barRect.right() - 6;
    const auto drawTrayPill = [&](const QString &label, const color_rgba_t &accent, bool isPrimary) {
        const int w = fm.horizontalAdvance(label) + (compact ? 10 : 14);
        if (trayX - w <= wsX + 8) {
            return;
        }
        trayX -= w;
        QRect pill(trayX, wsY, w, wsH);

        QColor bg = m_hasPalette ? toQColor(m_palette.surfaceContainerHigh) : QColor(40, 40, 50);
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawRoundedRect(pill, 5.0, 5.0);

        p.setPen(m_hasPalette ? toQColor(accent) : (isPrimary ? QColor(103, 80, 164) : Qt::white));
        p.drawText(pill, Qt::AlignCenter, label);
        trayX -= 4;
    };

    drawTrayPill(QStringLiteral("15:42"), m_palette.primary, true);
    drawTrayPill(QStringLiteral("98%"), m_palette.tertiary, false);
    if (!compact) {
        drawTrayPill(QStringLiteral("18%"), m_palette.secondary, false);
        drawTrayPill(QStringLiteral("Vol 75%"), m_palette.primaryFixedDim, false);
    }

    // Center module: Window Title Pill bounded strictly between [wsX + 6, trayX - 6]
    const int availCenterW = trayX - wsX - 12;
    if (availCenterW >= 48) {
        const QString titleText = QStringLiteral("wwal-studio — Material You Live Theme");
        const int titleW = std::min(fm.horizontalAdvance(titleText) + 18, availCenterW);
        const int titleX = wsX + 6 + (availCenterW - titleW) / 2;
        const QRect titleRect(titleX, wsY, titleW, wsH);

        QColor titleBg = m_hasPalette ? toQColor(m_palette.surfaceContainerHigh) : QColor(40, 40, 50);
        titleBg.setAlpha(220);
        p.setPen(Qt::NoPen);
        p.setBrush(titleBg);
        p.drawRoundedRect(titleRect, 5.0, 5.0);

        p.setPen(m_hasPalette ? toQColor(m_palette.onSurface) : Qt::white);
        const QString elidedTitle = fm.elidedText(titleText, Qt::ElideRight, titleW - 10);
        p.drawText(titleRect, Qt::AlignCenter, elidedTitle);
    }
}

void DesktopPreviewMockup::drawHyprlandWindows(QPainter &p, const QRect &desktopRect)
{
    const bool compact = desktopRect.width() < 340 || desktopRect.height() < 180;
    const int usableTop = desktopRect.y() + (compact ? 32 : 44);
    const int usableH = std::max(60, desktopRect.height() - (compact ? 38 : 52));
    const int usableW = desktopRect.width();
    const bool tallDesktop = (usableH >= 200);

    // Inactive Window (Behind, Top-Left)
    const int inactW = std::clamp(static_cast<int>(usableW * 0.42), 85, 380);
    const int inactH = std::clamp(static_cast<int>(usableH * (tallDesktop ? 0.50 : 0.68)), 56, 260);
    const QRect inactRect(desktopRect.x() + (compact ? 8 : 16), usableTop + (compact ? 3 : 6), inactW, inactH);

    // Inactive drop shadow
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 50));
    p.drawRoundedRect(inactRect.translated(3, 4), 8.0, 8.0);

    // Inactive Window Body
    QColor inactBg = m_hasPalette ? toQColor(m_palette.surfaceDim) : QColor(24, 24, 30);
    p.setPen(QPen(m_hasPalette ? toQColor(m_palette.outlineVariant) : QColor(70, 70, 80), 1.8));
    p.setBrush(inactBg);
    p.drawRoundedRect(inactRect, 8.0, 8.0);

    // Inactive Titlebar
    QRect inactTitleBar(inactRect.x(), inactRect.y(), inactRect.width(), 20);
    QPainterPath inactTitleClip;
    inactTitleClip.addRoundedRect(inactRect, 8.0, 8.0);
    p.setClipPath(inactTitleClip);

    p.setPen(Qt::NoPen);
    p.setBrush(m_hasPalette ? toQColor(m_palette.surfaceContainerLow) : QColor(32, 32, 40));
    p.drawRect(inactTitleBar);

    // Inactive window controls (small grey dots)
    for (int i = 0; i < 3; ++i) {
        p.setBrush(m_hasPalette ? toQColor(m_palette.outline) : QColor(90, 90, 100));
        p.drawEllipse(inactTitleBar.x() + 7 + (i * 11), inactTitleBar.y() + 6, 7, 7);
    }

    QFont inactFont = p.font();
    inactFont.setPixelSize(9);
    p.setFont(inactFont);
    QFontMetrics inactFm(inactFont);
    p.setPen(m_hasPalette ? toQColor(m_palette.onSurfaceVariant) : QColor(160, 160, 170));
    const QRect inactTitleTextRect = inactTitleBar.adjusted(42, 0, -6, 0);
    p.drawText(inactTitleTextRect, Qt::AlignVCenter | Qt::AlignLeft,
               inactFm.elidedText(QStringLiteral("Files — ~/wwal/themes"), Qt::ElideRight,
                                  std::max(20, inactTitleTextRect.width())));

    // Inactive window content lines (simulated file manager rows)
    const int rowCount = std::min(6, (inactRect.height() - 28) / 15);
    for (int r = 0; r < rowCount; ++r) {
        QRect row(inactRect.x() + 10, inactRect.y() + 26 + (r * 15), inactRect.width() - 20, 11);
        p.setPen(Qt::NoPen);
        p.setBrush(m_hasPalette ? toQColor(m_palette.surfaceContainerHigh) : QColor(38, 38, 48));
        p.drawRoundedRect(row, 3.0, 3.0);
    }
    p.setClipping(false);

    // Active Kitty Window:
    // - In tall desktops (usableH >= 200), place Kitty below the top-right Mako toast so they never overlap vertically,
    //   and make it wide enough (82% of usableW) that full prompt & HCT description strings never clip.
    // - In compact desktops (usableH < 200), place Kitty on the left/center so the right-side Mako toast never overlaps horizontally.
    QRect actRect;
    if (tallDesktop) {
        const int toastReserveH = 74;
        const int actW = std::clamp(static_cast<int>(usableW * 0.84), 210, 480);
        const int actH = std::clamp(usableH - toastReserveH - 6, 100, 360);
        const int actX = std::clamp(desktopRect.right() - actW - 10,
                                    desktopRect.x() + 12,
                                    desktopRect.right() - actW - 6);
        const int actY = usableTop + toastReserveH;
        actRect = QRect(actX, actY, actW, actH);
    } else {
        const int toastW = std::clamp(static_cast<int>(usableW * 0.36), 96, 176);
        const int availLeftW = std::max(118, usableW - toastW - (compact ? 14 : 22));
        const int actW = std::clamp(availLeftW - (compact ? 4 : 8), 114, 340);
        const int actH = std::clamp(usableH - 4, 78, 180);
        const int actX = desktopRect.x() + (compact ? 8 : 14);
        const int actY = usableTop + std::max(2, usableH - actH - 1);
        actRect = QRect(actX, actY, actW, actH);
    }

    // Active drop shadow
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 85));
    p.drawRoundedRect(actRect.translated(5, 6), 9.0, 9.0);

    // Hyprland 45° Gradient Active Border: rgb($primary) rgb($tertiary) 45deg
    QLinearGradient gradBorder(actRect.topLeft(), actRect.bottomRight());
    gradBorder.setColorAt(0.0, m_hasPalette ? toQColor(m_palette.primary) : QColor(103, 80, 164));
    gradBorder.setColorAt(1.0, m_hasPalette ? toQColor(m_palette.tertiary) : QColor(125, 82, 96));

    // Outer border stroke
    p.setPen(QPen(QBrush(gradBorder), 3.0));
    p.setBrush(m_hasPalette ? toQColor(m_palette.term0) : QColor(16, 17, 22));
    p.drawRoundedRect(actRect, 8.0, 8.0);

    // Inside Active Window: Kitty Terminal Mockup
    drawKittyTerminal(p, actRect.adjusted(3, 3, -3, -3));
}

void DesktopPreviewMockup::drawKittyTerminal(QPainter &p, const QRect &windowRect)
{
    QPainterPath winClip;
    winClip.addRoundedRect(windowRect, 6.0, 6.0);
    p.setClipPath(winClip);

    const bool compactTerm = (windowRect.width() < 230 || windowRect.height() < 105);

    // Terminal tab bar
    const int tabH = compactTerm ? 16 : 20;
    const QRect tabRect(windowRect.x(), windowRect.y(), windowRect.width(), tabH);
    p.setPen(Qt::NoPen);
    p.setBrush(m_hasPalette ? toQColor(m_palette.surfaceContainerLow) : QColor(28, 28, 36));
    p.drawRect(tabRect);

    // Colored window control buttons (Red, Yellow, Green)
    const int dotSize = compactTerm ? 6 : 8;
    const int dotY = tabRect.y() + (tabH - dotSize) / 2;
    p.setBrush(m_hasPalette ? toQColor(m_palette.error) : QColor(255, 95, 87));
    p.drawEllipse(tabRect.x() + 6, dotY, dotSize, dotSize);

    p.setBrush(m_hasPalette ? toQColor(m_palette.tertiary) : QColor(254, 188, 46));
    p.drawEllipse(tabRect.x() + 6 + dotSize + 4, dotY, dotSize, dotSize);

    p.setBrush(m_hasPalette ? toQColor(m_palette.primary) : QColor(40, 201, 64));
    p.drawEllipse(tabRect.x() + 6 + 2 * (dotSize + 4), dotY, dotSize, dotSize);

    // Active Kitty tab pill
    const int pillX = tabRect.x() + 6 + 3 * (dotSize + 4) + 2;
    const int pillW = std::clamp(windowRect.right() - pillX - 6, 48, 108);
    const QRect activeTab(pillX, tabRect.y() + 2, pillW, tabH - 2);
    p.setBrush(m_hasPalette ? toQColor(m_palette.surfaceContainerHigh) : QColor(42, 42, 54));
    p.drawRoundedRect(activeTab, 4.0, 4.0);

    QFont tabFont = p.font();
    tabFont.setPixelSize(compactTerm ? 8 : 9);
    tabFont.setBold(true);
    p.setFont(tabFont);
    p.setPen(m_hasPalette ? toQColor(m_palette.onSurface) : Qt::white);
    QFontMetrics tabFm(tabFont);
    p.drawText(activeTab, Qt::AlignCenter,
               tabFm.elidedText(QStringLiteral("kitty: ~/wwal"), Qt::ElideRight, pillW - 6));

    // Terminal body
    const QRect bodyRect(windowRect.x() + 5, windowRect.y() + tabH + 2,
                         windowRect.width() - 10, windowRect.height() - tabH - 4);

    QFont termFont(QStringLiteral("Monospace"));
    termFont.setStyleHint(QFont::Monospace);
    termFont.setPixelSize(compactTerm ? 8 : 9);
    p.setFont(termFont);
    QFontMetrics termFm(termFont);

    int textY = bodyRect.y() + (compactTerm ? 8 : 10);
    const int lineStep = compactTerm ? 10 : 12;

    // Shell Prompt Line: user@arch:~/wwal $ colorscript -e m3 (elided to fit bodyRect)
    int cursorX = bodyRect.x() + 2;
    const int maxRight = bodyRect.right() - 2;

    const auto drawSegment = [&](const QString &seg, const QColor &col) {
        if (cursorX >= maxRight) return;
        p.setPen(col);
        const int avail = maxRight - cursorX;
        const int adv = termFm.horizontalAdvance(seg);
        if (adv <= avail) {
            p.drawText(cursorX, textY, seg);
            cursorX += adv;
        } else {
            p.drawText(cursorX, textY, termFm.elidedText(seg, Qt::ElideRight, avail));
            cursorX = maxRight;
        }
    };

    drawSegment(QStringLiteral("execorn"), m_hasPalette ? toQColor(m_palette.term2) : QColor(80, 250, 123));
    drawSegment(QStringLiteral("@"), m_hasPalette ? toQColor(m_palette.onSurfaceVariant) : QColor(200, 200, 210));
    drawSegment(QStringLiteral("arch"), m_hasPalette ? toQColor(m_palette.term6) : QColor(139, 233, 253));
    drawSegment(QStringLiteral(":"), m_hasPalette ? toQColor(m_palette.onSurfaceVariant) : QColor(200, 200, 210));
    drawSegment(QStringLiteral("~/wwal"), m_hasPalette ? toQColor(m_palette.term4) : QColor(98, 114, 164));
    drawSegment(QStringLiteral(" $ "), m_hasPalette ? toQColor(m_palette.primary) : QColor(189, 147, 249));
    drawSegment(compactTerm ? QStringLiteral("colorscript") : QStringLiteral("colorscript -e m3"),
                m_hasPalette ? toQColor(m_palette.onSurface) : Qt::white);

    textY += lineStep;

    // System info snippet (elided cleanly within bodyRect)
    p.setPen(m_hasPalette ? toQColor(m_palette.onSurfaceVariant) : QColor(170, 170, 180));
    const QString infoStr = compactTerm
                                ? QStringLiteral("M3 HCT • OkLab • 32+16")
                                : QStringLiteral("Material You HCT • OkLab • 32 M3 + 16 ANSI");
    p.drawText(bodyRect.x() + 2, textY,
               termFm.elidedText(infoStr, Qt::ElideRight, std::max(20, bodyRect.width() - 4)));

    // 16 ANSI Color Squares Grid (2 rows of 8), scaled to fit both width and remaining vertical height
    const int sqGap = compactTerm ? 2 : 3;
    const int ansiStartY = textY + (compactTerm ? 3 : 4);
    const int availAnsiH = std::max(12, bodyRect.bottom() - ansiStartY - 1);
    const int maxSqByW = (bodyRect.width() - 4 - 7 * sqGap) / 8;
    const int maxSqByH = (availAnsiH - sqGap) / 2;
    const int sqSize = std::clamp(std::min(maxSqByW, maxSqByH), 5, 22);

    for (int col = 0; col < 8; ++col) {
        // Row 1: term0 .. term7 (Standard ANSI)
        QRect sq1(bodyRect.x() + 2 + (col * (sqSize + sqGap)), ansiStartY, sqSize, sqSize);
        p.setPen(QPen(QColor(0, 0, 0, 80), 0.5));
        p.setBrush(m_hasPalette ? toQColor(m_palette.term[col]) : QColor(40 + (col * 25), 40, 40));
        p.drawRoundedRect(sq1, 2.0, 2.0);

        // Row 2: term8 .. term15 (Bright ANSI)
        QRect sq2(bodyRect.x() + 2 + (col * (sqSize + sqGap)), ansiStartY + sqSize + sqGap, sqSize, sqSize);
        p.setBrush(m_hasPalette ? toQColor(m_palette.term[col + 8]) : QColor(80 + (col * 20), 80, 80));
        p.drawRoundedRect(sq2, 2.0, 2.0);
    }

    p.setClipping(false);
}

void DesktopPreviewMockup::drawMakoToast(QPainter &p, const QRect &desktopRect)
{
    const bool compact = desktopRect.width() < 340 || desktopRect.height() < 180;
    const int usableH = std::max(60, desktopRect.height() - (compact ? 38 : 52));
    const bool tallDesktop = (usableH >= 200);
    const int toastW = tallDesktop
                           ? std::clamp(static_cast<int>(desktopRect.width() * 0.48), 150, 250)
                           : std::clamp(static_cast<int>(desktopRect.width() * 0.36), 96, 176);
    const int toastH = tallDesktop ? 62 : 46;
    const QRect toastRect(desktopRect.right() - toastW - (compact ? 6 : 12),
                          desktopRect.y() + (compact ? 34 : 46), toastW, toastH);

    // Drop shadow
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 80));
    p.drawRoundedRect(toastRect.translated(2, 3), 8.0, 8.0);

    // Toast Card Body
    QColor toastBg = m_hasPalette ? toQColor(m_palette.surfaceContainer) : QColor(30, 30, 38);
    toastBg.setAlpha(240);
    p.setPen(QPen(m_hasPalette ? toQColor(m_palette.primary) : QColor(103, 80, 164), 1.6));
    p.setBrush(toastBg);
    p.drawRoundedRect(toastRect, 8.0, 8.0);

    // App Icon Badge
    const int iconSz = tallDesktop ? 18 : 16;
    const QRect iconRect(toastRect.x() + 6, toastRect.y() + 6, iconSz, iconSz);
    p.setPen(Qt::NoPen);
    p.setBrush(m_hasPalette ? toQColor(m_palette.primaryContainer) : QColor(79, 55, 139));
    p.drawRoundedRect(iconRect, 4.0, 4.0);

    p.setPen(m_hasPalette ? toQColor(m_palette.onPrimaryContainer) : Qt::white);
    QFont iconFont = p.font();
    iconFont.setPixelSize(tallDesktop ? 10 : 9);
    iconFont.setBold(true);
    p.setFont(iconFont);
    p.drawText(iconRect, Qt::AlignCenter, QStringLiteral("W"));

    // Toast Title & Time
    QFont titleFont = p.font();
    titleFont.setPixelSize(tallDesktop ? 9 : 8);
    titleFont.setBold(true);
    p.setFont(titleFont);
    QFontMetrics titleFm(titleFont);
    p.setPen(m_hasPalette ? toQColor(m_palette.onSurface) : Qt::white);
    const int titleX = iconRect.right() + 5;
    const int nowW = titleFm.horizontalAdvance(QStringLiteral("now")) + 4;
    const int maxTitleW = std::max(24, toastRect.right() - 6 - nowW - titleX);
    p.drawText(titleX, toastRect.y() + 15,
               titleFm.elidedText(QStringLiteral("waywal-daemon"), Qt::ElideRight, maxTitleW));

    p.setPen(m_hasPalette ? toQColor(m_palette.outline) : QColor(130, 130, 140));
    p.drawText(toastRect.right() - nowW - 4, toastRect.y() + 15, QStringLiteral("now"));

    // Action button pill ("Dismiss") & Toast Body — strictly non-overlapping in both tall and compact layouts
    QFont bodyFont = p.font();
    bodyFont.setPixelSize(8);
    bodyFont.setBold(false);
    p.setFont(bodyFont);
    QFontMetrics bodyFm(bodyFont);

    const int btnW = tallDesktop ? 46 : 38;
    const int btnH = 13;
    const QRect btnRect(toastRect.right() - btnW - 5, toastRect.bottom() - btnH - 4, btnW, btnH);

    const int bodyAvailW = tallDesktop
                               ? std::max(24, toastRect.width() - 12)
                               : std::max(20, btnRect.left() - (toastRect.x() + 6) - 4);
    const QRect bodyTextRect(toastRect.x() + 6,
                             tallDesktop ? (toastRect.y() + 25) : btnRect.y(),
                             bodyAvailW, 14);

    p.setPen(m_hasPalette ? toQColor(m_palette.onSurfaceVariant) : QColor(180, 180, 190));
    p.drawText(bodyTextRect, Qt::AlignLeft | Qt::AlignVCenter,
               bodyFm.elidedText(QStringLiteral("Palette tokens synchronized to desktop"),
                                 Qt::ElideRight, bodyAvailW));

    p.setPen(Qt::NoPen);
    p.setBrush(m_hasPalette ? toQColor(m_palette.surfaceContainerHigh) : QColor(50, 50, 60));
    p.drawRoundedRect(btnRect, 3.5, 3.5);

    p.setPen(m_hasPalette ? toQColor(m_palette.onSurface) : Qt::white);
    p.drawText(btnRect, Qt::AlignCenter, QStringLiteral("Dismiss"));
}

void DesktopPreviewMockup::drawLockscreenView(QPainter &p, const QRect &r)
{
    // 1. Kawase blurred wallpaper
    updateBlurredWallpaper();
    if (!m_blurredWallpaper.isNull()) {
        p.drawImage(r, m_blurredWallpaper);
    }

    // 2. Lockscreen mode banner at top
    const QRect bannerRect(r.center().x() - 110, r.y() + 14, 220, 22);
    p.setPen(Qt::NoPen);
    QColor bannerBg = m_hasPalette ? toQColor(m_palette.secondaryContainer) : QColor(74, 68, 88);
    bannerBg.setAlpha(210);
    p.setBrush(bannerBg);
    p.drawRoundedRect(bannerRect, 11.0, 11.0);

    QFont bannerFont = p.font();
    bannerFont.setPointSize(7);
    bannerFont.setBold(true);
    p.setFont(bannerFont);
    p.setPen(m_hasPalette ? toQColor(m_palette.onSecondaryContainer) : Qt::white);
    p.drawText(bannerRect, Qt::AlignCenter, QStringLiteral("🔒 Kawase Blur Lockscreen Preview"));

    // 3. Digital Clock
    const int clockY = r.center().y() - 50;
    QFont clockFont = p.font();
    clockFont.setPointSize(34);
    clockFont.setBold(true);
    p.setFont(clockFont);

    const QString timeStr = QStringLiteral("15:42");
    p.setPen(QColor(0, 0, 0, 90));
    p.drawText(r.adjusted(2, clockY + 2, 2, 0), Qt::AlignHCenter | Qt::AlignTop, timeStr);

    p.setPen(m_hasPalette ? toQColor(m_palette.onSurface) : Qt::white);
    p.drawText(r.adjusted(0, clockY, 0, 0), Qt::AlignHCenter | Qt::AlignTop, timeStr);

    // 4. Date
    QFont dateFont = p.font();
    dateFont.setPointSize(9);
    dateFont.setBold(false);
    p.setFont(dateFont);
    p.setPen(m_hasPalette ? toQColor(m_palette.onSurfaceVariant) : QColor(210, 210, 220));
    p.drawText(r.adjusted(0, clockY + 54, 0, 0), Qt::AlignHCenter | Qt::AlignTop,
               QStringLiteral("Thursday, November 6"));

    // 5. User Avatar Circle & Name
    const int avatarCenterY = clockY + 98;
    const int avatarRadius = 24;
    const QPoint avatarCenter(r.center().x(), avatarCenterY);

    p.setPen(QPen(m_hasPalette ? toQColor(m_palette.primary) : QColor(103, 80, 164), 2.5));
    p.setBrush(m_hasPalette ? toQColor(m_palette.surfaceContainerHigh) : QColor(50, 50, 60));
    p.drawEllipse(avatarCenter, avatarRadius, avatarRadius);

    QFont avatarFont = p.font();
    avatarFont.setPointSize(14);
    avatarFont.setBold(true);
    p.setFont(avatarFont);
    p.setPen(m_hasPalette ? toQColor(m_palette.onSurface) : Qt::white);
    p.drawText(QRect(avatarCenter.x() - avatarRadius, avatarCenter.y() - avatarRadius,
                     avatarRadius * 2, avatarRadius * 2),
               Qt::AlignCenter, QStringLiteral("E"));

    QFont userFont = p.font();
    userFont.setPointSize(8);
    userFont.setBold(true);
    p.setFont(userFont);
    p.drawText(r.adjusted(0, avatarCenterY + avatarRadius + 6, 0, 0),
               Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("execorn"));

    // 6. Password Input Pill with focus ring
    const int passY = avatarCenterY + avatarRadius + 28;
    const QRect passRect(r.center().x() - 90, passY, 180, 28);

    p.setPen(QPen(m_hasPalette ? toQColor(m_palette.primary) : QColor(103, 80, 164), 2.0));
    p.setBrush(m_hasPalette ? toQColor(m_palette.surfaceContainerHigh) : QColor(36, 36, 46));
    p.drawRoundedRect(passRect, 14.0, 14.0);

    QFont dotFont = p.font();
    dotFont.setPointSize(10);
    p.setFont(dotFont);
    p.setPen(m_hasPalette ? toQColor(m_palette.primary) : QColor(103, 80, 164));
    p.drawText(passRect.adjusted(14, 0, -32, 0), Qt::AlignVCenter | Qt::AlignLeft,
               QStringLiteral("••••••••"));

    // Enter arrow pill button
    const QRect enterPill(passRect.right() - 24, passRect.y() + 4, 20, 20);
    p.setPen(Qt::NoPen);
    p.setBrush(m_hasPalette ? toQColor(m_palette.primaryContainer) : QColor(79, 55, 139));
    p.drawEllipse(enterPill);

    p.setPen(m_hasPalette ? toQColor(m_palette.onPrimaryContainer) : Qt::white);
    QFont arrowFont = p.font();
    arrowFont.setPointSize(8);
    p.setFont(arrowFont);
    p.drawText(enterPill, Qt::AlignCenter, QStringLiteral("➔"));
}

} // namespace waywal::studio
