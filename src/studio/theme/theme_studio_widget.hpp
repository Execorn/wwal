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
#include <QPointer>
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

#include <cstdint>

namespace waywal::studio {

class OklchColorDialog;

/**
 * @brief Interactive color swatch chip showing color square, token name, and hex value.
 *        Supports 1-click and keyboard (Return/Space) hex clipboard copy.
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
    // Lightweight constructor for custom-painted grids without per-chip child widgets or QSS.
    explicit ColorSwatchChip(QWidget *parent = nullptr);

    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

  private:
    void refreshUi();
    void activateCopy(const QPoint &globalPos);

    QString m_tokenName;
    color_rgba_t m_color{};
    QString m_hexString;
};

/**
 * @brief Custom-painted, keyboard-accessible swatch grid replacing 62 styled child widgets.
 *        Zero per-swatch QSS stylesheets; supports arrow navigation, 2px #38BDF8 focus ring,
 *        Return/Space clipboard copy, and non-modal OkLCH editing on double-click or E/F2.
 */
class ColorSwatchGridWidget : public ColorSwatchChip {
    Q_OBJECT

  public:
    struct SwatchEntry {
        QString name;
        color_rgba_t color{};
        QString hex;
    };

    explicit ColorSwatchGridWidget(const QString &gridTitle, QWidget *parent = nullptr);
    ~ColorSwatchGridWidget() override = default;

    void setSwatches(const QVector<SwatchEntry> &entries);
    [[nodiscard]] int swatchCount() const noexcept { return m_entries.size(); }
    [[nodiscard]] int focusedIndex() const noexcept { return m_focusedIndex; }
    [[nodiscard]] QRect tileRectForIndex(int index) const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  Q_SIGNALS:
    void swatchEditRequested(const QString &name, const color_rgba_t &color);

  protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    bool event(QEvent *event) override;

  private:
    [[nodiscard]] int columnCountForWidth(int availWidth) const noexcept;
    [[nodiscard]] int indexAtPos(const QPoint &pos) const noexcept;
    void updateLayoutMetrics();
    void ensureFocusedVisible();
    void updateAccessibleFocus();
    void triggerCopyAt(int index, const QPoint &globalPos);

    QString m_gridTitle;
    QVector<SwatchEntry> m_entries;
    int m_focusedIndex{0};
    int m_hoveredIndex{-1};
    int m_columns{2};
    int m_tileHeight{48};
};

class WcagStatusPill;

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
    static color_rgba_t compositeOver(const color_rgba_t &fg, const color_rgba_t &bg);
    static double relativeLuminance(const color_rgba_t &c);
    static double computeContrastRatio(const color_rgba_t &fg, const color_rgba_t &bg);

    QString m_pairName;
    QLabel *m_nameLabel{nullptr};
    QLabel *m_ratioLabel{nullptr};
    WcagStatusPill *m_aaBadge{nullptr};
    WcagStatusPill *m_aaaBadge{nullptr};
    int m_lastPassAa{-1};
    int m_lastPassAaa{-1};
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
 * - 36 Material Design 3 tokens, 16 ANSI colors, and 14 Catppuccin tokens swatch grid
 * - Integrated DesktopPreviewMockup and TemplateExporter
 */
class ThemeStudioWidget : public QWidget {
    Q_OBJECT

  public:
    explicit ThemeStudioWidget(QWidget *parent = nullptr);
    ~ThemeStudioWidget() override = default;

    void loadWallpaper(const QString &filePath);
    void setWallpaperImage(const QImage &image, const QString &path = QString());

    void setSolarCoordinates(double latitudeDeg, double longitudeDeg, bool hasCoordinates = true);
    [[nodiscard]] bool hasSolarCoordinates() const noexcept { return m_hasSolarCoords; }
    [[nodiscard]] double solarLatitude() const noexcept { return m_solarLatitude; }
    [[nodiscard]] double solarLongitude() const noexcept { return m_solarLongitude; }

    [[nodiscard]] const waywal_palette_t &currentPalette() const { return m_palette; }
    [[nodiscard]] waywal_theme_mode_t themeMode() const { return m_mode; }
    [[nodiscard]] waywal_scheme_variant_t schemeVariant() const { return m_variant; }
    [[nodiscard]] float contrast() const { return m_contrast; }
    [[nodiscard]] uint8_t seedIndex() const { return m_seedIndex; }
    [[nodiscard]] bool isSolarTimerActive() const noexcept { return m_solarTimer.isActive(); }

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
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

  private Q_SLOTS:
    void onSolarTimerTick();
    void onSwatchCopied(const QString &name, const QString &hex);
    void onSwatchEditRequested(const QString &name, const color_rgba_t &color);

  private:
    void setupUi();
    void setupControlsCard(QVBoxLayout *parentLayout);
    void setupWcagBadgesCard(QVBoxLayout *parentLayout);
    void setupSwatchesCard(QTabWidget *tabWidget);

    void loadSolarConfigDefaultsIfNeeded();
    void applySolarScheduleEvaluation();
    void applyCustomSeedColor(const QColor &chosen, const QString &sourceContext = QString());
    void openSeedColorDialog(const QColor &initial, const QString &title,
                             const QString &sourceContext);
    void updateSeedRadioSwatches();
    void updateWcagBadges();
    void updateSwatchGrids();

    static QColor toQColor(const color_rgba_t &c);

    waywal_palette_t m_palette{};
    QImage m_sourceImage;
    QString m_wallpaperPath;

    waywal_theme_mode_t m_mode{WAYWAL_THEME_DARK};
    bool m_autoSolarSchedule{false};
    bool m_hasSolarCoords{false};
    bool m_solarConfigLoaded{false};
    double m_solarLatitude{0.0};
    double m_solarLongitude{0.0};
    int32_t m_sunriseFallbackMin{390};
    int32_t m_sunsetFallbackMin{1110};

    waywal_scheme_variant_t m_variant{WAYWAL_SCHEME_TONAL_SPOT};
    float m_contrast{0.0f};
    uint8_t m_seedIndex{0};
    bool m_hasSeedOverride{false};
    uint32_t m_seedOverrideRgb{0};

    QSplitter *m_mainSplitter{nullptr};
    DesktopPreviewMockup *m_mockup{nullptr};
    TemplateExporter *m_templateExporter{nullptr};

    QPushButton *m_toggleLockBtn{nullptr};
    QButtonGroup *m_modeGroup{nullptr};
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
    ColorSwatchGridWidget *m_m3SwatchGrid{nullptr};
    ColorSwatchGridWidget *m_ansiSwatchGrid{nullptr};
    ColorSwatchGridWidget *m_catppuccinSwatchGrid{nullptr};

    QPointer<OklchColorDialog> m_customSeedDialog;
    QPointer<QWidget> m_activeWallpaperDialog;

    QTimer m_solarTimer;
    QTimer m_toastTimer;
};

} // namespace waywal::studio
