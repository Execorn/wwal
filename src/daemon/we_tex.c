// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/we_tex.h"

#include "image_loader.h"
#include "waywal/log.h"

#include <ctype.h>
#include <dlfcn.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
// clang-format off
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>
// clang-format on

#ifdef HAVE_FFMPEG_SWDEC
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#endif

#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM_EXT
#define GL_COMPRESSED_RGBA_BPTC_UNORM_EXT 0x8E8C
#endif

static uint16_t read_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8u);
}

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) |
           ((uint32_t)p[3] << 24u);
}

static int32_t read_i32_le(const uint8_t *p) { return (int32_t)read_u32_le(p); }

static float read_f32_le(const uint8_t *p)
{
    uint32_t bits = read_u32_le(p);
    float val = 0.0f;
    memcpy(&val, &bits, sizeof(val));
    return val;
}

static bool has_tex_prefix_at(const uint8_t *buf, size_t size, size_t pos, const char *prefix4)
{
    if (pos + 8u <= size && memcmp(buf + pos, prefix4, 4u) == 0 && isdigit(buf[pos + 4u]) &&
        isdigit(buf[pos + 5u]) && isdigit(buf[pos + 6u]) && isdigit(buf[pos + 7u])) {
        return true;
    }
    if (pos + 12u <= size) {
        uint32_t len = read_u32_le(buf + pos);
        if (len >= 8u && len <= 15u && memcmp(buf + pos + 4u, prefix4, 4u) == 0 &&
            isdigit(buf[pos + 8u]) && isdigit(buf[pos + 9u]) && isdigit(buf[pos + 10u]) &&
            isdigit(buf[pos + 11u])) {
            return true;
        }
    }
    return false;
}

static bool is_valid_texb_at(const uint8_t *buf, size_t size, size_t pos, uint32_t texb_ver)
{
    if (texb_ver < 1u || texb_ver > 4u || pos + 4u > size) {
        return false;
    }
    int32_t img_cnt = read_i32_le(buf + pos);
    if (img_cnt <= 0 || img_cnt > 1024) {
        return false;
    }
    size_t c = pos + 4u;
    int32_t fif = WE_FIF_UNKNOWN;
    if (texb_ver >= 3u) {
        if (c + 4u > size) {
            return false;
        }
        fif = read_i32_le(buf + c);
        c += 4u;
        if (fif < -1 || fif > 64) {
            return false;
        }
    }
    bool is_mp4 = (fif == WE_FIF_MP4);
    if (texb_ver == 4u) {
        if (c + 4u > size) {
            return false;
        }
        int32_t v = read_i32_le(buf + c);
        c += 4u;
        if (v != 0 && v != 1) {
            return false;
        }
        if (v == 1) {
            is_mp4 = true;
        }
    }
    if (c + 4u > size) {
        return false;
    }
    int32_t mip_cnt = read_i32_le(buf + c);
    c += 4u;
    if (mip_cnt <= 0 || mip_cnt > 32) {
        return false;
    }
    if (texb_ver == 4u && is_mp4 && c + 28u <= size) {
        uint32_t p0 = read_u32_le(buf + c);
        uint32_t p1 = read_u32_le(buf + c + 4u);
        if (p0 == 1u && p1 == 2u) {
            c += 8u;
        }
    }
    if (texb_ver == 1u) {
        if (c + 12u > size) {
            return false;
        }
        uint32_t mw = read_u32_le(buf + c);
        uint32_t mh = read_u32_le(buf + c + 4u);
        uint32_t bc = read_u32_le(buf + c + 8u);
        if (mw > 16384u || mh > 16384u || bc == 0 || bc > WE_TEX_MAX_DECOMP_SIZE ||
            c + 12u + (size_t)bc > size) {
            return false;
        }
    } else {
        if (c + 20u > size) {
            return false;
        }
        uint32_t mw = read_u32_le(buf + c);
        uint32_t mh = read_u32_le(buf + c + 4u);
        uint32_t lz4 = read_u32_le(buf + c + 8u);
        uint32_t dsz = read_u32_le(buf + c + 12u);
        uint32_t bc = read_u32_le(buf + c + 16u);
        if (mw > 16384u || mh > 16384u || lz4 > 1u || dsz > WE_TEX_MAX_DECOMP_SIZE || bc == 0 ||
            bc > WE_TEX_MAX_DECOMP_SIZE || c + 20u + (size_t)bc > size) {
            return false;
        }
        if (lz4 != 0 && dsz == 0) {
            return false;
        }
    }
    return true;
}

static bool is_valid_texs_at(const uint8_t *buf, size_t size, size_t pos, uint32_t texs_ver,
                             bool *out_exact_end)
{
    if (out_exact_end) {
        *out_exact_end = false;
    }
    if (texs_ver < 1u || texs_ver > 3u || pos + 4u > size) {
        return false;
    }
    uint32_t fcnt = read_u32_le(buf + pos);
    if (fcnt == 0 || fcnt > 16384u) {
        return false;
    }
    size_t c = pos + 4u;
    if (texs_ver == 3u) {
        if (c + 8u > size) {
            return false;
        }
        uint32_t gw = read_u32_le(buf + c);
        uint32_t gh = read_u32_le(buf + c + 4u);
        if (gw > 16384u || gh > 16384u) {
            return false;
        }
        c += 8u;
    }
    if (c + (size_t)fcnt * 32u > size) {
        return false;
    }
    int32_t img_id = read_i32_le(buf + c);
    float ft = read_f32_le(buf + c + 4u);
    if (img_id < 0 || img_id > 1024 || !isfinite(ft) || ft < 0.0f || ft > 3600.0f) {
        return false;
    }
    if (out_exact_end) {
        *out_exact_end = (c + (size_t)fcnt * 32u == size);
    }
    return true;
}

static bool should_skip_magic_nul(const uint8_t *buf, size_t size, size_t cur, const char magic[16])
{
    if (cur >= size || buf[cur] != '\0') {
        return false;
    }
    if (magic[3] == 'I') {
        /* TEXI0001 is followed by 28 bytes of header and then TEXB0001..0004.
         * When format == 0 (RGBA8888), the first byte of format is also 0x00.
         * Disambiguate by checking whether TEXB sits at cur + 28 vs cur + 29. */
        bool texb_at_28 = has_tex_prefix_at(buf, size, cur + 28u, "TEXB");
        bool texb_at_29 = has_tex_prefix_at(buf, size, cur + 29u, "TEXB");
        if (texb_at_28 && !texb_at_29) {
            return false;
        }
        if (texb_at_29) {
            return true;
        }
        return (cur + 1u == size) || (buf[cur + 1u] != '\0');
    }
    if (magic[3] == 'V') {
        /* TEXV is followed by TEXI (starts with 'T' or length 8..15, never 0x00) */
        return true;
    }
    if (magic[3] == 'B') {
        uint32_t ver = (uint32_t)strtoul(magic + 4u, NULL, 10);
        bool valid_cur = is_valid_texb_at(buf, size, cur, ver);
        bool valid_skip = is_valid_texb_at(buf, size, cur + 1u, ver);
        if (valid_skip && !valid_cur) {
            return true;
        }
        if (valid_cur && !valid_skip) {
            return false;
        }
    }
    if (magic[3] == 'S') {
        uint32_t ver = (uint32_t)strtoul(magic + 4u, NULL, 10);
        bool exact_cur = false, exact_skip = false;
        bool valid_cur = is_valid_texs_at(buf, size, cur, ver, &exact_cur);
        bool valid_skip = is_valid_texs_at(buf, size, cur + 1u, ver, &exact_skip);
        if (valid_skip && !valid_cur) {
            return true;
        }
        if (valid_cur && !valid_skip) {
            return false;
        }
        if (valid_cur && valid_skip) {
            if (exact_skip && !exact_cur) {
                return true;
            }
            if (exact_cur && !exact_skip) {
                return false;
            }
        }
    }
    return (cur + 1u == size) || (buf[cur + 1u] != '\0');
}

bool we_tex_read_magic(const uint8_t *buf, size_t size, size_t *inout_cursor, char out_magic[16])
{
    if (!buf || !inout_cursor || !out_magic) {
        return false;
    }
    size_t cur = *inout_cursor;
    if (cur + 8u > size) {
        return false;
    }

    const uint8_t *p = buf + cur;
    if (memcmp(p, "TEX", 3u) == 0 && isupper(p[3]) && isdigit(p[4]) && isdigit(p[5]) &&
        isdigit(p[6]) && isdigit(p[7])) {
        memcpy(out_magic, p, 8u);
        out_magic[8] = '\0';
        cur += 8u;
        if (should_skip_magic_nul(buf, size, cur, out_magic)) {
            cur += 1u;
        }
        *inout_cursor = cur;
        return true;
    }

    if (cur + 12u <= size) {
        uint32_t len = read_u32_le(p);
        if (len >= 8u && len <= 15u && cur + 4u + (size_t)len <= size) {
            const uint8_t *mp = p + 4u;
            if (memcmp(mp, "TEX", 3u) == 0 && isupper(mp[3]) && isdigit(mp[4]) && isdigit(mp[5]) &&
                isdigit(mp[6]) && isdigit(mp[7])) {
                memcpy(out_magic, mp, 8u);
                out_magic[8] = '\0';
                cur += 4u + (size_t)len;
                if (len == 8u && should_skip_magic_nul(buf, size, cur, out_magic)) {
                    cur += 1u;
                }
                *inout_cursor = cur;
                return true;
            }
        }
    }

    return false;
}

typedef int (*pfn_lz4_decompress_safe_t)(const char *src, char *dst, int compressed_size,
                                         int dst_capacity);

static pfn_lz4_decompress_safe_t get_system_lz4_decompress(void)
{
    static int s_init = 0;
    static pfn_lz4_decompress_safe_t s_fn = NULL;
    if (!s_init) {
        void *h = dlopen("liblz4.so.1", RTLD_LAZY | RTLD_LOCAL);
        if (h) {
            void *sym = dlsym(h, "LZ4_decompress_safe");
            memcpy(&s_fn, &sym, sizeof(s_fn));
        }
        s_init = 1;
    }
    return s_fn;
}

int we_lz4_decompress_safe(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap)
{
    if (!src || !dst || dst_cap == 0 || dst_cap > WE_TEX_MAX_DECOMP_SIZE) {
        return -1;
    }
    if (src_len == 0) {
        return -1;
    }

    pfn_lz4_decompress_safe_t sys_lz4 = get_system_lz4_decompress();
    if (sys_lz4 && src_len <= (size_t)INT_MAX && dst_cap <= (size_t)INT_MAX) {
        int r = sys_lz4((const char *)src, (char *)dst, (int)src_len, (int)dst_cap);
        if (r >= 0) {
            return r;
        }
    }

    const uint8_t *ip = src;
    const uint8_t *const iend = src + src_len;
    uint8_t *op = dst;
    uint8_t *const oend = dst + dst_cap;

    while (ip < iend) {
        uint8_t token = *ip++;
        size_t lit_len = (size_t)(token >> 4u);

        if (lit_len == 15u) {
            uint8_t s = 0;
            do {
                if (ip >= iend) {
                    return -1;
                }
                s = *ip++;
                lit_len += (size_t)s;
                if (lit_len > dst_cap) {
                    return -1;
                }
            } while (s == 255u);
        }

        if (lit_len > (size_t)(iend - ip) || lit_len > (size_t)(oend - op)) {
            return -1;
        }
        if (lit_len > 0) {
            memcpy(op, ip, lit_len);
            ip += lit_len;
            op += lit_len;
        }

        if (ip == iend) {
            break;
        }

        if ((size_t)(iend - ip) < 2u) {
            return -1;
        }
        uint16_t offset = read_u16_le(ip);
        ip += 2u;

        if (offset == 0 || (size_t)(op - dst) < (size_t)offset) {
            return -1;
        }
        const uint8_t *match = op - offset;

        size_t match_len = (size_t)(token & 0x0Fu);
        if (match_len == 15u) {
            uint8_t s = 0;
            do {
                if (ip >= iend) {
                    return -1;
                }
                s = *ip++;
                match_len += (size_t)s;
                if (match_len > dst_cap) {
                    return -1;
                }
            } while (s == 255u);
        }
        match_len += 4u;

        if (match_len > (size_t)(oend - op)) {
            return -1;
        }

        if ((size_t)offset >= match_len) {
            memcpy(op, match, match_len);
            op += match_len;
        } else {
            for (size_t i = 0; i < match_len; ++i) {
                *op++ = match[i];
            }
        }
    }

    return (int)(op - dst);
}

static void unpack_rgb565(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint32_t r5 = (c >> 11u) & 0x1Fu;
    uint32_t g6 = (c >> 5u) & 0x3Fu;
    uint32_t b5 = c & 0x1Fu;
    *r = (uint8_t)((r5 << 3u) | (r5 >> 2u));
    *g = (uint8_t)((g6 << 2u) | (g6 >> 4u));
    *b = (uint8_t)((b5 << 3u) | (b5 >> 2u));
}

bool we_decode_dxt1_rgba8(const uint8_t *src, size_t src_len, uint32_t width, uint32_t height,
                          uint8_t *dst_rgba)
{
    if (!src || !dst_rgba || width == 0 || height == 0 || width > 16384u || height > 16384u) {
        return false;
    }
    uint32_t bw = (width + 3u) / 4u;
    uint32_t bh = (height + 3u) / 4u;
    size_t needed = (size_t)bw * (size_t)bh * 8u;
    if (src_len < needed) {
        return false;
    }

    const uint8_t *bp = src;
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            uint16_t c0 = read_u16_le(bp);
            uint16_t c1 = read_u16_le(bp + 2u);
            uint32_t lut = read_u32_le(bp + 4u);
            bp += 8u;

            uint8_t pal[4][4];
            unpack_rgb565(c0, &pal[0][0], &pal[0][1], &pal[0][2]);
            pal[0][3] = 255u;
            unpack_rgb565(c1, &pal[1][0], &pal[1][1], &pal[1][2]);
            pal[1][3] = 255u;

            if (c0 > c1) {
                pal[2][0] = (uint8_t)((2u * pal[0][0] + pal[1][0] + 1u) / 3u);
                pal[2][1] = (uint8_t)((2u * pal[0][1] + pal[1][1] + 1u) / 3u);
                pal[2][2] = (uint8_t)((2u * pal[0][2] + pal[1][2] + 1u) / 3u);
                pal[2][3] = 255u;

                pal[3][0] = (uint8_t)((pal[0][0] + 2u * pal[1][0] + 1u) / 3u);
                pal[3][1] = (uint8_t)((pal[0][1] + 2u * pal[1][1] + 1u) / 3u);
                pal[3][2] = (uint8_t)((pal[0][2] + 2u * pal[1][2] + 1u) / 3u);
                pal[3][3] = 255u;
            } else {
                pal[2][0] = (uint8_t)((pal[0][0] + pal[1][0]) >> 1u);
                pal[2][1] = (uint8_t)((pal[0][1] + pal[1][1]) >> 1u);
                pal[2][2] = (uint8_t)((pal[0][2] + pal[1][2]) >> 1u);
                pal[2][3] = 255u;

                pal[3][0] = 0;
                pal[3][1] = 0;
                pal[3][2] = 0;
                pal[3][3] = 0;
            }

            for (uint32_t py = 0; py < 4u; ++py) {
                uint32_t y = by * 4u + py;
                if (y >= height) {
                    continue;
                }
                for (uint32_t px = 0; px < 4u; ++px) {
                    uint32_t x = bx * 4u + px;
                    if (x >= width) {
                        continue;
                    }
                    uint32_t idx = (lut >> (2u * (py * 4u + px))) & 0x3u;
                    uint8_t *out = dst_rgba + ((size_t)y * (size_t)width + (size_t)x) * 4u;
                    out[0] = pal[idx][0];
                    out[1] = pal[idx][1];
                    out[2] = pal[idx][2];
                    out[3] = pal[idx][3];
                }
            }
        }
    }
    return true;
}

bool we_decode_dxt3_rgba8(const uint8_t *src, size_t src_len, uint32_t width, uint32_t height,
                          uint8_t *dst_rgba)
{
    if (!src || !dst_rgba || width == 0 || height == 0 || width > 16384u || height > 16384u) {
        return false;
    }
    uint32_t bw = (width + 3u) / 4u;
    uint32_t bh = (height + 3u) / 4u;
    size_t needed = (size_t)bw * (size_t)bh * 16u;
    if (src_len < needed) {
        return false;
    }

    const uint8_t *bp = src;
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t *alpha_bytes = bp;
            uint16_t c0 = read_u16_le(bp + 8u);
            uint16_t c1 = read_u16_le(bp + 10u);
            uint32_t lut = read_u32_le(bp + 12u);
            bp += 16u;

            uint8_t pal[4][3];
            unpack_rgb565(c0, &pal[0][0], &pal[0][1], &pal[0][2]);
            unpack_rgb565(c1, &pal[1][0], &pal[1][1], &pal[1][2]);
            pal[2][0] = (uint8_t)((2u * pal[0][0] + pal[1][0] + 1u) / 3u);
            pal[2][1] = (uint8_t)((2u * pal[0][1] + pal[1][1] + 1u) / 3u);
            pal[2][2] = (uint8_t)((2u * pal[0][2] + pal[1][2] + 1u) / 3u);
            pal[3][0] = (uint8_t)((pal[0][0] + 2u * pal[1][0] + 1u) / 3u);
            pal[3][1] = (uint8_t)((pal[0][1] + 2u * pal[1][1] + 1u) / 3u);
            pal[3][2] = (uint8_t)((pal[0][2] + 2u * pal[1][2] + 1u) / 3u);

            for (uint32_t py = 0; py < 4u; ++py) {
                uint32_t y = by * 4u + py;
                if (y >= height) {
                    continue;
                }
                uint16_t a_row = read_u16_le(alpha_bytes + py * 2u);
                for (uint32_t px = 0; px < 4u; ++px) {
                    uint32_t x = bx * 4u + px;
                    if (x >= width) {
                        continue;
                    }
                    uint32_t cidx = (lut >> (2u * (py * 4u + px))) & 0x3u;
                    uint8_t a4 = (uint8_t)((a_row >> (px * 4u)) & 0xFu);
                    uint8_t *out = dst_rgba + ((size_t)y * (size_t)width + (size_t)x) * 4u;
                    out[0] = pal[cidx][0];
                    out[1] = pal[cidx][1];
                    out[2] = pal[cidx][2];
                    out[3] = (uint8_t)((a4 << 4u) | a4);
                }
            }
        }
    }
    return true;
}

bool we_decode_dxt5_rgba8(const uint8_t *src, size_t src_len, uint32_t width, uint32_t height,
                          uint8_t *dst_rgba)
{
    if (!src || !dst_rgba || width == 0 || height == 0 || width > 16384u || height > 16384u) {
        return false;
    }
    uint32_t bw = (width + 3u) / 4u;
    uint32_t bh = (height + 3u) / 4u;
    size_t needed = (size_t)bw * (size_t)bh * 16u;
    if (src_len < needed) {
        return false;
    }

    const uint8_t *bp = src;
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            uint8_t a0 = bp[0];
            uint8_t a1 = bp[1];
            uint64_t alut = (uint64_t)bp[2] | ((uint64_t)bp[3] << 8u) | ((uint64_t)bp[4] << 16u) |
                            ((uint64_t)bp[5] << 24u) | ((uint64_t)bp[6] << 32u) |
                            ((uint64_t)bp[7] << 40u);

            uint8_t apal[8];
            apal[0] = a0;
            apal[1] = a1;
            if (a0 > a1) {
                for (uint32_t k = 1u; k <= 6u; ++k) {
                    apal[k + 1u] = (uint8_t)(((7u - k) * a0 + k * a1 + 3u) / 7u);
                }
            } else {
                for (uint32_t k = 1u; k <= 4u; ++k) {
                    apal[k + 1u] = (uint8_t)(((5u - k) * a0 + k * a1 + 2u) / 5u);
                }
                apal[6] = 0u;
                apal[7] = 255u;
            }

            uint16_t c0 = read_u16_le(bp + 8u);
            uint16_t c1 = read_u16_le(bp + 10u);
            uint32_t lut = read_u32_le(bp + 12u);
            bp += 16u;

            uint8_t pal[4][3];
            unpack_rgb565(c0, &pal[0][0], &pal[0][1], &pal[0][2]);
            unpack_rgb565(c1, &pal[1][0], &pal[1][1], &pal[1][2]);
            pal[2][0] = (uint8_t)((2u * pal[0][0] + pal[1][0] + 1u) / 3u);
            pal[2][1] = (uint8_t)((2u * pal[0][1] + pal[1][1] + 1u) / 3u);
            pal[2][2] = (uint8_t)((2u * pal[0][2] + pal[1][2] + 1u) / 3u);
            pal[3][0] = (uint8_t)((pal[0][0] + 2u * pal[1][0] + 1u) / 3u);
            pal[3][1] = (uint8_t)((pal[0][1] + 2u * pal[1][1] + 1u) / 3u);
            pal[3][2] = (uint8_t)((pal[0][2] + 2u * pal[1][2] + 1u) / 3u);

            for (uint32_t py = 0; py < 4u; ++py) {
                uint32_t y = by * 4u + py;
                if (y >= height) {
                    continue;
                }
                for (uint32_t px = 0; px < 4u; ++px) {
                    uint32_t x = bx * 4u + px;
                    if (x >= width) {
                        continue;
                    }
                    uint32_t pidx = py * 4u + px;
                    uint32_t cidx = (lut >> (2u * pidx)) & 0x3u;
                    uint32_t aidx = (uint32_t)((alut >> (3u * pidx)) & 0x7u);
                    uint8_t *out = dst_rgba + ((size_t)y * (size_t)width + (size_t)x) * 4u;
                    out[0] = pal[cidx][0];
                    out[1] = pal[cidx][1];
                    out[2] = pal[cidx][2];
                    out[3] = apal[aidx];
                }
            }
        }
    }
    return true;
}

/* BC7 / BPTC partition and weight tables per Khronos specification */
static const uint8_t BC7_WEIGHTS_2[4] = {0, 21, 43, 64};
static const uint8_t BC7_WEIGHTS_3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
static const uint8_t BC7_WEIGHTS_4[16] = {0,  4,  9,  13, 17, 21, 26, 30,
                                          34, 38, 43, 47, 51, 55, 60, 64};

static const uint16_t BC7_PARTITION_2[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80,
    0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000, 0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310,
    0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C, 0xAAAA,
    0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC,
    0x6996, 0xC33C, 0x9966, 0x0660, 0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6,
    0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22,
};

static const uint32_t BC7_PARTITION_3[64] = {
    0xAA685050u, 0x6A5A5040u, 0x5A5A4200u, 0x5450A0A8u, 0xA5A50000u, 0xA0A05050u, 0x5555A0A0u,
    0x5A5A5050u, 0xAA550000u, 0xAA555500u, 0xAAAA5500u, 0x90909090u, 0x94949494u, 0xA4A4A4A4u,
    0xA9A59450u, 0x2A0A4250u, 0xA5945040u, 0x0A425054u, 0xA5A5A500u, 0x55A0A0A0u, 0xA8A85454u,
    0x6A6A4040u, 0xA4A45000u, 0x1A1A0500u, 0x0050A4A4u, 0xAAA59090u, 0x14696914u, 0x69691400u,
    0xA08585A0u, 0xAA821414u, 0x50A4A450u, 0x6A5A0200u, 0xA9A58000u, 0x5090A0A8u, 0xA8A09050u,
    0x24242424u, 0x00AA5500u, 0x24924924u, 0x24499224u, 0x50A50A50u, 0x500AA550u, 0xAAAA4444u,
    0x66660000u, 0xA5A0A5A0u, 0x50A050A0u, 0x69286928u, 0x44AAAA44u, 0x66666600u, 0xAA444444u,
    0x54A854A8u, 0x95809580u, 0x96969600u, 0xA85454A8u, 0x80959580u, 0xAA141414u, 0x96960000u,
    0xAAAA1414u, 0xA05050A0u, 0xA0A5A5A0u, 0x96000000u, 0x40804080u, 0xA9A8A9A8u, 0xAAAAAA44u,
    0x2A4A5254u,
};

static const uint8_t BC7_ANCHOR_2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8, 2,  2, 8,
    8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2, 8,  2, 2,
    2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2, 15,
};

static const uint8_t BC7_ANCHOR_3A[64] = {
    3,  3,  15, 15, 8, 3,  15, 15, 8,  8,  6, 6,  6, 5,  3, 3,  3,  3,  8,  15, 3,  3,
    6,  10, 5,  8,  8, 6,  8,  5,  15, 15, 8, 15, 3, 5,  6, 10, 8,  15, 15, 3,  15, 5,
    15, 15, 15, 15, 3, 15, 5,  5,  5,  8,  5, 10, 5, 10, 8, 13, 15, 12, 3,  3,
};

static const uint8_t BC7_ANCHOR_3B[64] = {
    15, 8, 8, 3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,  15, 8,  15, 3,  15, 8,
    15, 8, 3, 15, 6,  10, 15, 15, 10, 8,  15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15,
    3,  6, 6, 8,  15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8,
};

typedef struct {
    uint8_t ns;
    uint8_t pb;
    uint8_t rb;
    uint8_t isb;
    uint8_t cb;
    uint8_t ab;
    uint8_t epb;
    uint8_t spb;
    uint8_t ib;
    uint8_t ib2;
} bc7_mode_info_t;

static const bc7_mode_info_t BC7_MODES[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
};

static uint32_t bc7_read_bits(const uint8_t block[16], uint32_t *bit_pos, uint32_t count)
{
    if (count == 0) {
        return 0;
    }
    uint32_t val = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t p = (*bit_pos)++;
        if (p < 128u) {
            uint32_t bit = (block[p >> 3u] >> (p & 7u)) & 1u;
            val |= (bit << i);
        }
    }
    return val;
}

static uint8_t bc7_expand_quant(uint8_t val, uint8_t bits)
{
    if (bits >= 8u) {
        return val;
    }
    uint8_t shifted = (uint8_t)(val << (8u - bits));
    return (uint8_t)(shifted | (shifted >> bits));
}

static uint8_t bc7_interp(uint8_t e0, uint8_t e1, uint8_t weight)
{
    return (uint8_t)(((64u - (uint32_t)weight) * (uint32_t)e0 + (uint32_t)weight * (uint32_t)e1 +
                      32u) >>
                     6u);
}

static void bc7_decode_block(const uint8_t block[16], uint8_t out_rgba[16][4])
{
    if (block[0] == 0) {
        memset(out_rgba, 0, 64u);
        return;
    }

    uint32_t mode = (uint32_t)__builtin_ctz(block[0]);
    if (mode >= 8u) {
        memset(out_rgba, 0, 64u);
        return;
    }

    uint32_t bit_pos = mode + 1u;
    const bc7_mode_info_t *mi = &BC7_MODES[mode];

    uint32_t part = bc7_read_bits(block, &bit_pos, mi->pb);
    uint32_t rot = bc7_read_bits(block, &bit_pos, mi->rb);
    uint32_t isel = bc7_read_bits(block, &bit_pos, mi->isb);

    uint8_t ep[3][2][4];
    memset(ep, 0, sizeof(ep));
    uint32_t num_ep = (uint32_t)mi->ns * 2u;

    for (uint32_t ch = 0; ch < 3u; ++ch) {
        for (uint32_t e = 0; e < num_ep; ++e) {
            ep[e >> 1u][e & 1u][ch] = (uint8_t)bc7_read_bits(block, &bit_pos, mi->cb);
        }
    }
    if (mi->ab > 0) {
        for (uint32_t e = 0; e < num_ep; ++e) {
            ep[e >> 1u][e & 1u][3] = (uint8_t)bc7_read_bits(block, &bit_pos, mi->ab);
        }
    } else {
        for (uint32_t s = 0; s < mi->ns; ++s) {
            ep[s][0][3] = 255u;
            ep[s][1][3] = 255u;
        }
    }

    uint8_t c_bits = (uint8_t)(mi->cb + (mi->epb | mi->spb));
    uint8_t a_bits = (uint8_t)(mi->ab + (mi->epb | mi->spb));

    if (mi->epb) {
        for (uint32_t e = 0; e < num_ep; ++e) {
            uint8_t pbit = (uint8_t)bc7_read_bits(block, &bit_pos, 1u);
            uint32_t s = e >> 1u;
            uint32_t j = e & 1u;
            for (uint32_t ch = 0; ch < 3u; ++ch) {
                ep[s][j][ch] = (uint8_t)((ep[s][j][ch] << 1u) | pbit);
            }
            if (mi->ab > 0) {
                ep[s][j][3] = (uint8_t)((ep[s][j][3] << 1u) | pbit);
            }
        }
    } else if (mi->spb) {
        for (uint32_t s = 0; s < mi->ns; ++s) {
            uint8_t pbit = (uint8_t)bc7_read_bits(block, &bit_pos, 1u);
            for (uint32_t j = 0; j < 2u; ++j) {
                for (uint32_t ch = 0; ch < 3u; ++ch) {
                    ep[s][j][ch] = (uint8_t)((ep[s][j][ch] << 1u) | pbit);
                }
                if (mi->ab > 0) {
                    ep[s][j][3] = (uint8_t)((ep[s][j][3] << 1u) | pbit);
                }
            }
        }
    }

    for (uint32_t s = 0; s < mi->ns; ++s) {
        for (uint32_t j = 0; j < 2u; ++j) {
            for (uint32_t ch = 0; ch < 3u; ++ch) {
                ep[s][j][ch] = bc7_expand_quant(ep[s][j][ch], c_bits);
            }
            if (mi->ab > 0) {
                ep[s][j][3] = bc7_expand_quant(ep[s][j][3], a_bits);
            }
        }
    }

    uint8_t idx1[16];
    uint8_t idx2[16];
    for (uint32_t i = 0; i < 16u; ++i) {
        bool is_anchor = (i == 0);
        if (mi->ns == 2u && i == BC7_ANCHOR_2[part]) {
            is_anchor = true;
        } else if (mi->ns == 3u && (i == BC7_ANCHOR_3A[part] || i == BC7_ANCHOR_3B[part])) {
            is_anchor = true;
        }
        uint32_t nbits = is_anchor ? ((uint32_t)mi->ib - 1u) : (uint32_t)mi->ib;
        idx1[i] = (uint8_t)bc7_read_bits(block, &bit_pos, nbits);
    }

    if (mi->ib2 > 0) {
        for (uint32_t i = 0; i < 16u; ++i) {
            uint32_t nbits = (i == 0) ? ((uint32_t)mi->ib2 - 1u) : (uint32_t)mi->ib2;
            idx2[i] = (uint8_t)bc7_read_bits(block, &bit_pos, nbits);
        }
    }

    const uint8_t *wtab1 =
        (mi->ib == 2u) ? BC7_WEIGHTS_2 : ((mi->ib == 3u) ? BC7_WEIGHTS_3 : BC7_WEIGHTS_4);
    const uint8_t *wtab2 = (mi->ib2 == 2u) ? BC7_WEIGHTS_2 : BC7_WEIGHTS_3;

    for (uint32_t i = 0; i < 16u; ++i) {
        uint32_t subset = 0;
        if (mi->ns == 2u) {
            subset = (BC7_PARTITION_2[part] >> i) & 1u;
        } else if (mi->ns == 3u) {
            subset = (BC7_PARTITION_3[part] >> (2u * i)) & 3u;
            if (subset > 2u) {
                subset = 2u;
            }
        }

        uint8_t cw = wtab1[idx1[i]];
        uint8_t aw = cw;
        if (mi->ib2 > 0) {
            if (isel) {
                cw = wtab2[idx2[i]];
                aw = wtab1[idx1[i]];
            } else {
                cw = wtab1[idx1[i]];
                aw = wtab2[idx2[i]];
            }
        }

        uint8_t r = bc7_interp(ep[subset][0][0], ep[subset][1][0], cw);
        uint8_t g = bc7_interp(ep[subset][0][1], ep[subset][1][1], cw);
        uint8_t b = bc7_interp(ep[subset][0][2], ep[subset][1][2], cw);
        uint8_t a = (mi->ab > 0) ? bc7_interp(ep[subset][0][3], ep[subset][1][3], aw) : 255u;

        if (rot == 1u) {
            uint8_t tmp = a;
            a = r;
            r = tmp;
        } else if (rot == 2u) {
            uint8_t tmp = a;
            a = g;
            g = tmp;
        } else if (rot == 3u) {
            uint8_t tmp = a;
            a = b;
            b = tmp;
        }

        out_rgba[i][0] = r;
        out_rgba[i][1] = g;
        out_rgba[i][2] = b;
        out_rgba[i][3] = a;
    }
}

bool we_decode_bc7_rgba8(const uint8_t *src, size_t src_len, uint32_t width, uint32_t height,
                         uint8_t *dst_rgba)
{
    if (!src || !dst_rgba || width == 0 || height == 0 || width > 16384u || height > 16384u) {
        return false;
    }
    uint32_t bw = (width + 3u) / 4u;
    uint32_t bh = (height + 3u) / 4u;
    size_t needed = (size_t)bw * (size_t)bh * 16u;
    if (src_len < needed) {
        return false;
    }

    const uint8_t *bp = src;
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            uint8_t block_rgba[16][4];
            bc7_decode_block(bp, block_rgba);
            bp += 16u;

            for (uint32_t py = 0; py < 4u; ++py) {
                uint32_t y = by * 4u + py;
                if (y >= height) {
                    continue;
                }
                for (uint32_t px = 0; px < 4u; ++px) {
                    uint32_t x = bx * 4u + px;
                    if (x >= width) {
                        continue;
                    }
                    uint32_t pidx = py * 4u + px;
                    uint8_t *out = dst_rgba + ((size_t)y * (size_t)width + (size_t)x) * 4u;
                    out[0] = block_rgba[pidx][0];
                    out[1] = block_rgba[pidx][1];
                    out[2] = block_rgba[pidx][2];
                    out[3] = block_rgba[pidx][3];
                }
            }
        }
    }
    return true;
}

we_tex_t *we_tex_parse_cpu(const uint8_t *data, size_t size)
{
    if (!data || size < 48u) {
        return NULL;
    }

    size_t cursor = 0;
    char magic_v[16] = {0};
    char magic_i[16] = {0};
    char magic_b[16] = {0};

    if (!we_tex_read_magic(data, size, &cursor, magic_v) || memcmp(magic_v, "TEXV", 4u) != 0) {
        return NULL;
    }
    uint32_t texv_ver = (uint32_t)strtoul(magic_v + 4u, NULL, 10);

    if (!we_tex_read_magic(data, size, &cursor, magic_i) || memcmp(magic_i, "TEXI", 4u) != 0) {
        return NULL;
    }
    uint32_t texi_ver = (uint32_t)strtoul(magic_i + 4u, NULL, 10);

    if (cursor + 28u > size) {
        return NULL;
    }

    int32_t raw_fmt = read_i32_le(data + cursor);
    uint32_t flags = read_u32_le(data + cursor + 4u);
    uint32_t tex_w = read_u32_le(data + cursor + 8u);
    uint32_t tex_h = read_u32_le(data + cursor + 12u);
    uint32_t img_w = read_u32_le(data + cursor + 16u);
    uint32_t img_h = read_u32_le(data + cursor + 20u);
    uint32_t unk_int0 = read_u32_le(data + cursor + 24u);
    cursor += 28u;

    /* Legacy TEXV0001 / TEXV0002 format mapping */
    if (texv_ver > 0 && texv_ver < 3u) {
        if (raw_fmt == 1) {
            raw_fmt = WE_TEX_FMT_DXT1;
        } else if (raw_fmt == 2) {
            raw_fmt = WE_TEX_FMT_DXT5;
        } else if (raw_fmt == 3) {
            raw_fmt = WE_TEX_FMT_RG88;
        }
    }

    if (tex_w == 0 || tex_h == 0 || tex_w > 16384u || tex_h > 16384u) {
        return NULL;
    }
    if (img_w == 0) {
        img_w = tex_w;
    }
    if (img_h == 0) {
        img_h = tex_h;
    }
    if (img_w > 16384u || img_h > 16384u) {
        return NULL;
    }

    if (!we_tex_read_magic(data, size, &cursor, magic_b) || memcmp(magic_b, "TEXB", 4u) != 0) {
        return NULL;
    }
    uint32_t texb_ver = (uint32_t)strtoul(magic_b + 4u, NULL, 10);
    if (texb_ver < 1u || texb_ver > 4u) {
        return NULL;
    }

    if (cursor + 4u > size) {
        return NULL;
    }
    int32_t image_count = read_i32_le(data + cursor);
    cursor += 4u;
    if (image_count <= 0 || image_count > 1024) {
        return NULL;
    }

    int32_t free_image_format = WE_FIF_UNKNOWN;
    if (texb_ver >= 3u) {
        if (cursor + 4u > size) {
            return NULL;
        }
        free_image_format = read_i32_le(data + cursor);
        cursor += 4u;
    }

    bool is_video_mp4 = false;
    if (texb_ver == 4u) {
        if (cursor + 4u > size) {
            return NULL;
        }
        is_video_mp4 = (read_i32_le(data + cursor) == 1);
        cursor += 4u;
        if (free_image_format == WE_FIF_UNKNOWN && is_video_mp4) {
            free_image_format = WE_FIF_MP4;
        }
    }
    if (free_image_format == WE_FIF_MP4) {
        is_video_mp4 = true;
    }

    we_tex_t *tex = (we_tex_t *)calloc(1, sizeof(we_tex_t));
    if (!tex) {
        return NULL;
    }
    tex->texv_version = texv_ver;
    tex->texi_version = texi_ver;
    tex->texb_version = texb_ver;
    tex->format = (we_tex_format_t)raw_fmt;
    tex->flags = flags;
    tex->texture_width = tex_w;
    tex->texture_height = tex_h;
    tex->image_width = img_w;
    tex->image_height = img_h;
    tex->unk_int0 = unk_int0;
    tex->image_count = (uint32_t)image_count;
    tex->free_image_format = free_image_format;
    tex->is_video_mp4 = is_video_mp4;

    tex->images = (we_tex_image_t *)calloc((size_t)image_count, sizeof(we_tex_image_t));
    if (!tex->images) {
        we_tex_destroy(tex);
        return NULL;
    }

    for (uint32_t img_idx = 0; img_idx < (uint32_t)image_count; ++img_idx) {
        if (cursor + 4u > size) {
            we_tex_destroy(tex);
            return NULL;
        }
        int32_t mip_count = read_i32_le(data + cursor);
        cursor += 4u;
        if (mip_count <= 0 || mip_count > 32) {
            we_tex_destroy(tex);
            return NULL;
        }

        tex->images[img_idx].mip_count = (uint32_t)mip_count;
        tex->images[img_idx].mips = (we_tex_mip_t *)calloc((size_t)mip_count, sizeof(we_tex_mip_t));
        if (!tex->images[img_idx].mips) {
            we_tex_destroy(tex);
            return NULL;
        }

        for (uint32_t m = 0; m < (uint32_t)mip_count; ++m) {
            we_tex_mip_t *mip = &tex->images[img_idx].mips[m];

            if (texb_ver == 4u && is_video_mp4 && cursor + 28u <= size) {
                uint32_t p0 = read_u32_le(data + cursor);
                uint32_t p1 = read_u32_le(data + cursor + 4u);
                if (p0 == 1u && p1 == 2u && (tex_w != 1u || tex_h != 2u)) {
                    cursor += 8u;
                }
            }

            if (texb_ver == 1u) {
                if (cursor + 12u > size) {
                    we_tex_destroy(tex);
                    return NULL;
                }
                mip->width = read_u32_le(data + cursor);
                mip->height = read_u32_le(data + cursor + 4u);
                mip->byte_count = read_u32_le(data + cursor + 8u);
                mip->is_lz4 = 0;
                mip->decomp_size = mip->byte_count;
                cursor += 12u;
            } else {
                if (cursor + 20u > size) {
                    we_tex_destroy(tex);
                    return NULL;
                }
                mip->width = read_u32_le(data + cursor);
                mip->height = read_u32_le(data + cursor + 4u);
                mip->is_lz4 = read_u32_le(data + cursor + 8u);
                mip->decomp_size = read_u32_le(data + cursor + 12u);
                mip->byte_count = read_u32_le(data + cursor + 16u);
                cursor += 20u;
                if (mip->is_lz4 == 0 && mip->decomp_size == 0) {
                    mip->decomp_size = mip->byte_count;
                }
            }

            if (mip->width == 0) {
                mip->width = tex_w;
            }
            if (mip->height == 0) {
                mip->height = tex_h;
            }
            if (mip->width == 0 || mip->height == 0 || mip->width > 16384u ||
                mip->height > 16384u || mip->byte_count == 0 ||
                mip->byte_count > WE_TEX_MAX_DECOMP_SIZE ||
                mip->decomp_size > WE_TEX_MAX_DECOMP_SIZE || cursor + mip->byte_count > size) {
                we_tex_destroy(tex);
                return NULL;
            }

            const uint8_t *payload = data + cursor;
            cursor += mip->byte_count;

            if (mip->is_lz4 != 0) {
                if (mip->decomp_size == 0) {
                    we_tex_destroy(tex);
                    return NULL;
                }
                mip->data = (uint8_t *)malloc(mip->decomp_size);
                if (!mip->data) {
                    we_tex_destroy(tex);
                    return NULL;
                }
                int dec_len =
                    we_lz4_decompress_safe(payload, mip->byte_count, mip->data, mip->decomp_size);
                if (dec_len < 0 || (uint32_t)dec_len != mip->decomp_size) {
                    we_tex_destroy(tex);
                    return NULL;
                }
                mip->data_size = mip->decomp_size;
            } else {
                mip->data = (uint8_t *)malloc(mip->byte_count);
                if (!mip->data) {
                    we_tex_destroy(tex);
                    return NULL;
                }
                memcpy(mip->data, payload, mip->byte_count);
                mip->data_size = mip->byte_count;
            }

            if (is_video_mp4 || free_image_format == WE_FIF_MP4) {
                if (img_idx == 0 && m == 0) {
                    /* Transfer ownership of mip->data to tex->mp4_data to avoid 2x RSS duplication
                     */
                    tex->mp4_data = mip->data;
                    tex->mp4_size = mip->data_size;
                    mip->data = NULL;
                    mip->data_size = 0;
                    tex->demuxer = demuxer_open_mem(tex->mp4_data, tex->mp4_size);
                }
            } else if (free_image_format >= 0) {
                uint8_t *dec_px = NULL;
                uint32_t dw = 0, dh = 0;
                if (!image_load_from_memory_rgba(mip->data, mip->data_size, &dec_px, &dw, &dh) ||
                    !dec_px || dw == 0 || dh == 0 || dw > 16384u || dh > 16384u) {
                    if (dec_px) {
                        image_free(dec_px);
                    }
                    we_tex_destroy(tex);
                    return NULL;
                }
                free(mip->data);
                mip->data = NULL;
                mip->data_size = 0;
                mip->rgba8 = dec_px;
                mip->width = dw;
                mip->height = dh;
                mip->is_bgra_swiz = true;
                if (img_idx == 0 && m == 0) {
                    tex->image_width = dw;
                    tex->image_height = dh;
                    tex->texture_width = dw;
                    tex->texture_height = dh;
                }
            }
        }
    }

    if ((flags & WE_TEX_FLAG_IS_GIF) != 0 && cursor < size) {
        char magic_s[16] = {0};
        if (!we_tex_read_magic(data, size, &cursor, magic_s) || memcmp(magic_s, "TEXS", 4u) != 0) {
            we_tex_destroy(tex);
            return NULL;
        }
        uint32_t texs_ver = (uint32_t)strtoul(magic_s + 4u, NULL, 10);
        if (texs_ver < 1u || texs_ver > 3u || cursor + 4u > size) {
            we_tex_destroy(tex);
            return NULL;
        }
        tex->texs_version = texs_ver;

        uint32_t frame_count = read_u32_le(data + cursor);
        cursor += 4u;
        if (frame_count == 0 || frame_count > 16384u) {
            we_tex_destroy(tex);
            return NULL;
        }

        if (texs_ver == 3u) {
            if (cursor + 8u > size) {
                we_tex_destroy(tex);
                return NULL;
            }
            tex->gif_width = read_u32_le(data + cursor);
            tex->gif_height = read_u32_le(data + cursor + 4u);
            cursor += 8u;
        } else {
            tex->gif_width = tex->image_width;
            tex->gif_height = tex->image_height;
        }

        if (cursor + (size_t)frame_count * 32u > size) {
            we_tex_destroy(tex);
            return NULL;
        }

        tex->frame_count = frame_count;
        tex->frames = (we_tex_frame_t *)calloc(frame_count, sizeof(we_tex_frame_t));
        if (!tex->frames) {
            we_tex_destroy(tex);
            return NULL;
        }

        for (uint32_t f = 0; f < frame_count; ++f) {
            we_tex_frame_t *fr = &tex->frames[f];
            fr->image_id = read_i32_le(data + cursor);
            fr->frametime = read_f32_le(data + cursor + 4u);
            if (fr->frametime <= 0.0f) {
                fr->frametime = 1.0f / 30.0f;
            }
            if (texs_ver == 1u) {
                fr->x = (float)read_i32_le(data + cursor + 8u);
                fr->y = (float)read_i32_le(data + cursor + 12u);
                fr->width = (float)read_i32_le(data + cursor + 16u);
                fr->width_y = (float)read_i32_le(data + cursor + 20u);
                fr->height_x = (float)read_i32_le(data + cursor + 24u);
                fr->height = (float)read_i32_le(data + cursor + 28u);
            } else {
                fr->x = read_f32_le(data + cursor + 8u);
                fr->y = read_f32_le(data + cursor + 12u);
                fr->width = read_f32_le(data + cursor + 16u);
                fr->width_y = read_f32_le(data + cursor + 20u);
                fr->height_x = read_f32_le(data + cursor + 24u);
                fr->height = read_f32_le(data + cursor + 28u);
            }
            cursor += 32u;
        }
    }

    return tex;
}

static bool decode_mip_to_rgba8(const we_tex_t *tex, const we_tex_mip_t *mip, uint8_t **out_rgba)
{
    if (!tex || !mip || !out_rgba || mip->width == 0 || mip->height == 0 || mip->width > 16384u ||
        mip->height > 16384u) {
        return false;
    }

    size_t npx = (size_t)mip->width * (size_t)mip->height;
    uint8_t *rgba = (uint8_t *)malloc(npx * 4u);
    if (!rgba) {
        return false;
    }

    if (mip->rgba8) {
        if (mip->is_bgra_swiz) {
            for (size_t i = 0; i < npx; ++i) {
                rgba[i * 4u + 0u] = mip->rgba8[i * 4u + 2u];
                rgba[i * 4u + 1u] = mip->rgba8[i * 4u + 1u];
                rgba[i * 4u + 2u] = mip->rgba8[i * 4u + 0u];
                rgba[i * 4u + 3u] = mip->rgba8[i * 4u + 3u];
            }
        } else {
            memcpy(rgba, mip->rgba8, npx * 4u);
        }
        *out_rgba = rgba;
        return true;
    }

    if (!mip->data) {
        free(rgba);
        return false;
    }

    bool ok = false;
    switch (tex->format) {
    case WE_TEX_FMT_RGBA8888:
        if (mip->data_size >= npx * 4u) {
            memcpy(rgba, mip->data, npx * 4u);
            ok = true;
        }
        break;
    case WE_TEX_FMT_RG88:
        if (mip->data_size >= npx * 2u) {
            for (size_t i = 0; i < npx; ++i) {
                uint8_t r = mip->data[i * 2u + 0u];
                uint8_t g = mip->data[i * 2u + 1u];
                rgba[i * 4u + 0u] = r;
                rgba[i * 4u + 1u] = r;
                rgba[i * 4u + 2u] = r;
                rgba[i * 4u + 3u] = g;
            }
            ok = true;
        }
        break;
    case WE_TEX_FMT_R8:
        if (mip->data_size >= npx) {
            for (size_t i = 0; i < npx; ++i) {
                uint8_t r = mip->data[i];
                rgba[i * 4u + 0u] = r;
                rgba[i * 4u + 1u] = r;
                rgba[i * 4u + 2u] = r;
                rgba[i * 4u + 3u] = r;
            }
            ok = true;
        }
        break;
    case WE_TEX_FMT_DXT1:
        ok = we_decode_dxt1_rgba8(mip->data, mip->data_size, mip->width, mip->height, rgba);
        break;
    case WE_TEX_FMT_DXT3:
        ok = we_decode_dxt3_rgba8(mip->data, mip->data_size, mip->width, mip->height, rgba);
        break;
    case WE_TEX_FMT_DXT5:
        ok = we_decode_dxt5_rgba8(mip->data, mip->data_size, mip->width, mip->height, rgba);
        break;
    case WE_TEX_FMT_BC7:
        ok = we_decode_bc7_rgba8(mip->data, mip->data_size, mip->width, mip->height, rgba);
        break;
    default:
        break;
    }

    if (!ok) {
        free(rgba);
        return false;
    }

    *out_rgba = rgba;
    return true;
}

bool we_tex_decode_rgba8(we_tex_t *tex, uint8_t **out_rgba, uint32_t *out_w, uint32_t *out_h)
{
    if (!tex || !out_rgba || tex->image_count == 0 || !tex->images ||
        tex->images[0].mip_count == 0 || !tex->images[0].mips) {
        return false;
    }
    const we_tex_mip_t *mip0 = &tex->images[0].mips[0];
    if (!decode_mip_to_rgba8(tex, mip0, out_rgba)) {
        return false;
    }
    if (out_w) {
        *out_w = mip0->width;
    }
    if (out_h) {
        *out_h = mip0->height;
    }
    return true;
}

static GLuint upload_single_image_gl(const we_tex_t *tex, const we_tex_image_t *img, bool has_s3tc,
                                     bool has_bptc)
{
    if (!tex || !img || img->mip_count == 0 || !img->mips) {
        return 0;
    }

    GLuint gl_tex = 0;
    glGenTextures(1, &gl_tex);
    if (!gl_tex) {
        return 0;
    }
    glBindTexture(GL_TEXTURE_2D, gl_tex);

    /* GLES S3TC/BPTC requires base level dimensions to be multiples of 4; if non-multiple of 4,
     * fall back to CPU RGBA8 decode for the entire mip chain to keep internalformat consistent. */
    bool use_hw_s3tc =
        has_s3tc && ((img->mips[0].width & 3u) == 0u) && ((img->mips[0].height & 3u) == 0u);
    bool use_hw_bptc =
        has_bptc && ((img->mips[0].width & 3u) == 0u) && ((img->mips[0].height & 3u) == 0u);

    uint32_t uploaded_levels = 0;
    uint32_t expected_w = 0;
    uint32_t expected_h = 0;

    for (uint32_t level = 0; level < img->mip_count; ++level) {
        const we_tex_mip_t *mip = &img->mips[level];
        if (mip->width == 0 || mip->height == 0 || mip->width > 16384u || mip->height > 16384u) {
            break;
        }
        if (level > 0 && (mip->width != expected_w || mip->height != expected_h)) {
            break;
        }
        expected_w = mip->width > 1u ? (mip->width >> 1u) : 1u;
        expected_h = mip->height > 1u ? (mip->height >> 1u) : 1u;

        if (mip->rgba8) {
            if (level == 0 && mip->is_bgra_swiz) {
                /* image_load_from_memory_rgba returns BGRA [B, G, R, A]: swizzle R<->B on GPU */
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
            }
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexImage2D(GL_TEXTURE_2D, (GLint)level, GL_RGBA8, (GLsizei)mip->width,
                         (GLsizei)mip->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, mip->rgba8);
            uploaded_levels++;
            continue;
        }

        if (tex->is_video_mp4) {
            uint8_t black[4] = {0, 0, 0, 255};
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
            uploaded_levels = 1u;
            break;
        }

        size_t npx = (size_t)mip->width * (size_t)mip->height;
        bool level_ok = false;

        switch (tex->format) {
        case WE_TEX_FMT_RGBA8888:
            if (mip->data && mip->data_size >= npx * 4u) {
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glTexImage2D(GL_TEXTURE_2D, (GLint)level, GL_RGBA8, (GLsizei)mip->width,
                             (GLsizei)mip->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, mip->data);
                level_ok = true;
            }
            break;

        case WE_TEX_FMT_RG88:
            if (mip->data && mip->data_size >= npx * 2u) {
                if (level == 0) {
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_GREEN);
                }
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(GL_TEXTURE_2D, (GLint)level, GL_RG8, (GLsizei)mip->width,
                             (GLsizei)mip->height, 0, GL_RG, GL_UNSIGNED_BYTE, mip->data);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                level_ok = true;
            }
            break;

        case WE_TEX_FMT_R8:
            if (mip->data && mip->data_size >= npx) {
                if (level == 0) {
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_RED);
                }
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(GL_TEXTURE_2D, (GLint)level, GL_R8, (GLsizei)mip->width,
                             (GLsizei)mip->height, 0, GL_RED, GL_UNSIGNED_BYTE, mip->data);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                level_ok = true;
            }
            break;

        case WE_TEX_FMT_DXT1:
        case WE_TEX_FMT_DXT3:
        case WE_TEX_FMT_DXT5: {
            size_t block_sz = (tex->format == WE_TEX_FMT_DXT1) ? 8u : 16u;
            size_t exp_bytes =
                (size_t)((mip->width + 3u) / 4u) * (size_t)((mip->height + 3u) / 4u) * block_sz;
            if (use_hw_s3tc && mip->data && mip->data_size == exp_bytes &&
                ((mip->width & 3u) == 0u || mip->width < 4u) &&
                ((mip->height & 3u) == 0u || mip->height < 4u)) {
                GLenum ifmt = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
                if (tex->format == WE_TEX_FMT_DXT1) {
                    ifmt = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
                } else if (tex->format == WE_TEX_FMT_DXT3) {
                    ifmt = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
                }
                glCompressedTexImage2D(GL_TEXTURE_2D, (GLint)level, ifmt, (GLsizei)mip->width,
                                       (GLsizei)mip->height, 0, (GLsizei)mip->data_size, mip->data);
                level_ok = true;
            } else if (!use_hw_s3tc || level == 0) {
                use_hw_s3tc = false;
                uint8_t *rgba = NULL;
                if (decode_mip_to_rgba8(tex, mip, &rgba)) {
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                    glTexImage2D(GL_TEXTURE_2D, (GLint)level, GL_RGBA8, (GLsizei)mip->width,
                                 (GLsizei)mip->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
                    free(rgba);
                    level_ok = true;
                }
            }
            break;
        }

        case WE_TEX_FMT_BC7: {
            size_t exp_bytes =
                (size_t)((mip->width + 3u) / 4u) * (size_t)((mip->height + 3u) / 4u) * 16u;
            if (use_hw_bptc && mip->data && mip->data_size == exp_bytes &&
                ((mip->width & 3u) == 0u || mip->width < 4u) &&
                ((mip->height & 3u) == 0u || mip->height < 4u)) {
                glCompressedTexImage2D(GL_TEXTURE_2D, (GLint)level,
                                       GL_COMPRESSED_RGBA_BPTC_UNORM_EXT, (GLsizei)mip->width,
                                       (GLsizei)mip->height, 0, (GLsizei)mip->data_size, mip->data);
                level_ok = true;
            } else if (!use_hw_bptc || level == 0) {
                use_hw_bptc = false;
                uint8_t *rgba = NULL;
                if (decode_mip_to_rgba8(tex, mip, &rgba)) {
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                    glTexImage2D(GL_TEXTURE_2D, (GLint)level, GL_RGBA8, (GLsizei)mip->width,
                                 (GLsizei)mip->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
                    free(rgba);
                    level_ok = true;
                }
            }
            break;
        }

        default:
            break;
        }

        if (!level_ok) {
            break;
        }
        uploaded_levels++;
    }

    if (uploaded_levels == 0) {
        glDeleteTextures(1, &gl_tex);
        return 0;
    }

    bool use_mips = (uploaded_levels > 1u);
    GLint mag_filter = (tex->flags & WE_TEX_FLAG_NO_INTERPOLATION) ? GL_NEAREST : GL_LINEAR;
    GLint min_filter = mag_filter;
    if (use_mips) {
        min_filter = (tex->flags & WE_TEX_FLAG_NO_INTERPOLATION) ? GL_NEAREST_MIPMAP_NEAREST
                                                                 : GL_LINEAR_MIPMAP_LINEAR;
    }
    GLint wrap = (tex->flags & WE_TEX_FLAG_CLAMP_UVS) ? GL_CLAMP_TO_EDGE : GL_REPEAT;

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)(uploaded_levels - 1u));

    return gl_tex;
}

bool we_tex_upload_gl(we_tex_t *tex)
{
    if (!tex || tex->image_count == 0 || !tex->images) {
        return false;
    }
    if (tex->gl_tex != 0) {
        return true;
    }

    if (tex->is_video_mp4 && tex->demuxer && tex->images[0].mip_count > 0 &&
        !tex->images[0].mips[0].rgba8) {
        (void)we_tex_advance_video(tex, 0.0f);
    }

    GLint prev_unpack_align = 4;
    GLint prev_tex2d = 0;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_unpack_align);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex2d);

    const char *exts = (const char *)glGetString(GL_EXTENSIONS);
    bool has_s3tc = exts && (strstr(exts, "GL_EXT_texture_compression_s3tc") != NULL ||
                             strstr(exts, "GL_ANGLE_texture_compression_dxt") != NULL ||
                             strstr(exts, "GL_EXT_texture_compression_dxt1") != NULL);
    bool has_bptc = exts && (strstr(exts, "GL_EXT_texture_compression_bptc") != NULL);

    if (tex->image_count > 1u) {
        tex->frame_gl_texs = (uint32_t *)calloc(tex->image_count, sizeof(uint32_t));
        if (!tex->frame_gl_texs) {
            glPixelStorei(GL_UNPACK_ALIGNMENT, prev_unpack_align);
            glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex2d);
            return false;
        }
        for (uint32_t i = 0; i < tex->image_count; ++i) {
            tex->frame_gl_texs[i] =
                upload_single_image_gl(tex, &tex->images[i], has_s3tc, has_bptc);
        }
        tex->gl_tex = tex->frame_gl_texs[0];
    } else {
        tex->gl_tex = upload_single_image_gl(tex, &tex->images[0], has_s3tc, has_bptc);
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_unpack_align);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex2d);

    if (tex->gl_tex != 0 && !tex->is_video_mp4) {
        for (uint32_t i = 0; i < tex->image_count; ++i) {
            for (uint32_t m = 0; m < tex->images[i].mip_count; ++m) {
                free(tex->images[i].mips[m].data);
                tex->images[i].mips[m].data = NULL;
                tex->images[i].mips[m].data_size = 0;
                if (tex->images[i].mips[m].rgba8) {
                    image_free(tex->images[i].mips[m].rgba8);
                    tex->images[i].mips[m].rgba8 = NULL;
                }
            }
        }
    }

    return tex->gl_tex != 0;
}

we_tex_t *we_tex_load(const uint8_t *data, size_t size, bool upload_gl)
{
    we_tex_t *tex = we_tex_parse_cpu(data, size);
    if (!tex) {
        return NULL;
    }
    if (upload_gl && !we_tex_upload_gl(tex)) {
        we_tex_destroy(tex);
        return NULL;
    }
    return tex;
}

bool we_tex_get_frame_transform(const we_tex_t *tex, uint32_t frame_idx, float out_translation[2],
                                float out_rotation[4])
{
    if (out_translation) {
        out_translation[0] = 0.0f;
        out_translation[1] = 0.0f;
    }
    if (out_rotation) {
        out_rotation[0] = 1.0f;
        out_rotation[1] = 0.0f;
        out_rotation[2] = 0.0f;
        out_rotation[3] = 1.0f;
    }

    if (!tex || tex->frame_count == 0 || !tex->frames || tex->texture_width == 0 ||
        tex->texture_height == 0) {
        return false;
    }

    uint32_t idx = frame_idx % tex->frame_count;
    const we_tex_frame_t *fr = &tex->frames[idx];
    float w = (float)tex->texture_width;
    float h = (float)tex->texture_height;

    if (out_translation) {
        out_translation[0] = fr->x / w;
        out_translation[1] = fr->y / h;
    }
    if (out_rotation) {
        out_rotation[0] = fr->width / w;
        out_rotation[1] = fr->width_y / h;
        out_rotation[2] = fr->height_x / w;
        out_rotation[3] = fr->height / h;
    }
    return true;
}

void we_tex_advance_spritesheet(we_tex_t *tex, float dt_sec, float out_translation[2],
                                float out_rotation[4])
{
    if (!tex || tex->frame_count == 0 || !tex->frames) {
        if (out_translation) {
            out_translation[0] = 0.0f;
            out_translation[1] = 0.0f;
        }
        if (out_rotation) {
            out_rotation[0] = 1.0f;
            out_rotation[1] = 0.0f;
            out_rotation[2] = 0.0f;
            out_rotation[3] = 1.0f;
        }
        return;
    }

    if (dt_sec > 0.0f) {
        tex->frame_time_accum += dt_sec;
        uint32_t safety = 0;
        while (safety++ < 256u) {
            float cur_dur = tex->frames[tex->current_frame_idx].frametime;
            if (cur_dur <= 1e-4f) {
                cur_dur = 1.0f / 30.0f;
            }
            if (tex->frame_time_accum < cur_dur) {
                break;
            }
            tex->frame_time_accum -= cur_dur;
            tex->current_frame_idx = (tex->current_frame_idx + 1u) % tex->frame_count;
        }
    }

    int32_t img_id = tex->frames[tex->current_frame_idx].image_id;
    if (tex->frame_gl_texs && img_id >= 0 && (uint32_t)img_id < tex->image_count &&
        tex->frame_gl_texs[img_id] != 0) {
        tex->gl_tex = tex->frame_gl_texs[img_id];
    }

    (void)we_tex_get_frame_transform(tex, tex->current_frame_idx, out_translation, out_rotation);
}

#ifdef HAVE_FFMPEG_SWDEC
typedef struct {
    void *lib_avcodec;
    void *lib_avutil;
    void *lib_swscale;

    const AVCodec *(*p_avcodec_find_decoder)(enum AVCodecID id);
    AVCodecContext *(*p_avcodec_alloc_context3)(const AVCodec *codec);
    void (*p_avcodec_free_context)(AVCodecContext **avctx);
    int (*p_avcodec_open2)(AVCodecContext *avctx, const AVCodec *codec, AVDictionary **options);
    int (*p_avcodec_send_packet)(AVCodecContext *avctx, const AVPacket *avpkt);
    int (*p_avcodec_receive_frame)(AVCodecContext *avctx, AVFrame *frame);
    void (*p_avcodec_flush_buffers)(AVCodecContext *avctx);

    AVPacket *(*p_av_packet_alloc)(void);
    void (*p_av_packet_free)(AVPacket **pkt);
    void (*p_av_packet_unref)(AVPacket *pkt);
    AVFrame *(*p_av_frame_alloc)(void);
    void (*p_av_frame_free)(AVFrame **frame);
    void (*p_av_frame_unref)(AVFrame *frame);
    void *(*p_av_mallocz)(size_t size);

    struct SwsContext *(*p_sws_getContext)(int srcW, int srcH, enum AVPixelFormat srcFormat,
                                           int dstW, int dstH, enum AVPixelFormat dstFormat,
                                           int flags, SwsFilter *srcFilter, SwsFilter *dstFilter,
                                           const double *param);
    int (*p_sws_scale)(struct SwsContext *c, const uint8_t *const srcSlice[], const int srcStride[],
                       int srcSliceY, int srcSliceH, uint8_t *const dst[], const int dstStride[]);
    void (*p_sws_freeContext)(struct SwsContext *swsContext);

    AVCodecContext *codec_ctx;
    AVPacket *pkt;
    AVFrame *frame;
    struct SwsContext *sws_ctx;
    uint32_t width;
    uint32_t height;
    uint32_t gl_alloc_w;
    uint32_t gl_alloc_h;
    int last_src_fmt;
    bool init_failed;
} we_tex_swdec_t;

static void *we_tex_dlopen_any(const char *const *names, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        void *h = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);
        if (h) {
            return h;
        }
    }
    return NULL;
}

static void we_tex_swdec_destroy(we_tex_swdec_t *sw)
{
    if (!sw) {
        return;
    }
    if (sw->sws_ctx && sw->p_sws_freeContext) {
        sw->p_sws_freeContext(sw->sws_ctx);
    }
    if (sw->frame && sw->p_av_frame_free) {
        sw->p_av_frame_free(&sw->frame);
    }
    if (sw->pkt && sw->p_av_packet_free) {
        sw->p_av_packet_free(&sw->pkt);
    }
    if (sw->codec_ctx && sw->p_avcodec_free_context) {
        sw->p_avcodec_free_context(&sw->codec_ctx);
    }
    if (sw->lib_swscale) {
        dlclose(sw->lib_swscale);
    }
    if (sw->lib_avcodec) {
        dlclose(sw->lib_avcodec);
    }
    if (sw->lib_avutil) {
        dlclose(sw->lib_avutil);
    }
    free(sw);
}

static bool we_tex_swdec_init(we_tex_t *tex)
{
    if (!tex || !tex->demuxer) {
        return false;
    }
    if (tex->video_swdec) {
        we_tex_swdec_t *existing = (we_tex_swdec_t *)tex->video_swdec;
        return !existing->init_failed;
    }

    we_tex_swdec_t *sw = (we_tex_swdec_t *)calloc(1, sizeof(we_tex_swdec_t));
    if (!sw) {
        return false;
    }
    tex->video_swdec = sw;
    sw->init_failed = true;

    const demuxer_t *demux = tex->demuxer;
    if (demux->width == 0 || demux->height == 0 || demux->width > 8192u || demux->height > 8192u) {
        return false;
    }

    enum AVCodecID cid = AV_CODEC_ID_NONE;
    switch (demux->codec) {
    case WAYWAL_CODEC_H264:
        cid = AV_CODEC_ID_H264;
        break;
    case WAYWAL_CODEC_HEVC:
        cid = AV_CODEC_ID_HEVC;
        break;
    case WAYWAL_CODEC_VP9:
        cid = AV_CODEC_ID_VP9;
        break;
    case WAYWAL_CODEC_AV1:
        cid = AV_CODEC_ID_AV1;
        break;
    default:
        return false;
    }

    static const char *const avcodec_names[] = {"libavcodec.so.61", "libavcodec.so.60",
                                                "libavcodec.so.59", "libavcodec.so"};
    static const char *const avutil_names[] = {"libavutil.so.59", "libavutil.so.58",
                                               "libavutil.so.57", "libavutil.so"};
    static const char *const swscale_names[] = {"libswscale.so.8", "libswscale.so.7",
                                                "libswscale.so.6", "libswscale.so"};

    sw->lib_avutil =
        we_tex_dlopen_any(avutil_names, sizeof(avutil_names) / sizeof(avutil_names[0]));
    sw->lib_avcodec =
        we_tex_dlopen_any(avcodec_names, sizeof(avcodec_names) / sizeof(avcodec_names[0]));
    sw->lib_swscale =
        we_tex_dlopen_any(swscale_names, sizeof(swscale_names) / sizeof(swscale_names[0]));
    if (!sw->lib_avutil || !sw->lib_avcodec || !sw->lib_swscale) {
        return false;
    }

#define WE_TEX_RESOLVE_SYM(handle, field, sym)                                                     \
    do {                                                                                           \
        *(void **)(&sw->field) = dlsym(handle, sym);                                               \
        if (!sw->field)                                                                            \
            return false;                                                                          \
    } while (0)

    WE_TEX_RESOLVE_SYM(sw->lib_avcodec, p_avcodec_find_decoder, "avcodec_find_decoder");
    WE_TEX_RESOLVE_SYM(sw->lib_avcodec, p_avcodec_alloc_context3, "avcodec_alloc_context3");
    WE_TEX_RESOLVE_SYM(sw->lib_avcodec, p_avcodec_free_context, "avcodec_free_context");
    WE_TEX_RESOLVE_SYM(sw->lib_avcodec, p_avcodec_open2, "avcodec_open2");
    WE_TEX_RESOLVE_SYM(sw->lib_avcodec, p_avcodec_send_packet, "avcodec_send_packet");
    WE_TEX_RESOLVE_SYM(sw->lib_avcodec, p_avcodec_receive_frame, "avcodec_receive_frame");
    WE_TEX_RESOLVE_SYM(sw->lib_avcodec, p_avcodec_flush_buffers, "avcodec_flush_buffers");
    WE_TEX_RESOLVE_SYM(sw->lib_avcodec, p_av_packet_alloc, "av_packet_alloc");
    WE_TEX_RESOLVE_SYM(sw->lib_avcodec, p_av_packet_free, "av_packet_free");
    WE_TEX_RESOLVE_SYM(sw->lib_avcodec, p_av_packet_unref, "av_packet_unref");

    WE_TEX_RESOLVE_SYM(sw->lib_avutil, p_av_frame_alloc, "av_frame_alloc");
    WE_TEX_RESOLVE_SYM(sw->lib_avutil, p_av_frame_free, "av_frame_free");
    WE_TEX_RESOLVE_SYM(sw->lib_avutil, p_av_frame_unref, "av_frame_unref");
    WE_TEX_RESOLVE_SYM(sw->lib_avutil, p_av_mallocz, "av_mallocz");

    WE_TEX_RESOLVE_SYM(sw->lib_swscale, p_sws_getContext, "sws_getContext");
    WE_TEX_RESOLVE_SYM(sw->lib_swscale, p_sws_scale, "sws_scale");
    WE_TEX_RESOLVE_SYM(sw->lib_swscale, p_sws_freeContext, "sws_freeContext");

#undef WE_TEX_RESOLVE_SYM

    const AVCodec *codec = sw->p_avcodec_find_decoder(cid);
    if (!codec) {
        return false;
    }

    sw->codec_ctx = sw->p_avcodec_alloc_context3(codec);
    sw->pkt = sw->p_av_packet_alloc();
    sw->frame = sw->p_av_frame_alloc();
    sw->width = demux->width;
    sw->height = demux->height;
    sw->last_src_fmt = -1;
    if (!sw->codec_ctx || !sw->pkt || !sw->frame) {
        return false;
    }

    sw->codec_ctx->width = (int)demux->width;
    sw->codec_ctx->height = (int)demux->height;
    sw->codec_ctx->thread_count = 1;

    if (demux->extradata && demux->extradata_size > 0) {
        sw->codec_ctx->extradata =
            (uint8_t *)sw->p_av_mallocz(demux->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (sw->codec_ctx->extradata) {
            memcpy(sw->codec_ctx->extradata, demux->extradata, demux->extradata_size);
            sw->codec_ctx->extradata_size = (int)demux->extradata_size;
        }
    }

    if (sw->p_avcodec_open2(sw->codec_ctx, codec, NULL) < 0) {
        return false;
    }

    sw->init_failed = false;
    return true;
}
#endif

bool we_tex_advance_video(we_tex_t *tex, float dt_sec)
{
    if (!tex || !tex->demuxer) {
        return false;
    }
    if (dt_sec > 0.0f) {
        tex->video_pts_sec += (double)dt_sec;
    }

#ifdef HAVE_FFMPEG_SWDEC
    if (we_tex_swdec_init(tex)) {
        we_tex_swdec_t *sw = (we_tex_swdec_t *)tex->video_swdec;
        for (int tries = 0; tries < 8; ++tries) {
            if (sw->p_avcodec_receive_frame(sw->codec_ctx, sw->frame) == 0) {
                if (sw->frame->format != sw->last_src_fmt || !sw->sws_ctx) {
                    if (sw->sws_ctx) {
                        sw->p_sws_freeContext(sw->sws_ctx);
                    }
                    sw->sws_ctx = sw->p_sws_getContext(
                        (int)sw->width, (int)sw->height, (enum AVPixelFormat)sw->frame->format,
                        (int)sw->width, (int)sw->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL,
                        NULL);
                    sw->last_src_fmt = sw->frame->format;
                }
                if (sw->sws_ctx && tex->image_count > 0 && tex->images &&
                    tex->images[0].mip_count > 0 && tex->images[0].mips) {
                    we_tex_mip_t *mip0 = &tex->images[0].mips[0];
                    size_t req_bytes = (size_t)sw->width * (size_t)sw->height * 4u;
                    if (!mip0->rgba8 || mip0->width != sw->width || mip0->height != sw->height) {
                        if (mip0->rgba8) {
                            image_free(mip0->rgba8);
                        }
                        mip0->rgba8 = (uint8_t *)malloc(req_bytes);
                    }
                    if (mip0->rgba8) {
                        mip0->width = sw->width;
                        mip0->height = sw->height;
                        mip0->is_bgra_swiz = false;
                        tex->image_width = sw->width;
                        tex->image_height = sw->height;
                        tex->texture_width = sw->width;
                        tex->texture_height = sw->height;

                        uint8_t *dst_planes[4] = {mip0->rgba8, NULL, NULL, NULL};
                        int dst_strides[4] = {(int)(sw->width * 4u), 0, 0, 0};
                        sw->p_sws_scale(sw->sws_ctx, (const uint8_t *const *)sw->frame->data,
                                        sw->frame->linesize, 0, (int)sw->height, dst_planes,
                                        dst_strides);

                        if (tex->gl_tex != 0) {
                            GLint prev_unpack_align = 4;
                            GLint prev_tex2d = 0;
                            glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_unpack_align);
                            glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex2d);
                            glBindTexture(GL_TEXTURE_2D, (GLuint)tex->gl_tex);
                            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                            if (sw->gl_alloc_w != sw->width || sw->gl_alloc_h != sw->height) {
                                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)sw->width,
                                             (GLsizei)sw->height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                             mip0->rgba8);
                                sw->gl_alloc_w = sw->width;
                                sw->gl_alloc_h = sw->height;
                            } else {
                                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)sw->width,
                                                (GLsizei)sw->height, GL_RGBA, GL_UNSIGNED_BYTE,
                                                mip0->rgba8);
                            }
                            glPixelStorei(GL_UNPACK_ALIGNMENT, prev_unpack_align);
                            glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex2d);
                        }
                    }
                }
                sw->p_av_frame_unref(sw->frame);
                return true;
            }

            demux_packet_t pkt;
            if (!tex->demuxer->read_packet(tex->demuxer, &pkt)) {
                if (tex->demuxer->seek_us) {
                    (void)tex->demuxer->seek_us(tex->demuxer, 0);
                    tex->video_pts_sec = 0.0;
                }
                sw->p_avcodec_flush_buffers(sw->codec_ctx);
                if (!tex->demuxer->read_packet(tex->demuxer, &pkt)) {
                    break;
                }
            }
            sw->p_av_packet_unref(sw->pkt);
            sw->pkt->data = (uint8_t *)(uintptr_t)pkt.data;
            sw->pkt->size = (int)pkt.size;
            sw->pkt->pts = pkt.pts_us;
            sw->pkt->dts = pkt.dts_us;
            (void)sw->p_avcodec_send_packet(sw->codec_ctx, sw->pkt);
        }
        return true;
    }
#endif

    demux_packet_t pkt;
    if (!tex->demuxer->read_packet(tex->demuxer, &pkt)) {
        if (tex->demuxer->seek_us) {
            (void)tex->demuxer->seek_us(tex->demuxer, 0);
            tex->video_pts_sec = 0.0;
            (void)tex->demuxer->read_packet(tex->demuxer, &pkt);
        }
    }
    return true;
}

void we_tex_destroy(we_tex_t *tex)
{
    if (!tex) {
        return;
    }
#ifdef HAVE_FFMPEG_SWDEC
    if (tex->video_swdec) {
        we_tex_swdec_destroy((we_tex_swdec_t *)tex->video_swdec);
        tex->video_swdec = NULL;
    }
#endif
    if (tex->images) {
        for (uint32_t i = 0; i < tex->image_count; ++i) {
            if (tex->images[i].mips) {
                for (uint32_t m = 0; m < tex->images[i].mip_count; ++m) {
                    free(tex->images[i].mips[m].data);
                    if (tex->images[i].mips[m].rgba8) {
                        image_free(tex->images[i].mips[m].rgba8);
                    }
                }
                free(tex->images[i].mips);
            }
        }
        free(tex->images);
    }
    free(tex->frames);
    if (tex->demuxer && tex->demuxer->destroy) {
        tex->demuxer->destroy(tex->demuxer);
    }
    free(tex->mp4_data);

    if (tex->frame_gl_texs) {
        for (uint32_t i = 0; i < tex->image_count; ++i) {
            if (tex->frame_gl_texs[i]) {
                GLuint t = tex->frame_gl_texs[i];
                glDeleteTextures(1, &t);
            }
        }
        free(tex->frame_gl_texs);
    } else if (tex->gl_tex) {
        GLuint t = tex->gl_tex;
        glDeleteTextures(1, &t);
    }
    free(tex);
}
