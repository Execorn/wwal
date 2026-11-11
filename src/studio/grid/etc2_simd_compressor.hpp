// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WWAL_STUDIO_GRID_ETC2_SIMD_COMPRESSOR_HPP
#define WWAL_STUDIO_GRID_ETC2_SIMD_COMPRESSOR_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace wwal::studio::etc2 {

inline constexpr int kModifierTables[8][2] = {
    {2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183},
};

// Rec.709 integer luma weights summing to 256; worst-case 8-pixel subblock error is
// 8 * 256 * 255^2 = 133,171,200, fitting comfortably inside signed 32-bit AVX2 lanes.
inline constexpr int kWeightR = 54;
inline constexpr int kWeightG = 183;
inline constexpr int kWeightB = 19;

struct SubblockResult {
    int table{0};
    uint16_t msb_bits{0};
    uint16_t lsb_bits{0};
    uint32_t error{0};
};

[[nodiscard]] inline int expand4To8(int v4) noexcept { return (v4 << 4) | v4; }
[[nodiscard]] inline int expand5To8(int v5) noexcept { return (v5 << 3) | (v5 >> 2); }

#if defined(__AVX2__)
[[nodiscard]] inline SubblockResult evaluateSubblockAvx2(const uint8_t blockRgba[16][4],
                                                         const int pixIndices[8], int baseR,
                                                         int baseG, int baseB) noexcept
{
    const __m256i vZero = _mm256_setzero_si256();
    const __m256i v255 = _mm256_set1_epi32(255);
    const __m256i vWR = _mm256_set1_epi32(kWeightR);
    const __m256i vWG = _mm256_set1_epi32(kWeightG);
    const __m256i vWB = _mm256_set1_epi32(kWeightB);

    const __m256i vBaseR = _mm256_set1_epi32(baseR);
    const __m256i vBaseG = _mm256_set1_epi32(baseG);
    const __m256i vBaseB = _mm256_set1_epi32(baseB);

    const __m256i vSmall = _mm256_setr_epi32(2, 5, 9, 13, 18, 24, 33, 47);
    const __m256i vLarge = _mm256_setr_epi32(8, 17, 29, 42, 60, 80, 106, 183);
    const __m256i vDeltas[4] = {
        vSmall,
        vLarge,
        _mm256_sub_epi32(vZero, vSmall),
        _mm256_sub_epi32(vZero, vLarge),
    };

    __m256i candR[4];
    __m256i candG[4];
    __m256i candB[4];
    for (int s = 0; s < 4; ++s) {
        candR[s] =
            _mm256_min_epi32(_mm256_max_epi32(_mm256_add_epi32(vBaseR, vDeltas[s]), vZero), v255);
        candG[s] =
            _mm256_min_epi32(_mm256_max_epi32(_mm256_add_epi32(vBaseG, vDeltas[s]), vZero), v255);
        candB[s] =
            _mm256_min_epi32(_mm256_max_epi32(_mm256_add_epi32(vBaseB, vDeltas[s]), vZero), v255);
    }

    __m256i vTableErr = vZero;
    __m256i vMsbMask = vZero;
    __m256i vLsbMask = vZero;

    const __m256i vOne = _mm256_set1_epi32(1);
    const __m256i vTwo = _mm256_set1_epi32(2);
    const __m256i vThree = _mm256_set1_epi32(3);

    for (int i = 0; i < 8; ++i) {
        const int pixIdx = pixIndices[i];
        const __m256i vPR = _mm256_set1_epi32(blockRgba[pixIdx][0]);
        const __m256i vPG = _mm256_set1_epi32(blockRgba[pixIdx][1]);
        const __m256i vPB = _mm256_set1_epi32(blockRgba[pixIdx][2]);
        const __m256i vBit = _mm256_set1_epi32(1 << pixIdx);

        auto calcErr = [&](int s) noexcept -> __m256i {
            const __m256i dr = _mm256_sub_epi32(vPR, candR[s]);
            const __m256i dg = _mm256_sub_epi32(vPG, candG[s]);
            const __m256i db = _mm256_sub_epi32(vPB, candB[s]);
            const __m256i er = _mm256_mullo_epi32(_mm256_mullo_epi32(dr, dr), vWR);
            const __m256i eg = _mm256_mullo_epi32(_mm256_mullo_epi32(dg, dg), vWG);
            const __m256i eb = _mm256_mullo_epi32(_mm256_mullo_epi32(db, db), vWB);
            return _mm256_add_epi32(_mm256_add_epi32(er, eg), eb);
        };

        const __m256i e0 = calcErr(0);
        const __m256i e1 = calcErr(1);
        const __m256i e2 = calcErr(2);
        const __m256i e3 = calcErr(3);

        __m256i bestE = e0;
        __m256i bestSel = vZero;

        const __m256i lt1 = _mm256_cmpgt_epi32(bestE, e1);
        bestE = _mm256_blendv_epi8(bestE, e1, lt1);
        bestSel = _mm256_blendv_epi8(bestSel, vOne, lt1);

        const __m256i lt2 = _mm256_cmpgt_epi32(bestE, e2);
        bestE = _mm256_blendv_epi8(bestE, e2, lt2);
        bestSel = _mm256_blendv_epi8(bestSel, vTwo, lt2);

        const __m256i lt3 = _mm256_cmpgt_epi32(bestE, e3);
        bestE = _mm256_blendv_epi8(bestE, e3, lt3);
        bestSel = _mm256_blendv_epi8(bestSel, vThree, lt3);

        vTableErr = _mm256_add_epi32(vTableErr, bestE);

        const __m256i hasMsb = _mm256_cmpeq_epi32(_mm256_and_si256(bestSel, vTwo), vTwo);
        const __m256i hasLsb = _mm256_cmpeq_epi32(_mm256_and_si256(bestSel, vOne), vOne);
        vMsbMask = _mm256_or_si256(vMsbMask, _mm256_and_si256(hasMsb, vBit));
        vLsbMask = _mm256_or_si256(vLsbMask, _mm256_and_si256(hasLsb, vBit));
    }

    alignas(32) uint32_t errs[8];
    alignas(32) uint32_t msbs[8];
    alignas(32) uint32_t lsbs[8];
    _mm256_store_si256(reinterpret_cast<__m256i *>(errs), vTableErr);
    _mm256_store_si256(reinterpret_cast<__m256i *>(msbs), vMsbMask);
    _mm256_store_si256(reinterpret_cast<__m256i *>(lsbs), vLsbMask);

    SubblockResult best{
        .table = 0,
        .msb_bits = static_cast<uint16_t>(msbs[0]),
        .lsb_bits = static_cast<uint16_t>(lsbs[0]),
        .error = errs[0],
    };
    for (int t = 1; t < 8; ++t) {
        if (errs[t] < best.error) {
            best.error = errs[t];
            best.table = t;
            best.msb_bits = static_cast<uint16_t>(msbs[t]);
            best.lsb_bits = static_cast<uint16_t>(lsbs[t]);
        }
    }
    return best;
}
#endif

[[nodiscard]] inline SubblockResult evaluateSubblockScalar(const uint8_t blockRgba[16][4],
                                                           const int pixIndices[8], int baseR,
                                                           int baseG, int baseB) noexcept
{
    SubblockResult best{};
    best.error = std::numeric_limits<uint32_t>::max();
    for (int t = 0; t < 8; ++t) {
        const int sm = kModifierTables[t][0];
        const int lg = kModifierTables[t][1];
        const int deltas[4] = {sm, lg, -sm, -lg};
        int candR[4];
        int candG[4];
        int candB[4];
        for (int s = 0; s < 4; ++s) {
            candR[s] = std::clamp(baseR + deltas[s], 0, 255);
            candG[s] = std::clamp(baseG + deltas[s], 0, 255);
            candB[s] = std::clamp(baseB + deltas[s], 0, 255);
        }
        uint32_t tableErr = 0;
        uint16_t msbMask = 0;
        uint16_t lsbMask = 0;
        for (int i = 0; i < 8; ++i) {
            const int pixIdx = pixIndices[i];
            const int pr = blockRgba[pixIdx][0];
            const int pg = blockRgba[pixIdx][1];
            const int pb = blockRgba[pixIdx][2];
            uint32_t bestE = std::numeric_limits<uint32_t>::max();
            int bestS = 0;
            for (int s = 0; s < 4; ++s) {
                const int dr = pr - candR[s];
                const int dg = pg - candG[s];
                const int db = pb - candB[s];
                const uint32_t e = static_cast<uint32_t>(kWeightR * dr * dr + kWeightG * dg * dg +
                                                         kWeightB * db * db);
                if (e < bestE) {
                    bestE = e;
                    bestS = s;
                }
            }
            tableErr += bestE;
            if (tableErr >= best.error) {
                break;
            }
            if (bestS & 2) {
                msbMask |= static_cast<uint16_t>(1u << pixIdx);
            }
            if (bestS & 1) {
                lsbMask |= static_cast<uint16_t>(1u << pixIdx);
            }
        }
        if (tableErr < best.error) {
            best = {.table = t, .msb_bits = msbMask, .lsb_bits = lsbMask, .error = tableErr};
        }
    }
    return best;
}

inline void encodeBlock4x4Perceptual(const uint8_t blockRgba[16][4], uint8_t out[8]) noexcept
{
    static constexpr int kFlip0Sub0[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    static constexpr int kFlip0Sub1[8] = {8, 9, 10, 11, 12, 13, 14, 15};
    static constexpr int kFlip1Sub0[8] = {0, 1, 4, 5, 8, 9, 12, 13};
    static constexpr int kFlip1Sub1[8] = {2, 3, 6, 7, 10, 11, 14, 15};

    uint64_t bestTotalErr = std::numeric_limits<uint64_t>::max();
    uint8_t bestBlock[8]{};

    for (int flip = 0; flip <= 1; ++flip) {
        const int *sub0 = (flip == 0) ? kFlip0Sub0 : kFlip1Sub0;
        const int *sub1 = (flip == 0) ? kFlip0Sub1 : kFlip1Sub1;

        int sumR0 = 0;
        int sumG0 = 0;
        int sumB0 = 0;
        int sumR1 = 0;
        int sumG1 = 0;
        int sumB1 = 0;
        for (int i = 0; i < 8; ++i) {
            sumR0 += blockRgba[sub0[i]][0];
            sumG0 += blockRgba[sub0[i]][1];
            sumB0 += blockRgba[sub0[i]][2];
            sumR1 += blockRgba[sub1[i]][0];
            sumG1 += blockRgba[sub1[i]][1];
            sumB1 += blockRgba[sub1[i]][2];
        }
        const int avgR0 = (sumR0 + 4) >> 3;
        const int avgG0 = (sumG0 + 4) >> 3;
        const int avgB0 = (sumB0 + 4) >> 3;
        const int avgR1 = (sumR1 + 4) >> 3;
        const int avgG1 = (sumG1 + 4) >> 3;
        const int avgB1 = (sumB1 + 4) >> 3;

        const int r5_0 = (avgR0 * 31 + 127) / 255;
        const int g5_0 = (avgG0 * 31 + 127) / 255;
        const int b5_0 = (avgB0 * 31 + 127) / 255;
        const int r5_1 = (avgR1 * 31 + 127) / 255;
        const int g5_1 = (avgG1 * 31 + 127) / 255;
        const int b5_1 = (avgB1 * 31 + 127) / 255;
        const int dR = r5_1 - r5_0;
        const int dG = g5_1 - g5_0;
        const int dB = b5_1 - b5_0;

        uint8_t b0 = 0;
        uint8_t b1 = 0;
        uint8_t b2 = 0;
        int diffBit = 0;
        int baseR0 = 0;
        int baseG0 = 0;
        int baseB0 = 0;
        int baseR1 = 0;
        int baseG1 = 0;
        int baseB1 = 0;
        if (dR >= -4 && dR <= 3 && dG >= -4 && dG <= 3 && dB >= -4 && dB <= 3) {
            diffBit = 1;
            b0 = static_cast<uint8_t>((r5_0 << 3) | (dR & 0x7));
            b1 = static_cast<uint8_t>((g5_0 << 3) | (dG & 0x7));
            b2 = static_cast<uint8_t>((b5_0 << 3) | (dB & 0x7));
            baseR0 = expand5To8(r5_0);
            baseG0 = expand5To8(g5_0);
            baseB0 = expand5To8(b5_0);
            baseR1 = expand5To8(r5_0 + dR);
            baseG1 = expand5To8(g5_0 + dG);
            baseB1 = expand5To8(b5_0 + dB);
        } else {
            const int r4_0 = (avgR0 * 15 + 127) / 255;
            const int g4_0 = (avgG0 * 15 + 127) / 255;
            const int b4_0 = (avgB0 * 15 + 127) / 255;
            const int r4_1 = (avgR1 * 15 + 127) / 255;
            const int g4_1 = (avgG1 * 15 + 127) / 255;
            const int b4_1 = (avgB1 * 15 + 127) / 255;
            b0 = static_cast<uint8_t>((r4_0 << 4) | r4_1);
            b1 = static_cast<uint8_t>((g4_0 << 4) | g4_1);
            b2 = static_cast<uint8_t>((b4_0 << 4) | b4_1);
            baseR0 = expand4To8(r4_0);
            baseG0 = expand4To8(g4_0);
            baseB0 = expand4To8(b4_0);
            baseR1 = expand4To8(r4_1);
            baseG1 = expand4To8(g4_1);
            baseB1 = expand4To8(b4_1);
        }

#if defined(__AVX2__)
        const SubblockResult ev0 = evaluateSubblockAvx2(blockRgba, sub0, baseR0, baseG0, baseB0);
        const SubblockResult ev1 = evaluateSubblockAvx2(blockRgba, sub1, baseR1, baseG1, baseB1);
#else
        const SubblockResult ev0 = evaluateSubblockScalar(blockRgba, sub0, baseR0, baseG0, baseB0);
        const SubblockResult ev1 = evaluateSubblockScalar(blockRgba, sub1, baseR1, baseG1, baseB1);
#endif
        const uint64_t totalErr = static_cast<uint64_t>(ev0.error) + ev1.error;
        if (totalErr < bestTotalErr) {
            bestTotalErr = totalErr;
            const uint16_t msb = static_cast<uint16_t>(ev0.msb_bits | ev1.msb_bits);
            const uint16_t lsb = static_cast<uint16_t>(ev0.lsb_bits | ev1.lsb_bits);
            bestBlock[0] = b0;
            bestBlock[1] = b1;
            bestBlock[2] = b2;
            bestBlock[3] =
                static_cast<uint8_t>((ev0.table << 5) | (ev1.table << 2) | (diffBit << 1) | flip);
            bestBlock[4] = static_cast<uint8_t>(msb >> 8);
            bestBlock[5] = static_cast<uint8_t>(msb & 0xFFu);
            bestBlock[6] = static_cast<uint8_t>(lsb >> 8);
            bestBlock[7] = static_cast<uint8_t>(lsb & 0xFFu);
        }
    }
    std::memcpy(out, bestBlock, 8);
}

inline void encodeRgbaToEtc2Simd(const uint8_t *rgba, int width, int height, int rowStrideBytes,
                                 std::vector<uint8_t> &outBlocks)
{
    if (!rgba || width <= 0 || height <= 0 || rowStrideBytes < width * 4) {
        outBlocks.clear();
        return;
    }
    const int blocksX = (width + 3) / 4;
    const int blocksY = (height + 3) / 4;
    outBlocks.resize(static_cast<size_t>(blocksX) * static_cast<size_t>(blocksY) * 8u);

    uint8_t *dst = outBlocks.data();
    const size_t stride = static_cast<size_t>(rowStrideBytes);
    for (int by = 0; by < blocksY; ++by) {
        const int py0 = by * 4;
        const bool interiorY = (py0 + 3 < height);
        const uint8_t *row0 = rgba + static_cast<size_t>(py0) * stride;
        const uint8_t *row1 =
            interiorY ? (row0 + stride)
                      : (rgba + static_cast<size_t>(std::min(height - 1, py0 + 1)) * stride);
        const uint8_t *row2 =
            interiorY ? (row1 + stride)
                      : (rgba + static_cast<size_t>(std::min(height - 1, py0 + 2)) * stride);
        const uint8_t *row3 =
            interiorY ? (row2 + stride)
                      : (rgba + static_cast<size_t>(std::min(height - 1, py0 + 3)) * stride);

        for (int bx = 0; bx < blocksX; ++bx) {
            alignas(16) uint8_t blockPixels[16][4];
            const int px0 = bx * 4;
            if (px0 + 3 < width) [[likely]] {
                for (int x = 0; x < 4; ++x) {
                    const size_t xOff = static_cast<size_t>(px0 + x) * 4u;
                    std::memcpy(blockPixels[x * 4 + 0], row0 + xOff, 4);
                    std::memcpy(blockPixels[x * 4 + 1], row1 + xOff, 4);
                    std::memcpy(blockPixels[x * 4 + 2], row2 + xOff, 4);
                    std::memcpy(blockPixels[x * 4 + 3], row3 + xOff, 4);
                }
            } else {
                for (int x = 0; x < 4; ++x) {
                    const size_t xOff = static_cast<size_t>(std::min(width - 1, px0 + x)) * 4u;
                    std::memcpy(blockPixels[x * 4 + 0], row0 + xOff, 4);
                    std::memcpy(blockPixels[x * 4 + 1], row1 + xOff, 4);
                    std::memcpy(blockPixels[x * 4 + 2], row2 + xOff, 4);
                    std::memcpy(blockPixels[x * 4 + 3], row3 + xOff, 4);
                }
            }
            encodeBlock4x4Perceptual(blockPixels, dst);
            dst += 8;
        }
    }
}

} // namespace wwal::studio::etc2

#endif // WWAL_STUDIO_GRID_ETC2_SIMD_COMPRESSOR_HPP
