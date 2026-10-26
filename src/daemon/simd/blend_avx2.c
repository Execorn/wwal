// SPDX-License-Identifier: GPL-3.0-or-later
#include "simd_blend.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#include <stdint.h>

__attribute__((target("avx2"))) void blend_scanline_avx2(uint8_t *restrict dst,
                                                         const uint8_t *restrict src_a,
                                                         const uint8_t *restrict src_b,
                                                         uint16_t weight_b, size_t num_bytes)
{
    size_t i = 0;

    if (weight_b == 0) {
        for (; i + 32 <= num_bytes; i += 32) {
            __m256i va = _mm256_loadu_si256((const __m256i *)(src_a + i));
            uint8_t *d = dst + i;
            if (((uintptr_t)d & 31u) == 0) {
                _mm256_stream_si256((__m256i *)d, va);
            } else {
                _mm256_storeu_si256((__m256i *)d, va);
            }
        }
    } else if (weight_b >= 256) {
        for (; i + 32 <= num_bytes; i += 32) {
            __m256i vb = _mm256_loadu_si256((const __m256i *)(src_b + i));
            uint8_t *d = dst + i;
            if (((uintptr_t)d & 31u) == 0) {
                _mm256_stream_si256((__m256i *)d, vb);
            } else {
                _mm256_storeu_si256((__m256i *)d, vb);
            }
        }
    } else {
        uint8_t wa_half = (uint8_t)((256u - weight_b) >> 1);
        uint8_t wb_half = (uint8_t)(weight_b >> 1);
        const __m256i v_weights =
            _mm256_set1_epi16((short)(((uint16_t)wb_half << 8) | (uint16_t)wa_half));

        if ((weight_b & 1u) == 0) {
            for (; i + 32 <= num_bytes; i += 32) {
                __m256i va = _mm256_loadu_si256((const __m256i *)(src_a + i));
                __m256i vb = _mm256_loadu_si256((const __m256i *)(src_b + i));

                __m256i ab_lo = _mm256_unpacklo_epi8(va, vb);
                __m256i ab_hi = _mm256_unpackhi_epi8(va, vb);

                __m256i r_lo = _mm256_srli_epi16(_mm256_maddubs_epi16(ab_lo, v_weights), 7);
                __m256i r_hi = _mm256_srli_epi16(_mm256_maddubs_epi16(ab_hi, v_weights), 7);

                __m256i packed = _mm256_packus_epi16(r_lo, r_hi);
                uint8_t *d = dst + i;
                if (((uintptr_t)d & 31u) == 0) {
                    _mm256_stream_si256((__m256i *)d, packed);
                } else {
                    _mm256_storeu_si256((__m256i *)d, packed);
                }
            }
        } else {
            const __m256i v_ones = _mm256_set1_epi16(0x0101);
            for (; i + 32 <= num_bytes; i += 32) {
                __m256i va = _mm256_loadu_si256((const __m256i *)(src_a + i));
                __m256i vb = _mm256_loadu_si256((const __m256i *)(src_b + i));

                __m256i ab_lo = _mm256_unpacklo_epi8(va, vb);
                __m256i ab_hi = _mm256_unpackhi_epi8(va, vb);

                __m256i r_lo = _mm256_srli_epi16(
                    _mm256_add_epi16(_mm256_maddubs_epi16(ab_lo, v_weights),
                                     _mm256_srli_epi16(_mm256_maddubs_epi16(ab_lo, v_ones), 1)),
                    7);
                __m256i r_hi = _mm256_srli_epi16(
                    _mm256_add_epi16(_mm256_maddubs_epi16(ab_hi, v_weights),
                                     _mm256_srli_epi16(_mm256_maddubs_epi16(ab_hi, v_ones), 1)),
                    7);

                __m256i packed = _mm256_packus_epi16(r_lo, r_hi);
                uint8_t *d = dst + i;
                if (((uintptr_t)d & 31u) == 0) {
                    _mm256_stream_si256((__m256i *)d, packed);
                } else {
                    _mm256_storeu_si256((__m256i *)d, packed);
                }
            }
        }
    }

    if (i < num_bytes) {
        blend_scanline_scalar(dst + i, src_a + i, src_b + i, weight_b, num_bytes - i);
    }
}
#endif
