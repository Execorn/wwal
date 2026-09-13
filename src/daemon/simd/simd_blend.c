#include "simd_blend.h"
#include "waywal/log.h"

simd_blend_scanline_fn simd_get_blend_fn(void) {
    static simd_blend_scanline_fn cached_fn = NULL;
    if (cached_fn) return cached_fn;

#if defined(__x86_64__) || defined(_M_X64)
    if (__builtin_cpu_supports("avx512f") &&
        __builtin_cpu_supports("avx512bw") &&
        __builtin_cpu_supports("avx512vbmi2")) {
        cached_fn = blend_scanline_avx512;
        return cached_fn;
    }
    if (__builtin_cpu_supports("avx2")) {
        cached_fn = blend_scanline_avx2;
        return cached_fn;
    }
#elif defined(__aarch64__) || defined(_M_ARM64)
    cached_fn = blend_scanline_neon;
    return cached_fn;
#endif

    cached_fn = blend_scanline_scalar;
    return cached_fn;
}
