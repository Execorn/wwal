// SPDX-License-Identifier: GPL-3.0-or-later
#include "oklch_color_dialog.hpp"

#include <QApplication>
#include <QBoxLayout>
#include <QButtonGroup>
#include <QClipboard>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QToolButton>
#include <QVBoxLayout>

namespace waywal::studio {

// =============================================================================
// OklchColorWheel Implementation
// =============================================================================

OklchColorWheel::OklchColorWheel(QWidget *parent)
    : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void OklchColorWheel::setMode(Mode m) {
    if (m_mode != m) {
        m_mode = m;
        m_cacheDirty = true;
        update();
    }
}

void OklchColorWheel::setLightness(float l) {
    float clamped = std::clamp(l, 0.0f, 1.0f);
    if (std::abs(m_lightness - clamped) > 1e-4f) {
        m_lightness = clamped;
        if (m_mode == Mode::OkLCH) {
            m_cacheDirty = true;
        }
        update();
    }
}

void OklchColorWheel::setChroma(float c) {
    float clamped = std::clamp(c, 0.0f, ColorScience::MAX_CHROMA);
    if (std::abs(m_chroma - clamped) > 1e-4f) {
        m_chroma = clamped;
        update();
    }
}

void OklchColorWheel::setHue(float h) {
    float norm_h = std::fmod(h, 360.0f);
    if (norm_h < 0.0f) norm_h += 360.0f;
    if (std::abs(m_hue - norm_h) > 1e-3f) {
        m_hue = norm_h;
        update();
    }
}

void OklchColorWheel::setSaturation(float s) {
    float clamped = std::clamp(s, 0.0f, 1.0f);
    if (std::abs(m_saturation - clamped) > 1e-4f) {
        m_saturation = clamped;
        update();
    }
}

void OklchColorWheel::setValue(float v) {
    float clamped = std::clamp(v, 0.0f, 1.0f);
    if (std::abs(m_value - clamped) > 1e-4f) {
        m_value = clamped;
        if (m_mode == Mode::HSV) {
            m_cacheDirty = true;
        }
        update();
    }
}

void OklchColorWheel::setOklch(float l, float c, float h) {
    setLightness(l);
    setChroma(c);
    setHue(h);
}

void OklchColorWheel::setHsv(float h, float s, float v) {
    setHue(h);
    setSaturation(s);
    setValue(v);
}

void OklchColorWheel::resizeEvent(QResizeEvent *) {
    m_cacheDirty = true;
}

void OklchColorWheel::updateWheelCache() {
    int w = width();
    int h = height();
    if (w <= 0 || h <= 0) return;

    qreal dpr = devicePixelRatioF();
    int pixW = static_cast<int>(std::round(w * dpr));
    int pixH = static_cast<int>(std::round(h * dpr));

    QImage img(pixW, pixH, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    float cx = pixW * 0.5f;
    float cy = pixH * 0.5f;
    float radius = std::min(cx, cy) - 6.0f * static_cast<float>(dpr);

    if (radius <= 1.0f) {
        m_wheelPixmap = QPixmap::fromImage(img);
        m_cacheDirty = false;
        return;
    }

    const float maxChroma = ColorScience::MAX_CHROMA;

    for (int y = 0; y < pixH; ++y) {
        auto *scanLine = reinterpret_cast<QRgb *>(img.scanLine(y));
        float dy = -(static_cast<float>(y) - cy);

        for (int x = 0; x < pixW; ++x) {
            float dx = static_cast<float>(x) - cx;
            float dist = std::sqrt(dx * dx + dy * dy);

            if (dist > radius + 1.0f) {
                scanLine[x] = 0x00000000;
                continue;
            }

            float alpha_edge = 1.0f;
            if (dist > radius - 1.0f) {
                alpha_edge = std::clamp(radius + 1.0f - dist, 0.0f, 1.0f);
            }

            float angle_rad = std::atan2(dy, dx);
            float angle_deg = angle_rad * ColorScience::RAD_TO_DEG;
            if (angle_deg < 0.0f) angle_deg += 360.0f;

            float norm_r = std::clamp(dist / radius, 0.0f, 1.0f);

            float r_out = 0.0f, g_out = 0.0f, b_out = 0.0f;

            if (m_mode == Mode::OkLCH) {
                float chroma = norm_r * maxChroma;
                float r_lin = 0.0f, g_lin = 0.0f, b_lin = 0.0f;
                ColorScience::oklchToLinearRgb(m_lightness, chroma, angle_deg, r_lin, g_lin, b_lin);

                bool inGamut = ColorScience::isLinearRgbInGamut(r_lin, g_lin, b_lin);
                r_out = ColorScience::linearToSrgb(r_lin);
                g_out = ColorScience::linearToSrgb(g_lin);
                b_out = ColorScience::linearToSrgb(b_lin);

                if (!inGamut) {
                    // Dim out-of-gamut regions subtly to reveal sRGB boundary
                    r_out = std::clamp(r_out, 0.0f, 1.0f) * 0.45f;
                    g_out = std::clamp(g_out, 0.0f, 1.0f) * 0.45f;
                    b_out = std::clamp(b_out, 0.0f, 1.0f) * 0.45f;
                }
            } else {
                ColorScience::hsvToSrgb(angle_deg, norm_r, m_value, r_out, g_out, b_out);
            }

            int ir = std::clamp(static_cast<int>(std::round(r_out * 255.0f * alpha_edge)), 0, 255);
            int ig = std::clamp(static_cast<int>(std::round(g_out * 255.0f * alpha_edge)), 0, 255);
            int ib = std::clamp(static_cast<int>(std::round(b_out * 255.0f * alpha_edge)), 0, 255);
            int ia = std::clamp(static_cast<int>(std::round(255.0f * alpha_edge)), 0, 255);

            scanLine[x] = qRgba(ir, ig, ib, ia);
        }
    }

    m_wheelPixmap = QPixmap::fromImage(img);
    m_wheelPixmap.setDevicePixelRatio(dpr);
    m_cacheDirty = false;
}

void OklchColorWheel::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    if (m_cacheDirty || m_wheelPixmap.isNull()) {
        updateWheelCache();
    }

    // Draw cached wheel pixmap
    p.drawPixmap(0, 0, m_wheelPixmap);

    float cx = width() * 0.5f;
    float cy = height() * 0.5f;
    float radius = std::min(cx, cy) - 6.0f;
    if (radius <= 1.0f) return;

    // Outer wheel border ring
    p.setPen(QPen(QColor(60, 65, 85, 200), 2.0));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(cx, cy), radius, radius);

    // In OkLCH mode, draw sRGB gamut boundary contour
    if (m_mode == Mode::OkLCH && m_lightness > 0.01f && m_lightness < 0.99f) {
        QPolygonF gamutContour;
        const int steps = 90;
        for (int i = 0; i <= steps; ++i) {
            float deg = (static_cast<float>(i) * 360.0f) / static_cast<float>(steps);
            float maxC = ColorScience::maxInGamutChroma(m_lightness, deg);
            float normC = std::clamp(maxC / ColorScience::MAX_CHROMA, 0.0f, 1.0f);
            float r_boundary = normC * radius;
            float rad = deg * ColorScience::DEG_TO_RAD;
            float gx = cx + r_boundary * std::cos(rad);
            float gy = cy - r_boundary * std::sin(rad);
            gamutContour << QPointF(gx, gy);
        }
        p.setPen(QPen(QColor(255, 255, 255, 140), 1.2, Qt::DashLine));
        p.drawPolyline(gamutContour);
    }

    // Determine crosshair / selector indicator position
    float sel_r = 0.0f;
    float sel_deg = m_hue;
    if (m_mode == Mode::OkLCH) {
        sel_r = std::clamp(m_chroma / ColorScience::MAX_CHROMA, 0.0f, 1.0f) * radius;
    } else {
        sel_r = std::clamp(m_saturation, 0.0f, 1.0f) * radius;
    }

    float sel_rad = sel_deg * ColorScience::DEG_TO_RAD;
    float sel_x = cx + sel_r * std::cos(sel_rad);
    float sel_y = cy - sel_r * std::sin(sel_rad);

    // Compute color for selector dot
    QColor indicatorColor;
    if (m_mode == Mode::OkLCH) {
        indicatorColor = ColorScience::oklchToQColor(OklchColor{m_lightness, m_chroma, m_hue, 1.0f});
    } else {
        float r = 0.0f, g = 0.0f, b = 0.0f;
        ColorScience::hsvToSrgb(m_hue, m_saturation, m_value, r, g, b);
        indicatorColor = QColor::fromRgbF(r, g, b);
    }

    // Double-ring indicator (outer dark shadow, inner white ring, colored center)
    p.setPen(QPen(QColor(0, 0, 0, 180), 3.0));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(sel_x, sel_y), 7.0, 7.0);

    p.setPen(QPen(QColor(255, 255, 255, 255), 1.5));
    p.setBrush(indicatorColor);
    p.drawEllipse(QPointF(sel_x, sel_y), 5.5, 5.5);
}

void OklchColorWheel::handleMousePosition(const QPointF &pos) {
    float cx = width() * 0.5f;
    float cy = height() * 0.5f;
    float radius = std::min(cx, cy) - 6.0f;
    if (radius <= 1.0f) return;

    float dx = pos.x() - cx;
    float dy = -(pos.y() - cy);

    float dist = std::sqrt(dx * dx + dy * dy);
    float norm_r = std::clamp(dist / radius, 0.0f, 1.0f);

    float angle_rad = std::atan2(dy, dx);
    float angle_deg = angle_rad * ColorScience::RAD_TO_DEG;
    if (angle_deg < 0.0f) angle_deg += 360.0f;

    m_hue = angle_deg;

    if (m_mode == Mode::OkLCH) {
        m_chroma = norm_r * ColorScience::MAX_CHROMA;
        emit oklchChanged(m_lightness, m_chroma, m_hue);
    } else {
        m_saturation = norm_r;
        emit hsvChanged(m_hue, m_saturation, m_value);
    }

    emit colorInteracted();
    update();
}

void OklchColorWheel::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        handleMousePosition(event->position());
    }
}

void OklchColorWheel::mouseMoveEvent(QMouseEvent *event) {
    if (m_dragging) {
        handleMousePosition(event->position());
    }
}

void OklchColorWheel::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
    }
}

// =============================================================================
// OklchColorDialog Implementation
// =============================================================================

OklchColorDialog::OklchColorDialog(QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(tr("OkLCH Perceptual Color Picker"));
    setMinimumSize(680, 520);
    setupUi();
    updateAllControlsFromOklch();
}

OklchColorDialog::OklchColorDialog(const QColor &initial, QWidget *parent)
    : OklchColorDialog(parent) {
    setCurrentColor(initial);
    m_initialOklch = m_currentOklch;
    updatePreviewSwatches();
}

void OklchColorDialog::setupUi() {
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(12);

    // --- Mode Toolbar ---
    auto *modeBar = new QHBoxLayout();
    auto *modeLabel = new QLabel(tr("<b>Color Model:</b>"), this);
    m_radioOklch = new QRadioButton(tr("OkLCH (Perceptual Uniform)"), this);
    m_radioHsv = new QRadioButton(tr("HSV (Standard)"), this);
    m_radioOklch->setChecked(true);

    auto *modeGroup = new QButtonGroup(this);
    modeGroup->addButton(m_radioOklch, 0);
    modeGroup->addButton(m_radioHsv, 1);
    connect(m_radioOklch, &QRadioButton::toggled, this, &OklchColorDialog::onModeToggled);

    modeBar->addWidget(modeLabel);
    modeBar->addWidget(m_radioOklch);
    modeBar->addWidget(m_radioHsv);
    modeBar->addStretch();

    // Gamut Warning / Action
    m_gamutWarningWidget = new QWidget(this);
    auto *gwLayout = new QHBoxLayout(m_gamutWarningWidget);
    gwLayout->setContentsMargins(0, 0, 0, 0);
    auto *warnLabel = new QLabel(tr("⚠️ <i>Out of sRGB Gamut</i>"), m_gamutWarningWidget);
    warnLabel->setStyleSheet("color: #e5c890; font-weight: bold;");
    auto *btnGamutMap = new QPushButton(tr("Map to Gamut"), m_gamutWarningWidget);
    btnGamutMap->setStyleSheet("background: #414559; color: #c6d0f5; padding: 3px 8px; border-radius: 4px;");
    connect(btnGamutMap, &QPushButton::clicked, this, &OklchColorDialog::onGamutMapClicked);
    gwLayout->addWidget(warnLabel);
    gwLayout->addWidget(btnGamutMap);
    m_gamutWarningWidget->setVisible(false);
    modeBar->addWidget(m_gamutWarningWidget);

    mainLayout->addLayout(modeBar);

    // --- Center Split ---
    auto *centerLayout = new QHBoxLayout();
    centerLayout->setSpacing(16);

    // Left Column: Color Wheel + Sliders
    auto *leftCol = new QVBoxLayout();
    leftCol->setSpacing(10);

    m_colorWheel = new OklchColorWheel(this);
    connect(m_colorWheel, &OklchColorWheel::oklchChanged, this, &OklchColorDialog::onWheelOklchChanged);
    connect(m_colorWheel, &OklchColorWheel::hsvChanged, this, &OklchColorDialog::onWheelHsvChanged);
    leftCol->addWidget(m_colorWheel, 1);

    // Lightness Slider
    auto *layoutL = new QHBoxLayout();
    auto *lblL = new QLabel(tr("Lightness (L):"), this);
    lblL->setFixedWidth(95);
    m_sliderL = new QSlider(Qt::Horizontal, this);
    m_sliderL->setRange(0, 1000);
    m_sliderL->setValue(700);
    m_spinL = new QDoubleSpinBox(this);
    m_spinL->setRange(0.0, 1.0);
    m_spinL->setSingleStep(0.01);
    m_spinL->setDecimals(3);
    m_spinL->setValue(0.70);
    m_spinL->setFixedWidth(75);
    connect(m_sliderL, &QSlider::valueChanged, this, &OklchColorDialog::onSliderLightnessChanged);
    connect(m_spinL, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &OklchColorDialog::onSpinOklchChanged);
    layoutL->addWidget(lblL);
    layoutL->addWidget(m_sliderL);
    layoutL->addWidget(m_spinL);
    leftCol->addLayout(layoutL);

    // Chroma Slider
    auto *layoutC = new QHBoxLayout();
    auto *lblC = new QLabel(tr("Chroma (C):"), this);
    lblC->setFixedWidth(95);
    m_sliderC = new QSlider(Qt::Horizontal, this);
    m_sliderC->setRange(0, 400);
    m_sliderC->setValue(160);
    m_spinC = new QDoubleSpinBox(this);
    m_spinC->setRange(0.0, 0.40);
    m_spinC->setSingleStep(0.005);
    m_spinC->setDecimals(3);
    m_spinC->setValue(0.160);
    m_spinC->setFixedWidth(75);
    connect(m_sliderC, &QSlider::valueChanged, this, &OklchColorDialog::onSliderChromaChanged);
    connect(m_spinC, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &OklchColorDialog::onSpinOklchChanged);
    layoutC->addWidget(lblC);
    layoutC->addWidget(m_sliderC);
    layoutC->addWidget(m_spinC);
    leftCol->addLayout(layoutC);

    // Hue Slider
    auto *layoutH = new QHBoxLayout();
    auto *lblH = new QLabel(tr("Hue (h°):"), this);
    lblH->setFixedWidth(95);
    m_sliderH = new QSlider(Qt::Horizontal, this);
    m_sliderH->setRange(0, 3600);
    m_sliderH->setValue(2200);
    m_spinH = new QDoubleSpinBox(this);
    m_spinH->setRange(0.0, 360.0);
    m_spinH->setSingleStep(1.0);
    m_spinH->setDecimals(1);
    m_spinH->setValue(220.0);
    m_spinH->setFixedWidth(75);
    connect(m_sliderH, &QSlider::valueChanged, this, &OklchColorDialog::onSliderHueChanged);
    connect(m_spinH, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &OklchColorDialog::onSpinOklchChanged);
    layoutH->addWidget(lblH);
    layoutH->addWidget(m_sliderH);
    layoutH->addWidget(m_spinH);
    leftCol->addLayout(layoutH);

    // Alpha Slider
    auto *layoutA = new QHBoxLayout();
    auto *lblA = new QLabel(tr("Opacity (A):"), this);
    lblA->setFixedWidth(95);
    m_sliderA = new QSlider(Qt::Horizontal, this);
    m_sliderA->setRange(0, 1000);
    m_sliderA->setValue(1000);
    m_spinA = new QDoubleSpinBox(this);
    m_spinA->setRange(0.0, 1.0);
    m_spinA->setSingleStep(0.01);
    m_spinA->setDecimals(2);
    m_spinA->setValue(1.0);
    m_spinA->setFixedWidth(75);
    connect(m_sliderA, &QSlider::valueChanged, this, &OklchColorDialog::onSliderAlphaChanged);
    connect(m_spinA, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &OklchColorDialog::onSpinOklchChanged);
    layoutA->addWidget(lblA);
    layoutA->addWidget(m_sliderA);
    layoutA->addWidget(m_spinA);
    leftCol->addLayout(layoutA);

    centerLayout->addLayout(leftCol, 5);

    // Right Column: Preview, Hex, Numeric Spaces, Quick Swatches
    auto *rightCol = new QVBoxLayout();
    rightCol->setSpacing(10);

    // Swatches: New vs Initial
    auto *swatchGroup = new QGroupBox(tr("Color Comparison"), this);
    auto *swatchLayout = new QHBoxLayout(swatchGroup);
    m_swatchCurrent = new QWidget(swatchGroup);
    m_swatchCurrent->setFixedHeight(48);
    m_swatchCurrent->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_swatchInitial = new QWidget(swatchGroup);
    m_swatchInitial->setFixedHeight(48);
    m_swatchInitial->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_swatchInitial->setToolTip(tr("Click to restore initial color"));
    m_swatchInitial->setCursor(Qt::PointingHandCursor);

    auto *btnRestore = new QToolButton(swatchGroup);
    btnRestore->setText(tr("⟲"));
    btnRestore->setToolTip(tr("Reset to initial color"));
    connect(btnRestore, &QToolButton::clicked, this, &OklchColorDialog::onResetToInitialClicked);

    swatchLayout->addWidget(new QLabel(tr("<b>New:</b>"), swatchGroup));
    swatchLayout->addWidget(m_swatchCurrent, 1);
    swatchLayout->addWidget(new QLabel(tr("<b>Old:</b>"), swatchGroup));
    swatchLayout->addWidget(m_swatchInitial, 1);
    swatchLayout->addWidget(btnRestore);
    rightCol->addWidget(swatchGroup);

    // Hex Input
    auto *hexLayout = new QHBoxLayout();
    auto *hexLabel = new QLabel(tr("<b>Hex Code:</b>"), this);
    m_hexEdit = new QLineEdit(this);
    m_hexEdit->setMaxLength(9);
    m_hexEdit->setFixedWidth(110);
    auto *btnCopyHex = new QPushButton(tr("Copy"), this);
    btnCopyHex->setFixedWidth(55);
    connect(m_hexEdit, &QLineEdit::textEdited, this, &OklchColorDialog::onHexEdited);
    connect(btnCopyHex, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(m_hexEdit->text());
    });
    hexLayout->addWidget(hexLabel);
    hexLayout->addWidget(m_hexEdit);
    hexLayout->addWidget(btnCopyHex);
    hexLayout->addStretch();
    rightCol->addLayout(hexLayout);

    // RGB Values
    auto *rgbGroup = new QGroupBox(tr("sRGB (0..255 and 0.0..1.0)"), this);
    auto *rgbLayout = new QGridLayout(rgbGroup);
    rgbLayout->setContentsMargins(8, 8, 8, 8);
    rgbLayout->setHorizontalSpacing(8);

    m_spinR = new QSpinBox(rgbGroup);
    m_spinR->setRange(0, 255);
    m_spinG = new QSpinBox(rgbGroup);
    m_spinG->setRange(0, 255);
    m_spinB = new QSpinBox(rgbGroup);
    m_spinB->setRange(0, 255);

    m_spinRf = new QDoubleSpinBox(rgbGroup);
    m_spinRf->setRange(0.0, 1.0);
    m_spinRf->setDecimals(3);
    m_spinGf = new QDoubleSpinBox(rgbGroup);
    m_spinGf->setRange(0.0, 1.0);
    m_spinGf->setDecimals(3);
    m_spinBf = new QDoubleSpinBox(rgbGroup);
    m_spinBf->setRange(0.0, 1.0);
    m_spinBf->setDecimals(3);

    connect(m_spinR, qOverload<int>(&QSpinBox::valueChanged), this, &OklchColorDialog::onSpinRgbChanged);
    connect(m_spinG, qOverload<int>(&QSpinBox::valueChanged), this, &OklchColorDialog::onSpinRgbChanged);
    connect(m_spinB, qOverload<int>(&QSpinBox::valueChanged), this, &OklchColorDialog::onSpinRgbChanged);

    connect(m_spinRf, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
        if (!m_updating) {
            m_spinR->setValue(static_cast<int>(std::round(m_spinRf->value() * 255.0)));
        }
    });
    connect(m_spinGf, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
        if (!m_updating) {
            m_spinG->setValue(static_cast<int>(std::round(m_spinGf->value() * 255.0)));
        }
    });
    connect(m_spinBf, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
        if (!m_updating) {
            m_spinB->setValue(static_cast<int>(std::round(m_spinBf->value() * 255.0)));
        }
    });

    rgbLayout->addWidget(new QLabel("R:", rgbGroup), 0, 0);
    rgbLayout->addWidget(m_spinR, 0, 1);
    rgbLayout->addWidget(m_spinRf, 0, 2);

    rgbLayout->addWidget(new QLabel("G:", rgbGroup), 1, 0);
    rgbLayout->addWidget(m_spinG, 1, 1);
    rgbLayout->addWidget(m_spinGf, 1, 2);

    rgbLayout->addWidget(new QLabel("B:", rgbGroup), 2, 0);
    rgbLayout->addWidget(m_spinB, 2, 1);
    rgbLayout->addWidget(m_spinBf, 2, 2);

    rightCol->addWidget(rgbGroup);

    // HSV Values
    auto *hsvGroup = new QGroupBox(tr("HSV (Standard)"), this);
    auto *hsvLayout = new QGridLayout(hsvGroup);
    hsvLayout->setContentsMargins(8, 8, 8, 8);
    m_spinHsvH = new QDoubleSpinBox(hsvGroup);
    m_spinHsvH->setRange(0.0, 360.0);
    m_spinHsvH->setDecimals(1);
    m_spinHsvS = new QDoubleSpinBox(hsvGroup);
    m_spinHsvS->setRange(0.0, 100.0);
    m_spinHsvS->setDecimals(1);
    m_spinHsvS->setSuffix("%");
    m_spinHsvV = new QDoubleSpinBox(hsvGroup);
    m_spinHsvV->setRange(0.0, 100.0);
    m_spinHsvV->setDecimals(1);
    m_spinHsvV->setSuffix("%");

    connect(m_spinHsvH, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &OklchColorDialog::onSpinHsvChanged);
    connect(m_spinHsvS, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &OklchColorDialog::onSpinHsvChanged);
    connect(m_spinHsvV, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &OklchColorDialog::onSpinHsvChanged);

    hsvLayout->addWidget(new QLabel("H:", hsvGroup), 0, 0);
    hsvLayout->addWidget(m_spinHsvH, 0, 1);
    hsvLayout->addWidget(new QLabel("S:", hsvGroup), 0, 2);
    hsvLayout->addWidget(m_spinHsvS, 0, 3);
    hsvLayout->addWidget(new QLabel("V:", hsvGroup), 0, 4);
    hsvLayout->addWidget(m_spinHsvV, 0, 5);
    rightCol->addWidget(hsvGroup);

    // Quick Palette Swatches
    auto *palGroup = new QGroupBox(tr("Quick Swatches"), this);
    auto *palGrid = new QGridLayout(palGroup);
    palGrid->setContentsMargins(6, 6, 6, 6);
    palGrid->setSpacing(4);

    const QColor quickColors[16] = {
        QColor("#e78284"), QColor("#ea999c"), QColor("#ef9f76"), QColor("#e5c890"),
        QColor("#a6d189"), QColor("#81c8be"), QColor("#99d1db"), QColor("#85c1dc"),
        QColor("#8caaee"), QColor("#babbf1"), QColor("#ca9ee6"), QColor("#f4b8e4"),
        QColor("#ffffff"), QColor("#a5adce"), QColor("#414559"), QColor("#000000")
    };

    for (int i = 0; i < 16; ++i) {
        auto *btn = new QPushButton(palGroup);
        btn->setFixedSize(26, 26);
        btn->setStyleSheet(QString("background-color: %1; border: 1px solid #51576d; border-radius: 4px;")
                               .arg(quickColors[i].name()));
        btn->setCursor(Qt::PointingHandCursor);
        QColor c = quickColors[i];
        connect(btn, &QPushButton::clicked, this, [this, c]() { onQuickColorClicked(c); });
        palGrid->addWidget(btn, i / 8, i % 8);
    }
    rightCol->addWidget(palGroup);

    centerLayout->addLayout(rightCol, 4);
    mainLayout->addLayout(centerLayout, 1);

    // --- Bottom Action Buttons ---
    auto *btnBox = new QHBoxLayout();
    btnBox->setContentsMargins(0, 8, 0, 0);

    auto *btnReset = new QPushButton(tr("Reset"), this);
    connect(btnReset, &QPushButton::clicked, this, &OklchColorDialog::onResetToInitialClicked);

    auto *btnCancel = new QPushButton(tr("Cancel"), this);
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);

    auto *btnOk = new QPushButton(tr("OK"), this);
    btnOk->setDefault(true);
    btnOk->setStyleSheet("background: #8caaee; color: #1e1e2e; font-weight: bold; padding: 6px 18px; border-radius: 5px;");
    connect(btnOk, &QPushButton::clicked, this, &QDialog::accept);

    btnBox->addWidget(btnReset);
    btnBox->addStretch();
    btnBox->addWidget(btnCancel);
    btnBox->addWidget(btnOk);
    mainLayout->addLayout(btnBox);
}

QColor OklchColorDialog::currentColor() const noexcept {
    return ColorScience::oklchToQColor(m_currentOklch);
}

void OklchColorDialog::setCurrentColor(const QColor &color) {
    m_currentOklch = ColorScience::qcolorToOklch(color);
    updateAllControlsFromOklch();
}

void OklchColorDialog::setOklch(float l, float c, float h, float a) {
    m_currentOklch.l = std::clamp(l, 0.0f, 1.0f);
    m_currentOklch.c = std::clamp(c, 0.0f, ColorScience::MAX_CHROMA);
    m_currentOklch.h = std::fmod(h, 360.0f);
    if (m_currentOklch.h < 0.0f) m_currentOklch.h += 360.0f;
    m_currentOklch.a = std::clamp(a, 0.0f, 1.0f);
    updateAllControlsFromOklch();
}

void OklchColorDialog::getOklch(float &out_l, float &out_c, float &out_h, float *out_a) const noexcept {
    out_l = m_currentOklch.l;
    out_c = m_currentOklch.c;
    out_h = m_currentOklch.h;
    if (out_a) {
        *out_a = m_currentOklch.a;
    }
}

void OklchColorDialog::getLinearRgb(float &out_r, float &out_g, float &out_b) const noexcept {
    ColorScience::oklchToLinearRgb(m_currentOklch.l, m_currentOklch.c, m_currentOklch.h, out_r, out_g, out_b);
}

QColor OklchColorDialog::getColor(const QColor &initial, QWidget *parent, const QString &title) {
    OklchColorDialog dlg(initial, parent);
    if (!title.isEmpty()) {
        dlg.setWindowTitle(title);
    }
    if (dlg.exec() == QDialog::Accepted) {
        return dlg.currentColor();
    }
    return initial;
}

void OklchColorDialog::updatePreviewSwatches() {
    QColor cur = currentColor();
    m_swatchCurrent->setStyleSheet(
        QString("background-color: %1; border: 1px solid #51576d; border-radius: 6px;").arg(cur.name(QColor::HexArgb)));

    QColor initCol = ColorScience::oklchToQColor(m_initialOklch);
    m_swatchInitial->setStyleSheet(
        QString("background-color: %1; border: 1px solid #51576d; border-radius: 6px;").arg(initCol.name(QColor::HexArgb)));
}

void OklchColorDialog::updateAllControlsFromOklch(bool updateWheel, bool updateSliders,
                                                 bool updateSpins, bool updateHex) {
    if (m_updating) return;
    m_updating = true;

    // Check gamut status
    bool inGamut = ColorScience::isOklchInGamut(m_currentOklch.l, m_currentOklch.c, m_currentOklch.h);
    m_gamutWarningWidget->setVisible(!inGamut);

    // Update Wheel
    if (updateWheel && m_colorWheel) {
        m_colorWheel->setLightness(m_currentOklch.l);
        m_colorWheel->setChroma(m_currentOklch.c);
        m_colorWheel->setHue(m_currentOklch.h);

        float r = 0.0f, g = 0.0f, b = 0.0f;
        ColorScience::oklchToSrgbClamped(m_currentOklch.l, m_currentOklch.c, m_currentOklch.h, r, g, b);
        float h_hsv = 0.0f, s_hsv = 0.0f, v_hsv = 0.0f;
        ColorScience::srgbToHsv(r, g, b, h_hsv, s_hsv, v_hsv);
        m_colorWheel->setSaturation(s_hsv);
        m_colorWheel->setValue(v_hsv);
    }

    // Update Sliders
    if (updateSliders) {
        m_sliderL->setValue(static_cast<int>(std::round(m_currentOklch.l * 1000.0f)));
        m_sliderC->setValue(static_cast<int>(std::round(m_currentOklch.c * 1000.0f)));
        m_sliderH->setValue(static_cast<int>(std::round(m_currentOklch.h * 10.0f)));
        m_sliderA->setValue(static_cast<int>(std::round(m_currentOklch.a * 1000.0f)));
    }

    // Update OkLCH Spinboxes
    if (updateSpins) {
        m_spinL->setValue(m_currentOklch.l);
        m_spinC->setValue(m_currentOklch.c);
        m_spinH->setValue(m_currentOklch.h);
        m_spinA->setValue(m_currentOklch.a);

        // Update sRGB
        float r = 0.0f, g = 0.0f, b = 0.0f;
        ColorScience::oklchToSrgbClamped(m_currentOklch.l, m_currentOklch.c, m_currentOklch.h, r, g, b);

        int ir = std::clamp(static_cast<int>(std::round(r * 255.0f)), 0, 255);
        int ig = std::clamp(static_cast<int>(std::round(g * 255.0f)), 0, 255);
        int ib = std::clamp(static_cast<int>(std::round(b * 255.0f)), 0, 255);

        m_spinR->setValue(ir);
        m_spinG->setValue(ig);
        m_spinB->setValue(ib);

        m_spinRf->setValue(r);
        m_spinGf->setValue(g);
        m_spinBf->setValue(b);

        // Update HSV
        float h_hsv = 0.0f, s_hsv = 0.0f, v_hsv = 0.0f;
        ColorScience::srgbToHsv(r, g, b, h_hsv, s_hsv, v_hsv);
        m_spinHsvH->setValue(h_hsv);
        m_spinHsvS->setValue(s_hsv * 100.0f);
        m_spinHsvV->setValue(v_hsv * 100.0f);
    }

    // Update Hex
    if (updateHex && m_hexEdit) {
        QColor col = currentColor();
        if (m_currentOklch.a < 0.999f) {
            m_hexEdit->setText(col.name(QColor::HexArgb).toUpper());
        } else {
            m_hexEdit->setText(col.name(QColor::HexRgb).toUpper());
        }
    }

    updatePreviewSwatches();
    m_updating = false;

    emit colorChanged(currentColor());
    emit oklchChanged(m_currentOklch.l, m_currentOklch.c, m_currentOklch.h);
}

void OklchColorDialog::onWheelOklchChanged(float l, float c, float h) {
    if (m_updating) return;
    m_currentOklch.l = l;
    m_currentOklch.c = c;
    m_currentOklch.h = h;
    updateAllControlsFromOklch(false, true, true, true);
}

void OklchColorDialog::onWheelHsvChanged(float h, float s, float v) {
    if (m_updating) return;
    float r = 0.0f, g = 0.0f, b = 0.0f;
    ColorScience::hsvToSrgb(h, s, v, r, g, b);
    m_currentOklch = ColorScience::srgbToOklch(r, g, b, m_currentOklch.a);
    updateAllControlsFromOklch(false, true, true, true);
}

void OklchColorDialog::onSliderLightnessChanged(int val) {
    if (m_updating) return;
    m_currentOklch.l = static_cast<float>(val) / 1000.0f;
    updateAllControlsFromOklch(true, false, true, true);
}

void OklchColorDialog::onSliderChromaChanged(int val) {
    if (m_updating) return;
    m_currentOklch.c = static_cast<float>(val) / 1000.0f;
    updateAllControlsFromOklch(true, false, true, true);
}

void OklchColorDialog::onSliderHueChanged(int val) {
    if (m_updating) return;
    m_currentOklch.h = static_cast<float>(val) / 10.0f;
    updateAllControlsFromOklch(true, false, true, true);
}

void OklchColorDialog::onSliderAlphaChanged(int val) {
    if (m_updating) return;
    m_currentOklch.a = static_cast<float>(val) / 1000.0f;
    updateAllControlsFromOklch(false, false, true, true);
}

void OklchColorDialog::onSpinOklchChanged() {
    if (m_updating) return;
    m_currentOklch.l = static_cast<float>(m_spinL->value());
    m_currentOklch.c = static_cast<float>(m_spinC->value());
    m_currentOklch.h = static_cast<float>(m_spinH->value());
    m_currentOklch.a = static_cast<float>(m_spinA->value());
    updateAllControlsFromOklch(true, true, false, true);
}

void OklchColorDialog::onSpinRgbChanged() {
    if (m_updating) return;
    float r = static_cast<float>(m_spinR->value()) / 255.0f;
    float g = static_cast<float>(m_spinG->value()) / 255.0f;
    float b = static_cast<float>(m_spinB->value()) / 255.0f;
    m_currentOklch = ColorScience::srgbToOklch(r, g, b, m_currentOklch.a);
    updateAllControlsFromOklch(true, true, false, true);
}

void OklchColorDialog::onSpinHsvChanged() {
    if (m_updating) return;
    float h = static_cast<float>(m_spinHsvH->value());
    float s = static_cast<float>(m_spinHsvS->value()) / 100.0f;
    float v = static_cast<float>(m_spinHsvV->value()) / 100.0f;
    float r = 0.0f, g = 0.0f, b = 0.0f;
    ColorScience::hsvToSrgb(h, s, v, r, g, b);
    m_currentOklch = ColorScience::srgbToOklch(r, g, b, m_currentOklch.a);
    updateAllControlsFromOklch(true, true, true, true);
}

void OklchColorDialog::onHexEdited(const QString &text) {
    if (m_updating) return;
    QString cleaned = text.trimmed();
    if (!cleaned.startsWith('#')) {
        cleaned.prepend('#');
    }
    if (QColor::isValidColorName(cleaned)) {
        QColor col(cleaned);
        m_currentOklch = ColorScience::qcolorToOklch(col);
        updateAllControlsFromOklch(true, true, true, false);
    }
}

void OklchColorDialog::onModeToggled(bool oklchMode) {
    if (m_colorWheel) {
        m_colorWheel->setMode(oklchMode ? OklchColorWheel::Mode::OkLCH : OklchColorWheel::Mode::HSV);
    }
}

void OklchColorDialog::onGamutMapClicked() {
    float maxC = ColorScience::maxInGamutChroma(m_currentOklch.l, m_currentOklch.h);
    m_currentOklch.c = std::min(m_currentOklch.c, maxC);
    updateAllControlsFromOklch();
}

void OklchColorDialog::onResetToInitialClicked() {
    m_currentOklch = m_initialOklch;
    updateAllControlsFromOklch();
}

void OklchColorDialog::onQuickColorClicked(const QColor &col) {
    m_currentOklch = ColorScience::qcolorToOklch(col);
    updateAllControlsFromOklch();
}

} // namespace waywal::studio
