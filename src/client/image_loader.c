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

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
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
    uint8_t *pixels = (uint8_t *)malloc(total_bytes);
    if (!pixels) {
        png_destroy_read_struct(&png, &info, NULL);
        return false;
    }

    png_bytep *row_pointers = (png_bytep *)malloc(sizeof(png_bytep) * height);
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

static bool load_bmp(FILE *fp, uint8_t **out_pixels, uint32_t *out_w, uint32_t *out_h)
{
    uint8_t header[54];
    if (fread(header, 1, 54, fp) != 54)
        return false;
    if (header[0] != 'B' || header[1] != 'M')
        return false;

    uint32_t data_offset = *(uint32_t *)&header[10];
    int32_t width = *(int32_t *)&header[18];
    int32_t height = *(int32_t *)&header[22];
    uint16_t bpp = *(uint16_t *)&header[28];
    uint32_t compression = *(uint32_t *)&header[30];

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
    } else {
        WAYWAL_LOG_ERR("Unsupported image format: %s", filepath);
    }

    fclose(fp);
    return ok;
}
