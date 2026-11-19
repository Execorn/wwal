// SPDX-License-Identifier: GPL-3.0-or-later
#include "oklch_color_dialog.hpp"

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QEvent>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdint>

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(WWAL_DISABLE_SIMD)
#include <cpuid.h>
#include <immintrin.h>
#endif

namespace waywal::studio {

namespace {

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(WWAL_DISABLE_SIMD)
/* Isolate _xgetbv under target("xsave") so builds without global -mavx2 stay ABI-valid. */
[[gnu::target("xsave")]] inline unsigned long long readXcr0Xsave() noexcept { return _xgetbv(0); }

inline bool detectAvx2FmaRuntime() noexcept
{
    unsigned int eax = 0U, ebx = 0U, ecx = 0U, edx = 0U;
    if (__get_cpuid(0, &eax, &ebx, &ecx, &edx) == 0 || eax < 7U) {
        return false;
    }
    __get_cpuid(1, &eax, &ebx, &ecx, &edx);
    const bool hasFma = (ecx & (1U << 12)) != 0U;
    const bool hasXsave = (ecx & (1U << 26)) != 0U;
    const bool hasOsxsave = (ecx & (1U << 27)) != 0U;
    const bool hasAvx = (ecx & (1U << 28)) != 0U;
    if (!hasFma || !hasXsave || !hasOsxsave || !hasAvx) {
        return false;
    }
    if ((readXcr0Xsave() & 0x6ULL) != 0x6ULL) {
        return false;
    }
    unsigned int a7 = 0U, b7 = 0U, c7 = 0U, d7 = 0U;
    __get_cpuid_count(7, 0, &a7, &b7, &c7, &d7);
    return (b7 & (1U << 5)) != 0U;
}

inline bool cpuSupportsAvx2Fma() noexcept
{
    static const bool supported = detectAvx2FmaRuntime();
    return supported;
}

[[gnu::target("avx2,fma")]] inline __m256 linearToSrgbAvx2(__m256 val) noexcept
{
    const __m256 vZero = _mm256_setzero_ps();
    const __m256 vOne = _mm256_set1_ps(1.0f);
    const __m256 c = _mm256_min_ps(_mm256_max_ps(val, vZero), vOne);

    const __m256 vLin = _mm256_mul_ps(c, _mm256_set1_ps(12.92f));
    const __m256 t = _mm256_sqrt_ps(_mm256_sqrt_ps(c));

    // Degree-4 polynomial fit of t^(5/3) on [0.0031308^0.25, 1.0] (max error < 0.022 / 255).
    __m256 p = _mm256_fmadd_ps(_mm256_set1_ps(0.073509858f), t, _mm256_set1_ps(-0.307881301f));
    p = _mm256_fmadd_ps(p, t, _mm256_set1_ps(1.054059631f));
    p = _mm256_fmadd_ps(p, t, _mm256_set1_ps(0.190060169f));
    p = _mm256_fmadd_ps(p, t, _mm256_set1_ps(-0.009696498f));

    const __m256 vPow = _mm256_fmadd_ps(p, _mm256_set1_ps(1.055f), _mm256_set1_ps(-0.055f));
    const __m256 vLowMask = _mm256_cmp_ps(c, _mm256_set1_ps(0.0031308f), _CMP_LE_OQ);
    const __m256 res = _mm256_blendv_ps(vPow, vLin, vLowMask);
    return _mm256_min_ps(_mm256_max_ps(res, vZero), vOne);
}

[[gnu::target("avx2,fma")]] inline int rasterizeOklchSpanAvx2(int xStart, int xEnd, float cx,
                                                              float dy, float radius,
                                                              float lightness,
                                                              QRgb *scanLine) noexcept
{
    const float invRadiusMaxC = ColorScience::MAX_CHROMA / radius;
    const float okB = dy * invRadiusMaxC;
    const float rowL0 = lightness + 0.2158037573f * okB;
    const float rowM0 = lightness - 0.0638541728f * okB;
    const float rowS0 = lightness - 1.2914855480f * okB;

    const __m256 vZero = _mm256_setzero_ps();
    const __m256 vOne = _mm256_set1_ps(1.0f);
    const __m256 vPoint45 = _mm256_set1_ps(0.45f);
    const __m256 v255 = _mm256_set1_ps(255.0f);
    const __m256 vNegEps = _mm256_set1_ps(-1e-4f);
    const __m256 vOnePlusEps = _mm256_set1_ps(1.0f + 1e-4f);
    const __m256 vRadPlus1 = _mm256_set1_ps(radius + 1.0f);
    const __m256 vCx = _mm256_set1_ps(cx);
    const __m256 vDy2 = _mm256_set1_ps(dy * dy);
    const __m256 vInvRadMaxC = _mm256_set1_ps(invRadiusMaxC);
    const __m256 vLaneOffsets = _mm256_setr_ps(0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f);

    const __m256 vRowL0 = _mm256_set1_ps(rowL0);
    const __m256 vRowM0 = _mm256_set1_ps(rowM0);
    const __m256 vRowS0 = _mm256_set1_ps(rowS0);
    const __m256 vCaL = _mm256_set1_ps(0.3963377774f);
    const __m256 vCaM = _mm256_set1_ps(-0.1055613458f);
    const __m256 vCaS = _mm256_set1_ps(-0.0894841775f);

    const __m256i vZeroI = _mm256_setzero_si256();
    const __m256i v255I = _mm256_set1_epi32(255);

    int x = xStart;
    for (; x + 8 <= xEnd; x += 8) {
        const __m256 vx = _mm256_add_ps(_mm256_set1_ps(static_cast<float>(x)), vLaneOffsets);
        const __m256 vdx = _mm256_sub_ps(vx, vCx);
        const __m256 vdist = _mm256_sqrt_ps(_mm256_fmadd_ps(vdx, vdx, vDy2));
        const __m256 vAlpha =
            _mm256_min_ps(_mm256_max_ps(_mm256_sub_ps(vRadPlus1, vdist), vZero), vOne);

        const __m256 vOkA = _mm256_mul_ps(vdx, vInvRadMaxC);
        const __m256 l_ = _mm256_fmadd_ps(vCaL, vOkA, vRowL0);
        const __m256 m_ = _mm256_fmadd_ps(vCaM, vOkA, vRowM0);
        const __m256 s_ = _mm256_fmadd_ps(vCaS, vOkA, vRowS0);

        const __m256 l3 = _mm256_mul_ps(_mm256_mul_ps(l_, l_), l_);
        const __m256 m3 = _mm256_mul_ps(_mm256_mul_ps(m_, m_), m_);
        const __m256 s3 = _mm256_mul_ps(_mm256_mul_ps(s_, s_), s_);

        __m256 rLin = _mm256_mul_ps(_mm256_set1_ps(+4.0767416621f), l3);
        rLin = _mm256_fmadd_ps(_mm256_set1_ps(-3.3077115913f), m3, rLin);
        rLin = _mm256_fmadd_ps(_mm256_set1_ps(+0.2309699292f), s3, rLin);

        __m256 gLin = _mm256_mul_ps(_mm256_set1_ps(-1.2684380046f), l3);
        gLin = _mm256_fmadd_ps(_mm256_set1_ps(+2.6097574011f), m3, gLin);
        gLin = _mm256_fmadd_ps(_mm256_set1_ps(-0.3413193965f), s3, gLin);

        __m256 bLin = _mm256_mul_ps(_mm256_set1_ps(-0.0041960863f), l3);
        bLin = _mm256_fmadd_ps(_mm256_set1_ps(-0.7034186147f), m3, bLin);
        bLin = _mm256_fmadd_ps(_mm256_set1_ps(+1.7076147010f), s3, bLin);

        const __m256 rOk = _mm256_and_ps(_mm256_cmp_ps(rLin, vNegEps, _CMP_GE_OQ),
                                         _mm256_cmp_ps(rLin, vOnePlusEps, _CMP_LE_OQ));
        const __m256 gOk = _mm256_and_ps(_mm256_cmp_ps(gLin, vNegEps, _CMP_GE_OQ),
                                         _mm256_cmp_ps(gLin, vOnePlusEps, _CMP_LE_OQ));
        const __m256 bOk = _mm256_and_ps(_mm256_cmp_ps(bLin, vNegEps, _CMP_GE_OQ),
                                         _mm256_cmp_ps(bLin, vOnePlusEps, _CMP_LE_OQ));
        const __m256 vInGamut = _mm256_and_ps(_mm256_and_ps(rOk, gOk), bOk);
        const __m256 vDim = _mm256_blendv_ps(vPoint45, vOne, vInGamut);

        const __m256 rSrgb = linearToSrgbAvx2(rLin);
        const __m256 gSrgb = linearToSrgbAvx2(gLin);
        const __m256 bSrgb = linearToSrgbAvx2(bLin);

        const __m256 vAlpha255 = _mm256_mul_ps(vAlpha, v255);
        const __m256 vScale = _mm256_mul_ps(vAlpha255, vDim);

        __m256i ir = _mm256_cvtps_epi32(_mm256_mul_ps(rSrgb, vScale));
        __m256i ig = _mm256_cvtps_epi32(_mm256_mul_ps(gSrgb, vScale));
        __m256i ib = _mm256_cvtps_epi32(_mm256_mul_ps(bSrgb, vScale));
        __m256i ia = _mm256_cvtps_epi32(vAlpha255);

        ir = _mm256_min_epi32(_mm256_max_epi32(ir, vZeroI), v255I);
        ig = _mm256_min_epi32(_mm256_max_epi32(ig, vZeroI), v255I);
        ib = _mm256_min_epi32(_mm256_max_epi32(ib, vZeroI), v255I);
        ia = _mm256_min_epi32(_mm256_max_epi32(ia, vZeroI), v255I);

        const __m256i px =
            _mm256_or_si256(_mm256_slli_epi32(ia, 24),
                            _mm256_or_si256(_mm256_slli_epi32(ir, 16),
                                            _mm256_or_si256(_mm256_slli_epi32(ig, 8), ib)));
        _mm256_storeu_si256(reinterpret_cast<__m256i *>(scanLine + x), px);
    }
    return x;
}
#endif

inline float linearToSrgbPolyScalar(float val) noexcept
{
    const float c = std::clamp(val, 0.0f, 1.0f);
    if (c <= 0.0031308f) {
        return c * 12.92f;
    }
    const float t = std::sqrt(std::sqrt(c));
    const float p =
        (((0.073509858f * t - 0.307881301f) * t + 1.054059631f) * t + 0.190060169f) * t -
        0.009696498f;
    return std::clamp(1.055f * p - 0.055f, 0.0f, 1.0f);
}

inline int hexDigitVal(QChar ch) noexcept
{
    const ushort u = ch.unicode();
    if (u >= '0' && u <= '9') {
        return static_cast<int>(u - '0');
    }
    if (u >= 'a' && u <= 'f') {
        return static_cast<int>(u - 'a' + 10);
    }
    if (u >= 'A' && u <= 'F') {
        return static_cast<int>(u - 'A' + 10);
    }
    return -1;
}

inline bool parseCssHexColor(const QString &input, float fallbackAlpha, int &outR, int &outG,
                             int &outB, float &outAlpha) noexcept
{
    QString clean = input.trimmed();
    if (clean.startsWith(QLatin1Char('#'))) {
        clean.remove(0, 1);
    }
    const int n = clean.size();
    if (n != 3 && n != 4 && n != 6 && n != 8) {
        return false;
    }
    for (int i = 0; i < n; ++i) {
        if (hexDigitVal(clean.at(i)) < 0) {
            return false;
        }
    }

    auto byteAt = [&clean](int idx) noexcept -> int {
        return hexDigitVal(clean.at(idx)) * 16 + hexDigitVal(clean.at(idx + 1));
    };

    if (n == 3 || n == 4) {
        outR = hexDigitVal(clean.at(0)) * 17;
        outG = hexDigitVal(clean.at(1)) * 17;
        outB = hexDigitVal(clean.at(2)) * 17;
        outAlpha =
            (n == 4) ? (static_cast<float>(hexDigitVal(clean.at(3)) * 17) / 255.0f) : fallbackAlpha;
        return true;
    }

    outR = byteAt(0);
    outG = byteAt(2);
    outB = byteAt(4);
    outAlpha = (n == 8) ? (static_cast<float>(byteAt(6)) / 255.0f) : fallbackAlpha;
    return true;
}

} // namespace

// =============================================================================
// OklchColorWheel Implementation
// =============================================================================

OklchColorWheel::OklchColorWheel(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAccessibleName(tr("OkLCH Color Wheel"));
    setAccessibleDescription(
        tr("Use arrow keys to adjust hue and chroma or saturation. Hold Shift for fine steps."));
}

void OklchColorWheel::setMode(Mode m)
{
    if (m_mode != m) {
        m_mode = m;
        m_cacheDirty = true;
        update();
    }
}

void OklchColorWheel::setLightness(float l)
{
    const float clamped = ColorScience::sanitizeClamped(l, 0.0f, 1.0f, m_lightness);
    if (std::abs(m_lightness - clamped) > 1e-4f) {
        m_lightness = clamped;
        m_gamutDirsValid = false;
        if (m_mode == Mode::OkLCH) {
            m_cacheDirty = true;
        }
        update();
    }
}

void OklchColorWheel::setChroma(float c)
{
    const float clamped =
        ColorScience::sanitizeClamped(c, 0.0f, ColorScience::MAX_CHROMA, m_chroma);
    if (std::abs(m_chroma - clamped) > 1e-4f) {
        m_chroma = clamped;
        update();
    }
}

void OklchColorWheel::setHue(float h)
{
    const float normH = ColorScience::normalizeHueDeg(h);
    if (std::abs(m_hue - normH) > 1e-3f) {
        m_hue = normH;
        update();
    }
}

void OklchColorWheel::setSaturation(float s)
{
    const float clamped = ColorScience::sanitizeClamped(s, 0.0f, 1.0f, m_saturation);
    if (std::abs(m_saturation - clamped) > 1e-4f) {
        m_saturation = clamped;
        update();
    }
}

void OklchColorWheel::setValue(float v)
{
    const float clamped = ColorScience::sanitizeClamped(v, 0.0f, 1.0f, m_value);
    if (std::abs(m_value - clamped) > 1e-4f) {
        m_value = clamped;
        if (m_mode == Mode::HSV) {
            m_cacheDirty = true;
        }
        update();
    }
}

void OklchColorWheel::setOklch(float l, float c, float h)
{
    setLightness(l);
    setChroma(c);
    setHue(h);
}

void OklchColorWheel::setHsv(float h, float s, float v)
{
    setHue(h);
    setSaturation(s);
    setValue(v);
}

void OklchColorWheel::resizeEvent(QResizeEvent *) { m_cacheDirty = true; }

bool OklchColorWheel::event(QEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    if (event && event->type() == QEvent::DevicePixelRatioChange) {
        m_cacheDirty = true;
        update();
    }
#endif
    return QWidget::event(event);
}

void OklchColorWheel::updateGamutCache()
{
    if (m_gamutDirsValid && std::abs(m_lightness - m_cachedGamutLightness) <= 1e-4f) {
        return;
    }
    constexpr int steps = 90;
    for (int i = 0; i <= steps; ++i) {
        const float deg = (static_cast<float>(i) * 360.0f) / static_cast<float>(steps);
        const float maxC = ColorScience::maxInGamutChroma(m_lightness, deg);
        const float normC = std::clamp(maxC / ColorScience::MAX_CHROMA, 0.0f, 1.0f);
        const float rad = deg * ColorScience::DEG_TO_RAD;
        m_cachedGamutDirs[static_cast<size_t>(i)] =
            QPointF(normC * std::cos(rad), -normC * std::sin(rad));
    }
    m_cachedGamutLightness = m_lightness;
    m_gamutDirsValid = true;
}

void OklchColorWheel::updateWheelCache()
{
    const int w = width();
    const int h = height();
    if (w <= 0 || h <= 0) {
        return;
    }

    const qreal dpr = std::max(1.0, devicePixelRatioF());
    const int pixW = std::max(1, static_cast<int>(std::lround(w * dpr)));
    const int pixH = std::max(1, static_cast<int>(std::lround(h * dpr)));

    QImage img(pixW, pixH, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    const float cx = pixW * 0.5f;
    const float cy = pixH * 0.5f;
    const float radius = std::min(cx, cy) - 6.0f * static_cast<float>(dpr);

    if (radius <= 1.0f) {
        m_wheelPixmap = QPixmap::fromImage(img);
        m_wheelPixmap.setDevicePixelRatio(dpr);
        m_cachedDpr = dpr;
        m_cacheDirty = false;
        return;
    }

    const float rOuter = radius + 1.0f;
    const float rOuter2 = rOuter * rOuter;

    if (m_mode == Mode::OkLCH) {
        const float invRadiusMaxC = ColorScience::MAX_CHROMA / radius;
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(WWAL_DISABLE_SIMD)
        const bool useAvx2 = cpuSupportsAvx2Fma();
#endif

        for (int y = 0; y < pixH; ++y) {
            auto *scanLine = reinterpret_cast<QRgb *>(img.scanLine(y));
            const float dy = -(static_cast<float>(y) - cy);
            const float dy2 = dy * dy;
            if (dy2 > rOuter2) {
                continue;
            }

            const float halfSpan = std::sqrt(std::max(0.0f, rOuter2 - dy2));
            const int xStart = std::max(0, static_cast<int>(std::floor(cx - halfSpan)));
            const int xEnd = std::min(pixW, static_cast<int>(std::ceil(cx + halfSpan)) + 1);

            int x = xStart;
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(WWAL_DISABLE_SIMD)
            if (useAvx2 && (xEnd - xStart) >= 8) {
                x = rasterizeOklchSpanAvx2(xStart, xEnd, cx, dy, radius, m_lightness, scanLine);
            }
#endif

            const float okB = dy * invRadiusMaxC;
            const float rowL0 = m_lightness + 0.2158037573f * okB;
            const float rowM0 = m_lightness - 0.0638541728f * okB;
            const float rowS0 = m_lightness - 1.2914855480f * okB;

            for (; x < xEnd; ++x) {
                const float dx = static_cast<float>(x) - cx;
                const float dist2 = dx * dx + dy2;
                if (dist2 > rOuter2) {
                    continue;
                }

                const float dist = std::sqrt(dist2);
                const float alphaEdge = std::clamp(rOuter - dist, 0.0f, 1.0f);

                const float okA = dx * invRadiusMaxC;
                const float l_ = rowL0 + 0.3963377774f * okA;
                const float m_ = rowM0 - 0.1055613458f * okA;
                const float s_ = rowS0 - 0.0894841775f * okA;

                const float l3 = l_ * l_ * l_;
                const float m3 = m_ * m_ * m_;
                const float s3 = s_ * s_ * s_;

                const float rLin = +4.0767416621f * l3 - 3.3077115913f * m3 + 0.2309699292f * s3;
                const float gLin = -1.2684380046f * l3 + 2.6097574011f * m3 - 0.3413193965f * s3;
                const float bLin = -0.0041960863f * l3 - 0.7034186147f * m3 + 1.7076147010f * s3;

                const bool inGamut = ColorScience::isLinearRgbInGamut(rLin, gLin, bLin);
                const float dim = inGamut ? 1.0f : 0.45f;

                const float rOut = linearToSrgbPolyScalar(rLin) * dim;
                const float gOut = linearToSrgbPolyScalar(gLin) * dim;
                const float bOut = linearToSrgbPolyScalar(bLin) * dim;

                const int ir =
                    std::clamp(static_cast<int>(std::lround(rOut * 255.0f * alphaEdge)), 0, 255);
                const int ig =
                    std::clamp(static_cast<int>(std::lround(gOut * 255.0f * alphaEdge)), 0, 255);
                const int ib =
                    std::clamp(static_cast<int>(std::lround(bOut * 255.0f * alphaEdge)), 0, 255);
                const int ia =
                    std::clamp(static_cast<int>(std::lround(255.0f * alphaEdge)), 0, 255);

                scanLine[x] = qRgba(ir, ig, ib, ia);
            }
        }
    } else {
        for (int y = 0; y < pixH; ++y) {
            auto *scanLine = reinterpret_cast<QRgb *>(img.scanLine(y));
            const float dy = -(static_cast<float>(y) - cy);
            const float dy2 = dy * dy;
            if (dy2 > rOuter2) {
                continue;
            }

            const float halfSpan = std::sqrt(std::max(0.0f, rOuter2 - dy2));
            const int xStart = std::max(0, static_cast<int>(std::floor(cx - halfSpan)));
            const int xEnd = std::min(pixW, static_cast<int>(std::ceil(cx + halfSpan)) + 1);

            for (int x = xStart; x < xEnd; ++x) {
                const float dx = static_cast<float>(x) - cx;
                const float dist2 = dx * dx + dy2;
                if (dist2 > rOuter2) {
                    continue;
                }

                const float dist = std::sqrt(dist2);
                const float alphaEdge = std::clamp(rOuter - dist, 0.0f, 1.0f);

                float angleDeg = std::atan2(dy, dx) * ColorScience::RAD_TO_DEG;
                if (angleDeg < 0.0f) {
                    angleDeg += 360.0f;
                }
                const float normR = std::clamp(dist / radius, 0.0f, 1.0f);

                float rOut = 0.0f, gOut = 0.0f, bOut = 0.0f;
                ColorScience::hsvToSrgb(angleDeg, normR, m_value, rOut, gOut, bOut);

                const int ir =
                    std::clamp(static_cast<int>(std::lround(rOut * 255.0f * alphaEdge)), 0, 255);
                const int ig =
                    std::clamp(static_cast<int>(std::lround(gOut * 255.0f * alphaEdge)), 0, 255);
                const int ib =
                    std::clamp(static_cast<int>(std::lround(bOut * 255.0f * alphaEdge)), 0, 255);
                const int ia =
                    std::clamp(static_cast<int>(std::lround(255.0f * alphaEdge)), 0, 255);

                scanLine[x] = qRgba(ir, ig, ib, ia);
            }
        }
    }

    m_wheelPixmap = QPixmap::fromImage(img);
    m_wheelPixmap.setDevicePixelRatio(dpr);
    m_cachedDpr = dpr;
    m_cacheDirty = false;
}

void OklchColorWheel::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const qreal dpr = std::max(1.0, devicePixelRatioF());
    if (m_cacheDirty || m_wheelPixmap.isNull() || std::abs(dpr - m_cachedDpr) > 1e-4) {
        updateWheelCache();
    }

    p.drawPixmap(0, 0, m_wheelPixmap);

    const float cx = width() * 0.5f;
    const float cy = height() * 0.5f;
    const float radius = std::min(cx, cy) - 6.0f;
    if (radius <= 1.0f) {
        return;
    }

    p.setPen(QPen(QColor(60, 65, 85, 200), 2.0));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(cx, cy), radius, radius);

    if (hasFocus()) {
        p.setPen(QPen(QColor(0x38, 0xBD, 0xF8), 2.0));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(cx, cy), radius + 2.5f, radius + 2.5f);
    }

    if (m_mode == Mode::OkLCH && m_lightness > 0.01f && m_lightness < 0.99f) {
        updateGamutCache();
        QPolygonF gamutContour;
        gamutContour.reserve(static_cast<int>(m_cachedGamutDirs.size()));
        for (const QPointF &dir : m_cachedGamutDirs) {
            gamutContour << QPointF(cx + dir.x() * radius, cy + dir.y() * radius);
        }
        p.setPen(QPen(QColor(255, 255, 255, 140), 1.2, Qt::DashLine));
        p.drawPolyline(gamutContour);
    }

    float selR = 0.0f;
    if (m_mode == Mode::OkLCH) {
        selR = std::clamp(m_chroma / ColorScience::MAX_CHROMA, 0.0f, 1.0f) * radius;
    } else {
        selR = std::clamp(m_saturation, 0.0f, 1.0f) * radius;
    }

    const float selRad = m_hue * ColorScience::DEG_TO_RAD;
    const float selX = cx + selR * std::cos(selRad);
    const float selY = cy - selR * std::sin(selRad);

    QColor indicatorColor;
    if (m_mode == Mode::OkLCH) {
        indicatorColor =
            ColorScience::oklchToQColor(OklchColor{m_lightness, m_chroma, m_hue, 1.0f});
    } else {
        float r = 0.0f, g = 0.0f, b = 0.0f;
        ColorScience::hsvToSrgb(m_hue, m_saturation, m_value, r, g, b);
        indicatorColor = QColor::fromRgbF(r, g, b);
    }

    p.setPen(QPen(QColor(0, 0, 0, 180), 3.0));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(selX, selY), 7.0, 7.0);

    p.setPen(QPen(QColor(255, 255, 255, 255), 1.5));
    p.setBrush(indicatorColor);
    p.drawEllipse(QPointF(selX, selY), 5.5, 5.5);
}

void OklchColorWheel::handleMousePosition(const QPointF &pos)
{
    const float cx = width() * 0.5f;
    const float cy = height() * 0.5f;
    const float radius = std::min(cx, cy) - 6.0f;
    if (radius <= 1.0f) {
        return;
    }

    const float dx = static_cast<float>(pos.x()) - cx;
    const float dy = -(static_cast<float>(pos.y()) - cy);

    const float dist = std::sqrt(dx * dx + dy * dy);
    const float normR = std::clamp(dist / radius, 0.0f, 1.0f);

    float angleDeg = std::atan2(dy, dx) * ColorScience::RAD_TO_DEG;
    if (angleDeg < 0.0f) {
        angleDeg += 360.0f;
    }

    m_hue = ColorScience::normalizeHueDeg(angleDeg);

    if (m_mode == Mode::OkLCH) {
        m_chroma = normR * ColorScience::MAX_CHROMA;
        emit oklchChanged(m_lightness, m_chroma, m_hue);
    } else {
        m_saturation = normR;
        emit hsvChanged(m_hue, m_saturation, m_value);
    }

    emit colorInteracted();
    update();
}

void OklchColorWheel::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        setFocus(Qt::MouseFocusReason);
        m_dragging = true;
        handleMousePosition(event->position());
    }
}

void OklchColorWheel::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        handleMousePosition(event->position());
    }
}

void OklchColorWheel::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
    }
}

void OklchColorWheel::keyPressEvent(QKeyEvent *event)
{
    const bool fine = (event->modifiers() & Qt::ShiftModifier) != 0;
    const float hueStep = fine ? 1.0f : 5.0f;
    const float chromaStep = fine ? 0.002f : 0.01f;
    const float unitStep = fine ? 0.005f : 0.02f;
    const float lumStep = fine ? 0.01f : 0.05f;

    bool changed = false;
    switch (event->key()) {
    case Qt::Key_Left:
        m_hue = ColorScience::normalizeHueDeg(m_hue - hueStep);
        changed = true;
        break;
    case Qt::Key_Right:
        m_hue = ColorScience::normalizeHueDeg(m_hue + hueStep);
        changed = true;
        break;
    case Qt::Key_Up:
        if (m_mode == Mode::OkLCH) {
            m_chroma = std::clamp(m_chroma + chromaStep, 0.0f, ColorScience::MAX_CHROMA);
        } else {
            m_saturation = std::clamp(m_saturation + unitStep, 0.0f, 1.0f);
        }
        changed = true;
        break;
    case Qt::Key_Down:
        if (m_mode == Mode::OkLCH) {
            m_chroma = std::clamp(m_chroma - chromaStep, 0.0f, ColorScience::MAX_CHROMA);
        } else {
            m_saturation = std::clamp(m_saturation - unitStep, 0.0f, 1.0f);
        }
        changed = true;
        break;
    case Qt::Key_PageUp:
        if (m_mode == Mode::OkLCH) {
            setLightness(m_lightness + lumStep);
        } else {
            setValue(m_value + lumStep);
        }
        changed = true;
        break;
    case Qt::Key_PageDown:
        if (m_mode == Mode::OkLCH) {
            setLightness(m_lightness - lumStep);
        } else {
            setValue(m_value - lumStep);
        }
        changed = true;
        break;
    case Qt::Key_Home:
        if (m_mode == Mode::OkLCH) {
            m_chroma = 0.0f;
        } else {
            m_saturation = 0.0f;
        }
        changed = true;
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }

    if (changed) {
        if (m_mode == Mode::OkLCH) {
            emit oklchChanged(m_lightness, m_chroma, m_hue);
        } else {
            emit hsvChanged(m_hue, m_saturation, m_value);
        }
        emit colorInteracted();
        update();
        event->accept();
    }
}

// =============================================================================
// OklchColorDialog Implementation
// =============================================================================

OklchColorDialog::OklchColorDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("OkLCH Perceptual Color Picker"));
    setMinimumSize(680, 520);
    setStyleSheet(QStringLiteral(
        "OklchColorDialog { background-color: #12141a; color: #f1f5f9; font-size: 12px; }"
        "OklchColorDialog QLabel { color: #f1f5f9; font-size: 12px; }"
        "OklchColorDialog QGroupBox { color: #94a3b8; font-size: 11px; font-weight: 600; "
        "border: 1px solid #262b36; border-radius: 6px; margin-top: 10px; padding-top: 8px; }"
        "OklchColorDialog QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; "
        "}"
        "OklchColorDialog QRadioButton { color: #f1f5f9; font-size: 12px; spacing: 6px; }"
        "OklchColorDialog QRadioButton:focus, OklchColorDialog QPushButton:focus, "
        "OklchColorDialog QToolButton:focus, OklchColorDialog QLineEdit:focus, "
        "OklchColorDialog QSpinBox:focus, OklchColorDialog QDoubleSpinBox:focus, "
        "OklchColorDialog QSlider:focus { border: 2px solid #38bdf8; outline: none; }"));
    setupUi();
    updateAllControls(EditSource::External);
}

OklchColorDialog::OklchColorDialog(const QColor &initial, QWidget *parent)
    : OklchColorDialog(parent)
{
    setCurrentColor(initial);
    m_initialOklch = m_currentOklch;
    updatePreviewSwatches();
}

void OklchColorDialog::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(12);

    auto *modeBar = new QHBoxLayout();
    auto *modeLabel = new QLabel(tr("<b>Color Model:</b>"), this);
    m_radioOklch = new QRadioButton(tr("OkLCH"), this);
    m_radioOklch->setMinimumHeight(28);
    m_radioHsv = new QRadioButton(tr("HSV"), this);
    m_radioHsv->setMinimumHeight(28);
    m_radioOklch->setChecked(true);

    auto *modeGroup = new QButtonGroup(this);
    modeGroup->addButton(m_radioOklch, 0);
    modeGroup->addButton(m_radioHsv, 1);
    connect(m_radioOklch, &QRadioButton::toggled, this, &OklchColorDialog::onModeToggled);

    modeBar->addWidget(modeLabel);
    modeBar->addWidget(m_radioOklch);
    modeBar->addWidget(m_radioHsv);
    modeBar->addStretch();

    m_gamutWarningWidget = new QWidget(this);
    auto *gwLayout = new QHBoxLayout(m_gamutWarningWidget);
    gwLayout->setContentsMargins(0, 0, 0, 0);
    auto *warnLabel = new QLabel(tr("<i>Out of sRGB gamut</i>"), m_gamutWarningWidget);
    warnLabel->setStyleSheet(QStringLiteral("color: #f59e0b; font-weight: bold; font-size: 11px;"));
    auto *btnGamutMap = new QPushButton(tr("Fit to Gamut"), m_gamutWarningWidget);
    btnGamutMap->setMinimumHeight(28);
    btnGamutMap->setStyleSheet(QStringLiteral(
        "QPushButton { background: #20242e; color: #f1f5f9; border: 1px solid #2c3240; "
        "padding: 4px 10px; border-radius: 4px; font-size: 11px; } "
        "QPushButton:focus { border: 2px solid #38bdf8; }"));
    connect(btnGamutMap, &QPushButton::clicked, this, &OklchColorDialog::onGamutMapClicked);
    gwLayout->addWidget(warnLabel);
    gwLayout->addWidget(btnGamutMap);
    m_gamutWarningWidget->setVisible(false);
    modeBar->addWidget(m_gamutWarningWidget);

    mainLayout->addLayout(modeBar);

    auto *centerLayout = new QHBoxLayout();
    centerLayout->setSpacing(16);

    auto *leftCol = new QVBoxLayout();
    leftCol->setSpacing(10);

    m_colorWheel = new OklchColorWheel(this);
    connect(m_colorWheel, &OklchColorWheel::oklchChanged, this,
            &OklchColorDialog::onWheelOklchChanged);
    connect(m_colorWheel, &OklchColorWheel::hsvChanged, this, &OklchColorDialog::onWheelHsvChanged);
    leftCol->addWidget(m_colorWheel, 1);

    auto *layoutL = new QHBoxLayout();
    auto *lblL = new QLabel(tr("Lightness (L):"), this);
    lblL->setMinimumWidth(95);
    m_sliderL = new QSlider(Qt::Horizontal, this);
    m_sliderL->setAccessibleName(tr("Lightness slider"));
    m_sliderL->setMinimumHeight(28);
    m_sliderL->setRange(0, 1000);
    m_sliderL->setValue(700);
    m_spinL = new QDoubleSpinBox(this);
    m_spinL->setAccessibleName(tr("Lightness value"));
    m_spinL->setMinimumHeight(28);
    m_spinL->setRange(0.0, 1.0);
    m_spinL->setSingleStep(0.01);
    m_spinL->setDecimals(3);
    m_spinL->setValue(0.70);
    m_spinL->setMinimumWidth(80);
    connect(m_sliderL, &QSlider::valueChanged, this, &OklchColorDialog::onSliderLightnessChanged);
    connect(m_spinL, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            &OklchColorDialog::onSpinOklchChanged);
    layoutL->addWidget(lblL);
    layoutL->addWidget(m_sliderL);
    layoutL->addWidget(m_spinL);
    leftCol->addLayout(layoutL);

    auto *layoutC = new QHBoxLayout();
    auto *lblC = new QLabel(tr("Chroma (C):"), this);
    lblC->setMinimumWidth(95);
    m_sliderC = new QSlider(Qt::Horizontal, this);
    m_sliderC->setAccessibleName(tr("Chroma slider"));
    m_sliderC->setMinimumHeight(28);
    m_sliderC->setRange(0, 400);
    m_sliderC->setValue(160);
    m_spinC = new QDoubleSpinBox(this);
    m_spinC->setAccessibleName(tr("Chroma value"));
    m_spinC->setMinimumHeight(28);
    m_spinC->setRange(0.0, 0.40);
    m_spinC->setSingleStep(0.005);
    m_spinC->setDecimals(3);
    m_spinC->setValue(0.160);
    m_spinC->setMinimumWidth(80);
    connect(m_sliderC, &QSlider::valueChanged, this, &OklchColorDialog::onSliderChromaChanged);
    connect(m_spinC, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            &OklchColorDialog::onSpinOklchChanged);
    layoutC->addWidget(lblC);
    layoutC->addWidget(m_sliderC);
    layoutC->addWidget(m_spinC);
    leftCol->addLayout(layoutC);

    auto *layoutH = new QHBoxLayout();
    auto *lblH = new QLabel(tr("Hue (deg):"), this);
    lblH->setMinimumWidth(95);
    m_sliderH = new QSlider(Qt::Horizontal, this);
    m_sliderH->setAccessibleName(tr("Hue slider"));
    m_sliderH->setMinimumHeight(28);
    m_sliderH->setRange(0, 3600);
    m_sliderH->setValue(2200);
    m_spinH = new QDoubleSpinBox(this);
    m_spinH->setAccessibleName(tr("Hue value"));
    m_spinH->setMinimumHeight(28);
    m_spinH->setRange(0.0, 360.0);
    m_spinH->setSingleStep(1.0);
    m_spinH->setDecimals(1);
    m_spinH->setValue(220.0);
    m_spinH->setMinimumWidth(80);
    connect(m_sliderH, &QSlider::valueChanged, this, &OklchColorDialog::onSliderHueChanged);
    connect(m_spinH, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            &OklchColorDialog::onSpinOklchChanged);
    layoutH->addWidget(lblH);
    layoutH->addWidget(m_sliderH);
    layoutH->addWidget(m_spinH);
    leftCol->addLayout(layoutH);

    auto *layoutA = new QHBoxLayout();
    auto *lblA = new QLabel(tr("Opacity (A):"), this);
    lblA->setMinimumWidth(95);
    m_sliderA = new QSlider(Qt::Horizontal, this);
    m_sliderA->setAccessibleName(tr("Opacity slider"));
    m_sliderA->setMinimumHeight(28);
    m_sliderA->setRange(0, 1000);
    m_sliderA->setValue(1000);
    m_spinA = new QDoubleSpinBox(this);
    m_spinA->setAccessibleName(tr("Opacity value"));
    m_spinA->setMinimumHeight(28);
    m_spinA->setRange(0.0, 1.0);
    m_spinA->setSingleStep(0.01);
    m_spinA->setDecimals(2);
    m_spinA->setValue(1.0);
    m_spinA->setMinimumWidth(80);
    connect(m_sliderA, &QSlider::valueChanged, this, &OklchColorDialog::onSliderAlphaChanged);
    connect(m_spinA, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            &OklchColorDialog::onSpinOklchChanged);
    layoutA->addWidget(lblA);
    layoutA->addWidget(m_sliderA);
    layoutA->addWidget(m_spinA);
    leftCol->addLayout(layoutA);

    centerLayout->addLayout(leftCol, 5);

    auto *rightCol = new QVBoxLayout();
    rightCol->setSpacing(10);

    auto *swatchGroup = new QGroupBox(tr("Color Comparison"), this);
    auto *swatchLayout = new QHBoxLayout(swatchGroup);
    m_swatchCurrent = new QWidget(swatchGroup);
    m_swatchCurrent->setObjectName(QStringLiteral("oklchSwatchCurrent"));
    m_swatchCurrent->setAccessibleName(tr("Current color swatch"));
    m_swatchCurrent->setAttribute(Qt::WA_StyledBackground, true);
    m_swatchCurrent->setMinimumHeight(48);
    m_swatchCurrent->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_swatchInitial = new QWidget(swatchGroup);
    m_swatchInitial->setObjectName(QStringLiteral("oklchSwatchInitial"));
    m_swatchInitial->setAccessibleName(tr("Initial color swatch"));
    m_swatchInitial->setAccessibleDescription(tr("Activate to restore initial color"));
    m_swatchInitial->setFocusPolicy(Qt::StrongFocus);
    m_swatchInitial->setAttribute(Qt::WA_StyledBackground, true);
    m_swatchInitial->setMinimumHeight(48);
    m_swatchInitial->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_swatchInitial->setToolTip(tr("Click or press Enter/Space to restore initial color"));
    m_swatchInitial->setCursor(Qt::PointingHandCursor);
    m_swatchInitial->installEventFilter(this);

    auto *btnRestore = new QToolButton(swatchGroup);
    btnRestore->setText(tr("Reset"));
    btnRestore->setMinimumSize(48, 28);
    btnRestore->setToolTip(tr("Reset to initial color"));
    connect(btnRestore, &QToolButton::clicked, this, &OklchColorDialog::onResetToInitialClicked);

    swatchLayout->addWidget(new QLabel(tr("<b>New:</b>"), swatchGroup));
    swatchLayout->addWidget(m_swatchCurrent, 1);
    swatchLayout->addWidget(new QLabel(tr("<b>Old:</b>"), swatchGroup));
    swatchLayout->addWidget(m_swatchInitial, 1);
    swatchLayout->addWidget(btnRestore);
    rightCol->addWidget(swatchGroup);

    auto *hexLayout = new QHBoxLayout();
    auto *hexLabel = new QLabel(tr("<b>Hex Code:</b>"), this);
    m_hexEdit = new QLineEdit(this);
    m_hexEdit->setAccessibleName(tr("CSS Hex color code"));
    m_hexEdit->setMaxLength(9);
    m_hexEdit->setMinimumSize(116, 28);
    auto *btnCopyHex = new QPushButton(tr("Copy"), this);
    btnCopyHex->setMinimumSize(60, 28);
    connect(m_hexEdit, &QLineEdit::textEdited, this, &OklchColorDialog::onHexEdited);
    connect(btnCopyHex, &QPushButton::clicked, this,
            [this]() { QApplication::clipboard()->setText(m_hexEdit->text()); });
    hexLayout->addWidget(hexLabel);
    hexLayout->addWidget(m_hexEdit);
    hexLayout->addWidget(btnCopyHex);
    hexLayout->addStretch();
    rightCol->addLayout(hexLayout);

    auto *rgbGroup = new QGroupBox(tr("sRGB (0..255 and 0.0..1.0)"), this);
    auto *rgbLayout = new QGridLayout(rgbGroup);
    rgbLayout->setContentsMargins(8, 8, 8, 8);
    rgbLayout->setHorizontalSpacing(8);

    m_spinR = new QSpinBox(rgbGroup);
    m_spinR->setAccessibleName(tr("Red 0 to 255"));
    m_spinR->setMinimumHeight(28);
    m_spinR->setRange(0, 255);
    m_spinG = new QSpinBox(rgbGroup);
    m_spinG->setAccessibleName(tr("Green 0 to 255"));
    m_spinG->setMinimumHeight(28);
    m_spinG->setRange(0, 255);
    m_spinB = new QSpinBox(rgbGroup);
    m_spinB->setAccessibleName(tr("Blue 0 to 255"));
    m_spinB->setMinimumHeight(28);
    m_spinB->setRange(0, 255);

    m_spinRf = new QDoubleSpinBox(rgbGroup);
    m_spinRf->setAccessibleName(tr("Red float 0 to 1"));
    m_spinRf->setMinimumHeight(28);
    m_spinRf->setRange(0.0, 1.0);
    m_spinRf->setSingleStep(0.01);
    m_spinRf->setDecimals(4);
    m_spinGf = new QDoubleSpinBox(rgbGroup);
    m_spinGf->setAccessibleName(tr("Green float 0 to 1"));
    m_spinGf->setMinimumHeight(28);
    m_spinGf->setRange(0.0, 1.0);
    m_spinGf->setSingleStep(0.01);
    m_spinGf->setDecimals(4);
    m_spinBf = new QDoubleSpinBox(rgbGroup);
    m_spinBf->setAccessibleName(tr("Blue float 0 to 1"));
    m_spinBf->setMinimumHeight(28);
    m_spinBf->setRange(0.0, 1.0);
    m_spinBf->setSingleStep(0.01);
    m_spinBf->setDecimals(4);

    connect(m_spinR, qOverload<int>(&QSpinBox::valueChanged), this,
            &OklchColorDialog::onSpinRgbChanged);
    connect(m_spinG, qOverload<int>(&QSpinBox::valueChanged), this,
            &OklchColorDialog::onSpinRgbChanged);
    connect(m_spinB, qOverload<int>(&QSpinBox::valueChanged), this,
            &OklchColorDialog::onSpinRgbChanged);

    connect(m_spinRf, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            &OklchColorDialog::onSpinRgbFloatChanged);
    connect(m_spinGf, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            &OklchColorDialog::onSpinRgbFloatChanged);
    connect(m_spinBf, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            &OklchColorDialog::onSpinRgbFloatChanged);

    rgbLayout->addWidget(new QLabel(QStringLiteral("R:"), rgbGroup), 0, 0);
    rgbLayout->addWidget(m_spinR, 0, 1);
    rgbLayout->addWidget(m_spinRf, 0, 2);

    rgbLayout->addWidget(new QLabel(QStringLiteral("G:"), rgbGroup), 1, 0);
    rgbLayout->addWidget(m_spinG, 1, 1);
    rgbLayout->addWidget(m_spinGf, 1, 2);

    rgbLayout->addWidget(new QLabel(QStringLiteral("B:"), rgbGroup), 2, 0);
    rgbLayout->addWidget(m_spinB, 2, 1);
    rgbLayout->addWidget(m_spinBf, 2, 2);

    rightCol->addWidget(rgbGroup);

    auto *hsvGroup = new QGroupBox(tr("HSV (Standard)"), this);
    auto *hsvLayout = new QGridLayout(hsvGroup);
    hsvLayout->setContentsMargins(8, 8, 8, 8);
    m_spinHsvH = new QDoubleSpinBox(hsvGroup);
    m_spinHsvH->setAccessibleName(tr("HSV Hue"));
    m_spinHsvH->setMinimumHeight(28);
    m_spinHsvH->setRange(0.0, 360.0);
    m_spinHsvH->setDecimals(1);
    m_spinHsvS = new QDoubleSpinBox(hsvGroup);
    m_spinHsvS->setAccessibleName(tr("HSV Saturation percent"));
    m_spinHsvS->setMinimumHeight(28);
    m_spinHsvS->setRange(0.0, 100.0);
    m_spinHsvS->setDecimals(1);
    m_spinHsvS->setSuffix(QStringLiteral("%"));
    m_spinHsvV = new QDoubleSpinBox(hsvGroup);
    m_spinHsvV->setAccessibleName(tr("HSV Value percent"));
    m_spinHsvV->setMinimumHeight(28);
    m_spinHsvV->setRange(0.0, 100.0);
    m_spinHsvV->setDecimals(1);
    m_spinHsvV->setSuffix(QStringLiteral("%"));

    connect(m_spinHsvH, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            &OklchColorDialog::onSpinHsvChanged);
    connect(m_spinHsvS, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            &OklchColorDialog::onSpinHsvChanged);
    connect(m_spinHsvV, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            &OklchColorDialog::onSpinHsvChanged);

    hsvLayout->addWidget(new QLabel(QStringLiteral("H:"), hsvGroup), 0, 0);
    hsvLayout->addWidget(m_spinHsvH, 0, 1);
    hsvLayout->addWidget(new QLabel(QStringLiteral("S:"), hsvGroup), 0, 2);
    hsvLayout->addWidget(m_spinHsvS, 0, 3);
    hsvLayout->addWidget(new QLabel(QStringLiteral("V:"), hsvGroup), 0, 4);
    hsvLayout->addWidget(m_spinHsvV, 0, 5);
    rightCol->addWidget(hsvGroup);

    auto *palGroup = new QGroupBox(tr("Quick Swatches"), this);
    auto *palGrid = new QGridLayout(palGroup);
    palGrid->setContentsMargins(6, 6, 6, 6);
    palGrid->setSpacing(4);

    const QColor quickColors[16] = {
        QColor("#e78284"), QColor("#ea999c"), QColor("#ef9f76"), QColor("#e5c890"),
        QColor("#a6d189"), QColor("#81c8be"), QColor("#99d1db"), QColor("#85c1dc"),
        QColor("#8caaee"), QColor("#babbf1"), QColor("#ca9ee6"), QColor("#f4b8e4"),
        QColor("#ffffff"), QColor("#a5adce"), QColor("#414559"), QColor("#000000")};

    for (int i = 0; i < 16; ++i) {
        const QColor c = quickColors[i];
        const QString hexUpper = c.name(QColor::HexRgb).toUpper();
        auto *btn = new QPushButton(palGroup);
        btn->setFixedSize(28, 28);
        btn->setAccessibleName(tr("Quick swatch %1").arg(hexUpper));
        btn->setToolTip(tr("Select quick color %1").arg(hexUpper));
        btn->setStyleSheet(QStringLiteral("QPushButton { background-color: %1; border: 1px solid "
                                          "#51576d; border-radius: 4px; } "
                                          "QPushButton:focus { border: 2px solid #38bdf8; }")
                               .arg(c.name()));
        btn->setCursor(Qt::PointingHandCursor);
        connect(btn, &QPushButton::clicked, this, [this, c]() { onQuickColorClicked(c); });
        palGrid->addWidget(btn, i / 8, i % 8);
    }
    rightCol->addWidget(palGroup);

    centerLayout->addLayout(rightCol, 4);
    mainLayout->addLayout(centerLayout, 1);

    auto *btnBox = new QHBoxLayout();
    btnBox->setContentsMargins(0, 8, 0, 0);

    auto *btnReset = new QPushButton(tr("Reset"), this);
    btnReset->setMinimumHeight(28);
    connect(btnReset, &QPushButton::clicked, this, &OklchColorDialog::onResetToInitialClicked);

    auto *btnCancel = new QPushButton(tr("Cancel"), this);
    btnCancel->setMinimumHeight(28);
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);

    auto *btnOk = new QPushButton(tr("OK"), this);
    btnOk->setMinimumHeight(28);
    btnOk->setDefault(true);
    btnOk->setStyleSheet(
        QStringLiteral("QPushButton { background: #8caaee; color: #0b0e14; font-weight: bold; "
                       "padding: 6px 18px; border-radius: 5px; font-size: 12px; } "
                       "QPushButton:focus { border: 2px solid #38bdf8; }"));
    connect(btnOk, &QPushButton::clicked, this, &OklchColorDialog::accept);

    btnBox->addWidget(btnReset);
    btnBox->addStretch();
    btnBox->addWidget(btnCancel);
    btnBox->addWidget(btnOk);
    mainLayout->addLayout(btnBox);
}

QColor OklchColorDialog::currentColor() const noexcept
{
    return ColorScience::oklchToQColor(m_currentOklch);
}

void OklchColorDialog::setCurrentColor(const QColor &color)
{
    m_currentOklch = ColorScience::qcolorToOklch(color);
    updateAllControls(EditSource::External);
}

void OklchColorDialog::setOklch(float l, float c, float h, float a)
{
    m_currentOklch.l = ColorScience::sanitizeClamped(l, 0.0f, 1.0f, 0.0f);
    m_currentOklch.c = ColorScience::sanitizeClamped(c, 0.0f, ColorScience::MAX_CHROMA, 0.0f);
    m_currentOklch.h = ColorScience::normalizeHueDeg(h);
    m_currentOklch.a = ColorScience::sanitizeClamped(a, 0.0f, 1.0f, 1.0f);
    updateAllControls(EditSource::External);
}

void OklchColorDialog::getOklch(float &out_l, float &out_c, float &out_h,
                                float *out_a) const noexcept
{
    out_l = m_currentOklch.l;
    out_c = m_currentOklch.c;
    out_h = m_currentOklch.h;
    if (out_a) {
        *out_a = m_currentOklch.a;
    }
}

void OklchColorDialog::getLinearRgb(float &out_r, float &out_g, float &out_b) const noexcept
{
    ColorScience::oklchToLinearRgbGamutMapped(m_currentOklch.l, m_currentOklch.c, m_currentOklch.h,
                                              out_r, out_g, out_b);
}

QColor OklchColorDialog::getColor(const QColor &initial, QWidget *parent, const QString &title)
{
    OklchColorDialog dlg(initial, parent);
    if (!title.isEmpty()) {
        dlg.setWindowTitle(title);
    }
    if (dlg.exec() == QDialog::Accepted) {
        return dlg.currentColor();
    }
    return QColor();
}

QPointer<OklchColorDialog>
OklchColorDialog::openNonModal(const QColor &initial, QWidget *parent, const QString &title,
                               QObject *context, std::function<void(const QColor &)> onAccepted,
                               std::function<void()> onRejected)
{
    auto *dlg = new OklchColorDialog(initial, parent);
    if (!title.isEmpty()) {
        dlg->setWindowTitle(title);
    }
    dlg->setAttribute(Qt::WA_DeleteOnClose, true);
    dlg->setWindowModality(Qt::NonModal);
    dlg->setModal(false);

    QPointer<OklchColorDialog> dlgPtr(dlg);
    QObject *bindCtx = context ? context : dlg;
    if (onAccepted) {
        connect(dlg, &OklchColorDialog::colorSelected, bindCtx, std::move(onAccepted));
    }
    if (onRejected) {
        connect(dlg, &QDialog::rejected, bindCtx, std::move(onRejected));
    }
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
    return dlgPtr;
}

void OklchColorDialog::accept()
{
    emit colorSelected(currentColor());
    QDialog::accept();
}

bool OklchColorDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_swatchInitial && event) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                onResetToInitialClicked();
                return true;
            }
        } else if (event->type() == QEvent::KeyPress) {
            auto *ke = static_cast<QKeyEvent *>(event);
            if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter ||
                ke->key() == Qt::Key_Space) {
                onResetToInitialClicked();
                return true;
            }
        } else if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut) {
            updatePreviewSwatches();
        }
    }
    return QDialog::eventFilter(watched, event);
}

void OklchColorDialog::updatePreviewSwatches()
{
    if (m_swatchCurrent) {
        const QColor cur = currentColor();
        m_swatchCurrent->setStyleSheet(
            QStringLiteral("background-color: %1; border: 1px solid #51576d; border-radius: 6px;")
                .arg(cur.name(QColor::HexArgb)));
    }
    if (m_swatchInitial) {
        const QColor initCol = ColorScience::oklchToQColor(m_initialOklch);
        const QString border = m_swatchInitial->hasFocus() ? QStringLiteral("2px solid #38bdf8")
                                                           : QStringLiteral("1px solid #51576d");
        m_swatchInitial->setStyleSheet(
            QStringLiteral("background-color: %1; border: %2; border-radius: 6px;")
                .arg(initCol.name(QColor::HexArgb), border));
    }
}

void OklchColorDialog::updateAllControls(EditSource source)
{
    if (m_updating) {
        return;
    }

    {
        const QScopedValueRollback<bool> guard(m_updating, true);

        const bool inGamut =
            ColorScience::isOklchInGamut(m_currentOklch.l, m_currentOklch.c, m_currentOklch.h);
        if (m_gamutWarningWidget) {
            m_gamutWarningWidget->setVisible(!inGamut);
        }

        float srgbR = 0.0f, srgbG = 0.0f, srgbB = 0.0f;
        ColorScience::oklchToSrgbClamped(m_currentOklch.l, m_currentOklch.c, m_currentOklch.h,
                                         srgbR, srgbG, srgbB);

        if (source != EditSource::SpinHsv) {
            float hHsv = 0.0f, sHsv = 0.0f, vHsv = 0.0f;
            ColorScience::srgbToHsv(srgbR, srgbG, srgbB, hHsv, sHsv, vHsv);
            if (sHsv > 1e-4f && vHsv > 1e-4f) {
                m_hsvHueCache = hHsv;
            } else if (source == EditSource::External || source == EditSource::SpinRgb8 ||
                       source == EditSource::SpinRgbFloat || source == EditSource::Hex) {
                m_hsvHueCache = m_currentOklch.h;
            }
            m_hsvSatCache = sHsv;
            m_hsvValCache = vHsv;
        }

        if (source != EditSource::Wheel && m_colorWheel) {
            m_colorWheel->setLightness(m_currentOklch.l);
            m_colorWheel->setChroma(m_currentOklch.c);
            m_colorWheel->setHue((m_colorWheel->mode() == OklchColorWheel::Mode::HSV)
                                     ? m_hsvHueCache
                                     : m_currentOklch.h);
            m_colorWheel->setSaturation(m_hsvSatCache);
            m_colorWheel->setValue(m_hsvValCache);
        }

        if (source != EditSource::Slider) {
            m_sliderL->setValue(static_cast<int>(std::lround(m_currentOklch.l * 1000.0f)));
            m_sliderC->setValue(static_cast<int>(std::lround(m_currentOklch.c * 1000.0f)));
            m_sliderH->setValue(static_cast<int>(std::lround(m_currentOklch.h * 10.0f)));
            m_sliderA->setValue(static_cast<int>(std::lround(m_currentOklch.a * 1000.0f)));
        }

        if (source != EditSource::SpinOklch) {
            m_spinL->setValue(m_currentOklch.l);
            m_spinC->setValue(m_currentOklch.c);
            m_spinH->setValue(m_currentOklch.h);
            m_spinA->setValue(m_currentOklch.a);
        }

        if (source != EditSource::SpinRgb8) {
            const int ir = std::clamp(static_cast<int>(std::lround(srgbR * 255.0f)), 0, 255);
            const int ig = std::clamp(static_cast<int>(std::lround(srgbG * 255.0f)), 0, 255);
            const int ib = std::clamp(static_cast<int>(std::lround(srgbB * 255.0f)), 0, 255);
            m_spinR->setValue(ir);
            m_spinG->setValue(ig);
            m_spinB->setValue(ib);
        }

        if (source != EditSource::SpinRgbFloat) {
            m_spinRf->setValue(srgbR);
            m_spinGf->setValue(srgbG);
            m_spinBf->setValue(srgbB);
        }

        if (source != EditSource::SpinHsv) {
            m_spinHsvH->setValue(m_hsvHueCache);
            m_spinHsvS->setValue(m_hsvSatCache * 100.0f);
            m_spinHsvV->setValue(m_hsvValCache * 100.0f);
        }

        if (source != EditSource::Hex && m_hexEdit) {
            const QColor col = currentColor();
            if (col.alpha() < 255 || m_currentOklch.a < 0.999f) {
                m_hexEdit->setText(QString::asprintf("#%02X%02X%02X%02X", col.red(), col.green(),
                                                     col.blue(), col.alpha()));
            } else {
                m_hexEdit->setText(
                    QString::asprintf("#%02X%02X%02X", col.red(), col.green(), col.blue()));
            }
        }

        updatePreviewSwatches();
    }

    emit colorChanged(currentColor());
    emit oklchChanged(m_currentOklch.l, m_currentOklch.c, m_currentOklch.h);
}

void OklchColorDialog::onWheelOklchChanged(float l, float c, float h)
{
    if (m_updating) {
        return;
    }
    m_currentOklch.l = ColorScience::sanitizeClamped(l, 0.0f, 1.0f, m_currentOklch.l);
    m_currentOklch.c =
        ColorScience::sanitizeClamped(c, 0.0f, ColorScience::MAX_CHROMA, m_currentOklch.c);
    m_currentOklch.h = ColorScience::normalizeHueDeg(h);
    updateAllControls(EditSource::Wheel);
}

void OklchColorDialog::onWheelHsvChanged(float h, float s, float v)
{
    if (m_updating) {
        return;
    }
    const float normH = ColorScience::normalizeHueDeg(h);
    const float clampS = ColorScience::sanitizeClamped(s, 0.0f, 1.0f, 0.0f);
    const float clampV = ColorScience::sanitizeClamped(v, 0.0f, 1.0f, 0.0f);
    m_hsvHueCache = normH;
    m_hsvSatCache = clampS;
    m_hsvValCache = clampV;

    float r = 0.0f, g = 0.0f, b = 0.0f;
    ColorScience::hsvToSrgb(normH, clampS, clampV, r, g, b);
    m_currentOklch = ColorScience::srgbToOklch(r, g, b, m_currentOklch.a);
    if (clampS <= 1e-4f || clampV <= 1e-4f) {
        m_currentOklch.h = normH;
    }
    updateAllControls(EditSource::Wheel);
}

void OklchColorDialog::onSliderLightnessChanged(int val)
{
    if (m_updating) {
        return;
    }
    m_currentOklch.l = std::clamp(static_cast<float>(val) / 1000.0f, 0.0f, 1.0f);
    updateAllControls(EditSource::Slider);
}

void OklchColorDialog::onSliderChromaChanged(int val)
{
    if (m_updating) {
        return;
    }
    m_currentOklch.c =
        std::clamp(static_cast<float>(val) / 1000.0f, 0.0f, ColorScience::MAX_CHROMA);
    updateAllControls(EditSource::Slider);
}

void OklchColorDialog::onSliderHueChanged(int val)
{
    if (m_updating) {
        return;
    }
    m_currentOklch.h = ColorScience::normalizeHueDeg(static_cast<float>(val) / 10.0f);
    updateAllControls(EditSource::Slider);
}

void OklchColorDialog::onSliderAlphaChanged(int val)
{
    if (m_updating) {
        return;
    }
    m_currentOklch.a = std::clamp(static_cast<float>(val) / 1000.0f, 0.0f, 1.0f);
    updateAllControls(EditSource::Slider);
}

void OklchColorDialog::onSpinOklchChanged()
{
    if (m_updating) {
        return;
    }
    m_currentOklch.l = ColorScience::sanitizeClamped(static_cast<float>(m_spinL->value()), 0.0f,
                                                     1.0f, m_currentOklch.l);
    m_currentOklch.c = ColorScience::sanitizeClamped(static_cast<float>(m_spinC->value()), 0.0f,
                                                     ColorScience::MAX_CHROMA, m_currentOklch.c);
    m_currentOklch.h = ColorScience::normalizeHueDeg(static_cast<float>(m_spinH->value()));
    m_currentOklch.a = ColorScience::sanitizeClamped(static_cast<float>(m_spinA->value()), 0.0f,
                                                     1.0f, m_currentOklch.a);
    updateAllControls(EditSource::SpinOklch);
}

void OklchColorDialog::onSpinRgbChanged()
{
    if (m_updating) {
        return;
    }
    const float r = static_cast<float>(m_spinR->value()) / 255.0f;
    const float g = static_cast<float>(m_spinG->value()) / 255.0f;
    const float b = static_cast<float>(m_spinB->value()) / 255.0f;
    m_currentOklch = ColorScience::srgbToOklch(r, g, b, m_currentOklch.a);
    updateAllControls(EditSource::SpinRgb8);
}

void OklchColorDialog::onSpinRgbFloatChanged()
{
    if (m_updating) {
        return;
    }
    const float r =
        ColorScience::sanitizeClamped(static_cast<float>(m_spinRf->value()), 0.0f, 1.0f, 0.0f);
    const float g =
        ColorScience::sanitizeClamped(static_cast<float>(m_spinGf->value()), 0.0f, 1.0f, 0.0f);
    const float b =
        ColorScience::sanitizeClamped(static_cast<float>(m_spinBf->value()), 0.0f, 1.0f, 0.0f);
    m_currentOklch = ColorScience::srgbToOklch(r, g, b, m_currentOklch.a);
    updateAllControls(EditSource::SpinRgbFloat);
}

void OklchColorDialog::onSpinHsvChanged()
{
    if (m_updating) {
        return;
    }
    const float h = ColorScience::normalizeHueDeg(static_cast<float>(m_spinHsvH->value()));
    const float s = ColorScience::sanitizeClamped(static_cast<float>(m_spinHsvS->value()) / 100.0f,
                                                  0.0f, 1.0f, 0.0f);
    const float v = ColorScience::sanitizeClamped(static_cast<float>(m_spinHsvV->value()) / 100.0f,
                                                  0.0f, 1.0f, 0.0f);
    m_hsvHueCache = h;
    m_hsvSatCache = s;
    m_hsvValCache = v;

    float r = 0.0f, g = 0.0f, b = 0.0f;
    ColorScience::hsvToSrgb(h, s, v, r, g, b);
    m_currentOklch = ColorScience::srgbToOklch(r, g, b, m_currentOklch.a);
    if (s <= 1e-4f || v <= 1e-4f) {
        m_currentOklch.h = h;
    }
    updateAllControls(EditSource::SpinHsv);
}

void OklchColorDialog::onHexEdited(const QString &text)
{
    if (m_updating) {
        return;
    }
    int r = 0, g = 0, b = 0;
    float alpha = m_currentOklch.a;
    if (parseCssHexColor(text, m_currentOklch.a, r, g, b, alpha)) {
        m_currentOklch = ColorScience::srgbToOklch(static_cast<float>(r) / 255.0f,
                                                   static_cast<float>(g) / 255.0f,
                                                   static_cast<float>(b) / 255.0f, alpha);
        updateAllControls(EditSource::Hex);
    }
}

void OklchColorDialog::onModeToggled(bool oklchMode)
{
    if (m_colorWheel) {
        m_colorWheel->setMode(oklchMode ? OklchColorWheel::Mode::OkLCH
                                        : OklchColorWheel::Mode::HSV);
        if (!oklchMode) {
            m_colorWheel->setHsv(m_hsvHueCache, m_hsvSatCache, m_hsvValCache);
        }
    }
}

void OklchColorDialog::onGamutMapClicked()
{
    const float maxC = ColorScience::maxInGamutChroma(m_currentOklch.l, m_currentOklch.h);
    m_currentOklch.c = std::min(m_currentOklch.c, maxC);
    updateAllControls(EditSource::External);
}

void OklchColorDialog::onResetToInitialClicked()
{
    m_currentOklch = m_initialOklch;
    updateAllControls(EditSource::External);
}

void OklchColorDialog::onQuickColorClicked(const QColor &col)
{
    m_currentOklch = ColorScience::qcolorToOklch(col);
    updateAllControls(EditSource::External);
}

} // namespace waywal::studio
