// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/theme/desktop_preview_mockup.hpp"

#include <QDateTime>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QLinearGradient>
#include <QLocale>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QSysInfo>

#include <algorithm>
#include <cmath>

namespace waywal::studio {

namespace {

inline int safeClampInt(int val, int lo, int hi) noexcept
{
    return (lo <= hi) ? std::clamp(val, lo, hi) : lo;
}

QString sanitizeAsciiLabel(const QString &raw, const QString &fallback)
{
    QString out;
    out.reserve(raw.size());
    for (QChar ch : raw) {
        const ushort u = ch.unicode();
        if (u >= 0x20 && u <= 0x7E) {
            out.append(ch);
        }
    }
    out = out.trimmed();
    return out.isEmpty() ? fallback : out;
}

QString previewUsername()
{
    QString user = qEnvironmentVariable("USER");
    if (user.isEmpty()) {
        user = qEnvironmentVariable("LOGNAME");
    }
    return sanitizeAsciiLabel(user, QStringLiteral("user"));
}

QString previewHostname()
{
    const QString host = QSysInfo::machineHostName().section(QLatin1Char('.'), 0, 0);
    return sanitizeAsciiLabel(host, QStringLiteral("waywal"));
}

} // namespace

DesktopPreviewMockup::DesktopPreviewMockup(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void DesktopPreviewMockup::setPalette(const waywal_palette_t &palette)
{
    m_palette = palette;
    m_hasPalette = true;
    ++m_paletteSerial;
    if (width() > 0 && height() > 0) {
        const qreal dpr = std::max(1.0, devicePixelRatioF());
        if (m_wallpaperImage.isNull()) {
            ensureDesktopSurface(size(), dpr);
        }
        if (m_lockscreenPreview) {
            ensureLockscreenSurface(size(), dpr);
        }
    }
    update();
}

void DesktopPreviewMockup::setWallpaperImage(const QImage &image)
{
    m_wallpaperImage = image;
    ++m_sourceSerial;
    if (width() > 0 && height() > 0) {
        const qreal dpr = std::max(1.0, devicePixelRatioF());
        ensureDesktopSurface(size(), dpr);
        if (m_lockscreenPreview) {
            ensureLockscreenSurface(size(), dpr);
        }
    }
    update();
}

void DesktopPreviewMockup::setLockscreenPreview(bool enabled)
{
    if (m_lockscreenPreview != enabled) {
        m_lockscreenPreview = enabled;
        if (m_lockscreenPreview && width() > 0 && height() > 0) {
            ensureLockscreenSurface(size(), std::max(1.0, devicePixelRatioF()));
        }
        Q_EMIT lockscreenPreviewChanged(m_lockscreenPreview);
        update();
    }
}

void DesktopPreviewMockup::toggleLockscreenPreview() { setLockscreenPreview(!m_lockscreenPreview); }

QSize DesktopPreviewMockup::sizeHint() const { return {640, 360}; }

QSize DesktopPreviewMockup::minimumSizeHint() const { return {170, 120}; }

void DesktopPreviewMockup::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (width() > 0 && height() > 0) {
        const qreal dpr = std::max(1.0, devicePixelRatioF());
        ensureDesktopSurface(size(), dpr);
        if (m_lockscreenPreview) {
            ensureLockscreenSurface(size(), dpr);
        }
    }
}

bool DesktopPreviewMockup::event(QEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    if (event && event->type() == QEvent::DevicePixelRatioChange) {
        if (width() > 0 && height() > 0) {
            const qreal dpr = std::max(1.0, devicePixelRatioF());
            ensureDesktopSurface(size(), dpr);
            if (m_lockscreenPreview) {
                ensureLockscreenSurface(size(), dpr);
            }
        }
        update();
    }
#endif
    return QWidget::event(event);
}

QColor DesktopPreviewMockup::toQColor(const color_rgba_t &c) { return QColor(c.r, c.g, c.b, c.a); }

QString DesktopPreviewMockup::toHex(const color_rgba_t &c)
{
    return QString::asprintf("#%02x%02x%02x", c.r, c.g, c.b);
}

QImage DesktopPreviewMockup::buildProceduralWallpaper(int pixW, int pixH) const
{
    const int w = std::max(1, pixW);
    const int h = std::max(1, pixH);
    QImage synthetic(w, h, QImage::Format_ARGB32_Premultiplied);

    const QColor base = m_hasPalette ? toQColor(m_palette.background) : QColor(18, 19, 24);
    const QColor pri = m_hasPalette ? toQColor(m_palette.primary) : QColor(103, 80, 164);
    const QColor sec = m_hasPalette ? toQColor(m_palette.secondary) : QColor(98, 91, 113);
    const QColor ter = m_hasPalette ? toQColor(m_palette.tertiary) : QColor(125, 82, 96);

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
    return synthetic;
}

QImage DesktopPreviewMockup::scaleCoverWallpaper(const QImage &src, int pixW, int pixH) const
{
    const int targetW = std::max(1, pixW);
    const int targetH = std::max(1, pixH);
    if (src.isNull()) {
        return buildProceduralWallpaper(targetW, targetH);
    }

    QImage scaled =
        src.scaled(targetW, targetH, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    if (scaled.width() == targetW && scaled.height() == targetH) {
        return scaled;
    }
    const int cropX = std::max(0, (scaled.width() - targetW) / 2);
    const int cropY = std::max(0, (scaled.height() - targetH) / 2);
    return scaled.copy(cropX, cropY, std::min(targetW, scaled.width()),
                       std::min(targetH, scaled.height()));
}

void DesktopPreviewMockup::ensureDesktopSurface(const QSize &logicalSize, qreal dpr)
{
    const int pixW = std::max(1, static_cast<int>(std::lround(logicalSize.width() * dpr)));
    const int pixH = std::max(1, static_cast<int>(std::lround(logicalSize.height() * dpr)));
    const QSize targetPx(pixW, pixH);
    const uint64_t palSerial = m_wallpaperImage.isNull() ? m_paletteSerial : 0ULL;

    if (!m_cachedDesktopPixmap.isNull() && m_cachedDesktopPixelSize == targetPx &&
        std::abs(m_cachedDesktopDpr - dpr) <= 1e-4 &&
        m_cachedDesktopSourceSerial == m_sourceSerial &&
        m_cachedDesktopPaletteSerial == palSerial) {
        return;
    }

    const QImage surface = m_wallpaperImage.isNull()
                               ? buildProceduralWallpaper(pixW, pixH)
                               : scaleCoverWallpaper(m_wallpaperImage, pixW, pixH);

    m_cachedDesktopPixmap = QPixmap::fromImage(surface);
    m_cachedDesktopPixmap.setDevicePixelRatio(dpr);
    m_cachedDesktopPixelSize = targetPx;
    m_cachedDesktopDpr = dpr;
    m_cachedDesktopSourceSerial = m_sourceSerial;
    m_cachedDesktopPaletteSerial = palSerial;
}

void DesktopPreviewMockup::ensureLockscreenSurface(const QSize &logicalSize, qreal dpr)
{
    const int pixW = std::max(1, static_cast<int>(std::lround(logicalSize.width() * dpr)));
    const int pixH = std::max(1, static_cast<int>(std::lround(logicalSize.height() * dpr)));
    const QSize targetPx(pixW, pixH);

    if (!m_cachedLockscreenPixmap.isNull() && m_cachedLockscreenPixelSize == targetPx &&
        std::abs(m_cachedLockscreenDpr - dpr) <= 1e-4 &&
        m_cachedLockscreenSourceSerial == m_sourceSerial &&
        m_cachedLockscreenPaletteSerial == m_paletteSerial) {
        return;
    }

    const int w2 = std::max(1, pixW / 2);
    const int h2 = std::max(1, pixH / 2);
    const int w4 = std::max(1, pixW / 4);
    const int h4 = std::max(1, pixH / 4);
    const int w8 = std::max(1, pixW / 8);
    const int h8 = std::max(1, pixH / 8);

    // Aspect-preserving cover crop at half resolution before Kawase downsample/upsample passes.
    QImage pass1 = scaleCoverWallpaper(m_wallpaperImage, w2, h2);
    QImage pass2 = pass1.scaled(w4, h4, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage pass3 = pass2.scaled(w8, h8, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage pass4 = pass3.scaled(w4, h4, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage blurred = pass4.scaled(pixW, pixH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (blurred.format() != QImage::Format_ARGB32_Premultiplied) {
        blurred = blurred.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    QPainter bp(&blurred);
    QColor tint = m_hasPalette ? toQColor(m_palette.surface) : QColor(25, 28, 30);
    tint.setAlpha(120);
    bp.fillRect(blurred.rect(), tint);
    bp.end();

    m_cachedLockscreenPixmap = QPixmap::fromImage(blurred);
    m_cachedLockscreenPixmap.setDevicePixelRatio(dpr);
    m_cachedLockscreenPixelSize = targetPx;
    m_cachedLockscreenDpr = dpr;
    m_cachedLockscreenSourceSerial = m_sourceSerial;
    m_cachedLockscreenPaletteSerial = m_paletteSerial;
}

void DesktopPreviewMockup::paintEvent(QPaintEvent * /*event*/)
{
    const QRect bounds = rect();
    if (bounds.isEmpty()) {
        return;
    }

    const qreal dpr = std::max(1.0, devicePixelRatioF());
    if (m_lockscreenPreview) {
        ensureLockscreenSurface(bounds.size(), dpr);
    } else {
        ensureDesktopSurface(bounds.size(), dpr);
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    QPainterPath containerClip;
    containerClip.addRoundedRect(bounds, 12.0, 12.0);
    p.setClipPath(containerClip);

    if (m_lockscreenPreview) {
        drawLockscreenView(p, bounds);
    } else {
        drawDesktopView(p, bounds);
    }

    p.setClipping(false);
    p.setPen(QPen(m_hasPalette ? toQColor(m_palette.outlineVariant) : QColor(80, 80, 90), 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(bounds.adjusted(1, 1, -1, -1), 12.0, 12.0);
}

void DesktopPreviewMockup::drawDesktopView(QPainter &p, const QRect &r)
{
    if (!m_cachedDesktopPixmap.isNull()) {
        p.drawPixmap(r, m_cachedDesktopPixmap);
    }

    drawWaybar(p, r);
    drawHyprlandWindows(p, r);
    drawMakoToast(p, r);
}

void DesktopPreviewMockup::drawWaybar(QPainter &p, const QRect &desktopRect)
{
    const bool compact = desktopRect.width() < 380;
    const int barMargin = compact ? 6 : 10;
    const int barH = compact ? 22 : 28;
    const QRect barRect(desktopRect.x() + barMargin, desktopRect.y() + barMargin,
                        std::max(40, desktopRect.width() - (barMargin * 2)), barH);

    QColor barBg = m_hasPalette ? toQColor(m_palette.surfaceContainerLowest) : QColor(20, 20, 26);
    barBg.setAlpha(215);

    p.setPen(QPen(m_hasPalette ? toQColor(m_palette.outlineVariant) : QColor(60, 60, 70), 1.0));
    p.setBrush(barBg);
    p.drawRoundedRect(barRect, 8.0, 8.0);

    int wsX = barRect.x() + 6;
    const int wsY = barRect.y() + 3;
    const int wsH = barH - 6;
    const int wsW = compact ? 15 : 20;
    const int wsCount = compact ? 3 : 5;

    QFont wsFont = p.font();
    wsFont.setPixelSize(compact ? 9 : 11);
    wsFont.setBold(true);
    p.setFont(wsFont);

    for (int i = 1; i <= wsCount; ++i) {
        if (wsX + wsW > barRect.right() - 6) {
            break;
        }
        const QRect wsPill(wsX, wsY, wsW, wsH);
        if (i == 1) {
            const QColor actBg = m_hasPalette ? toQColor(m_palette.primary) : QColor(103, 80, 164);
            const QColor actFg = m_hasPalette ? toQColor(m_palette.onPrimary) : Qt::white;
            p.setPen(Qt::NoPen);
            p.setBrush(actBg);
            p.drawRoundedRect(wsPill, 5.0, 5.0);
            p.setPen(actFg);
        } else {
            const QColor inactBg =
                m_hasPalette ? toQColor(m_palette.surfaceContainerHigh) : QColor(40, 40, 50);
            const QColor inactFg =
                m_hasPalette ? toQColor(m_palette.onSurfaceVariant) : QColor(180, 180, 190);
            p.setPen(Qt::NoPen);
            p.setBrush(inactBg);
            p.drawRoundedRect(wsPill, 5.0, 5.0);
            p.setPen(inactFg);
        }
        p.drawText(wsPill, Qt::AlignCenter, QString::number(i));
        wsX += wsW + 3;
    }

    const QFontMetrics fm(wsFont);
    const QString clockLabel =
        QLocale::c().toString(QDateTime::currentDateTime(), QStringLiteral("HH:mm"));

    int trayX = barRect.right() - 6;
    const auto drawTrayPill = [&](const QString &label, const color_rgba_t &accent,
                                  bool isPrimary) {
        const int w = fm.horizontalAdvance(label) + (compact ? 10 : 14);
        if (trayX - w <= wsX + 8) {
            return;
        }
        trayX -= w;
        const QRect pill(trayX, wsY, w, wsH);

        const QColor bg =
            m_hasPalette ? toQColor(m_palette.surfaceContainerHigh) : QColor(40, 40, 50);
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawRoundedRect(pill, 5.0, 5.0);

        p.setPen(m_hasPalette ? toQColor(accent) : (isPrimary ? QColor(103, 80, 164) : Qt::white));
        p.drawText(pill, Qt::AlignCenter, label);
        trayX -= 4;
    };

    drawTrayPill(clockLabel, m_palette.primary, true);
    drawTrayPill(QStringLiteral("98%"), m_palette.tertiary, false);
    if (!compact) {
        drawTrayPill(QStringLiteral("18%"), m_palette.secondary, false);
        drawTrayPill(QStringLiteral("Vol 75%"), m_palette.primaryFixedDim, false);
    }

    const int availCenterW = trayX - wsX - 12;
    if (availCenterW >= 48) {
        const QString titleText = QStringLiteral("WayWal Studio - Live Theme");
        const int titleW = std::min(fm.horizontalAdvance(titleText) + 18, availCenterW);
        const int titleX = wsX + 6 + (availCenterW - titleW) / 2;
        const QRect titleRect(titleX, wsY, titleW, wsH);

        QColor titleBg =
            m_hasPalette ? toQColor(m_palette.surfaceContainerHigh) : QColor(40, 40, 50);
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
    const int usableW = std::max(60, desktopRect.width());
    const bool tallDesktop = (usableH >= 200);

    const int inactPadX = compact ? 8 : 16;
    const int maxInactW = std::max(40, usableW - inactPadX * 2);
    const int minInactW = std::min(85, maxInactW);
    const int inactW =
        safeClampInt(static_cast<int>(usableW * 0.42), minInactW, std::max(minInactW, 380));
    const int maxInactH = std::max(36, usableH - 8);
    const int minInactH = std::min(56, maxInactH);
    const int inactH = safeClampInt(static_cast<int>(usableH * (tallDesktop ? 0.50 : 0.68)),
                                    minInactH, std::max(minInactH, 260));
    const QRect inactRect(desktopRect.x() + inactPadX, usableTop + (compact ? 3 : 6), inactW,
                          inactH);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 50));
    p.drawRoundedRect(inactRect.translated(3, 4), 8.0, 8.0);

    const QColor inactBg = m_hasPalette ? toQColor(m_palette.surfaceDim) : QColor(24, 24, 30);
    p.setPen(QPen(m_hasPalette ? toQColor(m_palette.outlineVariant) : QColor(70, 70, 80), 1.8));
    p.setBrush(inactBg);
    p.drawRoundedRect(inactRect, 8.0, 8.0);

    const QRect inactTitleBar(inactRect.x(), inactRect.y(), inactRect.width(), 20);
    QPainterPath inactTitleClip;
    inactTitleClip.addRoundedRect(inactRect, 8.0, 8.0);
    p.setClipPath(inactTitleClip);

    p.setPen(Qt::NoPen);
    p.setBrush(m_hasPalette ? toQColor(m_palette.surfaceContainerLow) : QColor(32, 32, 40));
    p.drawRect(inactTitleBar);

    for (int i = 0; i < 3; ++i) {
        p.setBrush(m_hasPalette ? toQColor(m_palette.outline) : QColor(90, 90, 100));
        p.drawEllipse(inactTitleBar.x() + 7 + (i * 11), inactTitleBar.y() + 6, 7, 7);
    }

    QFont inactFont = p.font();
    inactFont.setPixelSize(9);
    p.setFont(inactFont);
    const QFontMetrics inactFm(inactFont);
    p.setPen(m_hasPalette ? toQColor(m_palette.onSurfaceVariant) : QColor(160, 160, 170));
    const QRect inactTitleTextRect = inactTitleBar.adjusted(42, 0, -6, 0);
    p.drawText(inactTitleTextRect, Qt::AlignVCenter | Qt::AlignLeft,
               inactFm.elidedText(QStringLiteral("Files - ~/themes"), Qt::ElideRight,
                                  std::max(20, inactTitleTextRect.width())));

    const int rowCount = std::min(6, std::max(0, (inactRect.height() - 28) / 15));
    for (int r = 0; r < rowCount; ++r) {
        const QRect row(inactRect.x() + 10, inactRect.y() + 26 + (r * 15),
                        std::max(10, inactRect.width() - 20), 11);
        p.setPen(Qt::NoPen);
        p.setBrush(m_hasPalette ? toQColor(m_palette.surfaceContainerHigh) : QColor(38, 38, 48));
        p.drawRoundedRect(row, 3.0, 3.0);
    }
    p.setClipping(false);

    QRect actRect;
    if (tallDesktop) {
        const int toastReserveH = std::min(74, std::max(44, usableH / 3));
        const int horizMargin = compact ? 8 : 12;
        const int maxActW = std::max(60, usableW - horizMargin * 2);
        const int minActW = std::min(210, maxActW);
        const int upperActW = std::max(minActW, std::min(480, maxActW));
        const int actW = safeClampInt(static_cast<int>(usableW * 0.84), minActW, upperActW);

        const int availActH = std::max(40, usableH - toastReserveH - 6);
        const int minActH = std::min(100, availActH);
        const int actH = safeClampInt(availActH, minActH, std::max(minActH, 360));

        const int loX = desktopRect.x() + std::min(horizMargin, std::max(4, usableW - actW - 4));
        const int hiX = std::max(loX, desktopRect.right() - actW - 6);
        const int actX = safeClampInt(desktopRect.right() - actW - 10, loX, hiX);
        const int actY = usableTop + toastReserveH;
        actRect = QRect(actX, actY, actW, actH);
    } else {
        const int maxToastW = std::max(60, usableW - 24);
        const int minToastW = std::min(96, maxToastW);
        const int toastW = safeClampInt(static_cast<int>(usableW * 0.36), minToastW,
                                        std::max(minToastW, std::min(176, maxToastW)));
        const int availLeftW = std::max(64, usableW - toastW - (compact ? 14 : 22));
        const int minActW = std::min(114, availLeftW);
        const int actW =
            safeClampInt(availLeftW - (compact ? 4 : 8), minActW, std::max(minActW, 340));
        const int availActH = std::max(40, usableH - 4);
        const int minActH = std::min(78, availActH);
        const int actH = safeClampInt(availActH, minActH, std::max(minActH, 180));
        const int actX = desktopRect.x() + (compact ? 8 : 14);
        const int actY = usableTop + std::max(2, usableH - actH - 1);
        actRect = QRect(actX, actY, actW, actH);
    }

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 85));
    p.drawRoundedRect(actRect.translated(5, 6), 9.0, 9.0);

    QLinearGradient gradBorder(actRect.topLeft(), actRect.bottomRight());
    gradBorder.setColorAt(0.0, m_hasPalette ? toQColor(m_palette.primary) : QColor(103, 80, 164));
    gradBorder.setColorAt(1.0, m_hasPalette ? toQColor(m_palette.tertiary) : QColor(125, 82, 96));

    // Match TemplateExporter::formatKitty() terminal background token (surfaceContainerLowest).
    p.setPen(QPen(QBrush(gradBorder), 3.0));
    p.setBrush(m_hasPalette ? toQColor(m_palette.surfaceContainerLowest) : QColor(16, 17, 22));
    p.drawRoundedRect(actRect, 8.0, 8.0);

    drawKittyTerminal(p, actRect.adjusted(3, 3, -3, -3));
}

void DesktopPreviewMockup::drawKittyTerminal(QPainter &p, const QRect &windowRect)
{
    if (windowRect.width() < 24 || windowRect.height() < 24) {
        return;
    }

    QPainterPath winClip;
    winClip.addRoundedRect(windowRect, 6.0, 6.0);
    p.setClipPath(winClip);

    const bool compactTerm = (windowRect.width() < 230 || windowRect.height() < 105);

    const int tabH = compactTerm ? 16 : 20;
    const QRect tabRect(windowRect.x(), windowRect.y(), windowRect.width(), tabH);
    p.setPen(Qt::NoPen);
    p.setBrush(m_hasPalette ? toQColor(m_palette.surfaceContainerLow) : QColor(28, 28, 36));
    p.drawRect(tabRect);

    const int dotSize = compactTerm ? 6 : 8;
    const int dotY = tabRect.y() + (tabH - dotSize) / 2;
    p.setBrush(m_hasPalette ? toQColor(m_palette.error) : QColor(255, 95, 87));
    p.drawEllipse(tabRect.x() + 6, dotY, dotSize, dotSize);

    p.setBrush(m_hasPalette ? toQColor(m_palette.tertiary) : QColor(254, 188, 46));
    p.drawEllipse(tabRect.x() + 6 + dotSize + 4, dotY, dotSize, dotSize);

    p.setBrush(m_hasPalette ? toQColor(m_palette.primary) : QColor(40, 201, 64));
    p.drawEllipse(tabRect.x() + 6 + 2 * (dotSize + 4), dotY, dotSize, dotSize);

    const int pillX = tabRect.x() + 6 + 3 * (dotSize + 4) + 2;
    const int availPillW = std::max(16, windowRect.right() - pillX - 6);
    const int pillW = safeClampInt(availPillW, std::min(48, availPillW), std::max(48, 108));
    const QRect activeTab(pillX, tabRect.y() + 2, pillW, tabH - 2);
    p.setBrush(m_hasPalette ? toQColor(m_palette.surfaceContainerHigh) : QColor(42, 42, 54));
    p.drawRoundedRect(activeTab, 4.0, 4.0);

    QFont tabFont = p.font();
    tabFont.setPixelSize(compactTerm ? 8 : 9);
    tabFont.setBold(true);
    p.setFont(tabFont);
    p.setPen(m_hasPalette ? toQColor(m_palette.onSurface) : Qt::white);
    const QFontMetrics tabFm(tabFont);
    p.drawText(
        activeTab, Qt::AlignCenter,
        tabFm.elidedText(QStringLiteral("kitty: ~"), Qt::ElideRight, std::max(8, pillW - 6)));

    const QRect bodyRect(windowRect.x() + 5, windowRect.y() + tabH + 2,
                         std::max(12, windowRect.width() - 10),
                         std::max(12, windowRect.height() - tabH - 4));

    QFont termFont(QStringLiteral("Monospace"));
    termFont.setStyleHint(QFont::Monospace);
    termFont.setPixelSize(compactTerm ? 8 : 9);
    p.setFont(termFont);
    const QFontMetrics termFm(termFont);

    int textY = bodyRect.y() + (compactTerm ? 8 : 10);
    const int lineStep = compactTerm ? 10 : 12;

    int cursorX = bodyRect.x() + 2;
    const int maxRight = bodyRect.right() - 2;

    const auto drawSegment = [&](const QString &seg, const QColor &col) {
        if (cursorX >= maxRight) {
            return;
        }
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

    drawSegment(previewUsername(), m_hasPalette ? toQColor(m_palette.term2) : QColor(80, 250, 123));
    drawSegment(QStringLiteral("@"),
                m_hasPalette ? toQColor(m_palette.onSurfaceVariant) : QColor(200, 200, 210));
    drawSegment(previewHostname(),
                m_hasPalette ? toQColor(m_palette.term6) : QColor(139, 233, 253));
    drawSegment(QStringLiteral(":"),
                m_hasPalette ? toQColor(m_palette.onSurfaceVariant) : QColor(200, 200, 210));
    drawSegment(QStringLiteral("~/themes"),
                m_hasPalette ? toQColor(m_palette.term4) : QColor(98, 114, 164));
    drawSegment(QStringLiteral(" $ "),
                m_hasPalette ? toQColor(m_palette.primary) : QColor(189, 147, 249));
    drawSegment(compactTerm ? QStringLiteral("colorscript") : QStringLiteral("colorscript -e m3"),
                m_hasPalette ? toQColor(m_palette.onSurface) : Qt::white);

    textY += lineStep;

    p.setPen(m_hasPalette ? toQColor(m_palette.onSurfaceVariant) : QColor(170, 170, 180));
    const QString infoStr = compactTerm
                                ? QStringLiteral("M3 HCT | OkLab | 36+16")
                                : QStringLiteral("Material You HCT | OkLab | 36 M3 + 16 ANSI");
    p.drawText(bodyRect.x() + 2, textY,
               termFm.elidedText(infoStr, Qt::ElideRight, std::max(16, bodyRect.width() - 4)));

    const int sqGap = compactTerm ? 2 : 3;
    const int ansiStartY = textY + (compactTerm ? 3 : 4);
    const int availAnsiH = std::max(8, bodyRect.bottom() - ansiStartY - 1);
    const int maxSqByW = std::max(2, (bodyRect.width() - 4 - 7 * sqGap) / 8);
    const int maxSqByH = std::max(2, (availAnsiH - sqGap) / 2);
    const int rawSq = std::min(maxSqByW, maxSqByH);
    const int minSq = std::min(5, rawSq);
    const int sqSize = safeClampInt(rawSq, minSq, std::max(minSq, 22));

    for (int col = 0; col < 8; ++col) {
        const QRect sq1(bodyRect.x() + 2 + (col * (sqSize + sqGap)), ansiStartY, sqSize, sqSize);
        p.setPen(QPen(QColor(0, 0, 0, 80), 0.5));
        p.setBrush(m_hasPalette ? toQColor(m_palette.term[col]) : QColor(40 + (col * 25), 40, 40));
        p.drawRoundedRect(sq1, 2.0, 2.0);

        const QRect sq2(bodyRect.x() + 2 + (col * (sqSize + sqGap)), ansiStartY + sqSize + sqGap,
                        sqSize, sqSize);
        p.setBrush(m_hasPalette ? toQColor(m_palette.term[col + 8])
                                : QColor(80 + (col * 20), 80, 80));
        p.drawRoundedRect(sq2, 2.0, 2.0);
    }

    p.setClipping(false);
}

void DesktopPreviewMockup::drawMakoToast(QPainter &p, const QRect &desktopRect)
{
    const bool compact = desktopRect.width() < 340 || desktopRect.height() < 180;
    const int usableH = std::max(60, desktopRect.height() - (compact ? 38 : 52));
    const bool tallDesktop = (usableH >= 200);
    const int maxToastW = std::max(64, desktopRect.width() - (compact ? 12 : 24));
    const int desiredMinW = std::min(tallDesktop ? 150 : 96, maxToastW);
    const int desiredMaxW = std::max(desiredMinW, std::min(tallDesktop ? 250 : 176, maxToastW));
    const int toastW =
        safeClampInt(static_cast<int>(desktopRect.width() * (tallDesktop ? 0.48 : 0.36)),
                     desiredMinW, desiredMaxW);
    const int toastH = tallDesktop ? 62 : 46;
    const QRect toastRect(desktopRect.right() - toastW - (compact ? 6 : 12),
                          desktopRect.y() + (compact ? 34 : 46), toastW, toastH);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 80));
    p.drawRoundedRect(toastRect.translated(2, 3), 8.0, 8.0);

    QColor toastBg = m_hasPalette ? toQColor(m_palette.surfaceContainer) : QColor(30, 30, 38);
    toastBg.setAlpha(240);
    p.setPen(QPen(m_hasPalette ? toQColor(m_palette.primary) : QColor(103, 80, 164), 1.6));
    p.setBrush(toastBg);
    p.drawRoundedRect(toastRect, 8.0, 8.0);

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

    QFont titleFont = p.font();
    titleFont.setPixelSize(tallDesktop ? 9 : 8);
    titleFont.setBold(true);
    p.setFont(titleFont);
    const QFontMetrics titleFm(titleFont);
    p.setPen(m_hasPalette ? toQColor(m_palette.onSurface) : Qt::white);
    const int titleX = iconRect.right() + 5;
    const int nowW = titleFm.horizontalAdvance(QStringLiteral("now")) + 4;
    const int maxTitleW = std::max(20, toastRect.right() - 6 - nowW - titleX);
    p.drawText(titleX, toastRect.y() + 15,
               titleFm.elidedText(QStringLiteral("waywal-daemon"), Qt::ElideRight, maxTitleW));

    p.setPen(m_hasPalette ? toQColor(m_palette.outline) : QColor(130, 130, 140));
    p.drawText(toastRect.right() - nowW - 4, toastRect.y() + 15, QStringLiteral("now"));

    QFont bodyFont = p.font();
    bodyFont.setPixelSize(8);
    bodyFont.setBold(false);
    p.setFont(bodyFont);
    const QFontMetrics bodyFm(bodyFont);

    const int btnW = tallDesktop ? 46 : 38;
    const int btnH = 13;
    const QRect btnRect(toastRect.right() - btnW - 5, toastRect.bottom() - btnH - 4, btnW, btnH);

    const int bodyAvailW = tallDesktop ? std::max(20, toastRect.width() - 12)
                                       : std::max(16, btnRect.left() - (toastRect.x() + 6) - 4);
    const QRect bodyTextRect(toastRect.x() + 6, tallDesktop ? (toastRect.y() + 25) : btnRect.y(),
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
    if (!m_cachedLockscreenPixmap.isNull()) {
        p.drawPixmap(r, m_cachedLockscreenPixmap);
    }

    const qreal scale = std::clamp(std::min(r.width() / 420.0, r.height() / 280.0), 0.35, 1.4);
    const bool compact = (r.width() < 260 || r.height() < 190);

    int topCursor = r.y() + (compact ? 6 : 12);
    if (r.height() >= 115) {
        const int bannerW = std::min(std::max(60, r.width() - 16),
                                     static_cast<int>(std::lround(180.0 * std::max(0.70, scale))));
        const int bannerH =
            safeClampInt(static_cast<int>(std::lround(22.0 * std::max(0.75, scale))), 16, 26);
        const QRect bannerRect(r.center().x() - bannerW / 2, topCursor, bannerW, bannerH);
        p.setPen(Qt::NoPen);
        QColor bannerBg =
            m_hasPalette ? toQColor(m_palette.secondaryContainer) : QColor(74, 68, 88);
        bannerBg.setAlpha(210);
        p.setBrush(bannerBg);
        p.drawRoundedRect(bannerRect, bannerH * 0.5, bannerH * 0.5);

        QFont bannerFont = p.font();
        bannerFont.setPixelSize(compact ? 9 : 11);
        bannerFont.setBold(true);
        p.setFont(bannerFont);
        p.setPen(m_hasPalette ? toQColor(m_palette.onSecondaryContainer) : Qt::white);
        p.drawText(bannerRect, Qt::AlignCenter,
                   QFontMetrics(bannerFont)
                       .elidedText(QStringLiteral("Lockscreen Preview"), Qt::ElideRight,
                                   std::max(16, bannerW - 10)));
        topCursor = bannerRect.bottom() + (compact ? 4 : 8);
    }

    const int bottomMargin = compact ? 6 : 12;
    const int availH = std::max(40, (r.bottom() - bottomMargin) - topCursor);
    const bool showAvatar = (availH >= 140);
    const bool showDate = (availH >= 84);

    const QDateTime now = QDateTime::currentDateTime();
    const QLocale cLoc = QLocale::c();
    const QString timeStr = cLoc.toString(now, QStringLiteral("HH:mm"));
    const QString dateStr = cLoc.toString(now, QStringLiteral("dddd, MMMM d"));
    const QString userStr = previewUsername();
    const QString initialStr = userStr.left(1).toUpper();

    QFont clockFont = p.font();
    clockFont.setPixelSize(
        safeClampInt(static_cast<int>(std::lround(44.0 * scale)), 16, std::max(16, availH / 3)));
    clockFont.setBold(true);
    const QFontMetrics clockFm(clockFont);
    const int clockH = clockFm.height();

    QFont dateFont = p.font();
    dateFont.setPixelSize(safeClampInt(static_cast<int>(std::lround(13.0 * scale)), 9, 15));
    dateFont.setBold(false);
    const QFontMetrics dateFm(dateFont);
    const int dateH = showDate ? dateFm.height() : 0;

    const int avatarRadius =
        showAvatar ? safeClampInt(static_cast<int>(std::lround(22.0 * scale)), 12, 26) : 0;
    QFont userFont = p.font();
    userFont.setPixelSize(safeClampInt(static_cast<int>(std::lround(12.0 * scale)), 9, 13));
    userFont.setBold(true);
    const QFontMetrics userFm(userFont);
    const int userH = userFm.height();

    const int passW = std::min(std::max(72, r.width() - 24),
                               safeClampInt(static_cast<int>(std::lround(190.0 * scale)), 96, 220));
    const int passH =
        safeClampInt(static_cast<int>(std::lround(28.0 * std::max(0.72, scale))), 18, 30);

    const int gap = compact ? 3 : 6;
    int totalStackH = clockH + (showDate ? (gap + dateH) : 0) +
                      (showAvatar ? (gap + avatarRadius * 2) : 0) + gap + userH + gap + passH;
    int yCursor = topCursor + std::max(0, (availH - totalStackH) / 2);

    p.setFont(clockFont);
    const QRect clockRect(r.x() + 6, yCursor, std::max(20, r.width() - 12), clockH);
    p.setPen(QColor(0, 0, 0, 90));
    p.drawText(clockRect.translated(1, 2), Qt::AlignCenter, timeStr);
    p.setPen(m_hasPalette ? toQColor(m_palette.onSurface) : Qt::white);
    p.drawText(clockRect, Qt::AlignCenter, timeStr);
    yCursor += clockH + gap;

    if (showDate) {
        p.setFont(dateFont);
        p.setPen(m_hasPalette ? toQColor(m_palette.onSurfaceVariant) : QColor(210, 210, 220));
        const QRect dateRect(r.x() + 6, yCursor, std::max(20, r.width() - 12), dateH);
        p.drawText(dateRect, Qt::AlignCenter,
                   dateFm.elidedText(dateStr, Qt::ElideRight, dateRect.width()));
        yCursor += dateH + gap;
    }

    if (showAvatar) {
        const QPoint avatarCenter(r.center().x(), yCursor + avatarRadius);
        p.setPen(QPen(m_hasPalette ? toQColor(m_palette.primary) : QColor(103, 80, 164), 2.0));
        p.setBrush(m_hasPalette ? toQColor(m_palette.surfaceContainerHigh) : QColor(50, 50, 60));
        p.drawEllipse(avatarCenter, avatarRadius, avatarRadius);

        QFont avatarFont = p.font();
        avatarFont.setPixelSize(std::max(11, avatarRadius));
        avatarFont.setBold(true);
        p.setFont(avatarFont);
        p.setPen(m_hasPalette ? toQColor(m_palette.onSurface) : Qt::white);
        p.drawText(QRect(avatarCenter.x() - avatarRadius, avatarCenter.y() - avatarRadius,
                         avatarRadius * 2, avatarRadius * 2),
                   Qt::AlignCenter, initialStr);
        yCursor += avatarRadius * 2 + gap;
    }

    p.setFont(userFont);
    p.setPen(m_hasPalette ? toQColor(m_palette.onSurface) : Qt::white);
    const QRect userRect(r.x() + 6, yCursor, std::max(20, r.width() - 12), userH);
    p.drawText(userRect, Qt::AlignCenter,
               userFm.elidedText(userStr, Qt::ElideRight, userRect.width()));
    yCursor += userH + gap;

    if (yCursor + passH <= r.bottom() - 2) {
        const QRect passRect(r.center().x() - passW / 2, yCursor, passW, passH);
        p.setPen(QPen(m_hasPalette ? toQColor(m_palette.primary) : QColor(103, 80, 164), 2.0));
        p.setBrush(m_hasPalette ? toQColor(m_palette.surfaceContainerHigh) : QColor(36, 36, 46));
        p.drawRoundedRect(passRect, passH * 0.5, passH * 0.5);

        QFont dotFont = p.font();
        dotFont.setPixelSize(std::max(9, passH / 2));
        p.setFont(dotFont);
        p.setPen(m_hasPalette ? toQColor(m_palette.primary) : QColor(103, 80, 164));
        const int enterSize = std::max(12, passH - 8);
        p.drawText(passRect.adjusted(10, 0, -(enterSize + 8), 0), Qt::AlignVCenter | Qt::AlignLeft,
                   QStringLiteral("********"));

        const QRect enterPill(passRect.right() - enterSize - 4, passRect.y() + 4, enterSize,
                              enterSize);
        p.setPen(Qt::NoPen);
        p.setBrush(m_hasPalette ? toQColor(m_palette.primaryContainer) : QColor(79, 55, 139));
        p.drawEllipse(enterPill);

        p.setPen(m_hasPalette ? toQColor(m_palette.onPrimaryContainer) : Qt::white);
        QFont arrowFont = p.font();
        arrowFont.setPixelSize(std::max(8, enterSize - 6));
        arrowFont.setBold(true);
        p.setFont(arrowFont);
        p.drawText(enterPill, Qt::AlignCenter, QStringLiteral(">"));
    }
}

} // namespace waywal::studio
