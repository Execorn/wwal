#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "image_loader.h"

#include "waywal/log.h"

#include <ctype.h>
#include <dlfcn.h>
#include <fcntl.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#include <math.h>
#include <png.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <turbojpeg.h>
#include <unistd.h>

#undef image_load
#undef image_load_fd
#undef image_load_from_memory
#undef image_free

#if defined(__x86_64__) || defined(_M_X64)
__attribute__((target("avx2"))) static void swizzle_rgba_to_bgra_avx2(uint8_t *pixels,
                                                                      size_t num_bytes)
{
    const __m256i mask = _mm256_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15, 2,
                                          1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
    size_t i = 0;
    for (; i + 32 <= num_bytes; i += 32) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(pixels + i));
        v = _mm256_shuffle_epi8(v, mask);
        _mm256_storeu_si256((__m256i *)(pixels + i), v);
    }
    for (; i + 4 <= num_bytes; i += 4) {
        uint8_t tmp = pixels[i + 0];
        pixels[i + 0] = pixels[i + 2];
        pixels[i + 2] = tmp;
    }
}
#endif

static void swizzle_rgba_to_bgra_inplace(uint8_t *pixels, size_t num_bytes)
{
#if defined(__x86_64__) || defined(_M_X64)
    if (__builtin_cpu_supports("avx2")) {
        swizzle_rgba_to_bgra_avx2(pixels, num_bytes);
        return;
    }
#endif
    for (size_t i = 0; i + 4 <= num_bytes; i += 4) {
        uint8_t tmp = pixels[i + 0];
        pixels[i + 0] = pixels[i + 2];
        pixels[i + 2] = tmp;
    }
}

static uint8_t *alloc_aligned_rgba(size_t size)
{
    if (size == 0 || size > (size_t)16384 * 16384 * 4)
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

static bool load_png(FILE *fp, uint8_t **out_pixels, uint32_t *out_w, uint32_t *out_h)
{
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png)
        return false;

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        return false;
    }

    uint8_t *volatile pixels = NULL;
    uint8_t *volatile raw_16bit = NULL;
    png_bytep *volatile row_pointers = NULL;

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        if (pixels)
            free(pixels);
        if (raw_16bit)
            free(raw_16bit);
        if (row_pointers)
            free(row_pointers);
        return false;
    }

    png_init_io(png, fp);
    png_read_info(png, info);

    png_uint_32 width = 0;
    png_uint_32 height = 0;
    int bit_depth = 0;
    int color_type = 0;

    png_get_IHDR(png, info, &width, &height, &bit_depth, &color_type, NULL, NULL, NULL);
    if (width == 0 || height == 0 || width > 16384 || height > 16384) {
        png_destroy_read_struct(&png, &info, NULL);
        return false;
    }

    bool is_16bit = (bit_depth == 16);
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png);
    }
    if (png_get_valid(png, info, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_filler(png, is_16bit ? 0xFFFF : 0xFF, PNG_FILLER_AFTER);
    }
    if (is_16bit) {
        png_set_swap(png);
    }
    png_set_bgr(png); /* Normalize to DRM_FORMAT_ARGB8888 little-endian byte order [B, G, R, A] */

    png_read_update_info(png, info);

    size_t rowbytes = png_get_rowbytes(png, info);
    size_t out_bytes = (size_t)width * (size_t)height * 4;
    if (rowbytes == 0 || out_bytes == 0 || out_bytes > (size_t)16384 * 16384 * 4) {
        png_destroy_read_struct(&png, &info, NULL);
        return false;
    }
    pixels = alloc_aligned_rgba(out_bytes);
    if (!pixels) {
        png_destroy_read_struct(&png, &info, NULL);
        return false;
    }

    row_pointers = (png_bytep *)malloc(sizeof(png_bytep) * height);
    if (!row_pointers) {
        free(pixels);
        png_destroy_read_struct(&png, &info, NULL);
        return false;
    }

    if (is_16bit) {
        raw_16bit = (uint8_t *)malloc(rowbytes * height);
        if (!raw_16bit) {
            free(row_pointers);
            free(pixels);
            png_destroy_read_struct(&png, &info, NULL);
            return false;
        }
        for (png_uint_32 y = 0; y < height; ++y) {
            row_pointers[y] = raw_16bit + y * rowbytes;
        }
        png_read_image(png, row_pointers);
        png_read_end(png, NULL);

        size_t total_channels = (size_t)width * (size_t)height * 4;
        const uint16_t *src16 = (const uint16_t *)raw_16bit;
        for (size_t i = 0; i < total_channels; ++i) {
            pixels[i] = (uint8_t)(((uint32_t)src16[i] * 255U + 32768U) / 65535U);
        }
        free(raw_16bit);
        raw_16bit = NULL;
    } else {
        for (png_uint_32 y = 0; y < height; ++y) {
            row_pointers[y] = pixels + y * rowbytes;
        }
        png_read_image(png, row_pointers);
        png_read_end(png, NULL);
    }

    free(row_pointers);
    png_destroy_read_struct(&png, &info, NULL);

    *out_pixels = pixels;
    *out_w = (uint32_t)width;
    *out_h = (uint32_t)height;
    return true;
}

static bool load_jpeg_from_memory(const uint8_t *jpeg_buf, size_t fsize, uint8_t **out_pixels,
                                  uint32_t *out_w, uint32_t *out_h)
{
    if (!jpeg_buf || fsize == 0)
        return false;

    tjhandle tj = tjInitDecompress();
    if (!tj)
        return false;

    int width = 0, height = 0, jpeg_subsamp = 0, jpeg_colorspace = 0;
    if (tjDecompressHeader3(tj, jpeg_buf, (unsigned long)fsize, &width, &height, &jpeg_subsamp,
                            &jpeg_colorspace) != 0 ||
        width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        tjDestroy(tj);
        return false;
    }

    size_t out_size = (size_t)width * (size_t)height * 4;
    uint8_t *pixels = alloc_aligned_rgba(out_size);
    if (!pixels) {
        tjDestroy(tj);
        return false;
    }

    /* TJPF_BGRA produces byte order [B, G, R, A] matching little-endian DRM_FORMAT_ARGB8888 */
    if (tjDecompress2(tj, jpeg_buf, (unsigned long)fsize, pixels, width, 0, height, TJPF_BGRA, 0) !=
        0) {
        free(pixels);
        tjDestroy(tj);
        return false;
    }

    tjDestroy(tj);

    *out_pixels = pixels;
    *out_w = (uint32_t)width;
    *out_h = (uint32_t)height;
    return true;
}

__attribute__((unused)) static bool load_jpeg(FILE *fp, uint8_t **out_pixels, uint32_t *out_w,
                                              uint32_t *out_h)
{
    int fd = fileno(fp);
    struct stat st;
    if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
        st.st_size <= 256L * 1024L * 1024L) {
        size_t sz = (size_t)st.st_size;
        void *mapped = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapped != MAP_FAILED) {
            bool ok = load_jpeg_from_memory((const uint8_t *)mapped, sz, out_pixels, out_w, out_h);
            munmap(mapped, sz);
            return ok;
        }
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    if (fsize <= 0)
        return false;
    fseek(fp, 0, SEEK_SET);

    uint8_t *jpeg_buf = (uint8_t *)malloc((size_t)fsize);
    if (!jpeg_buf)
        return false;

    if (fread(jpeg_buf, 1, (size_t)fsize, fp) != (size_t)fsize) {
        free(jpeg_buf);
        return false;
    }

    bool ok = load_jpeg_from_memory(jpeg_buf, (size_t)fsize, out_pixels, out_w, out_h);
    free(jpeg_buf);
    return ok;
}

static inline uint16_t read_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline int32_t read_i32_le(const uint8_t *p) { return (int32_t)read_u32_le(p); }

static bool load_bmp(FILE *fp, uint8_t **out_pixels, uint32_t *out_w, uint32_t *out_h)
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

    if (width <= 0 || height == 0 || width > 16384 || height > 16384 || height < -16384 ||
        (bpp != 24 && bpp != 32) || compression != 0) {
        return false;
    }

    bool flip_y = (height > 0);
    uint32_t abs_h = (uint32_t)(height > 0 ? height : -height);
    uint32_t abs_w = (uint32_t)width;

    size_t row_stride = ((size_t)abs_w * (bpp / 8) + 3) & ~3;
    uint8_t *row_buf = (uint8_t *)malloc(row_stride);
    if (!row_buf)
        return false;

    uint8_t *pixels = alloc_aligned_rgba((size_t)abs_w * abs_h * 4);
    if (!pixels) {
        free(row_buf);
        return false;
    }

    fseek(fp, (long)data_offset, SEEK_SET);

    for (uint32_t i = 0; i < abs_h; ++i) {
        uint32_t y = flip_y ? (abs_h - 1 - i) : i;
        if (fread(row_buf, 1, row_stride, fp) != row_stride) {
            free(row_buf);
            free(pixels);
            return false;
        }

        uint8_t *dst_row = pixels + (size_t)y * abs_w * 4;
        for (uint32_t x = 0; x < abs_w; ++x) {
            if (bpp == 24) {
                dst_row[x * 4 + 0] = row_buf[x * 3 + 0]; /* B (native) */
                dst_row[x * 4 + 1] = row_buf[x * 3 + 1]; /* G */
                dst_row[x * 4 + 2] = row_buf[x * 3 + 2]; /* R (native) */
                dst_row[x * 4 + 3] = 0xFF;               /* A */
            } else {
                dst_row[x * 4 + 0] = row_buf[x * 4 + 0]; /* B (native) */
                dst_row[x * 4 + 1] = row_buf[x * 4 + 1]; /* G */
                dst_row[x * 4 + 2] = row_buf[x * 4 + 2]; /* R (native) */
                dst_row[x * 4 + 3] = row_buf[x * 4 + 3]; /* A */
            }
        }
    }

    free(row_buf);
    *out_pixels = pixels;
    *out_w = abs_w;
    *out_h = abs_h;
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
    int code = (int)(br->bit_buf & ((1U << num_bits) - 1));
    br->bit_buf >>= num_bits;
    br->bit_count -= num_bits;
    return code;
}

static bool load_gif(FILE *fp, uint8_t **out_pixels, uint32_t *out_w, uint32_t *out_h)
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

    bool has_gct = (lsd_packed & 0x80) != 0;
    int gct_entries = has_gct ? (1 << ((lsd_packed & 0x07) + 1)) : 0;
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

        if (block_type == 0x21) { /* Extension block */
            int label = fgetc(fp);
            if (label == EOF)
                break;

            if (label == 0xF9) { /* Graphic Control Extension */
                int ext_len = fgetc(fp);
                if (ext_len != 4)
                    return false;
                uint8_t gce[4];
                if (fread(gce, 1, 4, fp) != 4)
                    return false;
                has_trans = (gce[0] & 0x01) != 0;
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

        if (block_type == 0x2C) { /* Image Descriptor */
            uint8_t desc[9];
            if (fread(desc, 1, 9, fp) != 9)
                return false;

            uint16_t img_left = read_u16_le(&desc[0]);
            uint16_t img_top = read_u16_le(&desc[2]);
            uint16_t img_w = read_u16_le(&desc[4]);
            uint16_t img_h = read_u16_le(&desc[6]);
            uint8_t img_packed = desc[8];

            if (img_w == 0 || img_h == 0 || img_w > 16384 || img_h > 16384)
                return false;

            uint32_t canvas_w = screen_w > 0 ? (uint32_t)screen_w : (uint32_t)img_w;
            uint32_t canvas_h = screen_h > 0 ? (uint32_t)screen_h : (uint32_t)img_h;

            if (canvas_w == 0 || canvas_h == 0 || canvas_w > 16384 || canvas_h > 16384)
                return false;

            if ((uint32_t)img_left + img_w > canvas_w)
                canvas_w = (uint32_t)img_left + img_w;
            if ((uint32_t)img_top + img_h > canvas_h)
                canvas_h = (uint32_t)img_top + img_h;
            if (canvas_w > 16384 || canvas_h > 16384)
                return false;

            bool has_lct = (img_packed & 0x80) != 0;
            bool interlace = (img_packed & 0x40) != 0;
            int lct_entries = has_lct ? (1 << ((img_packed & 0x07) + 1)) : 0;
            uint8_t lct[256][3];
            memset(lct, 0, sizeof(lct));

            if (has_lct && lct_entries > 0) {
                if (fread(lct, 3, (size_t)lct_entries, fp) != (size_t)lct_entries)
                    return false;
            }

            uint8_t (*active_pal)[3] = has_lct ? lct : gct;

            /* Pre-compute 32-bit ARGB8888 (native little-endian BGRA) palette */
            uint32_t u32_palette[256];
            for (int i = 0; i < 256; ++i) {
                if (has_trans && i == (int)trans_idx) {
                    u32_palette[i] = 0x00000000;
                } else {
                    uint32_t r = active_pal[i][0];
                    uint32_t g = active_pal[i][1];
                    uint32_t b = active_pal[i][2];
                    u32_palette[i] = (0xFFU << 24) | (r << 16) | (g << 8) | b;
                }
            }

            uint8_t *canvas_bytes = alloc_aligned_rgba((size_t)canvas_w * canvas_h * 4);
            if (!canvas_bytes)
                return false;
            memset(canvas_bytes, 0, (size_t)canvas_w * canvas_h * 4);
            uint32_t *canvas = (uint32_t *)canvas_bytes;

            if (!has_trans) {
                uint32_t bg_val = u32_palette[bg_color_idx];
                for (size_t i = 0; i < (size_t)canvas_w * canvas_h; ++i) {
                    canvas[i] = bg_val;
                }
            }

            uint32_t *row_map = (uint32_t *)malloc(sizeof(uint32_t) * img_h);
            if (!row_map) {
                free(canvas);
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
                free(canvas);
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
                    if (code >= 4096)
                        break;
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
                    if (chain_err)
                        break;
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
                    if (chain_err)
                        break;
                    first_char = cur;
                    stack[stack_ptr++] = (uint8_t)cur;
                } else {
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

            /* Drain remaining sub-blocks for this image record */
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

            *out_pixels = (uint8_t *)canvas;
            *out_w = canvas_w;
            *out_h = canvas_h;
            return true;
        }
    }

    return false;
}

typedef int (*webp_get_info_fn)(const uint8_t *, size_t, int *, int *);
typedef uint8_t *(*webp_decode_into_fn)(const uint8_t *, size_t, uint8_t *, size_t, int);

static pthread_once_t g_webp_once = PTHREAD_ONCE_INIT;
static void *g_webp_handle = NULL;
static webp_get_info_fn g_fn_WebPGetInfo = NULL;
static webp_decode_into_fn g_fn_WebPDecodeBGRAInto = NULL;
static webp_decode_into_fn g_fn_WebPDecodeRGBAInto = NULL;

static void init_webp_lib(void)
{
    g_webp_handle = dlopen("libwebp.so.7", RTLD_LAZY | RTLD_NODELETE);
    if (!g_webp_handle)
        g_webp_handle = dlopen("libwebp.so", RTLD_LAZY | RTLD_NODELETE);
    if (!g_webp_handle)
        return;
    (void)resolve_symbol(g_webp_handle, "WebPGetInfo", &g_fn_WebPGetInfo);
    (void)resolve_symbol(g_webp_handle, "WebPDecodeBGRAInto", &g_fn_WebPDecodeBGRAInto);
    (void)resolve_symbol(g_webp_handle, "WebPDecodeRGBAInto", &g_fn_WebPDecodeRGBAInto);
}

static bool load_webp_from_memory(const uint8_t *buf, size_t size, raw_image_t *out_img)
{
    pthread_once(&g_webp_once, init_webp_lib);
    if (!g_webp_handle || !g_fn_WebPGetInfo ||
        (!g_fn_WebPDecodeBGRAInto && !g_fn_WebPDecodeRGBAInto)) {
        return false;
    }

    int w = 0, h = 0;
    if (!g_fn_WebPGetInfo(buf, size, &w, &h) || w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        return false;
    }

    int stride = w * 4;
    size_t out_bytes = (size_t)w * (size_t)h * 4;
    uint8_t *pixels = alloc_aligned_rgba(out_bytes);
    if (!pixels) {
        return false;
    }

    if (g_fn_WebPDecodeBGRAInto) {
        if (!g_fn_WebPDecodeBGRAInto(buf, size, pixels, out_bytes, stride)) {
            free(pixels);
            return false;
        }
    } else {
        if (!g_fn_WebPDecodeRGBAInto(buf, size, pixels, out_bytes, stride)) {
            free(pixels);
            return false;
        }
        swizzle_rgba_to_bgra_inplace(pixels, out_bytes);
    }

    fill_raw_image(out_img, pixels, (uint32_t)w, (uint32_t)h);
    return true;
}

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
    g_avif_handle = dlopen("libavif.so.16", RTLD_LAZY | RTLD_NODELETE);
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
}

static bool load_avif_from_memory(const uint8_t *buf, size_t size, raw_image_t *out_img)
{
    pthread_once(&g_avif_once, init_avif_lib);
    if (!g_avif_ready)
        return false;

    wwal_avif_decoder_t *decoder = g_fn_avifDecoderCreate();
    if (!decoder)
        return false;

    if (g_fn_avifDecoderSetIOMemory(decoder, buf, size) != 0 || g_fn_avifDecoderParse(decoder) != 0) {
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
    if (w == 0 || h == 0 || w > 16384 || h > 16384) {
        g_fn_avifDecoderDestroy(decoder);
        return false;
    }

    size_t out_bytes = (size_t)w * (size_t)h * 4;
    uint8_t *pixels = alloc_aligned_rgba(out_bytes);
    if (!pixels) {
        g_fn_avifDecoderDestroy(decoder);
        return false;
    }

    wwal_avif_rgb_image_t rgb;
    memset(&rgb, 0, sizeof(rgb));
    g_fn_avifRGBImageSetDefaults(&rgb, decoder->image);
    rgb.depth = 8;
    rgb.format = 3; /* AVIF_RGB_FORMAT_BGRA */
    rgb.pixels = pixels;
    rgb.rowBytes = w * 4;

    if (g_fn_avifImageYUVToRGB(decoder->image, &rgb) != 0) {
        free(pixels);
        g_fn_avifDecoderDestroy(decoder);
        return false;
    }

    g_fn_avifDecoderDestroy(decoder);
    fill_raw_image(out_img, pixels, w, h);
    return true;
}

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
typedef int (*jxl_decoder_set_input_fn)(void *, const uint8_t *, size_t);
typedef int (*jxl_decoder_process_input_fn)(void *);
typedef int (*jxl_decoder_get_basic_info_fn)(const void *, wwal_jxl_basic_info_t *);
typedef int (*jxl_decoder_set_image_out_buffer_fn)(void *, const wwal_jxl_pixel_format_t *, void *,
                                                   size_t);

static pthread_once_t g_jxl_once = PTHREAD_ONCE_INIT;
static void *g_jxl_handle = NULL;
static jxl_decoder_create_fn g_fn_JxlDecoderCreate = NULL;
static jxl_decoder_destroy_fn g_fn_JxlDecoderDestroy = NULL;
static jxl_decoder_subscribe_events_fn g_fn_JxlDecoderSubscribeEvents = NULL;
static jxl_decoder_set_input_fn g_fn_JxlDecoderSetInput = NULL;
static jxl_decoder_process_input_fn g_fn_JxlDecoderProcessInput = NULL;
static jxl_decoder_get_basic_info_fn g_fn_JxlDecoderGetBasicInfo = NULL;
static jxl_decoder_set_image_out_buffer_fn g_fn_JxlDecoderSetImageOutBuffer = NULL;
static bool g_jxl_ready = false;

static void init_jxl_lib(void)
{
    static const char *const jxl_sonames[] = {
        "libjxl.so.0.11", "libjxl.so.0.12", "libjxl.so.0.10", "libjxl.so.0.9", "libjxl.so",
    };

    for (size_t i = 0; i < sizeof(jxl_sonames) / sizeof(jxl_sonames[0]); ++i) {
        g_jxl_handle = dlopen(jxl_sonames[i], RTLD_LAZY | RTLD_NODELETE);
        if (g_jxl_handle)
            break;
    }
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
        g_jxl_ready = true;
    }
}

static bool load_jxl_from_memory(const uint8_t *buf, size_t size, raw_image_t *out_img)
{
    enum {
        JXL_DEC_SUCCESS = 0,
        JXL_DEC_ERROR = 1,
        JXL_DEC_NEED_MORE_INPUT = 2,
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

    if (g_fn_JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) !=
            JXL_DEC_SUCCESS ||
        g_fn_JxlDecoderSetInput(dec, buf, size) != JXL_DEC_SUCCESS) {
        g_fn_JxlDecoderDestroy(dec);
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
                info.ysize == 0 || info.xsize > 16384 || info.ysize > 16384) {
                break;
            }
            w = info.xsize;
            h = info.ysize;
            out_bytes = (size_t)w * (size_t)h * 4;
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
            if (decoded) {
                swizzle_rgba_to_bgra_inplace(pixels, out_bytes);
            }
            break;
        } else if (status == JXL_DEC_SUCCESS) {
            break;
        } else {
            break;
        }
    }

    g_fn_JxlDecoderDestroy(dec);

    if (!decoded || !pixels) {
        free(pixels);
        return false;
    }

    fill_raw_image(out_img, pixels, w, h);
    return true;
}

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
                sscanf(val, "%f %f %f %f", &vx, &vy, &vw, &vh);
            }
            if (svg_extract_attr(svg_tag, tag_end, "width", val, sizeof(val))) {
                if (strchr(val, '%') == NULL) {
                    float wf = strtof(val, NULL);
                    if (wf > 0.0f && wf <= 16384.0f)
                        w = (uint32_t)lroundf(wf);
                }
            }
            if (svg_extract_attr(svg_tag, tag_end, "height", val, sizeof(val))) {
                if (strchr(val, '%') == NULL) {
                    float hf = strtof(val, NULL);
                    if (hf > 0.0f && hf <= 16384.0f)
                        h = (uint32_t)lroundf(hf);
                }
            }
        }
    }

    if (w == 0 && vw > 0.0f && vw <= 16384.0f)
        w = (uint32_t)lroundf(vw);
    if (h == 0 && vh > 0.0f && vh <= 16384.0f)
        h = (uint32_t)lroundf(vh);
    if (w == 0)
        w = 256;
    if (h == 0)
        h = 256;
    if (vw <= 0.0f)
        vw = (float)w;
    if (vh <= 0.0f)
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
    g_rsvg_handle = dlopen("librsvg-2.so.2", RTLD_LAZY | RTLD_NODELETE);
    g_cairo_handle = dlopen("libcairo.so.2", RTLD_LAZY | RTLD_NODELETE);
    g_gobj_handle = dlopen("libgobject-2.0.so.0", RTLD_LAZY | RTLD_NODELETE);
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
}

static bool load_svg_librsvg(const uint8_t *buf, size_t size, raw_image_t *out_img)
{
    pthread_once(&g_rsvg_once, init_rsvg_lib);
    if (!g_rsvg_ready)
        return false;

    void *handle = g_fn_rsvg_handle_new_from_data(buf, size, NULL);
    if (!handle)
        return false;

    char *svg_str = (char *)malloc(size + 1);
    uint32_t parsed_w = 256, parsed_h = 256;
    float vx = 0.0f, vy = 0.0f, vw = 256.0f, vh = 256.0f;
    if (svg_str) {
        memcpy(svg_str, buf, size);
        svg_str[size] = '\0';
        svg_parse_dimensions(svg_str, &parsed_w, &parsed_h, &vx, &vy, &vw, &vh);
        free(svg_str);
    }

    double dw = 0.0, dh = 0.0;
    uint32_t w = parsed_w, h = parsed_h;
    if (g_fn_rsvg_handle_get_intrinsic_size_in_pixels(handle, &dw, &dh) && dw > 0.0 && dh > 0.0 &&
        dw <= 16384.0 && dh <= 16384.0) {
        w = (uint32_t)ceil(dw);
        h = (uint32_t)ceil(dh);
    }

    void *surface = g_fn_cairo_image_surface_create(0 /* CAIRO_FORMAT_ARGB32 */, (int)w, (int)h);
    if (!surface || g_fn_cairo_surface_status(surface) != 0) {
        if (surface)
            g_fn_cairo_surface_destroy(surface);
        if (g_fn_g_object_unref)
            g_fn_g_object_unref(handle);
        return false;
    }

    void *cr = g_fn_cairo_create(surface);
    rsvg_rect_t viewport = {.x = 0.0, .y = 0.0, .width = (double)w, .height = (double)h};
    int render_ok = g_fn_rsvg_handle_render_document(handle, cr, &viewport, NULL);
    g_fn_cairo_surface_flush(surface);

    uint8_t *src_data = g_fn_cairo_image_surface_get_data(surface);
    int src_stride = g_fn_cairo_image_surface_get_stride(surface);

    uint8_t *pixels = NULL;
    if (render_ok && src_data && src_stride >= (int)(w * 4)) {
        pixels = alloc_aligned_rgba((size_t)w * (size_t)h * 4);
        if (pixels) {
            for (uint32_t y = 0; y < h; ++y) {
                const uint8_t *srow = src_data + (size_t)y * (size_t)src_stride;
                uint8_t *drow = pixels + (size_t)y * (size_t)w * 4;
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
                    drow[x * 4 + 0] = b;
                    drow[x * 4 + 1] = g;
                    drow[x * 4 + 2] = r;
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
        {"black", 0, 0, 0},       {"white", 255, 255, 255}, {"red", 255, 0, 0},
        {"lime", 0, 255, 0},      {"green", 0, 128, 0},     {"blue", 0, 0, 255},
        {"yellow", 255, 255, 0},  {"cyan", 0, 255, 255},    {"magenta", 255, 0, 255},
        {"orange", 255, 165, 0},  {"purple", 128, 0, 128},  {"gray", 128, 128, 128},
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
    float t = (len2 > 1e-6f) ? (((u - grad->x1) * dx + (v - grad->y1) * dy) / len2) : u;
    if (t <= grad->stops[0].offset)
        return grad->stops[0].color;
    if (t >= grad->stops[grad->stop_count - 1].offset)
        return grad->stops[grad->stop_count - 1].color;

    for (size_t i = 0; i + 1 < grad->stop_count; ++i) {
        float t0 = grad->stops[i].offset;
        float t1 = grad->stops[i + 1].offset;
        if (t >= t0 && t <= t1) {
            float frac = (t1 > t0) ? ((t - t0) / (t1 - t0)) : 0.0f;
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

static void svg_blend_pixel(uint8_t *dst_bgra, svg_rgba_t src)
{
    if (src.a == 0)
        return;
    if (src.a == 255) {
        dst_bgra[0] = src.b;
        dst_bgra[1] = src.g;
        dst_bgra[2] = src.r;
        dst_bgra[3] = 255;
        return;
    }
    uint32_t sa = src.a;
    uint32_t inv_sa = 255U - sa;
    dst_bgra[0] = (uint8_t)((src.b * sa + dst_bgra[0] * inv_sa + 127U) / 255U);
    dst_bgra[1] = (uint8_t)((src.g * sa + dst_bgra[1] * inv_sa + 127U) / 255U);
    dst_bgra[2] = (uint8_t)((src.r * sa + dst_bgra[2] * inv_sa + 127U) / 255U);
    uint32_t out_a = sa + (dst_bgra[3] * inv_sa + 127U) / 255U;
    dst_bgra[3] = (uint8_t)(out_a > 255U ? 255U : out_a);
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

static bool load_svg_builtin_fallback(const uint8_t *buf, size_t size, raw_image_t *out_img)
{
    char *svg_text = (char *)malloc(size + 1);
    if (!svg_text)
        return false;
    memcpy(svg_text, buf, size);
    svg_text[size] = '\0';

    uint32_t w = 256, h = 256;
    float vx = 0.0f, vy = 0.0f, vw = 256.0f, vh = 256.0f;
    svg_parse_dimensions(svg_text, &w, &h, &vx, &vy, &vw, &vh);
    if (w == 0 || h == 0 || w > 16384 || h > 16384) {
        free(svg_text);
        return false;
    }

    uint8_t *pixels = alloc_aligned_rgba((size_t)w * (size_t)h * 4);
    if (!pixels) {
        free(svg_text);
        return false;
    }
    memset(pixels, 0, (size_t)w * (size_t)h * 4);

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
        if (svg_extract_attr(cursor, tag_end, "x1", val, sizeof(val))) {
            g->x1 = strtof(val, NULL);
            if (strchr(val, '%'))
                g->x1 *= 0.01f;
        }
        if (svg_extract_attr(cursor, tag_end, "y1", val, sizeof(val))) {
            g->y1 = strtof(val, NULL);
            if (strchr(val, '%'))
                g->y1 *= 0.01f;
        }
        if (svg_extract_attr(cursor, tag_end, "x2", val, sizeof(val))) {
            g->x2 = strtof(val, NULL);
            if (strchr(val, '%'))
                g->x2 *= 0.01f;
        }
        if (svg_extract_attr(cursor, tag_end, "y2", val, sizeof(val))) {
            g->y2 = strtof(val, NULL);
            if (strchr(val, '%'))
                g->y2 *= 0.01f;
        }

        const char *stop_p = tag_end + 1;
        while (g->stop_count < 8 && (stop_p = strcasestr(stop_p, "<stop")) != NULL &&
               stop_p < close_tag) {
            const char *stop_end = strchr(stop_p, '>');
            if (!stop_end || stop_end > close_tag)
                break;
            float offset = 0.0f;
            if (svg_extract_attr(stop_p, stop_end, "offset", val, sizeof(val))) {
                offset = strtof(val, NULL);
                if (strchr(val, '%'))
                    offset *= 0.01f;
            }
            svg_rgba_t sc = {0, 0, 0, 255};
            if (svg_extract_attr(stop_p, stop_end, "stop-color", val, sizeof(val))) {
                svg_parse_color(val, &sc);
            }
            if (svg_extract_attr(stop_p, stop_end, "stop-opacity", val, sizeof(val))) {
                float op = strtof(val, NULL);
                if (op < 0.0f)
                    op = 0.0f;
                if (op > 1.0f)
                    op = 1.0f;
                sc.a = (uint8_t)lroundf(op * 255.0f);
            }
            g->stops[g->stop_count++] = (svg_grad_stop_t){.offset = offset, .color = sc};
            stop_p = stop_end + 1;
        }
        grad_count++;
        cursor = close_tag + 17;
    }

    float scale_x = (vw > 0.0f) ? ((float)w / vw) : 1.0f;
    float scale_y = (vh > 0.0f) ? ((float)h / vh) : 1.0f;

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
                svg_blend_pixel(pixels + i * 4, bg_col);
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
                if (opacity < 0.0f)
                    opacity = 0.0f;
                if (opacity > 1.0f)
                    opacity = 1.0f;
            }

            if (has_fill && rw > 0.0f && rh > 0.0f) {
                int x0 = (int)floorf((rx - vx) * scale_x);
                int y0 = (int)floorf((ry - vy) * scale_y);
                int x1 = (int)ceilf((rx + rw - vx) * scale_x);
                int y1 = (int)ceilf((ry + rh - vy) * scale_y);
                if (x0 < 0)
                    x0 = 0;
                if (y0 < 0)
                    y0 = 0;
                if (x1 > (int)w)
                    x1 = (int)w;
                if (y1 > (int)h)
                    y1 = (int)h;

                for (int py = y0; py < y1; ++py) {
                    float v = (y1 > y0) ? ((float)(py - y0) / (float)(y1 - y0)) : 0.0f;
                    uint8_t *row = pixels + (size_t)py * (size_t)w * 4;
                    for (int px = x0; px < x1; ++px) {
                        svg_rgba_t c = fill_col;
                        if (grad) {
                            float u = (x1 > x0) ? ((float)(px - x0) / (float)(x1 - x0)) : 0.0f;
                            c = svg_sample_grad(grad, u, v);
                        }
                        c.a = (uint8_t)lroundf((float)c.a * opacity);
                        svg_blend_pixel(row + (size_t)px * 4, c);
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
                if (opacity < 0.0f)
                    opacity = 0.0f;
                if (opacity > 1.0f)
                    opacity = 1.0f;
            }

            if (has_fill && r > 0.0f) {
                float pcx = (cx - vx) * scale_x;
                float pcy = (cy - vy) * scale_y;
                float prx = r * scale_x;
                float pry = r * scale_y;
                int x0 = (int)floorf(pcx - prx);
                int y0 = (int)floorf(pcy - pry);
                int x1 = (int)ceilf(pcx + prx);
                int y1 = (int)ceilf(pcy + pry);
                if (x0 < 0)
                    x0 = 0;
                if (y0 < 0)
                    y0 = 0;
                if (x1 > (int)w)
                    x1 = (int)w;
                if (y1 > (int)h)
                    y1 = (int)h;

                for (int py = y0; py < y1; ++py) {
                    float dy = ((float)py + 0.5f - pcy) / (pry > 1e-5f ? pry : 1.0f);
                    uint8_t *row = pixels + (size_t)py * (size_t)w * 4;
                    for (int px = x0; px < x1; ++px) {
                        float dx = ((float)px + 0.5f - pcx) / (prx > 1e-5f ? prx : 1.0f);
                        if (dx * dx + dy * dy <= 1.0f) {
                            svg_rgba_t c = fill_col;
                            if (grad) {
                                c = svg_sample_grad(grad, 0.5f * (dx + 1.0f), 0.5f * (dy + 1.0f));
                            }
                            c.a = (uint8_t)lroundf((float)c.a * opacity);
                            svg_blend_pixel(row + (size_t)px * 4, c);
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

static bool load_svg_from_memory(const uint8_t *buf, size_t size, raw_image_t *out_img)
{
    if (load_svg_librsvg(buf, size, out_img))
        return true;
    return load_svg_builtin_fallback(buf, size, out_img);
}

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
    if (off + 5 <= size &&
        (strncasecmp((const char *)(buf + off), "<?xml", 5) == 0 ||
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

bool image_load_from_memory(const uint8_t *buf, size_t size, raw_image_t *out_img)
{
    if (!buf || size < 4 || !out_img)
        return false;
    memset(out_img, 0, sizeof(*out_img));

    if (size >= 12 && memcmp(buf, "RIFF", 4) == 0 && memcmp(buf + 8, "WEBP", 4) == 0) {
        return load_webp_from_memory(buf, size, out_img);
    }
    if (is_avif_or_heic_magic(buf, size)) {
        return load_avif_from_memory(buf, size, out_img);
    }
    if (is_jxl_magic(buf, size)) {
        return load_jxl_from_memory(buf, size, out_img);
    }
    if (is_svg_magic(buf, size)) {
        return load_svg_from_memory(buf, size, out_img);
    }

    uint8_t *pixels = NULL;
    uint32_t w = 0, h = 0;
    bool ok = false;

    if (buf[0] == 0xFF && buf[1] == 0xD8) {
        ok = load_jpeg_from_memory(buf, size, &pixels, &w, &h);
    } else {
        FILE *fp = fmemopen((void *)buf, size, "rb");
        if (!fp)
            return false;
        if (buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G') {
            ok = load_png(fp, &pixels, &w, &h);
        } else if (buf[0] == 'B' && buf[1] == 'M') {
            ok = load_bmp(fp, &pixels, &w, &h);
        } else if (buf[0] == 'G' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == '8') {
            ok = load_gif(fp, &pixels, &w, &h);
        }
        fclose(fp);
    }

    if (!ok || !pixels)
        return false;

    fill_raw_image(out_img, pixels, w, h);
    return true;
}

bool image_load_from_memory_rgba(const uint8_t *buf, size_t size, uint8_t **out_pixels,
                                 uint32_t *out_w, uint32_t *out_h)
{
    if (!out_pixels || !out_w || !out_h)
        return false;
    raw_image_t img;
    if (!image_load_from_memory(buf, size, &img))
        return false;
    *out_pixels = img.data;
    *out_w = img.width;
    *out_h = img.height;
    return true;
}

bool image_load_fd(int fd, raw_image_t *out_img)
{
    if (fd < 0 || !out_img)
        return false;

    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 4 &&
        st.st_size <= 256L * 1024L * 1024L && lseek(fd, 0, SEEK_CUR) == 0) {
        size_t sz = (size_t)st.st_size;
        void *mapped = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapped != MAP_FAILED) {
            bool ok = image_load_from_memory((const uint8_t *)mapped, sz, out_img);
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
            size_t next_cap = cap * 2;
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
            free(buf);
            return false;
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }

    bool ok = image_load_from_memory(buf, len, out_img);
    free(buf);
    return ok;
}

bool image_load_fd_rgba(int fd, uint8_t **out_pixels, uint32_t *out_w, uint32_t *out_h)
{
    if (!out_pixels || !out_w || !out_h)
        return false;
    raw_image_t img;
    if (!image_load_fd(fd, &img))
        return false;
    *out_pixels = img.data;
    *out_w = img.width;
    *out_h = img.height;
    return true;
}

bool image_load(const char *filepath, uint8_t **out_pixels, uint32_t *out_w, uint32_t *out_h)
{
    if (!filepath || !out_pixels || !out_w || !out_h)
        return false;

    if (strcmp(filepath, "-") == 0) {
        return image_load_fd_rgba(STDIN_FILENO, out_pixels, out_w, out_h);
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
            raw_image_t img;
            bool ok = image_load_from_memory((const uint8_t *)mapped, sz, &img);
            munmap(mapped, sz);
            close(fd);
            if (!ok) {
                WAYWAL_LOG_ERR("Unsupported or invalid image file: %s", filepath);
                return false;
            }
            *out_pixels = img.data;
            *out_w = img.width;
            *out_h = img.height;
            return true;
        }
    }

    bool ok = image_load_fd_rgba(fd, out_pixels, out_w, out_h);
    close(fd);
    if (!ok) {
        WAYWAL_LOG_ERR("Unsupported image format: %s", filepath);
    }
    return ok;
}

bool image_load_raw(const char *filepath, raw_image_t *out_img)
{
    if (!filepath || !out_img)
        return false;
    uint8_t *pixels = NULL;
    uint32_t w = 0, h = 0;
    if (!image_load(filepath, &pixels, &w, &h))
        return false;
    fill_raw_image(out_img, pixels, w, h);
    return true;
}
