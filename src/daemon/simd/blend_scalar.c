#include "simd_blend.h"

void blend_scanline_scalar(
    uint8_t *restrict dst,
    const uint8_t *restrict src_a,
    const uint8_t *restrict src_b,
    uint16_t weight_b,
    size_t num_bytes
) {
    const uint16_t wa = 256 - weight_b;

    for (size_t i = 0; i < num_bytes; ++i) {
        dst[i] = (uint8_t)(((uint32_t)src_a[i] * wa + (uint32_t)src_b[i] * weight_b) >> 8);
    }
}
