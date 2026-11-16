// SPDX-License-Identifier: GPL-3.0-or-later
#include "simd_blend.h"

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
#include <immintrin.h>
#include <stdbool.h>
#include <stdint.h>

__attribute__((target("avx512f,avx512bw"), always_inline)) static inline void
store_vec512(uint8_t *dst, __m512i v, bool aligned_dst, bool *used_stream)
{
    if (aligned_dst) {
        _mm512_stream_si512((__m512i *)dst, v);
        *used_stream = true;
    } else {
        _mm512_storeu_si512((void *)dst, v);
    }
}

__attribute__((target("avx512f,avx512bw"), always_inline)) static inline void
store_pair512(uint8_t *dst, __m512i v0, __m512i v1, bool aligned_dst, bool *used_stream)
{
    if (aligned_dst) {
        _mm512_stream_si512((__m512i *)dst, v0);
        _mm512_stream_si512((__m512i *)(dst + 64), v1);
        *used_stream = true;
    } else {
        _mm512_storeu_si512((void *)dst, v0);
        _mm512_storeu_si512((void *)(dst + 64), v1);
    }
}

__attribute__((target("avx512f,avx512bw"), always_inline)) static inline size_t
copy_scanline_avx512(uint8_t *restrict dst, const uint8_t *restrict src, size_t num_bytes,
                     bool aligned_dst, bool *used_stream)
{
    size_t i = 0;
    for (; num_bytes >= 128 && i <= num_bytes - 128; i += 128) {
        __m512i v0 = _mm512_loadu_si512((const void *)(src + i));
        __m512i v1 = _mm512_loadu_si512((const void *)(src + i + 64));
        store_pair512(dst + i, v0, v1, aligned_dst, used_stream);
    }
    for (; num_bytes >= 64 && i <= num_bytes - 64; i += 64) {
        __m512i v = _mm512_loadu_si512((const void *)(src + i));
        store_vec512(dst + i, v, aligned_dst, used_stream);
    }
    return i;
}

__attribute__((target("avx512f,avx512bw"), always_inline)) static inline __m512i
blend64_even_avx512(__m512i va, __m512i vb, __m512i v_weights, __m512i v_round)
{
    __m512i ab_lo = _mm512_unpacklo_epi8(va, vb);
    __m512i ab_hi = _mm512_unpackhi_epi8(va, vb);
    __m512i r_lo =
        _mm512_srli_epi16(_mm512_add_epi16(_mm512_maddubs_epi16(ab_lo, v_weights), v_round), 7);
    __m512i r_hi =
        _mm512_srli_epi16(_mm512_add_epi16(_mm512_maddubs_epi16(ab_hi, v_weights), v_round), 7);
    return _mm512_packus_epi16(r_lo, r_hi);
}

__attribute__((target("avx512f,avx512bw"), always_inline)) static inline __m512i
blend64_odd_avx512(__m512i va, __m512i vb, __m512i v_weights, __m512i v_ones, __m512i v_round)
{
    __m512i ab_lo = _mm512_unpacklo_epi8(va, vb);
    __m512i ab_hi = _mm512_unpackhi_epi8(va, vb);
    __m512i odd_lo = _mm512_srli_epi16(_mm512_maddubs_epi16(ab_lo, v_ones), 1);
    __m512i odd_hi = _mm512_srli_epi16(_mm512_maddubs_epi16(ab_hi, v_ones), 1);
    __m512i sum_lo = _mm512_add_epi16(_mm512_maddubs_epi16(ab_lo, v_weights), odd_lo);
    __m512i sum_hi = _mm512_add_epi16(_mm512_maddubs_epi16(ab_hi, v_weights), odd_hi);
    __m512i r_lo = _mm512_srli_epi16(_mm512_add_epi16(sum_lo, v_round), 7);
    __m512i r_hi = _mm512_srli_epi16(_mm512_add_epi16(sum_hi, v_round), 7);
    return _mm512_packus_epi16(r_lo, r_hi);
}

__attribute__((target("avx512f,avx512bw"))) void
blend_scanline_avx512(uint8_t *restrict dst, const uint8_t *restrict src_a,
                      const uint8_t *restrict src_b, uint16_t weight_b, size_t num_bytes)
{
    if (num_bytes == 0 || !dst) {
        return;
    }

    const bool aligned_dst = (((uintptr_t)dst & 63u) == 0u);
    bool used_stream = false;
    size_t i = 0;

    if (weight_b == 0) {
        if (!src_a) {
            return;
        }
        i = copy_scanline_avx512(dst, src_a, num_bytes, aligned_dst, &used_stream);
    } else if (weight_b >= 256) {
        if (!src_b) {
            return;
        }
        i = copy_scanline_avx512(dst, src_b, num_bytes, aligned_dst, &used_stream);
    } else {
        if (!src_a || !src_b) {
            return;
        }
        /* VPMADDUBSW takes signed int8 weights; halve to 0..127 and add odd bit via (a+b)>>1. */
        const uint8_t wa_half = (uint8_t)((256u - weight_b) >> 1);
        const uint8_t wb_half = (uint8_t)(weight_b >> 1);
        const __m512i v_weights =
            _mm512_set1_epi16((short)(((uint16_t)wb_half << 8) | (uint16_t)wa_half));
        const __m512i v_round = _mm512_set1_epi16(64);

        if ((weight_b & 1u) == 0u) {
            for (; num_bytes >= 128 && i <= num_bytes - 128; i += 128) {
                __m512i va0 = _mm512_loadu_si512((const void *)(src_a + i));
                __m512i vb0 = _mm512_loadu_si512((const void *)(src_b + i));
                __m512i va1 = _mm512_loadu_si512((const void *)(src_a + i + 64));
                __m512i vb1 = _mm512_loadu_si512((const void *)(src_b + i + 64));
                __m512i out0 = blend64_even_avx512(va0, vb0, v_weights, v_round);
                __m512i out1 = blend64_even_avx512(va1, vb1, v_weights, v_round);
                store_pair512(dst + i, out0, out1, aligned_dst, &used_stream);
            }
            for (; num_bytes >= 64 && i <= num_bytes - 64; i += 64) {
                __m512i va = _mm512_loadu_si512((const void *)(src_a + i));
                __m512i vb = _mm512_loadu_si512((const void *)(src_b + i));
                __m512i out = blend64_even_avx512(va, vb, v_weights, v_round);
                store_vec512(dst + i, out, aligned_dst, &used_stream);
            }
        } else {
            const __m512i v_ones = _mm512_set1_epi16(0x0101);
            for (; num_bytes >= 128 && i <= num_bytes - 128; i += 128) {
                __m512i va0 = _mm512_loadu_si512((const void *)(src_a + i));
                __m512i vb0 = _mm512_loadu_si512((const void *)(src_b + i));
                __m512i va1 = _mm512_loadu_si512((const void *)(src_a + i + 64));
                __m512i vb1 = _mm512_loadu_si512((const void *)(src_b + i + 64));
                __m512i out0 = blend64_odd_avx512(va0, vb0, v_weights, v_ones, v_round);
                __m512i out1 = blend64_odd_avx512(va1, vb1, v_weights, v_ones, v_round);
                store_pair512(dst + i, out0, out1, aligned_dst, &used_stream);
            }
            for (; num_bytes >= 64 && i <= num_bytes - 64; i += 64) {
                __m512i va = _mm512_loadu_si512((const void *)(src_a + i));
                __m512i vb = _mm512_loadu_si512((const void *)(src_b + i));
                __m512i out = blend64_odd_avx512(va, vb, v_weights, v_ones, v_round);
                store_vec512(dst + i, out, aligned_dst, &used_stream);
            }
        }
    }

    if (i < num_bytes) {
        blend_scanline_scalar(dst + i, src_a + i, src_b + i, weight_b, num_bytes - i);
    }
    if (used_stream) {
        _mm_sfence();
    }
}
#endif
