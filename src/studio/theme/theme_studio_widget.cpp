// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/theme/theme_studio_widget.hpp"

#if __has_include("studio/inspector/oklch_color_dialog.hpp")
#include "studio/inspector/oklch_color_dialog.hpp"
#define HAVE_OKLCH_COLOR_DIALOG 1
#else
#define HAVE_OKLCH_COLOR_DIALOG 0
#endif

#include <QApplication>
#include <QClipboard>
#include <QColorDialog>
#include <QDateTime>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QRadialGradient>
#include <QScrollArea>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <QFileInfo>

extern "C" {
#include <waywal/image_loader.h>
}

namespace waywal::studio {

// ============================================================================
// ColorSwatchChip Implementation
// ============================================================================

ColorSwatchChip::ColorSwatchChip(const QString &tokenName, const color_rgba_t &color,
                                 QWidget *parent)
    : QFrame(parent), m_tokenName(tokenName), m_color(color)
{
    setFrameShape(QFrame::StyledPanel);
    setCursor(Qt::PointingHandCursor);
    setMinimumWidth(84);
    setFixedHeight(28);
    setStyleSheet(QStringLiteral(
        "ColorSwatchChip { background-color: #20222a; border: 1px solid #333644; "
        "border-radius: 5px; padding: 1px; } "
        "ColorSwatchChip:hover { background-color: #2c2f3c; border-color: #6750A4; }"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 1, 4, 1);
    layout->setSpacing(0);

    m_nameLabel = new QLabel(m_tokenName, this);
    m_nameLabel->setMinimumWidth(28);
    m_nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_nameLabel->setStyleSheet(
        QStringLiteral("font-weight: 600; font-size: 8.5px; color: #E0E2EC;"));
    layout->addWidget(m_nameLabel);

    auto *bottomRow = new QHBoxLayout();
    bottomRow->setContentsMargins(0, 0, 0, 0);
    bottomRow->setSpacing(4);

    m_colorBox = new QWidget(this);
    m_colorBox->setFixedSize(22, 10);
    bottomRow->addWidget(m_colorBox);

    m_hexLabel = new QLabel(this);
    m_hexLabel->setStyleSheet(
        QStringLiteral("font-family: monospace; font-size: 8.5px; color: #9094A6;"));
    bottomRow->addWidget(m_hexLabel, 1);

    layout->addLayout(bottomRow);

    refreshUi();
}

void ColorSwatchChip::updateColor(const color_rgba_t &color)
{
    m_color = color;
    refreshUi();
}

void ColorSwatchChip::refreshUi()
{
    m_hexString = QString::asprintf("#%02X%02X%02X", m_color.r, m_color.g, m_color.b);
    if (m_hexLabel) {
        m_hexLabel->setText(m_hexString);
    }
    if (m_colorBox) {
        m_colorBox->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid "
                                                 "rgba(255,255,255,0.2); border-radius: 3px;")
                                      .arg(m_hexString));
    }
    setToolTip(
        QStringLiteral("%1: %2\nClick to copy hex to clipboard").arg(m_tokenName, m_hexString));
}

void ColorSwatchChip::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        QGuiApplication::clipboard()->setText(m_hexString);
        QToolTip::showText(event->globalPosition().toPoint(),
                           QStringLiteral("Copied %1 (%2)").arg(m_hexString, m_tokenName), this);
        Q_EMIT copiedToClipboard(m_tokenName, m_hexString);
    }
    QFrame::mousePressEvent(event);
}

// ============================================================================
// WcagBadgeWidget Implementation
// ============================================================================

WcagBadgeWidget::WcagBadgeWidget(const QString &pairName, QWidget *parent)
    : QFrame(parent), m_pairName(pairName)
{
    setFrameShape(QFrame::StyledPanel);
    setToolTip(pairName);
    setFixedHeight(15);
    setStyleSheet(
        QStringLiteral("WcagBadgeWidget { background-color: #1a1c23; border: 1px solid #2b2e3b; "
                       "border-radius: 3px; padding: 0px; }"));

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(2, 0, 2, 0);
    layout->setSpacing(2);

    QString shortLabel = pairName;
    const int slashIdx = pairName.indexOf(QLatin1Char('/'));
    if (slashIdx >= 0) {
        shortLabel = pairName.mid(slashIdx + 1).trimmed();
        shortLabel.replace(QLatin1String("primaryContainer"), QLatin1String("priCont"));
        shortLabel.replace(QLatin1String("secondaryContainer"), QLatin1String("secCont"));
        shortLabel.replace(QLatin1String("tertiaryContainer"), QLatin1String("terCont"));
        shortLabel.replace(QLatin1String("surfaceVariant"), QLatin1String("surfVar"));
        shortLabel.replace(QLatin1String("background"), QLatin1String("bg"));
        shortLabel.replace(QLatin1String("secondary"), QLatin1String("second"));
        shortLabel.replace(QLatin1String("tertiary"), QLatin1String("tert"));
    }

    m_nameLabel = new QLabel(shortLabel, this);
    m_nameLabel->setMinimumWidth(16);
    m_nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_nameLabel->setStyleSheet(
        QStringLiteral("font-size: 8px; font-weight: bold; color: #D0D2DE; border: none;"));
    layout->addWidget(m_nameLabel, 1);

    m_ratioLabel = new QLabel(QStringLiteral("0.0:1"), this);
    m_ratioLabel->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 8px; "
                                               "font-weight: bold; color: #FFFFFF; border: none;"));
    layout->addWidget(m_ratioLabel, 0);

    m_aaBadge = new QLabel(QStringLiteral("AA"), this);
    m_aaBadge->setAlignment(Qt::AlignCenter);
    m_aaBadge->setFixedSize(14, 11);
    layout->addWidget(m_aaBadge, 0);

    m_aaaBadge = new QLabel(QStringLiteral("AAA"), this);
    m_aaaBadge->setAlignment(Qt::AlignCenter);
    m_aaaBadge->setFixedSize(18, 11);
    layout->addWidget(m_aaaBadge, 0);
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
    const double y1 = relativeLuminance(fg);
    const double y2 = relativeLuminance(bg);
    const double lighter = std::max(y1, y2);
    const double darker = std::min(y1, y2);
    return (lighter + 0.05) / (darker + 0.05);
}

void WcagBadgeWidget::updateRatio(const color_rgba_t &fg, const color_rgba_t &bg)
{
    const double ratio = computeContrastRatio(fg, bg);
    m_ratioLabel->setText(QString::asprintf("%.1f:1", ratio));

    const bool passAa = (ratio >= 4.5);
    const bool passAaa = (ratio >= 7.0);

    if (passAa) {
        m_aaBadge->setStyleSheet(
            QStringLiteral("background-color: #2e7d32; color: white; border-radius: 2px; "
                           "font-size: 7px; font-weight: bold; border: none;"));
        m_aaBadge->setToolTip(QStringLiteral("%1 — WCAG 2.1 AA (4.5:1) Pass").arg(m_pairName));
    } else {
        m_aaBadge->setStyleSheet(
            QStringLiteral("background-color: #c62828; color: white; border-radius: 2px; "
                           "font-size: 7px; font-weight: bold; border: none;"));
        m_aaBadge->setToolTip(QStringLiteral("%1 — WCAG 2.1 AA (4.5:1) Fail").arg(m_pairName));
    }

    if (passAaa) {
        m_aaaBadge->setStyleSheet(
            QStringLiteral("background-color: #2e7d32; color: white; border-radius: 2px; "
                           "font-size: 7px; font-weight: bold; border: none;"));
        m_aaaBadge->setToolTip(QStringLiteral("%1 — WCAG 2.1 AAA (7.0:1) Pass").arg(m_pairName));
    } else {
        m_aaaBadge->setStyleSheet(
            QStringLiteral("background-color: #555555; color: #bbbbbb; border-radius: 2px; "
                           "font-size: 7px; font-weight: bold; border: none;"));
        m_aaaBadge->setToolTip(QStringLiteral("%1 — WCAG 2.1 AAA (7.0:1) Not met").arg(m_pairName));
    }
}

// ============================================================================
// ThemeStudioWidget Implementation
// ============================================================================

ThemeStudioWidget::ThemeStudioWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumWidth(210);
    setStyleSheet(
        QStringLiteral("QGroupBox { border: 1px solid #2b2e3b; border-radius: 5px; margin-top: "
                       "8px; padding-top: 2px; "
                       "font-size: 8pt; font-weight: bold; color: #c7d2fe; } "
                       "QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top "
                       "left; left: 6px; top: 0px; padding: 0 3px; }"));
    setupUi();

    // Solar timer every 60 seconds
    connect(&m_solarTimer, &QTimer::timeout, this, &ThemeStudioWidget::onSolarTimerTick);
    m_solarTimer.start(60000);

    // Initial synthetic palette extraction
    reextract();
}

void ThemeStudioWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_mainSplitter) {
        Qt::Orientation target = (width() >= 540) ? Qt::Horizontal : Qt::Vertical;
        if (m_mainSplitter->orientation() != target) {
            m_mainSplitter->setOrientation(target);
        }
        if (target == Qt::Vertical && height() > 240) {
            const int availH = std::max(240, height() - 28);
            const int bottomH = (availH >= 560) ? 330 : 120;
            const int topH = std::max(316, availH - bottomH - 4);
            m_mainSplitter->setSizes({topH, std::max(96, availH - topH)});
        } else if (target == Qt::Horizontal && width() > 300) {
            int leftW = static_cast<int>(width() * 0.54);
            m_mainSplitter->setSizes({leftW, width() - leftW});
        }
    }
}

QColor ThemeStudioWidget::toQColor(const color_rgba_t &c) { return QColor(c.r, c.g, c.b, c.a); }

void ThemeStudioWidget::setupUi()
{
    auto *topLayout = new QVBoxLayout(this);
    topLayout->setContentsMargins(5, 3, 5, 3);
    topLayout->setSpacing(3);

    // Header toolbar
    auto *headerLayout = new QHBoxLayout();
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(3);

    auto *titleLabel = new QLabel(QStringLiteral("<b>Material You HCT</b>"), this);
    titleLabel->setStyleSheet(QStringLiteral("font-size: 8.5pt; color: #E0E2EC;"));
    titleLabel->setMinimumWidth(60);
    titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    headerLayout->addWidget(titleLabel, 1);

    m_clipboardToastLabel = new QLabel(this);
    m_clipboardToastLabel->setStyleSheet(
        QStringLiteral("color: #80cbc4; font-size: 7.5pt; font-weight: bold;"));
    headerLayout->addWidget(m_clipboardToastLabel, 0);

    m_perfLabel = new QLabel(QStringLiteral("Ready"), this);
    m_perfLabel->setToolTip(
        QStringLiteral("Material You HCT Extraction Latency (Target: < 2.5 ms)"));
    m_perfLabel->setStyleSheet(
        QStringLiteral("color: #888888; font-size: 7pt; font-family: monospace;"));
    headerLayout->addWidget(m_perfLabel, 0);

    auto *toggleLockBtn = new QPushButton(QStringLiteral("Blur"), this);
    toggleLockBtn->setToolTip(QStringLiteral("Toggle Lockscreen Kawase Blur Preview Mode"));
    toggleLockBtn->setFixedHeight(19);
    toggleLockBtn->setStyleSheet(QStringLiteral("padding: 1px 5px; font-size: 7.5pt;"));
    headerLayout->addWidget(toggleLockBtn, 0);

    auto *loadWallBtn = new QPushButton(QStringLiteral("Open..."), this);
    loadWallBtn->setToolTip(QStringLiteral("Open image to extract Material You HCT palette"));
    loadWallBtn->setFixedHeight(19);
    loadWallBtn->setStyleSheet(
        QStringLiteral("padding: 1px 5px; font-size: 7.5pt; font-weight: bold;"));
    connect(loadWallBtn, &QPushButton::clicked, this, &ThemeStudioWidget::openWallpaperDialog);
    headerLayout->addWidget(loadWallBtn, 0);

    topLayout->addLayout(headerLayout);

    // Main responsive splitter (switches between Horizontal when wide and Vertical when docked
    // narrow)
    m_mainSplitter = new QSplitter(Qt::Horizontal, this);
    m_mainSplitter->setChildrenCollapsible(false);

    // Left/Top Pane: Scrollable Interactive Controls & Desktop Mockup Preview
    auto *leftScroll = new QScrollArea(m_mainSplitter);
    leftScroll->setWidgetResizable(true);
    leftScroll->setFrameShape(QFrame::NoFrame);
    leftScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    leftScroll->setStyleSheet(
        QStringLiteral("QScrollArea { background: transparent; border: none; }"));

    auto *leftColWidget = new QWidget(leftScroll);
    auto *leftColLayout = new QVBoxLayout(leftColWidget);
    leftColLayout->setContentsMargins(0, 0, 2, 0);
    leftColLayout->setSpacing(2);

    // Controls Card
    setupControlsCard(leftColLayout);

    // WCAG Badges Card
    setupWcagBadgesCard(leftColLayout);

    // Desktop Preview Mockup
    auto *previewCard = new QGroupBox(QStringLiteral("Live Desktop Vector Mockup"), leftColWidget);
    auto *previewLayout = new QVBoxLayout(previewCard);
    previewLayout->setContentsMargins(3, 2, 3, 2);
    previewLayout->setSpacing(1);

    m_mockup = new DesktopPreviewMockup(previewCard);
    m_mockup->setMinimumSize(170, 114);
    previewLayout->addWidget(m_mockup, 1);
    connect(toggleLockBtn, &QPushButton::clicked, m_mockup,
            &DesktopPreviewMockup::toggleLockscreenPreview);

    leftColLayout->addWidget(previewCard, 1);
    leftScroll->setWidget(leftColWidget);
    m_mainSplitter->addWidget(leftScroll);

    // Right/Bottom Pane: Token Swatches Tabs & Exporter
    auto *rightColWidget = new QWidget(m_mainSplitter);
    auto *rightColLayout = new QVBoxLayout(rightColWidget);
    rightColLayout->setContentsMargins(0, 0, 0, 0);
    rightColLayout->setSpacing(2);

    auto *tabWidget = new QTabWidget(rightColWidget);
    tabWidget->setStyleSheet(
        QStringLiteral("QTabBar::tab { padding: 2px 5px; font-size: 7.5pt; }"));
    setupSwatchesCard(tabWidget);

    rightColLayout->addWidget(tabWidget, 1);
    m_mainSplitter->addWidget(rightColWidget);

    m_mainSplitter->setStretchFactor(0, 6);
    m_mainSplitter->setStretchFactor(1, 4);

    topLayout->addWidget(m_mainSplitter, 1);
}

void ThemeStudioWidget::setupControlsCard(QVBoxLayout *parentLayout)
{
    auto *card = new QGroupBox(QStringLiteral("Theme Generation Controls"), this);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(3, 2, 3, 2);
    layout->setSpacing(1);

    // Row 1: Mode & Solar Schedule
    auto *row1 = new QHBoxLayout();
    row1->setContentsMargins(0, 0, 0, 0);
    row1->setSpacing(4);

    auto *modeLabel = new QLabel(QStringLiteral("Mode:"), card);
    modeLabel->setStyleSheet(QStringLiteral("font-weight: bold; font-size: 8pt;"));
    row1->addWidget(modeLabel);

    m_modeDarkBtn = new QPushButton(QStringLiteral("Dark"), card);
    m_modeDarkBtn->setCheckable(true);
    m_modeDarkBtn->setChecked(true);
    m_modeDarkBtn->setFixedHeight(18);
    m_modeDarkBtn->setStyleSheet(QStringLiteral("padding: 1px 6px; font-size: 7.5pt;"));
    row1->addWidget(m_modeDarkBtn);

    m_modeLightBtn = new QPushButton(QStringLiteral("Light"), card);
    m_modeLightBtn->setCheckable(true);
    m_modeLightBtn->setChecked(false);
    m_modeLightBtn->setFixedHeight(18);
    m_modeLightBtn->setStyleSheet(QStringLiteral("padding: 1px 6px; font-size: 7.5pt;"));
    row1->addWidget(m_modeLightBtn);

    connect(m_modeDarkBtn, &QPushButton::clicked, this,
            [this]() { setThemeMode(WAYWAL_THEME_DARK); });
    connect(m_modeLightBtn, &QPushButton::clicked, this,
            [this]() { setThemeMode(WAYWAL_THEME_LIGHT); });

    row1->addSpacing(4);

    m_autoSolarCheck = new QCheckBox(QStringLiteral("Solar"), card);
    m_autoSolarCheck->setToolTip(
        QStringLiteral("Automatically switch Dark/Light mode via astronomical solar schedule"));
    m_autoSolarCheck->setStyleSheet(QStringLiteral(
        "QCheckBox { font-size: 7.5pt; spacing: 3px; } "
        "QCheckBox::indicator { width: 11px; height: 11px; border: 1px solid #55596a; "
        "border-radius: 2px; background: #181a20; } "
        "QCheckBox::indicator:checked { background: #e58824; border-color: #f08d49; }"));
    connect(m_autoSolarCheck, &QCheckBox::toggled, this, &ThemeStudioWidget::setAutoSolarSchedule);
    row1->addWidget(m_autoSolarCheck);

    m_solarPhaseBadge = new QLabel(QStringLiteral("Standby"), card);
    m_solarPhaseBadge->setStyleSheet(
        QStringLiteral("color: #7986cb; font-size: 7pt; font-family: monospace;"));
    m_solarPhaseBadge->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    row1->addWidget(m_solarPhaseBadge, 1);

    layout->addLayout(row1);

    // Row 2: M3 Variant Combo
    auto *row2 = new QHBoxLayout();
    row2->setContentsMargins(0, 0, 0, 0);
    row2->setSpacing(4);

    auto *varLabel = new QLabel(QStringLiteral("Scheme:"), card);
    varLabel->setStyleSheet(QStringLiteral("font-weight: bold; font-size: 8pt;"));
    row2->addWidget(varLabel);

    m_variantCombo = new QComboBox(card);
    m_variantCombo->setFixedHeight(19);
    m_variantCombo->setMinimumWidth(80);
    m_variantCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_variantCombo->addItem(QStringLiteral("Tonal Spot (Balanced)"),
                            static_cast<int>(WAYWAL_SCHEME_TONAL_SPOT));
    m_variantCombo->addItem(QStringLiteral("Vibrant (Max chroma)"),
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
        setSchemeVariant(
            static_cast<waywal_scheme_variant_t>(m_variantCombo->itemData(idx).toInt()));
    });
    row2->addWidget(m_variantCombo, 1);

    layout->addLayout(row2);

    // Row 2b: Contrast Slider (own row so it never clips horizontally in narrow docks)
    auto *row2b = new QHBoxLayout();
    row2b->setContentsMargins(0, 0, 0, 0);
    row2b->setSpacing(4);

    m_contrastValLabel = new QLabel(QStringLiteral("Contrast: 0.00"), card);
    m_contrastValLabel->setStyleSheet(
        QStringLiteral("font-weight: bold; font-size: 7.5pt; font-family: monospace;"));
    row2b->addWidget(m_contrastValLabel);

    m_contrastSlider = new QSlider(Qt::Horizontal, card);
    m_contrastSlider->setFixedHeight(16);
    m_contrastSlider->setRange(-100, 100);
    m_contrastSlider->setValue(0);
    m_contrastSlider->setSingleStep(5);
    m_contrastSlider->setPageStep(25);
    m_contrastSlider->setMinimumWidth(50);
    connect(m_contrastSlider, &QSlider::valueChanged, this,
            [this](int val) { setContrast(static_cast<float>(val) / 100.0f); });
    row2b->addWidget(m_contrastSlider, 1);

    m_resetContrastBtn = new QPushButton(QStringLiteral("0.0"), card);
    m_resetContrastBtn->setToolTip(QStringLiteral("Reset contrast to standard 0.0"));
    m_resetContrastBtn->setFixedSize(28, 17);
    m_resetContrastBtn->setStyleSheet(
        QStringLiteral("padding: 0px 2px; font-size: 7pt; font-family: monospace;"));
    connect(m_resetContrastBtn, &QPushButton::clicked, this,
            [this]() { m_contrastSlider->setValue(0); });
    row2b->addWidget(m_resetContrastBtn);

    layout->addLayout(row2b);

    // Row 3: Ranked Seeds 0..3 & Custom Override (compact so it fits in <220px width)
    auto *row3 = new QHBoxLayout();
    row3->setContentsMargins(0, 0, 0, 0);
    row3->setSpacing(2);

    auto *seedLabel = new QLabel(QStringLiteral("Seed:"), card);
    seedLabel->setStyleSheet(QStringLiteral("font-weight: bold; font-size: 9px;"));
    row3->addWidget(seedLabel);

    m_seedGroup = new QButtonGroup(this);
    m_seedRadios.resize(4);
    m_seedSwatches.resize(4);

    const QString seedShortNames[4] = {QStringLiteral("0"), QStringLiteral("1"),
                                       QStringLiteral("2"), QStringLiteral("3")};
    const QString seedTooltips[4] = {QStringLiteral("Seed 0 (Primary ranked wallpaper accent)"),
                                     QStringLiteral("Seed 1 (Secondary ranked wallpaper accent)"),
                                     QStringLiteral("Seed 2 (Tertiary ranked wallpaper accent)"),
                                     QStringLiteral("Seed 3 (Neutral ranked wallpaper accent)")};

    for (int i = 0; i < 4; ++i) {
        auto *radio = new QRadioButton(seedShortNames[i], card);
        radio->setToolTip(seedTooltips[i]);
        radio->setStyleSheet(
            QStringLiteral("QRadioButton { font-size: 9px; spacing: 1px; }"
                           "QRadioButton::indicator { width: 10px; height: 10px; }"));
        if (i == 0) {
            radio->setChecked(true);
        }
        m_seedGroup->addButton(radio, i);
        m_seedRadios[i] = radio;

        auto *swatch = new QWidget(card);
        swatch->setFixedSize(9, 9);
        swatch->setToolTip(seedTooltips[i]);
        swatch->setStyleSheet(
            QStringLiteral("border-radius: 2px; border: 1px solid rgba(255,255,255,0.2);"));
        m_seedSwatches[i] = swatch;

        row3->addWidget(radio);
        row3->addWidget(swatch);
        row3->addSpacing(1);
    }

    connect(m_seedGroup, &QButtonGroup::idClicked, this,
            [this](int id) { setSeedIndex(static_cast<uint8_t>(id)); });

    row3->addSpacing(2);

    m_customSeedBtn = new QPushButton(QStringLiteral("Pick"), card);
    m_customSeedBtn->setToolTip(
        QStringLiteral("Override seed color with custom OkLCH / RGB color"));
    m_customSeedBtn->setFixedHeight(16);
    m_customSeedBtn->setStyleSheet(QStringLiteral("padding: 0px 4px; font-size: 9px;"));
    connect(m_customSeedBtn, &QPushButton::clicked, this, &ThemeStudioWidget::openCustomSeedDialog);
    row3->addWidget(m_customSeedBtn);

    m_customSeedSwatch = new QWidget(card);
    m_customSeedSwatch->setFixedSize(9, 9);
    m_customSeedSwatch->setStyleSheet(QStringLiteral(
        "border-radius: 2px; border: 1px solid #777; background-color: transparent;"));
    row3->addWidget(m_customSeedSwatch);

    m_clearSeedOverrideBtn = new QPushButton(QStringLiteral("✕"), card);
    m_clearSeedOverrideBtn->setToolTip(
        QStringLiteral("Clear custom override; revert to wallpaper seeds"));
    m_clearSeedOverrideBtn->setFixedSize(16, 16);
    m_clearSeedOverrideBtn->setStyleSheet(QStringLiteral("padding: 0px; font-size: 8.5px;"));
    m_clearSeedOverrideBtn->setEnabled(false);
    connect(m_clearSeedOverrideBtn, &QPushButton::clicked, this,
            &ThemeStudioWidget::clearCustomSeed);
    row3->addWidget(m_clearSeedOverrideBtn);

    row3->addStretch(1);
    layout->addLayout(row3);

    parentLayout->addWidget(card);
}

void ThemeStudioWidget::setupWcagBadgesCard(QVBoxLayout *parentLayout)
{
    auto *card =
        new QGroupBox(QStringLiteral("WCAG 2.1 Contrast Governor (AA 4.5 / AAA 7.0)"), this);
    auto *grid = new QGridLayout(card);
    grid->setContentsMargins(3, 2, 3, 2);
    grid->setSpacing(1);
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
                             QStringLiteral("onError / error")};

    m_wcagBadges.reserve(10);
    for (int i = 0; i < 10; ++i) {
        auto *badge = new WcagBadgeWidget(pairs[i], card);
        m_wcagBadges.push_back(badge);
        grid->addWidget(badge, i / 2, i % 2);
    }

    parentLayout->addWidget(card);
}

void ThemeStudioWidget::setupSwatchesCard(QTabWidget *tabWidget)
{
    // Tab 1: Material Design 3 Tokens (32)
    auto *m3Scroll = new QScrollArea(tabWidget);
    m3Scroll->setWidgetResizable(true);
    m3Scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *m3Container = new QWidget();
    auto *m3Grid = new QGridLayout(m3Container);
    m3Grid->setContentsMargins(3, 3, 3, 3);
    m3Grid->setSpacing(2);
    m3Grid->setColumnStretch(0, 1);
    m3Grid->setColumnStretch(1, 1);

    const QString m3Names[] = {QStringLiteral("primary"),
                               QStringLiteral("onPrimary"),
                               QStringLiteral("primaryContainer"),
                               QStringLiteral("onPrimaryContainer"),
                               QStringLiteral("inversePrimary"),
                               QStringLiteral("secondary"),
                               QStringLiteral("onSecondary"),
                               QStringLiteral("secondaryContainer"),
                               QStringLiteral("onSecondaryContainer"),
                               QStringLiteral("tertiary"),
                               QStringLiteral("onTertiary"),
                               QStringLiteral("tertiaryContainer"),
                               QStringLiteral("onTertiaryContainer"),
                               QStringLiteral("error"),
                               QStringLiteral("onError"),
                               QStringLiteral("errorContainer"),
                               QStringLiteral("onErrorContainer"),
                               QStringLiteral("background"),
                               QStringLiteral("onBackground"),
                               QStringLiteral("surface"),
                               QStringLiteral("onSurface"),
                               QStringLiteral("surfaceDim"),
                               QStringLiteral("surfaceBright"),
                               QStringLiteral("surfaceVariant"),
                               QStringLiteral("onSurfaceVariant"),
                               QStringLiteral("surfaceContainerLowest"),
                               QStringLiteral("surfaceContainerLow"),
                               QStringLiteral("surfaceContainer"),
                               QStringLiteral("surfaceContainerHigh"),
                               QStringLiteral("surfaceContainerHighest"),
                               QStringLiteral("outline"),
                               QStringLiteral("outlineVariant")};

    m_m3Chips.reserve(32);
    color_rgba_t placeholder{{100, 100, 100, 255}};
    for (int i = 0; i < 32; ++i) {
        auto *chip = new ColorSwatchChip(m3Names[i], placeholder, m3Container);
        connect(chip, &ColorSwatchChip::copiedToClipboard, this,
                &ThemeStudioWidget::onSwatchCopied);
        m_m3Chips.push_back(chip);
        m3Grid->addWidget(chip, i / 2, i % 2);
    }
    m3Scroll->setWidget(m3Container);
    tabWidget->addTab(m3Scroll, QStringLiteral("M3 (32)"));

    // Tab 2: ANSI Terminal Colors (16)
    auto *ansiScroll = new QScrollArea(tabWidget);
    ansiScroll->setWidgetResizable(true);
    ansiScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *ansiContainer = new QWidget();
    auto *ansiGrid = new QGridLayout(ansiContainer);
    ansiGrid->setContentsMargins(4, 4, 4, 4);
    ansiGrid->setSpacing(3);
    ansiGrid->setColumnStretch(0, 1);
    ansiGrid->setColumnStretch(1, 1);

    const QString ansiNames[] = {
        QStringLiteral("term0 (Black)"),         QStringLiteral("term1 (Red)"),
        QStringLiteral("term2 (Green)"),         QStringLiteral("term3 (Yellow)"),
        QStringLiteral("term4 (Blue)"),          QStringLiteral("term5 (Magenta)"),
        QStringLiteral("term6 (Cyan)"),          QStringLiteral("term7 (White)"),
        QStringLiteral("term8 (Bright Black)"),  QStringLiteral("term9 (Bright Red)"),
        QStringLiteral("term10 (Bright Green)"), QStringLiteral("term11 (Bright Yellow)"),
        QStringLiteral("term12 (Bright Blue)"),  QStringLiteral("term13 (Bright Magenta)"),
        QStringLiteral("term14 (Bright Cyan)"),  QStringLiteral("term15 (Bright White)")};

    m_ansiChips.reserve(16);
    for (int i = 0; i < 16; ++i) {
        auto *chip = new ColorSwatchChip(ansiNames[i], placeholder, ansiContainer);
        connect(chip, &ColorSwatchChip::copiedToClipboard, this,
                &ThemeStudioWidget::onSwatchCopied);
        m_ansiChips.push_back(chip);
        ansiGrid->addWidget(chip, i / 2, i % 2);
    }
    ansiScroll->setWidget(ansiContainer);
    tabWidget->addTab(ansiScroll, QStringLiteral("ANSI (16)"));

    // Tab 3: Catppuccin Aligned Tokens (14)
    auto *catScroll = new QScrollArea(tabWidget);
    catScroll->setWidgetResizable(true);
    catScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *catContainer = new QWidget();
    auto *catGrid = new QGridLayout(catContainer);
    catGrid->setContentsMargins(4, 4, 4, 4);
    catGrid->setSpacing(3);
    catGrid->setColumnStretch(0, 1);
    catGrid->setColumnStretch(1, 1);

    const QString catNames[] = {
        QStringLiteral("rosewater"), QStringLiteral("flamingo"), QStringLiteral("pink"),
        QStringLiteral("mauve"),     QStringLiteral("red"),      QStringLiteral("maroon"),
        QStringLiteral("peach"),     QStringLiteral("yellow"),   QStringLiteral("green"),
        QStringLiteral("teal"),      QStringLiteral("sky"),      QStringLiteral("sapphire"),
        QStringLiteral("blue"),      QStringLiteral("lavender")};

    m_catppuccinChips.reserve(14);
    for (int i = 0; i < 14; ++i) {
        auto *chip = new ColorSwatchChip(catNames[i], placeholder, catContainer);
        connect(chip, &ColorSwatchChip::copiedToClipboard, this,
                &ThemeStudioWidget::onSwatchCopied);
        m_catppuccinChips.push_back(chip);
        catGrid->addWidget(chip, i / 2, i % 2);
    }
    catScroll->setWidget(catContainer);
    tabWidget->addTab(catScroll, QStringLiteral("Catppuccin (14)"));

    // Tab 4: Template Exporter Widget (wrapped in QScrollArea so it never forces minimum size)
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
    QImage loaded(filePath);
    if (loaded.isNull() && QFileInfo::exists(filePath)) {
        uint8_t *rgba = nullptr;
        uint32_t w = 0, h = 0;
        std::string stdPath = filePath.toStdString();
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

    // If no wallpaper loaded, create a vibrant aesthetic procedural gradient
    if (m_sourceImage.isNull()) {
        const int w = 128;
        const int h = 128;
        QImage synthetic(w, h, QImage::Format_RGBA8888);
        QPainter p(&synthetic);
        QLinearGradient g(0, 0, w, h);
        g.setColorAt(0.0, QColor(48, 20, 96));
        g.setColorAt(0.5, QColor(140, 60, 180));
        g.setColorAt(1.0, QColor(240, 100, 80));
        p.fillRect(0, 0, w, h, g);
        p.end();
        m_sourceImage = synthetic;
        if (m_mockup) {
            m_mockup->setWallpaperImage(m_sourceImage);
        }
    }

    // Downscale to 128x128 for ultra-fast perceptual extraction (< 2.5 ms)
    QImage sample = m_sourceImage;
    if (sample.width() > 128 || sample.height() > 128) {
        sample = sample.scaled(128, 128, Qt::KeepAspectRatio, Qt::FastTransformation);
    }
    sample = sample.convertToFormat(QImage::Format_RGBA8888);

    // Call libwaywal_core / palette_hct.c extraction API
    const bool ok = waywal_palette_extract_seed_ex(
        sample.constBits(), static_cast<uint32_t>(sample.width()),
        static_cast<uint32_t>(sample.height()), static_cast<uint32_t>(sample.bytesPerLine()),
        false, // is_bgra = false
        m_mode, m_variant, m_contrast, m_seedIndex, m_hasSeedOverride, m_seedOverrideRgb,
        &m_palette);

    const auto t1 = std::chrono::steady_clock::now();
    const double elapsedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (m_perfLabel) {
        m_perfLabel->setText(QStringLiteral("%1 ms %2")
                                 .arg(QString::number(elapsedMs, 'f', 2))
                                 .arg(ok ? QStringLiteral("✓") : QStringLiteral("✗")));
    }

    // Propagate updated palette to Mockup and Exporter
    if (m_mockup) {
        m_mockup->setPalette(m_palette);
    }
    if (m_templateExporter) {
        m_templateExporter->setPalette(m_palette);
    }

    // Update UI components
    updateSeedRadioSwatches();
    updateWcagBadges();
    updateSwatchGrids();

    Q_EMIT paletteChanged(m_palette);
}

void ThemeStudioWidget::updateSeedRadioSwatches()
{
    for (int i = 0; i < 4; ++i) {
        if (m_seedSwatches[i]) {
            const uint32_t c = m_palette.seeds[i];
            const QString hex =
                QString::asprintf("#%02x%02x%02x", (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
            m_seedSwatches[i]->setStyleSheet(
                QStringLiteral("background-color: %1; border-radius: 3px; border: 1px solid "
                               "rgba(255,255,255,0.3);")
                    .arg(hex));
        }
    }

    if (m_customSeedSwatch) {
        if (m_hasSeedOverride) {
            const QString hex =
                QString::asprintf("#%02x%02x%02x", (m_seedOverrideRgb >> 16) & 0xFF,
                                  (m_seedOverrideRgb >> 8) & 0xFF, m_seedOverrideRgb & 0xFF);
            m_customSeedSwatch->setStyleSheet(
                QStringLiteral(
                    "background-color: %1; border-radius: 4px; border: 1px solid #FFFFFF;")
                    .arg(hex));
            m_clearSeedOverrideBtn->setEnabled(true);
        } else {
            m_customSeedSwatch->setStyleSheet(QStringLiteral(
                "background-color: transparent; border-radius: 4px; border: 1px dashed #777777;"));
            m_clearSeedOverrideBtn->setEnabled(false);
        }
    }
}

void ThemeStudioWidget::updateWcagBadges()
{
    if (m_wcagBadges.size() < 10) {
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
}

void ThemeStudioWidget::updateSwatchGrids()
{
    // 32 M3 tokens
    if (m_m3Chips.size() == 32) {
        const color_rgba_t colors[32] = {m_palette.primary,
                                         m_palette.onPrimary,
                                         m_palette.primaryContainer,
                                         m_palette.onPrimaryContainer,
                                         m_palette.inversePrimary,
                                         m_palette.secondary,
                                         m_palette.onSecondary,
                                         m_palette.secondaryContainer,
                                         m_palette.onSecondaryContainer,
                                         m_palette.tertiary,
                                         m_palette.onTertiary,
                                         m_palette.tertiaryContainer,
                                         m_palette.onTertiaryContainer,
                                         m_palette.error,
                                         m_palette.onError,
                                         m_palette.errorContainer,
                                         m_palette.onErrorContainer,
                                         m_palette.background,
                                         m_palette.onBackground,
                                         m_palette.surface,
                                         m_palette.onSurface,
                                         m_palette.surfaceDim,
                                         m_palette.surfaceBright,
                                         m_palette.surfaceVariant,
                                         m_palette.onSurfaceVariant,
                                         m_palette.surfaceContainerLowest,
                                         m_palette.surfaceContainerLow,
                                         m_palette.surfaceContainer,
                                         m_palette.surfaceContainerHigh,
                                         m_palette.surfaceContainerHighest,
                                         m_palette.outline,
                                         m_palette.outlineVariant};
        for (int i = 0; i < 32; ++i) {
            m_m3Chips[i]->updateColor(colors[i]);
        }
    }

    // 16 ANSI colors
    if (m_ansiChips.size() == 16) {
        for (int i = 0; i < 16; ++i) {
            m_ansiChips[i]->updateColor(m_palette.term[i]);
        }
    }

    // 14 Catppuccin tokens
    if (m_catppuccinChips.size() == 14) {
        const color_rgba_t catColors[14] = {
            m_palette.rosewater, m_palette.flamingo, m_palette.pink,  m_palette.mauve,
            m_palette.red,       m_palette.maroon,   m_palette.peach, m_palette.yellow,
            m_palette.green,     m_palette.teal,     m_palette.sky,   m_palette.sapphire,
            m_palette.blue,      m_palette.lavender};
        for (int i = 0; i < 14; ++i) {
            m_catppuccinChips[i]->updateColor(catColors[i]);
        }
    }
}

void ThemeStudioWidget::setThemeMode(waywal_theme_mode_t mode)
{
    if (m_mode != mode) {
        m_mode = mode;
        m_modeDarkBtn->setChecked(m_mode == WAYWAL_THEME_DARK);
        m_modeLightBtn->setChecked(m_mode == WAYWAL_THEME_LIGHT);
        reextract();
        Q_EMIT modeChanged(m_mode);
    }
}

void ThemeStudioWidget::setAutoSolarSchedule(bool enabled)
{
    m_autoSolarSchedule = enabled;
    m_modeDarkBtn->setEnabled(!m_autoSolarSchedule);
    m_modeLightBtn->setEnabled(!m_autoSolarSchedule);

    if (m_autoSolarSchedule) {
        applySolarScheduleEvaluation();
    } else {
        m_solarPhaseBadge->setText(QStringLiteral("Solar: Manual"));
    }
}

void ThemeStudioWidget::applySolarScheduleEvaluation()
{
    const time_t utc_now = time(nullptr);
    struct tm local_tm;
    localtime_r(&utc_now, &local_tm);

    waywal_solar_phase_t phase = WAYWAL_SOLAR_DAY;
    const bool isDaytime = waywal_solar_is_daytime_at(0.0, 0.0, false, utc_now, &local_tm, &phase);
    const char *phaseStr = waywal_solar_phase_to_string(phase);

    m_solarPhaseBadge->setText(QStringLiteral("Solar: %1 (%2)")
                                   .arg(isDaytime ? QStringLiteral("Day") : QStringLiteral("Night"))
                                   .arg(QString::fromUtf8(phaseStr)));

    const waywal_theme_mode_t desiredMode = isDaytime ? WAYWAL_THEME_LIGHT : WAYWAL_THEME_DARK;
    if (m_mode != desiredMode) {
        setThemeMode(desiredMode);
    }
}

void ThemeStudioWidget::onSolarTimerTick()
{
    if (m_autoSolarSchedule) {
        applySolarScheduleEvaluation();
    }
}

void ThemeStudioWidget::setSchemeVariant(waywal_scheme_variant_t variant)
{
    if (m_variant != variant) {
        m_variant = variant;
        reextract();
        Q_EMIT variantChanged(m_variant);
    }
}

void ThemeStudioWidget::setContrast(float contrast)
{
    if (std::abs(m_contrast - contrast) > 0.005f) {
        m_contrast = contrast;
        m_contrastValLabel->setText(
            QStringLiteral("Contrast: %1%2")
                .arg(m_contrast >= 0.0f ? QStringLiteral("+") : QStringLiteral(""))
                .arg(QString::number(m_contrast, 'f', 2)));
        reextract();
        Q_EMIT contrastChanged(m_contrast);
    }
}

void ThemeStudioWidget::setSeedIndex(uint8_t index)
{
    if (m_seedIndex != index || m_hasSeedOverride) {
        m_seedIndex = index;
        m_hasSeedOverride = false;
        reextract();
        Q_EMIT seedIndexChanged(m_seedIndex);
    }
}

void ThemeStudioWidget::openCustomSeedDialog()
{
    QColor current = m_hasSeedOverride
                         ? QColor((m_seedOverrideRgb >> 16) & 0xFF, (m_seedOverrideRgb >> 8) & 0xFF,
                                  m_seedOverrideRgb & 0xFF)
                         : toQColor(m_palette.primary);

#if HAVE_OKLCH_COLOR_DIALOG
    // Launch Worker-08's perceptual OkLCH color picker
    const QColor chosen = OklchColorDialog::getColor(
        current, this, QStringLiteral("Select Custom Seed Color Override"));
    if (chosen.isValid()) {
        m_hasSeedOverride = true;
        m_seedOverrideRgb = (static_cast<uint32_t>(chosen.red()) << 16) |
                            (static_cast<uint32_t>(chosen.green()) << 8) |
                            static_cast<uint32_t>(chosen.blue());
        reextract();
    }
#else
    const QColor chosen =
        QColorDialog::getColor(current, this, QStringLiteral("Select Custom Seed Color Override"),
                               QColorDialog::ShowAlphaChannel);
    if (chosen.isValid()) {
        m_hasSeedOverride = true;
        m_seedOverrideRgb = (static_cast<uint32_t>(chosen.red()) << 16) |
                            (static_cast<uint32_t>(chosen.green()) << 8) |
                            static_cast<uint32_t>(chosen.blue());
        reextract();
    }
#endif
}

void ThemeStudioWidget::clearCustomSeed()
{
    if (m_hasSeedOverride) {
        m_hasSeedOverride = false;
        m_seedOverrideRgb = 0;
        reextract();
    }
}

void ThemeStudioWidget::openWallpaperDialog()
{
    const QString filePath = QFileDialog::getOpenFileName(
        this, QStringLiteral("Select Wallpaper Image"), QDir::homePath(),
        QStringLiteral("Images (*.png *.jpg *.jpeg *.webp *.bmp *.svg);;All Files (*)"));

    if (!filePath.isEmpty()) {
        loadWallpaper(filePath);
    }
}

void ThemeStudioWidget::onSwatchCopied(const QString &name, const QString &hex)
{
    const QString msg = QStringLiteral("Copied %1 (%2) to clipboard!").arg(hex, name);
    if (m_clipboardToastLabel) {
        m_clipboardToastLabel->setText(msg);
        QTimer::singleShot(3500, this, [this]() {
            if (m_clipboardToastLabel) {
                m_clipboardToastLabel->clear();
            }
        });
    }
    Q_EMIT statusMessageRequested(msg);
}

} // namespace waywal::studio
