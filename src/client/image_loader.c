#include "image_loader.h"

#include "waywal/log.h"

#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>

void image_free(uint8_t *pixels) { free(pixels); }

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

    uint8_t * volatile pixels = NULL;
    png_bytep * volatile row_pointers = NULL;

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        if (pixels) free(pixels);
        if (row_pointers) free(row_pointers);
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

    if (bit_depth == 16) {
        png_set_strip_16(png);
    }
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
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    }
    png_set_bgr(png); /* Normalize to DRM_FORMAT_ARGB8888 little-endian byte order [B, G, R, A] */

    png_read_update_info(png, info);

    size_t rowbytes = png_get_rowbytes(png, info);
    size_t total_bytes = rowbytes * height;
    if (total_bytes == 0 || total_bytes > (size_t)16384 * 16384 * 4) {
        png_destroy_read_struct(&png, &info, NULL);
        return false;
    }
    pixels = (uint8_t *)malloc(total_bytes);
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

    for (png_uint_32 y = 0; y < height; ++y) {
        row_pointers[y] = pixels + y * rowbytes;
    }

    png_read_image(png, row_pointers);
    png_read_end(png, NULL);

    free(row_pointers);
    png_destroy_read_struct(&png, &info, NULL);

    *out_pixels = pixels;
    *out_w = (uint32_t)width;
    *out_h = (uint32_t)height;
    return true;
}

static bool load_jpeg(FILE *fp, uint8_t **out_pixels, uint32_t *out_w, uint32_t *out_h)
{
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

    tjhandle tj = tjInitDecompress();
    if (!tj) {
        free(jpeg_buf);
        return false;
    }

    int width = 0, height = 0, jpeg_subsamp = 0, jpeg_colorspace = 0;
    if (tjDecompressHeader3(tj, jpeg_buf, (unsigned long)fsize, &width, &height, &jpeg_subsamp,
                            &jpeg_colorspace) != 0 ||
        width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        tjDestroy(tj);
        free(jpeg_buf);
        return false;
    }

    size_t out_size = (size_t)width * (size_t)height * 4;
    uint8_t *pixels = (uint8_t *)malloc(out_size);
    if (!pixels) {
        tjDestroy(tj);
        free(jpeg_buf);
        return false;
    }

    /* TJPF_BGRA produces byte order [B, G, R, A] matching little-endian DRM_FORMAT_ARGB8888 */
    if (tjDecompress2(tj, jpeg_buf, (unsigned long)fsize, pixels, width, 0, height, TJPF_BGRA, 0) !=
        0) {
        free(pixels);
        tjDestroy(tj);
        free(jpeg_buf);
        return false;
    }

    tjDestroy(tj);
    free(jpeg_buf);

    *out_pixels = pixels;
    *out_w = (uint32_t)width;
    *out_h = (uint32_t)height;
    return true;
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

    uint8_t *pixels = (uint8_t *)malloc((size_t)abs_w * abs_h * 4);
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

            uint32_t *canvas = (uint32_t *)calloc((size_t)canvas_w * canvas_h, sizeof(uint32_t));
            if (!canvas)
                return false;

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

                if (code == next_code) {
                    stack[stack_ptr++] = 0;
                    int cur = old_code;
                    while (cur >= clear_code) {
                        if (cur >= 4096 || stack_ptr >= 4096)
                            break;
                        stack[stack_ptr++] = suffix[cur];
                        cur = prefix[cur];
                    }
                    first_char = cur;
                    if (stack_ptr < 4096)
                        stack[stack_ptr++] = (uint8_t)cur;
                    stack[0] = (uint8_t)first_char;
                } else if (code < next_code) {
                    int cur = code;
                    while (cur >= clear_code) {
                        if (cur >= 4096 || stack_ptr >= 4096)
                            break;
                        stack[stack_ptr++] = suffix[cur];
                        cur = prefix[cur];
                    }
                    first_char = cur;
                    if (stack_ptr < 4096)
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

bool image_load(const char *filepath, uint8_t **out_pixels, uint32_t *out_w, uint32_t *out_h)
{
    if (!filepath || !out_pixels || !out_w || !out_h)
        return false;

    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        WAYWAL_LOG_ERR("Could not open image file: %s", filepath);
        return false;
    }

    uint8_t magic[8];
    if (fread(magic, 1, sizeof(magic), fp) < 4) {
        fclose(fp);
        return false;
    }
    fseek(fp, 0, SEEK_SET);

    bool ok = false;
    if (magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G') {
        ok = load_png(fp, out_pixels, out_w, out_h);
    } else if (magic[0] == 0xFF && magic[1] == 0xD8) {
        ok = load_jpeg(fp, out_pixels, out_w, out_h);
    } else if (magic[0] == 'B' && magic[1] == 'M') {
        ok = load_bmp(fp, out_pixels, out_w, out_h);
    } else if (magic[0] == 'G' && magic[1] == 'I' && magic[2] == 'F' && magic[3] == '8') {
        ok = load_gif(fp, out_pixels, out_w, out_h);
    } else {
        WAYWAL_LOG_ERR("Unsupported image format: %s", filepath);
    }

    fclose(fp);
    return ok;
}
