// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/we_vdf.h"
#include "waywal/toml_parser.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define VDF_HAVE_X86_SIMD 1
#else
#define VDF_HAVE_X86_SIMD 0
#endif

#define VDF_SLAB_DEFAULT_BYTES (256u * 1024u)
#define VDF_ARENA_MAX_BYTES    (512ULL * 1024ULL * 1024ULL)
#define VDF_MAX_FILE_BYTES     (128ULL * 1024ULL * 1024ULL)

typedef struct vdf_slab {
    struct vdf_slab *next;
    size_t cap;
    size_t used;
    _Alignas(max_align_t) uint8_t data[];
} vdf_slab_t;

typedef struct {
    vdf_slab_t *head;
    vdf_slab_t *curr;
    size_t total_bytes;
} vdf_arena_ctx_t;

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    vdf_arena_ctx_t *arena;
    int depth;
} vdf_parser_t;

static vdf_slab_t *vdf_slab_create(vdf_arena_ctx_t *ctx, size_t min_cap)
{
    size_t cap = VDF_SLAB_DEFAULT_BYTES;
    if (min_cap > cap) {
        cap = min_cap;
    }
    if (cap > VDF_ARENA_MAX_BYTES || ctx->total_bytes > VDF_ARENA_MAX_BYTES - cap ||
        cap > SIZE_MAX - sizeof(vdf_slab_t)) {
        return NULL;
    }

    vdf_slab_t *slab = (vdf_slab_t *)malloc(sizeof(vdf_slab_t) + cap);
    if (!slab) {
        return NULL;
    }
    slab->next = NULL;
    slab->cap = cap;
    slab->used = 0;
    ctx->total_bytes += cap;
    return slab;
}

static void *vdf_arena_alloc(vdf_arena_ctx_t *ctx, size_t size, size_t alignment)
{
    if (!ctx || size == 0) {
        return NULL;
    }
    if (alignment == 0 || (alignment & (alignment - 1u)) != 0) {
        alignment = _Alignof(void *);
    }
    if (size > SIZE_MAX - alignment) {
        return NULL;
    }

    vdf_slab_t *curr = ctx->curr;
    if (curr) {
        uintptr_t base = (uintptr_t)curr->data;
        uintptr_t aligned = (base + curr->used + (alignment - 1u)) & ~(uintptr_t)(alignment - 1u);
        size_t aligned_off = (size_t)(aligned - base);
        if (aligned_off <= curr->cap && size <= curr->cap - aligned_off) {
            curr->used = aligned_off + size;
            return (void *)aligned;
        }
    }

    size_t min_needed = size + (alignment - 1u);
    vdf_slab_t *slab = vdf_slab_create(ctx, min_needed);
    if (!slab) {
        return NULL;
    }

    /* Keep partially filled 256 KiB active slab when servicing a large child-pointer array */
    if (curr && min_needed > VDF_SLAB_DEFAULT_BYTES && curr->cap - curr->used >= 4096u) {
        slab->next = ctx->head;
        ctx->head = slab;
    } else {
        slab->next = ctx->head;
        ctx->head = slab;
        ctx->curr = slab;
    }

    uintptr_t base = (uintptr_t)slab->data;
    uintptr_t aligned = (base + (alignment - 1u)) & ~(uintptr_t)(alignment - 1u);
    slab->used = (size_t)(aligned - base) + size;
    return (void *)aligned;
}

static void vdf_arena_destroy(vdf_arena_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    vdf_slab_t *slab = ctx->head;
    while (slab) {
        vdf_slab_t *next = slab->next;
        free(slab);
        slab = next;
    }
    ctx->head = NULL;
    ctx->curr = NULL;
    ctx->total_bytes = 0;
}

#if VDF_HAVE_X86_SIMD
static int g_vdf_has_avx2 = -1;

static inline bool vdf_cpu_has_avx2(void)
{
    int v = __atomic_load_n(&g_vdf_has_avx2, __ATOMIC_RELAXED);
    if (__builtin_expect(v < 0, 0)) {
        v = __builtin_cpu_supports("avx2") ? 1 : 0;
        __atomic_store_n(&g_vdf_has_avx2, v, __ATOMIC_RELAXED);
    }
    return v != 0;
}

__attribute__((target("avx2"))) static size_t vdf_skip_ws_avx2(const char *src, size_t pos,
                                                               size_t len)
{
    const __m256i v_sp = _mm256_set1_epi8(' ');
    const __m256i v_lo = _mm256_set1_epi8(8);
    const __m256i v_hi = _mm256_set1_epi8(14);
    while (pos + 32u <= len) {
        __m256i chunk = _mm256_loadu_si256((const __m256i *)(const void *)(src + pos));
        __m256i ctrl_ws =
            _mm256_and_si256(_mm256_cmpgt_epi8(chunk, v_lo), _mm256_cmpgt_epi8(v_hi, chunk));
        __m256i ws = _mm256_or_si256(_mm256_cmpeq_epi8(chunk, v_sp), ctrl_ws);
        uint32_t non_ws = ~(uint32_t)_mm256_movemask_epi8(ws);
        if (non_ws != 0u) {
            return pos + (size_t)__builtin_ctz(non_ws);
        }
        pos += 32u;
    }
    return pos;
}

__attribute__((target("avx2"))) static size_t vdf_scan_quote_or_esc_avx2(const char *start,
                                                                         size_t rem)
{
    size_t i = 0u;
    const __m256i v_q = _mm256_set1_epi8('"');
    const __m256i v_bs = _mm256_set1_epi8('\\');
    while (i + 32u <= rem) {
        __m256i chunk = _mm256_loadu_si256((const __m256i *)(const void *)(start + i));
        __m256i hit =
            _mm256_or_si256(_mm256_cmpeq_epi8(chunk, v_q), _mm256_cmpeq_epi8(chunk, v_bs));
        uint32_t m = (uint32_t)_mm256_movemask_epi8(hit);
        if (m != 0u) {
            return i + (size_t)__builtin_ctz(m);
        }
        i += 32u;
    }
    return i;
}
#endif

static void skip_whitespace_and_comments(vdf_parser_t *p)
{
    const char *src = p->src;
    size_t len = p->len;
    size_t pos = p->pos;
#if VDF_HAVE_X86_SIMD
    bool use_avx2 = (len - pos >= 32u) && vdf_cpu_has_avx2();
#endif

    while (pos < len) {
#if VDF_HAVE_X86_SIMD
        if (use_avx2 && pos + 32u <= len) {
            pos = vdf_skip_ws_avx2(src, pos, len);
            if (pos >= len) {
                break;
            }
        }
#endif
        unsigned char c = (unsigned char)src[pos];
        if (c <= ' ' &&
            (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f')) {
            pos++;
            continue;
        }
        if (c == '/' && pos + 1u < len && src[pos + 1u] == '/') {
            pos += 2u;
            const char *nl = (const char *)memchr(src + pos, '\n', len - pos);
            if (nl) {
                pos = (size_t)(nl - src) + 1u;
            } else {
                pos = len;
            }
            continue;
        }
        break;
    }
    p->pos = pos;
}

static void skip_quoted_string(vdf_parser_t *p)
{
    if (p->pos >= p->len || p->src[p->pos] != '"') {
        return;
    }
    p->pos++;
#if VDF_HAVE_X86_SIMD
    bool use_avx2 = (p->len - p->pos >= 32u) && vdf_cpu_has_avx2();
#endif
    while (p->pos < p->len) {
#if VDF_HAVE_X86_SIMD
        if (use_avx2 && p->pos + 32u <= p->len) {
            p->pos += vdf_scan_quote_or_esc_avx2(p->src + p->pos, p->len - p->pos);
            if (p->pos >= p->len) {
                break;
            }
        }
#endif
        char c = p->src[p->pos++];
        if (c == '"') {
            break;
        }
        if (c == '\\' && p->pos < p->len) {
            p->pos++;
        }
    }
}

static bool parse_hex4(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)s[i];
        uint32_t d;
        if (c >= '0' && c <= '9') {
            d = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = (uint32_t)(c - 'A' + 10);
        } else {
            return false;
        }
        v = (v << 4u) | d;
    }
    *out = v;
    return true;
}

static size_t encode_utf8_codepoint(uint32_t cp, char *dst)
{
    if (cp <= 0x7Fu) {
        dst[0] = (char)cp;
        return 1u;
    }
    if (cp <= 0x7FFu) {
        dst[0] = (char)(0xC0u | (cp >> 6u));
        dst[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2u;
    }
    if (cp >= 0xD800u && cp <= 0xDFFFu) {
        cp = 0xFFFDu;
    }
    if (cp <= 0xFFFFu) {
        dst[0] = (char)(0xE0u | (cp >> 12u));
        dst[1] = (char)(0x80u | ((cp >> 6u) & 0x3Fu));
        dst[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3u;
    }
    if (cp <= 0x10FFFFu) {
        dst[0] = (char)(0xF0u | (cp >> 18u));
        dst[1] = (char)(0x80u | ((cp >> 12u) & 0x3Fu));
        dst[2] = (char)(0x80u | ((cp >> 6u) & 0x3Fu));
        dst[3] = (char)(0x80u | (cp & 0x3Fu));
        return 4u;
    }
    return encode_utf8_codepoint(0xFFFDu, dst);
}

static char *parse_quoted_string(vdf_parser_t *p)
{
    if (p->pos >= p->len || p->src[p->pos] != '"') {
        return NULL;
    }
    p->pos++;

    const char *start = p->src + p->pos;
    size_t rem = p->len - p->pos;

    /* Fast path for common unescaped VDF keys and values */
    size_t i = 0u;
#if VDF_HAVE_X86_SIMD
    if (rem >= 32u && vdf_cpu_has_avx2()) {
        i = vdf_scan_quote_or_esc_avx2(start, rem);
    }
#endif
    for (; i < rem; i++) {
        char c = start[i];
        if (c == '"') {
            char *dest = (char *)vdf_arena_alloc(p->arena, i + 1u, 1u);
            if (!dest) {
                return NULL;
            }
            if (i > 0) {
                memcpy(dest, start, i);
            }
            dest[i] = '\0';
            p->pos += i + 1u;
            return dest;
        }
        if (c == '\\') {
            break;
        }
    }

    size_t scan_pos = p->pos;
    while (scan_pos < p->len) {
        char c = p->src[scan_pos++];
        if (c == '"') {
            break;
        }
        if (c == '\\' && scan_pos < p->len) {
            scan_pos++;
        }
    }

    size_t token_len = scan_pos - p->pos;
    char *dest = (char *)vdf_arena_alloc(p->arena, token_len + 4u, 1u);
    if (!dest) {
        return NULL;
    }

    size_t out_len = 0;
    while (p->pos < p->len) {
        char c = p->src[p->pos++];
        if (c == '"') {
            break;
        }
        if (c == '\\') {
            if (p->pos >= p->len) {
                dest[out_len++] = '\\';
                break;
            }
            char next = p->src[p->pos++];
            switch (next) {
            case '"':
                dest[out_len++] = '"';
                break;
            case '\\':
                dest[out_len++] = '\\';
                break;
            case '/':
                dest[out_len++] = '/';
                break;
            case 'n':
                dest[out_len++] = '\n';
                break;
            case 'r':
                dest[out_len++] = '\r';
                break;
            case 't':
                dest[out_len++] = '\t';
                break;
            case 'b':
                dest[out_len++] = '\b';
                break;
            case 'f':
                dest[out_len++] = '\f';
                break;
            case 'u': {
                uint32_t hi = 0;
                if (p->pos + 4u <= p->len && parse_hex4(p->src + p->pos, &hi)) {
                    p->pos += 4u;
                    uint32_t cp = hi;
                    if (hi >= 0xD800u && hi <= 0xDBFFu) {
                        uint32_t lo = 0;
                        if (p->pos + 6u <= p->len && p->src[p->pos] == '\\' &&
                            p->src[p->pos + 1u] == 'u' && parse_hex4(p->src + p->pos + 2u, &lo) &&
                            lo >= 0xDC00u && lo <= 0xDFFFu) {
                            cp = 0x10000u + (((hi - 0xD800u) << 10u) | (lo - 0xDC00u));
                            p->pos += 6u;
                        } else {
                            cp = 0xFFFDu;
                        }
                    } else if (hi >= 0xDC00u && hi <= 0xDFFFu) {
                        cp = 0xFFFDu;
                    }
                    out_len += encode_utf8_codepoint(cp, dest + out_len);
                } else {
                    dest[out_len++] = 'u';
                }
                break;
            }
            default:
                dest[out_len++] = next;
                break;
            }
        } else {
            dest[out_len++] = c;
        }
    }
    dest[out_len] = '\0';
    return dest;
}

static char *parse_bare_string(vdf_parser_t *p)
{
    size_t start = p->pos;
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f' || c == '{' ||
            c == '}' || c == '"') {
            break;
        }
        if (c == '/' && p->pos + 1u < p->len && p->src[p->pos + 1u] == '/') {
            break;
        }
        p->pos++;
    }

    size_t count = p->pos - start;
    if (count == 0) {
        return NULL;
    }

    char *dest = (char *)vdf_arena_alloc(p->arena, count + 1u, 1u);
    if (!dest) {
        return NULL;
    }
    memcpy(dest, &p->src[start], count);
    dest[count] = '\0';
    return dest;
}

static char *parse_token_string(vdf_parser_t *p)
{
    skip_whitespace_and_comments(p);
    if (p->pos >= p->len) {
        return NULL;
    }
    if (p->src[p->pos] == '"') {
        return parse_quoted_string(p);
    }
    if (p->src[p->pos] == '{' || p->src[p->pos] == '}') {
        return NULL;
    }
    return parse_bare_string(p);
}

static bool vdf_object_add_child(vdf_arena_ctx_t *arena, waywal_vdf_object_t *obj,
                                 waywal_vdf_node_t *child)
{
    if (obj->child_count >= obj->child_capacity) {
        size_t new_cap = obj->child_capacity == 0 ? 4u : obj->child_capacity * 2u;
        if (new_cap > SIZE_MAX / sizeof(waywal_vdf_node_t *)) {
            return false;
        }
        waywal_vdf_node_t **new_children = (waywal_vdf_node_t **)vdf_arena_alloc(
            arena, new_cap * sizeof(waywal_vdf_node_t *), _Alignof(void *));
        if (!new_children) {
            return false;
        }
        if (obj->child_count > 0 && obj->children) {
            memcpy(new_children, obj->children, obj->child_count * sizeof(waywal_vdf_node_t *));
        }
        obj->children = new_children;
        obj->child_capacity = new_cap;
    }
    obj->children[obj->child_count++] = child;
    return true;
}

static waywal_vdf_node_t *alloc_vdf_node(vdf_arena_ctx_t *arena, char *key, waywal_vdf_type_t type)
{
    waywal_vdf_node_t *node = (waywal_vdf_node_t *)vdf_arena_alloc(arena, sizeof(waywal_vdf_node_t),
                                                                   _Alignof(waywal_vdf_node_t));
    if (!node) {
        return NULL;
    }
    node->key = key;
    node->type = type;
    if (type == VDF_NODE_OBJECT) {
        node->object.children = NULL;
        node->object.child_count = 0;
        node->object.child_capacity = 0;
    } else {
        node->string_value = "";
    }
    return node;
}

static bool parse_object_children(vdf_parser_t *p, waywal_vdf_node_t *parent_node)
{
    while (p->pos < p->len) {
        skip_whitespace_and_comments(p);
        if (p->pos >= p->len || p->src[p->pos] == '}') {
            break;
        }

        char *key = parse_token_string(p);
        if (!key) {
            if (p->pos < p->len && p->src[p->pos] == '}') {
                break;
            }
            p->pos++;
            continue;
        }

        skip_whitespace_and_comments(p);
        if (p->pos >= p->len) {
            waywal_vdf_node_t *node = alloc_vdf_node(p->arena, key, VDF_NODE_STRING);
            if (!node || !vdf_object_add_child(p->arena, &parent_node->object, node)) {
                return false;
            }
            break;
        }

        if (p->src[p->pos] == '{') {
            p->pos++;
            waywal_vdf_node_t *obj_node = alloc_vdf_node(p->arena, key, VDF_NODE_OBJECT);
            if (!obj_node) {
                return false;
            }

            if (p->depth < 64) {
                p->depth++;
                if (!parse_object_children(p, obj_node)) {
                    p->depth--;
                    return false;
                }
                p->depth--;
                skip_whitespace_and_comments(p);
                if (p->pos < p->len && p->src[p->pos] == '}') {
                    p->pos++;
                }
            } else {
                int brace_count = 1;
                while (p->pos < p->len && brace_count > 0) {
                    skip_whitespace_and_comments(p);
                    if (p->pos >= p->len) {
                        break;
                    }
                    if (p->src[p->pos] == '{') {
                        brace_count++;
                        p->pos++;
                    } else if (p->src[p->pos] == '}') {
                        brace_count--;
                        p->pos++;
                    } else if (p->src[p->pos] == '"') {
                        skip_quoted_string(p);
                    } else {
                        p->pos++;
                    }
                }
            }

            if (!vdf_object_add_child(p->arena, &parent_node->object, obj_node)) {
                return false;
            }
        } else if (p->src[p->pos] == '}') {
            waywal_vdf_node_t *node = alloc_vdf_node(p->arena, key, VDF_NODE_STRING);
            if (!node || !vdf_object_add_child(p->arena, &parent_node->object, node)) {
                return false;
            }
            break;
        } else {
            char *value = parse_token_string(p);
            if (!value) {
                value = "";
            }
            waywal_vdf_node_t *str_node = alloc_vdf_node(p->arena, key, VDF_NODE_STRING);
            if (!str_node) {
                return false;
            }
            str_node->string_value = value;
            if (!vdf_object_add_child(p->arena, &parent_node->object, str_node)) {
                return false;
            }
        }
    }
    return true;
}

waywal_vdf_doc_t *waywal_vdf_parse(const char *text, size_t length)
{
    if (!text) {
        return NULL;
    }

    waywal_vdf_doc_t *doc = (waywal_vdf_doc_t *)calloc(1, sizeof(waywal_vdf_doc_t));
    if (!doc) {
        return NULL;
    }

    vdf_arena_ctx_t *ctx = (vdf_arena_ctx_t *)calloc(1, sizeof(vdf_arena_ctx_t));
    if (!ctx) {
        free(doc);
        return NULL;
    }

    vdf_slab_t *first_slab = vdf_slab_create(ctx, VDF_SLAB_DEFAULT_BYTES);
    if (!first_slab) {
        free(ctx);
        free(doc);
        return NULL;
    }
    ctx->head = first_slab;
    ctx->curr = first_slab;
    doc->arena_ctx = ctx;

    waywal_vdf_node_t *container = alloc_vdf_node(ctx, "", VDF_NODE_OBJECT);
    if (!container) {
        waywal_vdf_free(doc);
        return NULL;
    }

    vdf_parser_t parser = {
        .src = text,
        .len = length,
        .pos = 0,
        .arena = ctx,
        .depth = 0,
    };

    if (!parse_object_children(&parser, container)) {
        waywal_vdf_free(doc);
        return NULL;
    }

    if (container->object.child_count == 0) {
        doc->root = NULL;
    } else if (container->object.child_count == 1) {
        doc->root = container->object.children[0];
    } else {
        doc->root = container;
    }

    return doc;
}

void waywal_vdf_free(waywal_vdf_doc_t *doc)
{
    if (!doc) {
        return;
    }
    if (doc->arena_ctx) {
        vdf_arena_ctx_t *ctx = (vdf_arena_ctx_t *)doc->arena_ctx;
        vdf_arena_destroy(ctx);
        free(ctx);
    }
    free(doc);
}

const waywal_vdf_node_t *waywal_vdf_find_child(const waywal_vdf_node_t *parent, const char *key)
{
    if (!parent || !key) {
        return NULL;
    }

    if (parent->type == VDF_NODE_OBJECT && parent->object.children) {
        for (size_t i = 0; i < parent->object.child_count; i++) {
            const waywal_vdf_node_t *child = parent->object.children[i];
            if (child && child->key && strcasecmp(child->key, key) == 0) {
                return child;
            }
        }
    }

    return NULL;
}

const char *waywal_vdf_get_string(const waywal_vdf_node_t *parent, const char *key,
                                  const char *fallback)
{
    if (!parent || !key) {
        return fallback;
    }

    const waywal_vdf_node_t *child = waywal_vdf_find_child(parent, key);
    if (child && child->type == VDF_NODE_STRING && child->string_value) {
        return child->string_value;
    }
    return fallback;
}

static char *read_entire_file_local(const char *path, size_t *out_len)
{
    if (!path || !out_len) {
        return NULL;
    }
    *out_len = 0;

    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > VDF_MAX_FILE_BYTES) {
        close(fd);
        return NULL;
    }

    size_t sz = (size_t)st.st_size;
    char *buf = (char *)malloc(sz + 1u);
    if (!buf) {
        close(fd);
        return NULL;
    }

    size_t total_rd = 0u;
    bool io_err = false;
    while (total_rd < sz) {
        ssize_t n = read(fd, buf + total_rd, sz - total_rd);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            io_err = true;
            break;
        }
        if (n == 0) {
            break;
        }
        total_rd += (size_t)n;
    }
    close(fd);

    if (io_err || total_rd != sz) {
        free(buf);
        return NULL;
    }

    buf[total_rd] = '\0';
    *out_len = total_rd;
    return buf;
}

static bool check_workshop_content_on_disk(const char *base_path)
{
    char probe[4096];
    struct stat st;

    if (snprintf(probe, sizeof(probe), "%s/steamapps/workshop/content/431960", base_path) <
            (int)sizeof(probe) &&
        stat(probe, &st) == 0 && S_ISDIR(st.st_mode)) {
        return true;
    }
    if (snprintf(probe, sizeof(probe), "%s/workshop/content/431960", base_path) <
            (int)sizeof(probe) &&
        stat(probe, &st) == 0 && S_ISDIR(st.st_mode)) {
        return true;
    }
    if (snprintf(probe, sizeof(probe), "%s/431960", base_path) < (int)sizeof(probe) &&
        stat(probe, &st) == 0 && S_ISDIR(st.st_mode)) {
        return true;
    }
    if (snprintf(probe, sizeof(probe), "%s/steamapps/common/wallpaper_engine", base_path) <
            (int)sizeof(probe) &&
        stat(probe, &st) == 0 && S_ISDIR(st.st_mode)) {
        return true;
    }
    return false;
}

static void vdf_copy_cstr(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    if (dst == src) {
        dst[cap - 1] = '\0';
        return;
    }
    size_t i = 0;
    while (i + 1 < cap && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void add_or_merge_library(waywal_steam_library_t *out_libs, size_t *inout_count,
                                 size_t max_libs, const char *raw_path, const char *label,
                                 uint64_t total_size, uint64_t app_size, bool has_we)
{
    if (!out_libs || !inout_count || *inout_count >= max_libs || !raw_path || raw_path[0] == '\0') {
        return;
    }

    char canon[4096];
    if (realpath(raw_path, canon) == NULL) {
        vdf_copy_cstr(canon, sizeof(canon), raw_path);
    }

    size_t clen = strlen(canon);
    while (clen > 1 && canon[clen - 1] == '/') {
        canon[--clen] = '\0';
    }

    if (!has_we) {
        has_we = check_workshop_content_on_disk(canon);
    }

    for (size_t i = 0; i < *inout_count; i++) {
        if (strcmp(out_libs[i].path, canon) == 0) {
            if (has_we) {
                out_libs[i].has_we_app = true;
            }
            if (app_size > out_libs[i].app_size_bytes) {
                out_libs[i].app_size_bytes = app_size;
            }
            if (total_size > out_libs[i].total_size_bytes) {
                out_libs[i].total_size_bytes = total_size;
            }
            if (label && label[0] != '\0' && out_libs[i].label[0] == '\0') {
                vdf_copy_cstr(out_libs[i].label, sizeof(out_libs[i].label), label);
            }
            return;
        }
    }

    waywal_steam_library_t *lib = &out_libs[*inout_count];
    memset(lib, 0, sizeof(*lib));
    vdf_copy_cstr(lib->path, sizeof(lib->path), canon);
    if (label && label[0] != '\0') {
        vdf_copy_cstr(lib->label, sizeof(lib->label), label);
    }
    lib->total_size_bytes = total_size;
    lib->app_size_bytes = app_size;
    lib->has_we_app = has_we;
    (*inout_count)++;
}

static void parse_libraryfolders_vdf(const char *vdf_path, waywal_steam_library_t *out_libs,
                                     size_t *inout_count, size_t max_libs)
{
    size_t file_len = 0;
    char *buf = read_entire_file_local(vdf_path, &file_len);
    if (!buf) {
        return;
    }

    waywal_vdf_doc_t *doc = waywal_vdf_parse(buf, file_len);
    free(buf);
    if (!doc || !doc->root) {
        if (doc) {
            waywal_vdf_free(doc);
        }
        return;
    }

    const waywal_vdf_node_t *lf = waywal_vdf_find_child(doc->root, "libraryfolders");
    if (!lf) {
        lf = doc->root;
    }

    if (lf && lf->type == VDF_NODE_OBJECT && lf->object.children) {
        for (size_t i = 0; i < lf->object.child_count && *inout_count < max_libs; i++) {
            const waywal_vdf_node_t *item = lf->object.children[i];
            if (!item) {
                continue;
            }

            if (item->type == VDF_NODE_OBJECT) {
                const char *path = waywal_vdf_get_string(item, "path", NULL);
                if (!path || path[0] == '\0') {
                    path = item->key;
                }
                const char *label = waywal_vdf_get_string(item, "label", "");
                const char *totalsize_str = waywal_vdf_get_string(item, "totalsize", NULL);
                if (!totalsize_str) {
                    totalsize_str = waywal_vdf_get_string(item, "total_size", "0");
                }
                uint64_t total_size = strtoull(totalsize_str, NULL, 10);

                bool has_we = false;
                uint64_t app_size = 0;

                const waywal_vdf_node_t *apps = waywal_vdf_find_child(item, "apps");
                if (apps && apps->type == VDF_NODE_OBJECT) {
                    const waywal_vdf_node_t *app_entry = waywal_vdf_find_child(apps, "431960");
                    if (app_entry) {
                        has_we = true;
                        if (app_entry->type == VDF_NODE_STRING && app_entry->string_value) {
                            app_size = strtoull(app_entry->string_value, NULL, 10);
                        }
                    }
                }

                add_or_merge_library(out_libs, inout_count, max_libs, path, label, total_size,
                                     app_size, has_we);
            } else if (item->type == VDF_NODE_STRING && item->string_value) {
                /* Format v1: "1" "/mnt/storage/SteamLibrary" */
                const char *path = item->string_value;
                const char *label = item->key;
                add_or_merge_library(out_libs, inout_count, max_libs, path, label, 0, 0, false);
            }
        }
    }

    waywal_vdf_free(doc);
}

static void probe_candidate_steam_root(const char *steam_root, waywal_steam_library_t *out_libs,
                                       size_t *inout_count, size_t max_libs)
{
    if (!steam_root || steam_root[0] == '\0' || *inout_count >= max_libs) {
        return;
    }

    char vdf_path[4096];
    if (snprintf(vdf_path, sizeof(vdf_path), "%s/steamapps/libraryfolders.vdf", steam_root) <
        (int)sizeof(vdf_path)) {
        struct stat st;
        if (stat(vdf_path, &st) == 0 && S_ISREG(st.st_mode)) {
            parse_libraryfolders_vdf(vdf_path, out_libs, inout_count, max_libs);
        }
    }

    /* Also check <steam_root>/libraryfolders.vdf */
    if (snprintf(vdf_path, sizeof(vdf_path), "%s/libraryfolders.vdf", steam_root) <
        (int)sizeof(vdf_path)) {
        struct stat st;
        if (stat(vdf_path, &st) == 0 && S_ISREG(st.st_mode)) {
            parse_libraryfolders_vdf(vdf_path, out_libs, inout_count, max_libs);
        }
    }

    /* Also record steam_root itself if it exists */
    struct stat root_st;
    if (stat(steam_root, &root_st) == 0 && S_ISDIR(root_st.st_mode)) {
        add_or_merge_library(out_libs, inout_count, max_libs, steam_root, "", 0, 0, false);
    }
}

static void probe_colon_separated_roots(const char *path_list, waywal_steam_library_t *out_libs,
                                        size_t *inout_count, size_t max_libs)
{
    if (!path_list || path_list[0] == '\0' || !out_libs || !inout_count) {
        return;
    }

    const char *p = path_list;
    while (*p && *inout_count < max_libs) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        if (len > 0 && len < 4096) {
            char candidate[4096];
            memcpy(candidate, p, len);
            candidate[len] = '\0';

            char *start = candidate;
            while (*start && isspace((unsigned char)*start)) {
                start++;
            }
            char *end = start + strlen(start);
            while (end > start && isspace((unsigned char)*(end - 1))) {
                end--;
                *end = '\0';
            }

            if (start[0] != '\0') {
                probe_candidate_steam_root(start, out_libs, inout_count, max_libs);
                struct stat st;
                if (stat(start, &st) == 0 && S_ISDIR(st.st_mode)) {
                    add_or_merge_library(out_libs, inout_count, max_libs, start, "custom", 0, 0,
                                         true);
                }
            }
        }
        if (!colon) {
            break;
        }
        p = colon + 1;
    }
}

static void parse_workshop_dir_from_toml(const char *toml_path, char *out_dir, size_t max_len)
{
    if (!toml_path || !out_dir || max_len == 0) {
        return;
    }
    out_dir[0] = '\0';

    int fd = open(toml_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > VDF_MAX_FILE_BYTES) {
        close(fd);
        return;
    }

    FILE *fp = fdopen(fd, "r");
    if (!fp) {
        close(fd);
        return;
    }

    char *line = NULL;
    size_t line_cap = 0;
    while (getline(&line, &line_cap, fp) >= 0) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '#' || *p == ';') {
            continue;
        }
        if (strncmp(p, "workshop_dir", 12) == 0) {
            p += 12;
            while (*p && isspace((unsigned char)*p)) {
                p++;
            }
            if (*p == '=') {
                p++;
                while (*p && isspace((unsigned char)*p)) {
                    p++;
                }
                char quote = *p;
                if (quote == '"' || quote == '\'') {
                    p++;
                    char *end_q = strchr(p, quote);
                    if (end_q) {
                        size_t val_len = (size_t)(end_q - p);
                        if (val_len < max_len) {
                            memcpy(out_dir, p, val_len);
                            out_dir[val_len] = '\0';
                        }
                    }
                }
                break;
            }
        }
    }
    free(line);
    fclose(fp);
}

size_t waywal_steam_discover_libraries(waywal_steam_library_t *out_libs, size_t max_libs)
{
    if (!out_libs || max_libs == 0) {
        return 0;
    }

    size_t count = 0;

    /* 1. Explicit Environment Variable: $WWAL_WORKSHOP_DIR */
    const char *env_ws = getenv("WWAL_WORKSHOP_DIR");
    if (env_ws && env_ws[0] != '\0') {
        probe_colon_separated_roots(env_ws, out_libs, &count, max_libs);
    }

    /* 2. Application Configuration: ~/.config/waywal/config.toml, ~/.config/wwal/config.toml */
    waywal_config_t cfg;
    if (waywal_config_load_default(&cfg) && cfg.daemon.workshop_dir[0] != '\0') {
        probe_colon_separated_roots(cfg.daemon.workshop_dir, out_libs, &count, max_libs);
    }

    char pw_stack_buf[4096];
    char *pw_heap_buf = NULL;
    struct passwd pw_entry;
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') {
        struct passwd *pw_res = NULL;
        int rc = getpwuid_r(getuid(), &pw_entry, pw_stack_buf, sizeof(pw_stack_buf), &pw_res);
        if (rc == ERANGE) {
            long sys_max = sysconf(_SC_GETPW_R_SIZE_MAX);
            size_t dyn_sz =
                (sys_max > 0 && (size_t)sys_max > sizeof(pw_stack_buf)) ? (size_t)sys_max : 16384u;
            pw_heap_buf = (char *)malloc(dyn_sz);
            if (pw_heap_buf) {
                rc = getpwuid_r(getuid(), &pw_entry, pw_heap_buf, dyn_sz, &pw_res);
            }
        }
        if (rc == 0 && pw_res && pw_res->pw_dir && pw_res->pw_dir[0] != '\0') {
            home = pw_res->pw_dir;
        }
    }

    if (home && home[0] != '\0') {
        char custom_cfg[4096];
        char parsed_dir[4096];

        if (snprintf(custom_cfg, sizeof(custom_cfg), "%s/.config/waywal/config.toml", home) <
            (int)sizeof(custom_cfg)) {
            parse_workshop_dir_from_toml(custom_cfg, parsed_dir, sizeof(parsed_dir));
            if (parsed_dir[0] != '\0') {
                probe_colon_separated_roots(parsed_dir, out_libs, &count, max_libs);
            }
        }

        if (snprintf(custom_cfg, sizeof(custom_cfg), "%s/.config/wwal/config.toml", home) <
            (int)sizeof(custom_cfg)) {
            parse_workshop_dir_from_toml(custom_cfg, parsed_dir, sizeof(parsed_dir));
            if (parsed_dir[0] != '\0') {
                probe_colon_separated_roots(parsed_dir, out_libs, &count, max_libs);
            }
        }

        /* 3. Native Linux Steam: ~/.local/share/Steam and ~/.steam/steam, ~/.steam/root */
        static const char *const native_steam_rel[] = {
            ".local/share/Steam",
            ".steam/steam",
            ".steam/root",
        };
        for (size_t i = 0; i < sizeof(native_steam_rel) / sizeof(native_steam_rel[0]); i++) {
            char root_buf[4096];
            if (snprintf(root_buf, sizeof(root_buf), "%s/%s", home, native_steam_rel[i]) <
                (int)sizeof(root_buf)) {
                probe_candidate_steam_root(root_buf, out_libs, &count, max_libs);
            }
        }

        /* 4. Sandboxed Flatpak Steam */
        static const char *const flatpak_steam_rel[] = {
            ".var/app/com.valvesoftware.Steam/.local/share/Steam",
            ".var/app/com.valvesoftware.Steam/.steam/steam",
        };
        for (size_t i = 0; i < sizeof(flatpak_steam_rel) / sizeof(flatpak_steam_rel[0]); i++) {
            char root_buf[4096];
            if (snprintf(root_buf, sizeof(root_buf), "%s/%s", home, flatpak_steam_rel[i]) <
                (int)sizeof(root_buf)) {
                probe_candidate_steam_root(root_buf, out_libs, &count, max_libs);
            }
        }

        /* 5. Sandboxed Snap Steam */
        static const char *const snap_steam_rel[] = {
            "snap/steam/common/.local/share/Steam",
        };
        for (size_t i = 0; i < sizeof(snap_steam_rel) / sizeof(snap_steam_rel[0]); i++) {
            char root_buf[4096];
            if (snprintf(root_buf, sizeof(root_buf), "%s/%s", home, snap_steam_rel[i]) <
                (int)sizeof(root_buf)) {
                probe_candidate_steam_root(root_buf, out_libs, &count, max_libs);
            }
        }
    }

    free(pw_heap_buf);
    return count;
}
