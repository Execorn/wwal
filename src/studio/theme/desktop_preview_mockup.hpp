// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "waywal/palette_hct.h"

#include <QColor>
#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>
#include <QWidget>

namespace waywal::studio {

/**
 * @brief Live miniature vector mockup rendering simulated desktop UI components
 *        using Material You HCT extracted tokens.
 *
 * Simulates:
 * - Hyprland active window with 45° gradient border ($primary -> $tertiary)
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

  protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

  private:
    void ensureWallpaperValid();
    void updateBlurredWallpaper();

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
    QImage m_blurredWallpaper;
    bool m_lockscreenPreview{false};
    bool m_blurDirty{true};
};

} // namespace waywal::studio
