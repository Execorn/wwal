// SPDX-License-Identifier: GPL-3.0-or-later
#include "simd_blend.h"

#include <string.h>

void blend_scanline_scalar(uint8_t *restrict dst, const uint8_t *restrict src_a,
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

    const uint16_t wa = 256 - weight_b;

    size_t i = 0;
    for (; i + 8 <= num_bytes; i += 8) {
        dst[i + 0] =
            (uint8_t)(((uint32_t)src_a[i + 0] * wa + (uint32_t)src_b[i + 0] * weight_b) >> 8);
        dst[i + 1] =
            (uint8_t)(((uint32_t)src_a[i + 1] * wa + (uint32_t)src_b[i + 1] * weight_b) >> 8);
        dst[i + 2] =
            (uint8_t)(((uint32_t)src_a[i + 2] * wa + (uint32_t)src_b[i + 2] * weight_b) >> 8);
        dst[i + 3] =
            (uint8_t)(((uint32_t)src_a[i + 3] * wa + (uint32_t)src_b[i + 3] * weight_b) >> 8);
        dst[i + 4] =
            (uint8_t)(((uint32_t)src_a[i + 4] * wa + (uint32_t)src_b[i + 4] * weight_b) >> 8);
        dst[i + 5] =
            (uint8_t)(((uint32_t)src_a[i + 5] * wa + (uint32_t)src_b[i + 5] * weight_b) >> 8);
        dst[i + 6] =
            (uint8_t)(((uint32_t)src_a[i + 6] * wa + (uint32_t)src_b[i + 6] * weight_b) >> 8);
        dst[i + 7] =
            (uint8_t)(((uint32_t)src_a[i + 7] * wa + (uint32_t)src_b[i + 7] * weight_b) >> 8);
    }
    for (; i < num_bytes; ++i) {
        dst[i] = (uint8_t)(((uint32_t)src_a[i] * wa + (uint32_t)src_b[i] * weight_b) >> 8);
    }
}
