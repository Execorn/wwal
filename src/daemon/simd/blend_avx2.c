#include "simd_blend.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>

__attribute__((target("avx2"))) void blend_scanline_avx2(uint8_t *restrict dst,
                                                         const uint8_t *restrict src_a,
                                                         const uint8_t *restrict src_b,
                                                         uint16_t weight_b, size_t num_bytes)
{
    const __m256i wb = _mm256_set1_epi16((short)weight_b);
    const __m256i wa = _mm256_set1_epi16((short)(256 - weight_b));
    const __m256i zero = _mm256_setzero_si256();

    size_t i = 0;
    for (; i + 32 <= num_bytes; i += 32) {
        __m256i a = _mm256_loadu_si256((const __m256i *)(src_a + i));
        __m256i b = _mm256_loadu_si256((const __m256i *)(src_b + i));

        __m256i a_lo = _mm256_unpacklo_epi8(a, zero);
        __m256i a_hi = _mm256_unpackhi_epi8(a, zero);
        __m256i b_lo = _mm256_unpacklo_epi8(b, zero);
        __m256i b_hi = _mm256_unpackhi_epi8(b, zero);

        __m256i r_lo = _mm256_srli_epi16(
            _mm256_add_epi16(_mm256_mullo_epi16(a_lo, wa), _mm256_mullo_epi16(b_lo, wb)), 8);
        __m256i r_hi = _mm256_srli_epi16(
            _mm256_add_epi16(_mm256_mullo_epi16(a_hi, wa), _mm256_mullo_epi16(b_hi, wb)), 8);

        __m256i packed = _mm256_packus_epi16(r_lo, r_hi);

        if (((uintptr_t)(dst + i) & 31) == 0) {
            _mm256_stream_si256((__m256i *)(dst + i), packed);
        } else {
            _mm256_storeu_si256((__m256i *)(dst + i), packed);
        }
    }

    if (i > 0) {
        _mm_sfence();
    }

    if (i < num_bytes) {
        blend_scanline_scalar(dst + i, src_a + i, src_b + i, weight_b, num_bytes - i);
    }
}
#endif
