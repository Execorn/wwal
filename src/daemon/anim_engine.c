// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/anim_engine.h"

#include "waywal/log.h"

#include <dlfcn.h>
#if defined(__has_include) && __has_include(<stdckdint.h>)
#include <stdckdint.h>
#else
#define ckd_add(r, a, b) __builtin_add_overflow((a), (b), (r))
#define ckd_sub(r, a, b) __builtin_sub_overflow((a), (b), (r))
#define ckd_mul(r, a, b) __builtin_mul_overflow((a), (b), (r))
#endif
#include <stdlib.h>
#include <string.h>

#define GIF_MAX_LZW_BITS   12
#define GIF_LZW_TABLE_SIZE (1 << GIF_MAX_LZW_BITS)

typedef struct {
    uint16_t left;
    uint16_t top;
    uint16_t width;
    uint16_t height;
    uint32_t delay_ms;
    uint8_t disposal;
    bool has_transparency;
    uint8_t trans_index;
    bool interlaced;
    bool has_lct;
    uint16_t lct_count;
    uint8_t lct[256 * 3];
    size_t lzw_offset;
} gif_frame_meta_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t x_offset;
    uint32_t y_offset;
    uint32_t delay_ms;
    uint8_t dispose_op;
    uint8_t blend_op;
    uint8_t *zdata;
    size_t zsize;
    size_t zcap;
} apng_frame_t;

typedef struct {
    const uint8_t *bytes;
    size_t size;
} webp_data_t;

typedef struct {
    int color_mode;
    int use_threads;
    uint32_t padding[7];
} webp_anim_options_t;

typedef struct {
    uint32_t canvas_width;
    uint32_t canvas_height;
    uint32_t loop_count;
    uint32_t bgcolor;
    uint32_t frame_count;
    uint32_t pad[4];
} webp_anim_info_t;

typedef int (*pfn_WebPAnimDecoderOptionsInitInternal)(webp_anim_options_t *, int);
typedef void *(*pfn_WebPAnimDecoderNewInternal)(const webp_data_t *, const webp_anim_options_t *,
                                                int);
typedef int (*pfn_WebPAnimDecoderGetInfo)(const void *, webp_anim_info_t *);
typedef int (*pfn_WebPAnimDecoderGetNext)(void *, uint8_t **, int *);
typedef int (*pfn_WebPAnimDecoderHasMoreFrames)(const void *);
typedef void (*pfn_WebPAnimDecoderReset)(void *);
typedef void (*pfn_WebPAnimDecoderDelete)(void *);

struct waywal_anim_decoder {
    waywal_anim_format_t format;
    uint8_t *raw_data;
    size_t raw_size;

    uint32_t canvas_w;
    uint32_t canvas_h;
    uint32_t frame_count;
    uint32_t current_frame;

    uint8_t *canvas;
    uint8_t *prev_canvas;
    uint8_t *scratch_buf;
    size_t scratch_cap;

    uint8_t prev_disposal;
    uint32_t prev_rect_x;
    uint32_t prev_rect_y;
    uint32_t prev_rect_w;
    uint32_t prev_rect_h;

    bool gif_has_gct;
    uint16_t gif_gct_count;
    uint8_t gif_gct[256 * 3];
    uint8_t gif_bg_index;
    gif_frame_meta_t *gif_frames;

    uint8_t png_bit_depth;
    uint8_t png_color_type;
    uint8_t png_plte[256 * 3];
    uint16_t png_plte_count;
    uint8_t png_trns[256];
    uint16_t png_trns_count;
    apng_frame_t *apng_frames;

    void *webp_dl_demux;
    void *webp_dl_codec;
    void *webp_dec;
    webp_data_t webp_raw;
    int webp_prev_timestamp;
    pfn_WebPAnimDecoderGetNext fn_webp_next;
    pfn_WebPAnimDecoderHasMoreFrames fn_webp_has_more;
    pfn_WebPAnimDecoderReset fn_webp_reset;
    pfn_WebPAnimDecoderDelete fn_webp_delete;
};

static uint16_t read_le16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_be16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | (uint16_t)p[1]; }

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static bool skip_gif_subblocks(const uint8_t *data, size_t size, size_t *pos)
{
    while (*pos < size) {
        uint8_t block_len = data[(*pos)++];
        if (block_len == 0) {
            return true;
        }
        if (*pos + block_len > size) {
            return false;
        }
        *pos += block_len;
    }
    return false;
}

static bool probe_gif_frames(const uint8_t *data, size_t size, uint32_t *out_frames,
                             bool *out_has_anim_ext)
{
    if (size < 14) {
        return false;
    }
    if (memcmp(data, "GIF87a", 6) != 0 && memcmp(data, "GIF89a", 6) != 0) {
        return false;
    }

    uint8_t packed = data[10];
    size_t pos = 13;
    if (packed & 0x80u) {
        uint32_t gct_count = 1u << ((packed & 0x07u) + 1u);
        if (pos + gct_count * 3u > size) {
            return false;
        }
        pos += gct_count * 3u;
    }

    uint32_t frames = 0;
    bool has_anim = false;
    while (pos < size) {
        uint8_t marker = data[pos++];
        if (marker == 0x3Bu) {
            break;
        } else if (marker == 0x21u) {
            if (pos >= size) {
                break;
            }
            uint8_t ext_label = data[pos++];
            if (ext_label == 0xF9u) {
                has_anim = true;
            } else if (ext_label == 0xFFu) {
                has_anim = true;
            }
            if (!skip_gif_subblocks(data, size, &pos)) {
                break;
            }
        } else if (marker == 0x2Cu) {
            if (pos + 9 > size) {
                break;
            }
            uint8_t img_packed = data[pos + 8];
            pos += 9;
            if (img_packed & 0x80u) {
                uint32_t lct_count = 1u << ((img_packed & 0x07u) + 1u);
                if (pos + lct_count * 3u > size) {
                    break;
                }
                pos += lct_count * 3u;
            }
            if (pos >= size) {
                break;
            }
            pos++;
            if (!skip_gif_subblocks(data, size, &pos)) {
                break;
            }
            frames++;
        } else {
            break;
        }
    }

    if (out_frames) {
        *out_frames = frames;
    }
    if (out_has_anim_ext) {
        *out_has_anim_ext = has_anim;
    }
    return frames > 0;
}

static bool probe_apng(const uint8_t *data, size_t size)
{
    static const uint8_t png_sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (size < 33 || memcmp(data, png_sig, 8) != 0) {
        return false;
    }

    size_t pos = 8;
    while (pos + 12 <= size) {
        uint32_t chunk_len = read_be32(data + pos);
        const uint8_t *chunk_type = data + pos + 4;
        if (pos + 12 + (size_t)chunk_len > size) {
            return false;
        }
        if (memcmp(chunk_type, "acTL", 4) == 0 && chunk_len >= 8) {
            uint32_t num_frames = read_be32(data + pos + 8);
            return num_frames > 0;
        }
        if (memcmp(chunk_type, "IDAT", 4) == 0 || memcmp(chunk_type, "IEND", 4) == 0) {
            return false;
        }
        pos += 12 + (size_t)chunk_len;
    }
    return false;
}

static bool probe_webp_anim(const uint8_t *data, size_t size)
{
    if (size < 30) {
        return false;
    }
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WEBP", 4) != 0) {
        return false;
    }
    if (memcmp(data + 12, "VP8X", 4) == 0) {
        uint8_t flags = data[20];
        if (flags & 0x02u) {
            return true;
        }
    }
    size_t pos = 12;
    while (pos + 8 <= size) {
        uint32_t chunk_size = read_le32(data + pos + 4);
        if (memcmp(data + pos, "ANIM", 4) == 0 || memcmp(data + pos, "ANMF", 4) == 0) {
            return true;
        }
        size_t padded = (size_t)chunk_size + (chunk_size & 1u);
        if (pos + 8 + padded > size) {
            break;
        }
        pos += 8 + padded;
    }
    return false;
}

waywal_anim_format_t waywal_anim_detect_format(const uint8_t *data, size_t size)
{
    if (!data || size < 12) {
        return WAYWAL_ANIM_FORMAT_UNKNOWN;
    }
    uint32_t gif_frames = 0;
    bool gif_anim_ext = false;
    if (probe_gif_frames(data, size, &gif_frames, &gif_anim_ext)) {
        if (gif_frames >= 2 || gif_anim_ext) {
            return WAYWAL_ANIM_FORMAT_GIF;
        }
    }
    if (probe_apng(data, size)) {
        return WAYWAL_ANIM_FORMAT_APNG;
    }
    if (probe_webp_anim(data, size)) {
        return WAYWAL_ANIM_FORMAT_WEBP;
    }
    return WAYWAL_ANIM_FORMAT_UNKNOWN;
}

bool waywal_anim_probe(const uint8_t *data, size_t size)
{
    return waywal_anim_detect_format(data, size) != WAYWAL_ANIM_FORMAT_UNKNOWN;
}

static void apply_previous_disposal(waywal_anim_decoder_t *dec)
{
    if (dec->prev_disposal == 1 || dec->prev_disposal == 2) {
        for (uint32_t y = 0; y < dec->prev_rect_h; ++y) {
            uint32_t cy = dec->prev_rect_y + y;
            if (cy >= dec->canvas_h) {
                continue;
            }
            for (uint32_t x = 0; x < dec->prev_rect_w; ++x) {
                uint32_t cx = dec->prev_rect_x + x;
                if (cx >= dec->canvas_w) {
                    continue;
                }
                size_t idx = ((size_t)cy * dec->canvas_w + cx) * 4u;
                dec->canvas[idx + 0] = 0;
                dec->canvas[idx + 1] = 0;
                dec->canvas[idx + 2] = 0;
                dec->canvas[idx + 3] = 0;
            }
        }
    } else if (dec->prev_disposal == 3 && dec->prev_canvas) {
        memcpy(dec->canvas, dec->prev_canvas, (size_t)dec->canvas_w * dec->canvas_h * 4u);
    }
    dec->prev_disposal = 0;
}

static bool decode_gif_lzw(const uint8_t *data, size_t size, size_t lzw_offset,
                           uint8_t *out_indices, size_t expected_pixels)
{
    if (lzw_offset >= size) {
        return false;
    }
    uint8_t min_code_size = data[lzw_offset];
    if (min_code_size < 2 || min_code_size >= GIF_MAX_LZW_BITS) {
        return false;
    }

    uint16_t clear_code = (uint16_t)(1u << min_code_size);
    uint16_t eoi_code = (uint16_t)(clear_code + 1u);
    uint16_t next_code = (uint16_t)(eoi_code + 1u);
    uint8_t code_size = (uint8_t)(min_code_size + 1u);
    uint16_t code_mask = (uint16_t)((1u << code_size) - 1u);

    uint16_t prefix[GIF_LZW_TABLE_SIZE];
    uint8_t suffix[GIF_LZW_TABLE_SIZE];
    uint8_t first_byte[GIF_LZW_TABLE_SIZE];
    uint8_t stack[GIF_LZW_TABLE_SIZE];

    for (uint16_t i = 0; i < clear_code; ++i) {
        prefix[i] = 0xFFFFu;
        suffix[i] = (uint8_t)i;
        first_byte[i] = (uint8_t)i;
    }

    size_t pos = lzw_offset + 1;
    uint8_t block_rem = 0;
    uint32_t bit_buf = 0;
    uint32_t bit_count = 0;
    size_t out_pos = 0;
    uint16_t old_code = 0xFFFFu;

    while (out_pos < expected_pixels) {
        while (bit_count < code_size) {
            if (block_rem == 0) {
                if (pos >= size) {
                    goto done;
                }
                block_rem = data[pos++];
                if (block_rem == 0) {
                    goto done;
                }
            }
            if (pos >= size) {
                goto done;
            }
            bit_buf |= ((uint32_t)data[pos++]) << bit_count;
            bit_count += 8;
            block_rem--;
        }

        uint16_t code = (uint16_t)(bit_buf & code_mask);
        bit_buf >>= code_size;
        bit_count -= code_size;

        if (code == clear_code) {
            code_size = (uint8_t)(min_code_size + 1u);
            code_mask = (uint16_t)((1u << code_size) - 1u);
            next_code = (uint16_t)(eoi_code + 1u);
            old_code = 0xFFFFu;
            continue;
        }
        if (code == eoi_code) {
            break;
        }

        if (old_code == 0xFFFFu) {
            if (code >= clear_code) {
                return false;
            }
            out_indices[out_pos++] = (uint8_t)code;
            old_code = code;
            continue;
        }

        uint16_t in_code = code;
        size_t sp = 0;

        if (code >= next_code) {
            if (code > next_code || sp >= GIF_LZW_TABLE_SIZE) {
                return false;
            }
            stack[sp++] = first_byte[old_code];
            code = old_code;
        }

        while (code >= clear_code) {
            if (sp >= GIF_LZW_TABLE_SIZE - 1u) {
                return false;
            }
            stack[sp++] = suffix[code];
            uint16_t next = prefix[code];
            if (next >= code) {
                return false;
            }
            code = next;
        }
        stack[sp++] = (uint8_t)code;

        uint8_t first_char = (uint8_t)code;
        while (sp > 0 && out_pos < expected_pixels) {
            out_indices[out_pos++] = stack[--sp];
        }

        if (next_code < GIF_LZW_TABLE_SIZE) {
            prefix[next_code] = old_code;
            suffix[next_code] = first_char;
            first_byte[next_code] = first_byte[old_code];
            next_code++;
            if (next_code == (1u << code_size) && code_size < GIF_MAX_LZW_BITS) {
                code_size++;
                code_mask = (uint16_t)((1u << code_size) - 1u);
            }
        }
        old_code = in_code;
    }

done:
    if (out_pos < expected_pixels) {
        memset(out_indices + out_pos, 0, expected_pixels - out_pos);
    }
    return out_pos > 0;
}

static uint8_t *ensure_scratch_buf(waywal_anim_decoder_t *dec, size_t needed);

static bool init_gif_decoder(waywal_anim_decoder_t *dec)
{
    const uint8_t *data = dec->raw_data;
    size_t size = dec->raw_size;
    if (size < 14) {
        return false;
    }

    dec->canvas_w = read_le16(data + 6);
    dec->canvas_h = read_le16(data + 8);
    if (dec->canvas_w == 0 || dec->canvas_h == 0 || dec->canvas_w > 16384 ||
        dec->canvas_h > 16384) {
        return false;
    }

    uint8_t packed = data[10];
    dec->gif_bg_index = data[11];
    size_t pos = 13;

    if (packed & 0x80u) {
        dec->gif_has_gct = true;
        dec->gif_gct_count = (uint16_t)(1u << ((packed & 0x07u) + 1u));
        if (pos + (size_t)dec->gif_gct_count * 3u > size) {
            return false;
        }
        memcpy(dec->gif_gct, data + pos, (size_t)dec->gif_gct_count * 3u);
        pos += (size_t)dec->gif_gct_count * 3u;
    }

    uint32_t cap = 16;
    dec->gif_frames = (gif_frame_meta_t *)calloc(cap, sizeof(gif_frame_meta_t));
    if (!dec->gif_frames) {
        return false;
    }

    uint8_t cur_disposal = 0;
    bool cur_has_trans = false;
    uint8_t cur_trans_idx = 0;
    uint32_t cur_delay_ms = 100;

    while (pos < size) {
        uint8_t marker = data[pos++];
        if (marker == 0x3Bu) {
            break;
        } else if (marker == 0x21u) {
            if (pos >= size) {
                break;
            }
            uint8_t ext = data[pos++];
            if (ext == 0xF9u && pos + 6 <= size && data[pos] == 4) {
                uint8_t gce_packed = data[pos + 1];
                uint16_t delay_cs = read_le16(data + pos + 2);
                uint8_t trans_idx = data[pos + 4];
                cur_disposal = (uint8_t)((gce_packed >> 2) & 0x07u);
                cur_has_trans = (gce_packed & 0x01u) != 0;
                cur_trans_idx = trans_idx;
                /* Browsers clamp GIF delays < 20ms (0 or 1 cs) to 100ms to prevent CPU spin. */
                cur_delay_ms = (delay_cs < 2) ? 100u : (uint32_t)delay_cs * 10u;
            }
            if (!skip_gif_subblocks(data, size, &pos)) {
                break;
            }
        } else if (marker == 0x2Cu) {
            if (pos + 9 > size) {
                break;
            }
            if (dec->frame_count >= cap) {
                uint32_t new_cap = cap * 2u;
                gif_frame_meta_t *nf = (gif_frame_meta_t *)realloc(
                    dec->gif_frames, (size_t)new_cap * sizeof(gif_frame_meta_t));
                if (!nf) {
                    return false;
                }
                dec->gif_frames = nf;
                cap = new_cap;
            }

            gif_frame_meta_t *f = &dec->gif_frames[dec->frame_count];
            memset(f, 0, sizeof(*f));
            f->left = read_le16(data + pos);
            f->top = read_le16(data + pos + 2);
            f->width = read_le16(data + pos + 4);
            f->height = read_le16(data + pos + 6);
            uint8_t img_packed = data[pos + 8];
            pos += 9;

            f->interlaced = (img_packed & 0x40u) != 0;
            f->disposal = cur_disposal;
            f->has_transparency = cur_has_trans;
            f->trans_index = cur_trans_idx;
            f->delay_ms = cur_delay_ms;

            if (img_packed & 0x80u) {
                f->has_lct = true;
                f->lct_count = (uint16_t)(1u << ((img_packed & 0x07u) + 1u));
                if (pos + (size_t)f->lct_count * 3u > size) {
                    break;
                }
                memcpy(f->lct, data + pos, (size_t)f->lct_count * 3u);
                pos += (size_t)f->lct_count * 3u;
            }

            if (pos >= size) {
                break;
            }
            f->lzw_offset = pos;
            pos++;
            if (!skip_gif_subblocks(data, size, &pos)) {
                break;
            }

            if (f->width > 0 && f->height > 0) {
                dec->frame_count++;
            }

            cur_disposal = 0;
            cur_has_trans = false;
            cur_trans_idx = 0;
            cur_delay_ms = 100;
        } else {
            break;
        }
    }

    if (dec->frame_count == 0) {
        return false;
    }

    size_t num_pixels = 0;
    size_t canvas_bytes = 0;
    if (ckd_mul(&num_pixels, (size_t)dec->canvas_w, (size_t)dec->canvas_h) ||
        ckd_mul(&canvas_bytes, num_pixels, (size_t)4u)) {
        return false;
    }
    dec->canvas = (uint8_t *)calloc(1, canvas_bytes);
    if (!dec->canvas) {
        return false;
    }

    size_t max_frame_pixels = 0;
    for (uint32_t i = 0; i < dec->frame_count; ++i) {
        size_t fp = (size_t)dec->gif_frames[i].width * (size_t)dec->gif_frames[i].height;
        if (fp > max_frame_pixels) {
            max_frame_pixels = fp;
        }
    }
    if (max_frame_pixels > 0 && !ensure_scratch_buf(dec, max_frame_pixels)) {
        return false;
    }
    return true;
}

static uint8_t *ensure_scratch_buf(waywal_anim_decoder_t *dec, size_t needed)
{
    if (!dec || needed == 0)
        return NULL;
    if (dec->scratch_buf && dec->scratch_cap >= needed)
        return dec->scratch_buf;
    uint8_t *nb = (uint8_t *)realloc(dec->scratch_buf, needed);
    if (!nb)
        return NULL;
    dec->scratch_buf = nb;
    dec->scratch_cap = needed;
    return dec->scratch_buf;
}

static bool step_gif_frame(waywal_anim_decoder_t *dec, const uint8_t **out_rgba, uint32_t *out_w,
                           uint32_t *out_h, uint32_t *out_delay_ms)
{
    if (dec->current_frame >= dec->frame_count) {
        waywal_anim_reset(dec);
    }

    apply_previous_disposal(dec);

    const gif_frame_meta_t *f = &dec->gif_frames[dec->current_frame];
    if (f->disposal == 3) {
        size_t canvas_bytes = (size_t)dec->canvas_w * dec->canvas_h * 4u;
        if (!dec->prev_canvas) {
            dec->prev_canvas = (uint8_t *)calloc(1, canvas_bytes);
        }
        if (dec->prev_canvas) {
            memcpy(dec->prev_canvas, dec->canvas, canvas_bytes);
        }
    }

    size_t num_pixels = (size_t)f->width * f->height;
    uint8_t *indices = ensure_scratch_buf(dec, num_pixels);
    if (!indices) {
        return false;
    }

    if (!decode_gif_lzw(dec->raw_data, dec->raw_size, f->lzw_offset, indices, num_pixels)) {
        return false;
    }

    const uint8_t *palette = f->has_lct ? f->lct : dec->gif_gct;
    uint16_t palette_count = f->has_lct ? f->lct_count : dec->gif_gct_count;

    static const uint8_t interlace_start[4] = {0, 4, 2, 1};
    static const uint8_t interlace_step[4] = {8, 8, 4, 2};

    size_t src_row_idx = 0;
    for (int pass = 0; pass < (f->interlaced ? 4 : 1); ++pass) {
        uint32_t y_start = f->interlaced ? interlace_start[pass] : 0;
        uint32_t y_step = f->interlaced ? interlace_step[pass] : 1;

        for (uint32_t y = y_start; y < f->height; y += y_step) {
            uint32_t dst_y = (uint32_t)f->top + y;
            const uint8_t *row_in = indices + src_row_idx * f->width;
            src_row_idx++;

            if (dst_y >= dec->canvas_h) {
                continue;
            }

            for (uint32_t x = 0; x < f->width; ++x) {
                uint32_t dst_x = (uint32_t)f->left + x;
                if (dst_x >= dec->canvas_w) {
                    continue;
                }
                uint8_t color_idx = row_in[x];
                if (f->has_transparency && color_idx == f->trans_index) {
                    continue;
                }
                size_t dst_off = ((size_t)dst_y * dec->canvas_w + dst_x) * 4u;
                if (color_idx < palette_count) {
                    dec->canvas[dst_off + 0] = palette[color_idx * 3u + 0];
                    dec->canvas[dst_off + 1] = palette[color_idx * 3u + 1];
                    dec->canvas[dst_off + 2] = palette[color_idx * 3u + 2];
                    dec->canvas[dst_off + 3] = 255;
                }
            }
        }
    }

    /* GIF disposal 2 clears to background/transparent; map 2->1 and 3->3 in shared state. */
    dec->prev_disposal = (f->disposal == 2) ? 1 : (f->disposal == 3 ? 3 : 0);
    dec->prev_rect_x = f->left;
    dec->prev_rect_y = f->top;
    dec->prev_rect_w = f->width;
    dec->prev_rect_h = f->height;
    dec->current_frame++;

    if (out_rgba) {
        *out_rgba = dec->canvas;
    }
    if (out_w) {
        *out_w = dec->canvas_w;
    }
    if (out_h) {
        *out_h = dec->canvas_h;
    }
    if (out_delay_ms) {
        *out_delay_ms = f->delay_ms;
    }
    return true;
}

/* Native RFC 1951 / RFC 1950 inflate for zero-dependency APNG IDAT/fdAT decoding. */
typedef struct {
    const uint8_t *src;
    size_t size;
    size_t pos;
    uint32_t bitbuf;
    uint32_t bitcnt;
} deflate_bits_t;

typedef struct {
    uint16_t counts[16];
    uint16_t symbols[288];
} huff_table_t;

static uint32_t bits_pull(deflate_bits_t *b, uint32_t n)
{
    while (b->bitcnt < n) {
        if (b->pos >= b->size) {
            return 0;
        }
        b->bitbuf |= ((uint32_t)b->src[b->pos++]) << b->bitcnt;
        b->bitcnt += 8;
    }
    uint32_t val = b->bitbuf & ((1u << n) - 1u);
    b->bitbuf >>= n;
    b->bitcnt -= n;
    return val;
}

static bool huff_build(huff_table_t *h, const uint8_t *lengths, uint32_t num_syms)
{
    memset(h->counts, 0, sizeof(h->counts));
    for (uint32_t i = 0; i < num_syms; ++i) {
        if (lengths[i] > 15) {
            return false;
        }
        h->counts[lengths[i]]++;
    }
    h->counts[0] = 0;

    uint16_t offs[16];
    uint16_t sum = 0;
    for (int len = 1; len <= 15; ++len) {
        offs[len] = sum;
        sum = (uint16_t)(sum + h->counts[len]);
    }
    for (uint32_t i = 0; i < num_syms; ++i) {
        uint8_t l = lengths[i];
        if (l != 0) {
            h->symbols[offs[l]++] = (uint16_t)i;
        }
    }
    return true;
}

static int huff_decode(deflate_bits_t *b, const huff_table_t *h)
{
    int code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len <= 15; ++len) {
        code |= (int)bits_pull(b, 1);
        int count = h->counts[len];
        if (code - count < first) {
            return h->symbols[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static bool zlib_inflate_buf(const uint8_t *src, size_t src_size, uint8_t *dst, size_t dst_cap)
{
    if (src_size < 6) {
        return false;
    }
    uint8_t cmf = src[0];
    uint8_t flg = src[1];
    if ((cmf & 0x0Fu) != 8 || ((cmf * 256u + flg) % 31u) != 0) {
        return false;
    }
    size_t start_offset = (flg & 0x20u) ? 6 : 2;
    if (start_offset >= src_size) {
        return false;
    }

    deflate_bits_t b = {
        .src = src,
        .size = src_size - 4,
        .pos = start_offset,
        .bitbuf = 0,
        .bitcnt = 0,
    };

    static const uint16_t len_base[29] = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                          15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                          67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                          2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dist_base[30] = {
        1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                           6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    static const uint8_t clen_order[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                           11, 4,  12, 3, 13, 2, 14, 1, 15};

    size_t out_pos = 0;
    uint32_t bfinal = 0;

    do {
        bfinal = bits_pull(&b, 1);
        uint32_t btype = bits_pull(&b, 2);

        if (btype == 0) {
            b.bitbuf = 0;
            b.bitcnt = 0;
            if (b.pos + 4 > b.size) {
                return false;
            }
            uint16_t blk_len = read_le16(b.src + b.pos);
            uint16_t blk_nlen = read_le16(b.src + b.pos + 2);
            b.pos += 4;
            if ((uint16_t)~blk_len != blk_nlen) {
                return false;
            }
            if (b.pos + blk_len > b.size || out_pos + blk_len > dst_cap) {
                return false;
            }
            memcpy(dst + out_pos, b.src + b.pos, blk_len);
            b.pos += blk_len;
            out_pos += blk_len;
        } else if (btype == 1 || btype == 2) {
            huff_table_t litlen_tab;
            huff_table_t dist_tab;
            uint8_t lengths[320];

            if (btype == 1) {
                for (int i = 0; i < 144; ++i)
                    lengths[i] = 8;
                for (int i = 144; i < 256; ++i)
                    lengths[i] = 9;
                for (int i = 256; i < 280; ++i)
                    lengths[i] = 7;
                for (int i = 280; i < 288; ++i)
                    lengths[i] = 8;
                for (int i = 0; i < 32; ++i)
                    lengths[288 + i] = 5;
                huff_build(&litlen_tab, lengths, 288);
                huff_build(&dist_tab, lengths + 288, 32);
            } else {
                uint32_t hlit = bits_pull(&b, 5) + 257u;
                uint32_t hdist = bits_pull(&b, 5) + 1u;
                uint32_t hclen = bits_pull(&b, 4) + 4u;
                if (hlit > 286 || hdist > 30) {
                    return false;
                }
                uint8_t clen[19];
                memset(clen, 0, sizeof(clen));
                for (uint32_t i = 0; i < hclen; ++i) {
                    clen[clen_order[i]] = (uint8_t)bits_pull(&b, 3);
                }
                huff_table_t clen_tab;
                if (!huff_build(&clen_tab, clen, 19)) {
                    return false;
                }
                uint32_t idx = 0;
                uint32_t total = hlit + hdist;
                while (idx < total) {
                    int sym = huff_decode(&b, &clen_tab);
                    if (sym < 0) {
                        return false;
                    }
                    if (sym < 16) {
                        lengths[idx++] = (uint8_t)sym;
                    } else {
                        uint32_t rep = 0;
                        uint8_t val = 0;
                        if (sym == 16) {
                            if (idx == 0) {
                                return false;
                            }
                            val = lengths[idx - 1];
                            rep = 3u + bits_pull(&b, 2);
                        } else if (sym == 17) {
                            rep = 3u + bits_pull(&b, 3);
                        } else {
                            rep = 11u + bits_pull(&b, 7);
                        }
                        if (idx + rep > total) {
                            return false;
                        }
                        while (rep-- > 0) {
                            lengths[idx++] = val;
                        }
                    }
                }
                if (!huff_build(&litlen_tab, lengths, hlit) ||
                    !huff_build(&dist_tab, lengths + hlit, hdist)) {
                    return false;
                }
            }

            for (;;) {
                int sym = huff_decode(&b, &litlen_tab);
                if (sym < 0) {
                    return false;
                }
                if (sym < 256) {
                    if (out_pos >= dst_cap) {
                        return false;
                    }
                    dst[out_pos++] = (uint8_t)sym;
                } else if (sym == 256) {
                    break;
                } else {
                    sym -= 257;
                    if (sym >= 29) {
                        return false;
                    }
                    uint32_t match_len = len_base[sym] + bits_pull(&b, len_extra[sym]);
                    int dsym = huff_decode(&b, &dist_tab);
                    if (dsym < 0 || dsym >= 30) {
                        return false;
                    }
                    uint32_t dist = dist_base[dsym] + bits_pull(&b, dist_extra[dsym]);
                    if (dist > out_pos || out_pos + match_len > dst_cap) {
                        return false;
                    }
                    for (uint32_t i = 0; i < match_len; ++i) {
                        dst[out_pos] = dst[out_pos - dist];
                        out_pos++;
                    }
                }
            }
        } else {
            return false;
        }
    } while (!bfinal);

    return out_pos == dst_cap;
}

static uint8_t paeth_predictor(int a, int b, int c)
{
    int p = a + b - c;
    int pa = abs(p - a);
    int pb = abs(p - b);
    int pc = abs(p - c);
    if (pa <= pb && pa <= pc) {
        return (uint8_t)a;
    }
    if (pb <= pc) {
        return (uint8_t)b;
    }
    return (uint8_t)c;
}

static bool append_apng_zdata(apng_frame_t *f, const uint8_t *chunk, size_t len)
{
    if (f->zsize + len > f->zcap) {
        size_t new_cap = f->zcap ? f->zcap * 2u : 4096u;
        while (new_cap < f->zsize + len) {
            new_cap *= 2u;
        }
        uint8_t *nb = (uint8_t *)realloc(f->zdata, new_cap);
        if (!nb) {
            return false;
        }
        f->zdata = nb;
        f->zcap = new_cap;
    }
    memcpy(f->zdata + f->zsize, chunk, len);
    f->zsize += len;
    return true;
}

static bool init_apng_decoder(waywal_anim_decoder_t *dec)
{
    const uint8_t *data = dec->raw_data;
    size_t size = dec->raw_size;
    size_t pos = 8;

    bool seen_ihdr = false;
    bool seen_actl = false;
    bool seen_idat = false;
    uint32_t cap = 16;
    dec->apng_frames = (apng_frame_t *)calloc(cap, sizeof(apng_frame_t));
    if (!dec->apng_frames) {
        return false;
    }

    apng_frame_t *cur_frame = NULL;
    bool fctl_before_idat = false;

    while (pos + 12 <= size) {
        uint32_t clen = read_be32(data + pos);
        const uint8_t *ctype = data + pos + 4;
        const uint8_t *cpayload = data + pos + 8;
        if (pos + 12 + (size_t)clen > size) {
            break;
        }

        if (memcmp(ctype, "IHDR", 4) == 0 && clen >= 13) {
            dec->canvas_w = read_be32(cpayload);
            dec->canvas_h = read_be32(cpayload + 4);
            dec->png_bit_depth = cpayload[8];
            dec->png_color_type = cpayload[9];
            if (dec->canvas_w == 0 || dec->canvas_h == 0 || dec->canvas_w > 16384u ||
                dec->canvas_h > 16384u || dec->png_bit_depth != 8 || cpayload[12] != 0) {
                return false;
            }
            seen_ihdr = true;
        } else if (memcmp(ctype, "PLTE", 4) == 0) {
            uint32_t entries = clen / 3u;
            if (entries > 256) {
                entries = 256;
            }
            memcpy(dec->png_plte, cpayload, entries * 3u);
            dec->png_plte_count = (uint16_t)entries;
        } else if (memcmp(ctype, "tRNS", 4) == 0) {
            uint32_t entries = clen > 256 ? 256 : clen;
            memcpy(dec->png_trns, cpayload, entries);
            dec->png_trns_count = (uint16_t)entries;
        } else if (memcmp(ctype, "acTL", 4) == 0 && clen >= 8) {
            seen_actl = true;
        } else if (memcmp(ctype, "fcTL", 4) == 0 && clen >= 26) {
            if (!seen_ihdr) {
                return false;
            }
            if (!seen_idat) {
                fctl_before_idat = true;
            }
            if (dec->frame_count >= cap) {
                uint32_t new_cap = cap * 2u;
                apng_frame_t *nf = (apng_frame_t *)realloc(dec->apng_frames,
                                                           (size_t)new_cap * sizeof(apng_frame_t));
                if (!nf) {
                    return false;
                }
                memset(nf + cap, 0, (size_t)(new_cap - cap) * sizeof(apng_frame_t));
                dec->apng_frames = nf;
                cap = new_cap;
            }
            cur_frame = &dec->apng_frames[dec->frame_count++];
            cur_frame->width = read_be32(cpayload + 4);
            cur_frame->height = read_be32(cpayload + 8);
            cur_frame->x_offset = read_be32(cpayload + 12);
            cur_frame->y_offset = read_be32(cpayload + 16);
            if (cur_frame->width == 0 || cur_frame->height == 0 ||
                (uint64_t)cur_frame->x_offset + cur_frame->width > dec->canvas_w ||
                (uint64_t)cur_frame->y_offset + cur_frame->height > dec->canvas_h) {
                return false;
            }
            uint16_t delay_num = read_be16(cpayload + 20);
            uint16_t delay_den = read_be16(cpayload + 22);
            if (delay_den == 0) {
                delay_den = 100;
            }
            uint32_t ms = ((uint32_t)delay_num * 1000u) / (uint32_t)delay_den;
            cur_frame->delay_ms = (ms < 10u) ? 100u : ms;
            cur_frame->dispose_op = cpayload[24];
            cur_frame->blend_op = cpayload[25];
        } else if (memcmp(ctype, "IDAT", 4) == 0) {
            seen_idat = true;
            if (fctl_before_idat && cur_frame) {
                if (!append_apng_zdata(cur_frame, cpayload, clen)) {
                    return false;
                }
            }
        } else if (memcmp(ctype, "fdAT", 4) == 0 && clen > 4) {
            if (cur_frame) {
                if (!append_apng_zdata(cur_frame, cpayload + 4, clen - 4)) {
                    return false;
                }
            }
        } else if (memcmp(ctype, "IEND", 4) == 0) {
            break;
        }

        pos += 12 + (size_t)clen;
    }

    if (!seen_ihdr || !seen_actl || dec->frame_count == 0 || dec->canvas_w == 0 ||
        dec->canvas_h == 0 || dec->canvas_w > 16384u || dec->canvas_h > 16384u) {
        return false;
    }

    size_t num_pixels = 0;
    size_t canvas_bytes = 0;
    if (ckd_mul(&num_pixels, (size_t)dec->canvas_w, (size_t)dec->canvas_h) ||
        ckd_mul(&canvas_bytes, num_pixels, (size_t)4u)) {
        return false;
    }
    dec->canvas = (uint8_t *)calloc(1, canvas_bytes);
    if (!dec->canvas) {
        return false;
    }

    /* All compressed frame chunks are stored in apng_frames[i].zdata; free raw_data immediately */
    free(dec->raw_data);
    dec->raw_data = NULL;
    dec->raw_size = 0;
    return true;
}

static uint32_t png_bytes_per_pixel(uint8_t color_type)
{
    switch (color_type) {
    case 0:
        return 1;
    case 2:
        return 3;
    case 3:
        return 1;
    case 4:
        return 2;
    case 6:
        return 4;
    default:
        return 0;
    }
}

static bool step_apng_frame(waywal_anim_decoder_t *dec, const uint8_t **out_rgba, uint32_t *out_w,
                            uint32_t *out_h, uint32_t *out_delay_ms)
{
    if (dec->current_frame >= dec->frame_count) {
        waywal_anim_reset(dec);
    }

    apply_previous_disposal(dec);

    const apng_frame_t *f = &dec->apng_frames[dec->current_frame];
    if (f->dispose_op == 2) {
        size_t canvas_bytes = (size_t)dec->canvas_w * dec->canvas_h * 4u;
        if (!dec->prev_canvas) {
            dec->prev_canvas = (uint8_t *)calloc(1, canvas_bytes);
        }
        if (dec->prev_canvas) {
            memcpy(dec->prev_canvas, dec->canvas, canvas_bytes);
        }
    }

    uint32_t bpp = png_bytes_per_pixel(dec->png_color_type);
    if (bpp == 0 || f->width == 0 || f->height == 0 || f->width > dec->canvas_w ||
        f->height > dec->canvas_h || !f->zdata) {
        return false;
    }

    size_t stride = 0;
    size_t stride_plus_1 = 0;
    size_t raw_len = 0;
    if (ckd_mul(&stride, (size_t)f->width, (size_t)bpp) ||
        ckd_add(&stride_plus_1, stride, (size_t)1u) ||
        ckd_mul(&raw_len, stride_plus_1, (size_t)f->height)) {
        return false;
    }
    uint8_t *raw_scan = ensure_scratch_buf(dec, raw_len);
    if (!raw_scan) {
        return false;
    }

    if (!zlib_inflate_buf(f->zdata, f->zsize, raw_scan, raw_len)) {
        return false;
    }

    for (uint32_t y = 0; y < f->height; ++y) {
        uint8_t *line = raw_scan + (size_t)y * (stride + 1u);
        uint8_t filter = line[0];
        uint8_t *cur = line + 1;
        const uint8_t *prev = (y > 0) ? (line - stride) : NULL;

        for (size_t i = 0; i < stride; ++i) {
            int a = (i >= bpp) ? cur[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = (prev && i >= bpp) ? prev[i - bpp] : 0;

            switch (filter) {
            case 0:
                break;
            case 1:
                cur[i] = (uint8_t)(cur[i] + a);
                break;
            case 2:
                cur[i] = (uint8_t)(cur[i] + b);
                break;
            case 3:
                cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1));
                break;
            case 4:
                cur[i] = (uint8_t)(cur[i] + paeth_predictor(a, b, c));
                break;
            default:
                return false;
            }
        }

        uint64_t dst_y64 = (uint64_t)f->y_offset + y;
        if (dst_y64 >= dec->canvas_h) {
            continue;
        }
        uint32_t dst_y = (uint32_t)dst_y64;

        for (uint32_t x = 0; x < f->width; ++x) {
            uint64_t dst_x64 = (uint64_t)f->x_offset + x;
            if (dst_x64 >= dec->canvas_w) {
                continue;
            }
            uint32_t dst_x = (uint32_t)dst_x64;

            uint8_t sr = 0, sg = 0, sb = 0, sa = 255;
            if (dec->png_color_type == 6) {
                sr = cur[x * 4u + 0];
                sg = cur[x * 4u + 1];
                sb = cur[x * 4u + 2];
                sa = cur[x * 4u + 3];
            } else if (dec->png_color_type == 2) {
                sr = cur[x * 3u + 0];
                sg = cur[x * 3u + 1];
                sb = cur[x * 3u + 2];
                if (dec->png_trns_count >= 6 && sr == dec->png_trns[1] && sg == dec->png_trns[3] &&
                    sb == dec->png_trns[5]) {
                    sa = 0;
                } else {
                    sa = 255;
                }
            } else if (dec->png_color_type == 3) {
                uint8_t idx = cur[x];
                if (idx < dec->png_plte_count) {
                    sr = dec->png_plte[idx * 3u + 0];
                    sg = dec->png_plte[idx * 3u + 1];
                    sb = dec->png_plte[idx * 3u + 2];
                }
                sa = (idx < dec->png_trns_count) ? dec->png_trns[idx] : 255;
            } else if (dec->png_color_type == 4) {
                sr = sg = sb = cur[x * 2u + 0];
                sa = cur[x * 2u + 1];
            } else if (dec->png_color_type == 0) {
                sr = sg = sb = cur[x];
                sa = (dec->png_trns_count >= 2 && cur[x] == dec->png_trns[1]) ? 0 : 255;
            }

            size_t dst_off = ((size_t)dst_y * dec->canvas_w + dst_x) * 4u;
            if (f->blend_op == 0 || sa == 255) {
                dec->canvas[dst_off + 0] = sr;
                dec->canvas[dst_off + 1] = sg;
                dec->canvas[dst_off + 2] = sb;
                dec->canvas[dst_off + 3] = sa;
            } else if (sa > 0) {
                uint32_t dr = dec->canvas[dst_off + 0];
                uint32_t dg = dec->canvas[dst_off + 1];
                uint32_t db = dec->canvas[dst_off + 2];
                uint32_t da = dec->canvas[dst_off + 3];
                if (da == 255u) {
                    uint32_t inv_sa = 255u - sa;
                    dec->canvas[dst_off + 0] = (uint8_t)((sr * sa + dr * inv_sa + 127u) / 255u);
                    dec->canvas[dst_off + 1] = (uint8_t)((sg * sa + dg * inv_sa + 127u) / 255u);
                    dec->canvas[dst_off + 2] = (uint8_t)((sb * sa + db * inv_sa + 127u) / 255u);
                    dec->canvas[dst_off + 3] = 255u;
                } else {
                    uint32_t u = (255u - sa) * da;
                    uint32_t out_a = sa * 255u + u;
                    if (out_a > 0) {
                        dec->canvas[dst_off + 0] = (uint8_t)((sr * sa * 255u + dr * u) / out_a);
                        dec->canvas[dst_off + 1] = (uint8_t)((sg * sa * 255u + dg * u) / out_a);
                        dec->canvas[dst_off + 2] = (uint8_t)((sb * sa * 255u + db * u) / out_a);
                        dec->canvas[dst_off + 3] = (uint8_t)(out_a / 255u);
                    }
                }
            }
        }
    }

    dec->prev_disposal = (f->dispose_op == 1) ? 1 : (f->dispose_op == 2 ? 3 : 0);
    dec->prev_rect_x = f->x_offset;
    dec->prev_rect_y = f->y_offset;
    dec->prev_rect_w = f->width;
    dec->prev_rect_h = f->height;
    dec->current_frame++;

    if (out_rgba) {
        *out_rgba = dec->canvas;
    }
    if (out_w) {
        *out_w = dec->canvas_w;
    }
    if (out_h) {
        *out_h = dec->canvas_h;
    }
    if (out_delay_ms) {
        *out_delay_ms = f->delay_ms;
    }
    return true;
}

static bool init_webp_decoder(waywal_anim_decoder_t *dec)
{
#ifdef WWAL_DISABLE_WEBP
    (void)dec;
    return false;
#else
    dec->webp_dl_codec = dlopen("libwebp.so.7", RTLD_LAZY | RTLD_GLOBAL);
    if (!dec->webp_dl_codec) {
        dec->webp_dl_codec = dlopen("libwebp.so", RTLD_LAZY | RTLD_GLOBAL);
    }
    dec->webp_dl_demux = dlopen("libwebpdemux.so.2", RTLD_LAZY);
    if (!dec->webp_dl_demux) {
        dec->webp_dl_demux = dlopen("libwebpdemux.so", RTLD_LAZY);
    }
    if (!dec->webp_dl_demux) {
        WAYWAL_LOG_WARN("libwebpdemux.so.2 not found for animated WebP decoding");
        return false;
    }

    pfn_WebPAnimDecoderOptionsInitInternal fn_opts_init = NULL;
    pfn_WebPAnimDecoderNewInternal fn_new = NULL;
    pfn_WebPAnimDecoderGetInfo fn_info = NULL;
    void *sym_opts_init = dlsym(dec->webp_dl_demux, "WebPAnimDecoderOptionsInitInternal");
    void *sym_new = dlsym(dec->webp_dl_demux, "WebPAnimDecoderNewInternal");
    void *sym_info = dlsym(dec->webp_dl_demux, "WebPAnimDecoderGetInfo");
    void *sym_next = dlsym(dec->webp_dl_demux, "WebPAnimDecoderGetNext");
    void *sym_has_more = dlsym(dec->webp_dl_demux, "WebPAnimDecoderHasMoreFrames");
    void *sym_reset = dlsym(dec->webp_dl_demux, "WebPAnimDecoderReset");
    void *sym_delete = dlsym(dec->webp_dl_demux, "WebPAnimDecoderDelete");
    memcpy(&fn_opts_init, &sym_opts_init, sizeof(fn_opts_init));
    memcpy(&fn_new, &sym_new, sizeof(fn_new));
    memcpy(&fn_info, &sym_info, sizeof(fn_info));
    memcpy(&dec->fn_webp_next, &sym_next, sizeof(dec->fn_webp_next));
    memcpy(&dec->fn_webp_has_more, &sym_has_more, sizeof(dec->fn_webp_has_more));
    memcpy(&dec->fn_webp_reset, &sym_reset, sizeof(dec->fn_webp_reset));
    memcpy(&dec->fn_webp_delete, &sym_delete, sizeof(dec->fn_webp_delete));

    if (!fn_opts_init || !fn_new || !fn_info || !dec->fn_webp_next || !dec->fn_webp_has_more ||
        !dec->fn_webp_reset || !dec->fn_webp_delete) {
        return false;
    }

    webp_anim_options_t opts;
    memset(&opts, 0, sizeof(opts));
    if (!fn_opts_init(&opts, 0x0107)) {
        return false;
    }
    opts.color_mode = 1; /* MODE_RGBA */
    opts.use_threads = 1;

    dec->webp_raw.bytes = dec->raw_data;
    dec->webp_raw.size = dec->raw_size;
    dec->webp_dec = fn_new(&dec->webp_raw, &opts, 0x0107);
    if (!dec->webp_dec) {
        return false;
    }

    webp_anim_info_t info;
    memset(&info, 0, sizeof(info));
    if (!fn_info(dec->webp_dec, &info) || info.canvas_width == 0 || info.canvas_height == 0 ||
        info.canvas_width > 16384u || info.canvas_height > 16384u || info.frame_count == 0) {
        return false;
    }

    dec->canvas_w = info.canvas_width;
    dec->canvas_h = info.canvas_height;
    dec->frame_count = info.frame_count;
    dec->webp_prev_timestamp = 0;
    return true;
#endif
}

static bool step_webp_frame(waywal_anim_decoder_t *dec, const uint8_t **out_rgba, uint32_t *out_w,
                            uint32_t *out_h, uint32_t *out_delay_ms)
{
    if (!dec->webp_dec) {
        return false;
    }
    if (!dec->fn_webp_has_more(dec->webp_dec)) {
        dec->fn_webp_reset(dec->webp_dec);
        dec->webp_prev_timestamp = 0;
        dec->current_frame = 0;
    }

    uint8_t *buf = NULL;
    int timestamp = 0;
    if (!dec->fn_webp_next(dec->webp_dec, &buf, &timestamp) || !buf) {
        return false;
    }

    int delta = timestamp - dec->webp_prev_timestamp;
    dec->webp_prev_timestamp = timestamp;
    uint32_t delay_ms = (delta < 10) ? 100u : (uint32_t)delta;
    dec->current_frame++;

    if (out_rgba) {
        *out_rgba = buf;
    }
    if (out_w) {
        *out_w = dec->canvas_w;
    }
    if (out_h) {
        *out_h = dec->canvas_h;
    }
    if (out_delay_ms) {
        *out_delay_ms = delay_ms;
    }
    return true;
}

waywal_anim_decoder_t *waywal_anim_create(const uint8_t *data, size_t size)
{
    if (!data || size < 12) {
        return NULL;
    }

    waywal_anim_format_t fmt = waywal_anim_detect_format(data, size);
    if (fmt == WAYWAL_ANIM_FORMAT_UNKNOWN) {
        uint32_t gif_frames = 0;
        if (probe_gif_frames(data, size, &gif_frames, NULL) && gif_frames >= 1) {
            fmt = WAYWAL_ANIM_FORMAT_GIF;
        } else {
            return NULL;
        }
    }

    waywal_anim_decoder_t *dec = (waywal_anim_decoder_t *)calloc(1, sizeof(waywal_anim_decoder_t));
    if (!dec) {
        return NULL;
    }

    dec->format = fmt;
    dec->raw_data = (uint8_t *)malloc(size);
    if (!dec->raw_data) {
        free(dec);
        return NULL;
    }
    memcpy(dec->raw_data, data, size);
    dec->raw_size = size;

    bool ok = false;
    if (fmt == WAYWAL_ANIM_FORMAT_GIF) {
        ok = init_gif_decoder(dec);
    } else if (fmt == WAYWAL_ANIM_FORMAT_APNG) {
        ok = init_apng_decoder(dec);
    } else if (fmt == WAYWAL_ANIM_FORMAT_WEBP) {
        ok = init_webp_decoder(dec);
    }

    if (!ok) {
        waywal_anim_destroy(dec);
        return NULL;
    }
    return dec;
}

bool waywal_anim_next_frame(waywal_anim_decoder_t *dec, const uint8_t **out_rgba, uint32_t *out_w,
                            uint32_t *out_h, uint32_t *out_delay_ms)
{
    if (!dec) {
        return false;
    }
    switch (dec->format) {
    case WAYWAL_ANIM_FORMAT_GIF:
        return step_gif_frame(dec, out_rgba, out_w, out_h, out_delay_ms);
    case WAYWAL_ANIM_FORMAT_APNG:
        return step_apng_frame(dec, out_rgba, out_w, out_h, out_delay_ms);
    case WAYWAL_ANIM_FORMAT_WEBP:
        return step_webp_frame(dec, out_rgba, out_w, out_h, out_delay_ms);
    default:
        return false;
    }
}

void waywal_anim_reset(waywal_anim_decoder_t *dec)
{
    if (!dec) {
        return;
    }
    dec->current_frame = 0;
    dec->prev_disposal = 0;
    dec->prev_rect_x = 0;
    dec->prev_rect_y = 0;
    dec->prev_rect_w = 0;
    dec->prev_rect_h = 0;

    if (dec->canvas) {
        memset(dec->canvas, 0, (size_t)dec->canvas_w * dec->canvas_h * 4u);
    }
    if (dec->prev_canvas) {
        memset(dec->prev_canvas, 0, (size_t)dec->canvas_w * dec->canvas_h * 4u);
    }
    if (dec->format == WAYWAL_ANIM_FORMAT_WEBP && dec->webp_dec && dec->fn_webp_reset) {
        dec->fn_webp_reset(dec->webp_dec);
        dec->webp_prev_timestamp = 0;
    }
}

uint32_t waywal_anim_frame_count(const waywal_anim_decoder_t *dec)
{
    return dec ? dec->frame_count : 0;
}

waywal_anim_format_t waywal_anim_get_format(const waywal_anim_decoder_t *dec)
{
    return dec ? dec->format : WAYWAL_ANIM_FORMAT_UNKNOWN;
}

void waywal_anim_destroy(waywal_anim_decoder_t *dec)
{
    if (!dec) {
        return;
    }
    if (dec->webp_dec && dec->fn_webp_delete) {
        dec->fn_webp_delete(dec->webp_dec);
    }
    if (dec->webp_dl_demux) {
        dlclose(dec->webp_dl_demux);
    }
    if (dec->webp_dl_codec) {
        dlclose(dec->webp_dl_codec);
    }
    if (dec->apng_frames) {
        for (uint32_t i = 0; i < dec->frame_count; ++i) {
            free(dec->apng_frames[i].zdata);
        }
        free(dec->apng_frames);
    }
    free(dec->gif_frames);
    free(dec->scratch_buf);
    free(dec->canvas);
    free(dec->prev_canvas);
    free(dec->raw_data);
    free(dec);
}
