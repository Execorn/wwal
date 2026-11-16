// SPDX-License-Identifier: GPL-3.0-or-later
#include "simd_blend.h"

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
#include <immintrin.h>
#include <stdbool.h>
#include <stdint.h>

/* Unaligned accesses cast through __m256i_u to satisfy strict alignment under UBSan. */
__attribute__((target("avx2"), always_inline)) static inline void
store_vec256(uint8_t *dst, __m256i v, bool aligned_dst, bool *used_stream)
{
    if (aligned_dst) {
        _mm256_stream_si256((__m256i *)dst, v);
        *used_stream = true;
    } else {
        _mm256_storeu_si256((__m256i_u *)dst, v);
    }
}

__attribute__((target("avx2"), always_inline)) static inline void
store_pair256(uint8_t *dst, __m256i v0, __m256i v1, bool aligned_dst, bool *used_stream)
{
    if (aligned_dst) {
        _mm256_stream_si256((__m256i *)dst, v0);
        _mm256_stream_si256((__m256i *)(dst + 32), v1);
        *used_stream = true;
    } else {
        _mm256_storeu_si256((__m256i_u *)dst, v0);
        _mm256_storeu_si256((__m256i_u *)(dst + 32), v1);
    }
}

__attribute__((target("avx2"), always_inline)) static inline size_t
copy_scanline_avx2(uint8_t *restrict dst, const uint8_t *restrict src, size_t num_bytes,
                   bool aligned_dst, bool *used_stream)
{
    size_t i = 0;
    for (; num_bytes >= 64 && i <= num_bytes - 64; i += 64) {
        __m256i v0 = _mm256_loadu_si256((const __m256i_u *)(src + i));
        __m256i v1 = _mm256_loadu_si256((const __m256i_u *)(src + i + 32));
        store_pair256(dst + i, v0, v1, aligned_dst, used_stream);
    }
    for (; num_bytes >= 32 && i <= num_bytes - 32; i += 32) {
        __m256i v = _mm256_loadu_si256((const __m256i_u *)(src + i));
        store_vec256(dst + i, v, aligned_dst, used_stream);
    }
    return i;
}

__attribute__((target("avx2"), always_inline)) static inline __m256i
blend32_even_avx2(__m256i va, __m256i vb, __m256i v_weights, __m256i v_round)
{
    __m256i ab_lo = _mm256_unpacklo_epi8(va, vb);
    __m256i ab_hi = _mm256_unpackhi_epi8(va, vb);
    __m256i r_lo =
        _mm256_srli_epi16(_mm256_add_epi16(_mm256_maddubs_epi16(ab_lo, v_weights), v_round), 7);
    __m256i r_hi =
        _mm256_srli_epi16(_mm256_add_epi16(_mm256_maddubs_epi16(ab_hi, v_weights), v_round), 7);
    return _mm256_packus_epi16(r_lo, r_hi);
}

__attribute__((target("avx2"), always_inline)) static inline __m256i
blend32_odd_avx2(__m256i va, __m256i vb, __m256i v_weights, __m256i v_ones, __m256i v_round)
{
    __m256i ab_lo = _mm256_unpacklo_epi8(va, vb);
    __m256i ab_hi = _mm256_unpackhi_epi8(va, vb);
    __m256i odd_lo = _mm256_srli_epi16(_mm256_maddubs_epi16(ab_lo, v_ones), 1);
    __m256i odd_hi = _mm256_srli_epi16(_mm256_maddubs_epi16(ab_hi, v_ones), 1);
    __m256i sum_lo = _mm256_add_epi16(_mm256_maddubs_epi16(ab_lo, v_weights), odd_lo);
    __m256i sum_hi = _mm256_add_epi16(_mm256_maddubs_epi16(ab_hi, v_weights), odd_hi);
    __m256i r_lo = _mm256_srli_epi16(_mm256_add_epi16(sum_lo, v_round), 7);
    __m256i r_hi = _mm256_srli_epi16(_mm256_add_epi16(sum_hi, v_round), 7);
    return _mm256_packus_epi16(r_lo, r_hi);
}

__attribute__((target("avx2"))) void blend_scanline_avx2(uint8_t *restrict dst,
                                                         const uint8_t *restrict src_a,
                                                         const uint8_t *restrict src_b,
                                                         uint16_t weight_b, size_t num_bytes)
{
    if (num_bytes == 0 || !dst) {
        return;
    }

    const bool aligned_dst = (((uintptr_t)dst & 31u) == 0u);
    bool used_stream = false;
    size_t i = 0;

    if (weight_b == 0) {
        if (!src_a) {
            return;
        }
        i = copy_scanline_avx2(dst, src_a, num_bytes, aligned_dst, &used_stream);
    } else if (weight_b >= 256) {
        if (!src_b) {
            return;
        }
        i = copy_scanline_avx2(dst, src_b, num_bytes, aligned_dst, &used_stream);
    } else {
        if (!src_a || !src_b) {
            return;
        }
        /* VPMADDUBSW takes signed int8 weights; halve to 0..127 and add odd bit via (a+b)>>1. */
        const uint8_t wa_half = (uint8_t)((256u - weight_b) >> 1);
        const uint8_t wb_half = (uint8_t)(weight_b >> 1);
        const __m256i v_weights =
            _mm256_set1_epi16((short)(((uint16_t)wb_half << 8) | (uint16_t)wa_half));
        const __m256i v_round = _mm256_set1_epi16(64);

        if ((weight_b & 1u) == 0u) {
            for (; num_bytes >= 64 && i <= num_bytes - 64; i += 64) {
                __m256i va0 = _mm256_loadu_si256((const __m256i_u *)(src_a + i));
                __m256i vb0 = _mm256_loadu_si256((const __m256i_u *)(src_b + i));
                __m256i va1 = _mm256_loadu_si256((const __m256i_u *)(src_a + i + 32));
                __m256i vb1 = _mm256_loadu_si256((const __m256i_u *)(src_b + i + 32));
                __m256i out0 = blend32_even_avx2(va0, vb0, v_weights, v_round);
                __m256i out1 = blend32_even_avx2(va1, vb1, v_weights, v_round);
                store_pair256(dst + i, out0, out1, aligned_dst, &used_stream);
            }
            for (; num_bytes >= 32 && i <= num_bytes - 32; i += 32) {
                __m256i va = _mm256_loadu_si256((const __m256i_u *)(src_a + i));
                __m256i vb = _mm256_loadu_si256((const __m256i_u *)(src_b + i));
                __m256i out = blend32_even_avx2(va, vb, v_weights, v_round);
                store_vec256(dst + i, out, aligned_dst, &used_stream);
            }
        } else {
            const __m256i v_ones = _mm256_set1_epi16(0x0101);
            for (; num_bytes >= 64 && i <= num_bytes - 64; i += 64) {
                __m256i va0 = _mm256_loadu_si256((const __m256i_u *)(src_a + i));
                __m256i vb0 = _mm256_loadu_si256((const __m256i_u *)(src_b + i));
                __m256i va1 = _mm256_loadu_si256((const __m256i_u *)(src_a + i + 32));
                __m256i vb1 = _mm256_loadu_si256((const __m256i_u *)(src_b + i + 32));
                __m256i out0 = blend32_odd_avx2(va0, vb0, v_weights, v_ones, v_round);
                __m256i out1 = blend32_odd_avx2(va1, vb1, v_weights, v_ones, v_round);
                store_pair256(dst + i, out0, out1, aligned_dst, &used_stream);
            }
            for (; num_bytes >= 32 && i <= num_bytes - 32; i += 32) {
                __m256i va = _mm256_loadu_si256((const __m256i_u *)(src_a + i));
                __m256i vb = _mm256_loadu_si256((const __m256i_u *)(src_b + i));
                __m256i out = blend32_odd_avx2(va, vb, v_weights, v_ones, v_round);
                store_vec256(dst + i, out, aligned_dst, &used_stream);
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
