// SPDX-License-Identifier: GPL-3.0-or-later
#include "simd_blend.h"

#if !defined(WWAL_DISABLE_SIMD) && (defined(__aarch64__) || defined(_M_ARM64))
#include <arm_neon.h>
#include <string.h>

static inline void blend16_neon(uint8_t *restrict dst, const uint8_t *restrict src_a,
                                const uint8_t *restrict src_b, uint8x8_t wa, uint8x8_t wb)
{
    uint8x16_t a = vld1q_u8(src_a);
    uint8x16_t b = vld1q_u8(src_b);

    uint16x8_t r_lo = vmull_u8(vget_low_u8(a), wa);
    r_lo = vmlal_u8(r_lo, vget_low_u8(b), wb);

    uint16x8_t r_hi = vmull_u8(vget_high_u8(a), wa);
    r_hi = vmlal_u8(r_hi, vget_high_u8(b), wb);

    /* vrshrn_n_u16 adds 128 before shifting by 8, matching scalar nearest rounding. */
    uint8x16_t result = vcombine_u8(vrshrn_n_u16(r_lo, 8), vrshrn_n_u16(r_hi, 8));
    vst1q_u8(dst, result);
}

void blend_scanline_neon(uint8_t *restrict dst, const uint8_t *restrict src_a,
                         const uint8_t *restrict src_b, uint16_t weight_b, size_t num_bytes)
{
    if (num_bytes == 0 || !dst) {
        return;
    }
    if (weight_b == 0) {
        if (src_a) {
            memcpy(dst, src_a, num_bytes);
        }
        return;
    }
    if (weight_b >= 256) {
        if (src_b) {
            memcpy(dst, src_b, num_bytes);
        }
        return;
    }
    if (!src_a || !src_b) {
        return;
    }

    uint8x8_t wb = vdup_n_u8((uint8_t)weight_b);
    uint8x8_t wa = vdup_n_u8((uint8_t)(256u - weight_b));

    size_t i = 0;
    for (; num_bytes >= 32 && i <= num_bytes - 32; i += 32) {
        blend16_neon(dst + i, src_a + i, src_b + i, wa, wb);
        blend16_neon(dst + i + 16, src_a + i + 16, src_b + i + 16, wa, wb);
    }

    for (; num_bytes >= 16 && i <= num_bytes - 16; i += 16) {
        blend16_neon(dst + i, src_a + i, src_b + i, wa, wb);
    }

    if (i < num_bytes) {
        blend_scanline_scalar(dst + i, src_a + i, src_b + i, weight_b, num_bytes - i);
    }
}
#endif
