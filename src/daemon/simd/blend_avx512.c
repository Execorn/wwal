#include "simd_blend.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>

__attribute__((target("avx512f,avx512bw,avx512vbmi2"))) void
blend_scanline_avx512(uint8_t *restrict dst, const uint8_t *restrict src_a,
                      const uint8_t *restrict src_b, uint16_t weight_b, size_t num_bytes)
{
    const __m512i wb = _mm512_set1_epi16((short)weight_b);
    const __m512i wa = _mm512_set1_epi16((short)(256 - weight_b));
    const __m512i zero = _mm512_setzero_si512();

    size_t i = 0;
    for (; i + 64 <= num_bytes; i += 64) {
        __m512i a = _mm512_loadu_si512((const __m512i *)(src_a + i));
        __m512i b = _mm512_loadu_si512((const __m512i *)(src_b + i));

        __m512i a_lo = _mm512_unpacklo_epi8(a, zero);
        __m512i a_hi = _mm512_unpackhi_epi8(a, zero);
        __m512i b_lo = _mm512_unpacklo_epi8(b, zero);
        __m512i b_hi = _mm512_unpackhi_epi8(b, zero);

        __m512i r_lo = _mm512_srli_epi16(
            _mm512_add_epi16(_mm512_mullo_epi16(a_lo, wa), _mm512_mullo_epi16(b_lo, wb)), 8);
        __m512i r_hi = _mm512_srli_epi16(
            _mm512_add_epi16(_mm512_mullo_epi16(a_hi, wa), _mm512_mullo_epi16(b_hi, wb)), 8);

        __m512i packed = _mm512_packus_epi16(r_lo, r_hi);

        if (((uintptr_t)(dst + i) & 63) == 0) {
            _mm512_stream_si512((__m512i *)(dst + i), packed);
        } else {
            _mm512_storeu_si512((__m512i *)(dst + i), packed);
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
