// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/we_appworkshop.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(_OPENMP)
#include <omp.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#define WS_ASAN_POISON(addr, sz)   ASAN_POISON_MEMORY_REGION((addr), (sz))
#define WS_ASAN_UNPOISON(addr, sz) ASAN_UNPOISON_MEMORY_REGION((addr), (sz))
#else
#define WS_ASAN_POISON(addr, sz)   ((void)0)
#define WS_ASAN_UNPOISON(addr, sz) ((void)0)
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define WS_HAVE_X86_SIMD 1
#else
#define WS_HAVE_X86_SIMD 0
#endif

/* 2^64 / phi odd multiplier for multiplicative Fibonacci hashing */
#define WS_FIB_HASH_MULT 11400714819323198485ULL

#define WS_ASCII_SPACE_MASK                                                                        \
    ((1ULL << ' ') | (1ULL << '\t') | (1ULL << '\n') | (1ULL << '\r') | (1ULL << '\v') |           \
     (1ULL << '\f'))

#define WS_SWAR_LOWER 0x2020202020202020ULL

#define WS_PACK4(a, b, c, d)                                                                       \
    ((uint32_t)(unsigned char)(a) | ((uint32_t)(unsigned char)(b) << 8u) |                         \
     ((uint32_t)(unsigned char)(c) << 16u) | ((uint32_t)(unsigned char)(d) << 24u))

#define WS_PACK8(a, b, c, d, e, f, g, h)                                                           \
    ((uint64_t)WS_PACK4(a, b, c, d) | ((uint64_t)WS_PACK4(e, f, g, h) << 32u))

#define WS_MAX_PARALLEL_THREADS 8
#define WS_PARALLEL_CHUNKS      32
#define WS_ITEM_CACHE_MAX_CAP   (1u << 19)
#define WS_IO_CACHE_MAX_CAP     (32u * 1024u * 1024u)

#if defined(__GNUC__) || defined(__clang__)
#define WS_AINLINE static inline __attribute__((always_inline))
#else
#define WS_AINLINE static inline
#endif

typedef struct {
    const char *ptr;
    size_t len;
} ws_span_t;

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    char scratch[256];
} ws_vdf_cursor_t;

typedef struct {
    waywal_workshop_item_manifest_t *items;
    size_t count;
    size_t cap;
    size_t seq_hint;
    /* 1-based item indices let 0 mark an empty bucket with zero-fill initialization */
    uint32_t *slots;
    size_t ht_cap;
    unsigned ht_shift;
} ws_item_table_t;

/* Reusable workspace avoids repeated kernel VMA mmap/munmap page-table churn on >128 KiB buffers.
 * Guarded by lock-free test-and-set so concurrent callers fall back to heap allocation safely. */
static atomic_flag g_ws_scratch_lock = ATOMIC_FLAG_INIT;
static waywal_workshop_item_manifest_t *g_ws_sub_buf = NULL;
static size_t g_ws_sub_cap = 0;
static uint32_t *g_ws_slots_buf = NULL;
static size_t g_ws_slots_cap = 0;
static uint8_t *g_ws_claim_buf = NULL;
static size_t g_ws_claim_cap = 0;

static atomic_flag g_ws_item_cache_lock = ATOMIC_FLAG_INIT;
static waywal_workshop_item_manifest_t *g_ws_cached_items = NULL;
static size_t g_ws_cached_items_cap = 0;

static atomic_flag g_ws_io_lock = ATOMIC_FLAG_INIT;
static char *g_ws_io_buf = NULL;
static size_t g_ws_io_cap = 0;

__attribute__((destructor)) static void ws_global_scratch_cleanup(void)
{
    free(g_ws_sub_buf);
    g_ws_sub_buf = NULL;
    g_ws_sub_cap = 0;

    free(g_ws_slots_buf);
    g_ws_slots_buf = NULL;
    g_ws_slots_cap = 0;

    free(g_ws_claim_buf);
    g_ws_claim_buf = NULL;
    g_ws_claim_cap = 0;

    if (g_ws_cached_items) {
        WS_ASAN_UNPOISON(g_ws_cached_items,
                         g_ws_cached_items_cap * sizeof(waywal_workshop_item_manifest_t));
        free(g_ws_cached_items);
        g_ws_cached_items = NULL;
        g_ws_cached_items_cap = 0;
    }

    if (g_ws_io_buf) {
        WS_ASAN_UNPOISON(g_ws_io_buf, g_ws_io_cap);
        free(g_ws_io_buf);
        g_ws_io_buf = NULL;
        g_ws_io_cap = 0;
    }
}

static waywal_workshop_item_manifest_t *ws_alloc_items(size_t cap, bool zero_init,
                                                       size_t *out_actual_cap)
{
    if (!atomic_flag_test_and_set_explicit(&g_ws_item_cache_lock, memory_order_acquire)) {
        if (g_ws_cached_items && g_ws_cached_items_cap >= cap) {
            waywal_workshop_item_manifest_t *ptr = g_ws_cached_items;
            size_t actual_cap = g_ws_cached_items_cap;
            g_ws_cached_items = NULL;
            g_ws_cached_items_cap = 0;
            atomic_flag_clear_explicit(&g_ws_item_cache_lock, memory_order_release);
            WS_ASAN_UNPOISON(ptr, actual_cap * sizeof(waywal_workshop_item_manifest_t));
            if (zero_init) {
                memset(ptr, 0, actual_cap * sizeof(waywal_workshop_item_manifest_t));
            }
            if (out_actual_cap) {
                *out_actual_cap = actual_cap;
            }
            return ptr;
        }
        atomic_flag_clear_explicit(&g_ws_item_cache_lock, memory_order_release);
    }

    waywal_workshop_item_manifest_t *ptr = zero_init
                                               ? (waywal_workshop_item_manifest_t *)calloc(
                                                     cap, sizeof(waywal_workshop_item_manifest_t))
                                               : (waywal_workshop_item_manifest_t *)malloc(
                                                     cap * sizeof(waywal_workshop_item_manifest_t));
    if (ptr && out_actual_cap) {
        *out_actual_cap = cap;
    }
    return ptr;
}

static void ws_release_items(waywal_workshop_item_manifest_t *ptr, size_t min_known_cap)
{
    if (!ptr) {
        return;
    }
    if (min_known_cap >= 4096u && min_known_cap <= WS_ITEM_CACHE_MAX_CAP) {
        if (!atomic_flag_test_and_set_explicit(&g_ws_item_cache_lock, memory_order_acquire)) {
            if (!g_ws_cached_items) {
                WS_ASAN_POISON(ptr, min_known_cap * sizeof(waywal_workshop_item_manifest_t));
                g_ws_cached_items = ptr;
                g_ws_cached_items_cap = min_known_cap;
                atomic_flag_clear_explicit(&g_ws_item_cache_lock, memory_order_release);
                return;
            }
            atomic_flag_clear_explicit(&g_ws_item_cache_lock, memory_order_release);
        }
    }
    free(ptr);
}

WS_AINLINE bool ws_is_space(unsigned char ch)
{
    return ch <= 32u && ((WS_ASCII_SPACE_MASK >> ch) & 1ULL) != 0ULL;
}

static bool ws_table_init(ws_item_table_t *tbl, size_t estimated_items)
{
    memset(tbl, 0, sizeof(*tbl));

    size_t item_cap = estimated_items < 16u ? 16u : estimated_items;
    if (item_cap > (1u << 22)) {
        item_cap = (1u << 22);
    }

    unsigned log2_ht = 5u;
    size_t ht_cap = (size_t)1u << log2_ht;
    while (ht_cap * 3u < item_cap * 4u && log2_ht < 24u) {
        log2_ht++;
        ht_cap = (size_t)1u << log2_ht;
    }

    size_t actual_cap = item_cap;
    tbl->items = ws_alloc_items(item_cap, true, &actual_cap);
    if (!tbl->items) {
        return false;
    }
    tbl->slots = (uint32_t *)calloc(ht_cap, sizeof(uint32_t));
    if (!tbl->slots) {
        ws_release_items(tbl->items, actual_cap);
        tbl->items = NULL;
        return false;
    }

    tbl->cap = actual_cap;
    tbl->ht_cap = ht_cap;
    tbl->ht_shift = 64u - log2_ht;
    return true;
}

static void ws_table_free_all(ws_item_table_t *tbl)
{
    free(tbl->slots);
    ws_release_items(tbl->items, tbl->cap);
    memset(tbl, 0, sizeof(*tbl));
}

static bool ws_table_rehash(ws_item_table_t *tbl)
{
    if (tbl->ht_shift <= 33u || tbl->ht_cap > SIZE_MAX / (2u * sizeof(uint32_t))) {
        return false;
    }

    size_t new_ht_cap = tbl->ht_cap * 2u;
    unsigned new_shift = tbl->ht_shift - 1u;
    uint32_t *new_slots = (uint32_t *)calloc(new_ht_cap, sizeof(uint32_t));
    if (!new_slots) {
        return false;
    }

    size_t mask = new_ht_cap - 1u;
    for (size_t i = 0; i < tbl->count; i++) {
        uint64_t id = tbl->items[i].published_file_id;
        size_t idx = (size_t)((id * WS_FIB_HASH_MULT) >> new_shift);
        while (new_slots[idx] != 0u) {
            idx = (idx + 1u) & mask;
        }
        new_slots[idx] = (uint32_t)(i + 1u);
    }

    free(tbl->slots);
    tbl->slots = new_slots;
    tbl->ht_cap = new_ht_cap;
    tbl->ht_shift = new_shift;
    return true;
}

WS_AINLINE waywal_workshop_item_manifest_t *find_or_add_item(ws_item_table_t *tbl,
                                                             uint64_t published_file_id)
{
    size_t hint = tbl->seq_hint;
    if (hint < tbl->count && tbl->items[hint].published_file_id == published_file_id) {
        tbl->seq_hint = hint + 1u;
        return &tbl->items[hint];
    }

    size_t mask = tbl->ht_cap - 1u;
    size_t slot = (size_t)((published_file_id * WS_FIB_HASH_MULT) >> tbl->ht_shift);

    for (;;) {
        uint32_t entry = tbl->slots[slot];
        if (entry == 0u) {
            break;
        }
        size_t found_idx = (size_t)(entry - 1u);
        waywal_workshop_item_manifest_t *cand = &tbl->items[found_idx];
        if (cand->published_file_id == published_file_id) {
            tbl->seq_hint = found_idx + 1u;
            return cand;
        }
        slot = (slot + 1u) & mask;
    }

    if (__builtin_expect((tbl->count + 1u) * 4u > tbl->ht_cap * 3u, 0)) {
        if (!ws_table_rehash(tbl)) {
            return NULL;
        }
        mask = tbl->ht_cap - 1u;
        slot = (size_t)((published_file_id * WS_FIB_HASH_MULT) >> tbl->ht_shift);
        while (tbl->slots[slot] != 0u) {
            slot = (slot + 1u) & mask;
        }
    }

    if (__builtin_expect(tbl->count >= tbl->cap, 0)) {
        if (tbl->cap >= (size_t)UINT32_MAX ||
            tbl->cap > SIZE_MAX / (2u * sizeof(waywal_workshop_item_manifest_t))) {
            return NULL;
        }
        size_t old_cap = tbl->cap;
        size_t new_cap = old_cap * 2u;
        waywal_workshop_item_manifest_t *new_items = (waywal_workshop_item_manifest_t *)realloc(
            tbl->items, new_cap * sizeof(waywal_workshop_item_manifest_t));
        if (!new_items) {
            return NULL;
        }
        memset(new_items + old_cap, 0,
               (new_cap - old_cap) * sizeof(waywal_workshop_item_manifest_t));
        tbl->items = new_items;
        tbl->cap = new_cap;
    }

    size_t item_idx = tbl->count++;
    waywal_workshop_item_manifest_t *it = &tbl->items[item_idx];
    it->published_file_id = published_file_id;
    tbl->slots[slot] = (uint32_t)(item_idx + 1u);
    return it;
}

static inline void ws_skip_ws(ws_vdf_cursor_t *c)
{
    const char *src = c->src;
    size_t len = c->len;
    size_t pos = c->pos;

    while (pos < len) {
        unsigned char ch = (unsigned char)src[pos];
        if (ws_is_space(ch)) {
            pos++;
            continue;
        }
        if (ch == '/' && pos + 1u < len && src[pos + 1u] == '/') {
            pos += 2u;
            while (pos < len && src[pos] != '\n') {
                pos++;
            }
            if (pos < len) {
                pos++;
            }
            continue;
        }
        break;
    }
    c->pos = pos;
}

static bool ws_next_token(ws_vdf_cursor_t *c, ws_span_t *out)
{
    ws_skip_ws(c);
    if (c->pos >= c->len) {
        return false;
    }

    const char *src = c->src;
    size_t len = c->len;
    char first = src[c->pos];
    if (first == '{' || first == '}') {
        return false;
    }

    if (first == '"') {
        c->pos++;
        const char *start = src + c->pos;
        size_t rem = len - c->pos;

        for (size_t i = 0; i < rem; i++) {
            char ch = start[i];
            if (ch == '"') {
                out->ptr = start;
                out->len = i;
                c->pos += i + 1u;
                return true;
            }
            if (ch == '\\') {
                break;
            }
        }

        size_t out_len = 0;
        while (c->pos < len) {
            char ch = src[c->pos++];
            if (ch == '"') {
                break;
            }
            if (ch == '\\' && c->pos < len) {
                char esc = src[c->pos++];
                switch (esc) {
                case 'n':
                    ch = '\n';
                    break;
                case 'r':
                    ch = '\r';
                    break;
                case 't':
                    ch = '\t';
                    break;
                default:
                    ch = esc;
                    break;
                }
            }
            if (out_len + 1u < sizeof(c->scratch)) {
                c->scratch[out_len++] = ch;
            }
        }
        c->scratch[out_len] = '\0';
        out->ptr = c->scratch;
        out->len = out_len;
        return true;
    }

    size_t start_pos = c->pos;
    while (c->pos < len) {
        unsigned char ch = (unsigned char)src[c->pos];
        if (ws_is_space(ch) || ch == '{' || ch == '}' || ch == '"') {
            break;
        }
        if (ch == '/' && c->pos + 1u < len && src[c->pos + 1u] == '/') {
            break;
        }
        c->pos++;
    }

    out->ptr = src + start_pos;
    out->len = c->pos - start_pos;
    return out->len > 0u;
}

static void ws_skip_block_after_lbrace(ws_vdf_cursor_t *c)
{
    size_t depth = 1u;
    const char *src = c->src;
    size_t len = c->len;

    while (c->pos < len && depth > 0u) {
        ws_skip_ws(c);
        if (c->pos >= len) {
            break;
        }
        char ch = src[c->pos];
        if (ch == '{') {
            depth++;
            c->pos++;
        } else if (ch == '}') {
            depth--;
            c->pos++;
        } else {
            ws_span_t dummy;
            if (!ws_next_token(c, &dummy)) {
                c->pos++;
            }
        }
    }
}

static void ws_skip_value_or_block(ws_vdf_cursor_t *c)
{
    ws_skip_ws(c);
    if (c->pos >= c->len) {
        return;
    }
    if (c->src[c->pos] == '{') {
        c->pos++;
        ws_skip_block_after_lbrace(c);
    } else if (c->src[c->pos] != '}') {
        ws_span_t dummy;
        (void)ws_next_token(c, &dummy);
    }
}

static inline bool ws_span_ieq(ws_span_t s, const char *lower_lit, size_t lit_len)
{
    if (s.len != lit_len) {
        return false;
    }
    for (size_t i = 0; i < lit_len; i++) {
        if (((unsigned char)s.ptr[i] | 0x20u) != (unsigned char)lower_lit[i]) {
            return false;
        }
    }
    return true;
}

static inline uint64_t ws_parse_u64(ws_span_t s)
{
    size_t i = 0;
    while (i < s.len && ws_is_space((unsigned char)s.ptr[i])) {
        i++;
    }
    if (i < s.len && s.ptr[i] == '+') {
        i++;
    }
    uint64_t val = 0;
    for (; i < s.len; i++) {
        unsigned int d = (unsigned int)(unsigned char)s.ptr[i] - (unsigned int)'0';
        if (d > 9u) {
            break;
        }
        if (val >= UINT64_MAX / 10u && (val > UINT64_MAX / 10u || (uint64_t)d > UINT64_MAX % 10u)) {
            return UINT64_MAX;
        }
        val = val * 10u + (uint64_t)d;
    }
    return val;
}

static inline bool ws_parse_nonzero_int(ws_span_t s)
{
    size_t i = 0;
    while (i < s.len && ws_is_space((unsigned char)s.ptr[i])) {
        i++;
    }
    if (i < s.len && (s.ptr[i] == '+' || s.ptr[i] == '-')) {
        i++;
    }
    for (; i < s.len; i++) {
        unsigned int d = (unsigned int)(unsigned char)s.ptr[i] - (unsigned int)'0';
        if (d > 9u) {
            break;
        }
        if (d != 0u) {
            return true;
        }
    }
    return false;
}

#if WS_HAVE_X86_SIMD && defined(_OPENMP)

typedef struct {
    uint64_t sm;
    uint64_t qcarry;
    bool ok;
} ws_simd_step64_t;

/* Parses 1..16 ASCII digits ending at end_ptr; caller guarantees end_ptr - src >= 16. */
__attribute__((target("avx2,bmi"), always_inline)) static inline uint64_t
ws_simd_u64_end(const char *end_ptr, size_t slen)
{
    if (__builtin_expect(slen == 1u, 0)) {
        unsigned d = (unsigned)(unsigned char)end_ptr[-1] - (unsigned)'0';
        return d <= 9u ? (uint64_t)d : UINT64_MAX;
    }
    if (__builtin_expect((slen - 1u) >= 16u, 0)) {
        return UINT64_MAX;
    }

    __m128i raw = _mm_loadu_si128((const __m128i *)(const void *)(end_ptr - 16));
    const __m128i ramp = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    __m128i mask = _mm_cmpgt_epi8(_mm_set1_epi8((char)(16u - slen)), ramp);
    const __m128i v_zero = _mm_set1_epi8('0');
    __m128i aligned = _mm_blendv_epi8(raw, v_zero, mask);
    __m128i sub = _mm_sub_epi8(aligned, v_zero);
    __m128i bad = _mm_subs_epu8(sub, _mm_set1_epi8(9));
    if (__builtin_expect(!_mm_testz_si128(bad, bad), 0)) {
        return UINT64_MAX;
    }

    const __m128i m10 = _mm_setr_epi8(10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1);
    const __m128i m100 = _mm_setr_epi16(100, 1, 100, 1, 100, 1, 100, 1);
    const __m128i m10000 = _mm_setr_epi16(10000, 1, 10000, 1, 10000, 1, 10000, 1);

    __m128i t1 = _mm_maddubs_epi16(sub, m10);
    __m128i t2 = _mm_madd_epi16(t1, m100);
    __m128i t2p = _mm_packus_epi32(t2, t2);
    __m128i t3 = _mm_madd_epi16(t2p, m10000);
    uint64_t pair = (uint64_t)_mm_cvtsi128_si64(t3);
    return (pair & 0xFFFFFFFFULL) * 100000000ULL + (pair >> 32u);
}

/* Extracts structural quote/brace mask across 64 bytes while rejecting escapes, braces inside
 * quotes, and unquoted tokens so slice boundaries remain provably outside quotes by induction. */
__attribute__((target("avx2,bmi"), always_inline)) static inline ws_simd_step64_t
ws_simd_scan64(const char *src, size_t pos, size_t limit_hi, uint64_t qcarry)
{
    ws_simd_step64_t res = {0ULL, qcarry, true};
    if (__builtin_expect(pos + 64u <= limit_hi, 1)) {
        const __m256i v_quote = _mm256_set1_epi8('"');
        const __m256i v_bslash = _mm256_set1_epi8('\\');
        const __m256i v_lbrace = _mm256_set1_epi8('{');
        const __m256i v_rbrace = _mm256_set1_epi8('}');
        const __m256i v_sp = _mm256_set1_epi8(0x20);

        __m256i c0 = _mm256_loadu_si256((const __m256i *)(const void *)(src + pos));
        __m256i c1 = _mm256_loadu_si256((const __m256i *)(const void *)(src + pos + 32u));
        __m256i bs =
            _mm256_or_si256(_mm256_cmpeq_epi8(c0, v_bslash), _mm256_cmpeq_epi8(c1, v_bslash));
        if (__builtin_expect(!_mm256_testz_si256(bs, bs), 0)) {
            res.ok = false;
            return res;
        }

        uint64_t mq =
            ((uint64_t)(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(c1, v_quote)) << 32u) |
            (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(c0, v_quote));
        uint64_t mb =
            ((uint64_t)(uint32_t)_mm256_movemask_epi8(
                 _mm256_or_si256(_mm256_cmpeq_epi8(c1, v_lbrace), _mm256_cmpeq_epi8(c1, v_rbrace)))
             << 32u) |
            (uint32_t)_mm256_movemask_epi8(
                _mm256_or_si256(_mm256_cmpeq_epi8(c0, v_lbrace), _mm256_cmpeq_epi8(c0, v_rbrace)));
        uint64_t mnw =
            ((uint64_t)(uint32_t)_mm256_movemask_epi8(
                 _mm256_or_si256(c1, _mm256_cmpgt_epi8(c1, v_sp)))
             << 32u) |
            (uint32_t)_mm256_movemask_epi8(_mm256_or_si256(c0, _mm256_cmpgt_epi8(c0, v_sp)));

        uint64_t in_q = mq;
        in_q ^= in_q << 1u;
        in_q ^= in_q << 2u;
        in_q ^= in_q << 4u;
        in_q ^= in_q << 8u;
        in_q ^= in_q << 16u;
        in_q ^= in_q << 32u;
        in_q ^= qcarry;
        res.qcarry = (uint64_t)((int64_t)in_q >> 63);

        uint64_t outside = ~(in_q | mq);
        if (__builtin_expect(((mb & in_q) | (mnw & outside & ~mb)) != 0ULL, 0)) {
            res.ok = false;
            return res;
        }
        res.sm = mq | mb;
        return res;
    }

    size_t n = limit_hi > pos ? (limit_hi - pos) : 0u;
    if (n > 64u) {
        n = 64u;
    }
    uint64_t sm = 0ULL;
    for (size_t k = 0; k < n; k++) {
        unsigned char ch = (unsigned char)src[pos + k];
        if (ch == '\\') {
            res.ok = false;
            return res;
        }
        if (ch == '"') {
            qcarry = ~qcarry;
            sm |= (1ULL << k);
        } else if (qcarry == 0ULL) {
            if (ch == '{' || ch == '}') {
                sm |= (1ULL << k);
            } else if (!ws_is_space(ch)) {
                res.ok = false;
                return res;
            }
        } else if (ch == '{' || ch == '}') {
            res.ok = false;
            return res;
        }
    }
    res.sm = sm;
    res.qcarry = qcarry;
    return res;
}

__attribute__((target("avx2,bmi"))) static size_t ws_find_details_kw(const char *src, size_t start,
                                                                     size_t len)
{
    const __m256i vw = _mm256_set1_epi8('w');
    const __m256i vlow = _mm256_set1_epi8(0x20);
    size_t i = start;
    for (; i + 64u <= len; i += 32u) {
        __m256i c = _mm256_loadu_si256((const __m256i *)(const void *)(src + i));
        uint32_t m =
            (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_or_si256(c, vlow), vw));
        while (m != 0u) {
            size_t p = i + (size_t)_tzcnt_u32(m);
            m = _blsr_u32(m);
            if (p > start && src[p - 1u] == '"' && p + 20u <= len && src[p + 19u] == '"') {
                ws_span_t cand = {.ptr = src + p, .len = 19u};
                if (ws_span_ieq(cand, "workshopitemdetails", 19u)) {
                    return p - 1u;
                }
            }
        }
    }
    for (; i + 21u <= len; i++) {
        if (src[i] == '"' && src[i + 20u] == '"') {
            ws_span_t cand = {.ptr = src + i + 1u, .len = 19u};
            if (ws_span_ieq(cand, "workshopitemdetails", 19u)) {
                return i;
            }
        }
    }
    return 0u;
}

static size_t ws_snap_item_start(const char *src, size_t pos, size_t hi)
{
    while (pos < hi && src[pos] != '{') {
        pos++;
    }
    if (pos >= hi) {
        return hi;
    }
    size_t p = pos;
    while (p > 0u && ws_is_space((unsigned char)src[p - 1u])) {
        p--;
    }
    if (p == 0u || src[p - 1u] != '"') {
        return hi;
    }
    p--;
    while (p > 0u && src[p - 1u] != '"') {
        p--;
    }
    if (p == 0u) {
        return hi;
    }
    return p - 1u;
}

typedef struct {
    size_t count;
    size_t end_p0;
    bool ok;
} ws_slice_res_t;

__attribute__((target("avx2,bmi"))) static ws_slice_res_t
ws_parse_installed_slice(const char *src, size_t slice_lo, size_t slice_hi, size_t limit_hi,
                         int tid, waywal_workshop_item_manifest_t *sub_base, size_t per_cap,
                         uint8_t *claim_base, uint32_t *slots, size_t ht_mask, unsigned ht_shift)
{
    ws_slice_res_t out = {0u, slice_lo, false};
    if (slice_lo >= slice_hi) {
        out.ok = true;
        return out;
    }

    waywal_workshop_item_manifest_t *dst = sub_base + (size_t)tid * per_cap;
    uint8_t *my_claimed = claim_base + (size_t)tid * per_cap;
    uint32_t tid_tag = (uint32_t)tid << 24u;

    size_t base = slice_lo;
    uint64_t qcarry = 0ULL;
    ws_simd_step64_t st = ws_simd_scan64(src, base, limit_hi, qcarry);
    if (!st.ok) {
        return out;
    }
    uint64_t sm = st.sm;
    qcarry = st.qcarry;
    size_t cnt = 0u;

#define WS_SLICE_NEXT(out_p)                                                                       \
    do {                                                                                           \
        while (sm == 0ULL) {                                                                       \
            base += 64u;                                                                           \
            if (__builtin_expect(base >= limit_hi, 0)) {                                           \
                return out;                                                                        \
            }                                                                                      \
            ws_simd_step64_t s2 = ws_simd_scan64(src, base, limit_hi, qcarry);                     \
            if (__builtin_expect(!s2.ok, 0)) {                                                     \
                return out;                                                                        \
            }                                                                                      \
            sm = s2.sm;                                                                            \
            qcarry = s2.qcarry;                                                                    \
        }                                                                                          \
        (out_p) = base + (size_t)_tzcnt_u64(sm);                                                   \
        sm = _blsr_u64(sm);                                                                        \
    } while (0)

    for (;;) {
        size_t p0;
        WS_SLICE_NEXT(p0);
        if (p0 >= slice_hi || src[p0] == '}') {
            out.count = cnt;
            out.end_p0 = p0;
            out.ok = true;
            return out;
        }
        if (__builtin_expect(src[p0] != '"', 0)) {
            return out;
        }

        size_t p1, pb;
        WS_SLICE_NEXT(p1);
        WS_SLICE_NEXT(pb);
        if (__builtin_expect(src[pb] != '{', 0)) {
            return out;
        }

        uint64_t id = ws_simd_u64_end(src + p1, p1 - p0 - 1u);
        if (__builtin_expect(id == 0u || id == UINT64_MAX || cnt >= per_cap, 0)) {
            return out;
        }

        size_t local_idx = cnt++;
        my_claimed[local_idx] = 0u;
        waywal_workshop_item_manifest_t *it = &dst[local_idx];
        it->published_file_id = id;
        it->size_bytes = 0u;
        it->time_updated = 0u;
        it->manifest_id = 0u;
        it->state_flags = WAYWAL_UGC_STATE_INSTALLED;
        it->subscribed_by_user = false;
        it->is_installed = true;

        uint32_t encoded = tid_tag | (uint32_t)(local_idx + 1u);
        size_t ht_cap = ht_mask + 1u;
        size_t sl = (size_t)((id * WS_FIB_HASH_MULT) >> ht_shift);
        size_t steps = 0u;
        for (; steps < ht_cap; ++steps) {
            uint32_t exp = 0u;
            /* Publish published_file_id before making the slot visible to peer slices. */
            if (__atomic_compare_exchange_n(&slots[sl], &exp, encoded, false, __ATOMIC_RELEASE,
                                            __ATOMIC_ACQUIRE)) {
                break;
            }
            size_t otid = (size_t)(exp >> 24u);
            size_t oidx = (size_t)(exp & 0xFFFFFFu) - 1u;
            if (__builtin_expect(sub_base[otid * per_cap + oidx].published_file_id == id, 0)) {
                return out;
            }
            sl = (sl + 1u) & ht_mask;
        }
        if (__builtin_expect(steps == ht_cap, 0)) {
            return out;
        }

        uint64_t man_primary = 0u;
        uint64_t man_alt = 0u;
        bool has_man_primary = false;

        for (;;) {
            size_t kp0;
            WS_SLICE_NEXT(kp0);
            if (src[kp0] == '}') {
                break;
            }
            if (__builtin_expect(src[kp0] != '"', 0)) {
                return out;
            }

            size_t kp1, vp0, vp1;
            WS_SLICE_NEXT(kp1);
            WS_SLICE_NEXT(vp0);
            if (__builtin_expect(src[vp0] != '"', 0)) {
                return out;
            }
            WS_SLICE_NEXT(vp1);

            const char *kptr = src + kp0 + 1u;
            size_t klen = kp1 - kp0 - 1u;
            uint64_t val = ws_simd_u64_end(src + vp1, vp1 - vp0 - 1u);
            if (__builtin_expect(val == UINT64_MAX, 0)) {
                return out;
            }

            if (klen == 4u) {
                uint32_t w;
                memcpy(&w, kptr, 4u);
                if ((w | 0x20202020u) == WS_PACK4('s', 'i', 'z', 'e')) {
                    it->size_bytes = val;
                }
            } else if (klen == 8u) {
                uint64_t w;
                memcpy(&w, kptr, 8u);
                if ((w | WS_SWAR_LOWER) == WS_PACK8('m', 'a', 'n', 'i', 'f', 'e', 's', 't')) {
                    man_primary = val;
                    has_man_primary = true;
                }
            } else if (klen == 11u) {
                uint64_t w0;
                uint32_t w1;
                memcpy(&w0, kptr, 8u);
                memcpy(&w1, kptr + 7u, 4u);
                if ((w0 | WS_SWAR_LOWER) == WS_PACK8('t', 'i', 'm', 'e', 'u', 'p', 'd', 'a') &&
                    (w1 | 0x20202020u) == WS_PACK4('a', 't', 'e', 'd')) {
                    it->time_updated = val;
                }
            } else if (klen == 10u) {
                uint64_t w0;
                uint16_t w1;
                memcpy(&w0, kptr, 8u);
                memcpy(&w1, kptr + 8u, 2u);
                if ((w0 | WS_SWAR_LOWER) == WS_PACK8('m', 'a', 'n', 'i', 'f', 'e', 's', 't') &&
                    (uint16_t)(w1 | 0x2020u) == (uint16_t)((unsigned)'i' | ((unsigned)'d' << 8u))) {
                    man_alt = val;
                }
            }
        }
        it->manifest_id = has_man_primary ? man_primary : man_alt;
    }

#undef WS_SLICE_NEXT
}

__attribute__((target("avx2,bmi"))) static ws_slice_res_t
ws_parse_details_slice(const char *src, size_t slice_lo, size_t slice_hi, size_t limit_hi, int tid,
                       int nth, waywal_workshop_item_manifest_t *sub_base, size_t per_cap,
                       const ws_slice_res_t *ires, const uint32_t *slots, uint8_t *claim_base,
                       size_t ht_mask, unsigned ht_shift)
{
    ws_slice_res_t out = {0u, slice_lo, false};
    if (slice_lo >= slice_hi) {
        out.ok = true;
        return out;
    }

    size_t base = slice_lo;
    uint64_t qcarry = 0ULL;
    ws_simd_step64_t st = ws_simd_scan64(src, base, limit_hi, qcarry);
    if (!st.ok) {
        return out;
    }
    uint64_t sm = st.sm;
    qcarry = st.qcarry;

    int hint_tid = tid;
    size_t hint_k = 0u;
    while (hint_tid < nth && hint_k >= ires[hint_tid].count) {
        hint_tid++;
        hint_k = 0u;
    }

#define WS_SLICE_NEXT(out_p)                                                                       \
    do {                                                                                           \
        while (sm == 0ULL) {                                                                       \
            base += 64u;                                                                           \
            if (__builtin_expect(base >= limit_hi, 0)) {                                           \
                return out;                                                                        \
            }                                                                                      \
            ws_simd_step64_t s2 = ws_simd_scan64(src, base, limit_hi, qcarry);                     \
            if (__builtin_expect(!s2.ok, 0)) {                                                     \
                return out;                                                                        \
            }                                                                                      \
            sm = s2.sm;                                                                            \
            qcarry = s2.qcarry;                                                                    \
        }                                                                                          \
        (out_p) = base + (size_t)_tzcnt_u64(sm);                                                   \
        sm = _blsr_u64(sm);                                                                        \
    } while (0)

    for (;;) {
        size_t p0;
        WS_SLICE_NEXT(p0);
        if (p0 >= slice_hi || src[p0] == '}') {
            out.end_p0 = p0;
            out.ok = true;
            return out;
        }
        if (__builtin_expect(src[p0] != '"', 0)) {
            return out;
        }

        size_t p1, pb;
        WS_SLICE_NEXT(p1);
        WS_SLICE_NEXT(pb);
        if (__builtin_expect(src[pb] != '{', 0)) {
            return out;
        }

        uint64_t id = ws_simd_u64_end(src + p1, p1 - p0 - 1u);
        if (__builtin_expect(id == 0u || id == UINT64_MAX, 0)) {
            return out;
        }

        size_t owner_pos;
        if (__builtin_expect(hint_tid < nth &&
                                 sub_base[(size_t)hint_tid * per_cap + hint_k].published_file_id ==
                                     id,
                             1)) {
            owner_pos = (size_t)hint_tid * per_cap + hint_k;
            hint_k++;
            while (__builtin_expect(hint_tid < nth && hint_k >= ires[hint_tid].count, 0)) {
                hint_tid++;
                hint_k = 0u;
            }
        } else {
            size_t ht_cap = ht_mask + 1u;
            size_t slot = (size_t)((id * WS_FIB_HASH_MULT) >> ht_shift);
            size_t steps = 0u;
            for (; steps < ht_cap; ++steps) {
                uint32_t e = __atomic_load_n(&slots[slot], __ATOMIC_ACQUIRE);
                if (__builtin_expect(e == 0u, 0)) {
                    return out;
                }
                size_t otid = (size_t)(e >> 24u);
                size_t ok = (size_t)(e & 0xFFFFFFu) - 1u;
                size_t cand_pos = otid * per_cap + ok;
                if (sub_base[cand_pos].published_file_id == id) {
                    owner_pos = cand_pos;
                    hint_tid = (int)otid;
                    hint_k = ok + 1u;
                    while (hint_tid < nth && hint_k >= ires[hint_tid].count) {
                        hint_tid++;
                        hint_k = 0u;
                    }
                    break;
                }
                slot = (slot + 1u) & ht_mask;
            }
            if (__builtin_expect(steps == ht_cap, 0)) {
                return out;
            }
        }

        /* Prevent cross-thread writes if a manifest repeats a published_file_id in details. */
        if (__builtin_expect(
                __atomic_exchange_n(&claim_base[owner_pos], 1u, __ATOMIC_ACQ_REL) != 0u, 0)) {
            return out;
        }

        waywal_workshop_item_manifest_t *it = &sub_base[owner_pos];
        uint64_t man_primary = 0u;
        uint64_t man_alt = 0u;
        bool has_man_primary = false;
        int flags_prio = 0;
        uint32_t chosen_flags = 0u;

        for (;;) {
            size_t kp0;
            WS_SLICE_NEXT(kp0);
            if (src[kp0] == '}') {
                break;
            }
            if (__builtin_expect(src[kp0] != '"', 0)) {
                return out;
            }

            size_t kp1, vp0, vp1;
            WS_SLICE_NEXT(kp1);
            WS_SLICE_NEXT(vp0);
            if (__builtin_expect(src[vp0] != '"', 0)) {
                return out;
            }
            WS_SLICE_NEXT(vp1);

            const char *kptr = src + kp0 + 1u;
            size_t klen = kp1 - kp0 - 1u;
            size_t vlen = vp1 - vp0 - 1u;

            if (klen == 16u) {
                uint64_t w0, w1;
                memcpy(&w0, kptr, 8u);
                memcpy(&w1, kptr + 8u, 8u);
                if ((w0 | WS_SWAR_LOWER) == WS_PACK8('s', 'u', 'b', 's', 'c', 'r', 'i', 'b') &&
                    (w1 | WS_SWAR_LOWER) == WS_PACK8('e', 'd', 'b', 'y', 'u', 's', 'e', 'r')) {
                    uint64_t val = ws_simd_u64_end(src + vp1, vlen);
                    if (__builtin_expect(val == UINT64_MAX, 0)) {
                        return out;
                    }
                    if (val != 0u) {
                        it->subscribed_by_user = true;
                        it->state_flags |= WAYWAL_UGC_STATE_SUBSCRIBED;
                    }
                }
            } else if (klen == 5u) {
                uint32_t w;
                memcpy(&w, kptr, 4u);
                uint32_t wl = w | 0x20202020u;
                unsigned c4 = (unsigned)(unsigned char)kptr[4] | 0x20u;
                if (wl == WS_PACK4('s', 't', 'a', 't') && c4 == 'e') {
                    uint64_t val = ws_simd_u64_end(src + vp1, vlen);
                    if (__builtin_expect(val == UINT64_MAX, 0)) {
                        return out;
                    }
                    chosen_flags = (uint32_t)val;
                    flags_prio = 3;
                } else if (flags_prio < 1 && wl == WS_PACK4('f', 'l', 'a', 'g') && c4 == 's') {
                    uint64_t val = ws_simd_u64_end(src + vp1, vlen);
                    if (__builtin_expect(val == UINT64_MAX, 0)) {
                        return out;
                    }
                    chosen_flags = (uint32_t)val;
                    flags_prio = 1;
                }
            } else if (klen == 9u) {
                uint64_t w;
                memcpy(&w, kptr, 8u);
                uint64_t wl = w | WS_SWAR_LOWER;
                unsigned c8 = (unsigned)(unsigned char)kptr[8] | 0x20u;
                if (wl == WS_PACK8('i', 'n', 's', 't', 'a', 'l', 'l', 'e') && c8 == 'd') {
                    uint64_t val = ws_simd_u64_end(src + vp1, vlen);
                    if (__builtin_expect(val == UINT64_MAX, 0)) {
                        return out;
                    }
                    if (val != 0u) {
                        it->is_installed = true;
                        it->state_flags |= WAYWAL_UGC_STATE_INSTALLED;
                    }
                } else if (flags_prio < 2 &&
                           wl == WS_PACK8('i', 't', 'e', 'm', 's', 't', 'a', 't') && c8 == 'e') {
                    uint64_t val = ws_simd_u64_end(src + vp1, vlen);
                    if (__builtin_expect(val == UINT64_MAX, 0)) {
                        return out;
                    }
                    chosen_flags = (uint32_t)val;
                    flags_prio = 2;
                }
            } else if (klen == 8u) {
                uint64_t w;
                memcpy(&w, kptr, 8u);
                if ((w | WS_SWAR_LOWER) == WS_PACK8('m', 'a', 'n', 'i', 'f', 'e', 's', 't')) {
                    uint64_t val = ws_simd_u64_end(src + vp1, vlen);
                    if (__builtin_expect(val == UINT64_MAX, 0)) {
                        return out;
                    }
                    man_primary = val;
                    has_man_primary = true;
                }
            } else if (klen == 10u) {
                uint64_t w0;
                uint16_t w1;
                memcpy(&w0, kptr, 8u);
                memcpy(&w1, kptr + 8u, 2u);
                if ((w0 | WS_SWAR_LOWER) == WS_PACK8('m', 'a', 'n', 'i', 'f', 'e', 's', 't') &&
                    (uint16_t)(w1 | 0x2020u) == (uint16_t)((unsigned)'i' | ((unsigned)'d' << 8u))) {
                    uint64_t val = ws_simd_u64_end(src + vp1, vlen);
                    if (__builtin_expect(val == UINT64_MAX, 0)) {
                        return out;
                    }
                    man_alt = val;
                }
            } else if (klen == 11u) {
                uint64_t w0;
                uint32_t w1;
                memcpy(&w0, kptr, 8u);
                memcpy(&w1, kptr + 7u, 4u);
                if ((w0 | WS_SWAR_LOWER) == WS_PACK8('t', 'i', 'm', 'e', 'u', 'p', 'd', 'a') &&
                    (w1 | 0x20202020u) == WS_PACK4('a', 't', 'e', 'd')) {
                    uint64_t val = ws_simd_u64_end(src + vp1, vlen);
                    if (__builtin_expect(val == UINT64_MAX, 0)) {
                        return out;
                    }
                    if (val != 0u) {
                        it->time_updated = val;
                    }
                }
            }
        }

        uint64_t man = has_man_primary ? man_primary : man_alt;
        if (man != 0u) {
            it->manifest_id = man;
        }
        if (flags_prio > 0) {
            it->state_flags |= chosen_flags;
            if (it->state_flags & WAYWAL_UGC_STATE_SUBSCRIBED) {
                it->subscribed_by_user = true;
            }
            if (it->state_flags & WAYWAL_UGC_STATE_INSTALLED) {
                it->is_installed = true;
            }
        }
    }

#undef WS_SLICE_NEXT
}

static bool ws_try_parse_parallel(const char *src, size_t sz,
                                  waywal_appworkshop_manifest_t *out_manifest)
{
    if (!__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("bmi")) {
        return false;
    }

    ws_vdf_cursor_t c = {.src = src, .len = sz, .pos = 0u};
    ws_span_t root_key;
    if (!ws_next_token(&c, &root_key) || !ws_span_ieq(root_key, "appworkshop", 11u)) {
        return false;
    }
    ws_skip_ws(&c);
    if (c.pos >= sz || src[c.pos] != '{') {
        return false;
    }
    c.pos++;

    out_manifest->app_id = 431960u;
    bool has_needs_update = false;
    size_t inst_lo = 0u;

    while (c.pos < sz) {
        ws_skip_ws(&c);
        if (c.pos >= sz || src[c.pos] == '}' || src[c.pos] == '{') {
            return false;
        }
        ws_span_t key;
        if (!ws_next_token(&c, &key)) {
            return false;
        }
        ws_skip_ws(&c);
        if (c.pos >= sz) {
            return false;
        }
        if (src[c.pos] == '{') {
            if (!ws_span_ieq(key, "workshopitemsinstalled", 22u)) {
                return false;
            }
            c.pos++;
            inst_lo = c.pos;
            break;
        }
        ws_span_t val;
        if (!ws_next_token(&c, &val)) {
            return false;
        }
        if (ws_span_ieq(key, "appid", 5u)) {
            out_manifest->app_id = (uint32_t)ws_parse_u64(val);
        } else if (ws_span_ieq(key, "sizeondisk", 10u)) {
            out_manifest->size_on_disk_bytes = ws_parse_u64(val);
        } else if (ws_span_ieq(key, "needsupdate", 11u)) {
            out_manifest->needs_update_count = (uint32_t)ws_parse_u64(val);
            has_needs_update = true;
        } else if (!has_needs_update && ws_span_ieq(key, "needsupdatecount", 16u)) {
            out_manifest->needs_update_count = (uint32_t)ws_parse_u64(val);
        } else if (ws_span_ieq(key, "timelastupdated", 15u)) {
            out_manifest->time_last_updated = ws_parse_u64(val);
        } else if (ws_span_ieq(key, "timelastappcheck", 16u)) {
            out_manifest->time_last_app_check = ws_parse_u64(val);
        }
    }

    if (inst_lo < 16u || inst_lo >= sz) {
        return false;
    }

    size_t guess_mid = inst_lo + ((sz - inst_lo) * 9u) / 16u;
    size_t det_kw_pos = ws_find_details_kw(src, guess_mid, sz);
    if (det_kw_pos == 0u) {
        det_kw_pos = ws_find_details_kw(src, inst_lo, sz);
    }
    if (det_kw_pos == 0u) {
        return false;
    }

    size_t inst_hi = det_kw_pos;
    while (inst_hi > inst_lo && ws_is_space((unsigned char)src[inst_hi - 1u])) {
        inst_hi--;
    }
    if (inst_hi <= inst_lo || src[inst_hi - 1u] != '}') {
        return false;
    }
    inst_hi--;

    size_t det_lo = det_kw_pos + 21u;
    while (det_lo < sz && ws_is_space((unsigned char)src[det_lo])) {
        det_lo++;
    }
    if (det_lo >= sz || src[det_lo] != '{') {
        return false;
    }
    det_lo++;

    size_t tail_pos = sz;
    while (tail_pos > det_lo && ws_is_space((unsigned char)src[tail_pos - 1u])) {
        tail_pos--;
    }
    if (tail_pos <= det_lo || src[tail_pos - 1u] != '}') {
        return false;
    }
    tail_pos--;
    while (tail_pos > det_lo && ws_is_space((unsigned char)src[tail_pos - 1u])) {
        tail_pos--;
    }
    if (tail_pos <= det_lo || src[tail_pos - 1u] != '}') {
        return false;
    }
    size_t det_hi = tail_pos - 1u;

    int max_p = omp_get_max_threads();
    int nth = max_p > 4 ? 4 : (max_p < 1 ? 1 : max_p);
    if (nth > WS_MAX_PARALLEL_THREADS) {
        nth = WS_MAX_PARALLEL_THREADS;
    }
    const int nchunks = (nth == 1) ? 1 : WS_PARALLEL_CHUNKS;

    size_t per_cap = (inst_hi - inst_lo) / (24u * (size_t)nchunks) + 256u;
    if (per_cap >= (1u << 24)) {
        return false;
    }
    size_t total_sub_cap = per_cap * (size_t)nchunks;
    if (total_sub_cap > SIZE_MAX / 2u) {
        return false;
    }

    size_t est_items = sz / 120u + 64u;
    size_t min_ht = 64u;
    if (est_items <= SIZE_MAX / 4u && est_items * 4u > min_ht) {
        min_ht = est_items * 4u;
    }
    /* Keep open-addressing load factor <= 0.5 across the worst-case parallel item capacity. */
    if (total_sub_cap * 2u > min_ht) {
        min_ht = total_sub_cap * 2u;
    }

    unsigned log2_ht = 12u;
    while (((size_t)1u << log2_ht) < min_ht && log2_ht < 27u) {
        log2_ht++;
    }
    if (((size_t)1u << log2_ht) < min_ht) {
        return false;
    }
    size_t ht_cap = (size_t)1u << log2_ht;
    unsigned ht_shift = 64u - log2_ht;

    bool own_scratch = !atomic_flag_test_and_set_explicit(&g_ws_scratch_lock, memory_order_acquire);
    waywal_workshop_item_manifest_t *sub_base = NULL;
    uint32_t *slots = NULL;
    uint8_t *claimed = NULL;

    if (own_scratch) {
        if (g_ws_sub_cap < total_sub_cap) {
            waywal_workshop_item_manifest_t *nb = (waywal_workshop_item_manifest_t *)realloc(
                g_ws_sub_buf, total_sub_cap * sizeof(*nb));
            if (!nb) {
                atomic_flag_clear_explicit(&g_ws_scratch_lock, memory_order_release);
                return false;
            }
            g_ws_sub_buf = nb;
            g_ws_sub_cap = total_sub_cap;
        }
        if (g_ws_slots_cap < ht_cap) {
            uint32_t *ns = (uint32_t *)realloc(g_ws_slots_buf, ht_cap * sizeof(uint32_t));
            if (!ns) {
                atomic_flag_clear_explicit(&g_ws_scratch_lock, memory_order_release);
                return false;
            }
            g_ws_slots_buf = ns;
            g_ws_slots_cap = ht_cap;
        }
        if (g_ws_claim_cap < total_sub_cap) {
            uint8_t *nc = (uint8_t *)realloc(g_ws_claim_buf, total_sub_cap);
            if (!nc) {
                atomic_flag_clear_explicit(&g_ws_scratch_lock, memory_order_release);
                return false;
            }
            g_ws_claim_buf = nc;
            g_ws_claim_cap = total_sub_cap;
        }
        sub_base = g_ws_sub_buf;
        slots = g_ws_slots_buf;
        claimed = g_ws_claim_buf;
    } else {
        sub_base = (waywal_workshop_item_manifest_t *)malloc(total_sub_cap * sizeof(*sub_base));
        slots = (uint32_t *)malloc(ht_cap * sizeof(uint32_t));
        claimed = (uint8_t *)malloc(total_sub_cap);
        if (!sub_base || !slots || !claimed) {
            free(sub_base);
            free(slots);
            free(claimed);
            return false;
        }
    }

    size_t ibounds[WS_PARALLEL_CHUNKS + 1];
    size_t dbounds[WS_PARALLEL_CHUNKS + 1];
    ibounds[0] = ws_snap_item_start(src, inst_lo, inst_hi);
    dbounds[0] = ws_snap_item_start(src, det_lo, det_hi);
    for (int c_idx = 1; c_idx < nchunks; c_idx++) {
        size_t raw_i = inst_lo + ((inst_hi - inst_lo) * (size_t)c_idx) / (size_t)nchunks;
        size_t raw_d = det_lo + ((det_hi - det_lo) * (size_t)c_idx) / (size_t)nchunks;
        ibounds[c_idx] = ws_snap_item_start(src, raw_i, inst_hi);
        dbounds[c_idx] = ws_snap_item_start(src, raw_d, det_hi);
        if (ibounds[c_idx] < ibounds[c_idx - 1]) {
            ibounds[c_idx] = ibounds[c_idx - 1];
        }
        if (dbounds[c_idx] < dbounds[c_idx - 1]) {
            dbounds[c_idx] = dbounds[c_idx - 1];
        }
    }
    ibounds[nchunks] = inst_hi;
    dbounds[nchunks] = det_hi;

    /* Verify no unexpected tokens precede the first snapped item in either block */
    for (size_t p = inst_lo; p < ibounds[0]; p++) {
        if (!ws_is_space((unsigned char)src[p])) {
            if (own_scratch) {
                atomic_flag_clear_explicit(&g_ws_scratch_lock, memory_order_release);
            } else {
                free(sub_base);
                free(slots);
                free(claimed);
            }
            return false;
        }
    }
    for (size_t p = det_lo; p < dbounds[0]; p++) {
        if (!ws_is_space((unsigned char)src[p])) {
            if (own_scratch) {
                atomic_flag_clear_explicit(&g_ws_scratch_lock, memory_order_release);
            } else {
                free(sub_base);
                free(slots);
                free(claimed);
            }
            return false;
        }
    }

    ws_slice_res_t ires[WS_PARALLEL_CHUNKS];
    ws_slice_res_t dres[WS_PARALLEL_CHUNKS];
    size_t sub_off[WS_PARALLEL_CHUNKS];
    uint64_t part_size[WS_MAX_PARALLEL_THREADS];
    uint32_t part_upd[WS_MAX_PARALLEL_THREADS];
    uint32_t part_dl[WS_MAX_PARALLEL_THREADS];
    memset(ires, 0, sizeof(ires));
    memset(dres, 0, sizeof(dres));
    memset(sub_off, 0, sizeof(sub_off));
    memset(part_size, 0, sizeof(part_size));
    memset(part_upd, 0, sizeof(part_upd));
    memset(part_dl, 0, sizeof(part_dl));

    memset(slots, 0, ht_cap * sizeof(uint32_t));

    waywal_workshop_item_manifest_t *items = NULL;
    size_t items_alloc_cap = 0u;
    size_t total = 0u;
    int bail = 0;
    int next_ic = 0;
    int next_dc = 0;
    int next_mc = 0;

#pragma omp parallel num_threads(nth)
    {
        int tid = omp_get_thread_num();

        for (;;) {
            int ic = __atomic_fetch_add(&next_ic, 1, __ATOMIC_RELAXED);
            if (ic >= nchunks) {
                break;
            }
            ires[ic] =
                ws_parse_installed_slice(src, ibounds[ic], ibounds[ic + 1], inst_hi + 1u, ic,
                                         sub_base, per_cap, claimed, slots, ht_cap - 1u, ht_shift);
            if (!ires[ic].ok || ires[ic].end_p0 != ibounds[ic + 1]) {
                __atomic_store_n(&bail, 1, __ATOMIC_RELAXED);
                break;
            }
        }

#pragma omp barrier
#pragma omp single
        {
            if (!bail) {
                for (int ic = 0; ic < nchunks; ic++) {
                    sub_off[ic] = total;
                    total += ires[ic].count;
                }
                if (total * 4u > ht_cap * 3u) {
                    bail = 1;
                } else if (total > 0u) {
                    items = ws_alloc_items(total, false, &items_alloc_cap);
                    if (!items) {
                        bail = 1;
                    }
                }
            }
        }

        if (!bail) {
            for (;;) {
                int dc = __atomic_fetch_add(&next_dc, 1, __ATOMIC_RELAXED);
                if (dc >= nchunks) {
                    break;
                }
                dres[dc] = ws_parse_details_slice(src, dbounds[dc], dbounds[dc + 1], det_hi + 1u,
                                                  dc, nchunks, sub_base, per_cap, ires, slots,
                                                  claimed, ht_cap - 1u, ht_shift);
                if (!dres[dc].ok || dres[dc].end_p0 != dbounds[dc + 1]) {
                    __atomic_store_n(&bail, 1, __ATOMIC_RELAXED);
                    break;
                }
            }
        }

#pragma omp barrier
        if (!bail) {
            uint64_t sz_sum = 0u;
            uint32_t upd_sum = 0u;
            uint32_t dl_sum = 0u;
            for (;;) {
                int mc = __atomic_fetch_add(&next_mc, 1, __ATOMIC_RELAXED);
                if (mc >= nchunks) {
                    break;
                }
                size_t my_cnt = ires[mc].count;
                if (my_cnt == 0u) {
                    continue;
                }
                const waywal_workshop_item_manifest_t *my_sub = sub_base + (size_t)mc * per_cap;
                memcpy(items + sub_off[mc], my_sub, my_cnt * sizeof(*items));

                for (size_t i = 0u; i < my_cnt; i++) {
                    const waywal_workshop_item_manifest_t *it = &my_sub[i];
                    sz_sum += it->size_bytes;
                    if (it->state_flags & WAYWAL_UGC_STATE_NEEDS_UPDATE) {
                        upd_sum++;
                    }
                    if ((it->state_flags &
                         (WAYWAL_UGC_STATE_DOWNLOADING | WAYWAL_UGC_STATE_DOWNLOAD_PENDING)) ||
                        (it->subscribed_by_user && !it->is_installed)) {
                        dl_sum++;
                    }
                }
            }
            part_size[tid] = sz_sum;
            part_upd[tid] = upd_sum;
            part_dl[tid] = dl_sum;
        }
    }

    if (own_scratch) {
        atomic_flag_clear_explicit(&g_ws_scratch_lock, memory_order_release);
    } else {
        free(sub_base);
        free(slots);
        free(claimed);
    }

    if (bail) {
        ws_release_items(items, items_alloc_cap);
        memset(out_manifest, 0, sizeof(*out_manifest));
        return false;
    }

    uint64_t total_computed_size = 0u;
    uint32_t calc_needs_update = 0u;
    uint32_t calc_needs_download = 0u;
    for (int t = 0; t < nth; t++) {
        total_computed_size += part_size[t];
        calc_needs_update += part_upd[t];
        calc_needs_download += part_dl[t];
    }

    if (out_manifest->size_on_disk_bytes == 0u) {
        out_manifest->size_on_disk_bytes = total_computed_size;
    }
    if (out_manifest->needs_update_count == 0u) {
        out_manifest->needs_update_count = calc_needs_update;
    }
    out_manifest->needs_download_count = calc_needs_download;
    out_manifest->item_count = total;
    out_manifest->items = items;
    return true;
}

#endif /* WS_HAVE_X86_SIMD && _OPENMP */

static bool parse_installed_block(ws_vdf_cursor_t *c, ws_item_table_t *tbl)
{
    while (c->pos < c->len) {
        ws_skip_ws(c);
        if (c->pos >= c->len) {
            break;
        }
        if (c->src[c->pos] == '}') {
            c->pos++;
            break;
        }

        ws_span_t id_tok;
        if (!ws_next_token(c, &id_tok)) {
            c->pos++;
            continue;
        }

        uint64_t id = ws_parse_u64(id_tok);
        ws_skip_ws(c);
        if (c->pos >= c->len) {
            break;
        }

        if (id == 0u) {
            ws_skip_value_or_block(c);
            continue;
        }

        waywal_workshop_item_manifest_t *it = find_or_add_item(tbl, id);
        if (!it) {
            return false;
        }

        it->is_installed = true;
        it->state_flags |= WAYWAL_UGC_STATE_INSTALLED;

        if (c->src[c->pos] == '{') {
            c->pos++;
            uint64_t man_primary = 0;
            uint64_t man_alt = 0;
            bool has_man_primary = false;

            while (c->pos < c->len) {
                ws_skip_ws(c);
                if (c->pos >= c->len) {
                    break;
                }
                if (c->src[c->pos] == '}') {
                    c->pos++;
                    break;
                }

                ws_span_t k;
                if (!ws_next_token(c, &k)) {
                    c->pos++;
                    continue;
                }
                ws_skip_ws(c);
                if (c->pos >= c->len || c->src[c->pos] == '}') {
                    break;
                }
                if (c->src[c->pos] == '{') {
                    c->pos++;
                    ws_skip_block_after_lbrace(c);
                    continue;
                }

                ws_span_t v;
                if (!ws_next_token(c, &v)) {
                    continue;
                }

                if (ws_span_ieq(k, "size", 4u)) {
                    it->size_bytes = ws_parse_u64(v);
                } else if (ws_span_ieq(k, "timeupdated", 11u)) {
                    it->time_updated = ws_parse_u64(v);
                } else if (ws_span_ieq(k, "manifest", 8u)) {
                    man_primary = ws_parse_u64(v);
                    has_man_primary = true;
                } else if (ws_span_ieq(k, "manifestid", 10u)) {
                    man_alt = ws_parse_u64(v);
                }
            }
            it->manifest_id = has_man_primary ? man_primary : man_alt;
        } else if (c->src[c->pos] != '}') {
            ws_span_t val_tok;
            if (ws_next_token(c, &val_tok)) {
                it->manifest_id = ws_parse_u64(val_tok);
            }
        }
    }
    return true;
}

static bool parse_details_block(ws_vdf_cursor_t *c, ws_item_table_t *tbl)
{
    tbl->seq_hint = 0u;
    while (c->pos < c->len) {
        ws_skip_ws(c);
        if (c->pos >= c->len) {
            break;
        }
        if (c->src[c->pos] == '}') {
            c->pos++;
            break;
        }

        ws_span_t id_tok;
        if (!ws_next_token(c, &id_tok)) {
            c->pos++;
            continue;
        }

        uint64_t id = ws_parse_u64(id_tok);
        ws_skip_ws(c);
        if (c->pos >= c->len) {
            break;
        }

        if (id == 0u || c->src[c->pos] != '{') {
            ws_skip_value_or_block(c);
            continue;
        }

        waywal_workshop_item_manifest_t *it = find_or_add_item(tbl, id);
        if (!it) {
            return false;
        }

        c->pos++;
        uint64_t man_primary = 0;
        uint64_t man_alt = 0;
        bool has_man_primary = false;
        int flags_prio = 0;
        uint32_t chosen_flags = 0;

        while (c->pos < c->len) {
            ws_skip_ws(c);
            if (c->pos >= c->len) {
                break;
            }
            if (c->src[c->pos] == '}') {
                c->pos++;
                break;
            }

            ws_span_t k;
            if (!ws_next_token(c, &k)) {
                c->pos++;
                continue;
            }
            ws_skip_ws(c);
            if (c->pos >= c->len || c->src[c->pos] == '}') {
                break;
            }
            if (c->src[c->pos] == '{') {
                c->pos++;
                ws_skip_block_after_lbrace(c);
                continue;
            }

            ws_span_t v;
            if (!ws_next_token(c, &v)) {
                continue;
            }

            if (ws_span_ieq(k, "manifest", 8u)) {
                man_primary = ws_parse_u64(v);
                has_man_primary = true;
            } else if (ws_span_ieq(k, "manifestid", 10u)) {
                man_alt = ws_parse_u64(v);
            } else if (ws_span_ieq(k, "timeupdated", 11u)) {
                uint64_t tu = ws_parse_u64(v);
                if (tu != 0u) {
                    it->time_updated = tu;
                }
            } else if (ws_span_ieq(k, "subscribedbyuser", 16u)) {
                if (ws_parse_nonzero_int(v)) {
                    it->subscribed_by_user = true;
                    it->state_flags |= WAYWAL_UGC_STATE_SUBSCRIBED;
                }
            } else if (ws_span_ieq(k, "installed", 9u)) {
                if (ws_parse_nonzero_int(v)) {
                    it->is_installed = true;
                    it->state_flags |= WAYWAL_UGC_STATE_INSTALLED;
                }
            } else if (ws_span_ieq(k, "state", 5u)) {
                chosen_flags = (uint32_t)ws_parse_u64(v);
                flags_prio = 3;
            } else if (flags_prio < 2 && ws_span_ieq(k, "itemstate", 9u)) {
                chosen_flags = (uint32_t)ws_parse_u64(v);
                flags_prio = 2;
            } else if (flags_prio < 1 && ws_span_ieq(k, "flags", 5u)) {
                chosen_flags = (uint32_t)ws_parse_u64(v);
                flags_prio = 1;
            }
        }

        uint64_t man = has_man_primary ? man_primary : man_alt;
        if (man != 0u) {
            it->manifest_id = man;
        }
        if (flags_prio > 0) {
            it->state_flags |= chosen_flags;
            if (it->state_flags & WAYWAL_UGC_STATE_SUBSCRIBED) {
                it->subscribed_by_user = true;
            }
            if (it->state_flags & WAYWAL_UGC_STATE_INSTALLED) {
                it->is_installed = true;
            }
        }
    }
    return true;
}

static bool parse_appworkshop_scope(ws_vdf_cursor_t *c, waywal_appworkshop_manifest_t *out,
                                    ws_item_table_t *tbl, bool *has_needs_update, int depth)
{
    while (c->pos < c->len) {
        ws_skip_ws(c);
        if (c->pos >= c->len) {
            break;
        }
        if (c->src[c->pos] == '}') {
            c->pos++;
            if (depth > 0) {
                break;
            }
            continue;
        }
        if (c->src[c->pos] == '{') {
            c->pos++;
            if (depth < 4) {
                if (!parse_appworkshop_scope(c, out, tbl, has_needs_update, depth + 1)) {
                    return false;
                }
            } else {
                ws_skip_block_after_lbrace(c);
            }
            continue;
        }

        ws_span_t key;
        if (!ws_next_token(c, &key)) {
            c->pos++;
            continue;
        }

        ws_skip_ws(c);
        if (c->pos >= c->len) {
            break;
        }

        if (c->src[c->pos] == '{') {
            c->pos++;
            if (ws_span_ieq(key, "workshopitemsinstalled", 22u)) {
                if (!parse_installed_block(c, tbl)) {
                    return false;
                }
            } else if (ws_span_ieq(key, "workshopitemdetails", 19u)) {
                if (!parse_details_block(c, tbl)) {
                    return false;
                }
            } else if ((depth == 0 || ws_span_ieq(key, "appworkshop", 11u)) && depth < 4) {
                if (!parse_appworkshop_scope(c, out, tbl, has_needs_update, depth + 1)) {
                    return false;
                }
            } else {
                ws_skip_block_after_lbrace(c);
            }
            continue;
        }

        if (c->src[c->pos] == '}') {
            continue;
        }

        ws_span_t val;
        if (!ws_next_token(c, &val)) {
            continue;
        }

        if (ws_span_ieq(key, "appid", 5u)) {
            out->app_id = (uint32_t)ws_parse_u64(val);
        } else if (ws_span_ieq(key, "sizeondisk", 10u)) {
            out->size_on_disk_bytes = ws_parse_u64(val);
        } else if (ws_span_ieq(key, "needsupdate", 11u)) {
            out->needs_update_count = (uint32_t)ws_parse_u64(val);
            *has_needs_update = true;
        } else if (!*has_needs_update && ws_span_ieq(key, "needsupdatecount", 16u)) {
            out->needs_update_count = (uint32_t)ws_parse_u64(val);
        } else if (ws_span_ieq(key, "timelastupdated", 15u)) {
            out->time_last_updated = ws_parse_u64(val);
        } else if (ws_span_ieq(key, "timelastappcheck", 16u)) {
            out->time_last_app_check = ws_parse_u64(val);
        }
    }
    return true;
}

bool waywal_appworkshop_parse_file(const char *manifest_path,
                                   waywal_appworkshop_manifest_t *out_manifest)
{
    if (!manifest_path || manifest_path[0] == '\0' || !out_manifest) {
        return false;
    }
    memset(out_manifest, 0, sizeof(*out_manifest));

    int fd = open(manifest_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return false;
    }

    size_t sz = (size_t)st.st_size;
    if (sz > (512ULL * 1024ULL * 1024ULL)) {
        close(fd);
        return false;
    }

    const char *data = NULL;
    char *heap_buf = NULL;
    void *mapped = MAP_FAILED;
    bool own_io = false;

    if (sz + 1u <= WS_IO_CACHE_MAX_CAP &&
        !atomic_flag_test_and_set_explicit(&g_ws_io_lock, memory_order_acquire)) {
        own_io = true;
        if (g_ws_io_cap < sz + 1u) {
            if (g_ws_io_buf) {
                WS_ASAN_UNPOISON(g_ws_io_buf, g_ws_io_cap);
            }
            size_t new_cap = sz + 1u + 65536u;
            if (new_cap > WS_IO_CACHE_MAX_CAP) {
                new_cap = WS_IO_CACHE_MAX_CAP;
            }
            char *nb = (char *)realloc(g_ws_io_buf, new_cap);
            if (nb) {
                g_ws_io_buf = nb;
                g_ws_io_cap = new_cap;
                WS_ASAN_POISON(g_ws_io_buf, g_ws_io_cap);
            } else {
                if (g_ws_io_buf) {
                    WS_ASAN_POISON(g_ws_io_buf, g_ws_io_cap);
                }
                atomic_flag_clear_explicit(&g_ws_io_lock, memory_order_release);
                own_io = false;
            }
        }
        if (own_io) {
            WS_ASAN_UNPOISON(g_ws_io_buf, sz + 1u);
            size_t total_rd = 0u;
            bool io_err = false;
            while (total_rd < sz) {
                ssize_t n = read(fd, g_ws_io_buf + total_rd, sz - total_rd);
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
            if (io_err || total_rd != sz) {
                WS_ASAN_POISON(g_ws_io_buf, g_ws_io_cap);
                atomic_flag_clear_explicit(&g_ws_io_lock, memory_order_release);
                close(fd);
                return false;
            }
            g_ws_io_buf[total_rd] = '\0';
            data = g_ws_io_buf;
        }
    }

    if (!data && sz >= 65536u) {
        mapped = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapped != MAP_FAILED) {
            data = (const char *)mapped;
        }
    }
    if (!data) {
        heap_buf = (char *)malloc(sz + 1u);
        if (!heap_buf) {
            close(fd);
            return false;
        }
        size_t total_rd = 0u;
        bool io_err = false;
        while (total_rd < sz) {
            ssize_t n = read(fd, heap_buf + total_rd, sz - total_rd);
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
        if (io_err || total_rd != sz) {
            free(heap_buf);
            close(fd);
            return false;
        }
        heap_buf[total_rd] = '\0';
        data = heap_buf;
    }
    close(fd);

#define WS_RELEASE_FILE_BUF()                                                                      \
    do {                                                                                           \
        if (own_io) {                                                                              \
            WS_ASAN_POISON(g_ws_io_buf, g_ws_io_cap);                                              \
            atomic_flag_clear_explicit(&g_ws_io_lock, memory_order_release);                       \
        }                                                                                          \
        if (mapped != MAP_FAILED) {                                                                \
            munmap(mapped, (size_t)st.st_size);                                                    \
        }                                                                                          \
        free(heap_buf);                                                                            \
    } while (0)

#if WS_HAVE_X86_SIMD && defined(_OPENMP)
    if (sz >= 262144u && ws_try_parse_parallel(data, sz, out_manifest)) {
        WS_RELEASE_FILE_BUF();
        return true;
    }
    memset(out_manifest, 0, sizeof(*out_manifest));
#endif

    ws_vdf_cursor_t cursor = {
        .src = data,
        .len = sz,
        .pos = 0,
    };
    ws_skip_ws(&cursor);
    if (cursor.pos >= cursor.len) {
        WS_RELEASE_FILE_BUF();
        return false;
    }

    ws_item_table_t tbl;
    if (!ws_table_init(&tbl, sz / 140u)) {
        WS_RELEASE_FILE_BUF();
        return false;
    }

    out_manifest->app_id = 431960u;
    bool has_needs_update = false;
    bool ok = parse_appworkshop_scope(&cursor, out_manifest, &tbl, &has_needs_update, 0);

    WS_RELEASE_FILE_BUF();
#undef WS_RELEASE_FILE_BUF

    if (!ok) {
        ws_table_free_all(&tbl);
        memset(out_manifest, 0, sizeof(*out_manifest));
        return false;
    }

    free(tbl.slots);
    tbl.slots = NULL;

    uint32_t calc_needs_update = 0;
    uint32_t calc_needs_download = 0;
    uint64_t total_computed_size = 0;

    for (size_t i = 0; i < tbl.count; i++) {
        const waywal_workshop_item_manifest_t *it = &tbl.items[i];
        total_computed_size += it->size_bytes;
        if (it->state_flags & WAYWAL_UGC_STATE_NEEDS_UPDATE) {
            calc_needs_update++;
        }
        if ((it->state_flags &
             (WAYWAL_UGC_STATE_DOWNLOADING | WAYWAL_UGC_STATE_DOWNLOAD_PENDING)) ||
            (it->subscribed_by_user && !it->is_installed)) {
            calc_needs_download++;
        }
    }

    if (out_manifest->size_on_disk_bytes == 0u) {
        out_manifest->size_on_disk_bytes = total_computed_size;
    }
    if (out_manifest->needs_update_count == 0u) {
        out_manifest->needs_update_count = calc_needs_update;
    }
    out_manifest->needs_download_count = calc_needs_download;
    out_manifest->item_count = tbl.count;
    if (tbl.count == 0u) {
        ws_release_items(tbl.items, tbl.cap);
        out_manifest->items = NULL;
    } else {
        out_manifest->items = tbl.items;
    }

    return true;
}

void waywal_appworkshop_free(waywal_appworkshop_manifest_t *manifest)
{
    if (!manifest) {
        return;
    }
    if (manifest->items) {
        ws_release_items(manifest->items, manifest->item_count);
        manifest->items = NULL;
    }
    manifest->item_count = 0;
}
