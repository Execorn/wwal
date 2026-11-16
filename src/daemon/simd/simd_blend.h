// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_SIMD_BLEND_H
#define WAYWAL_SIMD_BLEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#if defined(__GNUC__) || defined(__clang__)
#define WWAL_SIMD_RESTRICT __restrict__
#else
#define WWAL_SIMD_RESTRICT
#endif
#else
#define WWAL_SIMD_RESTRICT restrict
#endif

typedef void (*simd_blend_scanline_fn)(uint8_t *WWAL_SIMD_RESTRICT dst,
                                       const uint8_t *WWAL_SIMD_RESTRICT src_a,
                                       const uint8_t *WWAL_SIMD_RESTRICT src_b, uint16_t weight_b,
                                       size_t num_bytes);

void blend_scanline_scalar(uint8_t *WWAL_SIMD_RESTRICT dst, const uint8_t *WWAL_SIMD_RESTRICT src_a,
                           const uint8_t *WWAL_SIMD_RESTRICT src_b, uint16_t weight_b,
                           size_t num_bytes);

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
void blend_scanline_avx2(uint8_t *WWAL_SIMD_RESTRICT dst, const uint8_t *WWAL_SIMD_RESTRICT src_a,
                         const uint8_t *WWAL_SIMD_RESTRICT src_b, uint16_t weight_b,
                         size_t num_bytes);

void blend_scanline_avx512(uint8_t *WWAL_SIMD_RESTRICT dst, const uint8_t *WWAL_SIMD_RESTRICT src_a,
                           const uint8_t *WWAL_SIMD_RESTRICT src_b, uint16_t weight_b,
                           size_t num_bytes);
#endif

#if !defined(WWAL_DISABLE_SIMD) && (defined(__aarch64__) || defined(_M_ARM64))
void blend_scanline_neon(uint8_t *WWAL_SIMD_RESTRICT dst, const uint8_t *WWAL_SIMD_RESTRICT src_a,
                         const uint8_t *WWAL_SIMD_RESTRICT src_b, uint16_t weight_b,
                         size_t num_bytes);
#endif

#undef WWAL_SIMD_RESTRICT

/* Returns the optimal blend kernel available on the current host CPU */
simd_blend_scanline_fn simd_get_blend_fn(void);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_SIMD_BLEND_H */
