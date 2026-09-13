#ifndef WAYWAL_SIMD_BLEND_H
#define WAYWAL_SIMD_BLEND_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*simd_blend_scanline_fn)(
    uint8_t *restrict dst,
    const uint8_t *restrict src_a,
    const uint8_t *restrict src_b,
    uint16_t weight_b,
    size_t num_bytes
);

void blend_scanline_scalar(
    uint8_t *restrict dst,
    const uint8_t *restrict src_a,
    const uint8_t *restrict src_b,
    uint16_t weight_b,
    size_t num_bytes
);

#if defined(__x86_64__) || defined(_M_X64)
void blend_scanline_avx2(
    uint8_t *restrict dst,
    const uint8_t *restrict src_a,
    const uint8_t *restrict src_b,
    uint16_t weight_b,
    size_t num_bytes
);

void blend_scanline_avx512(
    uint8_t *restrict dst,
    const uint8_t *restrict src_a,
    const uint8_t *restrict src_b,
    uint16_t weight_b,
    size_t num_bytes
);
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
void blend_scanline_neon(
    uint8_t *restrict dst,
    const uint8_t *restrict src_a,
    const uint8_t *restrict src_b,
    uint16_t weight_b,
    size_t num_bytes
);
#endif

/* Returns the optimal blend kernel available on the current host CPU */
simd_blend_scanline_fn simd_get_blend_fn(void);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_SIMD_BLEND_H */
