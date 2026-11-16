// SPDX-License-Identifier: GPL-3.0-or-later
#include "simd_blend.h"

#include <string.h>

static inline uint8_t blend_u8_scalar(uint8_t a, uint8_t b, uint16_t wa, uint16_t wb)
{
    return (uint8_t)(((uint32_t)a * wa + (uint32_t)b * wb + 128u) >> 8);
}

void blend_scanline_scalar(uint8_t *restrict dst, const uint8_t *restrict src_a,
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

    const uint16_t wa = (uint16_t)(256u - weight_b);

    size_t i = 0;
    for (; num_bytes >= 8 && i <= num_bytes - 8; i += 8) {
        dst[i + 0] = blend_u8_scalar(src_a[i + 0], src_b[i + 0], wa, weight_b);
        dst[i + 1] = blend_u8_scalar(src_a[i + 1], src_b[i + 1], wa, weight_b);
        dst[i + 2] = blend_u8_scalar(src_a[i + 2], src_b[i + 2], wa, weight_b);
        dst[i + 3] = blend_u8_scalar(src_a[i + 3], src_b[i + 3], wa, weight_b);
        dst[i + 4] = blend_u8_scalar(src_a[i + 4], src_b[i + 4], wa, weight_b);
        dst[i + 5] = blend_u8_scalar(src_a[i + 5], src_b[i + 5], wa, weight_b);
        dst[i + 6] = blend_u8_scalar(src_a[i + 6], src_b[i + 6], wa, weight_b);
        dst[i + 7] = blend_u8_scalar(src_a[i + 7], src_b[i + 7], wa, weight_b);
    }
    for (; i < num_bytes; ++i) {
        dst[i] = blend_u8_scalar(src_a[i], src_b[i], wa, weight_b);
    }
}
