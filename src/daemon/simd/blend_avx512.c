// SPDX-License-Identifier: GPL-3.0-or-later
#include "simd_blend.h"

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
#include <immintrin.h>

#include <stdint.h>

__attribute__((target("avx512f,avx512bw"))) void
blend_scanline_avx512(uint8_t *restrict dst, const uint8_t *restrict src_a,
                      const uint8_t *restrict src_b, uint16_t weight_b, size_t num_bytes)
{
    size_t i = 0;

    if (weight_b == 0) {
        for (; i + 128 <= num_bytes; i += 128) {
            __m512i va0 = _mm512_loadu_si512((const __m512i *)(src_a + i));
            __m512i va1 = _mm512_loadu_si512((const __m512i *)(src_a + i + 64));
            uint8_t *d0 = dst + i;
            uint8_t *d1 = dst + i + 64;
            if (((uintptr_t)d0 & 63u) == 0) {
                _mm512_stream_si512((__m512i *)d0, va0);
                _mm512_stream_si512((__m512i *)d1, va1);
            } else {
                _mm512_storeu_si512((__m512i *)d0, va0);
                _mm512_storeu_si512((__m512i *)d1, va1);
            }
        }
        for (; i + 64 <= num_bytes; i += 64) {
            __m512i va = _mm512_loadu_si512((const __m512i *)(src_a + i));
            uint8_t *d = dst + i;
            if (((uintptr_t)d & 63u) == 0) {
                _mm512_stream_si512((__m512i *)d, va);
            } else {
                _mm512_storeu_si512((__m512i *)d, va);
            }
        }
    } else if (weight_b >= 256) {
        for (; i + 128 <= num_bytes; i += 128) {
            __m512i vb0 = _mm512_loadu_si512((const __m512i *)(src_b + i));
            __m512i vb1 = _mm512_loadu_si512((const __m512i *)(src_b + i + 64));
            uint8_t *d0 = dst + i;
            uint8_t *d1 = dst + i + 64;
            if (((uintptr_t)d0 & 63u) == 0) {
                _mm512_stream_si512((__m512i *)d0, vb0);
                _mm512_stream_si512((__m512i *)d1, vb1);
            } else {
                _mm512_storeu_si512((__m512i *)d0, vb0);
                _mm512_storeu_si512((__m512i *)d1, vb1);
            }
        }
        for (; i + 64 <= num_bytes; i += 64) {
            __m512i vb = _mm512_loadu_si512((const __m512i *)(src_b + i));
            uint8_t *d = dst + i;
            if (((uintptr_t)d & 63u) == 0) {
                _mm512_stream_si512((__m512i *)d, vb);
            } else {
                _mm512_storeu_si512((__m512i *)d, vb);
            }
        }
    } else {
        uint8_t wa_half = (uint8_t)((256u - weight_b) >> 1);
        uint8_t wb_half = (uint8_t)(weight_b >> 1);
        const __m512i v_weights =
            _mm512_set1_epi16((short)(((uint16_t)wb_half << 8) | (uint16_t)wa_half));

        if ((weight_b & 1u) == 0) {
            for (; i + 128 <= num_bytes; i += 128) {
                __m512i va0 = _mm512_loadu_si512((const __m512i *)(src_a + i));
                __m512i vb0 = _mm512_loadu_si512((const __m512i *)(src_b + i));
                __m512i va1 = _mm512_loadu_si512((const __m512i *)(src_a + i + 64));
                __m512i vb1 = _mm512_loadu_si512((const __m512i *)(src_b + i + 64));

                __m512i ab_lo0 = _mm512_unpacklo_epi8(va0, vb0);
                __m512i ab_hi0 = _mm512_unpackhi_epi8(va0, vb0);
                __m512i ab_lo1 = _mm512_unpacklo_epi8(va1, vb1);
                __m512i ab_hi1 = _mm512_unpackhi_epi8(va1, vb1);

                __m512i r_lo0 = _mm512_srli_epi16(_mm512_maddubs_epi16(ab_lo0, v_weights), 7);
                __m512i r_hi0 = _mm512_srli_epi16(_mm512_maddubs_epi16(ab_hi0, v_weights), 7);
                __m512i r_lo1 = _mm512_srli_epi16(_mm512_maddubs_epi16(ab_lo1, v_weights), 7);
                __m512i r_hi1 = _mm512_srli_epi16(_mm512_maddubs_epi16(ab_hi1, v_weights), 7);

                __m512i packed0 = _mm512_packus_epi16(r_lo0, r_hi0);
                __m512i packed1 = _mm512_packus_epi16(r_lo1, r_hi1);

                uint8_t *d0 = dst + i;
                uint8_t *d1 = dst + i + 64;
                if (((uintptr_t)d0 & 63u) == 0) {
                    _mm512_stream_si512((__m512i *)d0, packed0);
                    _mm512_stream_si512((__m512i *)d1, packed1);
                } else {
                    _mm512_storeu_si512((__m512i *)d0, packed0);
                    _mm512_storeu_si512((__m512i *)d1, packed1);
                }
            }
            for (; i + 64 <= num_bytes; i += 64) {
                __m512i va = _mm512_loadu_si512((const __m512i *)(src_a + i));
                __m512i vb = _mm512_loadu_si512((const __m512i *)(src_b + i));

                __m512i ab_lo = _mm512_unpacklo_epi8(va, vb);
                __m512i ab_hi = _mm512_unpackhi_epi8(va, vb);

                __m512i r_lo = _mm512_srli_epi16(_mm512_maddubs_epi16(ab_lo, v_weights), 7);
                __m512i r_hi = _mm512_srli_epi16(_mm512_maddubs_epi16(ab_hi, v_weights), 7);

                __m512i packed = _mm512_packus_epi16(r_lo, r_hi);
                uint8_t *d = dst + i;
                if (((uintptr_t)d & 63u) == 0) {
                    _mm512_stream_si512((__m512i *)d, packed);
                } else {
                    _mm512_storeu_si512((__m512i *)d, packed);
                }
            }
        } else {
            const __m512i v_ones = _mm512_set1_epi16(0x0101);
            for (; i + 128 <= num_bytes; i += 128) {
                __m512i va0 = _mm512_loadu_si512((const __m512i *)(src_a + i));
                __m512i vb0 = _mm512_loadu_si512((const __m512i *)(src_b + i));
                __m512i va1 = _mm512_loadu_si512((const __m512i *)(src_a + i + 64));
                __m512i vb1 = _mm512_loadu_si512((const __m512i *)(src_b + i + 64));

                __m512i ab_lo0 = _mm512_unpacklo_epi8(va0, vb0);
                __m512i ab_hi0 = _mm512_unpackhi_epi8(va0, vb0);
                __m512i ab_lo1 = _mm512_unpacklo_epi8(va1, vb1);
                __m512i ab_hi1 = _mm512_unpackhi_epi8(va1, vb1);

                __m512i r_lo0 = _mm512_srli_epi16(
                    _mm512_add_epi16(_mm512_maddubs_epi16(ab_lo0, v_weights),
                                     _mm512_srli_epi16(_mm512_maddubs_epi16(ab_lo0, v_ones), 1)),
                    7);
                __m512i r_hi0 = _mm512_srli_epi16(
                    _mm512_add_epi16(_mm512_maddubs_epi16(ab_hi0, v_weights),
                                     _mm512_srli_epi16(_mm512_maddubs_epi16(ab_hi0, v_ones), 1)),
                    7);
                __m512i r_lo1 = _mm512_srli_epi16(
                    _mm512_add_epi16(_mm512_maddubs_epi16(ab_lo1, v_weights),
                                     _mm512_srli_epi16(_mm512_maddubs_epi16(ab_lo1, v_ones), 1)),
                    7);
                __m512i r_hi1 = _mm512_srli_epi16(
                    _mm512_add_epi16(_mm512_maddubs_epi16(ab_hi1, v_weights),
                                     _mm512_srli_epi16(_mm512_maddubs_epi16(ab_hi1, v_ones), 1)),
                    7);

                __m512i packed0 = _mm512_packus_epi16(r_lo0, r_hi0);
                __m512i packed1 = _mm512_packus_epi16(r_lo1, r_hi1);
                uint8_t *d0 = dst + i;
                uint8_t *d1 = dst + i + 64;
                if (((uintptr_t)d0 & 63u) == 0) {
                    _mm512_stream_si512((__m512i *)d0, packed0);
                    _mm512_stream_si512((__m512i *)d1, packed1);
                } else {
                    _mm512_storeu_si512((__m512i *)d0, packed0);
                    _mm512_storeu_si512((__m512i *)d1, packed1);
                }
            }
            for (; i + 64 <= num_bytes; i += 64) {
                __m512i va = _mm512_loadu_si512((const __m512i *)(src_a + i));
                __m512i vb = _mm512_loadu_si512((const __m512i *)(src_b + i));

                __m512i ab_lo = _mm512_unpacklo_epi8(va, vb);
                __m512i ab_hi = _mm512_unpackhi_epi8(va, vb);

                __m512i r_lo = _mm512_srli_epi16(
                    _mm512_add_epi16(_mm512_maddubs_epi16(ab_lo, v_weights),
                                     _mm512_srli_epi16(_mm512_maddubs_epi16(ab_lo, v_ones), 1)),
                    7);
                __m512i r_hi = _mm512_srli_epi16(
                    _mm512_add_epi16(_mm512_maddubs_epi16(ab_hi, v_weights),
                                     _mm512_srli_epi16(_mm512_maddubs_epi16(ab_hi, v_ones), 1)),
                    7);

                __m512i packed = _mm512_packus_epi16(r_lo, r_hi);
                uint8_t *d = dst + i;
                if (((uintptr_t)d & 63u) == 0) {
                    _mm512_stream_si512((__m512i *)d, packed);
                } else {
                    _mm512_storeu_si512((__m512i *)d, packed);
                }
            }
        }
    }

    if (i < num_bytes) {
        blend_scanline_scalar(dst + i, src_a + i, src_b + i, weight_b, num_bytes - i);
    }
    _mm_sfence();
}
#endif
