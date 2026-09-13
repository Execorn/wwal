#include "simd_blend.h"

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>

void blend_scanline_neon(uint8_t *restrict dst, const uint8_t *restrict src_a,
                         const uint8_t *restrict src_b, uint16_t weight_b, size_t num_bytes)
{
    if (weight_b == 0) {
        memcpy(dst, src_a, num_bytes);
        return;
    }
    if (weight_b >= 256) {
        memcpy(dst, src_b, num_bytes);
        return;
    }

    uint8x8_t wb = vdup_n_u8((uint8_t)weight_b);
    uint8x8_t wa = vdup_n_u8((uint8_t)(256 - weight_b));

    size_t i = 0;
    for (; i + 16 <= num_bytes; i += 16) {
        uint8x16_t a = vld1q_u8(src_a + i);
        uint8x16_t b = vld1q_u8(src_b + i);

        uint8x8_t a_lo = vget_low_u8(a);
        uint8x8_t a_hi = vget_high_u8(a);
        uint8x8_t b_lo = vget_low_u8(b);
        uint8x8_t b_hi = vget_high_u8(b);

        uint16x8_t r_lo = vmull_u8(a_lo, wa);
        r_lo = vmlal_u8(r_lo, b_lo, wb);

        uint16x8_t r_hi = vmull_u8(a_hi, wa);
        r_hi = vmlal_u8(r_hi, b_hi, wb);

        uint8x8_t packed_lo = vshrn_n_u16(r_lo, 8);
        uint8x8_t packed_hi = vshrn_n_u16(r_hi, 8);

        uint8x16_t result = vcombine_u8(packed_lo, packed_hi);
        vst1q_u8(dst + i, result);
    }

    if (i < num_bytes) {
        blend_scanline_scalar(dst + i, src_a + i, src_b + i, weight_b, num_bytes - i);
    }
}
#endif
