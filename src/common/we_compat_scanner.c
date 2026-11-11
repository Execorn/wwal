// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "waywal/we_compat_scanner.h"

#include "waywal/log.h"
#include "waywal/we_json.h"
#include "waywal/we_pkg.h"
#include "waywal/we_project.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/openat2.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

static_assert(sizeof(we_compat_report_t) == 252280,
              "sizeof(we_compat_report_t) must equal exactly 252280 bytes");

struct we_compat_scanner {
    bool supports_compute;
    bool supports_bc7;
};

we_compat_report_t *we_compat_report_alloc(void)
{
    return (we_compat_report_t *)calloc(1, sizeof(we_compat_report_t));
}

void we_compat_report_free(we_compat_report_t *report) { free(report); }

we_compat_scanner_t *we_compat_scanner_create(bool supports_compute, bool supports_bc7)
{
    we_compat_scanner_t *s = (we_compat_scanner_t *)calloc(1, sizeof(we_compat_scanner_t));
    if (!s) {
        return NULL;
    }
    s->supports_compute = supports_compute;
    s->supports_bc7 = supports_bc7;
    return s;
}

void we_compat_scanner_destroy(we_compat_scanner_t *scanner) { free(scanner); }

static void report_add_issue(we_compat_report_t *report, const char *code,
                             we_issue_severity_t severity, const char *msg, const char *file_path,
                             uint32_t line_no, const char *suggested_fix)
{
    if (!report || report->issue_count >= WE_SCAN_MAX_ISSUES) {
        return;
    }
    we_compat_issue_t *iss = &report->issues[report->issue_count++];
    snprintf(iss->code, sizeof(iss->code), "%s", code ? code : "");
    iss->severity = severity;
    snprintf(iss->message, sizeof(iss->message), "%s", msg ? msg : "");
    snprintf(iss->file_path, sizeof(iss->file_path), "%s", file_path ? file_path : "");
    iss->line_number = line_no;
    snprintf(iss->suggested_fix, sizeof(iss->suggested_fix), "%s",
             suggested_fix ? suggested_fix : "");
}

static void report_add_repair(we_compat_report_t *report, const char *transformation_id,
                              const char *target_file, const char *description)
{
    if (!report || report->repair_count >= WE_SCAN_MAX_REPAIRS) {
        return;
    }
    for (uint32_t i = 0; i < report->repair_count; i++) {
        if (strcmp(report->repairs[i].transformation_id, transformation_id) == 0 &&
            strcmp(report->repairs[i].target_file, target_file) == 0) {
            return;
        }
    }
    we_auto_repair_t *rep = &report->repairs[report->repair_count++];
    snprintf(rep->transformation_id, sizeof(rep->transformation_id), "%s",
             transformation_id ? transformation_id : "");
    snprintf(rep->target_file, sizeof(rep->target_file), "%s", target_file ? target_file : "");
    snprintf(rep->description, sizeof(rep->description), "%s", description ? description : "");
}

static inline uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) |
           ((uint32_t)p[3] << 24u);
}

static bool has_ext_ci(const char *path, size_t plen, const char *ext)
{
    size_t elen = strlen(ext);
    if (plen < elen) {
        return false;
    }
    return strcasecmp(path + plen - elen, ext) == 0;
}

static const char *slice_find_str(const char *haystack, size_t hlen, const char *needle)
{
    if (!haystack || !needle) {
        return NULL;
    }
    size_t nlen = strlen(needle);
    if (nlen == 0u) {
        return haystack;
    }
    if (hlen < nlen) {
        return NULL;
    }
    return (const char *)memmem(haystack, hlen, needle, nlen);
}

static bool slice_contains(const char *haystack, size_t hlen, const char *needle)
{
    return slice_find_str(haystack, hlen, needle) != NULL;
}

/* =========================================================================
 * Growable OOM-Safe String Builder
 * ========================================================================= */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool oom;
} compat_sb_t;

static void sb_init(compat_sb_t *sb)
{
    sb->cap = 4096u;
    sb->buf = (char *)malloc(sb->cap);
    sb->len = 0;
    sb->oom = (sb->buf == NULL);
    if (sb->buf) {
        sb->buf[0] = '\0';
    }
}

static bool sb_append_n(compat_sb_t *sb, const char *s, size_t slen)
{
    if (!sb || sb->oom || !sb->buf || (!s && slen > 0u)) {
        if (sb) {
            sb->oom = true;
        }
        return false;
    }
    if (slen == 0u) {
        return true;
    }
    if (slen > SIZE_MAX - sb->len - 1u) {
        sb->oom = true;
        return false;
    }

    size_t needed = sb->len + slen + 1u;
    if (needed > sb->cap) {
        size_t ncap = sb->cap ? sb->cap : 4096u;
        while (ncap < needed) {
            if (ncap > SIZE_MAX / 2u) {
                ncap = needed;
                break;
            }
            ncap *= 2u;
        }
        char *nb = (char *)realloc(sb->buf, ncap);
        if (!nb) {
            sb->oom = true;
            return false;
        }
        sb->buf = nb;
        sb->cap = ncap;
    }

    memcpy(sb->buf + sb->len, s, slen);
    sb->len += slen;
    sb->buf[sb->len] = '\0';
    return true;
}

static bool sb_append(compat_sb_t *sb, const char *s)
{
    if (!s) {
        return false;
    }
    return sb_append_n(sb, s, strlen(s));
}

static void sb_append_json_str(compat_sb_t *sb, const char *s)
{
    (void)sb_append_n(sb, "\"", 1u);
    if (s) {
        for (size_t i = 0; s[i]; i++) {
            unsigned char c = (unsigned char)s[i];
            if (c == '"') {
                (void)sb_append_n(sb, "\\\"", 2u);
            } else if (c == '\\') {
                (void)sb_append_n(sb, "\\\\", 2u);
            } else if (c == '\n') {
                (void)sb_append_n(sb, "\\n", 2u);
            } else if (c == '\r') {
                (void)sb_append_n(sb, "\\r", 2u);
            } else if (c == '\t') {
                (void)sb_append_n(sb, "\\t", 2u);
            } else if (c < 0x20u) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", c);
                (void)sb_append(sb, esc);
            } else {
                char ch = (char)c;
                (void)sb_append_n(sb, &ch, 1u);
            }
        }
    }
    (void)sb_append_n(sb, "\"", 1u);
}

/* =========================================================================
 * Rolling XOR De-masking & Binary PKG Inspection
 * ========================================================================= */

static void xor_demask_buffer(const uint8_t *src, size_t len, const uint8_t *key, size_t key_len,
                              uint8_t *dst)
{
    if (!src || !dst || len == 0 || key_len == 0) {
        return;
    }

#if defined(__AVX2__)
    if (key_len == 1 || key_len == 2 || key_len == 4 || key_len == 8 || key_len == 16) {
        uint8_t key32[32];
        for (size_t k = 0; k < 32; k++) {
            key32[k] = key[k % key_len];
        }
        __m256i vkey = _mm256_loadu_si256((const __m256i *)key32);
        size_t i = 0;
        for (; i + 32 <= len; i += 32) {
            __m256i vdata = _mm256_loadu_si256((const __m256i *)(src + i));
            __m256i vout = _mm256_xor_si256(vdata, vkey);
            _mm256_storeu_si256((__m256i *)(dst + i), vout);
        }
        for (; i < len; i++) {
            dst[i] = src[i] ^ key[i % key_len];
        }
        return;
    }
#endif

    for (size_t i = 0; i < len; i++) {
        dst[i] = src[i] ^ key[i % key_len];
    }
}

static bool check_pkg_version_digits(const uint8_t *ver4)
{
    if (!isdigit(ver4[0]) || !isdigit(ver4[1]) || !isdigit(ver4[2]) || !isdigit(ver4[3])) {
        return false;
    }
    uint32_t v = (uint32_t)(ver4[0] - '0') * 1000u + (uint32_t)(ver4[1] - '0') * 100u +
                 (uint32_t)(ver4[2] - '0') * 10u + (uint32_t)(ver4[3] - '0');
    return (v >= 1u && v <= 99u);
}

static inline uint8_t demask_byte_at(const uint8_t *data, const uint8_t *key, size_t period,
                                     size_t pos)
{
    return period == 0u ? data[pos] : (uint8_t)(data[pos] ^ key[pos % period]);
}

static inline uint32_t demask_u32_at(const uint8_t *data, const uint8_t *key, size_t period,
                                     size_t pos)
{
    return (uint32_t)demask_byte_at(data, key, period, pos) |
           ((uint32_t)demask_byte_at(data, key, period, pos + 1u) << 8u) |
           ((uint32_t)demask_byte_at(data, key, period, pos + 2u) << 16u) |
           ((uint32_t)demask_byte_at(data, key, period, pos + 3u) << 24u);
}

static bool validate_demasked_pkg_at(const uint8_t *data, size_t size, const uint8_t *key,
                                     size_t period, size_t table_cursor, bool *out_exact_end)
{
    if (out_exact_end) {
        *out_exact_end = false;
    }
    if (table_cursor + 4u > size) {
        return false;
    }

    uint32_t entry_count = demask_u32_at(data, key, period, table_cursor);
    if (entry_count > WE_PKG_MAX_ENTRIES) {
        return false;
    }

    size_t cursor = table_cursor + 4u;
    if (entry_count == 0u) {
        if (out_exact_end) {
            *out_exact_end = (cursor == size);
        }
        return cursor <= size;
    }

    if ((uint64_t)entry_count * 13ULL > (uint64_t)(size - cursor)) {
        return false;
    }

    for (uint32_t i = 0; i < entry_count; i++) {
        if (cursor + 4u > size) {
            return false;
        }
        uint32_t path_len = demask_u32_at(data, key, period, cursor);
        cursor += 4u;
        if (path_len == 0u || path_len > WE_PKG_MAX_PATH_LEN || cursor + path_len + 8u > size) {
            return false;
        }

        size_t eff_len = path_len;
        while (eff_len > 0u && demask_byte_at(data, key, period, cursor + eff_len - 1u) == '\0') {
            eff_len--;
        }
        if (eff_len == 0u) {
            return false;
        }
        for (size_t k = 0; k < eff_len; k++) {
            uint8_t ch = demask_byte_at(data, key, period, cursor + k);
            if (ch < 0x20u || ch >= 0x7Fu) {
                return false;
            }
        }
        cursor += path_len + 8u;
    }

    size_t data_base = cursor;
    size_t max_end = data_base;
    size_t walk = table_cursor + 4u;
    for (uint32_t i = 0; i < entry_count; i++) {
        uint32_t path_len = demask_u32_at(data, key, period, walk);
        walk += 4u + path_len;
        uint32_t off = demask_u32_at(data, key, period, walk);
        uint32_t len = demask_u32_at(data, key, period, walk + 4u);
        walk += 8u;

        uint64_t start = (uint64_t)data_base + (uint64_t)off;
        uint64_t end = start + (uint64_t)len;
        if (start < (uint64_t)data_base || end < start || end > (uint64_t)size) {
            return false;
        }
        if ((size_t)end > max_end) {
            max_end = (size_t)end;
        }
    }

    if (out_exact_end) {
        *out_exact_end = (max_end == size);
    }
    return true;
}

static bool validate_demasked_pkg(const uint8_t *data, size_t size, const uint8_t *key,
                                  size_t period, bool *out_exact_end)
{
    if (size < 16u) {
        return false;
    }
    uint32_t magic_len = demask_u32_at(data, key, period, 0u);
    if (magic_len != 8u && magic_len != 9u) {
        return false;
    }
    size_t table_cursor = 4u + (size_t)magic_len;
    if (validate_demasked_pkg_at(data, size, key, period, table_cursor, out_exact_end)) {
        return true;
    }
    if (table_cursor < size && demask_byte_at(data, key, period, table_cursor) == '\0') {
        return validate_demasked_pkg_at(data, size, key, period, table_cursor + 1u, out_exact_end);
    }
    return false;
}

/* Detects rolling XOR obfuscation for periods P in {1, 4, 8, 16}.
 * Returns key period (0 if plain unmasked PKGV, > 0 if masked), or -1 if invalid container. */
static int detect_rolling_xor_key(const uint8_t *data, size_t size, uint8_t out_key[16])
{
    if (!data || size < 16u) {
        return -1;
    }
    memset(out_key, 0, 16);

    /* 1. Plain unmasked PKGV */
    {
        uint32_t magic_len = read_u32_le(data);
        if ((magic_len == 8u || magic_len == 9u) && memcmp(data + 4, "PKGV", 4) == 0 &&
            check_pkg_version_digits(data + 8) &&
            validate_demasked_pkg(data, size, NULL, 0u, NULL)) {
            return 0;
        }
    }

    /* 2. Period 1 */
    {
        uint8_t k = (uint8_t)(data[4] ^ (uint8_t)'P');
        if (k != 0u && (data[5] ^ k) == 'K' && (data[6] ^ k) == 'G' && (data[7] ^ k) == 'V') {
            uint8_t m0 = (uint8_t)(data[0] ^ k);
            if ((m0 == 8u || m0 == 9u) && (data[1] ^ k) == 0u && (data[2] ^ k) == 0u &&
                (data[3] ^ k) == 0u) {
                uint8_t ver[4] = {(uint8_t)(data[8] ^ k), (uint8_t)(data[9] ^ k),
                                  (uint8_t)(data[10] ^ k), (uint8_t)(data[11] ^ k)};
                if (check_pkg_version_digits(ver) &&
                    validate_demasked_pkg(data, size, &k, 1u, NULL)) {
                    out_key[0] = k;
                    return 1;
                }
            }
        }
    }

    /* 3. Period 4 */
    {
        uint8_t k[4] = {(uint8_t)(data[4] ^ (uint8_t)'P'), (uint8_t)(data[5] ^ (uint8_t)'K'),
                        (uint8_t)(data[6] ^ (uint8_t)'G'), (uint8_t)(data[7] ^ (uint8_t)'V')};
        if ((k[0] | k[1] | k[2] | k[3]) != 0u) {
            uint8_t m0 = (uint8_t)(data[0] ^ k[0]);
            if ((m0 == 8u || m0 == 9u) && (data[1] ^ k[1]) == 0u && (data[2] ^ k[2]) == 0u &&
                (data[3] ^ k[3]) == 0u) {
                uint8_t ver[4] = {(uint8_t)(data[8] ^ k[0]), (uint8_t)(data[9] ^ k[1]),
                                  (uint8_t)(data[10] ^ k[2]), (uint8_t)(data[11] ^ k[3])};
                if (check_pkg_version_digits(ver) &&
                    validate_demasked_pkg(data, size, k, 4u, NULL)) {
                    memcpy(out_key, k, 4);
                    return 4;
                }
            }
        }
    }

    /* 4. Period 8: validate table so Period-16 keys with matching k[0..3]/k[8..11] do not
     * misclassify */
    for (uint8_t mlen = 8u; mlen <= 9u; mlen++) {
        uint8_t k[8];
        k[0] = (uint8_t)(data[0] ^ mlen);
        k[1] = data[1];
        k[2] = data[2];
        k[3] = data[3];
        k[4] = (uint8_t)(data[4] ^ (uint8_t)'P');
        k[5] = (uint8_t)(data[5] ^ (uint8_t)'K');
        k[6] = (uint8_t)(data[6] ^ (uint8_t)'G');
        k[7] = (uint8_t)(data[7] ^ (uint8_t)'V');

        uint8_t ver[4] = {(uint8_t)(data[8] ^ k[0]), (uint8_t)(data[9] ^ k[1]),
                          (uint8_t)(data[10] ^ k[2]), (uint8_t)(data[11] ^ k[3])};
        if (check_pkg_version_digits(ver) && validate_demasked_pkg(data, size, k, 8u, NULL)) {
            memcpy(out_key, k, 8);
            return 8;
        }
    }

    /* 5. Period 16: recover k[12..15] from candidate entry_count rather than zeroing entry_count */
    {
        uint8_t k[16];
        k[0] = (uint8_t)(data[0] ^ 8u);
        k[1] = data[1];
        k[2] = data[2];
        k[3] = data[3];
        k[4] = (uint8_t)(data[4] ^ (uint8_t)'P');
        k[5] = (uint8_t)(data[5] ^ (uint8_t)'K');
        k[6] = (uint8_t)(data[6] ^ (uint8_t)'G');
        k[7] = (uint8_t)(data[7] ^ (uint8_t)'V');

        if (size == 16u) {
            k[8] = (uint8_t)(data[8] ^ (uint8_t)'0');
            k[9] = (uint8_t)(data[9] ^ (uint8_t)'0');
            k[10] = (uint8_t)(data[10] ^ (uint8_t)'0');
            k[11] = (uint8_t)(data[11] ^ (uint8_t)'1');
            k[12] = data[12];
            k[13] = data[13];
            k[14] = data[14];
            k[15] = data[15];
            memcpy(out_key, k, 16);
            return 16;
        }

        if (size >= 29u) {
            /* Offset 16 (path0_len) aligns to 16 % 16 == 0, so k[0..3] decodes it in O(1) */
            uint32_t path0_len = (uint32_t)(data[16] ^ k[0]) | ((uint32_t)(data[17] ^ k[1]) << 8u) |
                                 ((uint32_t)(data[18] ^ k[2]) << 16u) |
                                 ((uint32_t)(data[19] ^ k[3]) << 24u);

            if (path0_len >= 1u && path0_len <= WE_PKG_MAX_PATH_LEN &&
                20u + (size_t)path0_len + 8u <= size) {
                /* Offset 20..23 aligns to k[4..7]; verify initial path bytes before searching */
                bool prefix_ok = true;
                size_t pre_check = path0_len < 4u ? (size_t)path0_len : 4u;
                for (size_t p = 0; p < pre_check; p++) {
                    uint8_t ch = (uint8_t)(data[20u + p] ^ k[4u + p]);
                    if ((ch < 0x20u || ch >= 0x7Fu) && !(ch == '\0' && p + 1u == path0_len)) {
                        prefix_ok = false;
                        break;
                    }
                }

                if (prefix_ok) {
                    size_t max_possible = (size - 16u) / 13u;
                    if (max_possible > WE_PKG_MAX_ENTRIES) {
                        max_possible = WE_PKG_MAX_ENTRIES;
                    }

                    uint8_t fallback_key[16];
                    bool have_fallback = false;

                    for (uint32_t v = 1u; v <= 24u; v++) {
                        k[8] = (uint8_t)(data[8] ^ (uint8_t)'0');
                        k[9] = (uint8_t)(data[9] ^ (uint8_t)'0');
                        k[10] = (uint8_t)(data[10] ^ (uint8_t)('0' + (v / 10u)));
                        k[11] = (uint8_t)(data[11] ^ (uint8_t)('0' + (v % 10u)));

                        if (path0_len > 4u) {
                            size_t v_end = path0_len < 8u ? (size_t)path0_len : 8u;
                            bool v_ok = true;
                            for (size_t p = 4u; p < v_end; p++) {
                                uint8_t ch = (uint8_t)(data[20u + p] ^ k[4u + p]);
                                if ((ch < 0x20u || ch >= 0x7Fu) &&
                                    !(ch == '\0' && p + 1u == path0_len)) {
                                    v_ok = false;
                                    break;
                                }
                            }
                            if (!v_ok) {
                                continue;
                            }
                        }

                        for (uint32_t ec = 1u; ec <= (uint32_t)max_possible; ec++) {
                            k[12] = (uint8_t)(data[12] ^ (uint8_t)(ec & 0xFFu));
                            k[13] = (uint8_t)(data[13] ^ (uint8_t)((ec >> 8u) & 0xFFu));
                            k[14] = (uint8_t)(data[14] ^ (uint8_t)((ec >> 16u) & 0xFFu));
                            k[15] = (uint8_t)(data[15] ^ (uint8_t)((ec >> 24u) & 0xFFu));

                            bool exact_end = false;
                            if (validate_demasked_pkg_at(data, size, k, 16u, 12u, &exact_end)) {
                                if (exact_end) {
                                    memcpy(out_key, k, 16);
                                    return 16;
                                }
                                if (!have_fallback) {
                                    memcpy(fallback_key, k, 16);
                                    have_fallback = true;
                                }
                            }
                        }
                    }

                    if (have_fallback) {
                        memcpy(out_key, fallback_key, 16);
                        return 16;
                    }
                }
            }
        }
    }

    return -1;
}

/* =========================================================================
 * Path Normalization & Malware / Security Quarantine
 * ========================================================================= */

static bool has_directory_traversal(const char *path)
{
    if (!path) {
        return false;
    }
    const char *p = path;
    while (*p) {
        if ((unsigned char)*p < 0x20u || (unsigned char)*p == 0x7Fu) {
            return true;
        }
        if (p[0] == '.' && p[1] == '.') {
            if ((p == path || p[-1] == '/' || p[-1] == '\\') &&
                (p[2] == '\0' || p[2] == '/' || p[2] == '\\')) {
                return true;
            }
        }
        p++;
    }
    return false;
}

static bool is_executable_binary_payload(const uint8_t *data, size_t size, const char *path)
{
    if (!data || size < 2u) {
        return false;
    }

    if (data[0] == 0x4Du && data[1] == 0x5Au) {
        return true;
    }

    if (size >= 4u && data[0] == 0x7Fu && data[1] == 'E' && data[2] == 'L' && data[3] == 'F') {
        return true;
    }

    if (path) {
        const char *dot = strrchr(path, '.');
        if (dot) {
            if (strcasecmp(dot, ".exe") == 0 || strcasecmp(dot, ".dll") == 0 ||
                strcasecmp(dot, ".so") == 0 || strcasecmp(dot, ".bin") == 0 ||
                strcasecmp(dot, ".com") == 0 || strcasecmp(dot, ".scr") == 0) {
                return true;
            }
        }
    }

    return false;
}

/* =========================================================================
 * Fast MDL Skeletal Puppet & TEX Header Parsers
 * ========================================================================= */

typedef struct {
    uint32_t vertex_count;
    uint32_t triangle_count;
    uint32_t bone_count;
    bool is_valid;
} we_compat_mdl_info_t;

static bool parse_mdl_header_fast(const uint8_t *data, size_t size, we_compat_mdl_info_t *out)
{
    if (!data || size < 32u || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    size_t cur = 0;
    if (size >= 12u && memcmp(data, "MDLV", 4) != 0 && data[0] == 8u && data[1] == 0u) {
        cur = 4u;
    }

    if (cur + 8u > size || memcmp(data + cur, "MDLV", 4) != 0) {
        return false;
    }
    cur += 8u;
    if (cur < size && data[cur] == '\0') {
        cur++;
    }

    if (cur + 12u > size) {
        return false;
    }
    cur += 12u;

    if (cur + 4u <= size) {
        uint32_t slen = read_u32_le(data + cur);
        if (slen > 0u && slen < 1024u && cur + 4u + slen <= size) {
            cur += 4u + slen;
        } else {
            while (cur < size && data[cur] != '\0') {
                cur++;
            }
            if (cur < size) {
                cur++;
            }
        }
    }

    if (cur + 4u > size) {
        return false;
    }
    uint32_t curr = read_u32_le(data + cur);
    cur += 4u;

    uint32_t vertex_byte_size = 0;
    uint32_t stride = 52u;
    if (curr == 0x0180000Fu) {
        if (cur + 4u > size) {
            return false;
        }
        vertex_byte_size = read_u32_le(data + cur);
        cur += 4u;
        stride = 80u;
    } else {
        vertex_byte_size = curr;
        stride = 52u;
    }

    if (vertex_byte_size > size - cur) {
        return false;
    }
    if (vertex_byte_size > 0u && (vertex_byte_size % stride) == 0u) {
        out->vertex_count = vertex_byte_size / stride;
    }
    cur += vertex_byte_size;

    if (cur + 4u <= size) {
        uint32_t indices_size = read_u32_le(data + cur);
        cur += 4u;
        if (indices_size > size - cur) {
            return false;
        }
        if (indices_size > 0u && (indices_size % 6u) == 0u) {
            out->triangle_count = indices_size / 6u;
        }
        cur += indices_size;
    }

    while (cur + 8u <= size) {
        if (memcmp(data + cur, "MDLS", 4) == 0 && isdigit(data[cur + 4])) {
            cur += 8u;
            if (cur < size && data[cur] == '\0') {
                cur++;
            }
            if (cur + 4u <= size) {
                out->bone_count = read_u32_le(data + cur);
            }
            break;
        }
        cur++;
    }

    out->is_valid = (out->vertex_count > 0u || out->triangle_count > 0u || out->bone_count > 0u);
    return out->is_valid;
}

typedef struct {
    uint32_t format;
    uint32_t width;
    uint32_t height;
    bool is_bc7;
} we_compat_tex_info_t;

static bool parse_tex_header_fast(const uint8_t *data, size_t size, we_compat_tex_info_t *out)
{
    if (!data || size < 32u || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    size_t cur = 0;
    if (size >= 12u && memcmp(data, "TEXV", 4) != 0 && data[0] == 8u && data[1] == 0u) {
        cur = 4u;
    }
    if (cur + 8u > size || memcmp(data + cur, "TEXV", 4) != 0) {
        return false;
    }
    cur += 8u;
    if (cur < size && data[cur] == '\0') {
        cur++;
    }

    while (cur + 8u <= size) {
        if (memcmp(data + cur, "TEXI", 4) == 0) {
            cur += 8u;
            if (cur < size && data[cur] == '\0') {
                cur++;
            }
            if (cur + 24u <= size) {
                out->format = read_u32_le(data + cur);
                out->width = read_u32_le(data + cur + 8u);
                out->height = read_u32_le(data + cur + 12u);
                out->is_bc7 = (out->format == 12u);
                return true;
            }
            break;
        }
        cur++;
    }
    return false;
}

/* =========================================================================
 * Stateful SceneScript JavaScript Lexer & Sandbox Auditor
 * ========================================================================= */

static const char *const FORBIDDEN_JS_PRIMITIVES[] = {
    "eval",           "Function", "require",   "process",       "window", "document",
    "XMLHttpRequest", "fetch",    "WebSocket", "child_process", NULL};

static inline bool is_js_ident_start(char c)
{
    return isalpha((unsigned char)c) || c == '_' || c == '$';
}

static inline bool is_js_ident_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '$';
}

typedef enum {
    JS_STATE_CODE = 0,
    JS_STATE_STR_SQ,
    JS_STATE_STR_DQ,
    JS_STATE_TEMPLATE,
} js_lex_state_t;

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    uint32_t line;
    js_lex_state_t state;
    uint32_t brace_depth;
    uint32_t tmpl_brace_stack[32];
    uint32_t tmpl_depth;
} js_lexer_t;

typedef enum {
    JS_TOK_EOF = 0,
    JS_TOK_IDENT,
    JS_TOK_NUMBER,
    JS_TOK_PUNCT,
} js_tok_kind_t;

typedef struct {
    js_tok_kind_t kind;
    const char *ptr;
    size_t len;
    uint32_t line;
} js_token_t;

static void js_lexer_init(js_lexer_t *lx, const char *src, size_t len)
{
    memset(lx, 0, sizeof(*lx));
    lx->src = src;
    lx->len = len;
    lx->line = 1u;
    lx->state = JS_STATE_CODE;
}

/* Advances through string literals, template literals (switching back to CODE inside ${...}),
 * comments, and whitespace so '//' inside "http://..." never masks trailing code. */
static bool js_lexer_next_code_token(js_lexer_t *lx, js_token_t *out)
{
    const char *src = lx->src;
    size_t len = lx->len;

    while (lx->pos < len) {
        if (lx->state == JS_STATE_STR_SQ || lx->state == JS_STATE_STR_DQ) {
            char quote = (lx->state == JS_STATE_STR_SQ) ? '\'' : '"';
            while (lx->pos < len) {
                char c = src[lx->pos++];
                if (c == '\\') {
                    if (lx->pos < len) {
                        if (src[lx->pos] == '\n') {
                            lx->line++;
                        }
                        lx->pos++;
                    }
                    continue;
                }
                if (c == '\n') {
                    lx->line++;
                    lx->state = JS_STATE_CODE;
                    break;
                }
                if (c == quote) {
                    lx->state = JS_STATE_CODE;
                    break;
                }
            }
            continue;
        }

        if (lx->state == JS_STATE_TEMPLATE) {
            while (lx->pos < len) {
                char c = src[lx->pos++];
                if (c == '\\') {
                    if (lx->pos < len) {
                        if (src[lx->pos] == '\n') {
                            lx->line++;
                        }
                        lx->pos++;
                    }
                    continue;
                }
                if (c == '\n') {
                    lx->line++;
                    continue;
                }
                if (c == '`') {
                    lx->state = JS_STATE_CODE;
                    break;
                }
                if (c == '$' && lx->pos < len && src[lx->pos] == '{') {
                    lx->pos++;
                    if (lx->tmpl_depth < 32u) {
                        lx->tmpl_brace_stack[lx->tmpl_depth++] = lx->brace_depth;
                    }
                    lx->brace_depth++;
                    lx->state = JS_STATE_CODE;
                    break;
                }
            }
            continue;
        }

        char c = src[lx->pos];
        if (c == '\n') {
            lx->line++;
            lx->pos++;
            continue;
        }
        if (isspace((unsigned char)c)) {
            lx->pos++;
            continue;
        }

        if (c == '/' && lx->pos + 1u < len && src[lx->pos + 1u] == '/') {
            lx->pos += 2u;
            while (lx->pos < len && src[lx->pos] != '\n') {
                lx->pos++;
            }
            continue;
        }

        if (c == '/' && lx->pos + 1u < len && src[lx->pos + 1u] == '*') {
            lx->pos += 2u;
            while (lx->pos < len) {
                if (src[lx->pos] == '*' && lx->pos + 1u < len && src[lx->pos + 1u] == '/') {
                    lx->pos += 2u;
                    break;
                }
                if (src[lx->pos] == '\n') {
                    lx->line++;
                }
                lx->pos++;
            }
            continue;
        }

        if (c == '\'') {
            lx->pos++;
            lx->state = JS_STATE_STR_SQ;
            continue;
        }
        if (c == '"') {
            lx->pos++;
            lx->state = JS_STATE_STR_DQ;
            continue;
        }
        if (c == '`') {
            lx->pos++;
            lx->state = JS_STATE_TEMPLATE;
            continue;
        }

        uint32_t tok_line = lx->line;
        if (is_js_ident_start(c)) {
            size_t start = lx->pos++;
            while (lx->pos < len && is_js_ident_char(src[lx->pos])) {
                lx->pos++;
            }
            out->kind = JS_TOK_IDENT;
            out->ptr = src + start;
            out->len = lx->pos - start;
            out->line = tok_line;
            return true;
        }

        if (isdigit((unsigned char)c)) {
            size_t start = lx->pos++;
            while (lx->pos < len && (isalnum((unsigned char)src[lx->pos]) || src[lx->pos] == '.')) {
                lx->pos++;
            }
            out->kind = JS_TOK_NUMBER;
            out->ptr = src + start;
            out->len = lx->pos - start;
            out->line = tok_line;
            return true;
        }

        lx->pos++;
        if (c == '{') {
            lx->brace_depth++;
        } else if (c == '}') {
            if (lx->brace_depth > 0u) {
                lx->brace_depth--;
            }
            if (lx->tmpl_depth > 0u &&
                lx->brace_depth == lx->tmpl_brace_stack[lx->tmpl_depth - 1u]) {
                lx->tmpl_depth--;
                lx->state = JS_STATE_TEMPLATE;
                continue;
            }
        }

        out->kind = JS_TOK_PUNCT;
        out->ptr = src + (lx->pos - 1u);
        out->len = 1u;
        out->line = tok_line;
        return true;
    }

    out->kind = JS_TOK_EOF;
    out->ptr = NULL;
    out->len = 0;
    out->line = lx->line;
    return false;
}

static const char *find_forbidden_js_primitive(const char *src, size_t len, uint32_t *out_line)
{
    if (!src || len == 0u) {
        return NULL;
    }

    js_lexer_t lx;
    js_lexer_init(&lx, src, len);

    js_token_t tok;
    while (js_lexer_next_code_token(&lx, &tok)) {
        if (tok.kind != JS_TOK_IDENT) {
            continue;
        }
        for (size_t k = 0; FORBIDDEN_JS_PRIMITIVES[k] != NULL; k++) {
            const char *prim = FORBIDDEN_JS_PRIMITIVES[k];
            size_t plen = strlen(prim);
            if (tok.len == plen && memcmp(tok.ptr, prim, plen) == 0) {
                if (out_line) {
                    *out_line = tok.line;
                }
                return prim;
            }
        }
    }
    return NULL;
}

static bool check_potential_infinite_loop(const char *src, size_t len, uint32_t *out_line)
{
    if (!src || len == 0u) {
        return false;
    }

    js_lexer_t lx;
    js_lexer_init(&lx, src, len);

    js_token_t window[5];
    size_t wcount = 0;

    js_token_t tok;
    while (js_lexer_next_code_token(&lx, &tok)) {
        if (wcount < 5u) {
            window[wcount++] = tok;
        } else {
            memmove(&window[0], &window[1], 4u * sizeof(js_token_t));
            window[4] = tok;
        }

        if (wcount >= 4u) {
            const js_token_t *t0 = &window[wcount - 4u];
            const js_token_t *t1 = &window[wcount - 3u];
            const js_token_t *t2 = &window[wcount - 2u];
            const js_token_t *t3 = &window[wcount - 1u];

            if (t0->kind == JS_TOK_IDENT && t0->len == 5u && memcmp(t0->ptr, "while", 5) == 0 &&
                t1->kind == JS_TOK_PUNCT && t1->ptr[0] == '(' && t3->kind == JS_TOK_PUNCT &&
                t3->ptr[0] == ')') {
                bool always_true =
                    (t2->kind == JS_TOK_IDENT && t2->len == 4u &&
                     memcmp(t2->ptr, "true", 4) == 0) ||
                    (t2->kind == JS_TOK_NUMBER && t2->len == 1u && t2->ptr[0] != '0');
                if (always_true) {
                    if (out_line) {
                        *out_line = t0->line;
                    }
                    return true;
                }
            }
        }

        if (wcount == 5u) {
            if (window[0].kind == JS_TOK_IDENT && window[0].len == 3u &&
                memcmp(window[0].ptr, "for", 3) == 0 && window[1].kind == JS_TOK_PUNCT &&
                window[1].ptr[0] == '(' && window[2].kind == JS_TOK_PUNCT &&
                window[2].ptr[0] == ';' && window[3].kind == JS_TOK_PUNCT &&
                window[3].ptr[0] == ';' && window[4].kind == JS_TOK_PUNCT &&
                window[4].ptr[0] == ')') {
                if (out_line) {
                    *out_line = window[0].line;
                }
                return true;
            }
        }
    }
    return false;
}

/* =========================================================================
 * Qualifier-Aware Shader Varying Parser & Swizzle-Safe Repair Engine
 * ========================================================================= */

typedef struct {
    char name[64];
    uint32_t width;
    uint32_t line;
    size_t type_off;
    size_t type_len;
    size_t name_off;
    size_t name_len;
    size_t semi_off;
} we_compat_varying_t;

static void glsl_skip_ws_and_comments(const char *src, size_t len, size_t *pos, uint32_t *line,
                                      bool skip_pp_lines)
{
    while (*pos < len) {
        char c = src[*pos];
        if (c == '\n') {
            (*line)++;
            (*pos)++;
            continue;
        }
        if (isspace((unsigned char)c)) {
            (*pos)++;
            continue;
        }
        if (c == '/' && *pos + 1u < len && src[*pos + 1u] == '/') {
            *pos += 2u;
            while (*pos < len && src[*pos] != '\n') {
                (*pos)++;
            }
            continue;
        }
        if (c == '/' && *pos + 1u < len && src[*pos + 1u] == '*') {
            *pos += 2u;
            while (*pos < len) {
                if (src[*pos] == '*' && *pos + 1u < len && src[*pos + 1u] == '/') {
                    *pos += 2u;
                    break;
                }
                if (src[*pos] == '\n') {
                    (*line)++;
                }
                (*pos)++;
            }
            continue;
        }
        if (skip_pp_lines && c == '#') {
            bool at_bol = (*pos == 0u);
            if (!at_bol) {
                size_t back = *pos;
                at_bol = true;
                while (back > 0u && src[back - 1u] != '\n') {
                    back--;
                    if (!isspace((unsigned char)src[back])) {
                        at_bol = false;
                        break;
                    }
                }
            }
            if (at_bol) {
                while (*pos < len && src[*pos] != '\n') {
                    if (src[*pos] == '\\' && *pos + 1u < len && src[*pos + 1u] == '\n') {
                        (*line)++;
                        *pos += 2u;
                        continue;
                    }
                    (*pos)++;
                }
                continue;
            }
        }
        break;
    }
}

static bool glsl_tok_eq(const char *ptr, size_t len, const char *kw)
{
    size_t klen = strlen(kw);
    return len == klen && memcmp(ptr, kw, klen) == 0;
}

static uint32_t parse_varyings_from_stage(const char *src, size_t len,
                                          we_compat_varying_t *out_vars, uint32_t max_vars,
                                          bool is_vertex_stage)
{
    if (!src || len == 0u || !out_vars || max_vars == 0u) {
        return 0;
    }

    uint32_t count = 0;
    size_t pos = 0;
    uint32_t line = 1u;
    uint32_t brace_depth = 0;
    bool stmt_start = true;

    while (pos < len && count < max_vars) {
        glsl_skip_ws_and_comments(src, len, &pos, &line, true);
        if (pos >= len) {
            break;
        }

        if (brace_depth > 0u || !stmt_start) {
            char c = src[pos++];
            if (c == '{') {
                brace_depth++;
            } else if (c == '}') {
                if (brace_depth > 0u) {
                    brace_depth--;
                }
                if (brace_depth == 0u) {
                    stmt_start = true;
                }
            } else if (c == ';' && brace_depth == 0u) {
                stmt_start = true;
            }
            continue;
        }

        /* At top-level statement start: parse optional qualifiers before type */
        bool has_stage_storage = false;
        uint32_t decl_line = line;

        for (;;) {
            glsl_skip_ws_and_comments(src, len, &pos, &line, true);
            if (pos >= len || !(isalpha((unsigned char)src[pos]) || src[pos] == '_')) {
                break;
            }

            size_t tok_start = pos;
            uint32_t tok_line = line;
            while (pos < len && (isalnum((unsigned char)src[pos]) || src[pos] == '_')) {
                pos++;
            }
            size_t tok_len = pos - tok_start;
            const char *tok = src + tok_start;

            if (glsl_tok_eq(tok, tok_len, "layout")) {
                glsl_skip_ws_and_comments(src, len, &pos, &line, false);
                if (pos < len && src[pos] == '(') {
                    uint32_t pdepth = 1u;
                    pos++;
                    while (pos < len && pdepth > 0u) {
                        if (src[pos] == '\n') {
                            line++;
                        } else if (src[pos] == '(') {
                            pdepth++;
                        } else if (src[pos] == ')') {
                            pdepth--;
                        }
                        pos++;
                    }
                }
                continue;
            }

            if (glsl_tok_eq(tok, tok_len, "invariant") || glsl_tok_eq(tok, tok_len, "precise") ||
                glsl_tok_eq(tok, tok_len, "flat") || glsl_tok_eq(tok, tok_len, "smooth") ||
                glsl_tok_eq(tok, tok_len, "noperspective") ||
                glsl_tok_eq(tok, tok_len, "centroid") || glsl_tok_eq(tok, tok_len, "sample") ||
                glsl_tok_eq(tok, tok_len, "lowp") || glsl_tok_eq(tok, tok_len, "mediump") ||
                glsl_tok_eq(tok, tok_len, "highp")) {
                continue;
            }

            if (glsl_tok_eq(tok, tok_len, "varying")) {
                has_stage_storage = true;
                decl_line = tok_line;
                continue;
            }
            if (is_vertex_stage && glsl_tok_eq(tok, tok_len, "out")) {
                has_stage_storage = true;
                decl_line = tok_line;
                continue;
            }
            if (!is_vertex_stage && glsl_tok_eq(tok, tok_len, "in")) {
                has_stage_storage = true;
                decl_line = tok_line;
                continue;
            }

            /* Reached non-qualifier token (type or identifier) */
            if (has_stage_storage) {
                uint32_t width = 0;
                if (glsl_tok_eq(tok, tok_len, "float")) {
                    width = 1u;
                } else if (glsl_tok_eq(tok, tok_len, "vec2")) {
                    width = 2u;
                } else if (glsl_tok_eq(tok, tok_len, "vec3")) {
                    width = 3u;
                } else if (glsl_tok_eq(tok, tok_len, "vec4")) {
                    width = 4u;
                }

                if (width > 0u) {
                    glsl_skip_ws_and_comments(src, len, &pos, &line, false);
                    if (pos < len && (isalpha((unsigned char)src[pos]) || src[pos] == '_')) {
                        size_t name_off = pos;
                        while (pos < len && (isalnum((unsigned char)src[pos]) || src[pos] == '_')) {
                            pos++;
                        }
                        size_t name_len = pos - name_off;
                        glsl_skip_ws_and_comments(src, len, &pos, &line, false);
                        if (name_len > 0u && name_len < sizeof(out_vars[0].name) && pos < len &&
                            src[pos] == ';') {
                            we_compat_varying_t *dst = &out_vars[count++];
                            memcpy(dst->name, src + name_off, name_len);
                            dst->name[name_len] = '\0';
                            dst->width = width;
                            dst->line = decl_line;
                            dst->type_off = tok_start;
                            dst->type_len = tok_len;
                            dst->name_off = name_off;
                            dst->name_len = name_len;
                            dst->semi_off = pos;
                            pos++;
                            stmt_start = true;
                            break;
                        }
                    }
                }
            }

            stmt_start = false;
            break;
        }

        if (pos < len && stmt_start && !(isalpha((unsigned char)src[pos]) || src[pos] == '_')) {
            char c = src[pos++];
            if (c == '{') {
                brace_depth++;
                stmt_start = false;
            } else if (c == '}') {
                if (brace_depth > 0u) {
                    brace_depth--;
                }
            } else if (c != ';') {
                stmt_start = false;
            }
        }
    }

    return count;
}

/* =========================================================================
 * Unified Package & Loose Filesystem VFS Scanner
 * ========================================================================= */

typedef struct {
    char workshop_dir[4096];
    we_pkg_t *pkg;
    uint8_t *demasked_buf;
    size_t demasked_size;
} we_compat_vfs_t;

static void compat_vfs_close(we_compat_vfs_t *vfs)
{
    if (!vfs) {
        return;
    }
    if (vfs->pkg) {
        we_pkg_close(vfs->pkg);
        vfs->pkg = NULL;
    }
    if (vfs->demasked_buf) {
        free(vfs->demasked_buf);
        vfs->demasked_buf = NULL;
        vfs->demasked_size = 0;
    }
}

static bool compat_vfs_open(we_compat_vfs_t *vfs, const char *path, we_compat_report_t *report)
{
    memset(vfs, 0, sizeof(*vfs));
    if (!path || path[0] == '\0') {
        return false;
    }
    snprintf(vfs->workshop_dir, sizeof(vfs->workshop_dir), "%s", path);

    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }

    char candidate_pkg[4096] = {0};
    if (S_ISREG(st.st_mode)) {
        snprintf(candidate_pkg, sizeof(candidate_pkg), "%s", path);
    } else if (S_ISDIR(st.st_mode)) {
        char p1[4096], p2[4096];
        snprintf(p1, sizeof(p1), "%s/scene.pkg", path);
        snprintf(p2, sizeof(p2), "%s/gifscene.pkg", path);
        if (access(p1, R_OK) == 0) {
            snprintf(candidate_pkg, sizeof(candidate_pkg), "%s", p1);
        } else if (access(p2, R_OK) == 0) {
            snprintf(candidate_pkg, sizeof(candidate_pkg), "%s", p2);
        } else {
            DIR *d = opendir(path);
            if (d) {
                struct dirent *ent;
                while ((ent = readdir(d)) != NULL) {
                    if (ent->d_type == DT_REG || ent->d_type == DT_UNKNOWN) {
                        size_t nlen = strlen(ent->d_name);
                        if (has_ext_ci(ent->d_name, nlen, ".pkg")) {
                            snprintf(candidate_pkg, sizeof(candidate_pkg), "%s/%s", path,
                                     ent->d_name);
                            break;
                        }
                    }
                }
                closedir(d);
            }
        }
    }

    if (candidate_pkg[0] != '\0') {
        int fd = open(candidate_pkg, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            struct stat pkg_st;
            if (fstat(fd, &pkg_st) == 0 && S_ISREG(pkg_st.st_mode) && pkg_st.st_size >= 16) {
                size_t sz = (size_t)pkg_st.st_size;
                void *mapped = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
                if (mapped != MAP_FAILED) {
                    uint8_t key[16] = {0};
                    int period = detect_rolling_xor_key((const uint8_t *)mapped, sz, key);
                    if (period > 0) {
                        vfs->demasked_buf = (uint8_t *)malloc(sz);
                        if (vfs->demasked_buf) {
                            vfs->demasked_size = sz;
                            xor_demask_buffer((const uint8_t *)mapped, sz, key, (size_t)period,
                                              vfs->demasked_buf);
                            vfs->pkg = we_pkg_open_mem(vfs->demasked_buf, sz, vfs->workshop_dir);
                            char fix[256];
                            snprintf(fix, sizeof(fix),
                                     "De-masked obfuscated container with %d-byte key", period);
                            report_add_issue(report, "PKG_OBFUSCATION_DEMASKED", WE_ISSUE_SEV_INFO,
                                             "Rolling XOR de-masking applied to container",
                                             candidate_pkg, 1, fix);
                        }
                    }
                    munmap(mapped, sz);
                }
            }
            close(fd);

            if (!vfs->pkg) {
                vfs->pkg = we_pkg_open_file(candidate_pkg, vfs->workshop_dir);
            }
        }
    }

    return true;
}

static const uint8_t *compat_vfs_read_file(we_compat_vfs_t *vfs, const char *rel_path,
                                           size_t *out_size, bool *out_must_free)
{
    if (out_must_free) {
        *out_must_free = false;
    }
    if (out_size) {
        *out_size = 0;
    }
    if (!vfs || !rel_path) {
        return NULL;
    }

    char norm[1024];
    if (!we_pkg_normalize_path(rel_path, strlen(rel_path), norm, sizeof(norm))) {
        return NULL;
    }

    if (vfs->pkg) {
        const uint8_t *data = we_pkg_find(vfs->pkg, norm, out_size);
        if (data) {
            return data;
        }
    }

    char full_path[8192];
    snprintf(full_path, sizeof(full_path), "%s/%s", vfs->workshop_dir, norm);
    int fd = open(full_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
            st.st_size <= (256LL * 1024LL * 1024LL)) {
            size_t sz = (size_t)st.st_size;
            uint8_t *buf = (uint8_t *)malloc(sz + 1u);
            if (buf) {
                size_t total = 0u;
                bool io_err = false;
                while (total < sz) {
                    ssize_t rd = read(fd, buf + total, sz - total);
                    if (rd < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        io_err = true;
                        break;
                    }
                    if (rd == 0) {
                        break;
                    }
                    total += (size_t)rd;
                }
                close(fd);
                if (!io_err && total == sz) {
                    buf[sz] = '\0';
                    if (out_size) {
                        *out_size = sz;
                    }
                    if (out_must_free) {
                        *out_must_free = true;
                    }
                    return buf;
                }
                free(buf);
                return NULL;
            }
        }
        close(fd);
    }

    return NULL;
}

/* =========================================================================
 * Full Directory Recursion for Loose Executable Malware & Traversal Check
 * ========================================================================= */

static bool scan_loose_directory_malware(const char *dir_path, we_compat_report_t *report,
                                         int depth)
{
    if (!dir_path || depth > 16) {
        return false;
    }
    DIR *d = opendir(dir_path);
    if (!d) {
        return false;
    }

    bool malware_found = false;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (has_directory_traversal(ent->d_name)) {
            report_add_issue(report, "SECURITY_TRAVERSAL", WE_ISSUE_SEV_FATAL,
                             "Directory traversal detected in filename", ent->d_name, 1,
                             "Sanitize file path");
            report->tier = WE_TIER_4_BLOCKED;
            report->compatibility_score = 0;
            malware_found = true;
            break;
        }

        char subpath[4096];
        snprintf(subpath, sizeof(subpath), "%s/%s", dir_path, ent->d_name);

        struct stat st;
        if (lstat(subpath, &st) != 0) {
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (scan_loose_directory_malware(subpath, report, depth + 1)) {
                malware_found = true;
                break;
            }
        } else if (S_ISREG(st.st_mode)) {
            int fd = open(subpath, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (fd >= 0) {
                uint8_t hdr[16];
                size_t total = 0u;
                while (total < sizeof(hdr)) {
                    ssize_t r = read(fd, hdr + total, sizeof(hdr) - total);
                    if (r < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        break;
                    }
                    if (r == 0) {
                        break;
                    }
                    total += (size_t)r;
                }
                close(fd);
                if (total >= 2u && is_executable_binary_payload(hdr, total, ent->d_name)) {
                    report_add_issue(report, "SECURITY_ALERT_MALICIOUS", WE_ISSUE_SEV_FATAL,
                                     "Malicious executable binary detected (PE/ELF header)",
                                     subpath, 1, "Quarantine package and remove executable");
                    report->tier = WE_TIER_4_BLOCKED;
                    report->compatibility_score = 0;
                    malware_found = true;
                    break;
                }
            }
        }
    }

    closedir(d);
    return malware_found;
}

/* =========================================================================
 * Asset Auditing Traversal Helpers
 * ========================================================================= */

typedef struct {
    bool cycle_detected;
    bool custom_shaders_present;
    bool varying_promoted;
    bool shader_dxbc_blocked;
    bool shader_stage_blocked;
    bool puppet_clamped;
    bool puppet_exceeded;
    bool puppet_blocked;
    bool particle_overflow;
    bool particle_advanced;
    bool audio_reactive;
    bool script_violation;
    bool script_loop;
    bool bc7_fallback;
    bool missing_user_prop;
    bool model3d_degraded;
} we_compat_audit_flags_t;

static void audit_single_asset(we_compat_vfs_t *vfs, const char *rel_path, const uint8_t *data,
                               size_t length, we_compat_scanner_t *scanner,
                               we_compat_report_t *out_report, we_compat_audit_flags_t *flags)
{
    if (!rel_path || !data || length == 0u) {
        return;
    }
    size_t plen = strlen(rel_path);

    if (has_ext_ci(rel_path, plen, ".vert") || has_ext_ci(rel_path, plen, ".frag") ||
        has_ext_ci(rel_path, plen, ".hlsl") || has_ext_ci(rel_path, plen, ".glsl")) {
        flags->custom_shaders_present = true;

        if (length >= 4u && memcmp(data, "DXBC", 4) == 0) {
            flags->shader_dxbc_blocked = true;
            report_add_issue(out_report, "SHADER_DXBC_BYTECODE", WE_ISSUE_SEV_FATAL,
                             "Compiled DirectX bytecode (DXBC) cannot run on GLES", rel_path, 1,
                             "Provide GLSL/HLSL source shader");
        }

        const char *src = (const char *)data;
        size_t slen = length;

        if (slice_contains(src, slen, "#pragma geometry") ||
            slice_contains(src, slen, "layout(triangles) in") ||
            slice_contains(src, slen, "#pragma tessellation")) {
            flags->shader_stage_blocked = true;
            report_add_issue(out_report, "SHADER_UNSUPPORTED_STAGE", WE_ISSUE_SEV_FATAL,
                             "Geometry or tessellation shader stages are unsupported", rel_path, 1,
                             "Remove geometry shader stage");
        }

        if (slice_contains(src, slen, "g_AudioSpectrum16") ||
            slice_contains(src, slen, "g_AudioSpectrum64") ||
            slice_contains(src, slen, "g_AudioSpectrum32")) {
            flags->audio_reactive = true;
        }

        if (has_ext_ci(rel_path, plen, ".vert")) {
            char frag_path[1024];
            snprintf(frag_path, sizeof(frag_path), "%.*s.frag", (int)(plen - 5u), rel_path);
            size_t frag_sz = 0;
            bool frag_free = false;
            const uint8_t *frag_data = compat_vfs_read_file(vfs, frag_path, &frag_sz, &frag_free);
            if (frag_data && frag_sz > 0u) {
                we_compat_varying_t v_vars[32], f_vars[32];
                uint32_t v_cnt = parse_varyings_from_stage(src, slen, v_vars, 32u, true);
                uint32_t f_cnt =
                    parse_varyings_from_stage((const char *)frag_data, frag_sz, f_vars, 32u, false);
                for (uint32_t vi = 0; vi < v_cnt; vi++) {
                    for (uint32_t fi = 0; fi < f_cnt; fi++) {
                        if (strcmp(v_vars[vi].name, f_vars[fi].name) == 0 &&
                            v_vars[vi].width != f_vars[fi].width) {
                            flags->varying_promoted = true;
                            char msg[256];
                            snprintf(
                                msg, sizeof(msg),
                                "Varying '%s' width mismatch between .vert (%u) and .frag (%u)",
                                v_vars[vi].name, v_vars[vi].width, f_vars[fi].width);
                            report_add_issue(out_report, "SHADER_VARYING_MISMATCH",
                                             WE_ISSUE_SEV_WARNING, msg, rel_path, v_vars[vi].line,
                                             "Promote varying interface to swizzle-safe vec4");
                            report_add_repair(out_report, "REPAIR_PROMOTE_VARYING", rel_path,
                                              "Promote varying interface to swizzle-safe vec4");
                        }
                    }
                }
            }
            if (frag_free) {
                free((void *)frag_data);
            }
        }
    }

    if (has_ext_ci(rel_path, plen, ".mdl")) {
        we_compat_mdl_info_t mdl;
        if (parse_mdl_header_fast(data, length, &mdl)) {
            if (mdl.bone_count > 256u) {
                flags->puppet_blocked = true;
                report_add_issue(out_report, "PUPPET_BONES_BLOCKED", WE_ISSUE_SEV_ERROR,
                                 "Puppet mesh bone count exceeds hardware ceiling of 256", rel_path,
                                 1, "Reduce skeleton bones to <= 128");
            } else if (mdl.bone_count > 128u) {
                flags->puppet_clamped = true;
                report_add_issue(out_report, "PUPPET_BONES_CLAMPED", WE_ISSUE_SEV_WARNING,
                                 "Puppet mesh bone count exceeds 128 (requires clamping)", rel_path,
                                 1, "Clamp bone count to 128");
                report_add_repair(out_report, "REPAIR_CLAMP_BONES", rel_path,
                                  "Clamp puppet bone count to 128");
            }

            if (mdl.vertex_count > 65536u) {
                flags->puppet_exceeded = true;
                report_add_issue(out_report, "PUPPET_VERTICES_EXCEEDED", WE_ISSUE_SEV_WARNING,
                                 "Puppet mesh vertex count exceeds 65536 limit", rel_path, 1,
                                 "Simplify mesh geometry");
            }
            if (mdl.triangle_count > 131072u) {
                flags->puppet_exceeded = true;
                report_add_issue(out_report, "PUPPET_TRIANGLES_EXCEEDED", WE_ISSUE_SEV_WARNING,
                                 "Puppet mesh triangle count exceeds 131072 limit", rel_path, 1,
                                 "Simplify mesh geometry");
            }
        }
    }

    if ((strstr(rel_path, "particles") || strstr(rel_path, "particle")) &&
        has_ext_ci(rel_path, plen, ".json")) {
        we_jval_t *p_json = we_json_parse((const char *)data, length);
        if (p_json) {
            out_report->summary.particle_systems++;
            const we_jval_t *mc = we_jobj_get(p_json, "maxcount");
            if (!mc) {
                mc = we_jobj_get(p_json, "count");
            }
            if (!mc) {
                const we_jval_t *sub = we_jobj_get(p_json, "particle");
                if (sub && sub->type == JVAL_OBJ) {
                    mc = we_jobj_get(sub, "maxcount");
                    if (!mc) {
                        mc = we_jobj_get(sub, "count");
                    }
                }
            }
            if (mc && mc->type == JVAL_NUM) {
                uint32_t cnt = (uint32_t)mc->num;
                if (cnt > 16384u) {
                    flags->particle_overflow = true;
                    report_add_issue(out_report, "PARTICLE_OVERFLOW", WE_ISSUE_SEV_WARNING,
                                     "Particle emitter count exceeds 16384 limit", rel_path, 1,
                                     "Clamp particle max count to 16384");
                    report_add_repair(out_report, "REPAIR_CLAMP_PARTICLES", rel_path,
                                      "Clamp particle max count to 16384");
                } else if (cnt > 4096u) {
                    flags->particle_advanced = true;
                }
            }
            we_jval_free(p_json);
        }
    }

    if (has_ext_ci(rel_path, plen, ".js")) {
        uint32_t bad_line = 1u;
        const char *bad_prim = find_forbidden_js_primitive((const char *)data, length, &bad_line);
        if (bad_prim) {
            flags->script_violation = true;
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Script attempts to access forbidden global or security API: '%s'", bad_prim);
            report_add_issue(out_report, "SECURITY_SCRIPT_VIOLATION", WE_ISSUE_SEV_FATAL, msg,
                             rel_path, bad_line, "Remove forbidden API call from SceneScript");
        }

        uint32_t loop_line = 1u;
        if (check_potential_infinite_loop((const char *)data, length, &loop_line)) {
            flags->script_loop = true;
            report_add_issue(out_report, "SCRIPT_STEP_BUDGET", WE_ISSUE_SEV_WARNING,
                             "Script contains potential infinite loop exceeding 100k step budget",
                             rel_path, loop_line,
                             "Replace script with safe identity watchdog stub");
            report_add_repair(out_report, "REPAIR_STUB_SCRIPT", rel_path,
                              "Replace errant script with static identity watchdog stub");
        }
    }

    if (has_ext_ci(rel_path, plen, ".tex")) {
        we_compat_tex_info_t tex_info;
        if (parse_tex_header_fast(data, length, &tex_info)) {
            if (tex_info.is_bc7 && !scanner->supports_bc7) {
                flags->bc7_fallback = true;
                report_add_issue(out_report, "BC7_TEXTURE_UNSUPPORTED", WE_ISSUE_SEV_WARNING,
                                 "BC7 texture format requires GPU support or software fallback",
                                 rel_path, 1, "Enable software BC7 fallback decoding");
                report_add_repair(out_report, "REPAIR_BC7_SOFTWARE_FALLBACK", rel_path,
                                  "Tag BC7 texture for CPU software decompression fallback");
            }
        }
    }
}

static void scan_loose_assets_recursive(we_compat_vfs_t *vfs, const char *dir_path,
                                        const char *rel_base, int depth,
                                        we_compat_scanner_t *scanner,
                                        we_compat_report_t *out_report,
                                        we_compat_audit_flags_t *flags)
{
    if (!dir_path || depth > 16) {
        return;
    }
    DIR *d = opendir(dir_path);
    if (!d) {
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0 ||
            strcmp(ent->d_name, ".waywal_compat.json") == 0) {
            continue;
        }

        char subpath[4096];
        snprintf(subpath, sizeof(subpath), "%s/%s", dir_path, ent->d_name);
        char rel_path[1024];
        if (rel_base[0] != '\0') {
            snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_base, ent->d_name);
        } else {
            snprintf(rel_path, sizeof(rel_path), "%s", ent->d_name);
        }

        struct stat st;
        if (lstat(subpath, &st) != 0 || S_ISLNK(st.st_mode)) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            scan_loose_assets_recursive(vfs, subpath, rel_path, depth + 1, scanner, out_report,
                                        flags);
        } else if (S_ISREG(st.st_mode)) {
            size_t plen = strlen(ent->d_name);
            if (has_ext_ci(ent->d_name, plen, ".js") || has_ext_ci(ent->d_name, plen, ".mdl") ||
                has_ext_ci(ent->d_name, plen, ".tex") || has_ext_ci(ent->d_name, plen, ".vert") ||
                has_ext_ci(ent->d_name, plen, ".frag") || has_ext_ci(ent->d_name, plen, ".hlsl") ||
                has_ext_ci(ent->d_name, plen, ".glsl") || has_ext_ci(ent->d_name, plen, ".json")) {
                int fd = open(subpath, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
                if (fd >= 0) {
                    size_t sz = (size_t)st.st_size;
                    if (sz > 0u && sz <= (64ULL * 1024ULL * 1024ULL)) {
                        uint8_t *buf = (uint8_t *)malloc(sz + 1u);
                        if (buf) {
                            size_t total = 0u;
                            bool io_err = false;
                            while (total < sz) {
                                ssize_t rd = read(fd, buf + total, sz - total);
                                if (rd < 0) {
                                    if (errno == EINTR) {
                                        continue;
                                    }
                                    io_err = true;
                                    break;
                                }
                                if (rd == 0) {
                                    break;
                                }
                                total += (size_t)rd;
                            }
                            if (!io_err && total == sz) {
                                buf[sz] = '\0';
                                audit_single_asset(vfs, rel_path, buf, sz, scanner, out_report,
                                                   flags);
                            }
                            free(buf);
                        }
                    }
                    close(fd);
                }
            }
        }
    }
    closedir(d);
}

static bool is_valid_prop_ident(const char *s)
{
    if (!s || s[0] == '\0') {
        return false;
    }
    size_t len = strlen(s);
    if (len >= 64u) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(isalnum(c) || c == '_')) {
            return false;
        }
    }
    return true;
}

static void record_missing_user_prop(const char *key, const we_project_t *proj,
                                     we_compat_report_t *out_report, we_compat_audit_flags_t *flags)
{
    if (!is_valid_prop_ident(key)) {
        return;
    }
    for (uint32_t pi = 0; pi < proj->prop_count; pi++) {
        if (strcmp(proj->props[pi].key, key) == 0) {
            return;
        }
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "Referenced user property '%s' is missing in project.json", key);
    for (uint32_t i = 0; i < out_report->issue_count; i++) {
        if (strcmp(out_report->issues[i].code, "MISSING_USER_PROPERTY") == 0 &&
            strcmp(out_report->issues[i].message, msg) == 0) {
            return;
        }
    }

    flags->missing_user_prop = true;
    report_add_issue(out_report, "MISSING_USER_PROPERTY", WE_ISSUE_SEV_WARNING, msg, "project.json",
                     1, "Synthesize default user property in project.json");
    report_add_repair(out_report, "REPAIR_SYNTHESIZE_USER_PROP", "project.json",
                      "Synthesize missing user property with default value");
}

static void collect_scene_user_props_ast(const we_jval_t *node, const we_project_t *proj,
                                         we_compat_report_t *out_report,
                                         we_compat_audit_flags_t *flags, int depth)
{
    if (!node || depth > 48) {
        return;
    }
    if (node->type == JVAL_ARR) {
        for (size_t i = 0; i < node->arr.count; i++) {
            collect_scene_user_props_ast(node->arr.items[i], proj, out_report, flags, depth + 1);
        }
    } else if (node->type == JVAL_OBJ) {
        for (size_t i = 0; i < node->obj.count; i++) {
            const char *k = node->obj.entries[i].key;
            const we_jval_t *v = node->obj.entries[i].val;
            if (k && v && (strcmp(k, "user") == 0 || strcmp(k, "userProperty") == 0)) {
                if (v->type == JVAL_STR && v->str) {
                    record_missing_user_prop(v->str, proj, out_report, flags);
                } else if (v->type == JVAL_OBJ) {
                    const we_jval_t *name_v = we_jobj_get(v, "name");
                    if (name_v && name_v->type == JVAL_STR && name_v->str) {
                        record_missing_user_prop(name_v->str, proj, out_report, flags);
                    }
                }
            }
            collect_scene_user_props_ast(v, proj, out_report, flags, depth + 1);
        }
    }
}

/* Bounded slice scan fallback when user property bindings live inside embedded strings */
static void collect_scene_user_props_slice(const char *scene_data, size_t scene_sz,
                                           const we_project_t *proj, we_compat_report_t *out_report,
                                           we_compat_audit_flags_t *flags)
{
    static const char *const PATS[2] = {"\"user\"", "\"userProperty\""};
    for (size_t p = 0; p < 2u; p++) {
        const char *pat = PATS[p];
        size_t plen = strlen(pat);
        const char *cur = scene_data;
        const char *end = scene_data + scene_sz;

        while (cur < end && (size_t)(end - cur) >= plen) {
            const char *hit = (const char *)memmem(cur, (size_t)(end - cur), pat, plen);
            if (!hit) {
                break;
            }
            const char *scan = hit + plen;
            while (scan < end && isspace((unsigned char)*scan)) {
                scan++;
            }
            if (scan >= end || *scan != ':') {
                cur = hit + plen;
                continue;
            }
            scan++;
            while (scan < end && isspace((unsigned char)*scan)) {
                scan++;
            }
            if (scan < end && *scan == '"') {
                const char *vstart = scan + 1;
                const char *vend = (const char *)memchr(vstart, '"', (size_t)(end - vstart));
                if (!vend) {
                    break;
                }
                size_t klen = (size_t)(vend - vstart);
                if (klen > 0u && klen < 64u) {
                    char key[64];
                    memcpy(key, vstart, klen);
                    key[klen] = '\0';
                    record_missing_user_prop(key, proj, out_report, flags);
                }
                cur = vend + 1;
            } else {
                cur = scan;
            }
        }
    }
}

/* =========================================================================
 * Main Scanner Entrypoint: we_compat_scan_directory
 * ========================================================================= */

bool we_compat_scan_directory(we_compat_scanner_t *scanner, const char *workshop_dir,
                              we_compat_report_t *out_report)
{
    if (!scanner || !workshop_dir || !out_report) {
        return false;
    }

    memset(out_report, 0, sizeof(*out_report));
    out_report->scan_timestamp = (uint64_t)time(NULL);

    const char *last_slash = strrchr(workshop_dir, '/');
    const char *id_cand = last_slash ? last_slash + 1 : workshop_dir;
    if (id_cand[0] != '\0') {
        snprintf(out_report->workshop_id, sizeof(out_report->workshop_id), "%s", id_cand);
    }

    we_compat_vfs_t vfs;
    if (!compat_vfs_open(&vfs, workshop_dir, out_report)) {
        report_add_issue(out_report, "DIRECTORY_NOT_FOUND", WE_ISSUE_SEV_FATAL,
                         "Workshop directory or package file cannot be accessed", workshop_dir, 1,
                         "Verify path existence");
        out_report->tier = WE_TIER_4_BLOCKED;
        out_report->compatibility_score = 0;
        return true;
    }

    if (vfs.pkg && vfs.pkg->entries) {
        for (uint32_t i = 0; i < vfs.pkg->entry_count; i++) {
            const we_pkg_entry_t *ent = &vfs.pkg->entries[i];
            if (has_directory_traversal(ent->path)) {
                report_add_issue(out_report, "SECURITY_TRAVERSAL", WE_ISSUE_SEV_FATAL,
                                 "Directory traversal or invalid character detected in entry path",
                                 ent->path, 1, "Reject malformed package");
                out_report->tier = WE_TIER_4_BLOCKED;
                out_report->compatibility_score = 0;
                compat_vfs_close(&vfs);
                return true;
            }

            if (is_executable_binary_payload(ent->data, ent->length, ent->path)) {
                report_add_issue(out_report, "SECURITY_ALERT_MALICIOUS", WE_ISSUE_SEV_FATAL,
                                 "Malicious executable binary detected (PE/ELF header) in package",
                                 ent->path, 1, "Quarantine package and remove executable binary");
                out_report->tier = WE_TIER_4_BLOCKED;
                out_report->compatibility_score = 0;
                compat_vfs_close(&vfs);
                return true;
            }
        }
    }

    struct stat root_st;
    if (stat(workshop_dir, &root_st) == 0 && S_ISDIR(root_st.st_mode)) {
        if (scan_loose_directory_malware(workshop_dir, out_report, 0)) {
            compat_vfs_close(&vfs);
            return true;
        }
    }

    size_t proj_sz = 0;
    bool proj_free = false;
    const uint8_t *proj_data = compat_vfs_read_file(&vfs, "project.json", &proj_sz, &proj_free);
    we_project_t proj;
    memset(&proj, 0, sizeof(proj));
    bool has_project = false;

    if (proj_data && proj_sz > 0u) {
        has_project = we_project_parse((const char *)proj_data, proj_sz, &proj);
        if (has_project) {
            snprintf(out_report->title, sizeof(out_report->title), "%s", proj.title);
            if (proj.workshop_id[0] != '\0') {
                snprintf(out_report->workshop_id, sizeof(out_report->workshop_id), "%s",
                         proj.workshop_id);
            }

            if (strcasecmp(proj.type, "application") == 0) {
                report_add_issue(out_report, "SECURITY_ALERT_MALICIOUS", WE_ISSUE_SEV_FATAL,
                                 "Application wallpapers (.exe) are untrusted and blocked",
                                 "project.json", 1, "Quarantine package");
                out_report->tier = WE_TIER_4_BLOCKED;
                out_report->compatibility_score = 0;
                if (proj_free) {
                    free((void *)proj_data);
                }
                compat_vfs_close(&vfs);
                return true;
            }
            if (strcasecmp(proj.type, "web") == 0) {
                report_add_issue(
                    out_report, "CEF_WEB_WALLPAPER", WE_ISSUE_SEV_FATAL,
                    "CEF HTML5 web wallpapers are disabled for security and resource bounds",
                    "project.json", 1, "Provide native scene wallpaper");
                out_report->tier = WE_TIER_4_BLOCKED;
                out_report->compatibility_score = 0;
                if (proj_free) {
                    free((void *)proj_data);
                }
                compat_vfs_close(&vfs);
                return true;
            }
            if (strcasecmp(proj.type, "video") == 0) {
                out_report->tier = WE_TIER_1_NATIVE;
                out_report->compatibility_score = 100;
                out_report->summary.total_objects = 1;
                out_report->summary.sprite_layers = 1;
                out_report->summary.estimated_vram_mb = 48.0f;
                out_report->summary.estimated_fps = 60;
                if (proj_free) {
                    free((void *)proj_data);
                }
                compat_vfs_close(&vfs);
                return true;
            }
        }
        if (proj_free) {
            free((void *)proj_data);
        }
    }

    const char *scene_filename = (has_project && proj.file[0] != '\0') ? proj.file : "scene.json";
    size_t scene_sz = 0;
    bool scene_free = false;
    const uint8_t *scene_data = compat_vfs_read_file(&vfs, scene_filename, &scene_sz, &scene_free);
    if (!scene_data) {
        scene_data = compat_vfs_read_file(&vfs, "scene.json", &scene_sz, &scene_free);
    }

    if (!scene_data) {
        report_add_issue(out_report, "SCENE_FILE_MISSING", WE_ISSUE_SEV_FATAL,
                         "Neither project.json file target nor scene.json was found",
                         scene_filename, 1, "Verify wallpaper package contents");
        out_report->tier = WE_TIER_4_BLOCKED;
        out_report->compatibility_score = 0;
        compat_vfs_close(&vfs);
        return true;
    }

    we_jval_t *scene_root = we_json_parse((const char *)scene_data, scene_sz);
    if (!scene_root) {
        report_add_issue(out_report, "SCENE_JSON_SYNTAX_ERROR", WE_ISSUE_SEV_ERROR,
                         "Failed to parse scene.json DOM AST", scene_filename, 1,
                         "Fix JSON syntax in scene.json");
        out_report->tier = WE_TIER_3_DEGRADED;
        out_report->compatibility_score = 30;
        if (scene_free) {
            free((void *)scene_data);
        }
        compat_vfs_close(&vfs);
        return true;
    }

    we_compat_audit_flags_t flags;
    memset(&flags, 0, sizeof(flags));

    const we_jval_t *objs = we_jobj_get(scene_root, "objects");
    if (objs && objs->type == JVAL_ARR) {
        out_report->summary.total_objects = (uint32_t)objs->arr.count;

        typedef struct {
            int32_t id;
            int32_t parent;
        } obj_node_t;

        size_t n_objs = objs->arr.count;
        obj_node_t *nodes = n_objs > 0u ? (obj_node_t *)calloc(n_objs, sizeof(obj_node_t)) : NULL;

        for (size_t i = 0; i < n_objs; i++) {
            const we_jval_t *obj = objs->arr.items[i];
            if (!obj || obj->type != JVAL_OBJ) {
                continue;
            }

            const we_jval_t *id_val = we_jobj_get(obj, "id");
            int32_t id = (id_val && id_val->type == JVAL_NUM) ? (int32_t)id_val->num : (int32_t)i;
            const we_jval_t *par_val = we_jobj_get(obj, "parent");
            int32_t parent = (par_val && par_val->type == JVAL_NUM) ? (int32_t)par_val->num : -1;

            if (nodes) {
                nodes[i].id = id;
                nodes[i].parent = parent;
            }

            const we_jval_t *type_val = we_jobj_get(obj, "type");
            const char *type_str = we_jval_str(type_val, "sprite");

            if (strcasecmp(type_str, "puppet") == 0 || strcasecmp(type_str, "puppetwarp") == 0) {
                out_report->summary.puppet_meshes++;
            } else if (strcasecmp(type_str, "particle") == 0) {
                out_report->summary.particle_systems++;
            } else if (strcasecmp(type_str, "sound") == 0) {
                out_report->summary.sound_layers++;
            } else if (strcasecmp(type_str, "model3d") == 0) {
                flags.model3d_degraded = true;
                report_add_issue(out_report, "MODEL_3D_DEGRADED", WE_ISSUE_SEV_WARNING,
                                 "3D model rendered as billboard quad in 2D engine", scene_filename,
                                 1, "Simplify model to 2D puppet warp");
            } else {
                out_report->summary.sprite_layers++;
            }

            const we_jval_t *scripts_val = we_jobj_get(obj, "scripts");
            const we_jval_t *script_val = we_jobj_get(obj, "script");
            if (scripts_val || script_val) {
                out_report->summary.active_scripts++;
            }
            if (script_val && script_val->type == JVAL_STR && script_val->str) {
                uint32_t bad_line = 1u;
                const char *bad_prim = find_forbidden_js_primitive(
                    script_val->str, strlen(script_val->str), &bad_line);
                if (bad_prim) {
                    flags.script_violation = true;
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "Script attempts to access forbidden global or security API: '%s'",
                             bad_prim);
                    report_add_issue(out_report, "SECURITY_SCRIPT_VIOLATION", WE_ISSUE_SEV_FATAL,
                                     msg, scene_filename, bad_line,
                                     "Remove forbidden API call from SceneScript");
                }
            }
        }

        if (nodes) {
            for (size_t i = 0; i < n_objs; i++) {
                int32_t cur = nodes[i].id;
                size_t steps = 0;
                while (cur >= 0 && steps <= n_objs) {
                    int32_t p = -1;
                    for (size_t k = 0; k < n_objs; k++) {
                        if (nodes[k].id == cur) {
                            p = nodes[k].parent;
                            break;
                        }
                    }
                    if (p < 0) {
                        break;
                    }
                    if (p == nodes[i].id) {
                        flags.cycle_detected = true;
                        report_add_issue(out_report, "CYCLE_DETECTED", WE_ISSUE_SEV_ERROR,
                                         "Circular parent hierarchy loop detected in scene graph",
                                         scene_filename, 1, "Break circular parent reference");
                        break;
                    }
                    cur = p;
                    steps++;
                }
                if (steps > n_objs) {
                    flags.cycle_detected = true;
                }
                if (flags.cycle_detected) {
                    break;
                }
            }
            free(nodes);
        }
    }

    if (vfs.pkg && vfs.pkg->entries) {
        for (uint32_t i = 0; i < vfs.pkg->entry_count; i++) {
            const we_pkg_entry_t *ent = &vfs.pkg->entries[i];
            audit_single_asset(&vfs, ent->path, ent->data, ent->length, scanner, out_report,
                               &flags);
        }
    }

    struct stat ws_st;
    if (stat(workshop_dir, &ws_st) == 0 && S_ISDIR(ws_st.st_mode)) {
        scan_loose_assets_recursive(&vfs, workshop_dir, "", 0, scanner, out_report, &flags);
    }

    if (has_project) {
        collect_scene_user_props_ast(scene_root, &proj, out_report, &flags, 0);
        collect_scene_user_props_slice((const char *)scene_data, scene_sz, &proj, out_report,
                                       &flags);
    }

    we_jval_free(scene_root);
    if (scene_free) {
        free((void *)scene_data);
    }
    compat_vfs_close(&vfs);

    if (flags.shader_dxbc_blocked || flags.shader_stage_blocked || flags.puppet_blocked ||
        flags.script_violation) {
        out_report->tier = WE_TIER_4_BLOCKED;
        out_report->compatibility_score = 0;
        out_report->summary.estimated_fps = 0;
        out_report->summary.estimated_vram_mb = 0.0f;
    } else if (flags.cycle_detected || flags.puppet_exceeded || flags.script_loop ||
               flags.model3d_degraded || flags.particle_overflow) {
        out_report->tier = WE_TIER_3_DEGRADED;
        int32_t sc = 68 - (int32_t)(out_report->issue_count * 4u);
        if (sc < 40) {
            sc = 40;
        }
        if (sc > 74) {
            sc = 74;
        }
        out_report->compatibility_score = (uint32_t)sc;
        out_report->summary.estimated_fps = 30;
        out_report->summary.estimated_vram_mb = 64.0f;
    } else if (flags.custom_shaders_present || flags.varying_promoted || flags.puppet_clamped ||
               flags.particle_advanced || flags.bc7_fallback || flags.missing_user_prop) {
        out_report->tier = WE_TIER_2_TRANSPILED;
        int32_t sc = 92 - (int32_t)(out_report->issue_count * 3u);
        if (sc < 75) {
            sc = 75;
        }
        if (sc > 94) {
            sc = 94;
        }
        out_report->compatibility_score = (uint32_t)sc;
        out_report->summary.estimated_fps = 120;
        out_report->summary.estimated_vram_mb = 48.0f;
    } else {
        out_report->tier = WE_TIER_1_NATIVE;
        int32_t sc = 100 - (int32_t)(out_report->issue_count * 2u);
        if (sc < 95) {
            sc = 95;
        }
        out_report->compatibility_score = (uint32_t)sc;
        out_report->summary.estimated_fps = 144;
        out_report->summary.estimated_vram_mb = 32.0f;
    }

    float vram = 24.0f;
    vram += (float)out_report->summary.sprite_layers * 6.0f;
    vram += (float)out_report->summary.puppet_meshes * 8.0f;
    vram += (float)out_report->summary.particle_systems * 4.0f;
    if (flags.audio_reactive) {
        vram += 4.0f;
    }
    if (out_report->tier != WE_TIER_4_BLOCKED) {
        out_report->summary.estimated_vram_mb = vram;
    }

    return true;
}

/* =========================================================================
 * Confined File Operations & Symlink-Hardened Directory Copy
 * ========================================================================= */

static bool is_safe_confined_relpath(const char *rel_path)
{
    if (!rel_path || rel_path[0] == '\0' || rel_path[0] == '/') {
        return false;
    }
    const char *p = rel_path;
    while (*p) {
        const char *seg = p;
        while (*p && *p != '/') {
            unsigned char c = (unsigned char)*p;
            if (c < 0x20u || c == 0x7Fu || c == '\\') {
                return false;
            }
            p++;
        }
        size_t slen = (size_t)(p - seg);
        if (slen == 0u || (slen == 1u && seg[0] == '.') ||
            (slen == 2u && seg[0] == '.' && seg[1] == '.')) {
            return false;
        }
        if (*p == '/') {
            p++;
            if (*p == '\0') {
                return false;
            }
        }
    }
    return true;
}

static int open_beneath_confined(int root_dfd, const char *rel_path, int flags, mode_t mode)
{
    if (root_dfd < 0 || !is_safe_confined_relpath(rel_path)) {
        errno = EXDEV;
        return -1;
    }

    struct open_how how;
    memset(&how, 0, sizeof(how));
    how.flags = (uint64_t)(flags | O_CLOEXEC | O_NOFOLLOW);
    how.mode = (flags & (O_CREAT | O_TMPFILE)) ? (uint64_t)mode : 0u;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;

    int fd = (int)syscall(SYS_openat2, root_dfd, rel_path, &how, sizeof(how));
    if (fd < 0 && (errno == ENOSYS || errno == EPERM)) {
        int cur_dfd = root_dfd;
        const char *p = rel_path;
        while (*p) {
            const char *slash = strchr(p, '/');
            if (!slash) {
                break;
            }
            size_t seg_len = (size_t)(slash - p);
            if (seg_len == 0u || seg_len >= 256u) {
                if (cur_dfd != root_dfd) {
                    close(cur_dfd);
                }
                errno = EINVAL;
                return -1;
            }
            char comp[256];
            memcpy(comp, p, seg_len);
            comp[seg_len] = '\0';

            int next_dfd = openat(cur_dfd, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (cur_dfd != root_dfd) {
                close(cur_dfd);
            }
            if (next_dfd < 0) {
                return -1;
            }
            cur_dfd = next_dfd;
            p = slash + 1;
        }

        if (flags & O_CREAT) {
            fd = openat(cur_dfd, p, flags | O_CLOEXEC | O_NOFOLLOW, mode);
        } else {
            fd = openat(cur_dfd, p, flags | O_CLOEXEC | O_NOFOLLOW);
        }
        if (cur_dfd != root_dfd) {
            close(cur_dfd);
        }
    }

    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            close(fd);
            errno = EINVAL;
            return -1;
        }
    }
    return fd;
}

static bool write_all_fd(int fd, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, p + written, len - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        written += (size_t)n;
    }
    return true;
}

static bool rewrite_fd_contents(int fd, const void *data, size_t len)
{
    if (lseek(fd, 0, SEEK_SET) < 0) {
        return false;
    }
    if (!write_all_fd(fd, data, len)) {
        return false;
    }
    if (ftruncate(fd, (off_t)len) != 0) {
        return false;
    }
    return true;
}

static char *read_confined_file(int root_dfd, const char *rel_path, size_t max_bytes,
                                size_t *out_len)
{
    if (out_len) {
        *out_len = 0;
    }
    int fd = open_beneath_confined(root_dfd, rel_path, O_RDONLY, 0);
    if (fd < 0) {
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (size_t)st.st_size > max_bytes) {
        close(fd);
        return NULL;
    }

    size_t sz = (size_t)st.st_size;
    char *buf = (char *)malloc(sz + 1u);
    if (!buf) {
        close(fd);
        return NULL;
    }

    size_t rd_total = 0;
    while (rd_total < sz) {
        ssize_t n = read(fd, buf + rd_total, sz - rd_total);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            free(buf);
            close(fd);
            return NULL;
        }
        rd_total += (size_t)n;
    }
    close(fd);

    buf[rd_total] = '\0';
    if (out_len) {
        *out_len = rd_total;
    }
    return buf;
}

static bool write_confined_file(int root_dfd, const char *rel_path, const void *data, size_t len,
                                bool allow_create)
{
    int flags = O_WRONLY | O_TRUNC | (allow_create ? O_CREAT : 0);
    int fd = open_beneath_confined(root_dfd, rel_path, flags, 0644);
    if (fd < 0) {
        return false;
    }
    bool ok = write_all_fd(fd, data, len);
    close(fd);
    return ok;
}

static bool copy_file_at_no_symlink(int sdir_fd, int ddir_fd, const char *name)
{
    int sfd = openat(sdir_fd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (sfd < 0) {
        return false;
    }
    struct stat sst;
    if (fstat(sfd, &sst) != 0 || !S_ISREG(sst.st_mode)) {
        close(sfd);
        return false;
    }

    int dfd = openat(ddir_fd, name, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (dfd < 0) {
        close(sfd);
        return false;
    }

    char buf[8192];
    bool ok = true;
    for (;;) {
        ssize_t n = read(sfd, buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
            break;
        }
        if (!write_all_fd(dfd, buf, (size_t)n)) {
            ok = false;
            break;
        }
    }

    close(sfd);
    close(dfd);
    return ok;
}

static bool copy_dir_recursive_depth(const char *src, const char *dst, int depth)
{
    if (!src || !dst || depth > 16) {
        return false;
    }

    struct stat src_st;
    if (lstat(src, &src_st) != 0 || !S_ISDIR(src_st.st_mode)) {
        return false;
    }

    struct stat dst_st;
    if (lstat(dst, &dst_st) == 0) {
        if (!S_ISDIR(dst_st.st_mode)) {
            return false;
        }
    } else if (mkdir(dst, 0755) != 0) {
        return false;
    }

    int sdir_fd = open(src, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (sdir_fd < 0) {
        return false;
    }
    int ddir_fd = open(dst, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (ddir_fd < 0) {
        close(sdir_fd);
        return false;
    }

    DIR *d = fdopendir(sdir_fd);
    if (!d) {
        close(sdir_fd);
        close(ddir_fd);
        return false;
    }

    struct dirent *ent;
    bool ok = true;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (has_directory_traversal(ent->d_name)) {
            ok = false;
            break;
        }

        struct stat st;
        if (fstatat(sdir_fd, ent->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            char s_sub[4096], d_sub[4096];
            if (snprintf(s_sub, sizeof(s_sub), "%s/%s", src, ent->d_name) >= (int)sizeof(s_sub) ||
                snprintf(d_sub, sizeof(d_sub), "%s/%s", dst, ent->d_name) >= (int)sizeof(d_sub) ||
                !copy_dir_recursive_depth(s_sub, d_sub, depth + 1)) {
                ok = false;
                break;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (!copy_file_at_no_symlink(sdir_fd, ddir_fd, ent->d_name)) {
                ok = false;
                break;
            }
        }
    }

    closedir(d);
    close(ddir_fd);
    return ok;
}

static bool copy_dir_recursive(const char *src, const char *dst)
{
    return copy_dir_recursive_depth(src, dst, 0);
}

/* =========================================================================
 * AST-Backed Auto-Repair Transformations
 * ========================================================================= */

static const char *varying_swizzle_suffix(uint32_t width)
{
    switch (width) {
    case 1u:
        return ".x";
    case 2u:
        return ".xy";
    case 3u:
        return ".xyz";
    default:
        return "";
    }
}

static bool find_main_body_open_brace(const char *src, size_t len, size_t *out_brace_pos)
{
    size_t pos = 0;
    uint32_t line = 1u;
    while (pos < len) {
        glsl_skip_ws_and_comments(src, len, &pos, &line, true);
        if (pos >= len) {
            break;
        }
        if (isalpha((unsigned char)src[pos]) || src[pos] == '_') {
            size_t start = pos++;
            while (pos < len && (isalnum((unsigned char)src[pos]) || src[pos] == '_')) {
                pos++;
            }
            if (pos - start == 4u && memcmp(src + start, "main", 4) == 0) {
                glsl_skip_ws_and_comments(src, len, &pos, &line, false);
                if (pos < len && src[pos] == '(') {
                    uint32_t pdepth = 1u;
                    pos++;
                    while (pos < len && pdepth > 0u) {
                        if (src[pos] == '(') {
                            pdepth++;
                        } else if (src[pos] == ')') {
                            pdepth--;
                        }
                        pos++;
                    }
                    glsl_skip_ws_and_comments(src, len, &pos, &line, false);
                    if (pos < len && src[pos] == '{') {
                        *out_brace_pos = pos;
                        return true;
                    }
                }
            }
            continue;
        }
        pos++;
    }
    return false;
}

/* Promotes selected varyings to vec4 _wwal_v4_<name> while preserving original vector arity
 * across bare r-values, bare l-values, and explicit .xyzw/.rgba/.stpq component accesses. */
static char *rewrite_shader_promote_varyings(const char *src, size_t len,
                                             const we_compat_varying_t *vars,
                                             const bool *promote_mask, uint32_t var_count,
                                             bool is_vertex_stage, size_t *out_len)
{
    compat_sb_t sb;
    sb_init(&sb);

    size_t main_brace = SIZE_MAX;
    if (is_vertex_stage) {
        (void)find_main_body_open_brace(src, len, &main_brace);
    }

    size_t pos = 0;
    bool prev_was_dot = false;
    bool pending_struct = false;
    uint32_t struct_brace_depth = 0u;
    while (pos < len) {
        if (src[pos] == '/' && pos + 1u < len && src[pos + 1u] == '/') {
            size_t start = pos;
            pos += 2u;
            while (pos < len && src[pos] != '\n') {
                pos++;
            }
            (void)sb_append_n(&sb, src + start, pos - start);
            continue;
        }
        if (src[pos] == '/' && pos + 1u < len && src[pos + 1u] == '*') {
            size_t start = pos;
            pos += 2u;
            while (pos < len) {
                if (src[pos] == '*' && pos + 1u < len && src[pos + 1u] == '/') {
                    pos += 2u;
                    break;
                }
                pos++;
            }
            (void)sb_append_n(&sb, src + start, pos - start);
            continue;
        }

        if (src[pos] == '#') {
            bool at_bol = (pos == 0u || src[pos - 1u] == '\n');
            if (at_bol) {
                size_t start = pos;
                while (pos < len && src[pos] != '\n') {
                    pos++;
                }
                (void)sb_append_n(&sb, src + start, pos - start);
                continue;
            }
        }

        bool handled_decl = false;
        for (uint32_t v = 0; v < var_count; v++) {
            if (!promote_mask[v]) {
                continue;
            }
            if (pos == vars[v].type_off) {
                /* Emit only the promoted vec4 declaration; body tokens are rewritten below so
                 * struct members sharing the identifier name are never corrupted by a #define. */
                (void)sb_append(&sb, "vec4 _wwal_v4_");
                (void)sb_append_n(&sb, vars[v].name, strlen(vars[v].name));
                (void)sb_append(&sb, ";");
                pos = vars[v].semi_off + 1u;
                prev_was_dot = false;
                pending_struct = false;
                handled_decl = true;
                break;
            }
        }
        if (handled_decl) {
            continue;
        }

        if (pos == main_brace) {
            (void)sb_append_n(&sb, "{", 1u);
            for (uint32_t v = 0; v < var_count; v++) {
                if (!promote_mask[v]) {
                    continue;
                }
                (void)sb_append(&sb, "\n    _wwal_v4_");
                (void)sb_append(&sb, vars[v].name);
                (void)sb_append(&sb, " = vec4(0.0);");
            }
            pos++;
            prev_was_dot = false;
            pending_struct = false;
            continue;
        }

        if (isalpha((unsigned char)src[pos]) || src[pos] == '_') {
            size_t tok_start = pos++;
            while (pos < len && (isalnum((unsigned char)src[pos]) || src[pos] == '_')) {
                pos++;
            }
            size_t tok_len = pos - tok_start;

            if (tok_len == 6u && memcmp(src + tok_start, "struct", 6) == 0) {
                (void)sb_append_n(&sb, src + tok_start, tok_len);
                pending_struct = true;
                prev_was_dot = false;
                continue;
            }

            int matched = -1;
            if (!prev_was_dot && !pending_struct && struct_brace_depth == 0u) {
                for (uint32_t v = 0; v < var_count; v++) {
                    if (promote_mask[v] && strlen(vars[v].name) == tok_len &&
                        memcmp(src + tok_start, vars[v].name, tok_len) == 0) {
                        matched = (int)v;
                        break;
                    }
                }
            }
            prev_was_dot = false;

            if (matched >= 0) {
                size_t peek = pos;
                uint32_t dummy_line = 1u;
                glsl_skip_ws_and_comments(src, len, &peek, &dummy_line, false);
                bool has_member_dot = (peek < len && src[peek] == '.');
                const char *swiz = varying_swizzle_suffix(vars[matched].width);

                (void)sb_append(&sb, "_wwal_v4_");
                (void)sb_append(&sb, vars[matched].name);
                if (!has_member_dot && swiz[0] != '\0') {
                    (void)sb_append(&sb, swiz);
                }
            } else {
                (void)sb_append_n(&sb, src + tok_start, tok_len);
            }
            continue;
        }

        if (!isspace((unsigned char)src[pos])) {
            char ch = src[pos];
            if (ch == '{') {
                if (pending_struct || struct_brace_depth > 0u) {
                    pending_struct = false;
                    struct_brace_depth++;
                }
                prev_was_dot = false;
            } else if (ch == '}') {
                if (struct_brace_depth > 0u) {
                    struct_brace_depth--;
                }
                pending_struct = false;
                prev_was_dot = false;
            } else if (ch == ';' || ch == '(' || ch == ')') {
                pending_struct = false;
                prev_was_dot = false;
            } else if (ch == '.') {
                prev_was_dot = true;
            } else {
                prev_was_dot = false;
            }
        }

        (void)sb_append_n(&sb, &src[pos], 1u);
        pos++;
    }

    if (sb.oom) {
        free(sb.buf);
        return NULL;
    }
    if (out_len) {
        *out_len = sb.len;
    }
    return sb.buf;
}

static bool repair_promote_varying_confined(int root_dfd, const char *target_rel)
{
    size_t plen = strlen(target_rel);
    bool is_vert = has_ext_ci(target_rel, plen, ".vert");
    bool is_frag = has_ext_ci(target_rel, plen, ".frag");

    size_t src_len = 0;
    char *src = read_confined_file(root_dfd, target_rel, 1024u * 1024u, &src_len);
    if (!src) {
        return false;
    }

    we_compat_varying_t vars[32];
    bool mask[32] = {false};
    uint32_t vcnt = parse_varyings_from_stage(src, src_len, vars, 32u, is_vert || !is_frag);

    char peer_rel[1024] = {0};
    char *peer_src = NULL;
    size_t peer_len = 0;
    if ((is_vert || is_frag) && plen >= 5u && plen < sizeof(peer_rel)) {
        snprintf(peer_rel, sizeof(peer_rel), "%.*s%s", (int)(plen - 5u), target_rel,
                 is_vert ? ".frag" : ".vert");
        peer_src = read_confined_file(root_dfd, peer_rel, 1024u * 1024u, &peer_len);
    }

    bool any = false;
    if (peer_src) {
        we_compat_varying_t pvars[32];
        bool pmask[32] = {false};
        uint32_t pcnt = parse_varyings_from_stage(peer_src, peer_len, pvars, 32u, !is_vert);

        for (uint32_t i = 0; i < vcnt; i++) {
            for (uint32_t j = 0; j < pcnt; j++) {
                if (strcmp(vars[i].name, pvars[j].name) == 0 && vars[i].width != pvars[j].width) {
                    mask[i] = true;
                    pmask[j] = true;
                    any = true;
                }
            }
        }

        if (any) {
            size_t new_peer_len = 0;
            char *new_peer = rewrite_shader_promote_varyings(peer_src, peer_len, pvars, pmask, pcnt,
                                                             !is_vert, &new_peer_len);
            if (!new_peer ||
                !write_confined_file(root_dfd, peer_rel, new_peer, new_peer_len, false)) {
                free(new_peer);
                free(peer_src);
                free(src);
                return false;
            }
            free(new_peer);
        }
        free(peer_src);
    } else {
        for (uint32_t i = 0; i < vcnt; i++) {
            if (vars[i].width < 4u) {
                mask[i] = true;
                any = true;
            }
        }
    }

    if (!any) {
        free(src);
        return true;
    }

    size_t new_len = 0;
    char *repaired = rewrite_shader_promote_varyings(src, src_len, vars, mask, vcnt,
                                                     is_vert || !is_frag, &new_len);
    free(src);
    if (!repaired) {
        return false;
    }

    bool ok = write_confined_file(root_dfd, target_rel, repaired, new_len, false);
    free(repaired);
    return ok;
}

static pthread_once_t g_compat_locale_once = PTHREAD_ONCE_INIT;
static locale_t g_compat_c_locale = (locale_t)0;

static void compat_init_c_locale(void)
{
    g_compat_c_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
}

__attribute__((destructor)) static void compat_free_c_locale(void)
{
    if (g_compat_c_locale != (locale_t)0) {
        freelocale(g_compat_c_locale);
        g_compat_c_locale = (locale_t)0;
    }
}

static locale_t compat_push_c_locale(void)
{
    pthread_once(&g_compat_locale_once, compat_init_c_locale);
    if (g_compat_c_locale == (locale_t)0) {
        return (locale_t)0;
    }
    return uselocale(g_compat_c_locale);
}

static void compat_pop_c_locale(locale_t prev)
{
    if (prev != (locale_t)0) {
        uselocale(prev);
    }
}

static void sb_append_indent(compat_sb_t *sb, int depth)
{
    for (int i = 0; i < depth; i++) {
        (void)sb_append_n(sb, "  ", 2u);
    }
}

static void sb_serialize_jval(compat_sb_t *sb, const we_jval_t *v, int depth)
{
    if (!v) {
        (void)sb_append(sb, "null");
        return;
    }
    switch (v->type) {
    case JVAL_NULL:
        (void)sb_append(sb, "null");
        break;
    case JVAL_BOOL:
        (void)sb_append(sb, v->bval ? "true" : "false");
        break;
    case JVAL_NUM: {
        char nbuf[64];
        if (isfinite(v->num)) {
            locale_t prev_loc = compat_push_c_locale();
            snprintf(nbuf, sizeof(nbuf), "%.17g", v->num);
            compat_pop_c_locale(prev_loc);
        } else {
            snprintf(nbuf, sizeof(nbuf), "0");
        }
        (void)sb_append(sb, nbuf);
        break;
    }
    case JVAL_STR:
        sb_append_json_str(sb, v->str ? v->str : "");
        break;
    case JVAL_ARR:
        if (v->arr.count == 0u) {
            (void)sb_append(sb, "[]");
            break;
        }
        (void)sb_append(sb, "[\n");
        for (size_t i = 0; i < v->arr.count; i++) {
            sb_append_indent(sb, depth + 1);
            sb_serialize_jval(sb, v->arr.items[i], depth + 1);
            if (i + 1u < v->arr.count) {
                (void)sb_append_n(sb, ",", 1u);
            }
            (void)sb_append_n(sb, "\n", 1u);
        }
        sb_append_indent(sb, depth);
        (void)sb_append_n(sb, "]", 1u);
        break;
    case JVAL_OBJ:
        if (v->obj.count == 0u) {
            (void)sb_append(sb, "{}");
            break;
        }
        (void)sb_append(sb, "{\n");
        for (size_t i = 0; i < v->obj.count; i++) {
            sb_append_indent(sb, depth + 1);
            sb_append_json_str(sb, v->obj.entries[i].key ? v->obj.entries[i].key : "");
            (void)sb_append(sb, ": ");
            sb_serialize_jval(sb, v->obj.entries[i].val, depth + 1);
            if (i + 1u < v->obj.count) {
                (void)sb_append_n(sb, ",", 1u);
            }
            (void)sb_append_n(sb, "\n", 1u);
        }
        sb_append_indent(sb, depth);
        (void)sb_append_n(sb, "}", 1u);
        break;
    }
}

static we_jval_t *jval_new_str(const char *s)
{
    we_jval_t *v = (we_jval_t *)calloc(1, sizeof(we_jval_t));
    if (!v) {
        return NULL;
    }
    v->type = JVAL_STR;
    v->str = strdup(s ? s : "");
    if (!v->str) {
        free(v);
        return NULL;
    }
    return v;
}

static we_jval_t *jval_new_num(double num)
{
    we_jval_t *v = (we_jval_t *)calloc(1, sizeof(we_jval_t));
    if (!v) {
        return NULL;
    }
    v->type = JVAL_NUM;
    v->num = num;
    return v;
}

static we_jval_t *jval_new_bool(bool bval)
{
    we_jval_t *v = (we_jval_t *)calloc(1, sizeof(we_jval_t));
    if (!v) {
        return NULL;
    }
    v->type = JVAL_BOOL;
    v->bval = bval;
    return v;
}

static we_jval_t *jval_new_obj(void)
{
    we_jval_t *v = (we_jval_t *)calloc(1, sizeof(we_jval_t));
    if (!v) {
        return NULL;
    }
    v->type = JVAL_OBJ;
    return v;
}

static we_jval_t *jobj_get_mut(we_jval_t *obj, const char *key)
{
    if (!obj || obj->type != JVAL_OBJ || !key) {
        return NULL;
    }
    for (size_t i = 0; i < obj->obj.count; i++) {
        if (obj->obj.entries[i].key && strcasecmp(obj->obj.entries[i].key, key) == 0) {
            return obj->obj.entries[i].val;
        }
    }
    return NULL;
}

static bool jobj_put_owned(we_jval_t *obj, const char *key, we_jval_t *val)
{
    if (!obj || obj->type != JVAL_OBJ || !key || !val) {
        we_jval_free(val);
        return false;
    }
    for (size_t i = 0; i < obj->obj.count; i++) {
        if (obj->obj.entries[i].key && strcmp(obj->obj.entries[i].key, key) == 0) {
            we_jval_free(obj->obj.entries[i].val);
            obj->obj.entries[i].val = val;
            return true;
        }
    }

    char *kcopy = strdup(key);
    if (!kcopy) {
        we_jval_free(val);
        return false;
    }
    we_jkv_t *nentries =
        (we_jkv_t *)realloc(obj->obj.entries, (obj->obj.count + 1u) * sizeof(we_jkv_t));
    if (!nentries) {
        free(kcopy);
        we_jval_free(val);
        return false;
    }
    obj->obj.entries = nentries;
    obj->obj.entries[obj->obj.count].key = kcopy;
    obj->obj.entries[obj->obj.count].val = val;
    obj->obj.count++;
    return true;
}

static uint32_t collect_missing_prop_names(const we_compat_report_t *report,
                                           const we_auto_repair_t *rep, char out_names[32][64])
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < report->issue_count && n < 32u; i++) {
        const we_compat_issue_t *iss = &report->issues[i];
        if (strcmp(iss->code, "MISSING_USER_PROPERTY") != 0) {
            continue;
        }
        const char *q1 = strchr(iss->message, '\'');
        const char *q2 = q1 ? strchr(q1 + 1, '\'') : NULL;
        if (q1 && q2 && q2 > q1 + 1 && (size_t)(q2 - q1 - 1) < 64u) {
            char cand[64];
            size_t clen = (size_t)(q2 - q1 - 1);
            memcpy(cand, q1 + 1, clen);
            cand[clen] = '\0';
            if (is_valid_prop_ident(cand)) {
                bool dup = false;
                for (uint32_t k = 0; k < n; k++) {
                    if (strcmp(out_names[k], cand) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) {
                    memcpy(out_names[n++], cand, clen + 1u);
                }
            }
        }
    }

    if (n == 0u && rep && rep->description[0] != '\0') {
        const char *q1 = strchr(rep->description, '\'');
        const char *q2 = q1 ? strchr(q1 + 1, '\'') : NULL;
        if (q1 && q2 && q2 > q1 + 1 && (size_t)(q2 - q1 - 1) < 64u) {
            size_t clen = (size_t)(q2 - q1 - 1);
            char cand[64];
            memcpy(cand, q1 + 1, clen);
            cand[clen] = '\0';
            if (is_valid_prop_ident(cand)) {
                memcpy(out_names[n++], cand, clen + 1u);
            }
        }
    }

    if (n == 0u) {
        snprintf(out_names[n++], 64u, "user_prop");
    }
    return n;
}

static bool inject_shader_uniforms_confined(int root_dfd, const char *shader_rel,
                                            char prop_names[32][64], uint32_t prop_count,
                                            bool only_if_referenced)
{
    size_t slen = 0;
    char *src = read_confined_file(root_dfd, shader_rel, 1024u * 1024u, &slen);
    if (!src) {
        return false;
    }

    size_t insert_off = 0;
    size_t pos = 0;
    while (pos < slen) {
        size_t line_start = pos;
        while (pos < slen && isspace((unsigned char)src[pos]) && src[pos] != '\n') {
            pos++;
        }
        if (pos < slen && ((slen - pos >= 8u && strncmp(src + pos, "#version", 8) == 0) ||
                           (slen - pos >= 10u && strncmp(src + pos, "#extension", 10) == 0) ||
                           (slen - pos >= 9u && strncmp(src + pos, "precision", 9) == 0))) {
            while (pos < slen && src[pos] != '\n') {
                pos++;
            }
            if (pos < slen && src[pos] == '\n') {
                pos++;
            }
            insert_off = pos;
            continue;
        }
        pos = line_start;
        break;
    }

    compat_sb_t decls;
    sb_init(&decls);
    bool injected_any = false;

    for (uint32_t i = 0; i < prop_count; i++) {
        const char *p = prop_names[i];
        char u_name[80];
        if (strncmp(p, "u_", 2) == 0) {
            snprintf(u_name, sizeof(u_name), "%s", p);
        } else {
            snprintf(u_name, sizeof(u_name), "u_%s", p);
        }

        char decl_probe1[96], decl_probe2[96];
        snprintf(decl_probe1, sizeof(decl_probe1), "uniform float %s", u_name);
        snprintf(decl_probe2, sizeof(decl_probe2), "uniform float %s", p);
        if (strstr(src, decl_probe1) || strstr(src, decl_probe2)) {
            continue;
        }

        if (only_if_referenced && !strstr(src, u_name) && !strstr(src, p)) {
            continue;
        }

        const char *chosen =
            (only_if_referenced && strstr(src, p) && !strstr(src, u_name)) ? p : u_name;
        char line_buf[256];
        snprintf(line_buf, sizeof(line_buf),
                 "uniform float %s; // {\"material\":\"%s\",\"default\":1.0}\n", chosen, p);
        (void)sb_append(&decls, line_buf);
        injected_any = true;
    }

    if (!injected_any) {
        free(decls.buf);
        free(src);
        return true;
    }

    compat_sb_t out;
    sb_init(&out);
    (void)sb_append_n(&out, src, insert_off);
    (void)sb_append_n(&out, decls.buf, decls.len);
    (void)sb_append_n(&out, src + insert_off, slen - insert_off);
    free(decls.buf);
    free(src);

    if (out.oom) {
        free(out.buf);
        return false;
    }
    bool ok = write_confined_file(root_dfd, shader_rel, out.buf, out.len, false);
    free(out.buf);
    return ok;
}

static void synthesize_shader_props_in_dir(int root_dfd, const char *rel_prefix, int depth,
                                           char prop_names[32][64], uint32_t prop_count)
{
    if (depth > 8) {
        return;
    }
    int dfd = -1;
    if (rel_prefix[0] == '\0') {
        dfd = fcntl(root_dfd, F_DUPFD_CLOEXEC, 0);
    } else if (is_safe_confined_relpath(rel_prefix)) {
        struct open_how how;
        memset(&how, 0, sizeof(how));
        how.flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
        how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
        dfd = (int)syscall(SYS_openat2, root_dfd, rel_prefix, &how, sizeof(how));
        if (dfd < 0 && (errno == ENOSYS || errno == EPERM)) {
            int cur_dfd = root_dfd;
            const char *p = rel_prefix;
            while (*p) {
                const char *slash = strchr(p, '/');
                size_t seg_len = slash ? (size_t)(slash - p) : strlen(p);
                if (seg_len == 0u || seg_len >= 256u) {
                    if (cur_dfd != root_dfd) {
                        close(cur_dfd);
                    }
                    cur_dfd = -1;
                    break;
                }
                char comp[256];
                memcpy(comp, p, seg_len);
                comp[seg_len] = '\0';
                int next_dfd =
                    openat(cur_dfd, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (cur_dfd != root_dfd) {
                    close(cur_dfd);
                }
                if (next_dfd < 0) {
                    cur_dfd = -1;
                    break;
                }
                cur_dfd = next_dfd;
                if (!slash) {
                    break;
                }
                p = slash + 1;
            }
            dfd = (cur_dfd != root_dfd) ? cur_dfd : -1;
        }
    }
    if (dfd < 0) {
        return;
    }

    DIR *d = fdopendir(dfd);
    if (!d) {
        close(dfd);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        struct stat st;
        if (fstatat(dfd, ent->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || S_ISLNK(st.st_mode)) {
            continue;
        }
        char child_rel[1024];
        if (rel_prefix[0] == '\0') {
            snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);
        } else {
            snprintf(child_rel, sizeof(child_rel), "%s/%s", rel_prefix, ent->d_name);
        }

        if (S_ISDIR(st.st_mode)) {
            synthesize_shader_props_in_dir(root_dfd, child_rel, depth + 1, prop_names, prop_count);
        } else if (S_ISREG(st.st_mode)) {
            size_t nlen = strlen(ent->d_name);
            if (has_ext_ci(ent->d_name, nlen, ".vert") || has_ext_ci(ent->d_name, nlen, ".frag") ||
                has_ext_ci(ent->d_name, nlen, ".glsl") || has_ext_ci(ent->d_name, nlen, ".hlsl")) {
                (void)inject_shader_uniforms_confined(root_dfd, child_rel, prop_names, prop_count,
                                                      true);
            }
        }
    }
    closedir(d);
}

static bool repair_synthesize_user_prop_confined(int root_dfd, const we_compat_report_t *report,
                                                 const we_auto_repair_t *rep)
{
    char prop_names[32][64];
    uint32_t prop_count = collect_missing_prop_names(report, rep, prop_names);

    size_t plen = strlen(rep->target_file);
    if (has_ext_ci(rep->target_file, plen, ".vert") ||
        has_ext_ci(rep->target_file, plen, ".frag") ||
        has_ext_ci(rep->target_file, plen, ".glsl") ||
        has_ext_ci(rep->target_file, plen, ".hlsl")) {
        return inject_shader_uniforms_confined(root_dfd, rep->target_file, prop_names, prop_count,
                                               false);
    }

    size_t json_len = 0;
    char *json_buf = read_confined_file(root_dfd, rep->target_file, 4u * 1024u * 1024u, &json_len);
    if (!json_buf) {
        return false;
    }

    we_jval_t *root = we_json_parse_root(json_buf, json_len);
    free(json_buf);
    if (!root || root->type != JVAL_OBJ) {
        we_jval_free(root);
        return false;
    }

    we_jval_t *general = jobj_get_mut(root, "general");
    if (!general || general->type != JVAL_OBJ) {
        we_jval_t *new_gen = jval_new_obj();
        if (!new_gen || !jobj_put_owned(root, "general", new_gen)) {
            we_jval_free(root);
            return false;
        }
        general = new_gen;
    }

    we_jval_t *props = jobj_get_mut(general, "properties");
    if (!props || props->type != JVAL_OBJ) {
        we_jval_t *new_props = jval_new_obj();
        if (!new_props || !jobj_put_owned(general, "properties", new_props)) {
            we_jval_free(root);
            return false;
        }
        props = new_props;
    }

    for (uint32_t i = 0; i < prop_count; i++) {
        if (jobj_get_mut(props, prop_names[i])) {
            continue;
        }
        we_jval_t *pobj = jval_new_obj();
        if (!pobj) {
            we_jval_free(root);
            return false;
        }
        if (!jobj_put_owned(pobj, "text", jval_new_str(prop_names[i])) ||
            !jobj_put_owned(pobj, "type", jval_new_str("slider")) ||
            !jobj_put_owned(pobj, "value", jval_new_num(1.0)) ||
            !jobj_put_owned(props, prop_names[i], pobj)) {
            we_jval_free(root);
            return false;
        }
    }

    compat_sb_t sb;
    sb_init(&sb);
    sb_serialize_jval(&sb, root, 0);
    (void)sb_append_n(&sb, "\n", 1u);
    we_jval_free(root);

    if (sb.oom) {
        free(sb.buf);
        return false;
    }

    bool ok = write_confined_file(root_dfd, rep->target_file, sb.buf, sb.len, false);
    free(sb.buf);
    if (!ok) {
        return false;
    }

    synthesize_shader_props_in_dir(root_dfd, "", 0, prop_names, prop_count);
    return true;
}

static bool repair_clamp_particles_confined(int root_dfd, const char *target_rel)
{
    int fd = open_beneath_confined(root_dfd, target_rel, O_RDWR, 0);
    if (fd < 0) {
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 1024 * 1024) {
        close(fd);
        return false;
    }

    size_t sz = (size_t)st.st_size;
    char *code = (char *)malloc(sz + 1u);
    if (!code) {
        close(fd);
        return false;
    }
    size_t total = 0u;
    bool io_err = false;
    while (total < sz) {
        ssize_t r = read(fd, code + total, sz - total);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            io_err = true;
            break;
        }
        if (r == 0) {
            break;
        }
        total += (size_t)r;
    }
    if (io_err || total != sz) {
        free(code);
        close(fd);
        return false;
    }
    code[sz] = '\0';

    we_jval_t *root = we_json_parse_root(code, sz);
    free(code);
    if (!root || root->type != JVAL_OBJ) {
        we_jval_free(root);
        close(fd);
        return false;
    }

    we_jval_t *mc = jobj_get_mut(root, "maxcount");
    if (!mc) {
        mc = jobj_get_mut(root, "count");
    }
    if (!mc) {
        we_jval_t *sub = jobj_get_mut(root, "particle");
        if (sub && sub->type == JVAL_OBJ) {
            mc = jobj_get_mut(sub, "maxcount");
            if (!mc) {
                mc = jobj_get_mut(sub, "count");
            }
        }
    }
    if (mc && mc->type == JVAL_NUM && mc->num > 16384.0) {
        mc->num = 16384.0;
    }

    compat_sb_t sb;
    sb_init(&sb);
    sb_serialize_jval(&sb, root, 0);
    (void)sb_append_n(&sb, "\n", 1u);
    we_jval_free(root);

    bool ok = false;
    if (!sb.oom) {
        ok = rewrite_fd_contents(fd, sb.buf, sb.len);
    }
    free(sb.buf);
    close(fd);
    return ok;
}

static bool merge_compat_config_confined(int root_dfd, bool want_bc7_fallback,
                                         bool want_clamp_bones)
{
    size_t existing_len = 0u;
    char *existing_buf = read_confined_file(root_dfd, ".waywal_compat.json", 65536u, &existing_len);
    we_jval_t *cfg = NULL;
    if (existing_buf) {
        cfg = we_json_parse_root(existing_buf, existing_len);
        free(existing_buf);
        if (cfg && cfg->type != JVAL_OBJ) {
            we_jval_free(cfg);
            cfg = NULL;
        }
    }
    if (!cfg) {
        cfg = jval_new_obj();
        if (!cfg) {
            return false;
        }
    }

    if (want_bc7_fallback && !jobj_put_owned(cfg, "bc7_software_fallback", jval_new_bool(true))) {
        we_jval_free(cfg);
        return false;
    }
    if (want_clamp_bones && !jobj_put_owned(cfg, "clamp_puppet_bones", jval_new_num(128.0))) {
        we_jval_free(cfg);
        return false;
    }

    compat_sb_t sb;
    sb_init(&sb);
    sb_serialize_jval(&sb, cfg, 0);
    (void)sb_append_n(&sb, "\n", 1u);
    we_jval_free(cfg);
    if (sb.oom) {
        free(sb.buf);
        return false;
    }
    bool ok = write_confined_file(root_dfd, ".waywal_compat.json", sb.buf, sb.len, true);
    free(sb.buf);
    return ok;
}

static bool apply_repairs_confined_core(const we_compat_report_t *report, const char *target_dir,
                                        uint32_t *out_applied)
{
    if (out_applied) {
        *out_applied = 0;
    }
    if (!report || !target_dir || target_dir[0] == '\0') {
        return false;
    }

    int root_dfd = open(target_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root_dfd < 0) {
        return false;
    }

    uint32_t applied = 0;
    bool all_ok = true;
    bool want_bc7_fallback = false;
    bool want_clamp_bones = false;

    for (uint32_t i = 0; i < report->repair_count; i++) {
        const we_auto_repair_t *rep = &report->repairs[i];
        if (!is_safe_confined_relpath(rep->target_file)) {
            all_ok = false;
            break;
        }

        if (strcmp(rep->transformation_id, "REPAIR_PROMOTE_VARYING") == 0) {
            if (!repair_promote_varying_confined(root_dfd, rep->target_file)) {
                all_ok = false;
                break;
            }
            applied++;
        } else if (strcmp(rep->transformation_id, "REPAIR_SYNTHESIZE_USER_PROP") == 0) {
            if (!repair_synthesize_user_prop_confined(root_dfd, report, rep)) {
                all_ok = false;
                break;
            }
            applied++;
        } else if (strcmp(rep->transformation_id, "REPAIR_STUB_SCRIPT") == 0) {
            static const char stub[] = "// Safe identity watchdog stub synthesized by WayWal\n"
                                       "'use strict';\n"
                                       "export function init(value) { return value; }\n"
                                       "export function update(value) { return value; }\n";
            if (!write_confined_file(root_dfd, rep->target_file, stub, sizeof(stub) - 1u, false)) {
                all_ok = false;
                break;
            }
            applied++;
        } else if (strcmp(rep->transformation_id, "REPAIR_BC7_SOFTWARE_FALLBACK") == 0) {
            int check_fd = open_beneath_confined(root_dfd, rep->target_file, O_RDONLY, 0);
            if (check_fd < 0) {
                all_ok = false;
                break;
            }
            close(check_fd);
            want_bc7_fallback = true;
            applied++;
        } else if (strcmp(rep->transformation_id, "REPAIR_CLAMP_PARTICLES") == 0) {
            if (!repair_clamp_particles_confined(root_dfd, rep->target_file)) {
                all_ok = false;
                break;
            }
            applied++;
        } else if (strcmp(rep->transformation_id, "REPAIR_CLAMP_BONES") == 0) {
            int check_fd = open_beneath_confined(root_dfd, rep->target_file, O_RDONLY, 0);
            if (check_fd < 0) {
                all_ok = false;
                break;
            }
            close(check_fd);
            want_clamp_bones = true;
            applied++;
        } else {
            all_ok = false;
            break;
        }
    }

    if (all_ok && (want_bc7_fallback || want_clamp_bones)) {
        if (!merge_compat_config_confined(root_dfd, want_bc7_fallback, want_clamp_bones)) {
            all_ok = false;
        }
    }

    close(root_dfd);
    if (out_applied) {
        *out_applied = all_ok ? applied : 0u;
    }
    return all_ok;
}

uint32_t we_compat_apply_repairs(const we_compat_report_t *report, const char *target_dir)
{
    uint32_t applied = 0;
    if (!apply_repairs_confined_core(report, target_dir, &applied)) {
        return 0;
    }
    return applied;
}

bool we_compat_apply_auto_repairs(const we_compat_report_t *report, const char *source_dir,
                                  const char *target_dir)
{
    if (!report || !source_dir || !target_dir || source_dir[0] == '\0' || target_dir[0] == '\0') {
        return false;
    }

    if (strcmp(source_dir, target_dir) != 0) {
        if (!copy_dir_recursive(source_dir, target_dir)) {
            return false;
        }
    }

    return apply_repairs_confined_core(report, target_dir, NULL);
}

/* =========================================================================
 * Report to JSON Serializer
 * ========================================================================= */

char *we_compat_report_to_json(const we_compat_report_t *report)
{
    if (!report) {
        return NULL;
    }

    compat_sb_t sb;
    sb_init(&sb);

    const char *tier_names[] = {"Unknown", "Tier 1: Native", "Tier 2: Transpiled",
                                "Tier 3: Degraded", "Tier 4: Blocked"};
    uint32_t t_idx = (uint32_t)report->tier;
    if (t_idx > 4u) {
        t_idx = 0u;
    }

    (void)sb_append(&sb, "{\n");
    (void)sb_append(&sb, "  \"workshop_id\": ");
    sb_append_json_str(&sb, report->workshop_id);
    (void)sb_append(&sb, ",\n  \"title\": ");
    sb_append_json_str(&sb, report->title);
    (void)sb_append(&sb, ",\n  \"tier\": ");
    char num[64];
    snprintf(num, sizeof(num), "%u", (uint32_t)report->tier);
    (void)sb_append(&sb, num);
    (void)sb_append(&sb, ",\n  \"tier_name\": ");
    sb_append_json_str(&sb, tier_names[t_idx]);
    (void)sb_append(&sb, ",\n  \"compatibility_score\": ");
    snprintf(num, sizeof(num), "%u", report->compatibility_score);
    (void)sb_append(&sb, num);
    (void)sb_append(&sb, ",\n  \"scan_timestamp\": ");
    snprintf(num, sizeof(num), "%llu", (unsigned long long)report->scan_timestamp);
    (void)sb_append(&sb, num);

    (void)sb_append(&sb, ",\n  \"summary\": {\n");
    char sum_buf[1024];
    locale_t prev_loc = compat_push_c_locale();
    snprintf(sum_buf, sizeof(sum_buf),
             "    \"total_objects\": %u,\n"
             "    \"sprite_layers\": %u,\n"
             "    \"puppet_meshes\": %u,\n"
             "    \"particle_systems\": %u,\n"
             "    \"sound_layers\": %u,\n"
             "    \"active_scripts\": %u,\n"
             "    \"estimated_vram_mb\": %.2f,\n"
             "    \"estimated_fps\": %u\n  }",
             report->summary.total_objects, report->summary.sprite_layers,
             report->summary.puppet_meshes, report->summary.particle_systems,
             report->summary.sound_layers, report->summary.active_scripts,
             report->summary.estimated_vram_mb, report->summary.estimated_fps);
    compat_pop_c_locale(prev_loc);
    (void)sb_append(&sb, sum_buf);

    (void)sb_append(&sb, ",\n  \"issues\": [\n");
    for (uint32_t i = 0; i < report->issue_count; i++) {
        const we_compat_issue_t *iss = &report->issues[i];
        (void)sb_append(&sb, "    {\n      \"code\": ");
        sb_append_json_str(&sb, iss->code);
        (void)sb_append(&sb, ",\n      \"severity\": ");
        snprintf(num, sizeof(num), "%u", (uint32_t)iss->severity);
        (void)sb_append(&sb, num);
        (void)sb_append(&sb, ",\n      \"message\": ");
        sb_append_json_str(&sb, iss->message);
        (void)sb_append(&sb, ",\n      \"file_path\": ");
        sb_append_json_str(&sb, iss->file_path);
        (void)sb_append(&sb, ",\n      \"line_number\": ");
        snprintf(num, sizeof(num), "%u", iss->line_number);
        (void)sb_append(&sb, num);
        (void)sb_append(&sb, ",\n      \"suggested_fix\": ");
        sb_append_json_str(&sb, iss->suggested_fix);
        (void)sb_append(&sb, "\n    }");
        if (i + 1u < report->issue_count) {
            (void)sb_append_n(&sb, ",", 1u);
        }
        (void)sb_append_n(&sb, "\n", 1u);
    }
    (void)sb_append(&sb, "  ],\n  \"issue_count\": ");
    snprintf(num, sizeof(num), "%u", report->issue_count);
    (void)sb_append(&sb, num);

    (void)sb_append(&sb, ",\n  \"repairs\": [\n");
    for (uint32_t i = 0; i < report->repair_count; i++) {
        const we_auto_repair_t *rep = &report->repairs[i];
        (void)sb_append(&sb, "    {\n      \"transformation_id\": ");
        sb_append_json_str(&sb, rep->transformation_id);
        (void)sb_append(&sb, ",\n      \"target_file\": ");
        sb_append_json_str(&sb, rep->target_file);
        (void)sb_append(&sb, ",\n      \"description\": ");
        sb_append_json_str(&sb, rep->description);
        (void)sb_append(&sb, "\n    }");
        if (i + 1u < report->repair_count) {
            (void)sb_append_n(&sb, ",", 1u);
        }
        (void)sb_append_n(&sb, "\n", 1u);
    }
    (void)sb_append(&sb, "  ],\n  \"repair_count\": ");
    snprintf(num, sizeof(num), "%u", report->repair_count);
    (void)sb_append(&sb, num);
    (void)sb_append(&sb, "\n}\n");

    if (sb.oom) {
        free(sb.buf);
        return NULL;
    }
    return sb.buf;
}
