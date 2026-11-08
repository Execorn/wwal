// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "studio/theme/desktop_preview_mockup.hpp"
#include "studio/theme/template_exporter.hpp"
#include "waywal/palette_hct.h"
#include "waywal/solar_schedule.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QSplitter>
#include <QString>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

namespace waywal::studio {

/**
 * @brief Interactive color swatch widget showing color square, token name, and hex value.
 *        1-click copies the hex string to clipboard.
 */
class ColorSwatchChip : public QFrame {
    Q_OBJECT

public:
    ColorSwatchChip(const QString &tokenName, const color_rgba_t &color, QWidget *parent = nullptr);
    ~ColorSwatchChip() override = default;

    void updateColor(const color_rgba_t &color);
    [[nodiscard]] QString tokenName() const { return m_tokenName; }
    [[nodiscard]] QString hexString() const { return m_hexString; }
    [[nodiscard]] color_rgba_t colorRgba() const { return m_color; }

Q_SIGNALS:
    void copiedToClipboard(const QString &name, const QString &hex);

protected:
    void mousePressEvent(QMouseEvent *event) override;

private:
    void refreshUi();

    QString m_tokenName;
    color_rgba_t m_color{};
    QString m_hexString;

    QWidget *m_colorBox{nullptr};
    QLabel *m_nameLabel{nullptr};
    QLabel *m_hexLabel{nullptr};
};

/**
 * @brief Live WCAG 2.1 contrast ratio badge widget for foreground/background token pairs.
 */
class WcagBadgeWidget : public QFrame {
    Q_OBJECT

public:
    explicit WcagBadgeWidget(const QString &pairName, QWidget *parent = nullptr);
    ~WcagBadgeWidget() override = default;

    void updateRatio(const color_rgba_t &fg, const color_rgba_t &bg);

private:
    static double relativeLuminance(const color_rgba_t &c);
    static double computeContrastRatio(const color_rgba_t &fg, const color_rgba_t &bg);

    QString m_pairName;
    QLabel *m_nameLabel{nullptr};
    QLabel *m_ratioLabel{nullptr};
    QLabel *m_aaBadge{nullptr};
    QLabel *m_aaaBadge{nullptr};
};

/**
 * @brief Main Material You HCT Live Theme Studio Widget.
 *
 * Exposes full interactive controls for:
 * - Wallpaper image loading and perceptual extraction in < 2.5 ms
 * - Dark / Light theme mode toggle & Auto Day/Night solar schedule toggle
 * - Seed 0..3 primary accent selector + custom seed color override
 * - All 9 Material Design 3 scheme variants
 * - [-1.0, +1.0] contrast slider with live WCAG 2.1 AA (4.5:1) & AAA (7.0:1) badges
 * - 32 Material Design 3 tokens, 16 ANSI colors, and 14 Catppuccin tokens swatch grid
 * - Integrated DesktopPreviewMockup and TemplateExporter
 */
class ThemeStudioWidget : public QWidget {
    Q_OBJECT

public:
    explicit ThemeStudioWidget(QWidget *parent = nullptr);
    ~ThemeStudioWidget() override = default;

    void loadWallpaper(const QString &filePath);
    void setWallpaperImage(const QImage &image, const QString &path = QString());

    [[nodiscard]] const waywal_palette_t &currentPalette() const { return m_palette; }
    [[nodiscard]] waywal_theme_mode_t themeMode() const { return m_mode; }
    [[nodiscard]] waywal_scheme_variant_t schemeVariant() const { return m_variant; }
    [[nodiscard]] float contrast() const { return m_contrast; }
    [[nodiscard]] uint8_t seedIndex() const { return m_seedIndex; }

public Q_SLOTS:
    void reextract();
    void setThemeMode(waywal_theme_mode_t mode);
    void setAutoSolarSchedule(bool enabled);
    void setSchemeVariant(waywal_scheme_variant_t variant);
    void setContrast(float contrast);
    void setSeedIndex(uint8_t index);
    void openCustomSeedDialog();
    void clearCustomSeed();
    void openWallpaperDialog();

Q_SIGNALS:
    void paletteChanged(const waywal_palette_t &palette);
    void modeChanged(waywal_theme_mode_t mode);
    void variantChanged(waywal_scheme_variant_t variant);
    void contrastChanged(float contrast);
    void seedIndexChanged(uint8_t index);
    void statusMessageRequested(const QString &msg);

protected:
    void resizeEvent(QResizeEvent *event) override;

private Q_SLOTS:
    void onSolarTimerTick();
    void onSwatchCopied(const QString &name, const QString &hex);

private:
    void setupUi();
    void setupControlsCard(QVBoxLayout *parentLayout);
    void setupWcagBadgesCard(QVBoxLayout *parentLayout);
    void setupSwatchesCard(QTabWidget *tabWidget);

    void applySolarScheduleEvaluation();
    void updateSeedRadioSwatches();
    void updateWcagBadges();
    void updateSwatchGrids();

    static QColor toQColor(const color_rgba_t &c);

    // Current State
    waywal_palette_t m_palette{};
    QImage m_sourceImage;
    QString m_wallpaperPath;

    waywal_theme_mode_t m_mode{WAYWAL_THEME_DARK};
    bool m_autoSolarSchedule{false};
    waywal_scheme_variant_t m_variant{WAYWAL_SCHEME_TONAL_SPOT};
    float m_contrast{0.0f};
    uint8_t m_seedIndex{0};
    bool m_hasSeedOverride{false};
    uint32_t m_seedOverrideRgb{0};

    // UI Widgets
    QSplitter *m_mainSplitter{nullptr};
    DesktopPreviewMockup *m_mockup{nullptr};
    TemplateExporter *m_templateExporter{nullptr};

    QPushButton *m_modeDarkBtn{nullptr};
    QPushButton *m_modeLightBtn{nullptr};
    QCheckBox *m_autoSolarCheck{nullptr};
    QLabel *m_solarPhaseBadge{nullptr};

    QComboBox *m_variantCombo{nullptr};
    QSlider *m_contrastSlider{nullptr};
    QLabel *m_contrastValLabel{nullptr};
    QPushButton *m_resetContrastBtn{nullptr};

    QButtonGroup *m_seedGroup{nullptr};
    QVector<QRadioButton *> m_seedRadios;
    QVector<QWidget *> m_seedSwatches;
    QPushButton *m_customSeedBtn{nullptr};
    QWidget *m_customSeedSwatch{nullptr};
    QPushButton *m_clearSeedOverrideBtn{nullptr};

    QLabel *m_perfLabel{nullptr};
    QLabel *m_clipboardToastLabel{nullptr};

    QVector<WcagBadgeWidget *> m_wcagBadges;
    QVector<ColorSwatchChip *> m_m3Chips;
    QVector<ColorSwatchChip *> m_ansiChips;
    QVector<ColorSwatchChip *> m_catppuccinChips;

    QTimer m_solarTimer;
};

} // namespace waywal::studio
