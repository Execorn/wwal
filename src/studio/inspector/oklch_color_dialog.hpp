// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_INSPECTOR_OKLCH_COLOR_DIALOG_HPP
#define WAYWAL_STUDIO_INSPECTOR_OKLCH_COLOR_DIALOG_HPP

#include <QColor>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QRadioButton>
#include <QSlider>
#include <QSpinBox>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace waywal::studio {

/**
 * @brief Representation of a perceptual color in OkLCH space.
 * L in [0.0, 1.0] (Perceptual Lightness)
 * C in [0.0, 0.4] (Chroma / Saturation)
 * h in [0.0, 360.0] (Hue angle in degrees)
 * a in [0.0, 1.0] (Alpha transparency)
 */
struct OklchColor {
    float l{1.0f};
    float c{0.0f};
    float h{0.0f};
    float a{1.0f};

    bool operator==(const OklchColor &other) const = default;
};

/**
 * @brief Representation of linear floating point sRGB triple.
 */
struct LinearRgb {
    float r{1.0f};
    float g{1.0f};
    float b{1.0f};
    float a{1.0f};
};

/**
 * @brief Pure perceptual color science mathematical conversion engine.
 * Implements Björn Ottosson's Oklab / OkLCH color space with exact IEC 61966-2-1
 * sRGB gamma transfer functions, bisection-based sRGB gamut boundary mapping,
 * and HSV bidirectional transformations.
 */
class ColorScience {
  public:
    static constexpr float PI_F = 3.14159265358979323846f;
    static constexpr float RAD_TO_DEG = 180.0f / PI_F;
    static constexpr float DEG_TO_RAD = PI_F / 180.0f;
    static constexpr float MAX_CHROMA = 0.4f;

    // --- Gamma Transfer Functions ---
    static inline float srgbToLinear(float val) noexcept
    {
        val = std::clamp(val, 0.0f, 1.0f);
        if (val <= 0.04045f) {
            return val / 12.92f;
        }
        return std::pow((val + 0.055f) / 1.055f, 2.4f);
    }

    static inline float linearToSrgb(float val) noexcept
    {
        if (val <= 0.0031308f) {
            return std::max(0.0f, val * 12.92f);
        }
        return 1.055f * std::pow(std::max(0.0f, val), 1.0f / 2.4f) - 0.055f;
    }

    // --- OkLab / Linear RGB Transformations ---
    static inline void linearRgbToOklab(float r, float g, float b, float &out_l, float &out_a,
                                        float &out_b) noexcept
    {
        // Linear RGB to LMS cone responses
        float l_lin = 0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b;
        float m_lin = 0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b;
        float s_lin = 0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b;

        // Cube root non-linearity
        float l_ = std::cbrt(std::max(0.0f, l_lin));
        float m_ = std::cbrt(std::max(0.0f, m_lin));
        float s_ = std::cbrt(std::max(0.0f, s_lin));

        out_l = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_;
        out_a = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
        out_b = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;
    }

    static inline void oklabToLinearRgb(float l, float a, float b_val, float &out_r, float &out_g,
                                        float &out_b) noexcept
    {
        float l_ = l + 0.3963377774f * a + 0.2158037573f * b_val;
        float m_ = l - 0.1055613458f * a - 0.0638541728f * b_val;
        float s_ = l - 0.0894841775f * a - 1.2914855480f * b_val;

        float l3 = l_ * l_ * l_;
        float m3 = m_ * m_ * m_;
        float s3 = s_ * s_ * s_;

        out_r = +4.0767416621f * l3 - 3.3077115913f * m3 + 0.2309699292f * s3;
        out_g = -1.2684380046f * l3 + 2.6097574011f * m3 - 0.3413193965f * s3;
        out_b = -0.0041960863f * l3 - 0.7034186147f * m3 + 1.7076147010f * s3;
    }

    // --- OkLab <-> OkLCH Transformations ---
    static inline void oklabToOklch(float l, float a, float b_val, float &out_l, float &out_c,
                                    float &out_h_deg) noexcept
    {
        out_l = l;
        out_c = std::sqrt(a * a + b_val * b_val);
        float h_rad = std::atan2(b_val, a);
        out_h_deg = h_rad * RAD_TO_DEG;
        if (out_h_deg < 0.0f) {
            out_h_deg += 360.0f;
        }
    }

    static inline void oklchToOklab(float l, float c, float h_deg, float &out_l, float &out_a,
                                    float &out_b) noexcept
    {
        out_l = l;
        float h_rad = h_deg * DEG_TO_RAD;
        out_a = c * std::cos(h_rad);
        out_b = c * std::sin(h_rad);
    }

    // --- End-to-End OkLCH <-> sRGB Transformations ---
    static inline OklchColor srgbToOklch(float r, float g, float b, float a = 1.0f) noexcept
    {
        float r_lin = srgbToLinear(r);
        float g_lin = srgbToLinear(g);
        float b_lin = srgbToLinear(b);

        float ok_l{0.0f}, ok_a{0.0f}, ok_b{0.0f};
        linearRgbToOklab(r_lin, g_lin, b_lin, ok_l, ok_a, ok_b);

        OklchColor res;
        res.a = a;
        oklabToOklch(ok_l, ok_a, ok_b, res.l, res.c, res.h);
        return res;
    }

    static inline void oklchToLinearRgb(float l, float c, float h_deg, float &out_r, float &out_g,
                                        float &out_b) noexcept
    {
        float ok_l{0.0f}, ok_a{0.0f}, ok_b{0.0f};
        oklchToOklab(l, c, h_deg, ok_l, ok_a, ok_b);
        oklabToLinearRgb(ok_l, ok_a, ok_b, out_r, out_g, out_b);
    }

    static inline bool isLinearRgbInGamut(float r, float g, float b, float eps = 1e-4f) noexcept
    {
        return (r >= -eps && r <= 1.0f + eps && g >= -eps && g <= 1.0f + eps && b >= -eps &&
                b <= 1.0f + eps);
    }

    static inline bool isOklchInGamut(float l, float c, float h_deg) noexcept
    {
        float r_lin{0.0f}, g_lin{0.0f}, b_lin{0.0f};
        oklchToLinearRgb(l, c, h_deg, r_lin, g_lin, b_lin);
        return isLinearRgbInGamut(r_lin, g_lin, b_lin);
    }

    /**
     * @brief Evaluates the maximum in-gamut chroma for a given Lightness and Hue
     * using a high-precision 16-step binary search (convergence < 0.0001).
     */
    static inline float maxInGamutChroma(float l, float h_deg) noexcept
    {
        if (l <= 0.0f || l >= 1.0f) {
            return 0.0f;
        }
        float lo = 0.0f;
        float hi = MAX_CHROMA;
        for (int i = 0; i < 16; ++i) {
            float mid = 0.5f * (lo + hi);
            if (isOklchInGamut(l, mid, h_deg)) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        return lo;
    }

    static inline void oklchToSrgbClamped(float l, float c, float h_deg, float &out_r, float &out_g,
                                          float &out_b) noexcept
    {
        float r_lin{0.0f}, g_lin{0.0f}, b_lin{0.0f};
        oklchToLinearRgb(l, c, h_deg, r_lin, g_lin, b_lin);

        out_r = std::clamp(linearToSrgb(r_lin), 0.0f, 1.0f);
        out_g = std::clamp(linearToSrgb(g_lin), 0.0f, 1.0f);
        out_b = std::clamp(linearToSrgb(b_lin), 0.0f, 1.0f);
    }

    static inline QColor oklchToQColor(const OklchColor &col) noexcept
    {
        float r{0.0f}, g{0.0f}, b{0.0f};
        oklchToSrgbClamped(col.l, col.c, col.h, r, g, b);
        int ir = std::clamp(static_cast<int>(std::round(r * 255.0f)), 0, 255);
        int ig = std::clamp(static_cast<int>(std::round(g * 255.0f)), 0, 255);
        int ib = std::clamp(static_cast<int>(std::round(b * 255.0f)), 0, 255);
        int ia = std::clamp(static_cast<int>(std::round(col.a * 255.0f)), 0, 255);
        return QColor(ir, ig, ib, ia);
    }

    static inline OklchColor qcolorToOklch(const QColor &qcol) noexcept
    {
        return srgbToOklch(qcol.redF(), qcol.greenF(), qcol.blueF(), qcol.alphaF());
    }

    // --- HSV Transformations ---
    static inline void srgbToHsv(float r, float g, float b, float &out_h, float &out_s,
                                 float &out_v) noexcept
    {
        float max_c = std::max({r, g, b});
        float min_c = std::min({r, g, b});
        float delta = max_c - min_c;

        out_v = max_c;
        out_s = (max_c > 1e-6f) ? (delta / max_c) : 0.0f;

        if (delta < 1e-6f) {
            out_h = 0.0f;
            return;
        }

        if (std::abs(max_c - r) < 1e-6f) {
            out_h = 60.0f * std::fmod((g - b) / delta, 6.0f);
        } else if (std::abs(max_c - g) < 1e-6f) {
            out_h = 60.0f * (((b - r) / delta) + 2.0f);
        } else {
            out_h = 60.0f * (((r - g) / delta) + 4.0f);
        }

        if (out_h < 0.0f) {
            out_h += 360.0f;
        }
    }

    static inline void hsvToSrgb(float h, float s, float v, float &out_r, float &out_g,
                                 float &out_b) noexcept
    {
        float c = v * s;
        float h_prime = std::fmod(h, 360.0f) / 60.0f;
        if (h_prime < 0.0f)
            h_prime += 6.0f;
        float x = c * (1.0f - std::abs(std::fmod(h_prime, 2.0f) - 1.0f));
        float m = v - c;

        float r1 = 0.0f, g1 = 0.0f, b1 = 0.0f;
        if (h_prime < 1.0f) {
            r1 = c;
            g1 = x;
            b1 = 0.0f;
        } else if (h_prime < 2.0f) {
            r1 = x;
            g1 = c;
            b1 = 0.0f;
        } else if (h_prime < 3.0f) {
            r1 = 0.0f;
            g1 = c;
            b1 = x;
        } else if (h_prime < 4.0f) {
            r1 = 0.0f;
            g1 = x;
            b1 = c;
        } else if (h_prime < 5.0f) {
            r1 = x;
            g1 = 0.0f;
            b1 = c;
        } else {
            r1 = c;
            g1 = 0.0f;
            b1 = x;
        }

        out_r = std::clamp(r1 + m, 0.0f, 1.0f);
        out_g = std::clamp(g1 + m, 0.0f, 1.0f);
        out_b = std::clamp(b1 + m, 0.0f, 1.0f);
    }
};

/**
 * @brief Interactive Perceptual OkLCH / HSV Color Wheel Widget.
 * In OkLCH mode: Angle is Hue [0..360], Radius is Chroma [0..0.4] at current Lightness.
 * In HSV mode: Angle is Hue [0..360], Radius is Saturation [0..1] at current Value.
 */
class OklchColorWheel : public QWidget {
    Q_OBJECT

  public:
    enum class Mode { OkLCH, HSV };

    explicit OklchColorWheel(QWidget *parent = nullptr);
    ~OklchColorWheel() override = default;

    [[nodiscard]] Mode mode() const noexcept { return m_mode; }
    void setMode(Mode m);

    [[nodiscard]] float lightness() const noexcept { return m_lightness; }
    void setLightness(float l);

    [[nodiscard]] float chroma() const noexcept { return m_chroma; }
    void setChroma(float c);

    [[nodiscard]] float hue() const noexcept { return m_hue; }
    void setHue(float h);

    [[nodiscard]] float saturation() const noexcept { return m_saturation; }
    void setSaturation(float s);

    [[nodiscard]] float value() const noexcept { return m_value; }
    void setValue(float v);

    void setOklch(float l, float c, float h);
    void setHsv(float h, float s, float v);

    [[nodiscard]] QSize sizeHint() const override { return {260, 260}; }
    [[nodiscard]] QSize minimumSizeHint() const override { return {180, 180}; }

  signals:
    void oklchChanged(float l, float c, float h);
    void hsvChanged(float h, float s, float v);
    void colorInteracted();

  protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

  private:
    void updateWheelCache();
    void handleMousePosition(const QPointF &pos);

    Mode m_mode{Mode::OkLCH};
    float m_lightness{0.70f};
    float m_chroma{0.16f};
    float m_hue{220.0f};

    float m_saturation{0.80f};
    float m_value{0.90f};

    bool m_dragging{false};
    QPixmap m_wheelPixmap;
    bool m_cacheDirty{true};
};

/**
 * @brief Interactive Perceptual OkLCH Color Dialog.
 */
class OklchColorDialog : public QDialog {
    Q_OBJECT

  public:
    explicit OklchColorDialog(QWidget *parent = nullptr);
    explicit OklchColorDialog(const QColor &initial, QWidget *parent = nullptr);
    ~OklchColorDialog() override = default;

    [[nodiscard]] QColor currentColor() const noexcept;
    void setCurrentColor(const QColor &color);

    void setOklch(float l, float c, float h, float a = 1.0f);
    void getOklch(float &out_l, float &out_c, float &out_h, float *out_a = nullptr) const noexcept;
    void getLinearRgb(float &out_r, float &out_g, float &out_b) const noexcept;

    /**
     * @brief Modal dialog convenience helper matching QColorDialog::getColor().
     */
    static QColor getColor(const QColor &initial = Qt::white, QWidget *parent = nullptr,
                           const QString &title = QString());

  signals:
    void colorChanged(const QColor &color);
    void oklchChanged(float l, float c, float h);

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private slots:
    void onWheelOklchChanged(float l, float c, float h);
    void onWheelHsvChanged(float h, float s, float v);
    void onSliderLightnessChanged(int val);
    void onSliderChromaChanged(int val);
    void onSliderHueChanged(int val);
    void onSliderAlphaChanged(int val);
    void onSpinOklchChanged();
    void onSpinRgbChanged();
    void onSpinHsvChanged();
    void onHexEdited(const QString &text);
    void onModeToggled(bool oklchMode);
    void onGamutMapClicked();
    void onResetToInitialClicked();
    void onQuickColorClicked(const QColor &col);

  private:
    void setupUi();
    void updateAllControlsFromOklch(bool updateWheel = true, bool updateSliders = true,
                                    bool updateSpins = true, bool updateHex = true);
    void updatePreviewSwatches();

    OklchColor m_currentOklch{0.70f, 0.16f, 220.0f, 1.0f};
    OklchColor m_initialOklch{0.70f, 0.16f, 220.0f, 1.0f};
    bool m_updating{false};

    // UI Widgets
    QRadioButton *m_radioOklch{nullptr};
    QRadioButton *m_radioHsv{nullptr};
    OklchColorWheel *m_colorWheel{nullptr};

    // Sliders
    QSlider *m_sliderL{nullptr};
    QSlider *m_sliderC{nullptr};
    QSlider *m_sliderH{nullptr};
    QSlider *m_sliderA{nullptr};

    // OkLCH SpinBoxes
    QDoubleSpinBox *m_spinL{nullptr};
    QDoubleSpinBox *m_spinC{nullptr};
    QDoubleSpinBox *m_spinH{nullptr};
    QDoubleSpinBox *m_spinA{nullptr};

    // RGB SpinBoxes (0..255)
    QSpinBox *m_spinR{nullptr};
    QSpinBox *m_spinG{nullptr};
    QSpinBox *m_spinB{nullptr};

    // RGB Float SpinBoxes (0..1)
    QDoubleSpinBox *m_spinRf{nullptr};
    QDoubleSpinBox *m_spinGf{nullptr};
    QDoubleSpinBox *m_spinBf{nullptr};

    // HSV SpinBoxes
    QDoubleSpinBox *m_spinHsvH{nullptr};
    QDoubleSpinBox *m_spinHsvS{nullptr};
    QDoubleSpinBox *m_spinHsvV{nullptr};

    // Hex and Badges
    QLineEdit *m_hexEdit{nullptr};
    QWidget *m_swatchCurrent{nullptr};
    QWidget *m_swatchInitial{nullptr};
    QWidget *m_gamutWarningWidget{nullptr};
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_INSPECTOR_OKLCH_COLOR_DIALOG_HPP
