// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "waywal/image_loader.h"

#include "waywal/log.h"

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(WWAL_DISABLE_SIMD)
#include <immintrin.h>
#endif
#include <limits.h>
#include <math.h>
#ifdef HAVE_OPENMP
#include <omp.h>
#endif
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#undef image_load
#undef image_load_fd
#undef image_load_from_memory
#undef image_free

enum {
    WWAL_IMG_MAX_DIM = 16384U,
};

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(WWAL_DISABLE_SIMD)
__attribute__((target("avx2"))) static void swizzle_rgba_to_bgra_avx2(uint8_t *pixels,
                                                                      size_t num_bytes)
{
    const __m256i mask = _mm256_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15, 2,
                                          1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
    size_t i = 0;
    for (; i + 128 <= num_bytes; i += 128) {
        __m256i v0 = _mm256_loadu_si256((const __m256i *)(pixels + i));
        __m256i v1 = _mm256_loadu_si256((const __m256i *)(pixels + i + 32));
        __m256i v2 = _mm256_loadu_si256((const __m256i *)(pixels + i + 64));
        __m256i v3 = _mm256_loadu_si256((const __m256i *)(pixels + i + 96));
        v0 = _mm256_shuffle_epi8(v0, mask);
        v1 = _mm256_shuffle_epi8(v1, mask);
        v2 = _mm256_shuffle_epi8(v2, mask);
        v3 = _mm256_shuffle_epi8(v3, mask);
        _mm256_storeu_si256((__m256i *)(pixels + i), v0);
        _mm256_storeu_si256((__m256i *)(pixels + i + 32), v1);
        _mm256_storeu_si256((__m256i *)(pixels + i + 64), v2);
        _mm256_storeu_si256((__m256i *)(pixels + i + 96), v3);
    }
    for (; i + 32 <= num_bytes; i += 32) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(pixels + i));
        v = _mm256_shuffle_epi8(v, mask);
        _mm256_storeu_si256((__m256i *)(pixels + i), v);
    }
    for (; i + 4 <= num_bytes; i += 4) {
        uint32_t px;
        memcpy(&px, pixels + i, 4);
        px = (px & 0xFF00FF00U) | ((px & 0x00FF0000U) >> 16) | ((px & 0x000000FFU) << 16);
        memcpy(pixels + i, &px, 4);
    }
}

__attribute__((target("ssse3"))) static void swizzle_rgba_to_bgra_ssse3(uint8_t *pixels,
                                                                        size_t num_bytes)
{
    const __m128i mask = _mm_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
    size_t i = 0;
    for (; i + 64 <= num_bytes; i += 64) {
        __m128i v0 = _mm_loadu_si128((const __m128i *)(pixels + i));
        __m128i v1 = _mm_loadu_si128((const __m128i *)(pixels + i + 16));
        __m128i v2 = _mm_loadu_si128((const __m128i *)(pixels + i + 32));
        __m128i v3 = _mm_loadu_si128((const __m128i *)(pixels + i + 48));
        v0 = _mm_shuffle_epi8(v0, mask);
        v1 = _mm_shuffle_epi8(v1, mask);
        v2 = _mm_shuffle_epi8(v2, mask);
        v3 = _mm_shuffle_epi8(v3, mask);
        _mm_storeu_si128((__m128i *)(pixels + i), v0);
        _mm_storeu_si128((__m128i *)(pixels + i + 16), v1);
        _mm_storeu_si128((__m128i *)(pixels + i + 32), v2);
        _mm_storeu_si128((__m128i *)(pixels + i + 48), v3);
    }
    for (; i + 16 <= num_bytes; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i *)(pixels + i));
        v = _mm_shuffle_epi8(v, mask);
        _mm_storeu_si128((__m128i *)(pixels + i), v);
    }
    for (; i + 4 <= num_bytes; i += 4) {
        uint32_t px;
        memcpy(&px, pixels + i, 4);
        px = (px & 0xFF00FF00U) | ((px & 0x00FF0000U) >> 16) | ((px & 0x000000FFU) << 16);
        memcpy(pixels + i, &px, 4);
    }
}
#elif defined(__aarch64__) && !defined(WWAL_DISABLE_SIMD)
#include <arm_neon.h>
static void swizzle_rgba_to_bgra_neon(uint8_t *pixels, size_t num_bytes)
{
    size_t i = 0;
    for (; i + 64 <= num_bytes; i += 64) {
        uint8x16x4_t v = vld4q_u8(pixels + i);
        uint8x16_t tmp = v.val[0];
        v.val[0] = v.val[2];
        v.val[2] = tmp;
        vst4q_u8(pixels + i, v);
    }
    for (; i + 4 <= num_bytes; i += 4) {
        uint32_t px;
        memcpy(&px, pixels + i, 4);
        px = (px & 0xFF00FF00U) | ((px & 0x00FF0000U) >> 16) | ((px & 0x000000FFU) << 16);
        memcpy(pixels + i, &px, 4);
    }
}
#endif

static void swizzle_rgba_to_bgra_inplace(uint8_t *pixels, size_t num_bytes)
{
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(WWAL_DISABLE_SIMD)
    if (__builtin_cpu_supports("avx2")) {
        swizzle_rgba_to_bgra_avx2(pixels, num_bytes);
        return;
    }
    if (__builtin_cpu_supports("ssse3")) {
        swizzle_rgba_to_bgra_ssse3(pixels, num_bytes);
        return;
    }
#elif defined(__aarch64__) && !defined(WWAL_DISABLE_SIMD)
    swizzle_rgba_to_bgra_neon(pixels, num_bytes);
    return;
#endif
    for (size_t i = 0; i + 4 <= num_bytes; i += 4) {
        uint32_t px;
        memcpy(&px, pixels + i, 4);
        px = (px & 0xFF00FF00U) | ((px & 0x00FF0000U) >> 16) | ((px & 0x000000FFU) << 16);
        memcpy(pixels + i, &px, 4);
    }
}

static uint8_t *alloc_aligned_rgba(size_t size)
{
    if (size == 0 || size > (size_t)WWAL_IMG_MAX_DIM * WWAL_IMG_MAX_DIM * 4U)
        return NULL;
    void *ptr = NULL;
    size_t aligned_sz = (size + 63U) & ~(size_t)63U;
    if (posix_memalign(&ptr, 64, aligned_sz) != 0 || !ptr)
        return NULL;
    return (uint8_t *)ptr;
}

static bool resolve_symbol(void *handle, const char *name, void *out_fn_ptr)
{
    if (!handle || !name || !out_fn_ptr)
        return false;
    void *sym = dlsym(handle, name);
    if (!sym)
        return false;
    memcpy(out_fn_ptr, &sym, sizeof(void *));
    return true;
}

static void *open_dl_sonames(const char *const *sonames, size_t count)
{
    const int flags = RTLD_LAZY | RTLD_LOCAL | RTLD_NODELETE;
    for (size_t i = 0; i < count; ++i) {
        void *h = dlopen(sonames[i], flags);
        if (h)
            return h;
    }
    return NULL;
}

static void fill_raw_image(raw_image_t *out_img, uint8_t *data, uint32_t w, uint32_t h)
{
    if (!out_img)
        return;
    memset(out_img, 0, sizeof(*out_img));
    out_img->data = data;
    out_img->width = w;
    out_img->height = h;
    out_img->channels = 4;
    out_img->stride = w * 4;
    out_img->size = (size_t)w * (size_t)h * 4;
}

void image_free(uint8_t *pixels) { free(pixels); }

void raw_image_free(raw_image_t *img)
{
    if (!img)
        return;
    free(img->data);
    memset(img, 0, sizeof(*img));
}

/* ============================================================================
 * Lazy libpng16 Loader & Encoder (Zero Direct DT_NEEDED)
 * ============================================================================ */

enum {
    WWAL_PNG_COLOR_TYPE_GRAY = 0,
    WWAL_PNG_COLOR_TYPE_RGB = 2,
    WWAL_PNG_COLOR_TYPE_PALETTE = 3,
    WWAL_PNG_COLOR_TYPE_GRAY_ALPHA = 4,
    WWAL_PNG_COLOR_TYPE_RGBA = 6,
    WWAL_PNG_COMPRESSION_TYPE_DEFAULT = 0,
    WWAL_PNG_FILTER_TYPE_DEFAULT = 0,
    WWAL_PNG_INTERLACE_NONE = 0,
    WWAL_PNG_INFO_tRNS = 0x0010U,
    WWAL_PNG_FILLER_AFTER = 1,
    WWAL_PNG_FILTER_NONE = 0x08,
    WWAL_PNG_FILTER_SUB = 0x10,
};

typedef void (*wwal_png_error_cb_fn)(void *, const char *);
typedef void (*wwal_png_longjmp_fn)(jmp_buf, int);
typedef void (*wwal_png_rw_cb_fn)(void *, uint8_t *, size_t);

typedef uint32_t (*png_access_version_number_fn)(void);
typedef void *(*png_create_struct_fn)(const char *, void *, wwal_png_error_cb_fn,
                                      wwal_png_error_cb_fn);
typedef void *(*png_create_info_struct_fn)(const void *);
typedef void (*png_destroy_read_struct_fn)(void **, void **, void **);
typedef void (*png_destroy_write_struct_fn)(void **, void **);
typedef jmp_buf *(*png_set_longjmp_fn_t)(void *, wwal_png_longjmp_fn, size_t);
typedef void (*png_init_io_fn)(void *, FILE *);
typedef void (*png_set_read_fn_t)(void *, void *, wwal_png_rw_cb_fn);
typedef void *(*png_get_io_ptr_fn)(const void *);
typedef void (*png_error_fn)(const void *, const char *);
typedef void (*png_read_info_fn)(void *, void *);
typedef uint32_t (*png_get_IHDR_fn)(const void *, const void *, uint32_t *, uint32_t *, int *,
                                    int *, int *, int *, int *);
typedef void (*png_void_fn)(void *);
typedef uint32_t (*png_get_valid_fn)(const void *, const void *, uint32_t);
typedef void (*png_set_filler_fn)(void *, uint32_t, int);
typedef int (*png_set_interlace_handling_fn)(void *);
typedef void (*png_read_update_info_fn)(void *, void *);
typedef size_t (*png_get_rowbytes_fn)(const void *, const void *);
typedef void (*png_read_image_fn)(void *, uint8_t **);
typedef void (*png_read_end_fn)(void *, void *);
typedef void (*png_set_compression_level_fn)(void *, int);
typedef void (*png_set_filter_fn)(void *, int, int);
typedef void (*png_set_IHDR_fn)(const void *, void *, uint32_t, uint32_t, int, int, int, int, int);
typedef void (*png_write_info_fn)(void *, void *);
typedef void (*png_write_row_fn)(void *, const uint8_t *);
typedef void (*png_write_end_fn)(void *, void *);

static pthread_once_t g_png_once = PTHREAD_ONCE_INIT;
static void *g_png_handle = NULL;
static char g_png_ver_str[32] = "1.6.0";
static bool g_png_read_ready = false;
static bool g_png_write_ready = false;

static png_access_version_number_fn g_fn_png_access_version_number = NULL;
static png_create_struct_fn g_fn_png_create_read_struct = NULL;
static png_create_struct_fn g_fn_png_create_write_struct = NULL;
static png_create_info_struct_fn g_fn_png_create_info_struct = NULL;
static png_destroy_read_struct_fn g_fn_png_destroy_read_struct = NULL;
static png_destroy_write_struct_fn g_fn_png_destroy_write_struct = NULL;
static png_set_longjmp_fn_t g_fn_png_set_longjmp_fn = NULL;
static png_init_io_fn g_fn_png_init_io = NULL;
static png_set_read_fn_t g_fn_png_set_read_fn = NULL;
static png_get_io_ptr_fn g_fn_png_get_io_ptr = NULL;
static png_error_fn g_fn_png_error = NULL;
static png_read_info_fn g_fn_png_read_info = NULL;
static png_get_IHDR_fn g_fn_png_get_IHDR = NULL;
static png_void_fn g_fn_png_set_scale_16 = NULL;
static png_void_fn g_fn_png_set_palette_to_rgb = NULL;
static png_void_fn g_fn_png_set_expand_gray_1_2_4_to_8 = NULL;
static png_get_valid_fn g_fn_png_get_valid = NULL;
static png_void_fn g_fn_png_set_tRNS_to_alpha = NULL;
static png_void_fn g_fn_png_set_gray_to_rgb = NULL;
static png_set_filler_fn g_fn_png_set_filler = NULL;
static png_void_fn g_fn_png_set_bgr = NULL;
static png_set_interlace_handling_fn g_fn_png_set_interlace_handling = NULL;
static png_read_update_info_fn g_fn_png_read_update_info = NULL;
static png_get_rowbytes_fn g_fn_png_get_rowbytes = NULL;
static png_read_image_fn g_fn_png_read_image = NULL;
static png_read_end_fn g_fn_png_read_end = NULL;
static png_set_compression_level_fn g_fn_png_set_compression_level = NULL;
static png_set_filter_fn g_fn_png_set_filter = NULL;
static png_set_IHDR_fn g_fn_png_set_IHDR = NULL;
static png_write_info_fn g_fn_png_write_info = NULL;
static png_write_row_fn g_fn_png_write_row = NULL;
static png_write_end_fn g_fn_png_write_end = NULL;

static void init_png_lib(void)
{
    static const char *const png_sonames[] = {"libpng16.so.16", "libpng16.so", "libpng.so"};
    g_png_handle = open_dl_sonames(png_sonames, sizeof(png_sonames) / sizeof(png_sonames[0]));
    if (!g_png_handle)
        return;

    if (resolve_symbol(g_png_handle, "png_access_version_number",
                       &g_fn_png_access_version_number) &&
        g_fn_png_access_version_number) {
        uint32_t v = g_fn_png_access_version_number();
        if (v >= 10000U) {
            snprintf(g_png_ver_str, sizeof(g_png_ver_str), "%u.%u.%u", v / 10000U,
                     (v / 100U) % 100U, v % 100U);
        }
    }

    bool core_ok =
        resolve_symbol(g_png_handle, "png_create_info_struct", &g_fn_png_create_info_struct) &&
        resolve_symbol(g_png_handle, "png_set_longjmp_fn", &g_fn_png_set_longjmp_fn) &&
        resolve_symbol(g_png_handle, "png_set_bgr", &g_fn_png_set_bgr);
    if (!core_ok)
        return;

    if (resolve_symbol(g_png_handle, "png_create_read_struct", &g_fn_png_create_read_struct) &&
        resolve_symbol(g_png_handle, "png_destroy_read_struct", &g_fn_png_destroy_read_struct) &&
        resolve_symbol(g_png_handle, "png_set_read_fn", &g_fn_png_set_read_fn) &&
        resolve_symbol(g_png_handle, "png_get_io_ptr", &g_fn_png_get_io_ptr) &&
        resolve_symbol(g_png_handle, "png_error", &g_fn_png_error) &&
        resolve_symbol(g_png_handle, "png_read_info", &g_fn_png_read_info) &&
        resolve_symbol(g_png_handle, "png_get_IHDR", &g_fn_png_get_IHDR) &&
        resolve_symbol(g_png_handle, "png_set_scale_16", &g_fn_png_set_scale_16) &&
        resolve_symbol(g_png_handle, "png_set_palette_to_rgb", &g_fn_png_set_palette_to_rgb) &&
        resolve_symbol(g_png_handle, "png_set_expand_gray_1_2_4_to_8",
                       &g_fn_png_set_expand_gray_1_2_4_to_8) &&
        resolve_symbol(g_png_handle, "png_get_valid", &g_fn_png_get_valid) &&
        resolve_symbol(g_png_handle, "png_set_tRNS_to_alpha", &g_fn_png_set_tRNS_to_alpha) &&
        resolve_symbol(g_png_handle, "png_set_gray_to_rgb", &g_fn_png_set_gray_to_rgb) &&
        resolve_symbol(g_png_handle, "png_set_filler", &g_fn_png_set_filler) &&
        resolve_symbol(g_png_handle, "png_set_interlace_handling",
                       &g_fn_png_set_interlace_handling) &&
        resolve_symbol(g_png_handle, "png_read_update_info", &g_fn_png_read_update_info) &&
        resolve_symbol(g_png_handle, "png_get_rowbytes", &g_fn_png_get_rowbytes) &&
        resolve_symbol(g_png_handle, "png_read_image", &g_fn_png_read_image) &&
        resolve_symbol(g_png_handle, "png_read_end", &g_fn_png_read_end)) {
        g_png_read_ready = true;
    }

    if (resolve_symbol(g_png_handle, "png_create_write_struct", &g_fn_png_create_write_struct) &&
        resolve_symbol(g_png_handle, "png_destroy_write_struct", &g_fn_png_destroy_write_struct) &&
        resolve_symbol(g_png_handle, "png_init_io", &g_fn_png_init_io) &&
        resolve_symbol(g_png_handle, "png_set_compression_level",
                       &g_fn_png_set_compression_level) &&
        resolve_symbol(g_png_handle, "png_set_filter", &g_fn_png_set_filter) &&
        resolve_symbol(g_png_handle, "png_set_IHDR", &g_fn_png_set_IHDR) &&
        resolve_symbol(g_png_handle, "png_write_info", &g_fn_png_write_info) &&
        resolve_symbol(g_png_handle, "png_write_row", &g_fn_png_write_row) &&
        resolve_symbol(g_png_handle, "png_write_end", &g_fn_png_write_end)) {
        g_png_write_ready = true;
    }
}

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
} wwal_png_mem_reader_t;

static void png_mem_read_cb(void *png_ptr, uint8_t *out_bytes, size_t byte_count)
{
    wwal_png_mem_reader_t *r = (wwal_png_mem_reader_t *)g_fn_png_get_io_ptr(png_ptr);
    if (!r || byte_count > r->size - r->pos) {
        g_fn_png_error(png_ptr, "unexpected EOF");
        return;
    }
    memcpy(out_bytes, r->data + r->pos, byte_count);
    r->pos += byte_count;
}

static bool load_png_from_memory(const uint8_t *buf, size_t size, bool want_rgba,
                                 uint8_t **out_pixels, uint32_t *out_w, uint32_t *out_h)
{
    if (!buf || size < 8)
        return false;

    pthread_once(&g_png_once, init_png_lib);
    if (!g_png_read_ready)
        return false;

    void *png = g_fn_png_create_read_struct(g_png_ver_str, NULL, NULL, NULL);
    if (!png)
        return false;

    void *info = g_fn_png_create_info_struct(png);
    if (!info) {
        g_fn_png_destroy_read_struct(&png, NULL, NULL);
        return false;
    }

    uint8_t *volatile pixels = NULL;
    uint8_t **volatile row_pointers = NULL;
    wwal_png_mem_reader_t reader = {.data = buf, .size = size, .pos = 0};

    jmp_buf *jb = g_fn_png_set_longjmp_fn(png, longjmp, sizeof(jmp_buf));
    if (!jb || setjmp(*jb) != 0) {
        g_fn_png_destroy_read_struct(&png, &info, NULL);
        free(pixels);
        free((void *)row_pointers);
        return false;
    }

    g_fn_png_set_read_fn(png, &reader, png_mem_read_cb);
    g_fn_png_read_info(png, info);

    uint32_t width = 0;
    uint32_t height = 0;
    int bit_depth = 0;
    int color_type = 0;

    if (g_fn_png_get_IHDR(png, info, &width, &height, &bit_depth, &color_type, NULL, NULL, NULL) ==
            0 ||
        width == 0 || height == 0 || width > WWAL_IMG_MAX_DIM || height > WWAL_IMG_MAX_DIM) {
        g_fn_png_destroy_read_struct(&png, &info, NULL);
        return false;
    }

    if (bit_depth == 16) {
        g_fn_png_set_scale_16(png);
    }
    if (color_type == WWAL_PNG_COLOR_TYPE_PALETTE) {
        g_fn_png_set_palette_to_rgb(png);
    }
    if (color_type == WWAL_PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        g_fn_png_set_expand_gray_1_2_4_to_8(png);
    }
    bool has_trns = g_fn_png_get_valid(png, info, WWAL_PNG_INFO_tRNS) != 0;
    if (has_trns &&
        (color_type == WWAL_PNG_COLOR_TYPE_RGB || color_type == WWAL_PNG_COLOR_TYPE_GRAY ||
         color_type == WWAL_PNG_COLOR_TYPE_PALETTE)) {
        g_fn_png_set_tRNS_to_alpha(png);
    }
    if (color_type == WWAL_PNG_COLOR_TYPE_GRAY || color_type == WWAL_PNG_COLOR_TYPE_GRAY_ALPHA) {
        g_fn_png_set_gray_to_rgb(png);
    }
    /* Only synthesize opaque alpha when tRNS did not already expand to an alpha channel */
    if (!has_trns &&
        (color_type == WWAL_PNG_COLOR_TYPE_RGB || color_type == WWAL_PNG_COLOR_TYPE_GRAY ||
         color_type == WWAL_PNG_COLOR_TYPE_PALETTE)) {
        g_fn_png_set_filler(png, 0xFFU, WWAL_PNG_FILLER_AFTER);
    }
    if (!want_rgba) {
        /* Normalize to DRM_FORMAT_ARGB8888 little-endian byte order [B, G, R, A] */
        g_fn_png_set_bgr(png);
    }

    (void)g_fn_png_set_interlace_handling(png);
    g_fn_png_read_update_info(png, info);

    size_t rowbytes = g_fn_png_get_rowbytes(png, info);
    size_t expected_rowbytes = (size_t)width * 4U;
    if (rowbytes != expected_rowbytes ||
        (size_t)height > ((size_t)WWAL_IMG_MAX_DIM * WWAL_IMG_MAX_DIM * 4U) / expected_rowbytes) {
        g_fn_png_destroy_read_struct(&png, &info, NULL);
        return false;
    }

    size_t out_bytes = expected_rowbytes * (size_t)height;
    pixels = alloc_aligned_rgba(out_bytes);
    if (!pixels) {
        g_fn_png_destroy_read_struct(&png, &info, NULL);
        return false;
    }

    row_pointers = (uint8_t **)malloc(sizeof(uint8_t *) * (size_t)height);
    if (!row_pointers) {
        free(pixels);
        g_fn_png_destroy_read_struct(&png, &info, NULL);
        return false;
    }

    for (uint32_t y = 0; y < height; ++y) {
        row_pointers[y] = pixels + (size_t)y * rowbytes;
    }
    g_fn_png_read_image(png, row_pointers);
    g_fn_png_read_end(png, NULL);

    free((void *)row_pointers);
    g_fn_png_destroy_read_struct(&png, &info, NULL);

    *out_pixels = pixels;
    *out_w = width;
    *out_h = height;
    return true;
}

/* ============================================================================
 * Lazy libturbojpeg Loader (Zero Direct DT_NEEDED)
 * ============================================================================ */

enum {
    WWAL_TJPF_RGBA = 7,
    WWAL_TJPF_BGRA = 8,
};

typedef void *(*tj_init_decompress_fn)(void);
typedef int (*tj_destroy_fn)(void *);
typedef int (*tj_decompress_header3_fn)(void *, const unsigned char *, unsigned long, int *, int *,
                                        int *, int *);
typedef int (*tj_decompress2_fn)(void *, const unsigned char *, unsigned long, unsigned char *, int,
                                 int, int, int, int);

static pthread_once_t g_tj_once = PTHREAD_ONCE_INIT;
static void *g_tj_handle = NULL;
static tj_init_decompress_fn g_fn_tjInitDecompress = NULL;
static tj_destroy_fn g_fn_tjDestroy = NULL;
static tj_decompress_header3_fn g_fn_tjDecompressHeader3 = NULL;
static tj_decompress2_fn g_fn_tjDecompress2 = NULL;
static bool g_tj_ready = false;

static void init_tj_lib(void)
{
    static const char *const tj_sonames[] = {"libturbojpeg.so.0", "libturbojpeg.so"};
    g_tj_handle = open_dl_sonames(tj_sonames, sizeof(tj_sonames) / sizeof(tj_sonames[0]));
    if (!g_tj_handle)
        return;

    if (resolve_symbol(g_tj_handle, "tjInitDecompress", &g_fn_tjInitDecompress) &&
        resolve_symbol(g_tj_handle, "tjDestroy", &g_fn_tjDestroy) &&
        resolve_symbol(g_tj_handle, "tjDecompressHeader3", &g_fn_tjDecompressHeader3) &&
        resolve_symbol(g_tj_handle, "tjDecompress2", &g_fn_tjDecompress2)) {
        g_tj_ready = true;
    }
}

static bool load_jpeg_from_memory(const uint8_t *jpeg_buf, size_t fsize, bool want_rgba,
                                  uint8_t **out_pixels, uint32_t *out_w, uint32_t *out_h)
{
    if (!jpeg_buf || fsize == 0 || fsize > (size_t)ULONG_MAX)
        return false;

    pthread_once(&g_tj_once, init_tj_lib);
    if (!g_tj_ready)
        return false;

    void *tj = g_fn_tjInitDecompress();
    if (!tj)
        return false;

    int width = 0, height = 0, jpeg_subsamp = 0, jpeg_colorspace = 0;
    if (g_fn_tjDecompressHeader3(tj, jpeg_buf, (unsigned long)fsize, &width, &height, &jpeg_subsamp,
                                 &jpeg_colorspace) != 0 ||
        width <= 0 || height <= 0 || width > (int)WWAL_IMG_MAX_DIM ||
        height > (int)WWAL_IMG_MAX_DIM) {
        g_fn_tjDestroy(tj);
        return false;
    }

    size_t out_size = (size_t)width * (size_t)height * 4U;
    uint8_t *pixels = alloc_aligned_rgba(out_size);
    if (!pixels) {
        g_fn_tjDestroy(tj);
        return false;
    }

    int pixfmt = want_rgba ? WWAL_TJPF_RGBA : WWAL_TJPF_BGRA;
    if (g_fn_tjDecompress2(tj, jpeg_buf, (unsigned long)fsize, pixels, width, 0, height, pixfmt,
                           0) != 0) {
        free(pixels);
        g_fn_tjDestroy(tj);
        return false;
    }

    g_fn_tjDestroy(tj);

    *out_pixels = pixels;
    *out_w = (uint32_t)width;
    *out_h = (uint32_t)height;
    return true;
}

/* ============================================================================
 * Native BMP, PNM, GIF, and Single-Pass QOI Decoders
 * ============================================================================ */

static inline uint16_t read_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint32_t read_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline int32_t read_i32_le(const uint8_t *p) { return (int32_t)read_u32_le(p); }

static inline uint32_t alpha_slot_u32(uint8_t a)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return (uint32_t)a;
#else
    return (uint32_t)a << 24;
#endif
}

static inline uint32_t pack_px_rgb_ahi(uint8_t r, uint8_t g, uint8_t b, uint32_t a_slot,
                                       bool want_rgba)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return want_rgba ? (((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | a_slot)
                     : (((uint32_t)b << 24) | ((uint32_t)g << 16) | ((uint32_t)r << 8) | a_slot);
#else
    return want_rgba ? (a_slot | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r)
                     : (a_slot | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
#endif
}

static inline uint32_t pack_px_native(uint8_t r, uint8_t g, uint8_t b, uint8_t a, bool want_rgba)
{
    return pack_px_rgb_ahi(r, g, b, alpha_slot_u32(a), want_rgba);
}

static inline void unpack_px_native(uint32_t px, uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a,
                                    bool want_rgba)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    *r = want_rgba ? (uint8_t)(px >> 24) : (uint8_t)(px >> 8);
    *g = (uint8_t)(px >> 16);
    *b = want_rgba ? (uint8_t)(px >> 8) : (uint8_t)(px >> 24);
    *a = (uint8_t)px;
#else
    *r = want_rgba ? (uint8_t)px : (uint8_t)(px >> 16);
    *g = (uint8_t)(px >> 8);
    *b = want_rgba ? (uint8_t)(px >> 16) : (uint8_t)px;
    *a = (uint8_t)(px >> 24);
#endif
}

static inline __attribute__((always_inline)) bool
qoi_decode_chunk(const uint8_t **pp, const uint8_t *data_end, uint32_t *dst, size_t *ppx_idx,
                 size_t total_pixels, uint32_t index[64], uint8_t *pr, uint8_t *pg, uint8_t *pb,
                 uint8_t *pa, uint32_t *pa_slot, uint32_t *pa_hash, uint32_t *ppx_u32,
                 bool want_rgba, bool checked)
{
    const uint8_t *p = *pp;
    size_t px_idx = *ppx_idx;
    uint8_t r = *pr, g = *pg, b = *pb, a = *pa;
    uint32_t a_slot = *pa_slot;
    uint32_t a_hash = *pa_hash;
    uint32_t px_u32 = *ppx_u32;

    while (checked ? (px_idx < total_pixels)
                   : (px_idx + 62U <= total_pixels && p + 5 <= data_end)) {
        if (checked && p >= data_end)
            return false;
        uint8_t b1 = *p++;
        switch (b1 >> 6) {
        case 0: {
            px_u32 = index[b1];
            uint8_t new_a = a;
            unpack_px_native(px_u32, &r, &g, &b, &new_a, want_rgba);
            if (new_a != a) {
                a = new_a;
                a_slot = alpha_slot_u32(a);
                a_hash = (uint32_t)a * 11U;
            }
            dst[px_idx++] = px_u32;
            continue;
        }
        case 1:
            r = (uint8_t)(r + ((b1 >> 4) & 0x03U) - 2U);
            g = (uint8_t)(g + ((b1 >> 2) & 0x03U) - 2U);
            b = (uint8_t)(b + (b1 & 0x03U) - 2U);
            break;
        case 2: {
            if (checked && p >= data_end)
                return false;
            uint8_t b2 = *p++;
            int dg = (int)(b1 & 0x3FU) - 32;
            r = (uint8_t)(r + dg + ((int)((b2 >> 4) & 0x0FU) - 8));
            g = (uint8_t)(g + dg);
            b = (uint8_t)(b + dg + ((int)(b2 & 0x0FU) - 8));
            break;
        }
        default:
            if (b1 < 0xFEU) {
                uint32_t count = (uint32_t)(b1 & 0x3FU) + 1U;
                if (checked && (size_t)count > total_pixels - px_idx)
                    return false;
                for (uint32_t k = 0; k < count; ++k) {
                    dst[px_idx + k] = px_u32;
                }
                px_idx += count;
                continue;
            }
            if (b1 == 0xFEU) {
                if (checked && p + 4 > data_end)
                    return false;
                r = p[0];
                g = p[1];
                b = p[2];
                a = p[3];
                a_slot = alpha_slot_u32(a);
                a_hash = (uint32_t)a * 11U;
                p += 4;
            } else {
                if (checked && p + 3 > data_end)
                    return false;
                r = p[0];
                g = p[1];
                b = p[2];
                p += 3;
            }
            break;
        }
        px_u32 = pack_px_rgb_ahi(r, g, b, a_slot, want_rgba);
        index[((uint32_t)r * 3U + (uint32_t)g * 5U + (uint32_t)b * 7U + a_hash) & 63U] = px_u32;
        dst[px_idx++] = px_u32;
    }

    *pp = p;
    *ppx_idx = px_idx;
    *pr = r;
    *pg = g;
    *pb = b;
    *pa = a;
    *pa_slot = a_slot;
    *pa_hash = a_hash;
    *ppx_u32 = px_u32;
    return true;
}

static inline __attribute__((always_inline)) bool
qoi_decode_pixels_impl(const uint8_t *p, const uint8_t *data_end, uint8_t *pixels,
                       size_t total_pixels, bool want_rgba)
{
    /* QOI spec: index starts zeroed whereas the running pixel starts at opaque black (a = 255) */
    uint32_t index[64] = {0};
    uint8_t r = 0, g = 0, b = 0, a = 255;
    uint32_t a_slot = alpha_slot_u32(a);
    uint32_t a_hash = (uint32_t)a * 11U;
    uint32_t px_u32 = pack_px_rgb_ahi(r, g, b, a_slot, want_rgba);
    uint32_t *dst = (uint32_t *)pixels;
    size_t px_idx = 0;

    if (!qoi_decode_chunk(&p, data_end, dst, &px_idx, total_pixels, index, &r, &g, &b, &a, &a_slot,
                          &a_hash, &px_u32, want_rgba, false)) {
        return false;
    }
    return qoi_decode_chunk(&p, data_end, dst, &px_idx, total_pixels, index, &r, &g, &b, &a,
                            &a_slot, &a_hash, &px_u32, want_rgba, true);
}

static bool load_qoi_from_memory(const uint8_t *buf, size_t size, raw_image_t *out_img,
                                 bool want_rgba)
{
    static const uint8_t k_qoi_eof[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    if (!buf || size < 22U || memcmp(buf, "qoif", 4) != 0)
        return false;

    uint32_t w = read_u32_be(buf + 4);
    uint32_t h = read_u32_be(buf + 8);
    uint8_t channels = buf[12];
    uint8_t colorspace = buf[13];

    if (w == 0 || h == 0 || w > WWAL_IMG_MAX_DIM || h > WWAL_IMG_MAX_DIM ||
        (channels != 3U && channels != 4U) || colorspace > 1U) {
        return false;
    }

    size_t total_pixels = (size_t)w * (size_t)h;
    uint8_t *pixels = alloc_aligned_rgba(total_pixels * 4U);
    if (!pixels)
        return false;

    const uint8_t *p = buf + 14;
    const uint8_t *data_end =
        (memcmp(buf + size - 8, k_qoi_eof, 8) == 0) ? (buf + size - 8) : (buf + size);

    bool ok = want_rgba ? qoi_decode_pixels_impl(p, data_end, pixels, total_pixels, true)
                        : qoi_decode_pixels_impl(p, data_end, pixels, total_pixels, false);
    if (!ok) {
        free(pixels);
        return false;
    }

    fill_raw_image(out_img, pixels, w, h);
    return true;
}

static bool load_bmp(FILE *fp, bool want_rgba, uint8_t **out_pixels, uint32_t *out_w,
                     uint32_t *out_h)
{
    uint8_t header[54];
    if (fread(header, 1, 54, fp) != 54)
        return false;
    if (header[0] != 'B' || header[1] != 'M')
        return false;

    uint32_t data_offset = read_u32_le(&header[10]);
    int32_t width = read_i32_le(&header[18]);
    int32_t height = read_i32_le(&header[22]);
    uint16_t bpp = read_u16_le(&header[28]);
    uint32_t compression = read_u32_le(&header[30]);

    if (width <= 0 || height == 0 || width > (int32_t)WWAL_IMG_MAX_DIM ||
        height > (int32_t)WWAL_IMG_MAX_DIM || height < -(int32_t)WWAL_IMG_MAX_DIM ||
        (bpp != 24 && bpp != 32) || compression != 0) {
        return false;
    }

    bool flip_y = (height > 0);
    uint32_t abs_h = (uint32_t)(height > 0 ? height : -height);
    uint32_t abs_w = (uint32_t)width;

    size_t row_stride = ((size_t)abs_w * (bpp / 8U) + 3U) & ~(size_t)3U;
    uint8_t *row_buf = (uint8_t *)malloc(row_stride);
    if (!row_buf)
        return false;

    uint8_t *pixels = alloc_aligned_rgba((size_t)abs_w * abs_h * 4U);
    if (!pixels) {
        free(row_buf);
        return false;
    }

    if (data_offset < 54 || fseek(fp, (long)data_offset, SEEK_SET) != 0) {
        free(row_buf);
        free(pixels);
        return false;
    }

    for (uint32_t i = 0; i < abs_h; ++i) {
        uint32_t y = flip_y ? (abs_h - 1U - i) : i;
        if (fread(row_buf, 1, row_stride, fp) != row_stride) {
            free(row_buf);
            free(pixels);
            return false;
        }

        uint8_t *dst_row = pixels + (size_t)y * abs_w * 4U;
        if (bpp == 32 && !want_rgba) {
            memcpy(dst_row, row_buf, (size_t)abs_w * 4U);
        } else if (bpp == 32) {
            for (uint32_t x = 0; x < abs_w; ++x) {
                dst_row[x * 4 + 0] = row_buf[x * 4 + 2];
                dst_row[x * 4 + 1] = row_buf[x * 4 + 1];
                dst_row[x * 4 + 2] = row_buf[x * 4 + 0];
                dst_row[x * 4 + 3] = row_buf[x * 4 + 3];
            }
        } else if (want_rgba) {
            for (uint32_t x = 0; x < abs_w; ++x) {
                dst_row[x * 4 + 0] = row_buf[x * 3 + 2];
                dst_row[x * 4 + 1] = row_buf[x * 3 + 1];
                dst_row[x * 4 + 2] = row_buf[x * 3 + 0];
                dst_row[x * 4 + 3] = 0xFF;
            }
        } else {
            for (uint32_t x = 0; x < abs_w; ++x) {
                dst_row[x * 4 + 0] = row_buf[x * 3 + 0];
                dst_row[x * 4 + 1] = row_buf[x * 3 + 1];
                dst_row[x * 4 + 2] = row_buf[x * 3 + 2];
                dst_row[x * 4 + 3] = 0xFF;
            }
        }
    }

    free(row_buf);
    *out_pixels = pixels;
    *out_w = abs_w;
    *out_h = abs_h;
    return true;
}

static bool pnm_next_token(FILE *fp, char *tok, size_t cap)
{
    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (c == '#') {
            while ((c = fgetc(fp)) != EOF && c != '\n' && c != '\r') {
            }
            continue;
        }
        if (!isspace((unsigned char)c))
            break;
    }
    if (c == EOF)
        return false;

    size_t i = 0;
    while (c != EOF && !isspace((unsigned char)c) && c != '#') {
        if (i + 1 < cap)
            tok[i++] = (char)c;
        c = fgetc(fp);
    }
    if (c == '#') {
        while ((c = fgetc(fp)) != EOF && c != '\n' && c != '\r') {
        }
    }
    tok[i] = '\0';
    return i > 0;
}

static bool load_pnm(FILE *fp, bool want_rgba, uint8_t **out_pixels, uint32_t *out_w,
                     uint32_t *out_h)
{
    char tok[64];
    if (!pnm_next_token(fp, tok, sizeof(tok)) || tok[0] != 'P')
        return false;
    char fmt = tok[1];
    if ((fmt != '3' && fmt != '5' && fmt != '6') || tok[2] != '\0')
        return false;

    long vals[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        if (!pnm_next_token(fp, tok, sizeof(tok)))
            return false;
        char *endptr = NULL;
        long v = strtol(tok, &endptr, 10);
        if (!endptr || *endptr != '\0' || v <= 0)
            return false;
        vals[i] = v;
    }

    if (vals[0] > (long)WWAL_IMG_MAX_DIM || vals[1] > (long)WWAL_IMG_MAX_DIM || vals[2] > 255)
        return false;

    uint32_t w = (uint32_t)vals[0];
    uint32_t h = (uint32_t)vals[1];
    uint32_t maxval = (uint32_t)vals[2];
    size_t total_px = (size_t)w * (size_t)h;
    uint8_t *pixels = alloc_aligned_rgba(total_px * 4U);
    if (!pixels)
        return false;

    if (fmt == '6') {
        uint8_t *rgb = (uint8_t *)malloc(total_px * 3U);
        if (!rgb || fread(rgb, 1, total_px * 3U, fp) != total_px * 3U) {
            free(rgb);
            free(pixels);
            return false;
        }
        for (size_t i = 0; i < total_px; ++i) {
            uint8_t r = (maxval == 255U) ? rgb[i * 3 + 0]
                                         : (uint8_t)(((uint32_t)rgb[i * 3 + 0] * 255U) / maxval);
            uint8_t g = (maxval == 255U) ? rgb[i * 3 + 1]
                                         : (uint8_t)(((uint32_t)rgb[i * 3 + 1] * 255U) / maxval);
            uint8_t b = (maxval == 255U) ? rgb[i * 3 + 2]
                                         : (uint8_t)(((uint32_t)rgb[i * 3 + 2] * 255U) / maxval);
            pixels[i * 4 + 0] = want_rgba ? r : b;
            pixels[i * 4 + 1] = g;
            pixels[i * 4 + 2] = want_rgba ? b : r;
            pixels[i * 4 + 3] = 0xFF;
        }
        free(rgb);
    } else if (fmt == '5') {
        uint8_t *gray = (uint8_t *)malloc(total_px);
        if (!gray || fread(gray, 1, total_px, fp) != total_px) {
            free(gray);
            free(pixels);
            return false;
        }
        for (size_t i = 0; i < total_px; ++i) {
            uint8_t g = (maxval == 255U) ? gray[i] : (uint8_t)(((uint32_t)gray[i] * 255U) / maxval);
            pixels[i * 4 + 0] = g;
            pixels[i * 4 + 1] = g;
            pixels[i * 4 + 2] = g;
            pixels[i * 4 + 3] = 0xFF;
        }
        free(gray);
    } else {
        for (size_t i = 0; i < total_px; ++i) {
            uint8_t c[3];
            for (int ch = 0; ch < 3; ++ch) {
                if (!pnm_next_token(fp, tok, sizeof(tok))) {
                    free(pixels);
                    return false;
                }
                char *endptr = NULL;
                long v = strtol(tok, &endptr, 10);
                if (!endptr || *endptr != '\0' || v < 0 || v > (long)maxval) {
                    free(pixels);
                    return false;
                }
                c[ch] = (maxval == 255U) ? (uint8_t)v : (uint8_t)(((uint32_t)v * 255U) / maxval);
            }
            pixels[i * 4 + 0] = want_rgba ? c[0] : c[2];
            pixels[i * 4 + 1] = c[1];
            pixels[i * 4 + 2] = want_rgba ? c[2] : c[0];
            pixels[i * 4 + 3] = 0xFF;
        }
    }

    *out_pixels = pixels;
    *out_w = w;
    *out_h = h;
    return true;
}

typedef struct {
    FILE *fp;
    uint32_t bit_buf;
    int bit_count;
    int sub_len;
    bool eof;
} gif_bit_reader_t;

static int gif_read_bits(gif_bit_reader_t *br, int num_bits)
{
    while (br->bit_count < num_bits) {
        if (br->sub_len == 0) {
            int len = fgetc(br->fp);
            if (len <= 0) {
                br->eof = true;
                return -1;
            }
            br->sub_len = len;
        }
        int b = fgetc(br->fp);
        if (b == EOF) {
            br->eof = true;
            return -1;
        }
        br->sub_len--;
        br->bit_buf |= ((uint32_t)b << br->bit_count);
        br->bit_count += 8;
    }
    int code = (int)(br->bit_buf & ((1U << num_bits) - 1U));
    br->bit_buf >>= num_bits;
    br->bit_count -= num_bits;
    return code;
}

static bool load_gif(FILE *fp, bool want_rgba, uint8_t **out_pixels, uint32_t *out_w,
                     uint32_t *out_h)
{
    uint8_t header[13];
    if (fread(header, 1, 13, fp) != 13)
        return false;

    if (strncmp((const char *)header, "GIF87a", 6) != 0 &&
        strncmp((const char *)header, "GIF89a", 6) != 0) {
        return false;
    }

    uint16_t screen_w = read_u16_le(&header[6]);
    uint16_t screen_h = read_u16_le(&header[8]);
    uint8_t lsd_packed = header[10];
    uint8_t bg_color_idx = header[11];

    bool has_gct = (lsd_packed & 0x80U) != 0;
    int gct_entries = has_gct ? (1 << ((lsd_packed & 0x07U) + 1U)) : 0;
    uint8_t gct[256][3];
    memset(gct, 0, sizeof(gct));

    if (has_gct && gct_entries > 0) {
        if (fread(gct, 3, (size_t)gct_entries, fp) != (size_t)gct_entries)
            return false;
    }

    bool has_trans = false;
    uint8_t trans_idx = 0;

    while (true) {
        int block_type = fgetc(fp);
        if (block_type == EOF || block_type == 0x3B)
            break;

        if (block_type == 0x21) {
            int label = fgetc(fp);
            if (label == EOF)
                break;

            if (label == 0xF9) {
                int ext_len = fgetc(fp);
                if (ext_len != 4)
                    return false;
                uint8_t gce[4];
                if (fread(gce, 1, 4, fp) != 4)
                    return false;
                has_trans = (gce[0] & 0x01U) != 0;
                trans_idx = gce[3];
                int term = fgetc(fp);
                while (term > 0 && term != EOF) {
                    if (fseek(fp, term, SEEK_CUR) != 0)
                        return false;
                    term = fgetc(fp);
                }
            } else {
                int sub_len;
                while ((sub_len = fgetc(fp)) > 0) {
                    if (fseek(fp, sub_len, SEEK_CUR) != 0)
                        return false;
                }
            }
            continue;
        }

        if (block_type == 0x2C) {
            uint8_t desc[9];
            if (fread(desc, 1, 9, fp) != 9)
                return false;

            uint16_t img_left = read_u16_le(&desc[0]);
            uint16_t img_top = read_u16_le(&desc[2]);
            uint16_t img_w = read_u16_le(&desc[4]);
            uint16_t img_h = read_u16_le(&desc[6]);
            uint8_t img_packed = desc[8];

            if (img_w == 0 || img_h == 0 || img_w > WWAL_IMG_MAX_DIM || img_h > WWAL_IMG_MAX_DIM)
                return false;

            uint32_t canvas_w = screen_w > 0 ? (uint32_t)screen_w : (uint32_t)img_w;
            uint32_t canvas_h = screen_h > 0 ? (uint32_t)screen_h : (uint32_t)img_h;

            if (canvas_w == 0 || canvas_h == 0 || canvas_w > WWAL_IMG_MAX_DIM ||
                canvas_h > WWAL_IMG_MAX_DIM)
                return false;

            if ((uint32_t)img_left + img_w > canvas_w)
                canvas_w = (uint32_t)img_left + img_w;
            if ((uint32_t)img_top + img_h > canvas_h)
                canvas_h = (uint32_t)img_top + img_h;
            if (canvas_w > WWAL_IMG_MAX_DIM || canvas_h > WWAL_IMG_MAX_DIM)
                return false;

            bool has_lct = (img_packed & 0x80U) != 0;
            bool interlace = (img_packed & 0x40U) != 0;
            int lct_entries = has_lct ? (1 << ((img_packed & 0x07U) + 1U)) : 0;
            uint8_t lct[256][3];
            memset(lct, 0, sizeof(lct));

            if (has_lct && lct_entries > 0) {
                if (fread(lct, 3, (size_t)lct_entries, fp) != (size_t)lct_entries)
                    return false;
            }

            uint8_t (*active_pal)[3] = has_lct ? lct : gct;

            uint32_t u32_palette[256];
            for (int i = 0; i < 256; ++i) {
                if (has_trans && i == (int)trans_idx) {
                    u32_palette[i] = 0x00000000U;
                } else {
                    u32_palette[i] = pack_px_native(active_pal[i][0], active_pal[i][1],
                                                    active_pal[i][2], 0xFF, want_rgba);
                }
            }

            uint8_t *canvas_bytes = alloc_aligned_rgba((size_t)canvas_w * canvas_h * 4U);
            if (!canvas_bytes)
                return false;
            memset(canvas_bytes, 0, (size_t)canvas_w * canvas_h * 4U);
            uint32_t *canvas = (uint32_t *)canvas_bytes;

            if (!has_trans) {
                uint32_t bg_val = u32_palette[bg_color_idx];
                for (size_t i = 0; i < (size_t)canvas_w * canvas_h; ++i) {
                    canvas[i] = bg_val;
                }
            }

            uint32_t *row_map = (uint32_t *)malloc(sizeof(uint32_t) * img_h);
            if (!row_map) {
                free(canvas_bytes);
                return false;
            }

            if (!interlace) {
                for (uint32_t r = 0; r < img_h; ++r)
                    row_map[r] = r;
            } else {
                uint32_t cur = 0;
                for (uint32_t r = 0; r < img_h; r += 8)
                    row_map[cur++] = r;
                for (uint32_t r = 4; r < img_h; r += 8)
                    row_map[cur++] = r;
                for (uint32_t r = 2; r < img_h; r += 4)
                    row_map[cur++] = r;
                for (uint32_t r = 1; r < img_h; r += 2)
                    row_map[cur++] = r;
            }

            int min_code_size = fgetc(fp);
            if (min_code_size < 2 || min_code_size > 11) {
                free(row_map);
                free(canvas_bytes);
                return false;
            }

            int clear_code = 1 << min_code_size;
            int end_code = clear_code + 1;
            int code_size = min_code_size + 1;
            int max_code = 1 << code_size;
            int next_code = end_code + 1;
            int old_code = -1;

            uint16_t prefix[4096];
            uint8_t suffix[4096];
            uint8_t stack[4096];
            memset(prefix, 0, sizeof(prefix));
            memset(suffix, 0, sizeof(suffix));
            int stack_ptr = 0;

            for (int i = 0; i < clear_code; ++i) {
                prefix[i] = 4096;
                suffix[i] = (uint8_t)i;
            }

            gif_bit_reader_t br = {
                .fp = fp,
                .bit_buf = 0,
                .bit_count = 0,
                .sub_len = 0,
                .eof = false,
            };

            size_t total_pixels = (size_t)img_w * img_h;
            size_t pixels_written = 0;
            uint32_t px = 0;
            uint32_t py = 0;
            uint32_t *dst_row = canvas + ((size_t)img_top + row_map[0]) * canvas_w + img_left;
            bool lzw_err = false;

            while (pixels_written < total_pixels) {
                int code = gif_read_bits(&br, code_size);
                if (code < 0 || code == end_code)
                    break;

                if (code == clear_code) {
                    code_size = min_code_size + 1;
                    max_code = 1 << code_size;
                    next_code = end_code + 1;
                    old_code = -1;
                    continue;
                }

                if (old_code == -1) {
                    /* First code after a clear must be a root literal (< clear_code) */
                    if (code >= clear_code) {
                        lzw_err = true;
                        break;
                    }
                    old_code = code;
                    dst_row[px] = u32_palette[(uint8_t)code];
                    px++;
                    if (px == img_w) {
                        px = 0;
                        py++;
                        if (py < img_h)
                            dst_row =
                                canvas + ((size_t)img_top + row_map[py]) * canvas_w + img_left;
                    }
                    pixels_written++;
                    continue;
                }

                int in_code = code;
                int first_char = 0;
                stack_ptr = 0;

                if (code == next_code) {
                    int kwkwk_slot = stack_ptr++;
                    stack[kwkwk_slot] = 0;
                    int cur = old_code;
                    bool chain_err = false;
                    while (cur >= clear_code) {
                        if (cur >= 4096 || stack_ptr >= 4095 || (int)prefix[cur] >= cur) {
                            chain_err = true;
                            break;
                        }
                        stack[stack_ptr++] = suffix[cur];
                        cur = prefix[cur];
                    }
                    if (chain_err) {
                        lzw_err = true;
                        break;
                    }
                    first_char = cur;
                    stack[stack_ptr++] = (uint8_t)cur;
                    stack[kwkwk_slot] = (uint8_t)first_char;
                } else if (code < next_code) {
                    int cur = code;
                    bool chain_err = false;
                    while (cur >= clear_code) {
                        if (cur >= 4096 || stack_ptr >= 4095 || (int)prefix[cur] >= cur) {
                            chain_err = true;
                            break;
                        }
                        stack[stack_ptr++] = suffix[cur];
                        cur = prefix[cur];
                    }
                    if (chain_err) {
                        lzw_err = true;
                        break;
                    }
                    first_char = cur;
                    stack[stack_ptr++] = (uint8_t)cur;
                } else {
                    lzw_err = true;
                    break;
                }

                if (next_code < 4096) {
                    prefix[next_code] = (uint16_t)old_code;
                    suffix[next_code] = (uint8_t)first_char;
                    next_code++;
                    if (next_code >= max_code && code_size < 12) {
                        code_size++;
                        max_code = 1 << code_size;
                    }
                }
                old_code = in_code;

                while (stack_ptr > 0 && pixels_written < total_pixels) {
                    uint8_t c = stack[--stack_ptr];
                    dst_row[px] = u32_palette[c];
                    px++;
                    if (px == img_w) {
                        px = 0;
                        py++;
                        if (py < img_h)
                            dst_row =
                                canvas + ((size_t)img_top + row_map[py]) * canvas_w + img_left;
                    }
                    pixels_written++;
                }
            }

            if (lzw_err || pixels_written == 0) {
                free(row_map);
                free(canvas_bytes);
                return false;
            }

            while (br.sub_len > 0) {
                fgetc(fp);
                br.sub_len--;
            }
            int sub_len;
            while ((sub_len = fgetc(fp)) > 0) {
                if (fseek(fp, sub_len, SEEK_CUR) != 0)
                    break;
            }

            free(row_map);

            *out_pixels = canvas_bytes;
            *out_w = canvas_w;
            *out_h = canvas_h;
            return true;
        }
    }

    return false;
}

/* ============================================================================
 * Lazy libwebp + libwebpdemux Loader & Multi-Threaded YUV420->BGRA/RGBA
 * ============================================================================ */

enum {
    WWAL_WEBP_DECODER_ABI_VERSION = 0x0210,
    WWAL_WEBP_DEMUX_ABI_VERSION = 0x0107,
    WWAL_WEBP_MODE_RGBA = 1,
    WWAL_WEBP_MODE_BGRA = 3,
    WWAL_WEBP_MODE_YUV = 11,
    WWAL_WEBP_MODE_YUVA = 12,
    WWAL_WEBP_FF_CANVAS_WIDTH = 1,
    WWAL_WEBP_FF_CANVAS_HEIGHT = 2,
};

typedef struct {
    int width;
    int height;
    int has_alpha;
    int has_animation;
    int format;
    uint32_t pad[5];
} wwal_webp_bitstream_features_t;

typedef struct {
    uint8_t *rgba;
    int stride;
    size_t size;
} wwal_webp_rgba_buffer_t;

typedef struct {
    uint8_t *y, *u, *v, *a;
    int y_stride;
    int u_stride, v_stride;
    int a_stride;
    size_t y_size;
    size_t u_size, v_size;
    size_t a_size;
} wwal_webp_yuva_buffer_t;

typedef struct {
    int colorspace;
    int width, height;
    int is_external_memory;
    union {
        wwal_webp_rgba_buffer_t RGBA;
        wwal_webp_yuva_buffer_t YUVA;
    } u;
    uint32_t pad[4];
    uint8_t *private_memory;
} wwal_webp_dec_buffer_t;

typedef struct {
    int bypass_filtering;
    int no_fancy_upsampling;
    int use_cropping;
    int crop_left, crop_top;
    int crop_width, crop_height;
    int use_scaling;
    int scaled_width, scaled_height;
    int use_threads;
    int dithering_strength;
    int flip;
    int alpha_dithering_strength;
    uint32_t pad[5];
} wwal_webp_decoder_options_t;

typedef struct {
    wwal_webp_bitstream_features_t input;
    wwal_webp_dec_buffer_t output;
    wwal_webp_decoder_options_t options;
} wwal_webp_decoder_config_t;

typedef struct {
    const uint8_t *bytes;
    size_t size;
} wwal_webp_data_t;

typedef struct {
    int frame_num;
    int num_frames;
    int x_offset, y_offset;
    int width, height;
    int duration;
    int dispose_method;
    int complete;
    wwal_webp_data_t fragment;
    int has_alpha;
    int blend_method;
    uint32_t pad[2];
    void *private_;
} wwal_webp_iterator_t;

typedef int (*webp_init_dec_cfg_internal_fn)(wwal_webp_decoder_config_t *, int);
typedef int (*webp_get_features_internal_fn)(const uint8_t *, size_t,
                                             wwal_webp_bitstream_features_t *, int);
typedef int (*webp_decode_fn)(const uint8_t *, size_t, wwal_webp_decoder_config_t *);
typedef void (*webp_free_dec_buf_fn)(wwal_webp_dec_buffer_t *);

typedef void *(*webp_demux_internal_fn)(const wwal_webp_data_t *, int, int *, int);
typedef uint32_t (*webp_demux_get_i_fn)(const void *, int);
typedef int (*webp_demux_get_frame_fn)(const void *, int, wwal_webp_iterator_t *);
typedef void (*webp_demux_release_iter_fn)(wwal_webp_iterator_t *);
typedef void (*webp_demux_delete_fn)(void *);

static pthread_once_t g_webp_once = PTHREAD_ONCE_INIT;
static void *g_webp_handle = NULL;
static void *g_webpdemux_handle = NULL;
static webp_init_dec_cfg_internal_fn g_fn_WebPInitDecoderConfigInternal = NULL;
static webp_get_features_internal_fn g_fn_WebPGetFeaturesInternal = NULL;
static webp_decode_fn g_fn_WebPDecode = NULL;
static webp_free_dec_buf_fn g_fn_WebPFreeDecBuffer = NULL;
static webp_demux_internal_fn g_fn_WebPDemuxInternal = NULL;
static webp_demux_get_i_fn g_fn_WebPDemuxGetI = NULL;
static webp_demux_get_frame_fn g_fn_WebPDemuxGetFrame = NULL;
static webp_demux_release_iter_fn g_fn_WebPDemuxReleaseIterator = NULL;
static webp_demux_delete_fn g_fn_WebPDemuxDelete = NULL;
static bool g_webp_ready = false;
static bool g_webpdemux_ready = false;

static void init_webp_lib(void)
{
#ifndef WWAL_DISABLE_WEBP
    static const char *const webp_sonames[] = {"libwebp.so.7", "libwebp.so"};
    static const char *const demux_sonames[] = {"libwebpdemux.so.2", "libwebpdemux.so"};

    g_webp_handle = open_dl_sonames(webp_sonames, sizeof(webp_sonames) / sizeof(webp_sonames[0]));
    if (!g_webp_handle)
        return;

    if (resolve_symbol(g_webp_handle, "WebPInitDecoderConfigInternal",
                       &g_fn_WebPInitDecoderConfigInternal) &&
        resolve_symbol(g_webp_handle, "WebPGetFeaturesInternal", &g_fn_WebPGetFeaturesInternal) &&
        resolve_symbol(g_webp_handle, "WebPDecode", &g_fn_WebPDecode) &&
        resolve_symbol(g_webp_handle, "WebPFreeDecBuffer", &g_fn_WebPFreeDecBuffer)) {
        g_webp_ready = true;
    }

    g_webpdemux_handle =
        open_dl_sonames(demux_sonames, sizeof(demux_sonames) / sizeof(demux_sonames[0]));
    if (g_webpdemux_handle &&
        resolve_symbol(g_webpdemux_handle, "WebPDemuxInternal", &g_fn_WebPDemuxInternal) &&
        resolve_symbol(g_webpdemux_handle, "WebPDemuxGetI", &g_fn_WebPDemuxGetI) &&
        resolve_symbol(g_webpdemux_handle, "WebPDemuxGetFrame", &g_fn_WebPDemuxGetFrame) &&
        resolve_symbol(g_webpdemux_handle, "WebPDemuxReleaseIterator",
                       &g_fn_WebPDemuxReleaseIterator) &&
        resolve_symbol(g_webpdemux_handle, "WebPDemuxDelete", &g_fn_WebPDemuxDelete)) {
        g_webpdemux_ready = true;
    }
#else
    (void)g_webp_handle;
    (void)g_webpdemux_handle;
#endif
}

static inline uint8_t clamp_u8_i32(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

static inline void yuv_pixel_to_packed(uint8_t y_val, uint8_t u_val, uint8_t v_val, uint8_t a_val,
                                       bool want_rgba, uint8_t *dst)
{
    /* Exact libwebp BT.601 fixed-point matrix matching src/dsp/yuv.h */
    int yy = 19077 * ((int)y_val - 16);
    int du = (int)u_val - 128;
    int dv = (int)v_val - 128;
    uint8_t r = clamp_u8_i32((yy + 26149 * dv + 8192) >> 14);
    uint8_t g = clamp_u8_i32((yy - 6419 * du - 13320 * dv + 8192) >> 14);
    uint8_t b = clamp_u8_i32((yy + 33050 * du + 8192) >> 14);
    if (want_rgba) {
        dst[0] = r;
        dst[1] = g;
        dst[2] = b;
    } else {
        dst[0] = b;
        dst[1] = g;
        dst[2] = r;
    }
    dst[3] = a_val;
}

static void convert_yuv420_row_scalar(const uint8_t *y_row, const uint8_t *u_near,
                                      const uint8_t *u_far, const uint8_t *v_near,
                                      const uint8_t *v_far, const uint8_t *a_row, uint8_t *dst_row,
                                      uint32_t x_start, uint32_t x_end, uint32_t cw, bool want_rgba)
{
    for (uint32_t x = x_start; x < x_end; ++x) {
        uint32_t cx = x >> 1U;
        uint32_t cx_far = cx;
        if (x > 0 && cw > 1) {
            if ((x & 1U) == 0) {
                cx_far = cx - 1U;
            } else if (cx + 1U < cw) {
                cx_far = cx + 1U;
            }
        }
        uint32_t u_val =
            (9U * u_near[cx] + 3U * u_near[cx_far] + 3U * u_far[cx] + u_far[cx_far] + 8U) >> 4U;
        uint32_t v_val =
            (9U * v_near[cx] + 3U * v_near[cx_far] + 3U * v_far[cx] + v_far[cx_far] + 8U) >> 4U;
        uint8_t a_val = a_row ? a_row[x] : 0xFFU;
        yuv_pixel_to_packed(y_row[x], (uint8_t)u_val, (uint8_t)v_val, a_val, want_rgba,
                            dst_row + (size_t)x * 4U);
    }
}

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(WWAL_DISABLE_SIMD)
__attribute__((target("avx2"))) static void
convert_yuv420_row_avx2(const uint8_t *y_row, const uint8_t *u_near, const uint8_t *u_far,
                        const uint8_t *v_near, const uint8_t *v_far, const uint8_t *a_row,
                        uint8_t *dst_row, uint32_t w, uint32_t cw, bool want_rgba)
{
    uint32_t x = 0;
    if (cw > 18U) {
        convert_yuv420_row_scalar(y_row, u_near, u_far, v_near, v_far, a_row, dst_row, 0, 2, cw,
                                  want_rgba);
        x = 2;

        const __m256i c16 = _mm256_set1_epi16(16);
        const __m256i c128 = _mm256_set1_epi16(128);
        const __m256i c32 = _mm256_set1_epi16(32);
        const __m256i kY = _mm256_set1_epi16(19077);
        const __m256i kVToR = _mm256_set1_epi16(26149);
        const __m256i kUToG = _mm256_set1_epi16(6419);
        const __m256i kVToG = _mm256_set1_epi16(13320);
        /* 33050 exceeds signed int16_t max (32767), so multiply by 16525 and double the result */
        const __m256i kUToB_half = _mm256_set1_epi16(16525);
        const __m256i alpha_ff = _mm256_set1_epi8((char)0xFF);

        uint32_t cx = 1U;
        uint32_t cx_limit = cw - 17U;
        for (; cx <= cx_limit; cx += 16U, x += 32U) {
            __m128i un_c = _mm_loadu_si128((const __m128i *)(u_near + cx));
            __m128i un_l = _mm_loadu_si128((const __m128i *)(u_near + cx - 1U));
            __m128i un_r = _mm_loadu_si128((const __m128i *)(u_near + cx + 1U));
            __m128i uf_c = _mm_loadu_si128((const __m128i *)(u_far + cx));
            __m128i uf_l = _mm_loadu_si128((const __m128i *)(u_far + cx - 1U));
            __m128i uf_r = _mm_loadu_si128((const __m128i *)(u_far + cx + 1U));

            __m128i uv_c = _mm_avg_epu8(un_c, _mm_avg_epu8(un_c, uf_c));
            __m128i uv_l = _mm_avg_epu8(un_l, _mm_avg_epu8(un_l, uf_l));
            __m128i uv_r = _mm_avg_epu8(un_r, _mm_avg_epu8(un_r, uf_r));
            __m128i u_even = _mm_avg_epu8(uv_c, _mm_avg_epu8(uv_c, uv_l));
            __m128i u_odd = _mm_avg_epu8(uv_c, _mm_avg_epu8(uv_c, uv_r));
            __m128i u_px_lo = _mm_unpacklo_epi8(u_even, u_odd);
            __m128i u_px_hi = _mm_unpackhi_epi8(u_even, u_odd);

            __m128i vn_c = _mm_loadu_si128((const __m128i *)(v_near + cx));
            __m128i vn_l = _mm_loadu_si128((const __m128i *)(v_near + cx - 1U));
            __m128i vn_r = _mm_loadu_si128((const __m128i *)(v_near + cx + 1U));
            __m128i vf_c = _mm_loadu_si128((const __m128i *)(v_far + cx));
            __m128i vf_l = _mm_loadu_si128((const __m128i *)(v_far + cx - 1U));
            __m128i vf_r = _mm_loadu_si128((const __m128i *)(v_far + cx + 1U));

            __m128i vv_c = _mm_avg_epu8(vn_c, _mm_avg_epu8(vn_c, vf_c));
            __m128i vv_l = _mm_avg_epu8(vn_l, _mm_avg_epu8(vn_l, vf_l));
            __m128i vv_r = _mm_avg_epu8(vn_r, _mm_avg_epu8(vn_r, vf_r));
            __m128i v_even = _mm_avg_epu8(vv_c, _mm_avg_epu8(vv_c, vv_l));
            __m128i v_odd = _mm_avg_epu8(vv_c, _mm_avg_epu8(vv_c, vv_r));
            __m128i v_px_lo = _mm_unpacklo_epi8(v_even, v_odd);
            __m128i v_px_hi = _mm_unpackhi_epi8(v_even, v_odd);

            __m128i y_px_lo = _mm_loadu_si128((const __m128i *)(y_row + x));
            __m128i y_px_hi = _mm_loadu_si128((const __m128i *)(y_row + x + 16U));

            __m256i yy_lo =
                _mm256_slli_epi16(_mm256_sub_epi16(_mm256_cvtepu8_epi16(y_px_lo), c16), 7);
            __m256i uu_lo =
                _mm256_slli_epi16(_mm256_sub_epi16(_mm256_cvtepu8_epi16(u_px_lo), c128), 7);
            __m256i vv_lo =
                _mm256_slli_epi16(_mm256_sub_epi16(_mm256_cvtepu8_epi16(v_px_lo), c128), 7);

            __m256i y64_lo = _mm256_mulhrs_epi16(yy_lo, kY);
            __m256i rv_lo = _mm256_mulhrs_epi16(vv_lo, kVToR);
            __m256i gu_lo = _mm256_mulhrs_epi16(uu_lo, kUToG);
            __m256i gv_lo = _mm256_mulhrs_epi16(vv_lo, kVToG);
            __m256i bu_lo_h = _mm256_mulhrs_epi16(uu_lo, kUToB_half);
            __m256i bu_lo = _mm256_add_epi16(bu_lo_h, bu_lo_h);

            __m256i r_lo =
                _mm256_srai_epi16(_mm256_add_epi16(_mm256_add_epi16(y64_lo, rv_lo), c32), 6);
            __m256i g_lo = _mm256_srai_epi16(
                _mm256_add_epi16(_mm256_sub_epi16(_mm256_sub_epi16(y64_lo, gu_lo), gv_lo), c32), 6);
            __m256i b_lo =
                _mm256_srai_epi16(_mm256_add_epi16(_mm256_add_epi16(y64_lo, bu_lo), c32), 6);

            __m256i yy_hi =
                _mm256_slli_epi16(_mm256_sub_epi16(_mm256_cvtepu8_epi16(y_px_hi), c16), 7);
            __m256i uu_hi =
                _mm256_slli_epi16(_mm256_sub_epi16(_mm256_cvtepu8_epi16(u_px_hi), c128), 7);
            __m256i vv_hi =
                _mm256_slli_epi16(_mm256_sub_epi16(_mm256_cvtepu8_epi16(v_px_hi), c128), 7);

            __m256i y64_hi = _mm256_mulhrs_epi16(yy_hi, kY);
            __m256i rv_hi = _mm256_mulhrs_epi16(vv_hi, kVToR);
            __m256i gu_hi = _mm256_mulhrs_epi16(uu_hi, kUToG);
            __m256i gv_hi = _mm256_mulhrs_epi16(vv_hi, kVToG);
            __m256i bu_hi_h = _mm256_mulhrs_epi16(uu_hi, kUToB_half);
            __m256i bu_hi = _mm256_add_epi16(bu_hi_h, bu_hi_h);

            __m256i r_hi =
                _mm256_srai_epi16(_mm256_add_epi16(_mm256_add_epi16(y64_hi, rv_hi), c32), 6);
            __m256i g_hi = _mm256_srai_epi16(
                _mm256_add_epi16(_mm256_sub_epi16(_mm256_sub_epi16(y64_hi, gu_hi), gv_hi), c32), 6);
            __m256i b_hi =
                _mm256_srai_epi16(_mm256_add_epi16(_mm256_add_epi16(y64_hi, bu_hi), c32), 6);

            __m256i r8 = _mm256_permute4x64_epi64(_mm256_packus_epi16(r_lo, r_hi), 0xD8);
            __m256i g8 = _mm256_permute4x64_epi64(_mm256_packus_epi16(g_lo, g_hi), 0xD8);
            __m256i b8 = _mm256_permute4x64_epi64(_mm256_packus_epi16(b_lo, b_hi), 0xD8);
            __m256i a8 = a_row ? _mm256_loadu_si256((const __m256i *)(a_row + x)) : alpha_ff;

            __m256i ch0 = want_rgba ? r8 : b8;
            __m256i ch2 = want_rgba ? b8 : r8;

            __m256i c01_lo = _mm256_unpacklo_epi8(ch0, g8);
            __m256i c01_hi = _mm256_unpackhi_epi8(ch0, g8);
            __m256i c23_lo = _mm256_unpacklo_epi8(ch2, a8);
            __m256i c23_hi = _mm256_unpackhi_epi8(ch2, a8);

            __m256i px0 = _mm256_unpacklo_epi16(c01_lo, c23_lo);
            __m256i px1 = _mm256_unpackhi_epi16(c01_lo, c23_lo);
            __m256i px2 = _mm256_unpacklo_epi16(c01_hi, c23_hi);
            __m256i px3 = _mm256_unpackhi_epi16(c01_hi, c23_hi);

            uint8_t *dst = dst_row + (size_t)x * 4U;
            _mm256_storeu_si256((__m256i *)(dst + 0), _mm256_permute2x128_si256(px0, px1, 0x20));
            _mm256_storeu_si256((__m256i *)(dst + 32), _mm256_permute2x128_si256(px2, px3, 0x20));
            _mm256_storeu_si256((__m256i *)(dst + 64), _mm256_permute2x128_si256(px0, px1, 0x31));
            _mm256_storeu_si256((__m256i *)(dst + 96), _mm256_permute2x128_si256(px2, px3, 0x31));
        }
    }

    if (x < w) {
        convert_yuv420_row_scalar(y_row, u_near, u_far, v_near, v_far, a_row, dst_row, x, w, cw,
                                  want_rgba);
    }
}
#endif

static void convert_yuv420_to_packed(const wwal_webp_yuva_buffer_t *yuva, uint32_t w, uint32_t h,
                                     bool has_alpha, bool want_rgba, uint8_t *dst_pixels)
{
    uint32_t cw = (w + 1U) / 2U;
    uint32_t ch = (h + 1U) / 2U;

    bool use_avx2 = false;
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(WWAL_DISABLE_SIMD)
    use_avx2 = __builtin_cpu_supports("avx2");
#endif

#ifdef HAVE_OPENMP
#pragma omp parallel for schedule(static) if (h >= 256U)
#endif
    for (uint32_t y = 0; y < h; ++y) {
        uint32_t cy = y >> 1U;
        uint32_t cy_far = cy;
        if (y > 0 && ch > 1U) {
            if ((y & 1U) == 0) {
                cy_far = cy - 1U;
            } else if (cy + 1U < ch) {
                cy_far = cy + 1U;
            }
        }
        const uint8_t *y_row = yuva->y + (size_t)y * (size_t)yuva->y_stride;
        const uint8_t *u_near = yuva->u + (size_t)cy * (size_t)yuva->u_stride;
        const uint8_t *u_far = yuva->u + (size_t)cy_far * (size_t)yuva->u_stride;
        const uint8_t *v_near = yuva->v + (size_t)cy * (size_t)yuva->v_stride;
        const uint8_t *v_far = yuva->v + (size_t)cy_far * (size_t)yuva->v_stride;
        const uint8_t *a_row =
            (has_alpha && yuva->a) ? (yuva->a + (size_t)y * (size_t)yuva->a_stride) : NULL;
        uint8_t *dst_row = dst_pixels + (size_t)y * (size_t)w * 4U;

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(WWAL_DISABLE_SIMD)
        if (use_avx2) {
            convert_yuv420_row_avx2(y_row, u_near, u_far, v_near, v_far, a_row, dst_row, w, cw,
                                    want_rgba);
            continue;
        }
#else
        (void)use_avx2;
#endif
        convert_yuv420_row_scalar(y_row, u_near, u_far, v_near, v_far, a_row, dst_row, 0, w, cw,
                                  want_rgba);
    }
}

static bool decode_webp_frame_direct(const uint8_t *buf, size_t size, uint32_t w, uint32_t h,
                                     int format, bool has_alpha, bool want_rgba,
                                     uint8_t **out_pixels)
{
    wwal_webp_decoder_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (!g_fn_WebPInitDecoderConfigInternal(&cfg, WWAL_WEBP_DECODER_ABI_VERSION))
        return false;

    /* Use multi-threaded YUV decode + OpenMP AVX2 upsampling for large lossy frames to keep full
     * deblocking and chroma quality while avoiding libwebp's single-threaded RGB pass */
    bool use_fast_yuv = (format == 1 && w >= 512U && h >= 512U);
    if (use_fast_yuv) {
        cfg.output.colorspace = has_alpha ? WWAL_WEBP_MODE_YUVA : WWAL_WEBP_MODE_YUV;
        cfg.output.is_external_memory = 0;
        cfg.options.use_threads = 1;

        if (g_fn_WebPDecode(buf, size, &cfg) != 0) {
            g_fn_WebPFreeDecBuffer(&cfg.output);
            return false;
        }

        const wwal_webp_yuva_buffer_t *yuva = &cfg.output.u.YUVA;
        uint32_t cw = (w + 1U) / 2U;
        uint32_t ch = (h + 1U) / 2U;
        bool yuv_ok = yuva->y && yuva->u && yuva->v && yuva->y_stride >= (int)w &&
                      yuva->u_stride >= (int)cw && yuva->v_stride >= (int)cw &&
                      yuva->y_size >= (size_t)yuva->y_stride * (h - 1U) + w &&
                      yuva->u_size >= (size_t)yuva->u_stride * (ch - 1U) + cw &&
                      yuva->v_size >= (size_t)yuva->v_stride * (ch - 1U) + cw &&
                      (!has_alpha || (yuva->a && yuva->a_stride >= (int)w &&
                                      yuva->a_size >= (size_t)yuva->a_stride * (h - 1U) + w));
        if (!yuv_ok) {
            g_fn_WebPFreeDecBuffer(&cfg.output);
            return false;
        }

        size_t out_bytes = (size_t)w * (size_t)h * 4U;
        uint8_t *pixels = alloc_aligned_rgba(out_bytes);
        if (!pixels) {
            g_fn_WebPFreeDecBuffer(&cfg.output);
            return false;
        }

        convert_yuv420_to_packed(yuva, w, h, has_alpha, want_rgba, pixels);
        g_fn_WebPFreeDecBuffer(&cfg.output);
        *out_pixels = pixels;
        return true;
    }

    size_t out_bytes = (size_t)w * (size_t)h * 4U;
    uint8_t *pixels = alloc_aligned_rgba(out_bytes);
    if (!pixels)
        return false;

    cfg.output.colorspace = want_rgba ? WWAL_WEBP_MODE_RGBA : WWAL_WEBP_MODE_BGRA;
    cfg.output.is_external_memory = 1;
    cfg.output.u.RGBA.rgba = pixels;
    cfg.output.u.RGBA.stride = (int)(w * 4U);
    cfg.output.u.RGBA.size = out_bytes;
    cfg.options.use_threads = ((size_t)w * (size_t)h >= 262144U) ? 1 : 0;

    int st = g_fn_WebPDecode(buf, size, &cfg);
    g_fn_WebPFreeDecBuffer(&cfg.output);
    if (st != 0) {
        free(pixels);
        return false;
    }

    *out_pixels = pixels;
    return true;
}

static bool load_webp_from_memory(const uint8_t *buf, size_t size, raw_image_t *out_img,
                                  bool want_rgba)
{
    pthread_once(&g_webp_once, init_webp_lib);
    if (!g_webp_ready)
        return false;

    wwal_webp_bitstream_features_t features;
    memset(&features, 0, sizeof(features));
    if (g_fn_WebPGetFeaturesInternal(buf, size, &features, WWAL_WEBP_DECODER_ABI_VERSION) != 0) {
        return false;
    }

    if (features.has_animation != 0) {
        if (!g_webpdemux_ready)
            return false;
        wwal_webp_data_t wdata = {.bytes = buf, .size = size};
        void *dmux = g_fn_WebPDemuxInternal(&wdata, 0, NULL, WWAL_WEBP_DEMUX_ABI_VERSION);
        if (!dmux)
            return false;

        uint32_t cw = g_fn_WebPDemuxGetI(dmux, WWAL_WEBP_FF_CANVAS_WIDTH);
        uint32_t ch = g_fn_WebPDemuxGetI(dmux, WWAL_WEBP_FF_CANVAS_HEIGHT);
        if (cw == 0 || ch == 0 || cw > WWAL_IMG_MAX_DIM || ch > WWAL_IMG_MAX_DIM) {
            g_fn_WebPDemuxDelete(dmux);
            return false;
        }

        wwal_webp_iterator_t iter;
        memset(&iter, 0, sizeof(iter));
        if (!g_fn_WebPDemuxGetFrame(dmux, 1, &iter) || !iter.fragment.bytes ||
            iter.fragment.size == 0 || iter.width <= 0 || iter.height <= 0 ||
            iter.width > (int)WWAL_IMG_MAX_DIM || iter.height > (int)WWAL_IMG_MAX_DIM) {
            g_fn_WebPDemuxDelete(dmux);
            return false;
        }

        wwal_webp_bitstream_features_t ffeat;
        memset(&ffeat, 0, sizeof(ffeat));
        if (g_fn_WebPGetFeaturesInternal(iter.fragment.bytes, iter.fragment.size, &ffeat,
                                         WWAL_WEBP_DECODER_ABI_VERSION) != 0 ||
            ffeat.width <= 0 || ffeat.height <= 0) {
            g_fn_WebPDemuxReleaseIterator(&iter);
            g_fn_WebPDemuxDelete(dmux);
            return false;
        }

        uint8_t *frame_px = NULL;
        uint32_t fw = (uint32_t)ffeat.width;
        uint32_t fh = (uint32_t)ffeat.height;
        bool dec_ok =
            decode_webp_frame_direct(iter.fragment.bytes, iter.fragment.size, fw, fh, ffeat.format,
                                     ffeat.has_alpha != 0, want_rgba, &frame_px);
        int x_off = iter.x_offset;
        int y_off = iter.y_offset;
        g_fn_WebPDemuxReleaseIterator(&iter);
        g_fn_WebPDemuxDelete(dmux);

        if (!dec_ok || !frame_px)
            return false;

        if (x_off == 0 && y_off == 0 && fw == cw && fh == ch) {
            fill_raw_image(out_img, frame_px, cw, ch);
            return true;
        }

        uint8_t *canvas = alloc_aligned_rgba((size_t)cw * (size_t)ch * 4U);
        if (!canvas) {
            free(frame_px);
            return false;
        }
        memset(canvas, 0, (size_t)cw * (size_t)ch * 4U);

        for (uint32_t fy = 0; fy < fh; ++fy) {
            int64_t dy = (int64_t)y_off + (int64_t)fy;
            if (dy < 0 || dy >= (int64_t)ch)
                continue;
            int64_t sx0 = (x_off < 0) ? -(int64_t)x_off : 0;
            int64_t dx0 = (x_off < 0) ? 0 : (int64_t)x_off;
            if (dx0 >= (int64_t)cw || sx0 >= (int64_t)fw)
                continue;
            int64_t copy_w = (int64_t)fw - sx0;
            if (dx0 + copy_w > (int64_t)cw)
                copy_w = (int64_t)cw - dx0;
            if (copy_w > 0) {
                memcpy(canvas + ((size_t)dy * cw + (size_t)dx0) * 4U,
                       frame_px + ((size_t)fy * fw + (size_t)sx0) * 4U, (size_t)copy_w * 4U);
            }
        }
        free(frame_px);
        fill_raw_image(out_img, canvas, cw, ch);
        return true;
    }

    if (features.width <= 0 || features.height <= 0 || features.width > (int)WWAL_IMG_MAX_DIM ||
        features.height > (int)WWAL_IMG_MAX_DIM) {
        return false;
    }

    uint32_t w = (uint32_t)features.width;
    uint32_t h = (uint32_t)features.height;
    uint8_t *pixels = NULL;
    if (!decode_webp_frame_direct(buf, size, w, h, features.format, features.has_alpha != 0,
                                  want_rgba, &pixels)) {
        return false;
    }

    fill_raw_image(out_img, pixels, w, h);
    return true;
}

/* ============================================================================
 * Lazy libavif Loader
 * ============================================================================ */

enum {
    WWAL_AVIF_RGB_FORMAT_RGBA = 1,
    WWAL_AVIF_RGB_FORMAT_BGRA = 4,
};

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
} wwal_avif_image_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    int format;
    int chromaUpsampling;
    int chromaDownsampling;
    int avoidLibYUV;
    int ignoreAlpha;
    int alphaPremultiplied;
    int isFloat;
    int maxThreads;
    uint8_t *pixels;
    uint32_t rowBytes;
    uint8_t padding[64];
} wwal_avif_rgb_image_t;

typedef struct {
    int codecChoice;
    int maxThreads;
    int requestedSource;
    int allowProgressive;
    int allowIncremental;
    int ignoreExif;
    int ignoreXMP;
    uint32_t imageSizeLimit;
    uint32_t imageDimensionLimit;
    uint32_t imageCountLimit;
    uint32_t strictFlags;
    wwal_avif_image_t *image;
    int imageIndex;
    int imageCount;
} wwal_avif_decoder_t;

typedef wwal_avif_decoder_t *(*avif_decoder_create_fn)(void);
typedef void (*avif_decoder_destroy_fn)(wwal_avif_decoder_t *);
typedef int (*avif_decoder_set_io_memory_fn)(wwal_avif_decoder_t *, const uint8_t *, size_t);
typedef int (*avif_decoder_parse_fn)(wwal_avif_decoder_t *);
typedef int (*avif_decoder_next_image_fn)(wwal_avif_decoder_t *);
typedef int (*avif_decoder_nth_image_fn)(wwal_avif_decoder_t *, uint32_t);
typedef void (*avif_rgb_set_defaults_fn)(wwal_avif_rgb_image_t *, const wwal_avif_image_t *);
typedef int (*avif_yuv_to_rgb_fn)(const wwal_avif_image_t *, wwal_avif_rgb_image_t *);

static pthread_once_t g_avif_once = PTHREAD_ONCE_INIT;
static void *g_avif_handle = NULL;
static avif_decoder_create_fn g_fn_avifDecoderCreate = NULL;
static avif_decoder_destroy_fn g_fn_avifDecoderDestroy = NULL;
static avif_decoder_set_io_memory_fn g_fn_avifDecoderSetIOMemory = NULL;
static avif_decoder_parse_fn g_fn_avifDecoderParse = NULL;
static avif_decoder_next_image_fn g_fn_avifDecoderNextImage = NULL;
static avif_decoder_nth_image_fn g_fn_avifDecoderNthImage = NULL;
static avif_rgb_set_defaults_fn g_fn_avifRGBImageSetDefaults = NULL;
static avif_yuv_to_rgb_fn g_fn_avifImageYUVToRGB = NULL;
static bool g_avif_ready = false;

static void init_avif_lib(void)
{
#ifndef WWAL_DISABLE_AVIF
    static const char *const avif_sonames[] = {
        "libavif.so.16", "libavif.so.15", "libavif.so.17", "libavif.so.14", "libavif.so",
    };
    g_avif_handle = open_dl_sonames(avif_sonames, sizeof(avif_sonames) / sizeof(avif_sonames[0]));
    if (!g_avif_handle)
        return;

    if (resolve_symbol(g_avif_handle, "avifDecoderCreate", &g_fn_avifDecoderCreate) &&
        resolve_symbol(g_avif_handle, "avifDecoderDestroy", &g_fn_avifDecoderDestroy) &&
        resolve_symbol(g_avif_handle, "avifDecoderSetIOMemory", &g_fn_avifDecoderSetIOMemory) &&
        resolve_symbol(g_avif_handle, "avifDecoderParse", &g_fn_avifDecoderParse) &&
        resolve_symbol(g_avif_handle, "avifDecoderNextImage", &g_fn_avifDecoderNextImage) &&
        resolve_symbol(g_avif_handle, "avifDecoderNthImage", &g_fn_avifDecoderNthImage) &&
        resolve_symbol(g_avif_handle, "avifRGBImageSetDefaults", &g_fn_avifRGBImageSetDefaults) &&
        resolve_symbol(g_avif_handle, "avifImageYUVToRGB", &g_fn_avifImageYUVToRGB)) {
        g_avif_ready = true;
    }
#else
    (void)g_avif_handle;
#endif
}

static bool load_avif_from_memory(const uint8_t *buf, size_t size, raw_image_t *out_img,
                                  bool want_rgba)
{
    pthread_once(&g_avif_once, init_avif_lib);
    if (!g_avif_ready)
        return false;

    wwal_avif_decoder_t *decoder = g_fn_avifDecoderCreate();
    if (!decoder)
        return false;

    if (g_fn_avifDecoderSetIOMemory(decoder, buf, size) != 0 ||
        g_fn_avifDecoderParse(decoder) != 0) {
        g_fn_avifDecoderDestroy(decoder);
        return false;
    }

    int dec_res = -1;
    if (decoder->imageCount > 1) {
        time_t now = time(NULL);
        struct tm tm_local;
        uint32_t local_hour = 12;
        if (localtime_r(&now, &tm_local)) {
            local_hour = (uint32_t)tm_local.tm_hour;
        }
        uint32_t idx =
            ((local_hour * (uint32_t)decoder->imageCount) / 24U) % (uint32_t)decoder->imageCount;
        dec_res = g_fn_avifDecoderNthImage(decoder, idx);
    }
    if (dec_res != 0) {
        dec_res = g_fn_avifDecoderNextImage(decoder);
    }
    if (dec_res != 0 || !decoder->image) {
        g_fn_avifDecoderDestroy(decoder);
        return false;
    }

    uint32_t w = decoder->image->width;
    uint32_t h = decoder->image->height;
    if (w == 0 || h == 0 || w > WWAL_IMG_MAX_DIM || h > WWAL_IMG_MAX_DIM) {
        g_fn_avifDecoderDestroy(decoder);
        return false;
    }

    size_t out_bytes = (size_t)w * (size_t)h * 4U;
    uint8_t *pixels = alloc_aligned_rgba(out_bytes);
    if (!pixels) {
        g_fn_avifDecoderDestroy(decoder);
        return false;
    }

    wwal_avif_rgb_image_t rgb;
    memset(&rgb, 0, sizeof(rgb));
    g_fn_avifRGBImageSetDefaults(&rgb, decoder->image);
    rgb.depth = 8;
    rgb.format = want_rgba ? WWAL_AVIF_RGB_FORMAT_RGBA : WWAL_AVIF_RGB_FORMAT_BGRA;
    rgb.pixels = pixels;
    rgb.rowBytes = w * 4U;

    if (g_fn_avifImageYUVToRGB(decoder->image, &rgb) != 0) {
        free(pixels);
        g_fn_avifDecoderDestroy(decoder);
        return false;
    }

    g_fn_avifDecoderDestroy(decoder);
    fill_raw_image(out_img, pixels, w, h);
    return true;
}

/* ============================================================================
 * Lazy libjxl + libjxl_threads Loader
 * ============================================================================ */

typedef struct {
    int have_container;
    uint32_t xsize;
    uint32_t ysize;
    uint32_t bits_per_sample;
    uint32_t exponent_bits_per_sample;
    float intensity_target;
    float min_nits;
    int relative_to_max_display;
    float linear_below;
    int uses_original_profile;
    int have_preview;
    int have_animation;
    int orientation;
    uint32_t num_color_channels;
    uint32_t num_extra_channels;
    uint32_t alpha_bits;
    uint32_t alpha_exponent_bits;
    int alpha_premultiplied;
    uint32_t preview_xsize;
    uint32_t preview_ysize;
    uint32_t anim_tps_numerator;
    uint32_t anim_tps_denominator;
    uint32_t anim_num_loops;
    int anim_have_timecodes;
    uint32_t intrinsic_xsize;
    uint32_t intrinsic_ysize;
    uint8_t padding[128];
} wwal_jxl_basic_info_t;

typedef struct {
    uint32_t num_channels;
    int data_type;
    int endianness;
    size_t align;
} wwal_jxl_pixel_format_t;

typedef void *(*jxl_decoder_create_fn)(const void *);
typedef void (*jxl_decoder_destroy_fn)(void *);
typedef int (*jxl_decoder_subscribe_events_fn)(void *, int);
typedef int (*jxl_decoder_set_parallel_runner_fn)(void *, void *, void *);
typedef int (*jxl_decoder_set_input_fn)(void *, const uint8_t *, size_t);
typedef int (*jxl_decoder_process_input_fn)(void *);
typedef int (*jxl_decoder_get_basic_info_fn)(const void *, wwal_jxl_basic_info_t *);
typedef int (*jxl_decoder_set_image_out_buffer_fn)(void *, const wwal_jxl_pixel_format_t *, void *,
                                                   size_t);
typedef void *(*jxl_thread_runner_create_fn)(const void *, size_t);
typedef void (*jxl_thread_runner_destroy_fn)(void *);
typedef size_t (*jxl_thread_runner_default_threads_fn)(void);

static pthread_once_t g_jxl_once = PTHREAD_ONCE_INIT;
static void *g_jxl_handle = NULL;
static void *g_jxl_threads_handle = NULL;
static jxl_decoder_create_fn g_fn_JxlDecoderCreate = NULL;
static jxl_decoder_destroy_fn g_fn_JxlDecoderDestroy = NULL;
static jxl_decoder_subscribe_events_fn g_fn_JxlDecoderSubscribeEvents = NULL;
static jxl_decoder_set_parallel_runner_fn g_fn_JxlDecoderSetParallelRunner = NULL;
static jxl_decoder_set_input_fn g_fn_JxlDecoderSetInput = NULL;
static jxl_decoder_process_input_fn g_fn_JxlDecoderProcessInput = NULL;
static jxl_decoder_get_basic_info_fn g_fn_JxlDecoderGetBasicInfo = NULL;
static jxl_decoder_set_image_out_buffer_fn g_fn_JxlDecoderSetImageOutBuffer = NULL;
static void *g_sym_JxlThreadParallelRunner = NULL;
static jxl_thread_runner_create_fn g_fn_JxlThreadParallelRunnerCreate = NULL;
static jxl_thread_runner_destroy_fn g_fn_JxlThreadParallelRunnerDestroy = NULL;
static jxl_thread_runner_default_threads_fn g_fn_JxlThreadParallelRunnerDefaultNumWorkerThreads =
    NULL;
static bool g_jxl_ready = false;
static bool g_jxl_threads_ready = false;

static void init_jxl_lib(void)
{
#ifndef WWAL_DISABLE_JXL
    static const char *const jxl_sonames[] = {
        "libjxl.so.0.12", "libjxl.so.0.11", "libjxl.so.0.10", "libjxl.so.0.9", "libjxl.so",
    };
    static const char *const jxl_threads_sonames[] = {
        "libjxl_threads.so.0.12", "libjxl_threads.so.0.11", "libjxl_threads.so.0.10",
        "libjxl_threads.so.0.9",  "libjxl_threads.so",
    };

    g_jxl_handle = open_dl_sonames(jxl_sonames, sizeof(jxl_sonames) / sizeof(jxl_sonames[0]));
    if (!g_jxl_handle)
        return;

    if (resolve_symbol(g_jxl_handle, "JxlDecoderCreate", &g_fn_JxlDecoderCreate) &&
        resolve_symbol(g_jxl_handle, "JxlDecoderDestroy", &g_fn_JxlDecoderDestroy) &&
        resolve_symbol(g_jxl_handle, "JxlDecoderSubscribeEvents",
                       &g_fn_JxlDecoderSubscribeEvents) &&
        resolve_symbol(g_jxl_handle, "JxlDecoderSetInput", &g_fn_JxlDecoderSetInput) &&
        resolve_symbol(g_jxl_handle, "JxlDecoderProcessInput", &g_fn_JxlDecoderProcessInput) &&
        resolve_symbol(g_jxl_handle, "JxlDecoderGetBasicInfo", &g_fn_JxlDecoderGetBasicInfo) &&
        resolve_symbol(g_jxl_handle, "JxlDecoderSetImageOutBuffer",
                       &g_fn_JxlDecoderSetImageOutBuffer)) {
        (void)resolve_symbol(g_jxl_handle, "JxlDecoderSetParallelRunner",
                             &g_fn_JxlDecoderSetParallelRunner);
        g_jxl_ready = true;
    }

    g_jxl_threads_handle = open_dl_sonames(jxl_threads_sonames, sizeof(jxl_threads_sonames) /
                                                                    sizeof(jxl_threads_sonames[0]));
    if (g_jxl_threads_handle && g_fn_JxlDecoderSetParallelRunner) {
        g_sym_JxlThreadParallelRunner = dlsym(g_jxl_threads_handle, "JxlThreadParallelRunner");
        if (g_sym_JxlThreadParallelRunner &&
            resolve_symbol(g_jxl_threads_handle, "JxlThreadParallelRunnerCreate",
                           &g_fn_JxlThreadParallelRunnerCreate) &&
            resolve_symbol(g_jxl_threads_handle, "JxlThreadParallelRunnerDestroy",
                           &g_fn_JxlThreadParallelRunnerDestroy) &&
            resolve_symbol(g_jxl_threads_handle, "JxlThreadParallelRunnerDefaultNumWorkerThreads",
                           &g_fn_JxlThreadParallelRunnerDefaultNumWorkerThreads)) {
            g_jxl_threads_ready = true;
        }
    }
#else
    (void)g_jxl_handle;
    (void)g_jxl_threads_handle;
#endif
}

static bool load_jxl_from_memory(const uint8_t *buf, size_t size, raw_image_t *out_img,
                                 bool want_rgba)
{
    enum {
        JXL_DEC_SUCCESS = 0,
        JXL_DEC_NEED_IMAGE_OUT_BUFFER = 5,
        JXL_DEC_BASIC_INFO = 0x40,
        JXL_DEC_FULL_IMAGE = 0x1000,
        JXL_TYPE_UINT8 = 2,
        JXL_NATIVE_ENDIAN = 0,
    };

    pthread_once(&g_jxl_once, init_jxl_lib);
    if (!g_jxl_ready)
        return false;

    void *dec = g_fn_JxlDecoderCreate(NULL);
    if (!dec)
        return false;

    void *runner = NULL;
    if (g_jxl_threads_ready) {
        size_t nthreads = g_fn_JxlThreadParallelRunnerDefaultNumWorkerThreads();
        if (nthreads < 1U)
            nthreads = 1U;
        if (nthreads > 8U)
            nthreads = 8U;
        runner = g_fn_JxlThreadParallelRunnerCreate(NULL, nthreads);
        if (runner && g_fn_JxlDecoderSetParallelRunner(dec, g_sym_JxlThreadParallelRunner,
                                                       runner) != JXL_DEC_SUCCESS) {
            g_fn_JxlThreadParallelRunnerDestroy(runner);
            runner = NULL;
        }
    }

    if (g_fn_JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) !=
            JXL_DEC_SUCCESS ||
        g_fn_JxlDecoderSetInput(dec, buf, size) != JXL_DEC_SUCCESS) {
        g_fn_JxlDecoderDestroy(dec);
        if (runner)
            g_fn_JxlThreadParallelRunnerDestroy(runner);
        return false;
    }

    uint32_t w = 0, h = 0;
    uint8_t *pixels = NULL;
    size_t out_bytes = 0;
    bool decoded = false;

    wwal_jxl_pixel_format_t fmt = {
        .num_channels = 4,
        .data_type = JXL_TYPE_UINT8,
        .endianness = JXL_NATIVE_ENDIAN,
        .align = 0,
    };

    for (;;) {
        int status = g_fn_JxlDecoderProcessInput(dec);
        if (status == JXL_DEC_BASIC_INFO) {
            wwal_jxl_basic_info_t info;
            memset(&info, 0, sizeof(info));
            if (g_fn_JxlDecoderGetBasicInfo(dec, &info) != JXL_DEC_SUCCESS || info.xsize == 0 ||
                info.ysize == 0 || info.xsize > WWAL_IMG_MAX_DIM || info.ysize > WWAL_IMG_MAX_DIM) {
                break;
            }
            w = info.xsize;
            h = info.ysize;
            out_bytes = (size_t)w * (size_t)h * 4U;
            pixels = alloc_aligned_rgba(out_bytes);
            if (!pixels)
                break;
        } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            if (!pixels ||
                g_fn_JxlDecoderSetImageOutBuffer(dec, &fmt, pixels, out_bytes) != JXL_DEC_SUCCESS) {
                break;
            }
        } else if (status == JXL_DEC_FULL_IMAGE) {
            decoded = (pixels != NULL);
            if (decoded && !want_rgba) {
                swizzle_rgba_to_bgra_inplace(pixels, out_bytes);
            }
            break;
        } else {
            break;
        }
    }

    g_fn_JxlDecoderDestroy(dec);
    if (runner)
        g_fn_JxlThreadParallelRunnerDestroy(runner);

    if (!decoded || !pixels) {
        free(pixels);
        return false;
    }

    fill_raw_image(out_img, pixels, w, h);
    return true;
}

/* ============================================================================
 * Lazy librsvg + libcairo + libgobject Loader & Hardened SVG Fallback
 * ============================================================================ */

static bool svg_extract_attr(const char *tag_start, const char *tag_end, const char *attr_name,
                             char *out_val, size_t out_cap)
{
    if (!tag_start || !tag_end || !attr_name || !out_val || out_cap == 0)
        return false;
    size_t name_len = strlen(attr_name);
    const char *p = tag_start;
    while (p < tag_end) {
        const char *hit = strcasestr(p, attr_name);
        if (!hit || hit >= tag_end)
            return false;
        bool valid_before = (hit == tag_start) || isspace((unsigned char)hit[-1]) ||
                            hit[-1] == ';' || hit[-1] == '"' || hit[-1] == '\'';
        const char *after = hit + name_len;
        while (after < tag_end && isspace((unsigned char)*after))
            after++;
        if (valid_before && after < tag_end && (*after == '=' || *after == ':')) {
            char sep = *after;
            after++;
            while (after < tag_end && isspace((unsigned char)*after))
                after++;
            if (after >= tag_end)
                return false;
            if (*after == '"' || *after == '\'') {
                char quote = *after++;
                const char *val_end = memchr(after, quote, (size_t)(tag_end - after));
                if (!val_end)
                    return false;
                size_t len = (size_t)(val_end - after);
                if (len >= out_cap)
                    len = out_cap - 1;
                memcpy(out_val, after, len);
                out_val[len] = '\0';
                return true;
            } else if (sep == ':') {
                const char *val_end = after;
                while (val_end < tag_end && *val_end != ';' && *val_end != '"' && *val_end != '\'')
                    val_end++;
                size_t len = (size_t)(val_end - after);
                while (len > 0 && isspace((unsigned char)after[len - 1]))
                    len--;
                if (len >= out_cap)
                    len = out_cap - 1;
                memcpy(out_val, after, len);
                out_val[len] = '\0';
                return true;
            }
        }
        p = hit + 1;
    }
    return false;
}

static void svg_parse_dimensions(const char *svg_text, uint32_t *out_w, uint32_t *out_h,
                                 float *out_vb_x, float *out_vb_y, float *out_vb_w, float *out_vb_h)
{
    uint32_t w = 0, h = 0;
    float vx = 0.0f, vy = 0.0f, vw = 0.0f, vh = 0.0f;

    const char *svg_tag = strcasestr(svg_text, "<svg");
    if (svg_tag) {
        const char *tag_end = strchr(svg_tag, '>');
        if (tag_end) {
            char val[128];
            if (svg_extract_attr(svg_tag, tag_end, "viewBox", val, sizeof(val))) {
                for (char *c = val; *c; ++c) {
                    if (*c == ',')
                        *c = ' ';
                }
                if (sscanf(val, "%f %f %f %f", &vx, &vy, &vw, &vh) == 4) {
                    if (!isfinite(vx) || !isfinite(vy) || !isfinite(vw) || !isfinite(vh)) {
                        vx = 0.0f;
                        vy = 0.0f;
                        vw = 0.0f;
                        vh = 0.0f;
                    }
                } else {
                    vx = 0.0f;
                    vy = 0.0f;
                    vw = 0.0f;
                    vh = 0.0f;
                }
            }
            if (svg_extract_attr(svg_tag, tag_end, "width", val, sizeof(val))) {
                if (strchr(val, '%') == NULL) {
                    float wf = strtof(val, NULL);
                    if (isfinite(wf) && wf > 0.0f && wf <= (float)WWAL_IMG_MAX_DIM)
                        w = (uint32_t)lroundf(wf);
                }
            }
            if (svg_extract_attr(svg_tag, tag_end, "height", val, sizeof(val))) {
                if (strchr(val, '%') == NULL) {
                    float hf = strtof(val, NULL);
                    if (isfinite(hf) && hf > 0.0f && hf <= (float)WWAL_IMG_MAX_DIM)
                        h = (uint32_t)lroundf(hf);
                }
            }
        }
    }

    if (w == 0 && isfinite(vw) && vw > 0.0f && vw <= (float)WWAL_IMG_MAX_DIM)
        w = (uint32_t)lroundf(vw);
    if (h == 0 && isfinite(vh) && vh > 0.0f && vh <= (float)WWAL_IMG_MAX_DIM)
        h = (uint32_t)lroundf(vh);
    if (w == 0)
        w = 256;
    if (h == 0)
        h = 256;
    if (!isfinite(vw) || vw <= 0.0f)
        vw = (float)w;
    if (!isfinite(vh) || vh <= 0.0f)
        vh = (float)h;

    *out_w = w;
    *out_h = h;
    *out_vb_x = vx;
    *out_vb_y = vy;
    *out_vb_w = vw;
    *out_vb_h = vh;
}

typedef struct {
    double x;
    double y;
    double width;
    double height;
} rsvg_rect_t;

typedef void *(*rsvg_new_from_data_fn)(const uint8_t *, size_t, void **);
typedef int (*rsvg_get_intrinsic_px_fn)(void *, double *, double *);
typedef int (*rsvg_render_doc_fn)(void *, void *, const rsvg_rect_t *, void **);
typedef void (*g_object_unref_fn)(void *);

typedef void *(*cairo_surface_create_fn)(int, int, int);
typedef int (*cairo_surface_status_fn)(void *);
typedef void *(*cairo_create_fn)(void *);
typedef void (*cairo_destroy_fn)(void *);
typedef void (*cairo_surface_flush_fn)(void *);
typedef uint8_t *(*cairo_surface_get_data_fn)(void *);
typedef int (*cairo_surface_get_stride_fn)(void *);
typedef void (*cairo_surface_destroy_fn)(void *);

static pthread_once_t g_rsvg_once = PTHREAD_ONCE_INIT;
static void *g_rsvg_handle = NULL;
static void *g_cairo_handle = NULL;
static void *g_gobj_handle = NULL;
static rsvg_new_from_data_fn g_fn_rsvg_handle_new_from_data = NULL;
static rsvg_get_intrinsic_px_fn g_fn_rsvg_handle_get_intrinsic_size_in_pixels = NULL;
static rsvg_render_doc_fn g_fn_rsvg_handle_render_document = NULL;
static g_object_unref_fn g_fn_g_object_unref = NULL;
static cairo_surface_create_fn g_fn_cairo_image_surface_create = NULL;
static cairo_surface_status_fn g_fn_cairo_surface_status = NULL;
static cairo_create_fn g_fn_cairo_create = NULL;
static cairo_destroy_fn g_fn_cairo_destroy = NULL;
static cairo_surface_flush_fn g_fn_cairo_surface_flush = NULL;
static cairo_surface_get_data_fn g_fn_cairo_image_surface_get_data = NULL;
static cairo_surface_get_stride_fn g_fn_cairo_image_surface_get_stride = NULL;
static cairo_surface_destroy_fn g_fn_cairo_surface_destroy = NULL;
static bool g_rsvg_ready = false;

static void init_rsvg_lib(void)
{
#ifndef WWAL_DISABLE_SVG
    static const char *const rsvg_sonames[] = {"librsvg-2.so.2", "librsvg-2.so"};
    static const char *const cairo_sonames[] = {"libcairo.so.2", "libcairo.so"};
    static const char *const gobj_sonames[] = {"libgobject-2.0.so.0", "libgobject-2.0.so"};

    g_rsvg_handle = open_dl_sonames(rsvg_sonames, sizeof(rsvg_sonames) / sizeof(rsvg_sonames[0]));
    g_cairo_handle =
        open_dl_sonames(cairo_sonames, sizeof(cairo_sonames) / sizeof(cairo_sonames[0]));
    g_gobj_handle = open_dl_sonames(gobj_sonames, sizeof(gobj_sonames) / sizeof(gobj_sonames[0]));
    if (!g_rsvg_handle || !g_cairo_handle)
        return;

    if (resolve_symbol(g_rsvg_handle, "rsvg_handle_new_from_data",
                       &g_fn_rsvg_handle_new_from_data) &&
        resolve_symbol(g_rsvg_handle, "rsvg_handle_get_intrinsic_size_in_pixels",
                       &g_fn_rsvg_handle_get_intrinsic_size_in_pixels) &&
        resolve_symbol(g_rsvg_handle, "rsvg_handle_render_document",
                       &g_fn_rsvg_handle_render_document) &&
        resolve_symbol(g_cairo_handle, "cairo_image_surface_create",
                       &g_fn_cairo_image_surface_create) &&
        resolve_symbol(g_cairo_handle, "cairo_surface_status", &g_fn_cairo_surface_status) &&
        resolve_symbol(g_cairo_handle, "cairo_create", &g_fn_cairo_create) &&
        resolve_symbol(g_cairo_handle, "cairo_destroy", &g_fn_cairo_destroy) &&
        resolve_symbol(g_cairo_handle, "cairo_surface_flush", &g_fn_cairo_surface_flush) &&
        resolve_symbol(g_cairo_handle, "cairo_image_surface_get_data",
                       &g_fn_cairo_image_surface_get_data) &&
        resolve_symbol(g_cairo_handle, "cairo_image_surface_get_stride",
                       &g_fn_cairo_image_surface_get_stride) &&
        resolve_symbol(g_cairo_handle, "cairo_surface_destroy", &g_fn_cairo_surface_destroy)) {
        if (!g_gobj_handle ||
            !resolve_symbol(g_gobj_handle, "g_object_unref", &g_fn_g_object_unref)) {
            (void)resolve_symbol(g_rsvg_handle, "g_object_unref", &g_fn_g_object_unref);
        }
        g_rsvg_ready = true;
    }
#else
    (void)g_rsvg_handle;
    (void)g_cairo_handle;
    (void)g_gobj_handle;
#endif
}

static uint8_t *sanitize_svg_xml(const uint8_t *buf, size_t size, size_t *out_len)
{
    if (!buf || size == 0 || size > 64U * 1024U * 1024U || !out_len)
        return NULL;

    uint8_t *clean = (uint8_t *)malloc(size + 1);
    if (!clean)
        return NULL;

    const char *src = (const char *)buf;
    const char *end = src + size;
    size_t dst_off = 0;

    while (src < end) {
        size_t rem = (size_t)(end - src);
        if (*src == '<' && rem >= 14 && strncasecmp(src, "<foreignObject", 14) == 0) {
            const char *close_tag = strcasestr(src + 14, "</foreignObject>");
            if (close_tag && close_tag + 16 <= end) {
                src = close_tag + 16;
                continue;
            }
            const char *tag_end = (const char *)memchr(src, '>', rem);
            if (tag_end) {
                src = tag_end + 1;
                continue;
            }
            break;
        }
        clean[dst_off++] = (uint8_t)(*src++);
    }

    clean[dst_off] = '\0';
    *out_len = dst_off;
    return clean;
}

static bool load_svg_librsvg(const uint8_t *buf, size_t size, raw_image_t *out_img, bool want_rgba)
{
    if (!buf || size == 0 || size > 64U * 1024U * 1024U)
        return false;

    pthread_once(&g_rsvg_once, init_rsvg_lib);
    if (!g_rsvg_ready)
        return false;

    size_t clean_len = 0;
    uint8_t *clean_svg = sanitize_svg_xml(buf, size, &clean_len);
    if (!clean_svg || clean_len == 0) {
        free(clean_svg);
        return false;
    }

    void *handle = g_fn_rsvg_handle_new_from_data(clean_svg, clean_len, NULL);
    if (!handle) {
        free(clean_svg);
        return false;
    }

    uint32_t parsed_w = 256, parsed_h = 256;
    float vx = 0.0f, vy = 0.0f, vw = 256.0f, vh = 256.0f;
    svg_parse_dimensions((const char *)clean_svg, &parsed_w, &parsed_h, &vx, &vy, &vw, &vh);
    free(clean_svg);

    double dw = 0.0, dh = 0.0;
    uint32_t w = parsed_w, h = parsed_h;
    if (g_fn_rsvg_handle_get_intrinsic_size_in_pixels(handle, &dw, &dh) && isfinite(dw) &&
        isfinite(dh) && dw > 0.0 && dh > 0.0 && dw <= (double)WWAL_IMG_MAX_DIM &&
        dh <= (double)WWAL_IMG_MAX_DIM) {
        w = (uint32_t)ceil(dw);
        h = (uint32_t)ceil(dh);
    }

    void *surface = g_fn_cairo_image_surface_create(0, (int)w, (int)h);
    if (!surface || g_fn_cairo_surface_status(surface) != 0) {
        if (surface)
            g_fn_cairo_surface_destroy(surface);
        if (g_fn_g_object_unref)
            g_fn_g_object_unref(handle);
        return false;
    }

    void *cr = g_fn_cairo_create(surface);
    if (!cr) {
        g_fn_cairo_surface_destroy(surface);
        if (g_fn_g_object_unref)
            g_fn_g_object_unref(handle);
        return false;
    }

    rsvg_rect_t viewport = {.x = 0.0, .y = 0.0, .width = (double)w, .height = (double)h};
    int render_ok = g_fn_rsvg_handle_render_document(handle, cr, &viewport, NULL);
    g_fn_cairo_surface_flush(surface);

    uint8_t *src_data = g_fn_cairo_image_surface_get_data(surface);
    int src_stride = g_fn_cairo_image_surface_get_stride(surface);

    uint8_t *pixels = NULL;
    if (render_ok && src_data && src_stride >= (int)(w * 4U)) {
        pixels = alloc_aligned_rgba((size_t)w * (size_t)h * 4U);
        if (pixels) {
            for (uint32_t y = 0; y < h; ++y) {
                const uint8_t *srow = src_data + (size_t)y * (size_t)src_stride;
                uint8_t *drow = pixels + (size_t)y * (size_t)w * 4U;
                for (uint32_t x = 0; x < w; ++x) {
                    uint8_t b = srow[x * 4 + 0];
                    uint8_t g = srow[x * 4 + 1];
                    uint8_t r = srow[x * 4 + 2];
                    uint8_t a = srow[x * 4 + 3];
                    if (a > 0 && a < 255) {
                        uint32_t ru = (r * 255U + (a / 2U)) / a;
                        uint32_t gu = (g * 255U + (a / 2U)) / a;
                        uint32_t bu = (b * 255U + (a / 2U)) / a;
                        r = (uint8_t)(ru > 255U ? 255U : ru);
                        g = (uint8_t)(gu > 255U ? 255U : gu);
                        b = (uint8_t)(bu > 255U ? 255U : bu);
                    }
                    drow[x * 4 + 0] = want_rgba ? r : b;
                    drow[x * 4 + 1] = g;
                    drow[x * 4 + 2] = want_rgba ? b : r;
                    drow[x * 4 + 3] = a;
                }
            }
        }
    }

    g_fn_cairo_destroy(cr);
    g_fn_cairo_surface_destroy(surface);
    if (g_fn_g_object_unref)
        g_fn_g_object_unref(handle);

    if (!pixels)
        return false;

    fill_raw_image(out_img, pixels, w, h);
    return true;
}

typedef struct {
    uint8_t r, g, b, a;
} svg_rgba_t;

typedef struct {
    float offset;
    svg_rgba_t color;
} svg_grad_stop_t;

typedef struct {
    char id[64];
    float x1, y1, x2, y2;
    svg_grad_stop_t stops[8];
    size_t stop_count;
} svg_linear_grad_t;

static bool svg_parse_color(const char *str, svg_rgba_t *out)
{
    if (!str || !out)
        return false;
    while (isspace((unsigned char)*str))
        str++;
    if (*str == '\0' || strcasecmp(str, "none") == 0 || strcasecmp(str, "transparent") == 0) {
        *out = (svg_rgba_t){0, 0, 0, 0};
        return false;
    }
    if (str[0] == '#') {
        str++;
        size_t len = 0;
        while (isxdigit((unsigned char)str[len]))
            len++;
        unsigned int r = 0, g = 0, b = 0, a = 255;
        if (len == 3) {
            sscanf(str, "%1x%1x%1x", &r, &g, &b);
            r = (r << 4) | r;
            g = (g << 4) | g;
            b = (b << 4) | b;
        } else if (len == 6) {
            sscanf(str, "%02x%02x%02x", &r, &g, &b);
        } else if (len == 8) {
            sscanf(str, "%02x%02x%02x%02x", &r, &g, &b, &a);
        } else {
            return false;
        }
        *out = (svg_rgba_t){(uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a};
        return true;
    }
    if (strncasecmp(str, "rgb(", 4) == 0) {
        int r = 0, g = 0, b = 0;
        if (sscanf(str + 4, "%d , %d , %d", &r, &g, &b) == 3) {
            r = r < 0 ? 0 : (r > 255 ? 255 : r);
            g = g < 0 ? 0 : (g > 255 ? 255 : g);
            b = b < 0 ? 0 : (b > 255 ? 255 : b);
            *out = (svg_rgba_t){(uint8_t)r, (uint8_t)g, (uint8_t)b, 255};
            return true;
        }
    }
    static const struct {
        const char *name;
        uint8_t r, g, b;
    } named_colors[] = {
        {"black", 0, 0, 0},      {"white", 255, 255, 255}, {"red", 255, 0, 0},
        {"lime", 0, 255, 0},     {"green", 0, 128, 0},     {"blue", 0, 0, 255},
        {"yellow", 255, 255, 0}, {"cyan", 0, 255, 255},    {"magenta", 255, 0, 255},
        {"orange", 255, 165, 0}, {"purple", 128, 0, 128},  {"gray", 128, 128, 128},
        {"grey", 128, 128, 128},
    };
    for (size_t i = 0; i < sizeof(named_colors) / sizeof(named_colors[0]); ++i) {
        if (strcasecmp(str, named_colors[i].name) == 0) {
            *out = (svg_rgba_t){named_colors[i].r, named_colors[i].g, named_colors[i].b, 255};
            return true;
        }
    }
    return false;
}

static svg_rgba_t svg_sample_grad(const svg_linear_grad_t *grad, float u, float v)
{
    if (!grad || grad->stop_count == 0)
        return (svg_rgba_t){0, 0, 0, 255};
    if (grad->stop_count == 1)
        return grad->stops[0].color;

    float dx = grad->x2 - grad->x1;
    float dy = grad->y2 - grad->y1;
    float len2 = dx * dx + dy * dy;
    float t =
        (isfinite(len2) && len2 > 1e-6f) ? (((u - grad->x1) * dx + (v - grad->y1) * dy) / len2) : u;
    if (!isfinite(t) || t <= grad->stops[0].offset)
        return grad->stops[0].color;
    if (t >= grad->stops[grad->stop_count - 1].offset)
        return grad->stops[grad->stop_count - 1].color;

    for (size_t i = 0; i + 1 < grad->stop_count; ++i) {
        float t0 = grad->stops[i].offset;
        float t1 = grad->stops[i + 1].offset;
        if (t >= t0 && t <= t1) {
            float frac = (t1 > t0) ? ((t - t0) / (t1 - t0)) : 0.0f;
            if (!isfinite(frac) || frac < 0.0f)
                frac = 0.0f;
            else if (frac > 1.0f)
                frac = 1.0f;
            svg_rgba_t c0 = grad->stops[i].color;
            svg_rgba_t c1 = grad->stops[i + 1].color;
            return (svg_rgba_t){
                .r = (uint8_t)lroundf((1.0f - frac) * c0.r + frac * c1.r),
                .g = (uint8_t)lroundf((1.0f - frac) * c0.g + frac * c1.g),
                .b = (uint8_t)lroundf((1.0f - frac) * c0.b + frac * c1.b),
                .a = (uint8_t)lroundf((1.0f - frac) * c0.a + frac * c1.a),
            };
        }
    }
    return grad->stops[grad->stop_count - 1].color;
}

static void svg_blend_pixel(uint8_t *dst_px, svg_rgba_t src, bool want_rgba)
{
    if (src.a == 0)
        return;
    uint8_t c0 = want_rgba ? src.r : src.b;
    uint8_t c2 = want_rgba ? src.b : src.r;
    if (src.a == 255) {
        dst_px[0] = c0;
        dst_px[1] = src.g;
        dst_px[2] = c2;
        dst_px[3] = 255;
        return;
    }
    uint32_t sa = src.a;
    uint32_t inv_sa = 255U - sa;
    dst_px[0] = (uint8_t)((c0 * sa + dst_px[0] * inv_sa + 127U) / 255U);
    dst_px[1] = (uint8_t)((src.g * sa + dst_px[1] * inv_sa + 127U) / 255U);
    dst_px[2] = (uint8_t)((c2 * sa + dst_px[2] * inv_sa + 127U) / 255U);
    uint32_t out_a = sa + (dst_px[3] * inv_sa + 127U) / 255U;
    dst_px[3] = (uint8_t)(out_a > 255U ? 255U : out_a);
}

static const svg_linear_grad_t *svg_find_grad(const svg_linear_grad_t *grads, size_t grad_count,
                                              const char *fill_val)
{
    if (!fill_val || strncasecmp(fill_val, "url(", 4) != 0)
        return NULL;
    const char *hash = strchr(fill_val, '#');
    if (!hash)
        return NULL;
    hash++;
    char id[64];
    size_t i = 0;
    while (hash[i] && hash[i] != ')' && hash[i] != '"' && hash[i] != '\'' &&
           !isspace((unsigned char)hash[i]) && i + 1 < sizeof(id)) {
        id[i] = hash[i];
        i++;
    }
    id[i] = '\0';
    for (size_t g = 0; g < grad_count; ++g) {
        if (strcmp(grads[g].id, id) == 0)
            return &grads[g];
    }
    return NULL;
}

static float svg_parse_finite_float(const char *val, float fallback, bool allow_pct,
                                    float pct_scale)
{
    if (!val)
        return fallback;
    float v = strtof(val, NULL);
    if (allow_pct && strchr(val, '%') != NULL)
        v *= pct_scale;
    return isfinite(v) ? v : fallback;
}

static bool load_svg_builtin_fallback(const uint8_t *buf, size_t size, raw_image_t *out_img,
                                      bool want_rgba)
{
    if (!buf || size == 0 || size > 64U * 1024U * 1024U)
        return false;

    char *svg_text = (char *)malloc(size + 1);
    if (!svg_text)
        return false;
    memcpy(svg_text, buf, size);
    svg_text[size] = '\0';

    uint32_t w = 256, h = 256;
    float vx = 0.0f, vy = 0.0f, vw = 256.0f, vh = 256.0f;
    svg_parse_dimensions(svg_text, &w, &h, &vx, &vy, &vw, &vh);
    if (w == 0 || h == 0 || w > WWAL_IMG_MAX_DIM || h > WWAL_IMG_MAX_DIM) {
        free(svg_text);
        return false;
    }

    uint8_t *pixels = alloc_aligned_rgba((size_t)w * (size_t)h * 4U);
    if (!pixels) {
        free(svg_text);
        return false;
    }
    memset(pixels, 0, (size_t)w * (size_t)h * 4U);

    svg_linear_grad_t grads[16];
    size_t grad_count = 0;
    const char *cursor = svg_text;
    while (grad_count < 16 && (cursor = strcasestr(cursor, "<linearGradient")) != NULL) {
        const char *tag_end = strchr(cursor, '>');
        const char *close_tag = strcasestr(cursor, "</linearGradient>");
        if (!tag_end || !close_tag || close_tag < tag_end)
            break;

        svg_linear_grad_t *g = &grads[grad_count];
        memset(g, 0, sizeof(*g));
        g->x1 = 0.0f;
        g->y1 = 0.0f;
        g->x2 = 1.0f;
        g->y2 = 0.0f;

        svg_extract_attr(cursor, tag_end, "id", g->id, sizeof(g->id));
        char val[64];
        if (svg_extract_attr(cursor, tag_end, "x1", val, sizeof(val)))
            g->x1 = svg_parse_finite_float(val, 0.0f, true, 0.01f);
        if (svg_extract_attr(cursor, tag_end, "y1", val, sizeof(val)))
            g->y1 = svg_parse_finite_float(val, 0.0f, true, 0.01f);
        if (svg_extract_attr(cursor, tag_end, "x2", val, sizeof(val)))
            g->x2 = svg_parse_finite_float(val, 1.0f, true, 0.01f);
        if (svg_extract_attr(cursor, tag_end, "y2", val, sizeof(val)))
            g->y2 = svg_parse_finite_float(val, 0.0f, true, 0.01f);

        const char *stop_p = tag_end + 1;
        while (g->stop_count < 8 && (stop_p = strcasestr(stop_p, "<stop")) != NULL &&
               stop_p < close_tag) {
            const char *stop_end = strchr(stop_p, '>');
            if (!stop_end || stop_end > close_tag)
                break;
            float offset = 0.0f;
            if (svg_extract_attr(stop_p, stop_end, "offset", val, sizeof(val))) {
                offset = svg_parse_finite_float(val, 0.0f, true, 0.01f);
                if (offset < 0.0f)
                    offset = 0.0f;
                if (offset > 1.0f)
                    offset = 1.0f;
            }
            svg_rgba_t sc = {0, 0, 0, 255};
            if (svg_extract_attr(stop_p, stop_end, "stop-color", val, sizeof(val))) {
                svg_parse_color(val, &sc);
            }
            if (svg_extract_attr(stop_p, stop_end, "stop-opacity", val, sizeof(val))) {
                float op = strtof(val, NULL);
                if (!isfinite(op) || op < 0.0f)
                    op = 0.0f;
                else if (op > 1.0f)
                    op = 1.0f;
                sc.a = (uint8_t)lroundf(op * 255.0f);
            }
            g->stops[g->stop_count++] = (svg_grad_stop_t){.offset = offset, .color = sc};
            stop_p = stop_end + 1;
        }
        grad_count++;
        cursor = close_tag + 17;
    }

    float scale_x = (vw > 0.0f && isfinite(vw)) ? ((float)w / vw) : 1.0f;
    float scale_y = (vh > 0.0f && isfinite(vh)) ? ((float)h / vh) : 1.0f;

    const char *svg_tag = strcasestr(svg_text, "<svg");
    if (svg_tag) {
        const char *svg_end = strchr(svg_tag, '>');
        char bg_str[64];
        svg_rgba_t bg_col;
        if (svg_end &&
            (svg_extract_attr(svg_tag, svg_end, "background", bg_str, sizeof(bg_str)) ||
             svg_extract_attr(svg_tag, svg_end, "background-color", bg_str, sizeof(bg_str)) ||
             svg_extract_attr(svg_tag, svg_end, "fill", bg_str, sizeof(bg_str))) &&
            svg_parse_color(bg_str, &bg_col)) {
            for (size_t i = 0; i < (size_t)w * (size_t)h; ++i) {
                svg_blend_pixel(pixels + i * 4, bg_col, want_rgba);
            }
        }
    }

    const char *p = svg_text;
    while ((p = strchr(p, '<')) != NULL) {
        if (strncasecmp(p, "<rect", 5) == 0 &&
            (isspace((unsigned char)p[5]) || p[5] == '/' || p[5] == '>')) {
            const char *tag_end = strchr(p, '>');
            if (!tag_end)
                break;
            char val[128];
            float rx = 0.0f, ry = 0.0f, rw = vw, rh = vh;
            if (svg_extract_attr(p, tag_end, "x", val, sizeof(val))) {
                rx = strtof(val, NULL);
                if (strchr(val, '%'))
                    rx = vx + (rx * 0.01f) * vw;
            }
            if (svg_extract_attr(p, tag_end, "y", val, sizeof(val))) {
                ry = strtof(val, NULL);
                if (strchr(val, '%'))
                    ry = vy + (ry * 0.01f) * vh;
            }
            if (svg_extract_attr(p, tag_end, "width", val, sizeof(val))) {
                rw = strtof(val, NULL);
                if (strchr(val, '%'))
                    rw = (rw * 0.01f) * vw;
            }
            if (svg_extract_attr(p, tag_end, "height", val, sizeof(val))) {
                rh = strtof(val, NULL);
                if (strchr(val, '%'))
                    rh = (rh * 0.01f) * vh;
            }

            svg_rgba_t fill_col = {0, 0, 0, 255};
            const svg_linear_grad_t *grad = NULL;
            bool has_fill = true;
            if (svg_extract_attr(p, tag_end, "fill", val, sizeof(val))) {
                grad = svg_find_grad(grads, grad_count, val);
                if (!grad) {
                    has_fill = svg_parse_color(val, &fill_col);
                }
            }
            float opacity = 1.0f;
            if (svg_extract_attr(p, tag_end, "opacity", val, sizeof(val)) ||
                svg_extract_attr(p, tag_end, "fill-opacity", val, sizeof(val))) {
                opacity = strtof(val, NULL);
                if (!isfinite(opacity) || opacity < 0.0f)
                    opacity = 0.0f;
                if (opacity > 1.0f)
                    opacity = 1.0f;
            }

            if (has_fill && isfinite(rx) && isfinite(ry) && isfinite(rw) && isfinite(rh) &&
                rw > 0.0f && rh > 0.0f) {
                float fx0 = floorf((rx - vx) * scale_x);
                float fy0 = floorf((ry - vy) * scale_y);
                float fx1 = ceilf((rx + rw - vx) * scale_x);
                float fy1 = ceilf((ry + rh - vy) * scale_y);
                if (isfinite(fx0) && isfinite(fy0) && isfinite(fx1) && isfinite(fy1)) {
                    int x0 = (fx0 < 0.0f) ? 0 : (fx0 > (float)w ? (int)w : (int)fx0);
                    int y0 = (fy0 < 0.0f) ? 0 : (fy0 > (float)h ? (int)h : (int)fy0);
                    int x1 = (fx1 < 0.0f) ? 0 : (fx1 > (float)w ? (int)w : (int)fx1);
                    int y1 = (fy1 < 0.0f) ? 0 : (fy1 > (float)h ? (int)h : (int)fy1);

                    for (int py = y0; py < y1; ++py) {
                        float v = (y1 > y0) ? ((float)(py - y0) / (float)(y1 - y0)) : 0.0f;
                        uint8_t *row = pixels + (size_t)py * (size_t)w * 4U;
                        for (int px = x0; px < x1; ++px) {
                            svg_rgba_t c = fill_col;
                            if (grad) {
                                float u = (x1 > x0) ? ((float)(px - x0) / (float)(x1 - x0)) : 0.0f;
                                c = svg_sample_grad(grad, u, v);
                            }
                            c.a = (uint8_t)lroundf((float)c.a * opacity);
                            svg_blend_pixel(row + (size_t)px * 4U, c, want_rgba);
                        }
                    }
                }
            }
            p = tag_end + 1;
            continue;
        }

        if (strncasecmp(p, "<circle", 7) == 0 &&
            (isspace((unsigned char)p[7]) || p[7] == '/' || p[7] == '>')) {
            const char *tag_end = strchr(p, '>');
            if (!tag_end)
                break;
            char val[128];
            float cx = 0.0f, cy = 0.0f, r = 0.0f;
            if (svg_extract_attr(p, tag_end, "cx", val, sizeof(val)))
                cx = strtof(val, NULL);
            if (svg_extract_attr(p, tag_end, "cy", val, sizeof(val)))
                cy = strtof(val, NULL);
            if (svg_extract_attr(p, tag_end, "r", val, sizeof(val)))
                r = strtof(val, NULL);

            svg_rgba_t fill_col = {0, 0, 0, 255};
            const svg_linear_grad_t *grad = NULL;
            bool has_fill = true;
            if (svg_extract_attr(p, tag_end, "fill", val, sizeof(val))) {
                grad = svg_find_grad(grads, grad_count, val);
                if (!grad) {
                    has_fill = svg_parse_color(val, &fill_col);
                }
            }
            float opacity = 1.0f;
            if (svg_extract_attr(p, tag_end, "opacity", val, sizeof(val)) ||
                svg_extract_attr(p, tag_end, "fill-opacity", val, sizeof(val))) {
                opacity = strtof(val, NULL);
                if (!isfinite(opacity) || opacity < 0.0f)
                    opacity = 0.0f;
                if (opacity > 1.0f)
                    opacity = 1.0f;
            }

            if (has_fill && isfinite(cx) && isfinite(cy) && isfinite(r) && r > 0.0f) {
                float pcx = (cx - vx) * scale_x;
                float pcy = (cy - vy) * scale_y;
                float prx = r * scale_x;
                float pry = r * scale_y;
                float fx0 = floorf(pcx - prx);
                float fy0 = floorf(pcy - pry);
                float fx1 = ceilf(pcx + prx);
                float fy1 = ceilf(pcy + pry);
                if (isfinite(pcx) && isfinite(pcy) && isfinite(prx) && isfinite(pry) &&
                    isfinite(fx0) && isfinite(fy0) && isfinite(fx1) && isfinite(fy1)) {
                    int x0 = (fx0 < 0.0f) ? 0 : (fx0 > (float)w ? (int)w : (int)fx0);
                    int y0 = (fy0 < 0.0f) ? 0 : (fy0 > (float)h ? (int)h : (int)fy0);
                    int x1 = (fx1 < 0.0f) ? 0 : (fx1 > (float)w ? (int)w : (int)fx1);
                    int y1 = (fy1 < 0.0f) ? 0 : (fy1 > (float)h ? (int)h : (int)fy1);

                    for (int py = y0; py < y1; ++py) {
                        float dy = ((float)py + 0.5f - pcy) / (pry > 1e-5f ? pry : 1.0f);
                        uint8_t *row = pixels + (size_t)py * (size_t)w * 4U;
                        for (int px = x0; px < x1; ++px) {
                            float dx = ((float)px + 0.5f - pcx) / (prx > 1e-5f ? prx : 1.0f);
                            if (dx * dx + dy * dy <= 1.0f) {
                                svg_rgba_t c = fill_col;
                                if (grad) {
                                    c = svg_sample_grad(grad, 0.5f * (dx + 1.0f),
                                                        0.5f * (dy + 1.0f));
                                }
                                c.a = (uint8_t)lroundf((float)c.a * opacity);
                                svg_blend_pixel(row + (size_t)px * 4U, c, want_rgba);
                            }
                        }
                    }
                }
            }
            p = tag_end + 1;
            continue;
        }

        p++;
    }

    free(svg_text);
    fill_raw_image(out_img, pixels, w, h);
    return true;
}

static bool load_svg_from_memory(const uint8_t *buf, size_t size, raw_image_t *out_img,
                                 bool want_rgba)
{
    if (load_svg_librsvg(buf, size, out_img, want_rgba))
        return true;
    return load_svg_builtin_fallback(buf, size, out_img, want_rgba);
}

/* ============================================================================
 * Magic Detection & Unified Format-Direct Public API
 * ============================================================================ */

static bool is_avif_or_heic_magic(const uint8_t *buf, size_t size)
{
    if (size < 12 || memcmp(buf + 4, "ftyp", 4) != 0)
        return false;
    static const char *const brands[] = {"avif", "avis", "heic", "heix", "mif1", "msf1"};
    for (size_t i = 0; i < sizeof(brands) / sizeof(brands[0]); ++i) {
        if (memcmp(buf + 8, brands[i], 4) == 0)
            return true;
    }
    return false;
}

static bool is_jxl_magic(const uint8_t *buf, size_t size)
{
    if (size >= 2 && buf[0] == 0xFF && buf[1] == 0x0A)
        return true;
    static const uint8_t jxl_container[12] = {0x00, 0x00, 0x00, 0x0C, 'J',  'X',
                                              'L',  ' ',  0x0D, 0x0A, 0x87, 0x0A};
    if (size >= 12 && memcmp(buf, jxl_container, 12) == 0)
        return true;
    return false;
}

static bool is_svg_magic(const uint8_t *buf, size_t size)
{
    if (!buf || size < 4)
        return false;
    size_t off = 0;
    if (size >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF)
        off += 3;
    while (off < size && isspace((unsigned char)buf[off]))
        off++;
    if (off + 4 <= size && strncasecmp((const char *)(buf + off), "<svg", 4) == 0)
        return true;
    if (off + 5 <= size && (strncasecmp((const char *)(buf + off), "<?xml", 5) == 0 ||
                            strncasecmp((const char *)(buf + off), "<!doc", 5) == 0 ||
                            strncasecmp((const char *)(buf + off), "<!--", 4) == 0)) {
        size_t scan_len = size - off;
        if (scan_len > 4096)
            scan_len = 4096;
        for (size_t i = 0; i + 4 <= scan_len; ++i) {
            if (strncasecmp((const char *)(buf + off + i), "<svg", 4) == 0)
                return true;
        }
    }
    return false;
}

static bool load_from_memory_fmt(const uint8_t *buf, size_t size, raw_image_t *out_img,
                                 bool want_rgba)
{
    if (!buf || size < 4 || !out_img)
        return false;
    memset(out_img, 0, sizeof(*out_img));

    if (size >= 12 && memcmp(buf, "RIFF", 4) == 0 && memcmp(buf + 8, "WEBP", 4) == 0) {
        return load_webp_from_memory(buf, size, out_img, want_rgba);
    }
    if (size >= 22 && memcmp(buf, "qoif", 4) == 0) {
        return load_qoi_from_memory(buf, size, out_img, want_rgba);
    }
    if (is_avif_or_heic_magic(buf, size)) {
        return load_avif_from_memory(buf, size, out_img, want_rgba);
    }
    if (is_jxl_magic(buf, size)) {
        return load_jxl_from_memory(buf, size, out_img, want_rgba);
    }
    if (is_svg_magic(buf, size)) {
        return load_svg_from_memory(buf, size, out_img, want_rgba);
    }

    uint8_t *pixels = NULL;
    uint32_t w = 0, h = 0;
    bool ok = false;

    if (buf[0] == 0xFF && buf[1] == 0xD8) {
        ok = load_jpeg_from_memory(buf, size, want_rgba, &pixels, &w, &h);
    } else if (size >= 8 && buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G') {
        ok = load_png_from_memory(buf, size, want_rgba, &pixels, &w, &h);
    } else {
        bool is_bmp = (buf[0] == 'B' && buf[1] == 'M');
        bool is_gif = (buf[0] == 'G' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == '8');
        bool is_pnm = (buf[0] == 'P' && (buf[1] == '3' || buf[1] == '5' || buf[1] == '6') &&
                       isspace((unsigned char)buf[2]));
        if (is_bmp || is_gif || is_pnm) {
            FILE *fp = fmemopen((void *)buf, size, "rb");
            if (!fp)
                return false;
            if (is_bmp) {
                ok = load_bmp(fp, want_rgba, &pixels, &w, &h);
            } else if (is_gif) {
                ok = load_gif(fp, want_rgba, &pixels, &w, &h);
            } else {
                ok = load_pnm(fp, want_rgba, &pixels, &w, &h);
            }
            fclose(fp);
        }
    }

    if (!ok || !pixels)
        return false;

    fill_raw_image(out_img, pixels, w, h);
    return true;
}

static bool load_fd_fmt(int fd, raw_image_t *out_img, bool want_rgba)
{
    if (fd < 0 || !out_img)
        return false;

    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 4 &&
        st.st_size <= 256L * 1024L * 1024L && lseek(fd, 0, SEEK_CUR) == 0) {
        size_t sz = (size_t)st.st_size;
        void *mapped = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapped != MAP_FAILED) {
            bool ok = load_from_memory_fmt((const uint8_t *)mapped, sz, out_img, want_rgba);
            munmap(mapped, sz);
            return ok;
        }
    }

    size_t cap = 65536;
    size_t len = 0;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf)
        return false;

    for (;;) {
        if (len == cap) {
            if (cap >= 256U * 1024U * 1024U) {
                free(buf);
                return false;
            }
            size_t next_cap = cap * 2U;
            uint8_t *next_buf = (uint8_t *)realloc(buf, next_cap);
            if (!next_buf) {
                free(buf);
                return false;
            }
            buf = next_buf;
            cap = next_cap;
        }
        ssize_t n = read(fd, buf + len, cap - len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            return false;
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }

    bool ok = load_from_memory_fmt(buf, len, out_img, want_rgba);
    free(buf);
    return ok;
}

static bool load_path_fmt(const char *filepath, raw_image_t *out_img, bool want_rgba)
{
    if (!filepath || !out_img)
        return false;

    if (strcmp(filepath, "-") == 0) {
        return load_fd_fmt(STDIN_FILENO, out_img, want_rgba);
    }

    int fd = open(filepath, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        WAYWAL_LOG_ERR("Could not open image file: %s", filepath);
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        if (st.st_size < 4 || st.st_size > 256L * 1024L * 1024L) {
            close(fd);
            return false;
        }
        size_t sz = (size_t)st.st_size;
        void *mapped = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapped != MAP_FAILED) {
            bool ok = load_from_memory_fmt((const uint8_t *)mapped, sz, out_img, want_rgba);
            munmap(mapped, sz);
            close(fd);
            if (!ok) {
                WAYWAL_LOG_ERR("Unsupported or invalid image file: %s", filepath);
                return false;
            }
            return true;
        }
    }

    bool ok = load_fd_fmt(fd, out_img, want_rgba);
    close(fd);
    if (!ok) {
        WAYWAL_LOG_ERR("Unsupported image format: %s", filepath);
        return false;
    }
    return true;
}

bool image_load_from_memory(const uint8_t *buf, size_t size, raw_image_t *out_img)
{
    return load_from_memory_fmt(buf, size, out_img, false);
}

bool image_load_from_memory_rgba(const uint8_t *buf, size_t size, uint8_t **out_pixels,
                                 uint32_t *out_w, uint32_t *out_h)
{
    if (!out_pixels || !out_w || !out_h)
        return false;
    raw_image_t img;
    if (!load_from_memory_fmt(buf, size, &img, true))
        return false;
    *out_pixels = img.data;
    *out_w = img.width;
    *out_h = img.height;
    return true;
}

bool image_load_fd(int fd, raw_image_t *out_img) { return load_fd_fmt(fd, out_img, false); }

bool image_load_fd_rgba(int fd, uint8_t **out_pixels, uint32_t *out_w, uint32_t *out_h)
{
    if (!out_pixels || !out_w || !out_h)
        return false;
    raw_image_t img;
    if (!load_fd_fmt(fd, &img, true))
        return false;
    *out_pixels = img.data;
    *out_w = img.width;
    *out_h = img.height;
    return true;
}

bool image_load(const char *filepath, uint8_t **out_pixels, uint32_t *out_w, uint32_t *out_h)
{
    if (!out_pixels || !out_w || !out_h)
        return false;
    raw_image_t img;
    if (!load_path_fmt(filepath, &img, false))
        return false;
    *out_pixels = img.data;
    *out_w = img.width;
    *out_h = img.height;
    return true;
}

bool image_load_rgba(const char *filepath, uint8_t **out_pixels, uint32_t *out_w, uint32_t *out_h)
{
    if (!out_pixels || !out_w || !out_h)
        return false;
    raw_image_t img;
    if (!load_path_fmt(filepath, &img, true))
        return false;
    *out_pixels = img.data;
    *out_w = img.width;
    *out_h = img.height;
    return true;
}

bool image_load_raw(const char *filepath, raw_image_t *out_img)
{
    return load_path_fmt(filepath, out_img, false);
}

static void ensure_parent_dirs_for_file(const char *filepath)
{
    if (!filepath || filepath[0] == '\0')
        return;
    char buf[4096];
    int n = snprintf(buf, sizeof(buf), "%s", filepath);
    if (n <= 0 || (size_t)n >= sizeof(buf))
        return;
    char *last_slash = strrchr(buf, '/');
    if (!last_slash || last_slash == buf)
        return;
    *last_slash = '\0';
    for (char *p = buf + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            (void)mkdir(buf, 0755);
            *p = '/';
        }
    }
    (void)mkdir(buf, 0755);
}

static bool save_png_internal(const char *filepath, const uint8_t *pixels, uint32_t width,
                              uint32_t height, bool is_bgra)
{
    if (!filepath || filepath[0] == '\0' || !pixels || width == 0 || height == 0 ||
        width > WWAL_IMG_MAX_DIM || height > WWAL_IMG_MAX_DIM) {
        return false;
    }

    pthread_once(&g_png_once, init_png_lib);
    if (!g_png_write_ready)
        return false;

    uint32_t dst_w = width;
    uint32_t dst_h = height;
    const uint32_t max_w = 1280;
    const uint32_t max_h = 720;
    if (dst_w > max_w || dst_h > max_h) {
        double sx = (double)max_w / (double)dst_w;
        double sy = (double)max_h / (double)dst_h;
        double scale = (sx < sy) ? sx : sy;
        dst_w = (uint32_t)((double)dst_w * scale + 0.5);
        dst_h = (uint32_t)((double)dst_h * scale + 0.5);
        if (dst_w == 0)
            dst_w = 1;
        if (dst_h == 0)
            dst_h = 1;
    }

    static _Atomic uint32_t s_png_seq = 0;
    uint32_t seq = atomic_fetch_add_explicit(&s_png_seq, 1, memory_order_relaxed);
    char tmp_path[4096];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d.%u", filepath, (int)getpid(), seq);
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        return false;
    }

    ensure_parent_dirs_for_file(filepath);

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }

    FILE *fp = fdopen(fd, "wb");
    if (!fp) {
        close(fd);
        unlink(tmp_path);
        return false;
    }

    void *png = g_fn_png_create_write_struct(g_png_ver_str, NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        unlink(tmp_path);
        return false;
    }

    void *info = g_fn_png_create_info_struct(png);
    if (!info) {
        g_fn_png_destroy_write_struct(&png, NULL);
        fclose(fp);
        unlink(tmp_path);
        return false;
    }

    uint8_t *volatile row_buf = (uint8_t *)malloc((size_t)dst_w * 4U);
    if (!row_buf) {
        g_fn_png_destroy_write_struct(&png, &info);
        fclose(fp);
        unlink(tmp_path);
        return false;
    }

    jmp_buf *jb = g_fn_png_set_longjmp_fn(png, longjmp, sizeof(jmp_buf));
    if (!jb || setjmp(*jb) != 0) {
        free((void *)row_buf);
        g_fn_png_destroy_write_struct(&png, &info);
        fclose(fp);
        unlink(tmp_path);
        return false;
    }

    g_fn_png_init_io(png, fp);
    g_fn_png_set_compression_level(png, 1);
    g_fn_png_set_filter(png, 0, WWAL_PNG_FILTER_SUB | WWAL_PNG_FILTER_NONE);
    g_fn_png_set_IHDR(png, info, dst_w, dst_h, 8, WWAL_PNG_COLOR_TYPE_RGBA, WWAL_PNG_INTERLACE_NONE,
                      WWAL_PNG_COMPRESSION_TYPE_DEFAULT, WWAL_PNG_FILTER_TYPE_DEFAULT);
    if (is_bgra) {
        g_fn_png_set_bgr(png);
    }
    g_fn_png_write_info(png, info);

    bool downscale = (dst_w != width || dst_h != height);
    for (uint32_t y = 0; y < dst_h; ++y) {
        if (!downscale) {
            const uint8_t *src_row = pixels + (size_t)y * (size_t)width * 4U;
            memcpy((void *)row_buf, src_row, (size_t)dst_w * 4U);
            for (uint32_t x = 0; x < dst_w; ++x) {
                row_buf[x * 4U + 3U] = 255;
            }
        } else {
            uint32_t sy0 = (uint32_t)(((uint64_t)y * height) / dst_h);
            uint32_t sy1 = (uint32_t)(((uint64_t)(y + 1U) * height) / dst_h);
            if (sy1 <= sy0)
                sy1 = sy0 + 1U;
            if (sy1 > height)
                sy1 = height;
            uint32_t ya = sy0;
            uint32_t yb = sy0 + ((sy1 - sy0) >> 1);
            if (yb >= height)
                yb = height - 1U;
            const uint8_t *srow_a = pixels + (size_t)ya * (size_t)width * 4U;
            const uint8_t *srow_b = pixels + (size_t)yb * (size_t)width * 4U;
            for (uint32_t x = 0; x < dst_w; ++x) {
                uint32_t sx0 = (uint32_t)(((uint64_t)x * width) / dst_w);
                uint32_t sx1 = (uint32_t)(((uint64_t)(x + 1U) * width) / dst_w);
                if (sx1 <= sx0)
                    sx1 = sx0 + 1U;
                if (sx1 > width)
                    sx1 = width;
                uint32_t xa = sx0;
                uint32_t xb = sx0 + ((sx1 - sx0) >> 1);
                if (xb >= width)
                    xb = width - 1U;
                const uint8_t *p00 = srow_a + (size_t)xa * 4U;
                const uint8_t *p10 = srow_a + (size_t)xb * 4U;
                const uint8_t *p01 = srow_b + (size_t)xa * 4U;
                const uint8_t *p11 = srow_b + (size_t)xb * 4U;
                uint32_t sum0 = (uint32_t)p00[0] + p10[0] + p01[0] + p11[0];
                uint32_t sum1 = (uint32_t)p00[1] + p10[1] + p01[1] + p11[1];
                uint32_t sum2 = (uint32_t)p00[2] + p10[2] + p01[2] + p11[2];
                row_buf[x * 4U + 0U] = (uint8_t)((sum0 + 2U) >> 2);
                row_buf[x * 4U + 1U] = (uint8_t)((sum1 + 2U) >> 2);
                row_buf[x * 4U + 2U] = (uint8_t)((sum2 + 2U) >> 2);
                row_buf[x * 4U + 3U] = 255;
            }
        }
        g_fn_png_write_row(png, (const uint8_t *)row_buf);
    }

    g_fn_png_write_end(png, NULL);
    free((void *)row_buf);
    g_fn_png_destroy_write_struct(&png, &info);

    bool io_ok = (fflush(fp) == 0 && !ferror(fp) && fsync(fileno(fp)) == 0);
    if (fclose(fp) != 0 || !io_ok) {
        unlink(tmp_path);
        return false;
    }

    if (rename(tmp_path, filepath) != 0) {
        unlink(tmp_path);
        return false;
    }
    return true;
}

bool image_save_png_bgra(const char *filepath, const uint8_t *bgra_pixels, uint32_t width,
                         uint32_t height)
{
    return save_png_internal(filepath, bgra_pixels, width, height, true);
}

bool image_save_png_rgba(const char *filepath, const uint8_t *rgba_pixels, uint32_t width,
                         uint32_t height)
{
    return save_png_internal(filepath, rgba_pixels, width, height, false);
}
