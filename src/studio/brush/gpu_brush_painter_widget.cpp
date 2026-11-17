// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/brush/gpu_brush_painter_widget.hpp"

#include "studio/brush/brush_dab_accum_glsl.hpp"
#include "studio/inspector/oklch_color_dialog.hpp"

#include <QColor>
#include <QFontMetricsF>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace waywal::studio {

namespace {

constexpr uint32_t kTexFmtRgba8888 = 0u;
constexpr uint32_t kTexFmtRg88 = 8u;
constexpr uint32_t kTexFmtR8 = 9u;
constexpr size_t kDabBatchFlushThreshold = 8u;
constexpr float kMinPointerMoveStepPx = 0.35f;
constexpr double kMinPointerMoveDtSec = 0.004;
constexpr float kFlowNeutralNorm = 128.0f / 255.0f;
constexpr float kFlowScaleNorm = 127.0f / 255.0f;

// Reference the embedded compute shader string so unused-header checks stay clean.
static_assert(!kBrushDabAccumCompSource.empty(), "Compute shader source must be non-empty");

float finiteOr(float v, float fallback) noexcept { return std::isfinite(v) ? v : fallback; }

bool isDabFinite(const we_brush_dab_t &d) noexcept
{
    return std::isfinite(d.ax) && std::isfinite(d.ay) && std::isfinite(d.bx) &&
           std::isfinite(d.by) && std::isfinite(d.radius_a) && std::isfinite(d.radius_b) &&
           std::isfinite(d.opacity_eff) && std::isfinite(d.flow_dir_x) &&
           std::isfinite(d.flow_dir_y) && std::isfinite(d.drag_dx_px) &&
           std::isfinite(d.drag_dy_px);
}

we_brush_params_t defaultBrushParams() noexcept
{
    we_brush_params_t p{};
    p.channel = WE_BRUSH_CHAN_MASK_R8;
    p.op_mode = WE_BRUSH_OP_ADD_BUILDUP;
    p.radius_px = 32.0f;
    p.hardness = 0.65f;
    p.falloff_power = 1.50f;
    p.opacity = 0.85f;
    p.target_value = 1.0f;
    p.flow_strength = 0.85f;
    p.blur_radius_px = 8.0f;
    p.pressure_size_weight = 0.50f;
    p.pressure_alpha_weight = 0.70f;
    p.velocity_damp_weight = 0.25f;
    p.color_rgba[0] = 1.0f;
    p.color_rgba[1] = 1.0f;
    p.color_rgba[2] = 1.0f;
    p.color_rgba[3] = 1.0f;
    return p;
}

we_brush_params_t sanitizeBrushParams(const we_brush_params_t &in) noexcept
{
    we_brush_params_t p = in;
    if (p.channel > WE_BRUSH_CHAN_SMUDGE) {
        p.channel = WE_BRUSH_CHAN_MASK_R8;
    }
    if (p.op_mode > WE_BRUSH_OP_SMUDGE_ADV) {
        p.op_mode = WE_BRUSH_OP_ADD_BUILDUP;
    }
    p.radius_px = std::clamp(finiteOr(p.radius_px, 32.0f), 1.0f, 512.0f);
    p.hardness = std::clamp(finiteOr(p.hardness, 0.65f), 0.0f, 0.999f);
    p.falloff_power = std::clamp(finiteOr(p.falloff_power, 1.50f), 0.25f, 4.0f);
    p.opacity = std::clamp(finiteOr(p.opacity, 0.85f), 0.0f, 1.0f);
    p.target_value = std::clamp(finiteOr(p.target_value, 1.0f), 0.0f, 1.0f);
    p.flow_strength = std::clamp(finiteOr(p.flow_strength, 0.85f), 0.0f, 1.0f);
    p.blur_radius_px = std::clamp(finiteOr(p.blur_radius_px, 8.0f), 1.0f, 64.0f);
    p.pressure_size_weight = std::clamp(finiteOr(p.pressure_size_weight, 0.50f), 0.0f, 1.0f);
    p.pressure_alpha_weight = std::clamp(finiteOr(p.pressure_alpha_weight, 0.70f), 0.0f, 1.0f);
    p.velocity_damp_weight = std::clamp(finiteOr(p.velocity_damp_weight, 0.25f), 0.0f, 1.0f);
    for (float &c : p.color_rgba) {
        c = std::clamp(finiteOr(c, 1.0f), 0.0f, 1.0f);
    }
    return p;
}

uint32_t bytesPerPixelForBrushTarget(we_brush_channel_t channel, uint32_t texFormat) noexcept
{
    if (texFormat == kTexFmtR8) {
        return 1u;
    }
    if (texFormat == kTexFmtRg88) {
        return 2u;
    }
    if (texFormat == kTexFmtRgba8888) {
        return 4u;
    }
    if (channel == WE_BRUSH_CHAN_MASK_R8) {
        return 1u;
    }
    if (channel == WE_BRUSH_CHAN_FLOW_RG88) {
        return 2u;
    }
    return 4u;
}

we_brush_op_mode_t defaultOpModeForChannel(we_brush_channel_t channel,
                                           uint32_t currentMode) noexcept
{
    switch (channel) {
    case WE_BRUSH_CHAN_MASK_R8:
    case WE_BRUSH_CHAN_COLOR_RGBA8:
        if (currentMode <= WE_BRUSH_OP_ERASE) {
            return static_cast<we_brush_op_mode_t>(currentMode);
        }
        return WE_BRUSH_OP_ADD_BUILDUP;
    case WE_BRUSH_CHAN_FLOW_RG88:
        if (currentMode >= WE_BRUSH_OP_FLOW_PAINT && currentMode <= WE_BRUSH_OP_FLOW_COMB) {
            return static_cast<we_brush_op_mode_t>(currentMode);
        }
        return WE_BRUSH_OP_FLOW_PAINT;
    case WE_BRUSH_CHAN_BLUR:
        return WE_BRUSH_OP_BLUR_MASKED;
    case WE_BRUSH_CHAN_SMUDGE:
        return WE_BRUSH_OP_SMUDGE_ADV;
    }
    return WE_BRUSH_OP_ADD_BUILDUP;
}

float evalFalloffWeight(float normDist, float hardness, float falloffPower, float opacity) noexcept
{
    const float inner = std::clamp(hardness, 0.0f, 0.999f);
    const float edgeT = std::clamp((normDist - inner) / std::max(1.0f - inner, 1e-3f), 0.0f, 1.0f);
    const float mask = 1.0f - edgeT * edgeT * (3.0f - 2.0f * edgeT);
    const float expPower = std::clamp(falloffPower, 0.25f, 4.0f);
    const float shaped =
        (mask > 0.0f) ? ((expPower == 1.0f) ? mask : std::pow(mask, expPower)) : 0.0f;
    return std::clamp(shaped * opacity, 0.0f, 1.0f);
}

inline float unevenCapsuleSdfScalar(float px, float py, float ax, float ay, float bx, float by,
                                    float ra, float rb, float baX, float baY, float l2, float rr,
                                    float a2, float il2, bool degenerate) noexcept
{
    const float paX = px - ax;
    const float paY = py - ay;
    if (degenerate) {
        if (ra >= rb) {
            return std::sqrt(paX * paX + paY * paY) - ra;
        }
        const float pbX = px - bx;
        const float pbY = py - by;
        return std::sqrt(pbX * pbX + pbY * pbY) - rb;
    }
    const float y = paX * baX + paY * baY;
    const float z = y - l2;
    const float dX = paX * l2 - baX * y;
    const float dY = paY * l2 - baY * y;
    const float x2 = dX * dX + dY * dY;
    const float y2 = y * y * l2;
    const float z2 = z * z * l2;
    const float signRr = (rr > 0.0f) ? 1.0f : ((rr < 0.0f) ? -1.0f : 0.0f);
    const float k = signRr * rr * rr * x2;
    const float signZ = (z > 0.0f) ? 1.0f : ((z < 0.0f) ? -1.0f : 0.0f);
    const float signY = (y > 0.0f) ? 1.0f : ((y < 0.0f) ? -1.0f : 0.0f);
    if (signZ * a2 * z2 > k) {
        return std::sqrt(x2 + z2) * il2 - rb;
    }
    if (signY * a2 * y2 < k) {
        return std::sqrt(x2 + y2) * il2 - ra;
    }
    return (std::sqrt(x2 * a2 * il2) + y * rr) * il2 - ra;
}

inline float evalDabWeightFromDist(float dist, float feather, float falloffPower,
                                   float opacity) noexcept
{
    if (dist >= 0.0f) {
        return 0.0f;
    }
    if (dist <= -feather) {
        return opacity;
    }
    const float t = std::clamp((dist + feather) / feather, 0.0f, 1.0f);
    const float mask = 1.0f - t * t * (3.0f - 2.0f * t);
    if (mask <= 0.0f) {
        return 0.0f;
    }
    const float shaped = (falloffPower == 1.0f) ? mask : std::pow(mask, falloffPower);
    return std::clamp(shaped * opacity, 0.0f, 1.0f);
}

inline uint8_t normToU8(float v) noexcept
{
    return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

void blendScalarPixel(uint8_t *pxPtr, uint32_t bpp, uint32_t channel, uint32_t opMode, float w,
                      float targetVal, const float colorRgba[4], float srcFlowX,
                      float srcFlowY) noexcept
{
    if (channel == WE_BRUSH_CHAN_FLOW_RG88 && bpp >= 2u) {
        const float dx =
            (static_cast<float>(pxPtr[0]) / 255.0f - kFlowNeutralNorm) / kFlowScaleNorm;
        const float dy =
            (static_cast<float>(pxPtr[1]) / 255.0f - kFlowNeutralNorm) / kFlowScaleNorm;
        float rx = dx;
        float ry = dy;
        if (opMode == WE_BRUSH_OP_FLOW_ERASE) {
            rx = dx * (1.0f - w);
            ry = dy * (1.0f - w);
        } else if (opMode == WE_BRUSH_OP_FLOW_COMB) {
            rx = dx + srcFlowX * w;
            ry = dy + srcFlowY * w;
            const float len = std::sqrt(rx * rx + ry * ry);
            if (len > 1.0f) {
                rx /= len;
                ry /= len;
            }
        } else {
            rx = dx + (srcFlowX - dx) * w;
            ry = dy + (srcFlowY - dy) * w;
        }
        pxPtr[0] = normToU8(kFlowNeutralNorm + rx * kFlowScaleNorm);
        pxPtr[1] = normToU8(kFlowNeutralNorm + ry * kFlowScaleNorm);
        if (bpp == 4u) {
            pxPtr[3] = 255u;
        }
        return;
    }

    if (channel == WE_BRUSH_CHAN_MASK_R8 || bpp == 1u) {
        const float d = static_cast<float>(pxPtr[0]) / 255.0f;
        float res = d;
        if (opMode == WE_BRUSH_OP_MAX_CAP) {
            res = std::max(d, targetVal * w);
        } else if (opMode == WE_BRUSH_OP_SUBTRACT) {
            res = std::clamp(d - w, 0.0f, 1.0f);
        } else if (opMode == WE_BRUSH_OP_ERASE) {
            res = d * (1.0f - w);
        } else {
            res = d + (targetVal - d) * w;
        }
        pxPtr[0] = normToU8(res);
        return;
    }

    if (bpp == 4u) {
        const float dr = static_cast<float>(pxPtr[0]) / 255.0f;
        const float dg = static_cast<float>(pxPtr[1]) / 255.0f;
        const float db = static_cast<float>(pxPtr[2]) / 255.0f;
        const float da = static_cast<float>(pxPtr[3]) / 255.0f;
        const float ca = std::clamp(w * colorRgba[3], 0.0f, 1.0f);

        if (opMode == WE_BRUSH_OP_ERASE) {
            pxPtr[3] = normToU8(da * (1.0f - w));
        } else if (opMode == WE_BRUSH_OP_MAX_CAP) {
            pxPtr[0] = normToU8(std::max(dr, colorRgba[0] * ca));
            pxPtr[1] = normToU8(std::max(dg, colorRgba[1] * ca));
            pxPtr[2] = normToU8(std::max(db, colorRgba[2] * ca));
            pxPtr[3] = normToU8(std::max(da, ca));
        } else if (opMode == WE_BRUSH_OP_SUBTRACT) {
            pxPtr[0] = normToU8(dr - colorRgba[0] * w);
            pxPtr[1] = normToU8(dg - colorRgba[1] * w);
            pxPtr[2] = normToU8(db - colorRgba[2] * w);
            pxPtr[3] = normToU8(da - colorRgba[3] * w);
        } else {
            const float outA = std::clamp(da + ca * (1.0f - da), 0.0f, 1.0f);
            if (outA > 1e-6f) {
                const float invA = 1.0f / outA;
                const float keep = da * (1.0f - ca);
                pxPtr[0] = normToU8((dr * keep + colorRgba[0] * ca) * invA);
                pxPtr[1] = normToU8((dg * keep + colorRgba[1] * ca) * invA);
                pxPtr[2] = normToU8((db * keep + colorRgba[2] * ca) * invA);
            } else {
                pxPtr[0] = 0u;
                pxPtr[1] = 0u;
                pxPtr[2] = 0u;
            }
            pxPtr[3] = normToU8(outA);
        }
    }
}

#if defined(__x86_64__) || defined(_M_X64)
bool cpuSupportsAvx2() noexcept
{
    static const bool supported = []() noexcept {
        return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
    }();
    return supported;
}

[[gnu::target("avx2,fma")]] void
paintCapsuleRgba8Avx2(uint8_t *tileBytes, int32_t lx0, int32_t ly0, int32_t lx1, int32_t ly1,
                      float ax, float ay, float bx, float by, float ra, float rb, float baX,
                      float baY, float l2, float rr, float a2, float il2, bool degenerate,
                      float feather, float falloffPower, float opacity, uint32_t opMode,
                      const float colorRgba[4]) noexcept
{
    const __m256 vAx = _mm256_set1_ps(ax);
    const __m256 vAy = _mm256_set1_ps(ay);
    const __m256 vBx = _mm256_set1_ps(bx);
    const __m256 vBy = _mm256_set1_ps(by);
    const __m256 vBaX = _mm256_set1_ps(baX);
    const __m256 vBaY = _mm256_set1_ps(baY);
    const __m256 vL2 = _mm256_set1_ps(l2);
    const __m256 vIl2 = _mm256_set1_ps(il2);
    const __m256 vRr = _mm256_set1_ps(rr);
    const __m256 vA2 = _mm256_set1_ps(a2);
    const __m256 vRa = _mm256_set1_ps(ra);
    const __m256 vRb = _mm256_set1_ps(rb);
    const float signRr = (rr > 0.0f) ? 1.0f : ((rr < 0.0f) ? -1.0f : 0.0f);
    const __m256 vSignRr2 = _mm256_set1_ps(signRr * rr * rr);
    const __m256 vFeather = _mm256_set1_ps(feather);
    const __m256 vInvFeather = _mm256_set1_ps(1.0f / feather);
    const __m256 vOpacity = _mm256_set1_ps(opacity);
    const __m256 vZero = _mm256_setzero_ps();
    const __m256 vOne = _mm256_set1_ps(1.0f);
    const __m256 vTwo = _mm256_set1_ps(2.0f);
    const __m256 vThree = _mm256_set1_ps(3.0f);
    const __m256 v255 = _mm256_set1_ps(255.0f);
    const __m256 vInv255 = _mm256_set1_ps(1.0f / 255.0f);
    const __m256 vHalf = _mm256_set1_ps(0.5f);
    const __m256 vLaneOffsets = _mm256_set_ps(7.5f, 6.5f, 5.5f, 4.5f, 3.5f, 2.5f, 1.5f, 0.5f);
    const __m256 vSignMask = _mm256_set1_ps(-0.0f);

    const bool solidAlpha1 =
        (opMode == WE_BRUSH_OP_ADD_BUILDUP && falloffPower == 1.0f && colorRgba[3] >= 1.0f - 1e-5f);
    const uint32_t packedSolid = static_cast<uint32_t>(normToU8(colorRgba[0])) |
                                 (static_cast<uint32_t>(normToU8(colorRgba[1])) << 8u) |
                                 (static_cast<uint32_t>(normToU8(colorRgba[2])) << 16u) |
                                 (uint32_t{255u} << 24u);

    alignas(32) float wBuf[8];
    for (int32_t ly = ly0; ly <= ly1; ++ly) {
        const float py = static_cast<float>(ly) + 0.5f;
        const __m256 vPy = _mm256_set1_ps(py);
        const __m256 vPaY = _mm256_sub_ps(vPy, vAy);
        const __m256 vPbY = _mm256_sub_ps(vPy, vBy);

        int32_t lx = lx0;
        uint8_t *rowPtr = tileBytes + static_cast<size_t>(ly) * WE_BRUSH_TILE_SIZE * 4u;

        for (; lx + 7 <= lx1; lx += 8) {
            const __m256 vPx = _mm256_add_ps(_mm256_set1_ps(static_cast<float>(lx)), vLaneOffsets);
            const __m256 vPaX = _mm256_sub_ps(vPx, vAx);

            __m256 vDist;
            if (degenerate) {
                if (ra >= rb) {
                    const __m256 d2 = _mm256_fmadd_ps(vPaX, vPaX, _mm256_mul_ps(vPaY, vPaY));
                    vDist = _mm256_sub_ps(_mm256_sqrt_ps(d2), vRa);
                } else {
                    const __m256 vPbX = _mm256_sub_ps(vPx, vBx);
                    const __m256 d2 = _mm256_fmadd_ps(vPbX, vPbX, _mm256_mul_ps(vPbY, vPbY));
                    vDist = _mm256_sub_ps(_mm256_sqrt_ps(d2), vRb);
                }
            } else {
                const __m256 y = _mm256_fmadd_ps(vPaX, vBaX, _mm256_mul_ps(vPaY, vBaY));
                const __m256 z = _mm256_sub_ps(y, vL2);
                const __m256 dX = _mm256_fmsub_ps(vPaX, vL2, _mm256_mul_ps(vBaX, y));
                const __m256 dY = _mm256_fmsub_ps(vPaY, vL2, _mm256_mul_ps(vBaY, y));
                const __m256 x2 = _mm256_fmadd_ps(dX, dX, _mm256_mul_ps(dY, dY));
                const __m256 y2 = _mm256_mul_ps(_mm256_mul_ps(y, y), vL2);
                const __m256 z2 = _mm256_mul_ps(_mm256_mul_ps(z, z), vL2);
                const __m256 k = _mm256_mul_ps(vSignRr2, x2);

                const __m256 sgnZ = _mm256_and_ps(_mm256_or_ps(_mm256_and_ps(z, vSignMask), vOne),
                                                  _mm256_cmp_ps(z, vZero, _CMP_NEQ_OQ));
                const __m256 sgnY = _mm256_and_ps(_mm256_or_ps(_mm256_and_ps(y, vSignMask), vOne),
                                                  _mm256_cmp_ps(y, vZero, _CMP_NEQ_OQ));

                const __m256 condZ =
                    _mm256_cmp_ps(_mm256_mul_ps(_mm256_mul_ps(sgnZ, vA2), z2), k, _CMP_GT_OQ);
                const __m256 condY =
                    _mm256_cmp_ps(_mm256_mul_ps(_mm256_mul_ps(sgnY, vA2), y2), k, _CMP_LT_OQ);

                const __m256 distZ =
                    _mm256_fmsub_ps(_mm256_sqrt_ps(_mm256_add_ps(x2, z2)), vIl2, vRb);
                const __m256 distY =
                    _mm256_fmsub_ps(_mm256_sqrt_ps(_mm256_add_ps(x2, y2)), vIl2, vRa);
                const __m256 coreTerm = _mm256_sqrt_ps(_mm256_mul_ps(_mm256_mul_ps(x2, vA2), vIl2));
                const __m256 distMid =
                    _mm256_fmsub_ps(_mm256_fmadd_ps(y, vRr, coreTerm), vIl2, vRa);

                vDist = _mm256_blendv_ps(_mm256_blendv_ps(distMid, distY, condY), distZ, condZ);
            }

            const __m256 insideMask = _mm256_cmp_ps(vDist, vZero, _CMP_LT_OQ);
            if (_mm256_testz_ps(insideMask, insideMask)) {
                continue;
            }

            const __m256 t = _mm256_min_ps(
                vOne,
                _mm256_max_ps(vZero, _mm256_mul_ps(_mm256_add_ps(vDist, vFeather), vInvFeather)));
            const __m256 smooth =
                _mm256_mul_ps(_mm256_mul_ps(t, t), _mm256_fnmadd_ps(vTwo, t, vThree));
            __m256 vMask = _mm256_sub_ps(vOne, smooth);
            vMask = _mm256_and_ps(vMask, insideMask);

            if (falloffPower == 1.0f) {
                const __m256 vW = _mm256_mul_ps(vMask, vOpacity);
                _mm256_store_ps(wBuf, vW);
            } else {
                _mm256_store_ps(wBuf, vMask);
                for (int i = 0; i < 8; ++i) {
                    if (wBuf[i] > 0.0f && wBuf[i] < 1.0f) {
                        wBuf[i] = std::clamp(std::pow(wBuf[i], falloffPower) * opacity, 0.0f, 1.0f);
                    } else if (wBuf[i] >= 1.0f) {
                        wBuf[i] = opacity;
                    } else {
                        wBuf[i] = 0.0f;
                    }
                }
            }

            if (solidAlpha1 && opacity >= 1.0f - 1e-5f) {
                const __m256 coreMask =
                    _mm256_cmp_ps(vDist, _mm256_sub_ps(vZero, vFeather), _CMP_LE_OQ);
                if (_mm256_movemask_ps(coreMask) == 0xFF) {
                    uint32_t *p32 =
                        reinterpret_cast<uint32_t *>(rowPtr + static_cast<size_t>(lx) * 4u);
                    for (int i = 0; i < 8; ++i) {
                        p32[i] = packedSolid;
                    }
                    continue;
                }
            }

            for (int i = 0; i < 8; ++i) {
                const float w = wBuf[i];
                if (w > 1e-5f) {
                    blendScalarPixel(rowPtr + static_cast<size_t>(lx + i) * 4u, 4u,
                                     WE_BRUSH_CHAN_COLOR_RGBA8, opMode, w, 1.0f, colorRgba, 0.0f,
                                     0.0f);
                }
            }
            (void)v255;
            (void)vInv255;
            (void)vHalf;
        }

        for (; lx <= lx1; ++lx) {
            const float px = static_cast<float>(lx) + 0.5f;
            const float dist = unevenCapsuleSdfScalar(px, py, ax, ay, bx, by, ra, rb, baX, baY, l2,
                                                      rr, a2, il2, degenerate);
            const float w = evalDabWeightFromDist(dist, feather, falloffPower, opacity);
            if (w > 1e-5f) {
                blendScalarPixel(rowPtr + static_cast<size_t>(lx) * 4u, 4u,
                                 WE_BRUSH_CHAN_COLOR_RGBA8, opMode, w, 1.0f, colorRgba, 0.0f, 0.0f);
            }
        }
    }
}
#endif

QColor brushColorFromParams(const we_brush_params_t &p)
{
    QColor c;
    c.setRgbF(p.color_rgba[0], p.color_rgba[1], p.color_rgba[2], p.color_rgba[3]);
    return c;
}

/* Renders live brush radial falloff and pressure curves using the dark authoring palette. */
class BrushProfilePreviewWidget final : public QWidget {
  public:
    explicit BrushProfilePreviewWidget(const GpuBrushPainterWidget *owner,
                                       QWidget *parent = nullptr)
        : QWidget(parent), m_owner(owner)
    {
        setMinimumHeight(88);
        setMaximumHeight(104);
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(QStringLiteral("Brush Falloff and Pressure Curve Preview"));
        setAccessibleDescription(
            QStringLiteral("Displays radial hardness falloff and stylus pressure response curves"));
    }

  protected:
    void paintEvent(QPaintEvent *event) override
    {
        (void)event;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QRectF r = rect().adjusted(1.0, 1.0, -1.0, -1.0);
        p.setPen(
            QPen(hasFocus() ? QColor(QStringLiteral("#38bdf8")) : QColor(QStringLiteral("#2a2f3d")),
                 hasFocus() ? 2.0 : 1.0));
        p.setBrush(QColor(QStringLiteral("#0b0c10")));
        p.drawRoundedRect(r, 4.0, 4.0);

        QFont legendFont(QStringLiteral("JetBrains Mono"));
        legendFont.setPixelSize(11);
        p.setFont(legendFont);
        const QFontMetricsF fm(legendFont);
        const double legendHeight = fm.height() + 4.0;

        const QRectF plot = r.adjusted(8.0, 8.0, -8.0, -(legendHeight + 6.0));
        if (plot.width() <= 4.0 || plot.height() <= 4.0) {
            return;
        }

        p.setPen(QPen(QColor(QStringLiteral("#181b22")), 1.0));
        for (int i = 1; i < 4; ++i) {
            const double gx = plot.left() + plot.width() * (0.25 * i);
            const double gy = plot.top() + plot.height() * (0.25 * i);
            p.drawLine(QPointF(gx, plot.top()), QPointF(gx, plot.bottom()));
            p.drawLine(QPointF(plot.left(), gy), QPointF(plot.right(), gy));
        }

        const we_brush_params_t params = m_owner ? m_owner->brushParams() : defaultBrushParams();

        QPainterPath falloffPath;
        QPainterPath pressureSizePath;
        QPainterPath pressureAlphaPath;

        constexpr int kSteps = 48;
        for (int i = 0; i <= kSteps; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(kSteps);
            const double x = plot.left() + plot.width() * u;

            const float fw =
                evalFalloffWeight(u, params.hardness, params.falloff_power, params.opacity);
            const double fy = plot.bottom() - plot.height() * fw;

            const float pSize =
                (1.0f - params.pressure_size_weight) + params.pressure_size_weight * u;
            const double psy = plot.bottom() - plot.height() * pSize;

            const float pAlpha =
                (1.0f - params.pressure_alpha_weight) + params.pressure_alpha_weight * u;
            const double pay = plot.bottom() - plot.height() * pAlpha;

            if (i == 0) {
                falloffPath.moveTo(x, fy);
                pressureSizePath.moveTo(x, psy);
                pressureAlphaPath.moveTo(x, pay);
            } else {
                falloffPath.lineTo(x, fy);
                pressureSizePath.lineTo(x, psy);
                pressureAlphaPath.lineTo(x, pay);
            }
        }

        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(QStringLiteral("#22c55e")), 1.3, Qt::DashLine));
        p.drawPath(pressureSizePath);

        p.setPen(QPen(QColor(QStringLiteral("#f59e0b")), 1.3, Qt::DotLine));
        p.drawPath(pressureAlphaPath);

        p.setPen(QPen(QColor(QStringLiteral("#38bdf8")), 2.0));
        p.drawPath(falloffPath);

        const QRectF legendRect(plot.left(), plot.bottom() + 3.0, plot.width(), legendHeight);
        const QString fullLegend =
            QStringLiteral("Falloff (Sky)  P->Size (Green)  P->Alpha (Amber)");
        const QString elidedLegend = fm.elidedText(fullLegend, Qt::ElideRight, plot.width());
        p.setPen(QColor(QStringLiteral("#94a3b8")));
        p.drawText(legendRect, Qt::AlignLeft | Qt::AlignVCenter, elidedLegend);
    }

  private:
    const GpuBrushPainterWidget *m_owner{nullptr};
};

QGroupBox *createStyledGroup(const QString &title, QWidget *parent)
{
    auto *group = new QGroupBox(title, parent);
    group->setObjectName(QStringLiteral("brushSectionGroup"));
    return group;
}

} // namespace

GpuBrushPainterWidget::GpuBrushPainterWidget(QWidget *parent)
    : QWidget(parent), m_params(defaultBrushParams())
{
    // Defer QWidget child hierarchy and styling until showEvent() so startup RssAnon stays lean.
}

void GpuBrushPainterWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    ensureUiInitialized();
}

void GpuBrushPainterWidget::setIpcClient(IpcClient *client)
{
    if (m_ipcClient == client) {
        return;
    }
    if (m_readbackConn) {
        disconnect(m_readbackConn);
        m_readbackConn = QMetaObject::Connection{};
    }
    m_ipcClient = client;
    if (m_ipcClient) {
        m_readbackConn = connect(m_ipcClient, &IpcClient::textureReadbackReceived, this,
                                 &GpuBrushPainterWidget::onTextureReadbackReceived);
        requestTargetReadback();
    }
}

const std::vector<uint8_t> *GpuBrushPainterWidget::findHostShadowTile(uint16_t tx,
                                                                      uint16_t ty) const noexcept
{
    const auto it = m_hostShadowTiles.find(DirtyTileCoord{tx, ty});
    if (it == m_hostShadowTiles.end()) {
        return nullptr;
    }
    return &it->second.bytes;
}

void GpuBrushPainterWidget::upsertHostShadowTile(const DirtyTileCoord &coord,
                                                 std::vector<uint8_t> bytes)
{
    bytes.shrink_to_fit();
    const size_t newBytes = bytes.capacity();
    auto it = m_hostShadowTiles.find(coord);
    if (it != m_hostShadowTiles.end()) {
        const size_t oldBytes = it->second.bytes.capacity();
        m_hostShadowBytes = (oldBytes <= m_hostShadowBytes) ? (m_hostShadowBytes - oldBytes) : 0u;
        it->second.bytes = std::move(bytes);
        m_hostShadowBytes += newBytes;
        m_shadowLru.splice(m_shadowLru.begin(), m_shadowLru, it->second.lruIt);
    } else {
        m_shadowLru.push_front(coord);
        ShadowTileEntry entry{std::move(bytes), m_shadowLru.begin()};
        m_hostShadowTiles.emplace(coord, std::move(entry));
        m_hostShadowBytes += newBytes;
    }
    evictShadowTilesIfOverBudget();
}

void GpuBrushPainterWidget::evictShadowTilesIfOverBudget()
{
    const size_t readbackBytes =
        static_cast<size_t>(std::max<qsizetype>(0, m_lastReadbackPixels.size()));
    const size_t undoBytes = m_tileUndoStack.totalHostBytes();
    const size_t totalBudget = DirtyTileUndoStack::kMaxHostCacheBytes;
    const size_t maxAllowedShadow =
        (undoBytes + readbackBytes < totalBudget)
            ? std::min(kMaxShadowCacheBytes, totalBudget - undoBytes - readbackBytes)
            : 0u;

    if (m_hostShadowBytes <= maxAllowedShadow) {
        return;
    }

    auto lruIt = m_shadowLru.end();
    while (m_hostShadowBytes > maxAllowedShadow && lruIt != m_shadowLru.begin()) {
        --lruIt;
        const DirtyTileCoord coord = *lruIt;
        if (m_strokeActive && m_activeStrokeTileSet.find(coord) != m_activeStrokeTileSet.end()) {
            continue;
        }
        const auto mapIt = m_hostShadowTiles.find(coord);
        if (mapIt != m_hostShadowTiles.end()) {
            const size_t freed = mapIt->second.bytes.capacity();
            m_hostShadowBytes = (freed <= m_hostShadowBytes) ? (m_hostShadowBytes - freed) : 0u;
            m_hostShadowTiles.erase(mapIt);
        }
        lruIt = m_shadowLru.erase(lruIt);
    }
}

void GpuBrushPainterWidget::invalidateHostShadowCache()
{
    if (m_strokeActive) {
        m_tileUndoStack.abortTransaction();
    }
    m_strokeActive = false;
    m_activeStrokeId = 0u;
    m_pendingDabs.clear();
    m_activeStrokeTileCount = 0u;
    m_activeStrokeTiles.clear();
    m_activeStrokeTileSet.clear();
    m_hostShadowTiles.clear();
    m_shadowLru.clear();
    m_hostShadowBytes = 0u;
    m_lastReadbackPixels.clear();
    m_lastReadbackPixels.squeeze();
    m_lastReadbackWidth = 0u;
    m_lastReadbackHeight = 0u;
    m_lastReadbackBpp = 0u;
    m_inflightReadbacks.clear();
    m_tileUndoStack.clear();
}

void GpuBrushPainterWidget::requestTargetReadback(uint64_t strokeId,
                                                  std::vector<DirtyTileCoord> tiles)
{
    if (!m_ipcClient || !m_ipcClient->isConnected() || m_targetObjectId < 0) {
        return;
    }
    PendingReadbackRequest req{};
    req.strokeId = strokeId;
    req.targetGeneration = m_targetGeneration;
    req.objectId = m_targetObjectId;
    req.passIndex = m_targetPassIndex;
    req.texSlot = m_targetTexSlot;
    req.tiles = std::move(tiles);
    if (m_ipcClient->sendReadbackTexture(m_targetOutput, m_targetObjectId, m_targetPassIndex,
                                         m_targetTexSlot, 0u)) {
        m_inflightReadbacks.push_back(std::move(req));
    }
}

void GpuBrushPainterWidget::setTargetTextureSlot(int32_t objectId, uint32_t passIndex,
                                                 uint32_t texSlot, uint32_t width, uint32_t height,
                                                 uint32_t texFormat)
{
    const uint32_t clampedW = std::clamp(width, 1u, 16384u);
    const uint32_t clampedH = std::clamp(height, 1u, 16384u);
    const bool targetChanged = (m_targetObjectId != objectId) || (m_targetPassIndex != passIndex) ||
                               (m_targetTexSlot != texSlot) || (m_canvasWidth != clampedW) ||
                               (m_canvasHeight != clampedH) || (m_targetTexFormat != texFormat);

    m_targetObjectId = objectId;
    m_targetPassIndex = passIndex;
    m_targetTexSlot = texSlot;
    m_canvasWidth = clampedW;
    m_canvasHeight = clampedH;
    m_targetTexFormat = texFormat;

    if (targetChanged) {
        ++m_targetGeneration;
        invalidateHostShadowCache();
    }

    if (texFormat == kTexFmtR8) {
        m_params.channel = WE_BRUSH_CHAN_MASK_R8;
        m_params.op_mode = defaultOpModeForChannel(WE_BRUSH_CHAN_MASK_R8, m_params.op_mode);
    } else if (texFormat == kTexFmtRg88) {
        m_params.channel = WE_BRUSH_CHAN_FLOW_RG88;
        m_params.op_mode = defaultOpModeForChannel(WE_BRUSH_CHAN_FLOW_RG88, m_params.op_mode);
    } else if (texFormat == kTexFmtRgba8888 && (m_params.channel == WE_BRUSH_CHAN_MASK_R8 ||
                                                m_params.channel == WE_BRUSH_CHAN_FLOW_RG88)) {
        m_params.channel = WE_BRUSH_CHAN_COLOR_RGBA8;
        m_params.op_mode = defaultOpModeForChannel(WE_BRUSH_CHAN_COLOR_RGBA8, m_params.op_mode);
    }

    if (m_targetObjectId >= 0) {
        sendBrushIpc(WE_BRUSH_IPC_BIND_TARGET, 0u, {});
        if (targetChanged) {
            requestTargetReadback();
        }
    }
    syncUiFromParams();
    emit targetChannelOverlayRequested(static_cast<we_brush_channel_t>(m_params.channel),
                                       m_showChannelOverlay);
    emit brushParamsChanged(m_params);
}

void GpuBrushPainterWidget::setBrushChannel(we_brush_channel_t channel)
{
    if (static_cast<uint32_t>(channel) > WE_BRUSH_CHAN_SMUDGE) {
        return;
    }
    m_params.channel = static_cast<uint32_t>(channel);
    m_params.op_mode = defaultOpModeForChannel(channel, m_params.op_mode);
    if (m_params.op_mode <= WE_BRUSH_OP_SUBTRACT) {
        m_lastMaskOpMode = static_cast<we_brush_op_mode_t>(m_params.op_mode);
    } else if (m_params.op_mode == WE_BRUSH_OP_FLOW_PAINT ||
               m_params.op_mode == WE_BRUSH_OP_FLOW_COMB) {
        m_lastFlowOpMode = static_cast<we_brush_op_mode_t>(m_params.op_mode);
    }
    syncUiFromParams();
    emit targetChannelOverlayRequested(channel, m_showChannelOverlay);
    emit brushParamsChanged(m_params);
}

void GpuBrushPainterWidget::setBrushOpMode(we_brush_op_mode_t mode)
{
    if (static_cast<uint32_t>(mode) > WE_BRUSH_OP_SMUDGE_ADV) {
        return;
    }
    const uint32_t prevChannel = m_params.channel;
    m_params.op_mode = static_cast<uint32_t>(mode);

    if (mode >= WE_BRUSH_OP_FLOW_PAINT && mode <= WE_BRUSH_OP_FLOW_COMB) {
        m_params.channel = WE_BRUSH_CHAN_FLOW_RG88;
        if (mode != WE_BRUSH_OP_FLOW_ERASE) {
            m_lastFlowOpMode = mode;
        }
    } else if (mode == WE_BRUSH_OP_BLUR_MASKED) {
        m_params.channel = WE_BRUSH_CHAN_BLUR;
    } else if (mode == WE_BRUSH_OP_SMUDGE_ADV) {
        m_params.channel = WE_BRUSH_CHAN_SMUDGE;
    } else {
        if (m_params.channel == WE_BRUSH_CHAN_FLOW_RG88 || m_params.channel == WE_BRUSH_CHAN_BLUR ||
            m_params.channel == WE_BRUSH_CHAN_SMUDGE) {
            m_params.channel = (m_targetTexFormat == kTexFmtR8) ? WE_BRUSH_CHAN_MASK_R8
                                                                : WE_BRUSH_CHAN_COLOR_RGBA8;
        }
        if (mode != WE_BRUSH_OP_ERASE) {
            m_lastMaskOpMode = mode;
        }
    }

    syncUiFromParams();
    if (m_params.channel != prevChannel) {
        emit targetChannelOverlayRequested(static_cast<we_brush_channel_t>(m_params.channel),
                                           m_showChannelOverlay);
    }
    emit brushParamsChanged(m_params);
}

void GpuBrushPainterWidget::setBrushParams(const we_brush_params_t &params)
{
    m_params = sanitizeBrushParams(params);
    if (m_params.op_mode <= WE_BRUSH_OP_SUBTRACT) {
        m_lastMaskOpMode = static_cast<we_brush_op_mode_t>(m_params.op_mode);
    } else if (m_params.op_mode == WE_BRUSH_OP_FLOW_PAINT ||
               m_params.op_mode == WE_BRUSH_OP_FLOW_COMB) {
        m_lastFlowOpMode = static_cast<we_brush_op_mode_t>(m_params.op_mode);
    }
    syncUiFromParams();
    emit brushParamsChanged(m_params);
}

void GpuBrushPainterWidget::adjustRadiusByFactor(float factor)
{
    if (!std::isfinite(factor) || factor <= 0.0f) {
        return;
    }
    m_params.radius_px = std::clamp(m_params.radius_px * factor, 1.0f, 512.0f);
    syncUiFromParams();
    emit brushParamsChanged(m_params);
}

void GpuBrushPainterWidget::setStylusTiltAndRotation(float xTiltDeg, float yTiltDeg,
                                                     float rotationDeg) noexcept
{
    m_stylusTiltXDeg = std::clamp(finiteOr(xTiltDeg, 0.0f), -60.0f, 60.0f);
    m_stylusTiltYDeg = std::clamp(finiteOr(yTiltDeg, 0.0f), -60.0f, 60.0f);
    m_stylusRotationDeg = finiteOr(rotationDeg, 0.0f);
}

void GpuBrushPainterWidget::handlePointerPressWithTilt(const QPointF &texelPos,
                                                       float stylusPressure, double timestampSec,
                                                       float xTiltDeg, float yTiltDeg,
                                                       float rotationDeg)
{
    setStylusTiltAndRotation(xTiltDeg, yTiltDeg, rotationDeg);
    handlePointerPress(texelPos, stylusPressure, timestampSec);
}

void GpuBrushPainterWidget::handlePointerMoveWithTilt(const QPointF &texelPos, float stylusPressure,
                                                      double timestampSec, float xTiltDeg,
                                                      float yTiltDeg, float rotationDeg)
{
    setStylusTiltAndRotation(xTiltDeg, yTiltDeg, rotationDeg);
    handlePointerMove(texelPos, stylusPressure, timestampSec);
}

void GpuBrushPainterWidget::handlePointerReleaseWithTilt(const QPointF &texelPos,
                                                         float stylusPressure, double timestampSec,
                                                         float xTiltDeg, float yTiltDeg,
                                                         float rotationDeg)
{
    setStylusTiltAndRotation(xTiltDeg, yTiltDeg, rotationDeg);
    handlePointerRelease(texelPos, stylusPressure, timestampSec);
}

void GpuBrushPainterWidget::handlePointerPress(const QPointF &texelPos, float stylusPressure,
                                               double timestampSec)
{
    if (!std::isfinite(texelPos.x()) || !std::isfinite(texelPos.y())) {
        return;
    }
    const double safeTs = std::isfinite(timestampSec) ? timestampSec : 0.0;

    if (m_strokeActive) {
        handlePointerRelease(m_lastTexelPos, m_lastPressure, safeTs);
    }

    const float rawP = finiteOr(stylusPressure, 1.0f);
    const float p = std::clamp((rawP > 0.0f) ? rawP : 1.0f, 0.0f, 1.0f);
    m_activeStrokeId = m_nextStrokeId++;
    m_strokeActive = true;
    m_lastTexelPos = texelPos;
    m_lastPressure = p;
    m_lastTimestampSec = safeTs;
    m_emaTangentX = 1.0f;
    m_emaTangentY = 0.0f;
    if (std::hypot(m_stylusTiltXDeg, m_stylusTiltYDeg) > 1e-3f) {
        const float tLen = std::hypot(m_stylusTiltXDeg, m_stylusTiltYDeg);
        m_emaTangentX = m_stylusTiltXDeg / tLen;
        m_emaTangentY = m_stylusTiltYDeg / tLen;
    } else if (std::abs(m_stylusRotationDeg) > 1e-3f) {
        const float rad = m_stylusRotationDeg * (3.14159265358979323846f / 180.0f);
        m_emaTangentX = std::cos(rad);
        m_emaTangentY = std::sin(rad);
    }
    m_pendingDabs.clear();
    m_activeStrokeTiles.clear();
    m_activeStrokeTileSet.clear();
    m_activeStrokeTileCount = 0u;
    m_lastDirtyRect = we_brush_dirty_rect_t{};

    const auto channel = static_cast<we_brush_channel_t>(m_params.channel);
    const uint32_t bpp = bytesPerPixelForBrushTarget(channel, m_targetTexFormat);
    m_tileUndoStack.beginTransaction(m_activeStrokeId, m_targetObjectId, m_targetPassIndex,
                                     m_targetTexSlot, channel, bpp, m_canvasWidth, m_canvasHeight);

    sendBrushIpc(WE_BRUSH_IPC_BEGIN_STROKE, m_activeStrokeId, {});

    const we_brush_dab_t initialDab = buildDabSegment(texelPos, texelPos, p, p, 0.0);
    m_pendingDabs.push_back(initialDab);
    // Flush the initial tap dab immediately so single clicks preview without drag motion.
    flushDabBatch(false);
}

void GpuBrushPainterWidget::handlePointerMove(const QPointF &texelPos, float stylusPressure,
                                              double timestampSec)
{
    if (!m_strokeActive || !std::isfinite(texelPos.x()) || !std::isfinite(texelPos.y())) {
        return;
    }
    const double safeTs = std::isfinite(timestampSec) ? timestampSec : m_lastTimestampSec;
    const float rawP = finiteOr(stylusPressure, m_lastPressure);
    // Preserve prior contact pressure when tablet/mouse transitions emit transient 0.0f pressure.
    const float p = (rawP > 1e-4f) ? std::clamp(rawP, 0.0f, 1.0f) : m_lastPressure;

    const double ds =
        std::hypot(texelPos.x() - m_lastTexelPos.x(), texelPos.y() - m_lastTexelPos.y());
    const double dtSec = std::max(0.0, safeTs - m_lastTimestampSec);
    if (ds < static_cast<double>(kMinPointerMoveStepPx) && dtSec < kMinPointerMoveDtSec &&
        std::fabs(p - m_lastPressure) <= 0.05f) {
        return;
    }

    const we_brush_dab_t dab = buildDabSegment(m_lastTexelPos, texelPos, m_lastPressure, p, dtSec);
    m_pendingDabs.push_back(dab);

    m_lastTexelPos = texelPos;
    m_lastPressure = p;
    m_lastTimestampSec = safeTs;

    if (m_pendingDabs.size() >= kDabBatchFlushThreshold) {
        flushDabBatch(false);
    }
}

void GpuBrushPainterWidget::handlePointerRelease(const QPointF &texelPos, float stylusPressure,
                                                 double timestampSec)
{
    if (!m_strokeActive) {
        return;
    }

    const QPointF safePos =
        (std::isfinite(texelPos.x()) && std::isfinite(texelPos.y())) ? texelPos : m_lastTexelPos;
    const double safeTs = std::isfinite(timestampSec) ? timestampSec : m_lastTimestampSec;
    const float rawP = finiteOr(stylusPressure, m_lastPressure);
    // Mouse release always emits 0.0f pressure; carry m_lastPressure to prevent end-of-stroke
    // pinch.
    const float p = (rawP > 1e-4f) ? std::clamp(rawP, 0.0f, 1.0f) : m_lastPressure;

    const double dx = safePos.x() - m_lastTexelPos.x();
    const double dy = safePos.y() - m_lastTexelPos.y();
    if (std::hypot(dx, dy) > 0.05) {
        const double dtSec = std::max(0.0, safeTs - m_lastTimestampSec);
        m_pendingDabs.push_back(buildDabSegment(m_lastTexelPos, safePos, m_lastPressure, p, dtSec));
    }

    flushDabBatch(true);
    sendBrushIpc(WE_BRUSH_IPC_END_STROKE, m_activeStrokeId, {});
    for (const DirtyTileCoord &coord : m_activeStrokeTiles) {
        const auto it = m_hostShadowTiles.find(coord);
        if (it != m_hostShadowTiles.end() && !it->second.bytes.empty()) {
            m_tileUndoStack.recordPostTileBytes(coord.tx, coord.ty, it->second.bytes.data(),
                                                it->second.bytes.size());
        }
    }
    m_tileUndoStack.commitTransaction();

    const uint64_t committedStrokeId = m_activeStrokeId;
    const auto committedChannel = static_cast<we_brush_channel_t>(m_params.channel);
    const uint32_t touchedTiles = m_activeStrokeTileCount;

    if (m_ipcClient && m_ipcClient->isConnected() && m_targetObjectId >= 0 &&
        !m_activeStrokeTiles.empty()) {
        requestTargetReadback(committedStrokeId, m_activeStrokeTiles);
    }

    m_strokeActive = false;
    m_activeStrokeId = 0u;
    m_activeStrokeTileCount = 0u;
    m_activeStrokeTiles.clear();
    m_activeStrokeTileSet.clear();
    m_stylusTiltXDeg = 0.0f;
    m_stylusTiltYDeg = 0.0f;
    m_stylusRotationDeg = 0.0f;
    evictShadowTilesIfOverBudget();
    updateTelemetryReadout();

    if (m_commandBus) {
        QPointer<GpuBrushPainterWidget> self(this);
        auto cmd = std::make_unique<BrushStrokeCommand>(
            committedStrokeId, committedChannel, touchedTiles, [self](uint64_t sid, bool isRedo) {
                if (!self) {
                    return;
                }
                self->applyHostStrokeHistory(sid, isRedo);
            });
        m_commandBus->execute(std::move(cmd));
    }

    updateTelemetryReadout();
}

void GpuBrushPainterWidget::applyHostStrokeHistory(uint64_t strokeId, bool isRedo)
{
    sendBrushIpc(isRedo ? WE_BRUSH_IPC_REDO_STROKE : WE_BRUSH_IPC_UNDO_STROKE, strokeId, {});

    if (!isRedo && m_tileUndoStack.canUndo()) {
        (void)m_tileUndoStack.undo();
    } else if (isRedo && m_tileUndoStack.canRedo()) {
        (void)m_tileUndoStack.redo();
    }

    const DirtyTileTransaction *tx = m_tileUndoStack.findTransaction(strokeId);
    if (tx) {
        const auto &snapshots = isRedo ? tx->postTiles : tx->preTiles;
        const uint32_t targetBpp = bytesPerPixelForBrushTarget(
            static_cast<we_brush_channel_t>(m_params.channel), m_targetTexFormat);
        const size_t expectedLen = static_cast<size_t>(WE_BRUSH_TILE_SIZE) *
                                   static_cast<size_t>(WE_BRUSH_TILE_SIZE) * targetBpp;

        we_brush_dirty_rect_t dirtyRect{};
        std::vector<uint8_t> decompressed;
        for (const auto &snap : snapshots) {
            if (!DirtyTileUndoStack::decompressTile(snap, decompressed)) {
                continue;
            }
            if (decompressed.size() != expectedLen) {
                const size_t pxCount = static_cast<size_t>(WE_BRUSH_TILE_SIZE) * WE_BRUSH_TILE_SIZE;
                if (decompressed.size() == pxCount * 4u && targetBpp == 1u) {
                    std::vector<uint8_t> conv(pxCount);
                    for (size_t i = 0; i < pxCount; ++i) {
                        conv[i] = decompressed[i * 4u];
                    }
                    decompressed = std::move(conv);
                } else if (decompressed.size() == pxCount * 4u && targetBpp == 2u) {
                    std::vector<uint8_t> conv(pxCount * 2u);
                    for (size_t i = 0; i < pxCount; ++i) {
                        conv[i * 2u] = decompressed[i * 4u];
                        conv[i * 2u + 1u] = decompressed[i * 4u + 1u];
                    }
                    decompressed = std::move(conv);
                } else {
                    continue;
                }
            }
            upsertHostShadowTile(snap.coord, decompressed);

            const int32_t txPx = static_cast<int32_t>(snap.coord.tx) * WE_BRUSH_TILE_SIZE;
            const int32_t tyPx = static_cast<int32_t>(snap.coord.ty) * WE_BRUSH_TILE_SIZE;
            const int32_t tw =
                std::min<int32_t>(WE_BRUSH_TILE_SIZE, static_cast<int32_t>(m_canvasWidth) - txPx);
            const int32_t th =
                std::min<int32_t>(WE_BRUSH_TILE_SIZE, static_cast<int32_t>(m_canvasHeight) - tyPx);
            if (tw > 0 && th > 0) {
                if (dirtyRect.width <= 0 || dirtyRect.height <= 0) {
                    dirtyRect = we_brush_dirty_rect_t{txPx, tyPx, tw, th};
                } else {
                    const int32_t x0 = std::min(dirtyRect.x, txPx);
                    const int32_t y0 = std::min(dirtyRect.y, tyPx);
                    const int32_t x1 = std::max(dirtyRect.x + dirtyRect.width, txPx + tw);
                    const int32_t y1 = std::max(dirtyRect.y + dirtyRect.height, tyPx + th);
                    dirtyRect = we_brush_dirty_rect_t{x0, y0, x1 - x0, y1 - y0};
                }
            }
        }
        if (dirtyRect.width > 0 && dirtyRect.height > 0) {
            m_lastDirtyRect = dirtyRect;
            emit brushStrokeUpdated(dirtyRect);
        }
    }

    if (m_ipcClient && m_ipcClient->isConnected() && m_targetObjectId >= 0) {
        requestTargetReadback();
    }
    updateTelemetryReadout();
    emit statusMessage(isRedo ? tr("Redid brush stroke #%1").arg(strokeId)
                              : tr("Undid brush stroke #%1").arg(strokeId));
}

void GpuBrushPainterWidget::flushDabBatch(bool isFinal)
{
    if (m_pendingDabs.empty()) {
        return;
    }

    we_brush_dirty_rect_t accumRect{};
    size_t offset = 0;
    while (offset < m_pendingDabs.size()) {
        const size_t chunkCount =
            std::min<size_t>(WE_BRUSH_MAX_STROKE_DABS, m_pendingDabs.size() - offset);
        std::vector<we_brush_dab_t> chunk(m_pendingDabs.begin() + static_cast<ptrdiff_t>(offset),
                                          m_pendingDabs.begin() +
                                              static_cast<ptrdiff_t>(offset + chunkCount));

        for (const auto &dab : chunk) {
            touchTilesForDab(dab, accumRect);
        }

        sendBrushIpc(WE_BRUSH_IPC_DAB_BATCH, m_activeStrokeId, chunk);
        offset += chunkCount;
    }

    m_pendingDabs.clear();
    if (accumRect.width > 0 && accumRect.height > 0) {
        m_lastDirtyRect = accumRect;
        emit brushStrokeUpdated(accumRect);
    }

    if (!isFinal) {
        updateTelemetryReadout();
    }
}

we_brush_dab_t GpuBrushPainterWidget::buildDabSegment(const QPointF &a, const QPointF &b,
                                                      float pressureA, float pressureB,
                                                      double dtSec)
{
    const float pA = std::clamp(finiteOr(pressureA, 1.0f), 0.0f, 1.0f);
    const float pB = std::clamp(finiteOr(pressureB, 1.0f), 0.0f, 1.0f);
    const float tiltMagDeg = std::hypot(m_stylusTiltXDeg, m_stylusTiltYDeg);
    const float tiltRad = std::clamp(tiltMagDeg, 0.0f, 60.0f) * (3.14159265358979323846f / 180.0f);
    const float tiltSizeScale = 1.0f + 0.35f * std::sin(tiltRad);

    const float sizeScaleA =
        ((1.0f - m_params.pressure_size_weight) + m_params.pressure_size_weight * pA) *
        tiltSizeScale;
    const float sizeScaleB =
        ((1.0f - m_params.pressure_size_weight) + m_params.pressure_size_weight * pB) *
        tiltSizeScale;

    const float ax = finiteOr(static_cast<float>(a.x()), 0.0f);
    const float ay = finiteOr(static_cast<float>(a.y()), 0.0f);
    const float bx = finiteOr(static_cast<float>(b.x()), 0.0f);
    const float by = finiteOr(static_cast<float>(b.y()), 0.0f);
    const float dx = bx - ax;
    const float dy = by - ay;
    const float ds = std::hypot(dx, dy);

    if (ds > 1e-4f) {
        const float rawTx = dx / ds;
        const float rawTy = dy / ds;
        const float alphaEma = 1.0f - std::exp(-ds / kFlowEmaArcLambdaPx);
        const float nx = m_emaTangentX + alphaEma * (rawTx - m_emaTangentX);
        const float ny = m_emaTangentY + alphaEma * (rawTy - m_emaTangentY);
        const float len = std::hypot(nx, ny);
        if (len > 1e-6f) {
            m_emaTangentX = nx / len;
            m_emaTangentY = ny / len;
        } else {
            m_emaTangentX = rawTx;
            m_emaTangentY = rawTy;
        }
    } else if (tiltMagDeg > 1e-3f) {
        m_emaTangentX = m_stylusTiltXDeg / tiltMagDeg;
        m_emaTangentY = m_stylusTiltYDeg / tiltMagDeg;
    }

    const float pMid = 0.5f * (pA + pB);
    const float alphaScale =
        (1.0f - m_params.pressure_alpha_weight) + m_params.pressure_alpha_weight * pMid;
    const float speedPxPerSec = (dtSec > 1e-5) ? (ds / static_cast<float>(dtSec)) : 0.0f;
    const float velocityFactor =
        1.0f / (1.0f + m_params.velocity_damp_weight * (speedPxPerSec / 1200.0f));

    we_brush_dab_t dab{};
    dab.ax = ax;
    dab.ay = ay;
    dab.bx = bx;
    dab.by = by;
    dab.radius_a = std::clamp(m_params.radius_px * sizeScaleA, 0.5f, 512.0f);
    dab.radius_b = std::clamp(m_params.radius_px * sizeScaleB, 0.5f, 512.0f);
    dab.opacity_eff = std::clamp(m_params.opacity * alphaScale * velocityFactor, 0.0f, 1.0f);
    // Keep flow_dir unscaled and unnegated; daemon and CPU blend apply flow_strength and flags & 1u
    // once.
    dab.flow_dir_x = m_emaTangentX;
    dab.flow_dir_y = m_emaTangentY;
    dab.drag_dx_px = dx;
    dab.drag_dy_px = dy;
    dab.flags = m_invertFlowY ? 1u : 0u;
    return dab;
}

void GpuBrushPainterWidget::sendBrushIpc(we_brush_ipc_action_t action, uint64_t strokeId,
                                         const std::vector<we_brush_dab_t> &dabs)
{
    if (!m_ipcClient || m_targetObjectId < 0) {
        return;
    }

    waywal_ipc_req_brush_stroke_t req{};
    const QByteArray outBa = m_targetOutput.isEmpty() ? QByteArray("*") : m_targetOutput.toUtf8();
    std::strncpy(req.target_output, outBa.constData(), sizeof(req.target_output) - 1u);
    req.action = static_cast<uint32_t>(action);
    req.object_id = m_targetObjectId;
    req.pass_index = m_targetPassIndex;
    req.tex_slot = m_targetTexSlot;
    req.stroke_id = strokeId;
    req.dab_count = static_cast<uint32_t>(
        std::min<size_t>(dabs.size(), static_cast<size_t>(WE_BRUSH_MAX_STROKE_DABS)));
    req.params = m_params;

    m_ipcClient->sendBrushStroke(req, dabs);
}

void GpuBrushPainterWidget::touchTilesForDab(const we_brush_dab_t &dab,
                                             we_brush_dirty_rect_t &accumRect)
{
    if (!isDabFinite(dab)) {
        return;
    }

    const float maxR = std::clamp(std::max(dab.radius_a, dab.radius_b), 0.5f, 512.0f);
    const bool needsBlurHalo =
        (m_params.channel == WE_BRUSH_CHAN_BLUR || m_params.channel == WE_BRUSH_CHAN_SMUDGE);
    const float halo =
        needsBlurHalo ? std::clamp(std::ceil(1.5f * m_params.blur_radius_px), 16.0f, 64.0f) : 2.0f;
    const float pad = maxR + halo;

    const int32_t maxW = static_cast<int32_t>(std::max(1u, m_canvasWidth)) - 1;
    const int32_t maxH = static_cast<int32_t>(std::max(1u, m_canvasHeight)) - 1;
    const float fMaxW = static_cast<float>(maxW);
    const float fMaxH = static_cast<float>(maxH);

    const float rawMinX = std::floor(std::min(dab.ax, dab.bx) - pad);
    const float rawMinY = std::floor(std::min(dab.ay, dab.by) - pad);
    const float rawMaxX = std::ceil(std::max(dab.ax, dab.bx) + pad);
    const float rawMaxY = std::ceil(std::max(dab.ay, dab.by) + pad);
    if (rawMaxX < 0.0f || rawMaxY < 0.0f || rawMinX > fMaxW || rawMinY > fMaxH) {
        return;
    }

    const int32_t minX = static_cast<int32_t>(std::clamp(rawMinX, 0.0f, fMaxW));
    const int32_t minY = static_cast<int32_t>(std::clamp(rawMinY, 0.0f, fMaxH));
    const int32_t maxX = static_cast<int32_t>(std::clamp(rawMaxX, 0.0f, fMaxW));
    const int32_t maxY = static_cast<int32_t>(std::clamp(rawMaxY, 0.0f, fMaxH));
    if (minX > maxX || minY > maxY) {
        return;
    }

    const uint16_t tx0 = static_cast<uint16_t>(minX / static_cast<int32_t>(WE_BRUSH_TILE_SIZE));
    const uint16_t tx1 = static_cast<uint16_t>(maxX / static_cast<int32_t>(WE_BRUSH_TILE_SIZE));
    const uint16_t ty0 = static_cast<uint16_t>(minY / static_cast<int32_t>(WE_BRUSH_TILE_SIZE));
    const uint16_t ty1 = static_cast<uint16_t>(maxY / static_cast<int32_t>(WE_BRUSH_TILE_SIZE));

    const auto channel = static_cast<we_brush_channel_t>(m_params.channel);
    const uint32_t bpp = bytesPerPixelForBrushTarget(channel, m_targetTexFormat);
    const size_t tileByteLen =
        static_cast<size_t>(WE_BRUSH_TILE_SIZE) * static_cast<size_t>(WE_BRUSH_TILE_SIZE) * bpp;

    for (uint16_t ty = ty0; ty <= ty1; ++ty) {
        for (uint16_t tx = tx0; tx <= tx1; ++tx) {
            const DirtyTileCoord coord{tx, ty};
            auto it = m_hostShadowTiles.find(coord);
            if (it == m_hostShadowTiles.end() || it->second.bytes.size() != tileByteLen) {
                std::vector<uint8_t> baseline(tileByteLen, 0u);
                if (m_lastReadbackWidth > 0u && m_lastReadbackHeight > 0u &&
                    m_lastReadbackBpp > 0u &&
                    static_cast<size_t>(m_lastReadbackPixels.size()) >=
                        static_cast<size_t>(m_lastReadbackWidth) * m_lastReadbackHeight *
                            m_lastReadbackBpp) {
                    const uint8_t *srcBase =
                        reinterpret_cast<const uint8_t *>(m_lastReadbackPixels.constData());
                    const uint32_t tileOriginX = static_cast<uint32_t>(tx) * WE_BRUSH_TILE_SIZE;
                    const uint32_t tileOriginY = static_cast<uint32_t>(ty) * WE_BRUSH_TILE_SIZE;
                    const uint32_t copyW =
                        (tileOriginX < m_lastReadbackWidth)
                            ? std::min<uint32_t>(WE_BRUSH_TILE_SIZE,
                                                 m_lastReadbackWidth - tileOriginX)
                            : 0u;
                    const uint32_t copyH =
                        (tileOriginY < m_lastReadbackHeight)
                            ? std::min<uint32_t>(WE_BRUSH_TILE_SIZE,
                                                 m_lastReadbackHeight - tileOriginY)
                            : 0u;
                    for (uint32_t row = 0; row < copyH; ++row) {
                        const size_t srcRowOff =
                            (static_cast<size_t>(tileOriginY + row) * m_lastReadbackWidth +
                             tileOriginX) *
                            m_lastReadbackBpp;
                        const size_t dstRowOff =
                            static_cast<size_t>(row) * WE_BRUSH_TILE_SIZE * bpp;
                        if (m_lastReadbackBpp == bpp) {
                            std::memcpy(baseline.data() + dstRowOff, srcBase + srcRowOff,
                                        static_cast<size_t>(copyW) * bpp);
                        } else {
                            for (uint32_t col = 0; col < copyW; ++col) {
                                const uint8_t *sp = srcBase + srcRowOff + col * m_lastReadbackBpp;
                                uint8_t *dp = baseline.data() + dstRowOff + col * bpp;
                                if (bpp == 1u) {
                                    dp[0] = sp[0];
                                } else if (bpp == 2u) {
                                    dp[0] = sp[0];
                                    dp[1] = (m_lastReadbackBpp >= 2u) ? sp[1] : 128u;
                                } else if (bpp == 4u) {
                                    dp[0] = sp[0];
                                    dp[1] = (m_lastReadbackBpp >= 2u) ? sp[1] : sp[0];
                                    dp[2] = (m_lastReadbackBpp >= 4u) ? sp[2] : sp[0];
                                    dp[3] = (m_lastReadbackBpp >= 4u) ? sp[3] : 255u;
                                }
                            }
                        }
                    }
                } else if (channel == WE_BRUSH_CHAN_FLOW_RG88 && bpp >= 2u) {
                    for (size_t off = 0; off + 1u < tileByteLen; off += bpp) {
                        baseline[off] = WE_BRUSH_FLOW_NEUTRAL_U8;
                        baseline[off + 1u] = WE_BRUSH_FLOW_NEUTRAL_U8;
                        if (bpp == 4u) {
                            baseline[off + 3u] = 255u;
                        }
                    }
                }
                upsertHostShadowTile(coord, std::move(baseline));
                it = m_hostShadowTiles.find(coord);
            } else {
                m_shadowLru.splice(m_shadowLru.begin(), m_shadowLru, it->second.lruIt);
            }

            if (it == m_hostShadowTiles.end()) {
                continue;
            }
            if (m_tileUndoStack.markTileTouched(tx, ty)) {
                ++m_activeStrokeTileCount;
                m_activeStrokeTiles.push_back(coord);
                m_activeStrokeTileSet.insert(coord);
                m_tileUndoStack.recordPreTileBytes(tx, ty, it->second.bytes.data(),
                                                   it->second.bytes.size());
            }
            paintDabIntoTile(dab, tx, ty, bpp, it->second.bytes);
        }
    }

    const int32_t w = maxX - minX + 1;
    const int32_t h = maxY - minY + 1;
    if (accumRect.width <= 0 || accumRect.height <= 0) {
        accumRect = we_brush_dirty_rect_t{minX, minY, w, h};
    } else {
        const int32_t x0 = std::min(accumRect.x, minX);
        const int32_t y0 = std::min(accumRect.y, minY);
        const int32_t x1 = std::max(accumRect.x + accumRect.width, maxX + 1);
        const int32_t y1 = std::max(accumRect.y + accumRect.height, maxY + 1);
        accumRect = we_brush_dirty_rect_t{x0, y0, x1 - x0, y1 - y0};
    }
}

void GpuBrushPainterWidget::onTextureReadbackReceived(
    const waywal_ipc_resp_readback_texture_t &resp, const QByteArray &data)
{
    if (m_inflightReadbacks.empty()) {
        return;
    }
    PendingReadbackRequest pending = std::move(m_inflightReadbacks.front());
    m_inflightReadbacks.pop_front();

    if (pending.targetGeneration != m_targetGeneration || pending.objectId != m_targetObjectId ||
        pending.passIndex != m_targetPassIndex || pending.texSlot != m_targetTexSlot) {
        return;
    }

    if (resp.status_code != 0u || resp.width == 0u || resp.height == 0u || data.isEmpty()) {
        return;
    }
    if (pending.strokeId != 0u && (resp.width != m_canvasWidth || resp.height != m_canvasHeight)) {
        return;
    }

    const uint64_t pixelCount = static_cast<uint64_t>(resp.width) * resp.height;
    if (pixelCount == 0u || static_cast<uint64_t>(data.size()) < pixelCount) {
        return;
    }
    const uint32_t readbackBpp =
        static_cast<uint32_t>(static_cast<uint64_t>(data.size()) / pixelCount);
    if (readbackBpp != 1u && readbackBpp != 2u && readbackBpp != 4u) {
        return;
    }

    const auto channel = static_cast<we_brush_channel_t>(m_params.channel);
    const uint32_t targetBpp = bytesPerPixelForBrushTarget(channel, m_targetTexFormat);

    // Avoid retaining huge (>16 MiB) raw readbacks outside the 64 MiB host cache cap.
    const size_t remainingBudget = (m_tileUndoStack.totalHostBytes() + m_hostShadowBytes <
                                    DirtyTileUndoStack::kMaxHostCacheBytes)
                                       ? (DirtyTileUndoStack::kMaxHostCacheBytes -
                                          m_tileUndoStack.totalHostBytes() - m_hostShadowBytes)
                                       : 0u;
    if (static_cast<size_t>(data.size()) <= kMaxShadowCacheBytes &&
        static_cast<size_t>(data.size()) <= remainingBudget) {
        m_lastReadbackPixels = data;
        m_lastReadbackWidth = resp.width;
        m_lastReadbackHeight = resp.height;
        m_lastReadbackBpp = readbackBpp;
    } else {
        m_lastReadbackPixels.clear();
        m_lastReadbackPixels.squeeze();
        m_lastReadbackWidth = 0u;
        m_lastReadbackHeight = 0u;
        m_lastReadbackBpp = 0u;
    }

    const size_t tileByteLen = static_cast<size_t>(WE_BRUSH_TILE_SIZE) *
                               static_cast<size_t>(WE_BRUSH_TILE_SIZE) * targetBpp;
    const uint8_t *srcBase = reinterpret_cast<const uint8_t *>(data.constData());

    auto extractTile = [&](const DirtyTileCoord &coord, std::vector<uint8_t> &outTile) {
        outTile.assign(tileByteLen, 0u);
        const uint32_t tileOriginX = static_cast<uint32_t>(coord.tx) * WE_BRUSH_TILE_SIZE;
        const uint32_t tileOriginY = static_cast<uint32_t>(coord.ty) * WE_BRUSH_TILE_SIZE;
        if (tileOriginX >= resp.width || tileOriginY >= resp.height) {
            return;
        }
        const uint32_t copyW = std::min<uint32_t>(WE_BRUSH_TILE_SIZE, resp.width - tileOriginX);
        const uint32_t copyH = std::min<uint32_t>(WE_BRUSH_TILE_SIZE, resp.height - tileOriginY);
        for (uint32_t row = 0; row < copyH; ++row) {
            const size_t srcRowOff =
                (static_cast<size_t>(tileOriginY + row) * resp.width + tileOriginX) * readbackBpp;
            const size_t dstRowOff = static_cast<size_t>(row) * WE_BRUSH_TILE_SIZE * targetBpp;
            if (readbackBpp == targetBpp) {
                std::memcpy(outTile.data() + dstRowOff, srcBase + srcRowOff,
                            static_cast<size_t>(copyW) * targetBpp);
            } else {
                for (uint32_t col = 0; col < copyW; ++col) {
                    const uint8_t *sp = srcBase + srcRowOff + col * readbackBpp;
                    uint8_t *dp = outTile.data() + dstRowOff + col * targetBpp;
                    if (targetBpp == 1u) {
                        dp[0] = sp[0];
                    } else if (targetBpp == 2u) {
                        dp[0] = sp[0];
                        dp[1] = (readbackBpp >= 2u) ? sp[1] : 128u;
                    } else {
                        dp[0] = sp[0];
                        dp[1] = (readbackBpp >= 2u) ? sp[1] : sp[0];
                        dp[2] = (readbackBpp >= 4u) ? sp[2] : sp[0];
                        dp[3] = (readbackBpp >= 4u) ? sp[3] : 255u;
                    }
                }
            }
        }
    };

    if (!m_strokeActive) {
        std::vector<DirtyTileCoord> existingCoords;
        existingCoords.reserve(m_hostShadowTiles.size());
        for (const auto &kv : m_hostShadowTiles) {
            existingCoords.push_back(kv.first);
        }
        std::vector<uint8_t> tileBuf;
        for (const DirtyTileCoord &coord : existingCoords) {
            extractTile(coord, tileBuf);
            upsertHostShadowTile(coord, tileBuf);
        }
    }

    if (pending.strokeId != 0u) {
        std::vector<uint8_t> tileBuf;
        for (const DirtyTileCoord &coord : pending.tiles) {
            extractTile(coord, tileBuf);
            if (!m_strokeActive ||
                m_activeStrokeTileSet.find(coord) == m_activeStrokeTileSet.end()) {
                upsertHostShadowTile(coord, tileBuf);
            }
            m_tileUndoStack.recordPostTileBytesForStroke(pending.strokeId, coord.tx, coord.ty,
                                                         tileBuf.data(), tileBuf.size(), resp.width,
                                                         resp.height);
        }
        updateTelemetryReadout();
    }
}

void GpuBrushPainterWidget::paintDabIntoTile(const we_brush_dab_t &dab, uint16_t tx, uint16_t ty,
                                             uint32_t bpp, std::vector<uint8_t> &tileBytes) const
{
    if (!isDabFinite(dab)) {
        return;
    }
    const size_t expectedLen =
        static_cast<size_t>(WE_BRUSH_TILE_SIZE) * static_cast<size_t>(WE_BRUSH_TILE_SIZE) * bpp;
    if (tileBytes.size() != expectedLen || (bpp != 1u && bpp != 2u && bpp != 4u)) {
        return;
    }

    const float ra = std::clamp(dab.radius_a, 0.5f, 512.0f);
    const float rb = std::clamp(dab.radius_b, 0.5f, 512.0f);
    const float maxR = std::max(ra, rb);
    const float opacity = std::clamp(dab.opacity_eff, 0.0f, 1.0f);
    if (opacity <= 1e-4f) {
        return;
    }

    const int32_t tileX0 = static_cast<int32_t>(tx) * static_cast<int32_t>(WE_BRUSH_TILE_SIZE);
    const int32_t tileY0 = static_cast<int32_t>(ty) * static_cast<int32_t>(WE_BRUSH_TILE_SIZE);
    const float fLx0 =
        std::floor(std::min(dab.ax, dab.bx) - maxR - 1.0f) - static_cast<float>(tileX0);
    const float fLy0 =
        std::floor(std::min(dab.ay, dab.by) - maxR - 1.0f) - static_cast<float>(tileY0);
    const float fLx1 =
        std::ceil(std::max(dab.ax, dab.bx) + maxR + 1.0f) - static_cast<float>(tileX0);
    const float fLy1 =
        std::ceil(std::max(dab.ay, dab.by) + maxR + 1.0f) - static_cast<float>(tileY0);

    constexpr float kMaxTileCoord = static_cast<float>(WE_BRUSH_TILE_SIZE - 1u);
    if (fLx1 < 0.0f || fLy1 < 0.0f || fLx0 > kMaxTileCoord || fLy0 > kMaxTileCoord) {
        return;
    }

    const int32_t lx0 = static_cast<int32_t>(std::clamp(fLx0, 0.0f, kMaxTileCoord));
    const int32_t ly0 = static_cast<int32_t>(std::clamp(fLy0, 0.0f, kMaxTileCoord));
    const int32_t lx1 = static_cast<int32_t>(std::clamp(fLx1, 0.0f, kMaxTileCoord));
    const int32_t ly1 = static_cast<int32_t>(std::clamp(fLy1, 0.0f, kMaxTileCoord));
    if (lx0 > lx1 || ly0 > ly1) {
        return;
    }

    const float localAx = dab.ax - static_cast<float>(tileX0);
    const float localAy = dab.ay - static_cast<float>(tileY0);
    const float localBx = dab.bx - static_cast<float>(tileX0);
    const float localBy = dab.by - static_cast<float>(tileY0);
    const float baX = localBx - localAx;
    const float baY = localBy - localAy;
    const float l2 = baX * baX + baY * baY;
    const float rr = ra - rb;
    const bool degenerate = (l2 < 1e-6f || l2 <= rr * rr);
    const float a2 = std::max(l2 - rr * rr, 1e-6f);
    const float il2 = (l2 > 1e-6f) ? (1.0f / l2) : 0.0f;

    const float hardness = std::clamp(m_params.hardness, 0.0f, 0.999f);
    const float feather = std::max((1.0f - hardness) * maxR, 0.75f);
    const float falloffPower = std::clamp(m_params.falloff_power, 0.25f, 4.0f);
    const float targetVal = std::clamp(m_params.target_value, 0.0f, 1.0f);

    uint32_t effectiveChannel = m_params.channel;
    uint32_t effectiveOp = m_params.op_mode;
    if (bpp == 1u && effectiveChannel == WE_BRUSH_CHAN_COLOR_RGBA8) {
        effectiveChannel = WE_BRUSH_CHAN_MASK_R8;
    } else if (bpp == 2u && effectiveChannel != WE_BRUSH_CHAN_BLUR &&
               effectiveChannel != WE_BRUSH_CHAN_SMUDGE) {
        effectiveChannel = WE_BRUSH_CHAN_FLOW_RG88;
    }

    if (effectiveChannel == WE_BRUSH_CHAN_BLUR || effectiveOp == WE_BRUSH_OP_BLUR_MASKED) {
        const std::vector<uint8_t> origTile = tileBytes;
        const int32_t kStep =
            std::clamp(static_cast<int32_t>(
                           std::lround(std::clamp(m_params.blur_radius_px, 1.0f, 64.0f) * 0.35f)),
                       1, 8);
        for (int32_t ly = ly0; ly <= ly1; ++ly) {
            const float py = static_cast<float>(ly) + 0.5f;
            for (int32_t lx = lx0; lx <= lx1; ++lx) {
                const float px = static_cast<float>(lx) + 0.5f;
                const float dist =
                    unevenCapsuleSdfScalar(px, py, localAx, localAy, localBx, localBy, ra, rb, baX,
                                           baY, l2, rr, a2, il2, degenerate);
                const float w = evalDabWeightFromDist(dist, feather, falloffPower, opacity);
                if (w <= 1e-5f) {
                    continue;
                }
                const int32_t xL = std::max(0, lx - kStep);
                const int32_t xR =
                    std::min(static_cast<int32_t>(WE_BRUSH_TILE_SIZE) - 1, lx + kStep);
                const int32_t yU = std::max(0, ly - kStep);
                const int32_t yD =
                    std::min(static_cast<int32_t>(WE_BRUSH_TILE_SIZE) - 1, ly + kStep);
                const size_t iC = (static_cast<size_t>(ly) * WE_BRUSH_TILE_SIZE + lx) * bpp;
                const size_t iL = (static_cast<size_t>(ly) * WE_BRUSH_TILE_SIZE + xL) * bpp;
                const size_t iR = (static_cast<size_t>(ly) * WE_BRUSH_TILE_SIZE + xR) * bpp;
                const size_t iU = (static_cast<size_t>(yU) * WE_BRUSH_TILE_SIZE + lx) * bpp;
                const size_t iD = (static_cast<size_t>(yD) * WE_BRUSH_TILE_SIZE + lx) * bpp;
                for (uint32_t c = 0; c < bpp; ++c) {
                    const float blurVal = 0.40f * static_cast<float>(origTile[iC + c]) +
                                          0.15f * (static_cast<float>(origTile[iL + c]) +
                                                   static_cast<float>(origTile[iR + c]) +
                                                   static_cast<float>(origTile[iU + c]) +
                                                   static_cast<float>(origTile[iD + c]));
                    const float origVal = static_cast<float>(origTile[iC + c]);
                    tileBytes[iC + c] = static_cast<uint8_t>(
                        std::clamp(origVal + (blurVal - origVal) * w, 0.0f, 255.0f) + 0.5f);
                }
            }
        }
        return;
    }

    if (effectiveChannel == WE_BRUSH_CHAN_SMUDGE || effectiveOp == WE_BRUSH_OP_SMUDGE_ADV) {
        const std::vector<uint8_t> origTile = tileBytes;
        const float dragX = std::clamp(dab.drag_dx_px, -64.0f, 64.0f);
        const float dragY = std::clamp(dab.drag_dy_px, -64.0f, 64.0f);
        auto sampleClamped = [&](float sx, float sy, uint32_t c) -> float {
            const int32_t ix = std::clamp(static_cast<int32_t>(std::floor(sx)), 0,
                                          static_cast<int32_t>(WE_BRUSH_TILE_SIZE) - 1);
            const int32_t iy = std::clamp(static_cast<int32_t>(std::floor(sy)), 0,
                                          static_cast<int32_t>(WE_BRUSH_TILE_SIZE) - 1);
            return static_cast<float>(
                origTile[(static_cast<size_t>(iy) * WE_BRUSH_TILE_SIZE + ix) * bpp + c]);
        };
        for (int32_t ly = ly0; ly <= ly1; ++ly) {
            const float py = static_cast<float>(ly) + 0.5f;
            for (int32_t lx = lx0; lx <= lx1; ++lx) {
                const float px = static_cast<float>(lx) + 0.5f;
                const float dist =
                    unevenCapsuleSdfScalar(px, py, localAx, localAy, localBx, localBy, ra, rb, baX,
                                           baY, l2, rr, a2, il2, degenerate);
                const float w = evalDabWeightFromDist(dist, feather, falloffPower, opacity);
                if (w <= 1e-5f) {
                    continue;
                }
                const size_t iC = (static_cast<size_t>(ly) * WE_BRUSH_TILE_SIZE + lx) * bpp;
                for (uint32_t c = 0; c < bpp; ++c) {
                    const float origVal = static_cast<float>(origTile[iC + c]);
                    const float adv =
                        0.40f * sampleClamped(px - dragX, py - dragY, c) +
                        0.30f * sampleClamped(px - dragX * 0.66f, py - dragY * 0.66f, c) +
                        0.20f * sampleClamped(px - dragX * 0.33f, py - dragY * 0.33f, c) +
                        0.10f * origVal;
                    tileBytes[iC + c] = static_cast<uint8_t>(
                        std::clamp(origVal + (adv - origVal) * w, 0.0f, 255.0f) + 0.5f);
                }
            }
        }
        return;
    }

#if defined(__x86_64__) || defined(_M_X64)
    if (bpp == 4u && effectiveChannel == WE_BRUSH_CHAN_COLOR_RGBA8 && cpuSupportsAvx2()) {
        paintCapsuleRgba8Avx2(tileBytes.data(), lx0, ly0, lx1, ly1, localAx, localAy, localBx,
                              localBy, ra, rb, baX, baY, l2, rr, a2, il2, degenerate, feather,
                              falloffPower, opacity, effectiveOp, m_params.color_rgba);
        return;
    }
#endif

    const float fscale = std::clamp(m_params.flow_strength, 0.0f, 1.0f);
    float srcFlowX = dab.flow_dir_x * fscale;
    float srcFlowY = ((dab.flags & 1u) ? -dab.flow_dir_y : dab.flow_dir_y) * fscale;
    const float flowMag = std::hypot(srcFlowX, srcFlowY);
    if (flowMag > 1.0f) {
        srcFlowX /= flowMag;
        srcFlowY /= flowMag;
    }

    for (int32_t ly = ly0; ly <= ly1; ++ly) {
        const float py = static_cast<float>(ly) + 0.5f;
        uint8_t *rowPtr = tileBytes.data() + static_cast<size_t>(ly) * WE_BRUSH_TILE_SIZE * bpp;
        for (int32_t lx = lx0; lx <= lx1; ++lx) {
            const float px = static_cast<float>(lx) + 0.5f;
            const float dist =
                unevenCapsuleSdfScalar(px, py, localAx, localAy, localBx, localBy, ra, rb, baX, baY,
                                       l2, rr, a2, il2, degenerate);
            const float w = evalDabWeightFromDist(dist, feather, falloffPower, opacity);
            if (w <= 1e-5f) {
                continue;
            }
            blendScalarPixel(rowPtr + static_cast<size_t>(lx) * bpp, bpp, effectiveChannel,
                             effectiveOp, w, targetVal, m_params.color_rgba, srcFlowX, srcFlowY);
        }
    }
}

void GpuBrushPainterWidget::ensureUiInitialized()
{
    if (m_uiInitialized) {
        return;
    }
    m_uiInitialized = true;

    setStyleSheet(QStringLiteral(
        "QWidget { background-color: #12141a; color: #f1f5f9; font-size: 12px; }"
        "QLabel { color: #f1f5f9; font-size: 12px; }"
        "QGroupBox#brushSectionGroup { background-color: #181b22; border: 1px solid #2a2f3d; "
        "border-radius: 6px; margin-top: 10px; padding-top: 10px; font-weight: 600; "
        "color: #f1f5f9; font-size: 12px; }"
        "QGroupBox#brushSectionGroup::title { subcontrol-origin: margin; left: 8px; padding: 0 "
        "4px; color: #94a3b8; font-size: 11px; }"
        "QComboBox, QDoubleSpinBox, QPushButton { background-color: #181b22; color: #f1f5f9; "
        "border: 1px solid #2a2f3d; border-radius: 4px; padding: 4px 8px; min-height: 28px; "
        "font-size: 12px; }"
        "QComboBox:hover, QDoubleSpinBox:hover, QPushButton:hover { background-color: #222631; }"
        "QComboBox:focus, QDoubleSpinBox:focus, QPushButton:focus, QCheckBox:focus, "
        "QSlider:focus { border: 2px solid #38bdf8; outline: none; }"
        "QComboBox:disabled, QDoubleSpinBox:disabled, QPushButton:disabled, "
        "QCheckBox:disabled { color: #94a3b8; border-color: #222631; }"
        "QDoubleSpinBox { font-family: 'JetBrains Mono', monospace; font-size: 12px; }"
        "QPushButton[activeToggle=\"true\"] { border: 1px solid #38bdf8; "
        "background-color: #222631; color: #38bdf8; }"));

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(4, 4, 4, 4);
    rootLayout->setSpacing(6);

    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto *container = new QWidget(scrollArea);
    auto *contentLayout = new QVBoxLayout(container);
    contentLayout->setContentsMargins(4, 4, 4, 4);
    contentLayout->setSpacing(8);

    // 1. Channel & Operation Mode
    auto *modeGroup = createStyledGroup(tr("Target Channel & Brush Mode"), container);
    auto *modeLayout = new QGridLayout(modeGroup);
    modeLayout->setContentsMargins(8, 8, 8, 8);
    modeLayout->setSpacing(6);

    m_channelCombo = new QComboBox(modeGroup);
    m_channelCombo->addItem(tr("Mask (R8)"), WE_BRUSH_CHAN_MASK_R8);
    m_channelCombo->addItem(tr("Flow Tangent (RG88)"), WE_BRUSH_CHAN_FLOW_RG88);
    m_channelCombo->addItem(tr("Color Albedo (RGBA8)"), WE_BRUSH_CHAN_COLOR_RGBA8);
    m_channelCombo->addItem(tr("Blur (Kawase / Gauss)"), WE_BRUSH_CHAN_BLUR);
    m_channelCombo->addItem(tr("Smudge (Advect)"), WE_BRUSH_CHAN_SMUDGE);
    m_channelCombo->setAccessibleName(tr("Brush Channel Target"));
    m_channelCombo->setAccessibleDescription(
        tr("Select R8 mask, RG88 flow map, RGBA8 color, blur, or smudge channel"));
    modeLayout->addWidget(new QLabel(tr("Channel:"), modeGroup), 0, 0);
    modeLayout->addWidget(m_channelCombo, 0, 1, 1, 2);

    m_opModeCombo = new QComboBox(modeGroup);
    m_opModeCombo->addItem(tr("Add Buildup"), WE_BRUSH_OP_ADD_BUILDUP);
    m_opModeCombo->addItem(tr("Max Cap"), WE_BRUSH_OP_MAX_CAP);
    m_opModeCombo->addItem(tr("Subtract"), WE_BRUSH_OP_SUBTRACT);
    m_opModeCombo->addItem(tr("Erase"), WE_BRUSH_OP_ERASE);
    m_opModeCombo->addItem(tr("Flow Paint"), WE_BRUSH_OP_FLOW_PAINT);
    m_opModeCombo->addItem(tr("Flow Erase (128,128)"), WE_BRUSH_OP_FLOW_ERASE);
    m_opModeCombo->addItem(tr("Flow Comb (Add)"), WE_BRUSH_OP_FLOW_COMB);
    m_opModeCombo->addItem(tr("Blur Masked"), WE_BRUSH_OP_BLUR_MASKED);
    m_opModeCombo->addItem(tr("Smudge Advect"), WE_BRUSH_OP_SMUDGE_ADV);
    m_opModeCombo->setAccessibleName(tr("Brush Blend Operation"));
    m_opModeCombo->setAccessibleDescription(tr("Choose blending operation for the active channel"));
    modeLayout->addWidget(new QLabel(tr("Operation:"), modeGroup), 1, 0);
    modeLayout->addWidget(m_opModeCombo, 1, 1);

    m_eraserBtn = new QPushButton(tr("Eraser"), modeGroup);
    m_eraserBtn->setCheckable(true);
    m_eraserBtn->setMinimumSize(64, 28);
    m_eraserBtn->setAccessibleName(tr("Toggle Eraser Mode"));
    m_eraserBtn->setAccessibleDescription(
        tr("Switches between painting and erasing on the active R8, RG88, or RGBA8 channel"));
    modeLayout->addWidget(m_eraserBtn, 1, 2);

    m_overlayCheck = new QCheckBox(tr("Show Channel Overlay"), modeGroup);
    m_overlayCheck->setChecked(m_showChannelOverlay);
    m_overlayCheck->setAccessibleName(tr("Show Channel Overlay"));
    modeLayout->addWidget(m_overlayCheck, 2, 0, 1, 3);

    contentLayout->addWidget(modeGroup);

    // 2. Tip Profile & Core Stroke Parameters
    auto *tipGroup = createStyledGroup(tr("Brush Tip & Falloff"), container);
    auto *tipLayout = new QGridLayout(tipGroup);
    tipLayout->setContentsMargins(8, 8, 8, 8);
    tipLayout->setSpacing(6);

    m_profilePreview = new BrushProfilePreviewWidget(this, tipGroup);
    tipLayout->addWidget(m_profilePreview, 0, 0, 1, 3);

    m_radiusSlider = new QSlider(Qt::Horizontal, tipGroup);
    m_radiusSlider->setRange(1, 512);
    m_radiusSlider->setMinimumHeight(28);
    m_radiusSlider->setAccessibleName(tr("Brush Radius Slider"));
    m_radiusSpin = new QDoubleSpinBox(tipGroup);
    m_radiusSpin->setRange(1.0, 512.0);
    m_radiusSpin->setDecimals(1);
    m_radiusSpin->setSingleStep(2.0);
    m_radiusSpin->setSuffix(QStringLiteral(" px"));
    m_radiusSpin->setKeyboardTracking(false);
    m_radiusSpin->setAccessibleName(tr("Brush Radius Pixels"));
    tipLayout->addWidget(new QLabel(tr("Radius:"), tipGroup), 1, 0);
    tipLayout->addWidget(m_radiusSlider, 1, 1);
    tipLayout->addWidget(m_radiusSpin, 1, 2);

    auto configureSpin = [](QDoubleSpinBox *s, double lo, double hi, double step, int dec,
                            const QString &name) {
        s->setRange(lo, hi);
        s->setSingleStep(step);
        s->setDecimals(dec);
        s->setKeyboardTracking(false);
        s->setAccessibleName(name);
    };

    m_hardnessSpin = new QDoubleSpinBox(tipGroup);
    configureSpin(m_hardnessSpin, 0.0, 0.999, 0.05, 3, tr("Brush Hardness"));
    tipLayout->addWidget(new QLabel(tr("Hardness:"), tipGroup), 2, 0);
    tipLayout->addWidget(m_hardnessSpin, 2, 1, 1, 2);

    m_falloffSpin = new QDoubleSpinBox(tipGroup);
    configureSpin(m_falloffSpin, 0.25, 4.0, 0.1, 2, tr("Edge Falloff Exponent"));
    tipLayout->addWidget(new QLabel(tr("Falloff Power:"), tipGroup), 3, 0);
    tipLayout->addWidget(m_falloffSpin, 3, 1, 1, 2);

    m_opacitySpin = new QDoubleSpinBox(tipGroup);
    configureSpin(m_opacitySpin, 0.0, 1.0, 0.05, 2, tr("Brush Opacity"));
    tipLayout->addWidget(new QLabel(tr("Opacity / Flow:"), tipGroup), 4, 0);
    tipLayout->addWidget(m_opacitySpin, 4, 1, 1, 2);

    m_targetValueSpin = new QDoubleSpinBox(tipGroup);
    configureSpin(m_targetValueSpin, 0.0, 1.0, 0.05, 2, tr("R8 Mask Target Value"));
    tipLayout->addWidget(new QLabel(tr("Mask Target:"), tipGroup), 5, 0);
    tipLayout->addWidget(m_targetValueSpin, 5, 1, 1, 2);

    m_colorBtn = new QPushButton(tr("Paint Color..."), tipGroup);
    m_colorBtn->setMinimumSize(64, 28);
    m_colorBtn->setAccessibleName(tr("Select RGBA8 Paint Color"));
    tipLayout->addWidget(new QLabel(tr("RGBA8 Color:"), tipGroup), 6, 0);
    tipLayout->addWidget(m_colorBtn, 6, 1, 1, 2);

    contentLayout->addWidget(tipGroup);

    // 3. Flow Tangent & Blur / Smudge
    auto *fxGroup = createStyledGroup(tr("Flow Tangent & Blur / Smudge"), container);
    auto *fxLayout = new QGridLayout(fxGroup);
    fxLayout->setContentsMargins(8, 8, 8, 8);
    fxLayout->setSpacing(6);

    m_flowStrengthSpin = new QDoubleSpinBox(fxGroup);
    configureSpin(m_flowStrengthSpin, 0.0, 1.0, 0.05, 2, tr("RG88 Flow Tangent Strength"));
    fxLayout->addWidget(new QLabel(tr("Flow Strength:"), fxGroup), 0, 0);
    fxLayout->addWidget(m_flowStrengthSpin, 0, 1);

    m_invertFlowYCheck = new QCheckBox(tr("Invert Flow Y"), fxGroup);
    m_invertFlowYCheck->setAccessibleName(tr("Invert RG88 Flow Y Vector"));
    fxLayout->addWidget(m_invertFlowYCheck, 1, 0, 1, 2);

    m_resetFlowRestBtn = new QPushButton(tr("Reset Flow Rest (128, 128)"), fxGroup);
    m_resetFlowRestBtn->setMinimumSize(64, 28);
    m_resetFlowRestBtn->setAccessibleName(tr("Reset Flow to Neutral 128, 128 Rest"));
    fxLayout->addWidget(m_resetFlowRestBtn, 2, 0, 1, 2);

    m_blurRadiusSpin = new QDoubleSpinBox(fxGroup);
    configureSpin(m_blurRadiusSpin, 1.0, 64.0, 1.0, 1, tr("Dual-Kawase and Gaussian Blur Radius"));
    m_blurRadiusSpin->setSuffix(QStringLiteral(" px"));
    m_blurRadiusSpin->setToolTip(
        tr("<= 4 px uses 5-tap bilinear Gaussian; > 4 px uses 4-level Dual-Kawase pyramid"));
    fxLayout->addWidget(new QLabel(tr("Blur Radius:"), fxGroup), 3, 0);
    fxLayout->addWidget(m_blurRadiusSpin, 3, 1);

    contentLayout->addWidget(fxGroup);

    // 4. Stylus Pressure & Velocity Curves
    auto *dynGroup = createStyledGroup(tr("Stylus Pressure & Velocity Curves"), container);
    auto *dynLayout = new QGridLayout(dynGroup);
    dynLayout->setContentsMargins(8, 8, 8, 8);
    dynLayout->setSpacing(6);

    m_pressureSizeSpin = new QDoubleSpinBox(dynGroup);
    configureSpin(m_pressureSizeSpin, 0.0, 1.0, 0.05, 2, tr("Pressure to Radius Coupling"));
    dynLayout->addWidget(new QLabel(tr("Pressure -> Size:"), dynGroup), 0, 0);
    dynLayout->addWidget(m_pressureSizeSpin, 0, 1);

    m_pressureAlphaSpin = new QDoubleSpinBox(dynGroup);
    configureSpin(m_pressureAlphaSpin, 0.0, 1.0, 0.05, 2, tr("Pressure to Opacity Coupling"));
    dynLayout->addWidget(new QLabel(tr("Pressure -> Opacity:"), dynGroup), 1, 0);
    dynLayout->addWidget(m_pressureAlphaSpin, 1, 1);

    m_velocityDampSpin = new QDoubleSpinBox(dynGroup);
    configureSpin(m_velocityDampSpin, 0.0, 1.0, 0.05, 2, tr("Pointer Velocity Damping"));
    dynLayout->addWidget(new QLabel(tr("Velocity Damping:"), dynGroup), 2, 0);
    dynLayout->addWidget(m_velocityDampSpin, 2, 1);

    contentLayout->addWidget(dynGroup);
    contentLayout->addStretch(1);

    scrollArea->setWidget(container);
    rootLayout->addWidget(scrollArea, 1);

    m_telemetryLabel = new QLabel(this);
    m_telemetryLabel->setWordWrap(true);
    m_telemetryLabel->setStyleSheet(
        QStringLiteral("QLabel { font-family: 'JetBrains Mono', monospace; font-size: 11px; "
                       "color: #94a3b8; background-color: #181b22; border: 1px solid #2a2f3d; "
                       "border-radius: 4px; padding: 6px; }"));
    m_telemetryLabel->setAccessibleName(tr("Brush Tile Cache Telemetry"));
    rootLayout->addWidget(m_telemetryLabel);

    setTabOrder(m_channelCombo, m_opModeCombo);
    setTabOrder(m_opModeCombo, m_eraserBtn);
    setTabOrder(m_eraserBtn, m_overlayCheck);
    setTabOrder(m_overlayCheck, m_radiusSlider);
    setTabOrder(m_radiusSlider, m_radiusSpin);
    setTabOrder(m_radiusSpin, m_hardnessSpin);
    setTabOrder(m_hardnessSpin, m_falloffSpin);
    setTabOrder(m_falloffSpin, m_opacitySpin);
    setTabOrder(m_opacitySpin, m_targetValueSpin);
    setTabOrder(m_targetValueSpin, m_colorBtn);
    setTabOrder(m_colorBtn, m_flowStrengthSpin);
    setTabOrder(m_flowStrengthSpin, m_invertFlowYCheck);
    setTabOrder(m_invertFlowYCheck, m_resetFlowRestBtn);
    setTabOrder(m_resetFlowRestBtn, m_blurRadiusSpin);
    setTabOrder(m_blurRadiusSpin, m_pressureSizeSpin);
    setTabOrder(m_pressureSizeSpin, m_pressureAlphaSpin);
    setTabOrder(m_pressureAlphaSpin, m_velocityDampSpin);

    connect(m_channelCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (m_updatingUi || idx < 0) {
            return;
        }
        setBrushChannel(static_cast<we_brush_channel_t>(m_channelCombo->itemData(idx).toUInt()));
    });

    connect(m_opModeCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (m_updatingUi || idx < 0) {
            return;
        }
        setBrushOpMode(static_cast<we_brush_op_mode_t>(m_opModeCombo->itemData(idx).toUInt()));
    });

    connect(m_eraserBtn, &QPushButton::toggled, this, [this](bool checked) {
        if (m_updatingUi) {
            return;
        }
        if (m_params.channel == WE_BRUSH_CHAN_FLOW_RG88) {
            setBrushOpMode(checked ? WE_BRUSH_OP_FLOW_ERASE : m_lastFlowOpMode);
        } else {
            setBrushOpMode(checked ? WE_BRUSH_OP_ERASE : m_lastMaskOpMode);
        }
    });

    connect(m_overlayCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_updatingUi) {
            return;
        }
        m_showChannelOverlay = checked;
        emit targetChannelOverlayRequested(static_cast<we_brush_channel_t>(m_params.channel),
                                           checked);
    });

    connect(m_radiusSlider, &QSlider::valueChanged, this, [this](int value) {
        if (m_updatingUi) {
            return;
        }
        m_params.radius_px = static_cast<float>(std::clamp(value, 1, 512));
        syncUiFromParams();
        emit brushParamsChanged(m_params);
    });

    auto bindSpin = [this](QDoubleSpinBox *spin, auto setter) {
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [this, setter](double v) {
                    if (m_updatingUi) {
                        return;
                    }
                    setter(static_cast<float>(v));
                    m_params = sanitizeBrushParams(m_params);
                    syncUiFromParams();
                    emit brushParamsChanged(m_params);
                });
    };

    bindSpin(m_radiusSpin, [this](float v) { m_params.radius_px = v; });
    bindSpin(m_hardnessSpin, [this](float v) { m_params.hardness = v; });
    bindSpin(m_falloffSpin, [this](float v) { m_params.falloff_power = v; });
    bindSpin(m_opacitySpin, [this](float v) { m_params.opacity = v; });
    bindSpin(m_targetValueSpin, [this](float v) { m_params.target_value = v; });
    bindSpin(m_flowStrengthSpin, [this](float v) { m_params.flow_strength = v; });
    bindSpin(m_blurRadiusSpin, [this](float v) { m_params.blur_radius_px = v; });
    bindSpin(m_pressureSizeSpin, [this](float v) { m_params.pressure_size_weight = v; });
    bindSpin(m_pressureAlphaSpin, [this](float v) { m_params.pressure_alpha_weight = v; });
    bindSpin(m_velocityDampSpin, [this](float v) { m_params.velocity_damp_weight = v; });

    connect(m_invertFlowYCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_updatingUi) {
            return;
        }
        m_invertFlowY = checked;
        emit brushParamsChanged(m_params);
    });

    connect(m_resetFlowRestBtn, &QPushButton::clicked, this, [this]() {
        m_emaTangentX = 1.0f;
        m_emaTangentY = 0.0f;
        setBrushChannel(WE_BRUSH_CHAN_FLOW_RG88);
        setBrushOpMode(WE_BRUSH_OP_FLOW_ERASE);
        emit statusMessage(tr("Flow brush set to neutral rest (128, 128)"));
    });

    connect(m_colorBtn, &QPushButton::clicked, this, [this]() {
        if (m_colorDialog) {
            m_colorDialog->setCurrentColor(brushColorFromParams(m_params));
            m_colorDialog->raise();
            m_colorDialog->activateWindow();
            return;
        }

        auto *dlg = new OklchColorDialog(brushColorFromParams(m_params), this);
        dlg->setWindowTitle(tr("Brush Paint Color"));
        dlg->setAttribute(Qt::WA_DeleteOnClose, true);
        m_colorDialog = dlg;

        const QPointer<GpuBrushPainterWidget> self(this);
        const QPointer<OklchColorDialog> dlgPtr(dlg);
        connect(dlg, &QDialog::accepted, this, [self, dlgPtr]() {
            if (!self || !dlgPtr) {
                return;
            }
            const QColor chosen = dlgPtr->currentColor();
            if (!chosen.isValid()) {
                return;
            }
            self->m_params.color_rgba[0] = static_cast<float>(chosen.redF());
            self->m_params.color_rgba[1] = static_cast<float>(chosen.greenF());
            self->m_params.color_rgba[2] = static_cast<float>(chosen.blueF());
            self->m_params.color_rgba[3] = static_cast<float>(chosen.alphaF());
            self->m_params = sanitizeBrushParams(self->m_params);
            self->syncUiFromParams();
            emit self->brushParamsChanged(self->m_params);
        });
        connect(dlg, &QDialog::finished, dlg, &QObject::deleteLater);
        dlg->setModal(false);
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    });

    syncUiFromParams();
}

void GpuBrushPainterWidget::syncUiFromParams()
{
    if (!m_uiInitialized) {
        return;
    }

    const QScopedValueRollback<bool> guard(m_updatingUi, true);

    auto selectComboByData = [](QComboBox *combo, uint32_t dataVal) {
        const QSignalBlocker blocker(combo);
        const int idx = combo->findData(dataVal);
        if (idx >= 0) {
            combo->setCurrentIndex(idx);
        }
    };

    selectComboByData(m_channelCombo, m_params.channel);
    selectComboByData(m_opModeCombo, m_params.op_mode);

    const bool isEraser =
        (m_params.op_mode == WE_BRUSH_OP_ERASE || m_params.op_mode == WE_BRUSH_OP_FLOW_ERASE);
    {
        const QSignalBlocker blocker(m_eraserBtn);
        m_eraserBtn->setChecked(isEraser);
        m_eraserBtn->setProperty("activeToggle", isEraser);
    }
    {
        const QSignalBlocker blocker(m_invertFlowYCheck);
        m_invertFlowYCheck->setChecked(m_invertFlowY);
    }
    {
        const QSignalBlocker blocker(m_overlayCheck);
        m_overlayCheck->setChecked(m_showChannelOverlay);
    }

    {
        const QSignalBlocker blocker(m_radiusSlider);
        m_radiusSlider->setValue(static_cast<int>(std::lround(m_params.radius_px)));
    }
    auto setSpin = [](QDoubleSpinBox *spin, float val) {
        const QSignalBlocker blocker(spin);
        spin->setValue(static_cast<double>(val));
    };

    setSpin(m_radiusSpin, m_params.radius_px);
    setSpin(m_hardnessSpin, m_params.hardness);
    setSpin(m_falloffSpin, m_params.falloff_power);
    setSpin(m_opacitySpin, m_params.opacity);
    setSpin(m_targetValueSpin, m_params.target_value);
    setSpin(m_flowStrengthSpin, m_params.flow_strength);
    setSpin(m_blurRadiusSpin, m_params.blur_radius_px);
    setSpin(m_pressureSizeSpin, m_params.pressure_size_weight);
    setSpin(m_pressureAlphaSpin, m_params.pressure_alpha_weight);
    setSpin(m_velocityDampSpin, m_params.velocity_damp_weight);

    const bool isMask = (m_params.channel == WE_BRUSH_CHAN_MASK_R8);
    const bool isFlow =
        (m_params.channel == WE_BRUSH_CHAN_FLOW_RG88) ||
        (m_params.op_mode >= WE_BRUSH_OP_FLOW_PAINT && m_params.op_mode <= WE_BRUSH_OP_FLOW_COMB);
    const bool isColor = (m_params.channel == WE_BRUSH_CHAN_COLOR_RGBA8);
    const bool isBlurOrSmudge =
        (m_params.channel == WE_BRUSH_CHAN_BLUR || m_params.channel == WE_BRUSH_CHAN_SMUDGE ||
         m_params.op_mode == WE_BRUSH_OP_BLUR_MASKED || m_params.op_mode == WE_BRUSH_OP_SMUDGE_ADV);

    m_targetValueSpin->setEnabled(isMask);
    m_colorBtn->setEnabled(isColor);
    m_flowStrengthSpin->setEnabled(isFlow);
    m_invertFlowYCheck->setEnabled(isFlow);
    m_resetFlowRestBtn->setEnabled(isFlow);
    m_blurRadiusSpin->setEnabled(isBlurOrSmudge);
    m_eraserBtn->setEnabled(!isBlurOrSmudge);

    const QColor col = brushColorFromParams(m_params);
    m_colorBtn->setStyleSheet(
        QStringLiteral("QPushButton { background-color: %1; color: %2; font-weight: 600; "
                       "min-height: 28px; }"
                       "QPushButton:disabled { background-color: #181b22; color: #94a3b8; "
                       "border-color: #222631; }")
            .arg(col.name(QColor::HexRgb),
                 col.lightnessF() > 0.5 ? QStringLiteral("#0b0c10") : QStringLiteral("#f1f5f9")));

    if (m_profilePreview) {
        m_profilePreview->update();
    }
    updateTelemetryReadout();
}

void GpuBrushPainterWidget::updateTelemetryReadout()
{
    if (!m_telemetryLabel) {
        return;
    }
    const double cacheMb =
        static_cast<double>(m_tileUndoStack.totalHostBytes() + m_hostShadowBytes) /
        (1024.0 * 1024.0);
    m_telemetryLabel->setText(tr("Canvas: %1x%2 | Slot: obj=%3 pass=%4 tex=%5\n"
                                 "CoW Strokes: %6 | Host Cache: %7 / 64.00 MiB")
                                  .arg(m_canvasWidth)
                                  .arg(m_canvasHeight)
                                  .arg(m_targetObjectId)
                                  .arg(m_targetPassIndex == 0xFFFFFFFFu
                                           ? QStringLiteral("base")
                                           : QString::number(m_targetPassIndex))
                                  .arg(m_targetTexSlot)
                                  .arg(m_tileUndoStack.transactionCount())
                                  .arg(cacheMb, 0, 'f', 2));
}

} // namespace waywal::studio
