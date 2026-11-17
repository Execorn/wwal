// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "waywal/palette_hct.h"

#include <QColor>
#include <QImage>
#include <QPixmap>
#include <QRect>
#include <QSize>
#include <QString>
#include <QWidget>

#include <cstdint>

namespace waywal::studio {

/**
 * @brief Live miniature vector mockup rendering simulated desktop UI components
 *        using Material You HCT extracted tokens.
 *
 * Simulates:
 * - Hyprland active window with 45 deg gradient border ($primary -> $tertiary)
 * - Hyprland inactive window with subtle border ($outline_variant)
 * - Waybar top bar with modular pills (workspaces, window title, sys-tray, clock)
 * - Kitty terminal prompt with 16 ANSI colors test grid
 * - Mako notification toast with action pills
 * - Lockscreen Kawase blur preview mode
 */
class DesktopPreviewMockup : public QWidget {
    Q_OBJECT

  public:
    explicit DesktopPreviewMockup(QWidget *parent = nullptr);
    ~DesktopPreviewMockup() override = default;

    void setPalette(const waywal_palette_t &palette);
    [[nodiscard]] const waywal_palette_t &palette() const { return m_palette; }
    [[nodiscard]] bool hasPalette() const { return m_hasPalette; }

    void setWallpaperImage(const QImage &image);
    [[nodiscard]] const QImage &wallpaperImage() const { return m_wallpaperImage; }

    void setLockscreenPreview(bool enabled);
    [[nodiscard]] bool isLockscreenPreview() const { return m_lockscreenPreview; }

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  public Q_SLOTS:
    void toggleLockscreenPreview();

  Q_SIGNALS:
    void lockscreenPreviewChanged(bool enabled);

  protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool event(QEvent *event) override;

  private:
    [[nodiscard]] QImage buildProceduralWallpaper(int pixW, int pixH) const;
    [[nodiscard]] QImage scaleCoverWallpaper(const QImage &src, int pixW, int pixH) const;
    void ensureDesktopSurface(const QSize &logicalSize, qreal dpr);
    void ensureLockscreenSurface(const QSize &logicalSize, qreal dpr);

    void drawDesktopView(QPainter &p, const QRect &targetRect);
    void drawLockscreenView(QPainter &p, const QRect &targetRect);

    void drawWaybar(QPainter &p, const QRect &desktopRect);
    void drawHyprlandWindows(QPainter &p, const QRect &desktopRect);
    void drawKittyTerminal(QPainter &p, const QRect &windowRect);
    void drawMakoToast(QPainter &p, const QRect &desktopRect);

    static QColor toQColor(const color_rgba_t &c);
    static QString toHex(const color_rgba_t &c);

    waywal_palette_t m_palette{};
    bool m_hasPalette{false};

    QImage m_wallpaperImage;
    bool m_lockscreenPreview{false};

    uint64_t m_sourceSerial{1};
    uint64_t m_paletteSerial{1};

    QPixmap m_cachedDesktopPixmap;
    QSize m_cachedDesktopPixelSize;
    qreal m_cachedDesktopDpr{0.0};
    uint64_t m_cachedDesktopSourceSerial{0};
    uint64_t m_cachedDesktopPaletteSerial{0};

    QPixmap m_cachedLockscreenPixmap;
    QSize m_cachedLockscreenPixelSize;
    qreal m_cachedLockscreenDpr{0.0};
    uint64_t m_cachedLockscreenSourceSerial{0};
    uint64_t m_cachedLockscreenPaletteSerial{0};
};

} // namespace waywal::studio
