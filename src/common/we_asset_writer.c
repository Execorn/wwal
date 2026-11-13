// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "waywal/we_asset_writer.h"

#include "waywal/we_pkg.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WE_LZ4_MIN_COMPRESS 13u
#define WE_LZ4_MFLIMIT      12u
#define WE_LZ4_LASTLITERALS 5u
#define WE_LZ4_MAX_DISTANCE 65535u
#define WE_LZ4_HASH_SIZE    4096u

static void set_err(we_asset_writer_err_t *out_err, we_asset_writer_err_t err)
{
    if (out_err) {
        *out_err = err;
    }
}

static inline void write_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8u) & 0xFFu);
    p[2] = (uint8_t)((v >> 16u) & 0xFFu);
    p[3] = (uint8_t)((v >> 24u) & 0xFFu);
}

static inline uint32_t lz4_read32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline uint8_t *lz4_emit_lsic(uint8_t *op, const uint8_t *out_end, size_t len)
{
    while (len >= 255u) {
        if (op >= out_end) {
            return NULL;
        }
        *op++ = 255u;
        len -= 255u;
    }
    if (op >= out_end) {
        return NULL;
    }
    *op++ = (uint8_t)len;
    return op;
}

size_t we_lz4_compress_bound(size_t src_len)
{
    if (src_len == 0 || src_len > WE_LZ4_MAX_INPUT_SIZE) {
        return 0;
    }
    return src_len + (src_len / 255u) + 16u;
}

size_t we_lz4_compress_block(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap)
{
    if (!src || !dst || src_len == 0 || src_len > WE_LZ4_MAX_INPUT_SIZE || dst_cap == 0) {
        return 0;
    }

    uint32_t ht[WE_LZ4_HASH_SIZE];
    /* UINT32_MAX sentinel keeps offset 0 distinguishable from an empty slot. */
    memset(ht, 0xFF, sizeof(ht));

    uint8_t *op = dst;
    const uint8_t *const out_end = dst + dst_cap;
    size_t ip = 0;
    size_t anchor = 0;

    if (src_len >= WE_LZ4_MIN_COMPRESS) {
        const size_t mf_limit = src_len - WE_LZ4_MFLIMIT;
        const size_t match_limit = src_len - WE_LZ4_LASTLITERALS;
        uint32_t search_nb = 1u << 6u;

        while (ip <= mf_limit) {
            uint32_t seq = lz4_read32(src + ip);
            uint32_t h = (seq * 2654435761u) >> 20u;
            uint32_t ref = ht[h];
            ht[h] = (uint32_t)ip;

            if (ref == UINT32_MAX || (ip - ref) > WE_LZ4_MAX_DISTANCE ||
                lz4_read32(src + ref) != seq) {
                size_t step = (size_t)(search_nb >> 6u);
                search_nb++;
                ip += step;
                continue;
            }

            search_nb = 1u << 6u;
            size_t lit_len = ip - anchor;
            size_t match_len = 4u;

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) &&                                 \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            while (ip + match_len + 8u <= match_limit) {
                uint64_t a, b;
                memcpy(&a, src + ref + match_len, sizeof(a));
                memcpy(&b, src + ip + match_len, sizeof(b));
                uint64_t diff = a ^ b;
                if (diff != 0) {
                    match_len += (size_t)(__builtin_ctzll(diff) >> 3);
                    goto match_extended;
                }
                match_len += 8u;
            }
#endif
            while (ip + match_len < match_limit && src[ref + match_len] == src[ip + match_len]) {
                match_len++;
            }
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) &&                                 \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        match_extended:;
#endif

            size_t ml_excess = match_len - 4u;
            if (op >= out_end) {
                return 0;
            }
            uint8_t *tok = op++;
            *tok = (uint8_t)(((lit_len < 15u ? lit_len : 15u) << 4u) |
                             (ml_excess < 15u ? ml_excess : 15u));
            if (lit_len >= 15u) {
                op = lz4_emit_lsic(op, out_end, lit_len - 15u);
                if (!op) {
                    return 0;
                }
            }
            if ((size_t)(out_end - op) < lit_len + 2u) {
                return 0;
            }
            if (lit_len > 0u) {
                memcpy(op, src + anchor, lit_len);
                op += lit_len;
            }

            uint16_t off = (uint16_t)(ip - ref);
            *op++ = (uint8_t)(off & 0xFFu);
            *op++ = (uint8_t)(off >> 8u);

            if (ml_excess >= 15u) {
                op = lz4_emit_lsic(op, out_end, ml_excess - 15u);
                if (!op) {
                    return 0;
                }
            }
            ip += match_len;
            anchor = ip;
        }
    }

    size_t last_lit = src_len - anchor;
    if (op >= out_end) {
        return 0;
    }
    *op++ = (uint8_t)((last_lit < 15u ? last_lit : 15u) << 4u);
    if (last_lit >= 15u) {
        op = lz4_emit_lsic(op, out_end, last_lit - 15u);
        if (!op) {
            return 0;
        }
    }
    if ((size_t)(out_end - op) < last_lit) {
        return 0;
    }
    if (last_lit > 0u) {
        memcpy(op, src + anchor, last_lit);
        op += last_lit;
    }
    return (size_t)(op - dst);
}

typedef struct {
    char norm_path[WE_PKG_MAX_PATH_LEN];
    uint32_t path_len;
    uint32_t offset;
    uint32_t size;
    const uint8_t *data;
} pkg_prep_entry_t;

bool we_normalize_rel_path(const char *raw_path, char *out_norm, size_t out_cap, uint32_t *out_len)
{
    if (out_len) {
        *out_len = 0u;
    }
    if (!raw_path || !out_norm || out_cap < 2u) {
        return false;
    }
    size_t raw_len = strlen(raw_path);
    if (raw_len == 0 || raw_len >= WE_PKG_MAX_PATH_LEN) {
        return false;
    }
    if (raw_path[0] == '/' || raw_path[0] == '\\') {
        return false;
    }

    char temp[WE_PKG_MAX_PATH_LEN];
    for (size_t i = 0; i < raw_len; i++) {
        unsigned char c = (unsigned char)raw_path[i];
        if (c < 0x20u || c == 0x7Fu || c == ':') {
            return false;
        }
        if (c == '\\') {
            temp[i] = '/';
        } else if (c >= 'A' && c <= 'Z') {
            temp[i] = (char)(c + ('a' - 'A'));
        } else {
            temp[i] = (char)c;
        }
    }
    temp[raw_len] = '\0';

    if (temp[raw_len - 1u] == '/') {
        return false;
    }

    size_t pos = 0;
    size_t nlen = 0;
    while (pos < raw_len) {
        while (pos < raw_len && temp[pos] == '/') {
            pos++;
        }
        if (pos >= raw_len) {
            break;
        }
        size_t seg_start = pos;
        while (pos < raw_len && temp[pos] != '/') {
            pos++;
        }
        size_t seg_len = pos - seg_start;
        if (seg_len == 1u && temp[seg_start] == '.') {
            return false;
        }
        if (seg_len == 2u && temp[seg_start] == '.' && temp[seg_start + 1u] == '.') {
            return false;
        }
        size_t need = seg_len + (nlen > 0u ? 1u : 0u);
        if (nlen + need >= out_cap) {
            return false;
        }
        if (nlen > 0u) {
            out_norm[nlen++] = '/';
        }
        memcpy(out_norm + nlen, temp + seg_start, seg_len);
        nlen += seg_len;
    }

    if (nlen == 0 || nlen >= WE_PKG_MAX_PATH_LEN) {
        return false;
    }
    out_norm[nlen] = '\0';
    if (out_len) {
        *out_len = (uint32_t)nlen;
    }
    return true;
}

static int cmp_pkg_prep_entry(const void *a, const void *b)
{
    const pkg_prep_entry_t *ea = (const pkg_prep_entry_t *)a;
    const pkg_prep_entry_t *eb = (const pkg_prep_entry_t *)b;
    const unsigned char *pa = (const unsigned char *)ea->norm_path;
    const unsigned char *pb = (const unsigned char *)eb->norm_path;
    while (*pa && *pa == *pb) {
        pa++;
        pb++;
    }
    return (int)*pa - (int)*pb;
}

static bool prepare_pkg_entries(const we_pkg_write_entry_t *entries, size_t entry_count,
                                pkg_prep_entry_t **out_items, uint64_t *out_dir_size,
                                uint64_t *out_total_size, we_asset_writer_err_t *out_err)
{
    *out_items = NULL;
    *out_dir_size = 16ULL;
    *out_total_size = 16ULL;

    if (entry_count > WE_PKG_MAX_WRITE_ENTRIES) {
        set_err(out_err, WE_WRITER_ERR_LIMIT_EXCEEDED);
        return false;
    }
    if (entry_count == 0) {
        return true;
    }
    if (!entries) {
        set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
        return false;
    }

    pkg_prep_entry_t *items = calloc(entry_count, sizeof(pkg_prep_entry_t));
    if (!items) {
        set_err(out_err, WE_WRITER_ERR_OOM);
        return false;
    }

    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].size > 0 && !entries[i].data) {
            free(items);
            set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
            return false;
        }
        if (entries[i].size > (size_t)UINT32_MAX) {
            free(items);
            set_err(out_err, WE_WRITER_ERR_OVERFLOW);
            return false;
        }
        if (!we_normalize_rel_path(entries[i].path, items[i].norm_path, sizeof(items[i].norm_path),
                                   &items[i].path_len)) {
            free(items);
            set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
            return false;
        }
        items[i].data = entries[i].data;
        items[i].size = (uint32_t)entries[i].size;
    }

    qsort(items, entry_count, sizeof(pkg_prep_entry_t), cmp_pkg_prep_entry);

    for (size_t i = 1; i < entry_count; i++) {
        if (strcmp(items[i - 1u].norm_path, items[i].norm_path) == 0) {
            free(items);
            set_err(out_err, WE_WRITER_ERR_DUPLICATE_PATH);
            return false;
        }
    }

    uint64_t dir_size = 16ULL;
    uint64_t data_off = 0ULL;
    for (size_t i = 0; i < entry_count; i++) {
        dir_size += 12ULL + (uint64_t)items[i].path_len;
        if (data_off > (uint64_t)UINT32_MAX ||
            (uint64_t)items[i].size > (uint64_t)UINT32_MAX - data_off) {
            free(items);
            set_err(out_err, WE_WRITER_ERR_OVERFLOW);
            return false;
        }
        items[i].offset = (uint32_t)data_off;
        data_off += (uint64_t)items[i].size;
    }

    if (dir_size > WE_PKG_MAX_WRITE_SIZE || data_off > WE_PKG_MAX_WRITE_SIZE - dir_size) {
        free(items);
        set_err(out_err, WE_WRITER_ERR_LIMIT_EXCEEDED);
        return false;
    }

    *out_items = items;
    *out_dir_size = dir_size;
    *out_total_size = dir_size + data_off;
    return true;
}

static void write_pkg_directory(uint8_t *dst, const pkg_prep_entry_t *items, size_t entry_count)
{
    write_u32_le(dst, 8u);
    memcpy(dst + 4u, "PKGV0022", 8u);
    write_u32_le(dst + 12u, (uint32_t)entry_count);

    uint8_t *p = dst + 16u;
    for (size_t i = 0; i < entry_count; i++) {
        write_u32_le(p, items[i].path_len);
        p += 4u;
        memcpy(p, items[i].norm_path, items[i].path_len);
        p += items[i].path_len;
        write_u32_le(p, items[i].offset);
        write_u32_le(p + 4u, items[i].size);
        p += 8u;
    }
}

bool we_pkg_write_mem(const we_pkg_write_entry_t *entries, size_t entry_count, uint8_t **out_buf,
                      size_t *out_size, we_asset_writer_err_t *out_err)
{
    if (!out_buf || !out_size) {
        set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
        return false;
    }
    *out_buf = NULL;
    *out_size = 0;

    pkg_prep_entry_t *items = NULL;
    uint64_t dir_size = 0;
    uint64_t total_size = 0;
    if (!prepare_pkg_entries(entries, entry_count, &items, &dir_size, &total_size, out_err)) {
        return false;
    }

    uint8_t *buf = malloc((size_t)total_size);
    if (!buf) {
        free(items);
        set_err(out_err, WE_WRITER_ERR_OOM);
        return false;
    }

    write_pkg_directory(buf, items, entry_count);
    uint8_t *p = buf + (size_t)dir_size;
    for (size_t i = 0; i < entry_count; i++) {
        if (items[i].size > 0u) {
            memcpy(p, items[i].data, items[i].size);
            p += items[i].size;
        }
    }

    free(items);
    *out_buf = buf;
    *out_size = (size_t)total_size;
    set_err(out_err, WE_WRITER_OK);
    return true;
}

static bool write_all_fd(int fd, const uint8_t *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, buf + done, len - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        done += (size_t)n;
    }
    return true;
}

static int open_atomic_tmp(const char *path, char **out_tmp_path, we_asset_writer_err_t *out_err)
{
    static atomic_uint_fast64_t s_tmp_seq = 0;
    uint64_t seq = atomic_fetch_add_explicit(&s_tmp_seq, 1u, memory_order_relaxed);
    size_t tmp_cap = strlen(path) + 64u;
    char *tmp_path = malloc(tmp_cap);
    if (!tmp_path) {
        set_err(out_err, WE_WRITER_ERR_OOM);
        return -1;
    }
    int n = snprintf(tmp_path, tmp_cap, "%s.tmp.%ld.%llu", path, (long)getpid(),
                     (unsigned long long)seq);
    if (n < 0 || (size_t)n >= tmp_cap) {
        free(tmp_path);
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return -1;
    }

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
        free(tmp_path);
        set_err(out_err, WE_WRITER_ERR_IO);
        return -1;
    }
    *out_tmp_path = tmp_path;
    return fd;
}

static bool finish_atomic_tmp(int fd, char *tmp_path, const char *dest_path, bool ok,
                              we_asset_writer_err_t *out_err)
{
    if (ok && fsync(fd) < 0 && errno != EINVAL) {
        ok = false;
    }
    if (close(fd) < 0) {
        ok = false;
    }
    if (ok && rename(tmp_path, dest_path) < 0) {
        ok = false;
    }
    if (!ok) {
        unlink(tmp_path);
        free(tmp_path);
        set_err(out_err, WE_WRITER_ERR_IO);
        return false;
    }
    free(tmp_path);
    set_err(out_err, WE_WRITER_OK);
    return true;
}

bool we_write_file_atomic(const char *path, const void *data, size_t size,
                          we_asset_writer_err_t *out_err)
{
    if (!path || path[0] == '\0' || (!data && size > 0u)) {
        set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
        return false;
    }

    char *tmp_path = NULL;
    int fd = open_atomic_tmp(path, &tmp_path, out_err);
    if (fd < 0) {
        return false;
    }

    bool ok = (size == 0u) || write_all_fd(fd, (const uint8_t *)data, size);
    return finish_atomic_tmp(fd, tmp_path, path, ok, out_err);
}

bool we_pkg_write_file(const char *out_path, const we_pkg_write_entry_t *entries,
                       size_t entry_count, we_asset_writer_err_t *out_err)
{
    if (!out_path || out_path[0] == '\0') {
        set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
        return false;
    }

    pkg_prep_entry_t *items = NULL;
    uint64_t dir_size = 0;
    uint64_t total_size = 0;
    if (!prepare_pkg_entries(entries, entry_count, &items, &dir_size, &total_size, out_err)) {
        return false;
    }
    (void)total_size;

    uint8_t *dir_buf = malloc((size_t)dir_size);
    if (!dir_buf) {
        free(items);
        set_err(out_err, WE_WRITER_ERR_OOM);
        return false;
    }
    write_pkg_directory(dir_buf, items, entry_count);

    char *tmp_path = NULL;
    int fd = open_atomic_tmp(out_path, &tmp_path, out_err);
    if (fd < 0) {
        free(dir_buf);
        free(items);
        return false;
    }

    bool ok = write_all_fd(fd, dir_buf, (size_t)dir_size);
    free(dir_buf);

    for (size_t i = 0; ok && i < entry_count; i++) {
        if (items[i].size > 0u) {
            ok = write_all_fd(fd, items[i].data, items[i].size);
        }
    }

    free(items);
    return finish_atomic_tmp(fd, tmp_path, out_path, ok, out_err);
}
