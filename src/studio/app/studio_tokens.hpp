/**
 * @file studio_tokens.hpp
 * @brief Shared dark-mode color, typography, focus-ring, and proxy-style tokens for Studio chrome.
 */
#pragma once

#include <QColor>
#include <QFont>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QProxyStyle>
#include <QPushButton>
#include <QString>
#include <QStyleFactory>
#include <QStyleOption>
#include <QWidget>

#include <algorithm>

namespace wwal::studio::tokens {

// Surface hierarchy matching dark studio grading surfaces (#0F1117 / #161922).
inline constexpr const char *kBgCanvasHex = "#0f1117";
inline constexpr const char *kSurfaceInsetHex = "#14171f";
inline constexpr const char *kSurfacePanelHex = "#161922";
inline constexpr const char *kSurfaceInteractiveHex = "#20242e";
inline constexpr const char *kSurfaceHoverHex = "#2a303d";
inline constexpr const char *kSurfacePressedHex = "#161920";

// Border and hairline dividers.
inline constexpr const char *kBorderSubtleHex = "#262b36";
inline constexpr const char *kBorderMenuHex = "#2c3240";
inline constexpr const char *kBorderControlHex = "#2e3545";

// Text colors meeting >= 4.5:1 WCAG 2.2 AA contrast against dark shell surfaces.
inline constexpr const char *kTextPrimaryHex = "#f1f5f9";
inline constexpr const char *kTextSecondaryHex = "#94a3b8";
inline constexpr const char *kTextInteractiveHex = "#cbd5e1";
inline constexpr const char *kTextHighlightHex = "#ffffff";

// #0369a1 paired with #ffffff achieves > 5.9:1 contrast for filled selected controls.
inline constexpr const char *kAccentPrimaryHex = "#0369a1";
inline constexpr const char *kFocusRingHex = "#38bdf8";
inline constexpr const char *kDangerHoverBgHex = "#7f1d1d";
inline constexpr const char *kDangerFocusBgHex = "#450a0a";
inline constexpr const char *kDangerFocusRingHex = "#f87171";

// Typography and minimum control geometry tokens.
inline constexpr int kFontSizeBasePx = 11;
inline constexpr int kFocusRingWidthPx = 2;
inline constexpr int kTouchTargetMinPx = 28;
inline constexpr int kDockTitleBarMinPx = 34;

[[nodiscard]] inline QPalette buildProStudioPalette()
{
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(0x0f, 0x11, 0x17));
    palette.setColor(QPalette::WindowText, QColor(0xf1, 0xf5, 0xf9));
    palette.setColor(QPalette::Base, QColor(0x16, 0x19, 0x22));
    palette.setColor(QPalette::AlternateBase, QColor(0x20, 0x24, 0x2e));
    palette.setColor(QPalette::ToolTipBase, QColor(0x16, 0x19, 0x22));
    palette.setColor(QPalette::ToolTipText, QColor(0xf1, 0xf5, 0xf9));
    palette.setColor(QPalette::Text, QColor(0xf1, 0xf5, 0xf9));
    palette.setColor(QPalette::PlaceholderText, QColor(0x94, 0xa3, 0xb8));
    palette.setColor(QPalette::Button, QColor(0x20, 0x24, 0x2e));
    palette.setColor(QPalette::ButtonText, QColor(0xf1, 0xf5, 0xf9));
    palette.setColor(QPalette::BrightText, QColor(0xff, 0xff, 0xff));
    palette.setColor(QPalette::Light, QColor(0x2e, 0x35, 0x45));
    palette.setColor(QPalette::Midlight, QColor(0x26, 0x2b, 0x36));
    palette.setColor(QPalette::Dark, QColor(0x0c, 0x0e, 0x13));
    palette.setColor(QPalette::Mid, QColor(0x1c, 0x20, 0x29));
    palette.setColor(QPalette::Highlight, QColor(0x03, 0x69, 0xa1));
    palette.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(0x94, 0xa3, 0xb8));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x94, 0xa3, 0xb8));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x94, 0xa3, 0xb8));
    return palette;
}

inline void applySecondaryTextRole(QWidget *widget, const QColor &color = QColor(0x94, 0xa3, 0xb8))
{
    if (!widget) {
        return;
    }
    QPalette pal = widget->palette();
    pal.setColor(QPalette::WindowText, color);
    pal.setColor(QPalette::Text, color);
    widget->setPalette(pal);
}

/**
 * @brief Fusion-backed proxy style replacing global QStyleSheetStyle wrappers.
 *
 * Draws dark surface tokens and 2px #38BDF8 focus rings directly via QPainter
 * without allocating per-widget QSS style caches or requiring Qt meta-object macros.
 */
class StudioProxyStyle final : public QProxyStyle {
  public:
    StudioProxyStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}

    void polish(QPalette &pal) override { pal = buildProStudioPalette(); }
    void polish(QWidget *widget) override
    {
        QProxyStyle::polish(widget);
        if (auto *btn = qobject_cast<QPushButton *>(widget)) {
            if (btn->maximumHeight() > 0 && btn->maximumHeight() < 22) {
                btn->setFixedHeight(22);
            }
        }
    }

    int pixelMetric(PixelMetric metric, const QStyleOption *opt = nullptr,
                    const QWidget *widget = nullptr) const override
    {
        if (metric == PM_FocusFrameHMargin || metric == PM_FocusFrameVMargin) {
            return kFocusRingWidthPx;
        }
        if (metric == PM_SplitterWidth) {
            return 4;
        }
        return QProxyStyle::pixelMetric(metric, opt, widget);
    }

    QSize sizeFromContents(ContentsType ct, const QStyleOption *opt, const QSize &contentsSize,
                           const QWidget *widget = nullptr) const override
    {
        QSize sz = QProxyStyle::sizeFromContents(ct, opt, contentsSize, widget);
        if (ct == CT_PushButton) {
            sz.setWidth(sz.width() + 12);
            sz.setHeight(std::max(sz.height(), 24));
        } else if (ct == CT_ToolButton) {
            const bool isDockBtn =
                widget && (widget->objectName() == QLatin1String("DockFloatButton") ||
                           widget->objectName() == QLatin1String("DockCloseButton"));
            if (isDockBtn) {
                sz.setWidth(std::max(sz.width(), kTouchTargetMinPx));
                sz.setHeight(std::max(sz.height(), kTouchTargetMinPx));
            } else {
                sz.setHeight(std::max(sz.height(), 22));
            }
        }
        return sz;
    }

    void drawPrimitive(PrimitiveElement pe, const QStyleOption *opt, QPainter *p,
                       const QWidget *widget = nullptr) const override
    {
        if (!opt || !p || !p->isActive()) {
            QProxyStyle::drawPrimitive(pe, opt, p, widget);
            return;
        }

        if (pe == PE_FrameFocusRect) {
            drawFocusOutline(p, opt->rect, QColor(0x38, 0xbd, 0xf8), 4.0);
            return;
        }

        if (pe == PE_PanelButtonCommand) {
            const bool isCloseBtn =
                widget && widget->objectName() == QLatin1String("DockCloseButton");
            const bool isFloatBtn =
                widget && widget->objectName() == QLatin1String("DockFloatButton");
            const bool hovered = (opt->state & State_MouseOver) != 0;
            const bool pressed = (opt->state & State_Sunken) != 0;
            const bool checked = (opt->state & State_On) != 0;
            const bool focused = (opt->state & State_HasFocus) != 0;
            const bool enabled = (opt->state & State_Enabled) != 0;

            QColor bg(0x20, 0x24, 0x2e);
            QColor border(0x2e, 0x35, 0x45);
            if (isCloseBtn || isFloatBtn) {
                bg = Qt::transparent;
                border = Qt::transparent;
                if (hovered) {
                    bg = isCloseBtn ? QColor(0x7f, 0x1d, 0x1d) : QColor(0x26, 0x2c, 0x3a);
                }
                if (focused && enabled) {
                    bg = isCloseBtn ? QColor(0x45, 0x0a, 0x0a) : QColor(0x1e, 0x29, 0x3b);
                    border = isCloseBtn ? QColor(0xf8, 0x71, 0x71) : QColor(0x38, 0xbd, 0xf8);
                }
            } else {
                if (checked) {
                    bg = QColor(0x03, 0x69, 0xa1);
                    border = QColor(0x38, 0xbd, 0xf8);
                } else if (pressed) {
                    bg = QColor(0x16, 0x19, 0x20);
                } else if (hovered) {
                    bg = QColor(0x2a, 0x30, 0x3d);
                    border = QColor(0x38, 0xbd, 0xf8);
                }
                if (focused && enabled) {
                    border = QColor(0x38, 0xbd, 0xf8);
                }
            }

            p->save();
            p->setRenderHint(QPainter::Antialiasing, true);
            const int bw = (focused && enabled) ? kFocusRingWidthPx : 1;
            const qreal inset = bw * 0.5;
            p->setBrush(bg);
            if (border.alpha() > 0) {
                p->setPen(QPen(border, bw));
            } else {
                p->setPen(Qt::NoPen);
            }
            const qreal radius = (isCloseBtn || isFloatBtn) ? 3.0 : 5.0;
            p->drawRoundedRect(QRectF(opt->rect).adjusted(inset, inset, -inset, -inset), radius,
                               radius);
            p->restore();
            return;
        }

        QProxyStyle::drawPrimitive(pe, opt, p, widget);
        if ((pe == PE_PanelLineEdit || pe == PE_Frame) && (opt->state & State_HasFocus) &&
            (opt->state & State_Enabled)) {
            drawFocusOutline(p, opt->rect, QColor(0x38, 0xbd, 0xf8), 4.0);
        }
    }

    void drawControl(ControlElement ce, const QStyleOption *opt, QPainter *p,
                     const QWidget *widget = nullptr) const override
    {
        if (ce == CE_TabBarTabShape && opt && p && p->isActive()) {
            const bool selected = (opt->state & State_Selected) != 0;
            const bool hovered = (opt->state & State_MouseOver) != 0;
            const bool focused = (opt->state & State_HasFocus) != 0;

            p->save();
            QColor bg(0x14, 0x17, 0x1f);
            if (selected) {
                bg = QColor(0x20, 0x24, 0x2e);
            } else if (hovered) {
                bg = QColor(0x1b, 0x1f, 0x29);
            }
            p->fillRect(opt->rect, bg);
            if (selected) {
                const QRect indicator(opt->rect.left(), opt->rect.bottom() - 1, opt->rect.width(),
                                      2);
                p->fillRect(indicator, QColor(0x38, 0xbd, 0xf8));
            }
            if (focused) {
                drawFocusOutline(p, opt->rect, QColor(0x38, 0xbd, 0xf8), 2.0);
            }
            p->restore();
            return;
        }
        QProxyStyle::drawControl(ce, opt, p, widget);
    }

    void drawComplexControl(ComplexControl cc, const QStyleOptionComplex *opt, QPainter *p,
                            const QWidget *widget = nullptr) const override
    {
        QProxyStyle::drawComplexControl(cc, opt, p, widget);
        if ((cc == CC_ComboBox || cc == CC_SpinBox || cc == CC_Slider) && opt && p &&
            p->isActive() && (opt->state & State_HasFocus) && (opt->state & State_Enabled)) {
            drawFocusOutline(p, opt->rect, QColor(0x38, 0xbd, 0xf8), 4.0);
        }
    }

  private:
    static void drawFocusOutline(QPainter *p, const QRect &rect, const QColor &color, qreal radius)
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        p->setPen(QPen(color, kFocusRingWidthPx));
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(QRectF(rect).adjusted(1.0, 1.0, -1.0, -1.0), radius, radius);
        p->restore();
    }
};

} // namespace wwal::studio::tokens
