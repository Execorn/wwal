// SPDX-License-Identifier: GPL-3.0-or-later
#include "simd_blend.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
#include <cpuid.h>
#include <immintrin.h>

/* Isolate _xgetbv under target("xsave") so builds without global -mxsave/-mavx2 stay ABI-valid. */
__attribute__((target("xsave"))) static uint64_t read_xcr0_xsave(void)
{
    return (uint64_t)_xgetbv(0);
}
#endif

static simd_blend_scanline_fn detect_blend_fn(void)
{
#if !defined(WWAL_DISABLE_SIMD)
#if defined(__x86_64__) || defined(_M_X64)
    unsigned int eax = 0u, ebx = 0u, ecx = 0u, edx = 0u;
    if (__get_cpuid(0, &eax, &ebx, &ecx, &edx) == 0 || eax < 7u) {
        return blend_scanline_scalar;
    }
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) == 0) {
        return blend_scanline_scalar;
    }
    /* Require CPUID XSAVE, OSXSAVE, and AVX before executing _xgetbv(0). */
    const unsigned int need_ecx1 = (1u << 26) | (1u << 27) | (1u << 28);
    if ((ecx & need_ecx1) != need_ecx1) {
        return blend_scanline_scalar;
    }

    const uint64_t xcr0 = read_xcr0_xsave();
    /* XCR0 bits [2:1] validate OS XMM/YMM saving; bits [7:5] add opmask and ZMM state. */
    if ((xcr0 & 0x6ULL) != 0x6ULL) {
        return blend_scanline_scalar;
    }

    unsigned int eax7 = 0u, ebx7 = 0u, ecx7 = 0u, edx7 = 0u;
    if (__get_cpuid_count(7, 0, &eax7, &ebx7, &ecx7, &edx7) == 0) {
        return blend_scanline_scalar;
    }

    const unsigned int need_avx512 = (1u << 16) | (1u << 30);
    if ((xcr0 & 0xE6ULL) == 0xE6ULL && (ebx7 & need_avx512) == need_avx512) {
        return blend_scanline_avx512;
    }
    if ((ebx7 & (1u << 5)) != 0u) {
        return blend_scanline_avx2;
    }
#elif defined(__aarch64__) || defined(_M_ARM64)
    return blend_scanline_neon;
#endif
#endif

    return blend_scanline_scalar;
}

simd_blend_scanline_fn simd_get_blend_fn(void)
{
    /* Acquire/release publication prevents data races when workers query before init. */
    static _Atomic(simd_blend_scanline_fn) cached_fn = NULL;
    simd_blend_scanline_fn fn = atomic_load_explicit(&cached_fn, memory_order_acquire);
    if (fn != NULL) {
        return fn;
    }

    fn = detect_blend_fn();
    atomic_store_explicit(&cached_fn, fn, memory_order_release);
    return fn;
}
