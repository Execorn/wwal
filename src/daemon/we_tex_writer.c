// SPDX-License-Identifier: GPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L

#include "waywal/we_asset_writer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static inline void set_err(we_asset_writer_err_t *out_err, we_asset_writer_err_t err)
{
    if (out_err) {
        *out_err = err;
    }
}

static inline void write_u32_le(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
    dst[2] = (uint8_t)((v >> 16) & 0xFFu);
    dst[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline void write_i32_le(uint8_t *dst, int32_t v)
{
    uint32_t u = 0u;
    memcpy(&u, &v, sizeof(u));
    write_u32_le(dst, u);
}

static inline void write_f32_le(uint8_t *dst, float v)
{
    uint32_t u = 0u;
    memcpy(&u, &v, sizeof(u));
    write_u32_le(dst, u);
}

static size_t format_bpp(we_tex_format_t fmt)
{
    switch (fmt) {
    case WE_TEX_FMT_RGBA8888:
        return 4u;
    case WE_TEX_FMT_RG88:
        return 2u;
    case WE_TEX_FMT_R8:
        return 1u;
    default:
        return 0u;
    }
}

static bool buf_reserve(uint8_t **buf, size_t *cap, size_t needed)
{
    if (needed <= *cap) {
        return true;
    }
    size_t new_cap = (*cap == 0u) ? 4096u : *cap;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2u) {
            new_cap = needed;
            break;
        }
        new_cap *= 2u;
    }
    uint8_t *grown = (uint8_t *)realloc(*buf, new_cap);
    if (!grown) {
        return false;
    }
    *buf = grown;
    *cap = new_cap;
    return true;
}

static bool append_bytes(uint8_t **buf, size_t *len, size_t *cap, const void *src, size_t n)
{
    if (*len > SIZE_MAX - n) {
        return false;
    }
    if (!buf_reserve(buf, cap, *len + n)) {
        return false;
    }
    memcpy(*buf + *len, src, n);
    *len += n;
    return true;
}

static bool validate_spec(const we_tex_encode_spec_t *spec, size_t bpp,
                          we_asset_writer_err_t *out_err)
{
    if (spec->texture_width == 0u || spec->texture_height == 0u ||
        spec->texture_width > WE_TEX_MAX_DIM || spec->texture_height > WE_TEX_MAX_DIM ||
        spec->image_width == 0u || spec->image_height == 0u ||
        spec->image_width > spec->texture_width || spec->image_height > spec->texture_height) {
        set_err(out_err, WE_WRITER_ERR_LIMIT_EXCEEDED);
        return false;
    }

    if ((spec->flags & WE_TEX_FLAG_IS_VIDEO_TEXTURE) != 0u) {
        set_err(out_err, WE_WRITER_ERR_UNSUPPORTED_FMT);
        return false;
    }

    if (spec->image_count == 0u || spec->image_count > WE_TEX_MAX_IMAGES) {
        set_err(out_err,
                spec->image_count == 0u ? WE_WRITER_ERR_INVALID_ARG : WE_WRITER_ERR_LIMIT_EXCEEDED);
        return false;
    }
    if (!spec->images) {
        set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
        return false;
    }

    for (uint32_t i = 0u; i < spec->image_count; ++i) {
        const we_tex_encode_image_t *img = &spec->images[i];
        if (img->mip_count == 0u || img->mip_count > WE_TEX_MAX_MIPS) {
            set_err(out_err, img->mip_count == 0u ? WE_WRITER_ERR_INVALID_ARG
                                                  : WE_WRITER_ERR_LIMIT_EXCEEDED);
            return false;
        }
        if (!img->mips) {
            set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
            return false;
        }
        if ((spec->texture_width != 0u && img->mips[0].width != spec->texture_width) ||
            (spec->texture_height != 0u && img->mips[0].height != spec->texture_height)) {
            set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
            return false;
        }

        for (uint32_t m = 0u; m < img->mip_count; ++m) {
            const we_tex_encode_mip_t *mip = &img->mips[m];
            if (mip->width == 0u || mip->height == 0u || mip->width > WE_TEX_MAX_DIM ||
                mip->height > WE_TEX_MAX_DIM || !mip->pixels) {
                set_err(out_err,
                        !mip->pixels ? WE_WRITER_ERR_INVALID_ARG : WE_WRITER_ERR_LIMIT_EXCEEDED);
                return false;
            }
            uint64_t expected = (uint64_t)mip->width * (uint64_t)mip->height * (uint64_t)bpp;
            /* Reject payloads above the parser decompression ceiling so output always loads back.
             */
            if (expected > WE_TEX_MAX_DECOMP_SIZE) {
                set_err(out_err, WE_WRITER_ERR_OVERFLOW);
                return false;
            }
            if ((uint64_t)mip->byte_size != expected) {
                set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
                return false;
            }
        }
    }

    if ((spec->flags & WE_TEX_FLAG_IS_GIF) != 0u) {
        if (spec->frame_count == 0u || !spec->frames || spec->gif_width == 0u ||
            spec->gif_height == 0u) {
            set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
            return false;
        }
        if (spec->frame_count > WE_TEX_MAX_FRAMES || spec->gif_width > WE_TEX_MAX_DIM ||
            spec->gif_height > WE_TEX_MAX_DIM) {
            set_err(out_err, WE_WRITER_ERR_LIMIT_EXCEEDED);
            return false;
        }
        for (uint32_t f = 0u; f < spec->frame_count; ++f) {
            const we_tex_frame_t *fr = &spec->frames[f];
            if (fr->image_id < 0 || (uint32_t)fr->image_id >= spec->image_count) {
                set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
                return false;
            }
            if (!isfinite(fr->frametime) || !isfinite(fr->x) || !isfinite(fr->y) ||
                !isfinite(fr->width) || !isfinite(fr->width_y) || !isfinite(fr->height_x) ||
                !isfinite(fr->height)) {
                set_err(out_err, WE_WRITER_ERR_NON_FINITE);
                return false;
            }
            if (fr->frametime <= 0.0f) {
                set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
                return false;
            }
        }
    } else if (spec->frame_count != 0u) {
        set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
        return false;
    }

    return true;
}

static bool append_texb_images(const we_tex_encode_spec_t *spec, uint8_t **buf, size_t *len,
                               size_t *cap, we_asset_writer_err_t *out_err)
{
    for (uint32_t i = 0u; i < spec->image_count; ++i) {
        const we_tex_encode_image_t *img = &spec->images[i];
        uint8_t mc_le[4];
        write_i32_le(mc_le, (int32_t)img->mip_count);
        if (!append_bytes(buf, len, cap, mc_le, sizeof(mc_le))) {
            set_err(out_err, WE_WRITER_ERR_OOM);
            return false;
        }

        for (uint32_t m = 0u; m < img->mip_count; ++m) {
            const we_tex_encode_mip_t *mip = &img->mips[m];
            size_t decomp_sz = mip->byte_size;
            if (*len > SIZE_MAX - 20u - decomp_sz ||
                !buf_reserve(buf, cap, *len + 20u + decomp_sz)) {
                set_err(out_err, WE_WRITER_ERR_OOM);
                return false;
            }

            uint32_t is_lz4 = 0u;
            uint32_t byte_count = (uint32_t)decomp_sz;
            if (spec->enable_lz4 && decomp_sz >= 13u) {
                /* Require strictly smaller output so incompressible mips store raw without
                 * overhead. */
                size_t comp_sz = we_lz4_compress_block(mip->pixels, decomp_sz, *buf + *len + 20u,
                                                       decomp_sz - 1u);
                if (comp_sz > 0u && comp_sz < decomp_sz) {
                    is_lz4 = 1u;
                    byte_count = (uint32_t)comp_sz;
                }
            }
            if (is_lz4 == 0u) {
                memcpy(*buf + *len + 20u, mip->pixels, decomp_sz);
            }

            write_u32_le(*buf + *len, mip->width);
            write_u32_le(*buf + *len + 4u, mip->height);
            write_u32_le(*buf + *len + 8u, is_lz4);
            write_u32_le(*buf + *len + 12u, (uint32_t)decomp_sz);
            write_u32_le(*buf + *len + 16u, byte_count);
            *len += 20u + (size_t)byte_count;
        }
    }
    return true;
}

static bool append_texs_frames(const we_tex_encode_spec_t *spec, uint8_t **buf, size_t *len,
                               size_t *cap, we_asset_writer_err_t *out_err)
{
    uint8_t texs_hdr[9u + 12u];
    memcpy(texs_hdr, "TEXS0003", 9u);
    write_u32_le(texs_hdr + 9u, spec->frame_count);
    write_u32_le(texs_hdr + 13u, spec->gif_width);
    write_u32_le(texs_hdr + 17u, spec->gif_height);
    if (!append_bytes(buf, len, cap, texs_hdr, sizeof(texs_hdr))) {
        set_err(out_err, WE_WRITER_ERR_OOM);
        return false;
    }

    size_t frames_bytes = (size_t)spec->frame_count * 32u;
    if (*len > SIZE_MAX - frames_bytes || !buf_reserve(buf, cap, *len + frames_bytes)) {
        set_err(out_err, WE_WRITER_ERR_OOM);
        return false;
    }
    for (uint32_t f = 0u; f < spec->frame_count; ++f) {
        const we_tex_frame_t *fr = &spec->frames[f];
        uint8_t *dst = *buf + *len;
        write_i32_le(dst, fr->image_id);
        write_f32_le(dst + 4u, fr->frametime);
        write_f32_le(dst + 8u, fr->x);
        write_f32_le(dst + 12u, fr->y);
        write_f32_le(dst + 16u, fr->width);
        write_f32_le(dst + 20u, fr->width_y);
        write_f32_le(dst + 24u, fr->height_x);
        write_f32_le(dst + 28u, fr->height);
        *len += 32u;
    }
    return true;
}

bool we_tex_encode_v5(const we_tex_encode_spec_t *spec, uint8_t **out_buf, size_t *out_size,
                      we_asset_writer_err_t *out_err)
{
    if (!spec || !out_buf || !out_size) {
        set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
        return false;
    }
    *out_buf = NULL;
    *out_size = 0u;

    size_t bpp = format_bpp(spec->format);
    if (bpp == 0u) {
        set_err(out_err, WE_WRITER_ERR_UNSUPPORTED_FMT);
        return false;
    }

    if (!validate_spec(spec, bpp, out_err)) {
        return false;
    }

    uint8_t *buf = NULL;
    size_t len = 0u;
    size_t cap = 0u;

    /* Header: "TEXV0005\0" (9) + "TEXI0001\0" (9) + 28 B TEXI + "TEXB0003\0" (9) + 8 B TEXB. */
    uint8_t hdr[9u + 9u + 28u + 9u + 8u];
    memcpy(hdr, "TEXV0005", 9u);
    memcpy(hdr + 9u, "TEXI0001", 9u);
    write_u32_le(hdr + 18u, (uint32_t)spec->format);
    write_u32_le(hdr + 22u, spec->flags);
    write_u32_le(hdr + 26u, spec->texture_width);
    write_u32_le(hdr + 30u, spec->texture_height);
    write_u32_le(hdr + 34u, spec->image_width);
    write_u32_le(hdr + 38u, spec->image_height);
    write_u32_le(hdr + 42u, 0u);
    memcpy(hdr + 46u, "TEXB0003", 9u);
    write_i32_le(hdr + 55u, (int32_t)spec->image_count);
    /* Raw uncompressed/LZ4 pixel payloads use WE_FIF_UNKNOWN (-1) in TEXB0003 with no mp4 flag. */
    write_i32_le(hdr + 59u, WE_FIF_UNKNOWN);

    if (!append_bytes(&buf, &len, &cap, hdr, sizeof(hdr)) ||
        !append_texb_images(spec, &buf, &len, &cap, out_err) ||
        (((spec->flags & WE_TEX_FLAG_IS_GIF) != 0u) &&
         !append_texs_frames(spec, &buf, &len, &cap, out_err))) {
        free(buf);
        if (out_err && *out_err == WE_WRITER_OK) {
            set_err(out_err, WE_WRITER_ERR_OOM);
        }
        return false;
    }

    *out_buf = buf;
    *out_size = len;
    set_err(out_err, WE_WRITER_OK);
    return true;
}

bool we_tex_write_v5_file(const char *out_path, const we_tex_encode_spec_t *spec,
                          we_asset_writer_err_t *out_err)
{
    if (!out_path || out_path[0] == '\0' || !spec) {
        set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
        return false;
    }

    uint8_t *buf = NULL;
    size_t size = 0u;
    if (!we_tex_encode_v5(spec, &buf, &size, out_err)) {
        return false;
    }

    bool ok = we_write_file_atomic(out_path, buf, size, out_err);
    free(buf);
    return ok;
}

bool we_tex_generate_mips(we_tex_format_t format, uint32_t width, uint32_t height,
                          const uint8_t *level0_pixels, we_tex_encode_mip_t **out_mips,
                          uint32_t *out_mip_count)
{
    if (!level0_pixels || !out_mips || !out_mip_count) {
        return false;
    }
    *out_mips = NULL;
    *out_mip_count = 0u;

    size_t bpp = format_bpp(format);
    if (bpp == 0u || width == 0u || height == 0u || width > WE_TEX_MAX_DIM ||
        height > WE_TEX_MAX_DIM) {
        return false;
    }

    uint64_t l0_bytes = (uint64_t)width * (uint64_t)height * (uint64_t)bpp;
    if (l0_bytes > WE_TEX_MAX_DECOMP_SIZE) {
        return false;
    }

    uint32_t count = 1u;
    uint32_t cw = width;
    uint32_t ch = height;
    while (cw > 1u || ch > 1u) {
        cw = (cw > 1u) ? (cw >> 1u) : 1u;
        ch = (ch > 1u) ? (ch >> 1u) : 1u;
        ++count;
        if (count > WE_TEX_MAX_MIPS) {
            return false;
        }
    }

    we_tex_encode_mip_t *mips = (we_tex_encode_mip_t *)calloc(count, sizeof(*mips));
    if (!mips) {
        return false;
    }

    uint8_t *l0_copy = (uint8_t *)malloc((size_t)l0_bytes);
    if (!l0_copy) {
        free(mips);
        return false;
    }
    memcpy(l0_copy, level0_pixels, (size_t)l0_bytes);
    mips[0].width = width;
    mips[0].height = height;
    mips[0].pixels = l0_copy;
    mips[0].byte_size = (size_t)l0_bytes;

    for (uint32_t level = 1u; level < count; ++level) {
        const we_tex_encode_mip_t *prev = &mips[level - 1u];
        uint32_t pw = prev->width;
        uint32_t ph = prev->height;
        uint32_t nw = (pw > 1u) ? (pw >> 1u) : 1u;
        uint32_t nh = (ph > 1u) ? (ph >> 1u) : 1u;
        size_t nbytes = (size_t)nw * (size_t)nh * bpp;

        uint8_t *dst = (uint8_t *)malloc(nbytes);
        if (!dst) {
            we_tex_free_generated_mips(mips, level);
            return false;
        }

        for (uint32_t y = 0u; y < nh; ++y) {
            uint32_t sy0 = y * 2u;
            uint32_t sy1 = (sy0 + 1u < ph) ? (sy0 + 1u) : sy0;
            const uint8_t *row0 = prev->pixels + (size_t)sy0 * (size_t)pw * bpp;
            const uint8_t *row1 = prev->pixels + (size_t)sy1 * (size_t)pw * bpp;
            uint8_t *drow = dst + (size_t)y * (size_t)nw * bpp;

            for (uint32_t x = 0u; x < nw; ++x) {
                uint32_t sx0 = x * 2u;
                uint32_t sx1 = (sx0 + 1u < pw) ? (sx0 + 1u) : sx0;
                const uint8_t *p00 = row0 + (size_t)sx0 * bpp;
                const uint8_t *p10 = row0 + (size_t)sx1 * bpp;
                const uint8_t *p01 = row1 + (size_t)sx0 * bpp;
                const uint8_t *p11 = row1 + (size_t)sx1 * bpp;
                uint8_t *dp = drow + (size_t)x * bpp;

                for (size_t c = 0u; c < bpp; ++c) {
                    uint32_t sum = (uint32_t)p00[c] + (uint32_t)p10[c] + (uint32_t)p01[c] +
                                   (uint32_t)p11[c] + 2u;
                    dp[c] = (uint8_t)(sum >> 2u);
                }
            }
        }

        mips[level].width = nw;
        mips[level].height = nh;
        mips[level].pixels = dst;
        mips[level].byte_size = nbytes;
    }

    *out_mips = mips;
    *out_mip_count = count;
    return true;
}

void we_tex_free_generated_mips(we_tex_encode_mip_t *mips, uint32_t mip_count)
{
    if (!mips) {
        return;
    }
    for (uint32_t i = 0u; i < mip_count; ++i) {
        free((void *)(uintptr_t)mips[i].pixels);
    }
    free(mips);
}
