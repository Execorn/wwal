// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/we_puppet.h"

#include "waywal/log.h"

#include <GLES3/gl31.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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

static bool is_mdl_magic8(const uint8_t *p)
{
    if ((memcmp(p, "MDLV", 4u) == 0 || memcmp(p, "MDLS", 4u) == 0 || memcmp(p, "MDLA", 4u) == 0 ||
         memcmp(p, "MDAT", 4u) == 0) &&
        isdigit(p[4]) && isdigit(p[5]) && isdigit(p[6]) && isdigit(p[7])) {
        return true;
    }
    return false;
}

static bool has_mdl_magic_at(const uint8_t *buf, size_t size, size_t pos)
{
    if (pos + 8u <= size && is_mdl_magic8(buf + pos)) {
        return true;
    }
    if (pos + 12u <= size) {
        uint32_t len = read_u32_le(buf + pos);
        if (len >= 8u && len <= 15u && pos + 4u + (size_t)len <= size &&
            is_mdl_magic8(buf + pos + 4u)) {
            return true;
        }
    }
    return false;
}

static bool is_printable_ascii_span(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (!((c >= 0x20u && c < 0x7Fu) || c == '\t' || c == '\n' || c == '\r')) {
            return false;
        }
    }
    return true;
}

/* Reads either a NUL-terminated C-string or a [u32 len][chars] string */
static bool read_mdl_string_ex(const uint8_t *buf, size_t size, size_t *inout_cur, char *out_str,
                               size_t out_cap, bool *out_unterminated_len_str)
{
    if (out_unterminated_len_str) {
        *out_unterminated_len_str = false;
    }
    if (!buf || !inout_cur || !out_str || out_cap == 0) {
        return false;
    }
    out_str[0] = '\0';
    size_t c = *inout_cur;
    if (c >= size) {
        return false;
    }

    /* Check for [u32 len][chars] format (with optional trailing NUL included in ulen) */
    if (c + 4u <= size) {
        uint32_t ulen = read_u32_le(buf + c);
        if (ulen > 0u && ulen <= 1024u && buf[c + 2u] == 0u && buf[c + 3u] == 0u &&
            c + 4u + (size_t)ulen <= size) {
            bool has_trailing_nul = (buf[c + 4u + ulen - 1u] == '\0');
            size_t span_len = has_trailing_nul ? (size_t)(ulen - 1u) : (size_t)ulen;
            if ((span_len == 0u && ulen == 1u && has_trailing_nul) ||
                (span_len > 0u && is_printable_ascii_span(buf + c + 4u, span_len))) {
                size_t copy_len = span_len < (out_cap - 1u) ? span_len : (out_cap - 1u);
                if (copy_len > 0u) {
                    memcpy(out_str, buf + c + 4u, copy_len);
                }
                out_str[copy_len] = '\0';
                c += 4u + (size_t)ulen;
                *inout_cur = c;
                if (out_unterminated_len_str) {
                    *out_unterminated_len_str = !has_trailing_nul;
                }
                return true;
            }
        }
    }

    /* Standard NUL-terminated C-string */
    size_t start = c;
    while (c < size && buf[c] != '\0') {
        if (c - start > 4096u) {
            return false;
        }
        c++;
    }
    if (c >= size) {
        return false;
    }
    size_t slen = c - start;
    size_t copy_len = slen < (out_cap - 1u) ? slen : (out_cap - 1u);
    memcpy(out_str, buf + start, copy_len);
    out_str[copy_len] = '\0';
    *inout_cur = c + 1u; /* skip trailing '\0' */
    return true;
}

static bool is_valid_curr_marker_at(const uint8_t *buf, size_t size, size_t pos)
{
    if (pos + 8u > size) {
        return false;
    }
    int32_t zero = read_i32_le(buf + pos);
    if (zero != 0) {
        return false;
    }
    uint32_t curr = read_u32_le(buf + pos + 4u);
    if (curr == 0x01800009u || curr == 0x0180000Fu || curr == 0u) {
        return true;
    }
    if (curr >= 52u && (curr % 52u) == 0u && (curr / 52u) <= WE_MDL_MAX_VERTICES) {
        return true;
    }
    return false;
}

static bool is_valid_mdlv_at(const uint8_t *buf, size_t size, size_t pos)
{
    if (pos + 4u > size) {
        return false;
    }
    int32_t flag = read_i32_le(buf + pos);
    if (flag == 9 && (pos + 4u == size || pos + 12u <= size)) {
        return true;
    }
    if (flag < 0 || flag > 256 || pos + 12u > size) {
        return false;
    }
    size_t c = pos + 12u;
    char tmp[256];
    bool unterm = false;
    if (!read_mdl_string_ex(buf, size, &c, tmp, sizeof(tmp), &unterm)) {
        return false;
    }
    if (is_valid_curr_marker_at(buf, size, c)) {
        return true;
    }
    if (unterm && c < size && buf[c] == '\0' && is_valid_curr_marker_at(buf, size, c + 1u)) {
        return true;
    }
    return false;
}

static bool is_valid_mdat_at(const uint8_t *buf, size_t size, size_t pos)
{
    if (pos + 4u > size) {
        return false;
    }
    uint32_t att_count = read_u32_le(buf + pos);
    if (att_count > 256u) {
        return false;
    }
    size_t att_cur = pos + 4u;
    if (att_count == 0u) {
        return (att_cur == size) || has_mdl_magic_at(buf, size, att_cur);
    }
    for (uint32_t a = 0; a < att_count; a++) {
        if (att_cur + 2u > size) {
            return false;
        }
        att_cur += 2u;
        char aname[128];
        bool unterm = false;
        if (!read_mdl_string_ex(buf, size, &att_cur, aname, sizeof(aname), &unterm)) {
            return false;
        }
        if (unterm && att_cur < size && buf[att_cur] == '\0' && att_cur + 1u + 64u <= size &&
            (att_cur + 1u + 64u == size || has_mdl_magic_at(buf, size, att_cur + 1u + 64u))) {
            att_cur += 1u;
        }
        if (att_cur + 64u > size) {
            return false;
        }
        att_cur += 64u;
    }
    return (att_cur == size) || has_mdl_magic_at(buf, size, att_cur);
}

static bool is_valid_mdls_at(const uint8_t *buf, size_t size, size_t pos)
{
    if (pos + 4u > size) {
        return false;
    }
    uint32_t bones_num = read_u32_le(buf + pos);
    if (bones_num > WE_MDL_MAX_BONES) {
        return false;
    }
    size_t c = pos + 4u;
    if (bones_num == 0u) {
        return (c == size) || has_mdl_magic_at(buf, size, c);
    }
    char name[128];
    bool unterm = false;
    if (!read_mdl_string_ex(buf, size, &c, name, sizeof(name), &unterm)) {
        return false;
    }
    /* Optional NUL after unterminated [u32 len] string if byte_size==64 sits 1 byte further */
    size_t max_skip = unterm ? 1u : 0u;
    for (size_t skip = 0; skip <= max_skip; skip++) {
        size_t p = c + skip;
        if (skip == 1u && (c >= size || buf[c] != '\0')) {
            break;
        }
        if (p + 12u + 64u <= size) {
            uint32_t parent = read_u32_le(buf + p + 4u);
            uint32_t bsz = read_u32_le(buf + p + 8u);
            if (parent == 0xFFFFFFFFu && bsz == 64u) {
                return true;
            }
        }
    }
    return false;
}

static bool is_valid_mdla_at(const uint8_t *buf, size_t size, size_t pos)
{
    if (pos + 4u > size) {
        return false;
    }
    uint32_t anim_cnt = read_u32_le(buf + pos);
    if (anim_cnt > WE_MDL_MAX_ANIMS) {
        return false;
    }
    size_t c = pos + 4u;
    if (anim_cnt == 0u) {
        return c == size;
    }
    if (c + 8u > size) {
        return false;
    }
    int32_t id = read_i32_le(buf + c);
    if (id < -1 || id > 100000) {
        return false;
    }
    c += 8u;
    char name[128], mode[32];
    bool unterm_name = false, unterm_mode = false;
    if (!read_mdl_string_ex(buf, size, &c, name, sizeof(name), &unterm_name)) {
        return false;
    }
    if (unterm_name && c < size && buf[c] == '\0' && c + 1u < size &&
        (buf[c + 1u] == 'l' || buf[c + 1u] == 'm' || buf[c + 1u] == 's' || buf[c + 1u] == 'L' ||
         buf[c + 1u] == 'M' || buf[c + 1u] == 'S')) {
        c += 1u;
    }
    if (!read_mdl_string_ex(buf, size, &c, mode, sizeof(mode), &unterm_mode)) {
        return false;
    }
    size_t max_skip = unterm_mode ? 1u : 0u;
    for (size_t skip = 0; skip <= max_skip; skip++) {
        size_t p = c + skip;
        if (skip == 1u && (c >= size || buf[c] != '\0')) {
            break;
        }
        if (p + 16u <= size) {
            float fps = read_f32_le(buf + p);
            int32_t length = read_i32_le(buf + p + 4u);
            uint32_t b_num = read_u32_le(buf + p + 12u);
            if (isfinite(fps) && fps >= 0.0f && fps <= 1000.0f && length >= 1 &&
                length <= 1000000 && b_num <= WE_MDL_MAX_BONES) {
                return true;
            }
        }
    }
    return false;
}

static bool should_skip_mdl_magic_nul(const uint8_t *buf, size_t size, size_t cur,
                                      const char magic[16])
{
    if (cur >= size || buf[cur] != '\0') {
        return false;
    }
    if (has_mdl_magic_at(buf, size, cur + 1u) && !has_mdl_magic_at(buf, size, cur)) {
        return true;
    }
    if (has_mdl_magic_at(buf, size, cur) && !has_mdl_magic_at(buf, size, cur + 1u)) {
        return false;
    }
    if (magic[3] == 'V') {
        bool v_cur = is_valid_mdlv_at(buf, size, cur);
        bool v_skip = is_valid_mdlv_at(buf, size, cur + 1u);
        if (v_skip && !v_cur) {
            return true;
        }
        if (v_cur && !v_skip) {
            return false;
        }
    } else if (magic[3] == 'T') {
        bool v_cur = is_valid_mdat_at(buf, size, cur);
        bool v_skip = is_valid_mdat_at(buf, size, cur + 1u);
        if (v_skip && !v_cur) {
            return true;
        }
        if (v_cur && !v_skip) {
            return false;
        }
    } else if (magic[3] == 'S') {
        bool v_cur = is_valid_mdls_at(buf, size, cur);
        bool v_skip = is_valid_mdls_at(buf, size, cur + 1u);
        if (v_skip && !v_cur) {
            return true;
        }
        if (v_cur && !v_skip) {
            return false;
        }
    } else if (magic[3] == 'A') {
        bool v_cur = is_valid_mdla_at(buf, size, cur);
        bool v_skip = is_valid_mdla_at(buf, size, cur + 1u);
        if (v_skip && !v_cur) {
            return true;
        }
        if (v_cur && !v_skip) {
            return false;
        }
    }
    return (cur + 1u == size) || (buf[cur + 1u] != '\0');
}

bool we_mdl_read_magic(const uint8_t *buf, size_t size, size_t *inout_cursor, char out_magic[16])
{
    if (!buf || !inout_cursor || !out_magic) {
        return false;
    }
    size_t cur = *inout_cursor;
    if (cur + 8u > size) {
        return false;
    }

    const uint8_t *p = buf + cur;
    if (is_mdl_magic8(p)) {
        memcpy(out_magic, p, 8u);
        out_magic[8] = '\0';
        cur += 8u;
        if (should_skip_mdl_magic_nul(buf, size, cur, out_magic)) {
            cur += 1u;
        }
        *inout_cursor = cur;
        return true;
    }

    if (cur + 12u <= size) {
        uint32_t len = read_u32_le(p);
        if (len >= 8u && len <= 15u && cur + 4u + (size_t)len <= size) {
            const uint8_t *mp = p + 4u;
            if (is_mdl_magic8(mp)) {
                memcpy(out_magic, mp, 8u);
                out_magic[8] = '\0';
                cur += 4u + (size_t)len;
                if (len == 8u && should_skip_mdl_magic_nul(buf, size, cur, out_magic)) {
                    cur += 1u;
                }
                *inout_cursor = cur;
                return true;
            }
        }
    }

    return false;
}

static void mat4_identity(float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
}

void we_mat4_mul(const float a[16], const float b[16], float out[16])
{
    float r[16];
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            r[col * 4 + row] = a[0 * 4 + row] * b[col * 4 + 0] + a[1 * 4 + row] * b[col * 4 + 1] +
                               a[2 * 4 + row] * b[col * 4 + 2] + a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
    memcpy(out, r, sizeof(r));
}

bool we_mat4_inverse(const float m[16], float out_inv[16])
{
    double inv[16];
    double s[16];
    for (int i = 0; i < 16; i++) {
        s[i] = (double)m[i];
    }

    inv[0] = s[5] * s[10] * s[15] - s[5] * s[11] * s[14] - s[9] * s[6] * s[15] +
             s[9] * s[7] * s[14] + s[13] * s[6] * s[11] - s[13] * s[7] * s[10];

    inv[4] = -s[4] * s[10] * s[15] + s[4] * s[11] * s[14] + s[8] * s[6] * s[15] -
             s[8] * s[7] * s[14] - s[12] * s[6] * s[11] + s[12] * s[7] * s[10];

    inv[8] = s[4] * s[9] * s[15] - s[4] * s[11] * s[13] - s[8] * s[5] * s[15] +
             s[8] * s[7] * s[13] + s[12] * s[5] * s[11] - s[12] * s[7] * s[9];

    inv[12] = -s[4] * s[9] * s[14] + s[4] * s[10] * s[13] + s[8] * s[5] * s[14] -
              s[8] * s[6] * s[13] - s[12] * s[5] * s[10] + s[12] * s[6] * s[9];

    inv[1] = -s[1] * s[10] * s[15] + s[1] * s[11] * s[14] + s[9] * s[2] * s[15] -
             s[9] * s[3] * s[14] - s[13] * s[2] * s[11] + s[13] * s[3] * s[10];

    inv[5] = s[0] * s[10] * s[15] - s[0] * s[11] * s[14] - s[8] * s[2] * s[15] +
             s[8] * s[3] * s[14] + s[12] * s[2] * s[11] - s[12] * s[3] * s[10];

    inv[9] = -s[0] * s[9] * s[15] + s[0] * s[11] * s[13] + s[8] * s[1] * s[15] -
             s[8] * s[3] * s[13] - s[12] * s[1] * s[11] + s[12] * s[3] * s[9];

    inv[13] = s[0] * s[9] * s[14] - s[0] * s[10] * s[13] - s[8] * s[1] * s[14] +
              s[8] * s[2] * s[13] + s[12] * s[1] * s[10] - s[12] * s[2] * s[9];

    inv[2] = s[1] * s[6] * s[15] - s[1] * s[7] * s[14] - s[5] * s[2] * s[15] + s[5] * s[3] * s[14] +
             s[13] * s[2] * s[7] - s[13] * s[3] * s[6];

    inv[6] = -s[0] * s[6] * s[15] + s[0] * s[7] * s[14] + s[4] * s[2] * s[15] -
             s[4] * s[3] * s[14] - s[12] * s[2] * s[7] + s[12] * s[3] * s[6];

    inv[10] = s[0] * s[5] * s[15] - s[0] * s[7] * s[13] - s[4] * s[1] * s[15] +
              s[4] * s[3] * s[13] + s[12] * s[1] * s[7] - s[12] * s[3] * s[5];

    inv[14] = -s[0] * s[5] * s[14] + s[0] * s[6] * s[13] + s[4] * s[1] * s[14] -
              s[4] * s[2] * s[13] - s[12] * s[1] * s[6] + s[12] * s[2] * s[5];

    inv[3] = -s[1] * s[6] * s[11] + s[1] * s[7] * s[10] + s[5] * s[2] * s[11] -
             s[5] * s[3] * s[10] - s[9] * s[2] * s[7] + s[9] * s[3] * s[6];

    inv[7] = s[0] * s[6] * s[11] - s[0] * s[7] * s[10] - s[4] * s[2] * s[11] + s[4] * s[3] * s[10] +
             s[8] * s[2] * s[7] - s[8] * s[3] * s[6];

    inv[11] = -s[0] * s[5] * s[11] + s[0] * s[7] * s[9] + s[4] * s[1] * s[11] - s[4] * s[3] * s[9] -
              s[8] * s[1] * s[7] + s[8] * s[3] * s[5];

    inv[15] = s[0] * s[5] * s[10] - s[0] * s[6] * s[9] - s[4] * s[1] * s[10] + s[4] * s[2] * s[9] +
              s[8] * s[1] * s[6] - s[8] * s[2] * s[5];

    double det = s[0] * inv[0] + s[1] * inv[4] + s[2] * inv[8] + s[3] * inv[12];
    if (fabs(det) < 1e-12) {
        mat4_identity(out_inv);
        return false;
    }

    double inv_det = 1.0 / det;
    for (int i = 0; i < 16; i++) {
        out_inv[i] = (float)(inv[i] * inv_det);
    }
    return true;
}

static void mat4_from_trs(const float pos[3], const float angle[3], const float scale[3],
                          float out[16])
{
    float cx = cosf(angle[0]), sx = sinf(angle[0]);
    float cy = cosf(angle[1]), sy = sinf(angle[1]);
    float cz = cosf(angle[2]), sz = sinf(angle[2]);

    float scx = scale[0], scy = scale[1], scz = scale[2];

    out[0] = (cz * cy) * scx;
    out[1] = (sz * cy) * scx;
    out[2] = (-sy) * scx;
    out[3] = 0.0f;

    out[4] = (cz * sy * sx - sz * cx) * scy;
    out[5] = (sz * sy * sx + cz * cx) * scy;
    out[6] = (cy * sx) * scy;
    out[7] = 0.0f;

    out[8] = (cz * sy * cx + sz * sx) * scz;
    out[9] = (sz * sy * cx - cz * sx) * scz;
    out[10] = (cy * cx) * scz;
    out[11] = 0.0f;

    out[12] = pos[0];
    out[13] = pos[1];
    out[14] = pos[2];
    out[15] = 1.0f;
}

static void mat4_decompose_trs(const float m[16], float out_pos[3], float out_angle[3],
                               float out_scale[3])
{
    out_pos[0] = m[12];
    out_pos[1] = m[13];
    out_pos[2] = m[14];

    float sx = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    float sy = sqrtf(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
    float sz = sqrtf(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);

    out_scale[0] = sx > 1e-6f ? sx : 1.0f;
    out_scale[1] = sy > 1e-6f ? sy : 1.0f;
    out_scale[2] = sz > 1e-6f ? sz : 1.0f;

    float r00 = m[0] / out_scale[0], r10 = m[1] / out_scale[0], r20 = m[2] / out_scale[0];
    float r11 = m[5] / out_scale[1], r21 = m[6] / out_scale[1];
    float r12 = m[9] / out_scale[2], r22 = m[10] / out_scale[2];

    float sy_rot = -r20;
    if (sy_rot < -1.0f) {
        sy_rot = -1.0f;
    } else if (sy_rot > 1.0f) {
        sy_rot = 1.0f;
    }
    out_angle[1] = asinf(sy_rot);
    if (fabsf(r20) < 0.9999f) {
        out_angle[0] = atan2f(r21, r22);
        out_angle[2] = atan2f(r10, r00);
    } else {
        out_angle[0] = atan2f(-r12, r11);
        out_angle[2] = 0.0f;
    }
}

/* Scans forward from cursor to find the next valid MDAT, MDLS, or MDLA header */
static bool scan_to_next_section(const uint8_t *buf, size_t size, size_t *inout_cur,
                                 const char *target_prefix4)
{
    size_t c = *inout_cur;
    while (c + 8u <= size) {
        if (target_prefix4) {
            if (memcmp(buf + c, target_prefix4, 4u) == 0 && isdigit(buf[c + 4u]) &&
                isdigit(buf[c + 5u]) && isdigit(buf[c + 6u]) && isdigit(buf[c + 7u])) {
                *inout_cur = c;
                return true;
            }
            if (c + 12u <= size) {
                uint32_t len = read_u32_le(buf + c);
                if (len >= 8u && len <= 15u && memcmp(buf + c + 4u, target_prefix4, 4u) == 0 &&
                    isdigit(buf[c + 8u]) && isdigit(buf[c + 9u]) && isdigit(buf[c + 10u]) &&
                    isdigit(buf[c + 11u])) {
                    *inout_cur = c;
                    return true;
                }
            }
        } else if (has_mdl_magic_at(buf, size, c)) {
            *inout_cur = c;
            return true;
        }
        c++;
    }
    return false;
}

we_puppet_t *we_puppet_parse_cpu(const uint8_t *data, size_t size)
{
    if (!data || size < 16u) {
        return NULL;
    }

    size_t cur = 0;
    char magic[16] = {0};
    if (!we_mdl_read_magic(data, size, &cur, magic) || strncmp(magic, "MDLV", 4) != 0) {
        return NULL;
    }

    uint32_t mdlv_ver = (uint32_t)strtoul(magic + 4, NULL, 10);
    if (mdlv_ver < 13u || mdlv_ver > 23u) {
        return NULL;
    }

    if (cur + 4u > size) {
        return NULL;
    }
    int32_t mdl_flag = read_i32_le(data + cur);
    cur += 4u;
    if (mdl_flag == 9) {
        /* Incomplete stub mesh */
        return NULL;
    }

    if (cur + 8u > size) {
        return NULL;
    }
    int32_t unk_1 = read_i32_le(data + cur);
    int32_t unk_2 = read_i32_le(data + cur + 4u);
    cur += 8u;

    we_puppet_t *puppet = calloc(1, sizeof(we_puppet_t));
    if (!puppet) {
        return NULL;
    }
    puppet->mdlv_version = mdlv_ver;
    puppet->mdl_flag = mdl_flag;
    puppet->unk_1 = unk_1;
    puppet->unk_2 = unk_2;

    bool unterm_mat = false;
    if (!read_mdl_string_ex(data, size, &cur, puppet->mat_json_file, sizeof(puppet->mat_json_file),
                            &unterm_mat)) {
        we_puppet_destroy(puppet);
        return NULL;
    }
    if (unterm_mat && !is_valid_curr_marker_at(data, size, cur) && cur < size &&
        data[cur] == '\0' && is_valid_curr_marker_at(data, size, cur + 1u)) {
        cur += 1u;
    }

    if (cur + 8u > size) {
        we_puppet_destroy(puppet);
        return NULL;
    }
    int32_t zero = read_i32_le(data + cur);
    uint32_t curr = read_u32_le(data + cur + 4u);
    cur += 8u;
    (void)zero;

    bool alt_mdl_format = false;
    uint32_t vertex_byte_size = 0;

    if (curr == 0x01800009u) {
        if (cur + 4u > size) {
            we_puppet_destroy(puppet);
            return NULL;
        }
        vertex_byte_size = read_u32_le(data + cur);
        cur += 4u;
        alt_mdl_format = false;
    } else if (curr == 0x0180000Fu) {
        if (cur + 4u > size) {
            we_puppet_destroy(puppet);
            return NULL;
        }
        vertex_byte_size = read_u32_le(data + cur);
        cur += 4u;
        alt_mdl_format = true;
    } else if (curr == 0u) {
        bool found_alt = false;
        for (uint32_t w = 0; w < 32u && cur + 4u <= size; w++) {
            uint32_t word = read_u32_le(data + cur);
            cur += 4u;
            if (word == 0x0180000Fu) {
                found_alt = true;
                break;
            }
        }
        if (!found_alt || cur + 4u > size) {
            we_puppet_destroy(puppet);
            return NULL;
        }
        vertex_byte_size = read_u32_le(data + cur);
        cur += 4u;
        alt_mdl_format = true;
    } else {
        vertex_byte_size = curr;
        alt_mdl_format = false;
    }

    puppet->alt_mdl_format = alt_mdl_format;
    uint32_t stride = alt_mdl_format ? 80u : 52u;
    if (vertex_byte_size == 0u || (vertex_byte_size % stride) != 0u) {
        we_puppet_destroy(puppet);
        return NULL;
    }
    uint32_t vertex_count = vertex_byte_size / stride;
    if (vertex_count == 0u || vertex_count > WE_MDL_MAX_VERTICES ||
        cur + (size_t)vertex_byte_size > size) {
        we_puppet_destroy(puppet);
        return NULL;
    }

    puppet->vertex_count = vertex_count;
    puppet->vertices = calloc(vertex_count, sizeof(we_mdl_vertex_t));
    puppet->skinned_verts = calloc(vertex_count, sizeof(we_skinned_vertex_t));
    if (!puppet->vertices || !puppet->skinned_verts) {
        we_puppet_destroy(puppet);
        return NULL;
    }

    puppet->min_pos[0] = puppet->min_pos[1] = puppet->min_pos[2] = 1e30f;
    puppet->max_pos[0] = puppet->max_pos[1] = puppet->max_pos[2] = -1e30f;

    for (uint32_t i = 0; i < vertex_count; i++) {
        we_mdl_vertex_t *v = &puppet->vertices[i];
        v->pos[0] = read_f32_le(data + cur + 0u);
        v->pos[1] = read_f32_le(data + cur + 4u);
        v->pos[2] = read_f32_le(data + cur + 8u);
        cur += 12u;

        if (alt_mdl_format) {
            cur += 28u; /* skip 7 * uint32_t normal/tangent/padding */
        }

        v->blend_indices[0] = read_u32_le(data + cur + 0u);
        v->blend_indices[1] = read_u32_le(data + cur + 4u);
        v->blend_indices[2] = read_u32_le(data + cur + 8u);
        v->blend_indices[3] = read_u32_le(data + cur + 12u);
        cur += 16u;

        v->weight[0] = read_f32_le(data + cur + 0u);
        v->weight[1] = read_f32_le(data + cur + 4u);
        v->weight[2] = read_f32_le(data + cur + 8u);
        v->weight[3] = read_f32_le(data + cur + 12u);
        cur += 16u;

        v->texcoord[0] = read_f32_le(data + cur + 0u);
        v->texcoord[1] = read_f32_le(data + cur + 4u);
        cur += 8u;
    }

    /* Triangle index buffer */
    if (cur + 4u > size) {
        we_puppet_destroy(puppet);
        return NULL;
    }
    uint32_t indices_size = read_u32_le(data + cur);
    cur += 4u;
    if (indices_size == 0u || (indices_size % 6u) != 0u || cur + (size_t)indices_size > size) {
        we_puppet_destroy(puppet);
        return NULL;
    }
    uint32_t index_count = indices_size / 2u;
    uint32_t triangle_count = indices_size / 6u;
    if (triangle_count > WE_MDL_MAX_TRIANGLES) {
        we_puppet_destroy(puppet);
        return NULL;
    }

    puppet->index_count = index_count;
    puppet->triangle_count = triangle_count;
    puppet->indices = calloc(index_count, sizeof(uint16_t));
    if (!puppet->indices) {
        we_puppet_destroy(puppet);
        return NULL;
    }

    for (uint32_t i = 0; i < index_count; i++) {
        uint16_t idx = read_u16_le(data + cur + (size_t)i * 2u);
        if (idx >= vertex_count) {
            we_puppet_destroy(puppet);
            return NULL;
        }
        puppet->indices[i] = idx;
    }
    cur += (size_t)indices_size;

    /* Skip optional trailing MDLV metadata (e.g. MDLV0019..0023) before MDAT / MDLS / MDLA */
    if (cur < size && !has_mdl_magic_at(data, size, cur)) {
        if (mdlv_ver >= 19u) {
            if (!scan_to_next_section(data, size, &cur, NULL)) {
                cur = size;
            }
        }
    }

    /* Optional MDAT section */
    if (cur < size) {
        size_t probe = cur;
        char sec_magic[16] = {0};
        if (we_mdl_read_magic(data, size, &probe, sec_magic) &&
            strncmp(sec_magic, "MDAT", 4) == 0) {
            puppet->mdat_version = (uint32_t)strtoul(sec_magic + 4, NULL, 10);
            cur = probe;
            /* Parse or skip MDAT attachments up to MDLS / MDLA / EOF */
            if (cur + 4u <= size) {
                uint32_t att_count = read_u32_le(data + cur);
                size_t att_cur = cur + 4u;
                bool parsed_atts = (att_count <= 256u);
                for (uint32_t a = 0; a < att_count && parsed_atts; a++) {
                    if (att_cur + 2u > size) {
                        parsed_atts = false;
                        break;
                    }
                    att_cur += 2u; /* uint16_t bone_index */
                    char aname[128];
                    bool unterm_att = false;
                    if (!read_mdl_string_ex(data, size, &att_cur, aname, sizeof(aname),
                                            &unterm_att)) {
                        parsed_atts = false;
                        break;
                    }
                    if (unterm_att && att_cur < size && data[att_cur] == '\0' &&
                        att_cur + 1u + 64u <= size &&
                        (att_cur + 1u + 64u == size ||
                         has_mdl_magic_at(data, size, att_cur + 1u + 64u))) {
                        att_cur += 1u;
                    }
                    if (att_cur + 64u > size) {
                        parsed_atts = false;
                        break;
                    }
                    att_cur += 64u;
                }
                if (parsed_atts && (att_cur == size || has_mdl_magic_at(data, size, att_cur))) {
                    cur = att_cur;
                } else if (!scan_to_next_section(data, size, &cur, "MDLS") &&
                           !scan_to_next_section(data, size, &cur, "MDLA")) {
                    cur = size;
                }
            }
        }
    }

    /* Optional MDLS0001..0002 skeleton hierarchy section */
    if (cur < size) {
        size_t probe = cur;
        char sec_magic[16] = {0};
        if (we_mdl_read_magic(data, size, &probe, sec_magic) &&
            strncmp(sec_magic, "MDLS", 4) == 0) {
            uint32_t mdls_ver = (uint32_t)strtoul(sec_magic + 4, NULL, 10);
            if (mdls_ver < 1u || mdls_ver > 2u || probe + 4u > size) {
                we_puppet_destroy(puppet);
                return NULL;
            }
            puppet->mdls_version = mdls_ver;
            cur = probe;

            uint32_t bones_num = read_u32_le(data + cur);
            cur += 4u;
            if (bones_num > WE_MDL_MAX_BONES) {
                we_puppet_destroy(puppet);
                return NULL;
            }
            puppet->bone_count = bones_num;
            if (bones_num > 0u) {
                puppet->bones = calloc(bones_num, sizeof(we_mdl_bone_t));
                puppet->skin_mats = calloc(bones_num, sizeof(float[16]));
                if (!puppet->bones || !puppet->skin_mats) {
                    we_puppet_destroy(puppet);
                    return NULL;
                }
            }

            for (uint32_t i = 0; i < bones_num; i++) {
                we_mdl_bone_t *b = &puppet->bones[i];
                bool unterm_bname = false;
                if (!read_mdl_string_ex(data, size, &cur, b->name, sizeof(b->name),
                                        &unterm_bname)) {
                    we_puppet_destroy(puppet);
                    return NULL;
                }
                if (unterm_bname && cur + 12u + 64u <= size &&
                    read_u32_le(data + cur + 8u) != 64u && cur + 1u + 12u + 64u <= size &&
                    data[cur] == '\0' && read_u32_le(data + cur + 1u + 8u) == 64u) {
                    cur += 1u;
                }
                if (cur + 12u + 64u > size) {
                    we_puppet_destroy(puppet);
                    return NULL;
                }
                b->unk = read_i32_le(data + cur);
                b->parent = read_u32_le(data + cur + 4u);
                uint32_t byte_size = read_u32_le(data + cur + 8u);
                cur += 12u;

                /* Validate cycle-free parent index and 64-byte matrix */
                if ((b->parent != 0xFFFFFFFFu && b->parent >= i) || byte_size != 64u) {
                    we_puppet_destroy(puppet);
                    return NULL;
                }

                for (int m = 0; m < 16; m++) {
                    b->local_trans[m] = read_f32_le(data + cur + (size_t)m * 4u);
                }
                cur += 64u;

                bool unterm_sim = false;
                if (!read_mdl_string_ex(data, size, &cur, b->simulation_json,
                                        sizeof(b->simulation_json), &unterm_sim)) {
                    we_puppet_destroy(puppet);
                    return NULL;
                }
                if (unterm_sim && cur < size && data[cur] == '\0' &&
                    (i + 1u == bones_num
                         ? (cur + 1u == size || has_mdl_magic_at(data, size, cur + 1u))
                         : (cur + 1u < size && data[cur + 1u] != '\0'))) {
                    cur += 1u;
                }

                mat4_decompose_trs(b->local_trans, b->bind_pos, b->bind_angle, b->bind_scale);

                if (b->parent == 0xFFFFFFFFu) {
                    memcpy(b->world_bind, b->local_trans, sizeof(b->world_bind));
                } else {
                    we_mat4_mul(puppet->bones[b->parent].world_bind, b->local_trans, b->world_bind);
                }
                we_mat4_inverse(b->world_bind, b->offset_trans);
                mat4_identity(puppet->skin_mats[i]);
            }
        }
    }

    /* Sanitize vertex positions, UVs, blend indices, and weights after bone_count is known */
    puppet->min_pos[0] = puppet->min_pos[1] = puppet->min_pos[2] = 1e30f;
    puppet->max_pos[0] = puppet->max_pos[1] = puppet->max_pos[2] = -1e30f;
    for (uint32_t i = 0; i < puppet->vertex_count; i++) {
        we_mdl_vertex_t *v = &puppet->vertices[i];
        for (int k = 0; k < 3; k++) {
            if (!isfinite(v->pos[k])) {
                v->pos[k] = 0.0f;
            }
            if (v->pos[k] < puppet->min_pos[k]) {
                puppet->min_pos[k] = v->pos[k];
            }
            if (v->pos[k] > puppet->max_pos[k]) {
                puppet->max_pos[k] = v->pos[k];
            }
        }
        for (int k = 0; k < 2; k++) {
            if (!isfinite(v->texcoord[k])) {
                v->texcoord[k] = 0.0f;
            }
        }
        float wsum = 0.0f;
        for (int j = 0; j < 4; j++) {
            if (!isfinite(v->weight[j]) || v->weight[j] <= 0.0f ||
                (puppet->bone_count > 0u && v->blend_indices[j] >= puppet->bone_count)) {
                v->weight[j] = 0.0f;
                v->blend_indices[j] = 0u;
            } else if (puppet->bone_count == 0u) {
                v->blend_indices[j] = 0u;
            }
            wsum += v->weight[j];
        }
        if (wsum > 1e-6f) {
            float inv_w = 1.0f / wsum;
            for (int j = 0; j < 4; j++) {
                v->weight[j] *= inv_w;
            }
        } else {
            v->blend_indices[0] = 0u;
            v->blend_indices[1] = 0u;
            v->blend_indices[2] = 0u;
            v->blend_indices[3] = 0u;
            v->weight[0] = 1.0f;
            v->weight[1] = 0.0f;
            v->weight[2] = 0.0f;
            v->weight[3] = 0.0f;
        }
        puppet->skinned_verts[i].pos[0] = v->pos[0];
        puppet->skinned_verts[i].pos[1] = v->pos[1];
        puppet->skinned_verts[i].pos[2] = v->pos[2];
        puppet->skinned_verts[i].uv[0] = v->texcoord[0];
        puppet->skinned_verts[i].uv[1] = v->texcoord[1];
    }

    if (cur < size && !has_mdl_magic_at(data, size, cur)) {
        scan_to_next_section(data, size, &cur, "MDLA");
    }

    /* Optional MDLA0001..0003 keyframe animations section */
    if (cur < size) {
        size_t probe = cur;
        char sec_magic[16] = {0};
        if (we_mdl_read_magic(data, size, &probe, sec_magic) &&
            strncmp(sec_magic, "MDLA", 4) == 0) {
            uint32_t mdla_ver = (uint32_t)strtoul(sec_magic + 4, NULL, 10);
            if (mdla_ver < 1u || mdla_ver > 3u || probe + 4u > size) {
                we_puppet_destroy(puppet);
                return NULL;
            }
            puppet->mdla_version = mdla_ver;
            cur = probe;

            uint32_t anim_count = read_u32_le(data + cur);
            cur += 4u;
            if (anim_count > WE_MDL_MAX_ANIMS) {
                we_puppet_destroy(puppet);
                return NULL;
            }
            puppet->anim_count = anim_count;
            if (anim_count > 0u) {
                puppet->anims = calloc(anim_count, sizeof(we_mdl_anim_t));
                if (!puppet->anims) {
                    we_puppet_destroy(puppet);
                    return NULL;
                }
            }

            for (uint32_t a = 0; a < anim_count; a++) {
                we_mdl_anim_t *anim = &puppet->anims[a];
                if (cur + 8u > size) {
                    we_puppet_destroy(puppet);
                    return NULL;
                }
                anim->id = read_i32_le(data + cur);
                anim->unk = read_i32_le(data + cur + 4u);
                cur += 8u;

                bool unterm_aname = false, unterm_amode = false;
                if (!read_mdl_string_ex(data, size, &cur, anim->name, sizeof(anim->name),
                                        &unterm_aname)) {
                    we_puppet_destroy(puppet);
                    return NULL;
                }
                if (unterm_aname && cur < size && data[cur] == '\0' && cur + 1u < size &&
                    (data[cur + 1u] == 'l' || data[cur + 1u] == 'm' || data[cur + 1u] == 's' ||
                     data[cur + 1u] == 'L' || data[cur + 1u] == 'M' || data[cur + 1u] == 'S')) {
                    cur += 1u;
                }
                if (!read_mdl_string_ex(data, size, &cur, anim->mode_str, sizeof(anim->mode_str),
                                        &unterm_amode)) {
                    we_puppet_destroy(puppet);
                    return NULL;
                }
                if (unterm_amode && cur < size && data[cur] == '\0' && cur + 1u + 16u <= size) {
                    float fps_cur = read_f32_le(data + cur);
                    int32_t len_cur = read_i32_le(data + cur + 4u);
                    uint32_t b_cur = read_u32_le(data + cur + 12u);
                    bool valid_cur = isfinite(fps_cur) && fps_cur > 0.0f && fps_cur <= 1000.0f &&
                                     len_cur >= 1 && len_cur <= 1000000 &&
                                     b_cur <= WE_MDL_MAX_BONES;
                    float fps_skip = read_f32_le(data + cur + 1u);
                    int32_t len_skip = read_i32_le(data + cur + 1u + 4u);
                    uint32_t b_skip = read_u32_le(data + cur + 1u + 12u);
                    bool valid_skip = isfinite(fps_skip) && fps_skip > 0.0f &&
                                      fps_skip <= 1000.0f && len_skip >= 1 && len_skip <= 1000000 &&
                                      b_skip <= WE_MDL_MAX_BONES;
                    if (valid_skip && !valid_cur) {
                        cur += 1u;
                    }
                }

                if (strcasecmp(anim->mode_str, "mirror") == 0) {
                    anim->mode = WE_ANIM_MIRROR;
                } else if (strcasecmp(anim->mode_str, "single") == 0) {
                    anim->mode = WE_ANIM_SINGLE;
                } else {
                    anim->mode = WE_ANIM_LOOP;
                }

                if (cur + 16u > size) {
                    we_puppet_destroy(puppet);
                    return NULL;
                }
                anim->fps = read_f32_le(data + cur);
                if (!isfinite(anim->fps) || anim->fps <= 0.0f) {
                    anim->fps = 30.0f;
                }
                anim->length = read_i32_le(data + cur + 4u);
                anim->unk2 = read_i32_le(data + cur + 8u);
                uint32_t b_num = read_u32_le(data + cur + 12u);
                cur += 16u;

                if (anim->length < 1 || b_num > WE_MDL_MAX_BONES) {
                    we_puppet_destroy(puppet);
                    return NULL;
                }

                anim->track_count = b_num;
                if (b_num > 0u) {
                    anim->tracks = calloc(b_num, sizeof(we_mdl_bone_track_t));
                    if (!anim->tracks) {
                        we_puppet_destroy(puppet);
                        return NULL;
                    }
                }

                for (uint32_t t = 0; t < b_num; t++) {
                    we_mdl_bone_track_t *tr = &anim->tracks[t];
                    if (cur + 12u > size) {
                        we_puppet_destroy(puppet);
                        return NULL;
                    }
                    tr->bone_index = read_i32_le(data + cur);
                    tr->unk = read_i32_le(data + cur + 4u);
                    uint32_t kf_byte_size = read_u32_le(data + cur + 8u);
                    cur += 12u;

                    if ((kf_byte_size % 36u) != 0u || cur + (size_t)kf_byte_size > size) {
                        we_puppet_destroy(puppet);
                        return NULL;
                    }
                    uint32_t kf_count = kf_byte_size / 36u;
                    if (kf_count > 65536u) {
                        we_puppet_destroy(puppet);
                        return NULL;
                    }
                    tr->kf_count = kf_count;
                    if (kf_count > 0u) {
                        tr->keyframes = calloc(kf_count, sizeof(we_mdl_keyframe_t));
                        if (!tr->keyframes) {
                            we_puppet_destroy(puppet);
                            return NULL;
                        }
                        for (uint32_t k = 0; k < kf_count; k++) {
                            we_mdl_keyframe_t *kf = &tr->keyframes[k];
                            kf->pos[0] = read_f32_le(data + cur + 0u);
                            kf->pos[1] = read_f32_le(data + cur + 4u);
                            kf->pos[2] = read_f32_le(data + cur + 8u);
                            kf->angle[0] = read_f32_le(data + cur + 12u);
                            kf->angle[1] = read_f32_le(data + cur + 16u);
                            kf->angle[2] = read_f32_le(data + cur + 20u);
                            kf->scale[0] = read_f32_le(data + cur + 24u);
                            kf->scale[1] = read_f32_le(data + cur + 28u);
                            kf->scale[2] = read_f32_le(data + cur + 32u);
                            cur += 36u;
                        }
                    }
                }

                if (b_num > 1u) {
                    bool all_same_idx = true;
                    for (uint32_t t = 1u; t < b_num; t++) {
                        if (anim->tracks[t].bone_index != anim->tracks[0].bone_index) {
                            all_same_idx = false;
                            break;
                        }
                    }
                    if (all_same_idx) {
                        bool unk_is_seq = true;
                        for (uint32_t t = 0; t < b_num; t++) {
                            if (anim->tracks[t].unk != (int32_t)t) {
                                unk_is_seq = false;
                                break;
                            }
                        }
                        for (uint32_t t = 0; t < b_num; t++) {
                            anim->tracks[t].bone_index =
                                unk_is_seq ? anim->tracks[t].unk : (int32_t)t;
                        }
                    }
                }
            }
        }
    }

    return puppet;
}

static const we_mdl_anim_t *find_puppet_anim(const we_puppet_t *puppet,
                                             const we_puppet_layer_t *layer)
{
    if (!puppet || !layer || puppet->anim_count == 0u) {
        return NULL;
    }
    if (layer->anim_name[0] != '\0') {
        for (uint32_t i = 0; i < puppet->anim_count; i++) {
            if (strcasecmp(puppet->anims[i].name, layer->anim_name) == 0) {
                return &puppet->anims[i];
            }
        }
    }
    for (uint32_t i = 0; i < puppet->anim_count; i++) {
        if (puppet->anims[i].id == layer->anim_id) {
            return &puppet->anims[i];
        }
    }
    if (layer->anim_id >= 0 && (uint32_t)layer->anim_id < puppet->anim_count) {
        return &puppet->anims[layer->anim_id];
    }
    return NULL;
}

static void eval_track_at_time(const we_mdl_anim_t *anim, const we_mdl_bone_track_t *tr,
                               double time_sec, float out_pos[3], float out_angle[3],
                               float out_scale[3])
{
    if (!tr || tr->kf_count == 0u) {
        return;
    }
    if (tr->kf_count == 1u) {
        memcpy(out_pos, tr->keyframes[0].pos, 3 * sizeof(float));
        memcpy(out_angle, tr->keyframes[0].angle, 3 * sizeof(float));
        memcpy(out_scale, tr->keyframes[0].scale, 3 * sizeof(float));
        return;
    }

    double fps = (anim->fps > 0.0f) ? (double)anim->fps : 30.0;
    double frame_time = 1.0 / fps;
    double max_time = (double)anim->length / fps;
    (void)max_time;

    double frame_pos = (isfinite(time_sec) ? time_sec : 0.0) / frame_time;
    uint32_t f0 = 0, f1 = 0;
    float alpha = 0.0f;

    if (anim->mode == WE_ANIM_SINGLE) {
        double max_f = (double)(tr->kf_count - 1u);
        double clamped = fmin(fmax(frame_pos, 0.0), max_f);
        double fl = floor(clamped);
        f0 = (uint32_t)fl;
        f1 = (f0 + 1u < tr->kf_count) ? (f0 + 1u) : f0;
        alpha = (float)(clamped - fl);
    } else if (anim->mode == WE_ANIM_MIRROR) {
        double span = (double)(anim->length > 1 ? (anim->length - 1) : 1);
        double period = 2.0 * span;
        double wrapped = fmod(frame_pos, period);
        if (wrapped < 0.0) {
            wrapped += period;
        }
        if (wrapped > span) {
            wrapped = period - wrapped;
        }
        double fl = floor(wrapped);
        f0 = (uint32_t)fl;
        if (f0 >= tr->kf_count) {
            f0 = tr->kf_count - 1u;
        }
        f1 = (f0 + 1u < tr->kf_count) ? (f0 + 1u) : f0;
        alpha = (float)(wrapped - fl);
    } else {
        /* WE_ANIM_LOOP: max_time = length / fps, cycle length = anim->length frames */
        double cycle = (double)(anim->length > 0 ? anim->length : (int32_t)tr->kf_count);
        double wrapped = fmod(frame_pos, cycle);
        if (wrapped < 0.0) {
            wrapped += cycle;
        }
        double fl = floor(wrapped);
        f0 = ((uint32_t)fl) % tr->kf_count;
        f1 = (f0 + 1u) % tr->kf_count;
        alpha = (float)(wrapped - fl);
    }

    const we_mdl_keyframe_t *k0 = &tr->keyframes[f0];
    const we_mdl_keyframe_t *k1 = &tr->keyframes[f1];
    for (int i = 0; i < 3; i++) {
        out_pos[i] = k0->pos[i] + alpha * (k1->pos[i] - k0->pos[i]);
        out_angle[i] = k0->angle[i] + alpha * (k1->angle[i] - k0->angle[i]);
        out_scale[i] = k0->scale[i] + alpha * (k1->scale[i] - k0->scale[i]);
    }
}

void we_puppet_eval_skin_matrices(const we_puppet_t *puppet, const we_puppet_layer_t *layers,
                                  uint32_t layer_count, double time_sec, float (*out_skin_mats)[16])
{
    if (!puppet || !out_skin_mats || puppet->bone_count == 0u) {
        return;
    }

    we_puppet_layer_t default_layer;
    const we_puppet_layer_t *active_layers = layers;
    uint32_t active_count = layer_count;

    if ((!active_layers || active_count == 0u) && puppet->layer_count > 0u) {
        active_layers = puppet->layers;
        active_count = puppet->layer_count;
    }
    if ((!active_layers || active_count == 0u) && puppet->anim_count > 0u) {
        memset(&default_layer, 0, sizeof(default_layer));
        default_layer.anim_id = puppet->anims[0].id;
        default_layer.rate = 1.0f;
        default_layer.blend = 1.0f;
        default_layer.visible = true;
        active_layers = &default_layer;
        active_count = 1u;
    }

    float anim_local[WE_MDL_MAX_BONES][16];
    float world_mats[WE_MDL_MAX_BONES][16];

    for (uint32_t k = 0; k < puppet->bone_count; k++) {
        const we_mdl_bone_t *bone = &puppet->bones[k];
        float pos[3] = {bone->bind_pos[0], bone->bind_pos[1], bone->bind_pos[2]};
        float angle[3] = {bone->bind_angle[0], bone->bind_angle[1], bone->bind_angle[2]};
        float scale[3] = {bone->bind_scale[0], bone->bind_scale[1], bone->bind_scale[2]};
        bool animated = false;

        for (uint32_t l = 0; l < active_count; l++) {
            const we_puppet_layer_t *lay = &active_layers[l];
            if (!lay->visible || lay->blend <= 0.0f) {
                continue;
            }
            const we_mdl_anim_t *anim = find_puppet_anim(puppet, lay);
            if (!anim) {
                continue;
            }
            for (uint32_t t = 0; t < anim->track_count; t++) {
                if (anim->tracks[t].bone_index == (int32_t)k && anim->tracks[t].kf_count > 0u) {
                    float t_pos[3], t_ang[3], t_scl[3];
                    float rate = isfinite(lay->rate) ? lay->rate : 1.0f;
                    eval_track_at_time(anim, &anim->tracks[t], time_sec * (double)rate, t_pos,
                                       t_ang, t_scl);
                    float w = fminf(fmaxf(lay->blend, 0.0f), 1.0f);
                    for (int c = 0; c < 3; c++) {
                        pos[c] = pos[c] + w * (t_pos[c] - pos[c]);
                        if (w >= 0.9999f) {
                            angle[c] = t_ang[c];
                        } else {
                            float da = remainderf(t_ang[c] - angle[c], 6.28318530717958647692f);
                            angle[c] = angle[c] + w * da;
                        }
                        scale[c] = scale[c] + w * (t_scl[c] - scale[c]);
                    }
                    animated = true;
                    break;
                }
            }
        }

        if (animated) {
            mat4_from_trs(pos, angle, scale, anim_local[k]);
        } else {
            memcpy(anim_local[k], bone->local_trans, sizeof(anim_local[k]));
        }
    }

    /* Pass 1: Compute animated world matrices down the bone hierarchy */
    for (uint32_t k = 0; k < puppet->bone_count; k++) {
        uint32_t parent = puppet->bones[k].parent;
        if (parent == 0xFFFFFFFFu || parent >= k) {
            memcpy(world_mats[k], anim_local[k], sizeof(world_mats[k]));
        } else {
            we_mat4_mul(world_mats[parent], anim_local[k], world_mats[k]);
        }
    }

    /* Pass 2: Post-multiply each bone's world matrix by its inverse bind matrix (offset_trans)
     * in a separate loop so child bones NEVER inherit parent inverse bind matrices. */
    for (uint32_t k = 0; k < puppet->bone_count; k++) {
        we_mat4_mul(world_mats[k], puppet->bones[k].offset_trans, out_skin_mats[k]);
    }
}

void we_puppet_skin_vertices_cpu(const we_puppet_t *puppet, const float (*skin_mats)[16],
                                 we_skinned_vertex_t *out_verts)
{
    if (!puppet || !out_verts || !puppet->vertices) {
        return;
    }

    for (uint32_t i = 0; i < puppet->vertex_count; i++) {
        const we_mdl_vertex_t *v = &puppet->vertices[i];
        out_verts[i].uv[0] = v->texcoord[0];
        out_verts[i].uv[1] = v->texcoord[1];

        if (!skin_mats || puppet->bone_count == 0u) {
            out_verts[i].pos[0] = v->pos[0];
            out_verts[i].pos[1] = v->pos[1];
            out_verts[i].pos[2] = v->pos[2];
            continue;
        }

        float sx = 0.0f, sy = 0.0f, sz = 0.0f;
        float w_accum = 0.0f;
        float px = v->pos[0], py = v->pos[1], pz = v->pos[2];

        for (int j = 0; j < 4; j++) {
            float w = v->weight[j];
            if (w <= 0.0f) {
                continue;
            }
            uint32_t bidx = v->blend_indices[j];
            if (bidx < puppet->bone_count) {
                const float *m = skin_mats[bidx];
                float tx = m[0] * px + m[4] * py + m[8] * pz + m[12];
                float ty = m[1] * px + m[5] * py + m[9] * pz + m[13];
                float tz = m[2] * px + m[6] * py + m[10] * pz + m[14];
                sx += w * tx;
                sy += w * ty;
                sz += w * tz;
            } else {
                sx += w * px;
                sy += w * py;
                sz += w * pz;
            }
            w_accum += w;
        }

        if (w_accum > 1e-6f) {
            out_verts[i].pos[0] = sx;
            out_verts[i].pos[1] = sy;
            out_verts[i].pos[2] = sz;
        } else {
            out_verts[i].pos[0] = px;
            out_verts[i].pos[1] = py;
            out_verts[i].pos[2] = pz;
        }
    }
}

typedef struct {
    float blend_indices_f[4];
    uint32_t blend_indices_u[4];
    float weight[4];
} we_gpu_skin_attrib_t;

bool we_puppet_upload_gl(we_puppet_t *puppet)
{
    if (!puppet || puppet->vertex_count == 0u || puppet->index_count == 0u) {
        return false;
    }
    if (puppet->gl_uploaded) {
        return true;
    }

    GLuint vao = 0, vbo_dyn = 0, vbo_skin = 0, ebo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo_dyn);
    glGenBuffers(1, &vbo_skin);
    glGenBuffers(1, &ebo);

    we_gpu_skin_attrib_t *skin_buf = calloc(puppet->vertex_count, sizeof(we_gpu_skin_attrib_t));
    if (!skin_buf) {
        glDeleteBuffers(1, &ebo);
        glDeleteBuffers(1, &vbo_skin);
        glDeleteBuffers(1, &vbo_dyn);
        glDeleteVertexArrays(1, &vao);
        return false;
    }

    glBindVertexArray(vao);

    /* Dynamic VBO: a_Position (loc 0, vec3) + a_TexCoord (loc 1, vec2) */
    we_puppet_skin_vertices_cpu(puppet, NULL, puppet->skinned_verts);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_dyn);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(puppet->vertex_count * sizeof(we_skinned_vertex_t)),
                 puppet->skinned_verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(we_skinned_vertex_t), (const void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(we_skinned_vertex_t),
                          (const void *)(3 * sizeof(float)));

    for (uint32_t i = 0; i < puppet->vertex_count; i++) {
        for (int j = 0; j < 4; j++) {
            uint32_t bidx = puppet->vertices[i].blend_indices[j];
            if (puppet->bone_count > 0u && bidx >= puppet->bone_count) {
                bidx = 0u;
            }
            skin_buf[i].blend_indices_f[j] = (float)bidx;
            skin_buf[i].blend_indices_u[j] = bidx;
            skin_buf[i].weight[j] = puppet->vertices[i].weight[j];
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, vbo_skin);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(puppet->vertex_count * sizeof(we_gpu_skin_attrib_t)),
                 skin_buf, GL_STATIC_DRAW);
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(we_gpu_skin_attrib_t),
                          (const void *)offsetof(we_gpu_skin_attrib_t, blend_indices_f));
    glEnableVertexAttribArray(6);
    glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, sizeof(we_gpu_skin_attrib_t),
                          (const void *)offsetof(we_gpu_skin_attrib_t, weight));
    free(skin_buf);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(puppet->index_count * sizeof(uint16_t)),
                 puppet->indices, GL_STATIC_DRAW);

    glBindVertexArray(0);

    puppet->gl_vao = vao;
    puppet->gl_vbo_dyn = vbo_dyn;
    puppet->gl_vbo_skin = vbo_skin;
    puppet->gl_ebo = ebo;
    puppet->gl_uploaded = true;
    puppet->vbo_has_bind_pose = true;
    return true;
}

void we_puppet_draw_gl(we_puppet_t *puppet, uint32_t gl_prog, double time_sec)
{
    if (!puppet || puppet->vertex_count == 0u || puppet->index_count == 0u) {
        return;
    }
    if (!puppet->gl_uploaded && !we_puppet_upload_gl(puppet)) {
        return;
    }

    glBindVertexArray(puppet->gl_vao);

    GLint loc_bones = -1;
    GLenum bone_utype = GL_FLOAT_MAT4;
    if (gl_prog != 0) {
        loc_bones = glGetUniformLocation((GLuint)gl_prog, "g_Bones");
        if (loc_bones < 0) {
            loc_bones = glGetUniformLocation((GLuint)gl_prog, "g_Bones[0]");
        }
        if (loc_bones >= 0) {
            GLint active_uniforms = 0;
            glGetProgramiv((GLuint)gl_prog, GL_ACTIVE_UNIFORMS, &active_uniforms);
            for (GLint u = 0; u < active_uniforms; u++) {
                char uname[64];
                GLsizei ulen = 0;
                GLint usize = 0;
                GLenum utype = 0;
                glGetActiveUniform((GLuint)gl_prog, (GLuint)u, (GLsizei)sizeof(uname), &ulen,
                                   &usize, &utype, uname);
                if (strcmp(uname, "g_Bones") == 0 || strcmp(uname, "g_Bones[0]") == 0) {
                    bone_utype = utype;
                    break;
                }
            }
        }

        GLint active_attribs = 0;
        glGetProgramiv((GLuint)gl_prog, GL_ACTIVE_ATTRIBUTES, &active_attribs);
        GLenum blend_idx_atype = GL_FLOAT_VEC4;
        for (GLint a = 0; a < active_attribs; a++) {
            char aname[64];
            GLsizei alen = 0;
            GLint asize = 0;
            GLenum atype = 0;
            glGetActiveAttrib((GLuint)gl_prog, (GLuint)a, (GLsizei)sizeof(aname), &alen, &asize,
                              &atype, aname);
            if (strcmp(aname, "a_BlendIndices") == 0) {
                blend_idx_atype = atype;
                break;
            }
        }
        glBindBuffer(GL_ARRAY_BUFFER, puppet->gl_vbo_skin);
        if (blend_idx_atype == GL_UNSIGNED_INT_VEC4 || blend_idx_atype == GL_INT_VEC4 ||
            blend_idx_atype == GL_UNSIGNED_INT || blend_idx_atype == GL_INT) {
            GLenum itype = (blend_idx_atype == GL_INT_VEC4 || blend_idx_atype == GL_INT)
                               ? GL_INT
                               : GL_UNSIGNED_INT;
            glVertexAttribIPointer(5, 4, itype, sizeof(we_gpu_skin_attrib_t),
                                   (const void *)offsetof(we_gpu_skin_attrib_t, blend_indices_u));
        } else {
            glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(we_gpu_skin_attrib_t),
                                  (const void *)offsetof(we_gpu_skin_attrib_t, blend_indices_f));
        }
    }

    if (puppet->bone_count > 0u && puppet->skin_mats) {
        we_puppet_eval_skin_matrices(puppet, puppet->layers, puppet->layer_count, time_sec,
                                     puppet->skin_mats);
        uint32_t bcnt = puppet->bone_count;
        if (bcnt > WE_MDL_MAX_BONES) {
            bcnt = WE_MDL_MAX_BONES;
        }
        if (loc_bones >= 0) {
            if (bone_utype == GL_FLOAT_MAT4x3) {
                float mats4x3[WE_MDL_MAX_BONES * 12];
                for (uint32_t b = 0; b < bcnt; b++) {
                    const float *m = puppet->skin_mats[b];
                    float *d = &mats4x3[b * 12u];
                    d[0] = m[0];
                    d[1] = m[1];
                    d[2] = m[2];
                    d[3] = m[4];
                    d[4] = m[5];
                    d[5] = m[6];
                    d[6] = m[8];
                    d[7] = m[9];
                    d[8] = m[10];
                    d[9] = m[12];
                    d[10] = m[13];
                    d[11] = m[14];
                }
                glUniformMatrix4x3fv(loc_bones, (GLsizei)bcnt, GL_FALSE, mats4x3);
            } else if (bone_utype == GL_FLOAT_MAT3x4) {
                float mats3x4[WE_MDL_MAX_BONES * 12];
                for (uint32_t b = 0; b < bcnt; b++) {
                    const float *m = puppet->skin_mats[b];
                    float *d = &mats3x4[b * 12u];
                    d[0] = m[0];
                    d[1] = m[4];
                    d[2] = m[8];
                    d[3] = m[12];
                    d[4] = m[1];
                    d[5] = m[5];
                    d[6] = m[9];
                    d[7] = m[13];
                    d[8] = m[2];
                    d[9] = m[6];
                    d[10] = m[10];
                    d[11] = m[14];
                }
                glUniformMatrix3x4fv(loc_bones, (GLsizei)bcnt, GL_FALSE, mats3x4);
            } else {
                glUniformMatrix4fv(loc_bones, (GLsizei)bcnt, GL_FALSE, &puppet->skin_mats[0][0]);
            }
            if (!puppet->vbo_has_bind_pose) {
                we_puppet_skin_vertices_cpu(puppet, NULL, puppet->skinned_verts);
                glBindBuffer(GL_ARRAY_BUFFER, puppet->gl_vbo_dyn);
                glBufferSubData(GL_ARRAY_BUFFER, 0,
                                (GLsizeiptr)(puppet->vertex_count * sizeof(we_skinned_vertex_t)),
                                puppet->skinned_verts);
                puppet->vbo_has_bind_pose = true;
            }
        } else {
            /* CPU-skin into VBO so shaders without #if SKINNING render deformed meshes */
            we_puppet_skin_vertices_cpu(puppet, puppet->skin_mats, puppet->skinned_verts);
            glBindBuffer(GL_ARRAY_BUFFER, puppet->gl_vbo_dyn);
            glBufferSubData(GL_ARRAY_BUFFER, 0,
                            (GLsizeiptr)(puppet->vertex_count * sizeof(we_skinned_vertex_t)),
                            puppet->skinned_verts);
            puppet->vbo_has_bind_pose = false;
        }
    } else if (loc_bones >= 0) {
        if (bone_utype == GL_FLOAT_MAT4x3) {
            const float id4x3[12] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                     0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
            glUniformMatrix4x3fv(loc_bones, 1, GL_FALSE, id4x3);
        } else if (bone_utype == GL_FLOAT_MAT3x4) {
            const float id3x4[12] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                     0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
            glUniformMatrix3x4fv(loc_bones, 1, GL_FALSE, id3x4);
        } else {
            const float id4[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                   0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
            glUniformMatrix4fv(loc_bones, 1, GL_FALSE, id4);
        }
    }

    glDrawElements(GL_TRIANGLES, (GLsizei)puppet->index_count, GL_UNSIGNED_SHORT, (const void *)0);
}

void we_puppet_destroy(we_puppet_t *puppet)
{
    if (!puppet) {
        return;
    }
    if (puppet->gl_uploaded) {
        if (puppet->gl_ebo) {
            GLuint b = puppet->gl_ebo;
            glDeleteBuffers(1, &b);
        }
        if (puppet->gl_vbo_skin) {
            GLuint b = puppet->gl_vbo_skin;
            glDeleteBuffers(1, &b);
        }
        if (puppet->gl_vbo_dyn) {
            GLuint b = puppet->gl_vbo_dyn;
            glDeleteBuffers(1, &b);
        }
        if (puppet->gl_vao) {
            GLuint v = puppet->gl_vao;
            glDeleteVertexArrays(1, &v);
        }
    }
    if (puppet->anims) {
        for (uint32_t a = 0; a < puppet->anim_count; a++) {
            if (puppet->anims[a].tracks) {
                for (uint32_t t = 0; t < puppet->anims[a].track_count; t++) {
                    free(puppet->anims[a].tracks[t].keyframes);
                }
                free(puppet->anims[a].tracks);
            }
        }
        free(puppet->anims);
    }
    free(puppet->bones);
    free(puppet->skin_mats);
    free(puppet->skinned_verts);
    free(puppet->indices);
    free(puppet->vertices);
    free(puppet);
}
