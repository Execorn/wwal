// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_THUMB_CACHE_H
#define WAYWAL_THUMB_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_POSIX_C_SOURCE) || defined(_GNU_SOURCE) || defined(_DEFAULT_SOURCE) || defined(_XOPEN_SOURCE)
#include <sys/stat.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define WAYWAL_THUMB_WIDTH  384u
#define WAYWAL_THUMB_HEIGHT 216u

typedef enum {
    WAYWAL_THUMB_FLAG_NONE = 0,
    WAYWAL_THUMB_FLAG_VIDEO = 1u << 0,
    WAYWAL_THUMB_FLAG_SHADER = 1u << 1,
    WAYWAL_THUMB_FLAG_SCENE = 1u << 2,
    WAYWAL_THUMB_FLAG_FAVORITE = 1u << 3,
} waywal_thumb_flags_t;

typedef struct {
    uint32_t id;
    uint32_t flags;    /* IS_VIDEO, IS_SHADER, IS_SCENE, IS_FAVORITE */
    uint64_t tag_mask; /* 64 bitmask categories: anime, nature, abstract, etc. */
    char title[128];
    char path[512];
    float aspect_ratio;
    uint32_t dominant_color; /* Hex RGB */
} waywal_thumb_meta_t;

/* Defined inline so C23 and C++23 callers resolve symbols without extra link targets. */
static inline uint32_t waywal_thumb_rotr32(uint32_t value, uint32_t bits)
{
    return (value >> bits) | (value << (32u - bits));
}

static inline void waywal_thumb_sha256_block(uint32_t state[8], const uint8_t block[64])
{
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };

    uint32_t w[64];
    for (size_t i = 0; i < 16; ++i) {
        size_t j = i * 4;
        w[i] = ((uint32_t)block[j] << 24u) | ((uint32_t)block[j + 1] << 16u) |
               ((uint32_t)block[j + 2] << 8u) | (uint32_t)block[j + 3];
    }
    for (size_t i = 16; i < 64; ++i) {
        uint32_t s0 = waywal_thumb_rotr32(w[i - 15], 7u) ^ waywal_thumb_rotr32(w[i - 15], 18u) ^ (w[i - 15] >> 3u);
        uint32_t s1 = waywal_thumb_rotr32(w[i - 2], 17u) ^ waywal_thumb_rotr32(w[i - 2], 19u) ^ (w[i - 2] >> 10u);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (size_t i = 0; i < 64; ++i) {
        uint32_t s1 = waywal_thumb_rotr32(e, 6u) ^ waywal_thumb_rotr32(e, 11u) ^ waywal_thumb_rotr32(e, 25u);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + k[i] + w[i];
        uint32_t s0 = waywal_thumb_rotr32(a, 2u) ^ waywal_thumb_rotr32(a, 13u) ^ waywal_thumb_rotr32(a, 22u);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

/* Resolves the standard cache root directory ~/.cache/wwal/thumbnails */
static inline bool waywal_thumb_get_cache_dir(char *out_dir, size_t out_cap)
{
    if (!out_dir || out_cap == 0) {
        return false;
    }
    out_dir[0] = '\0';

    const char *xdg_cache = getenv("XDG_CACHE_HOME");
    const char *base = NULL;
    const char *suffix = NULL;
    if (xdg_cache && xdg_cache[0] == '/') {
        base = xdg_cache;
        suffix = "wwal/thumbnails";
    } else {
        const char *home = getenv("HOME");
        if (!home || home[0] == '\0') {
            return false;
        }
        base = home;
        suffix = ".cache/wwal/thumbnails";
    }

    size_t base_len = strlen(base);
    while (base_len > 1 && base[base_len - 1] == '/') {
        --base_len;
    }
    if (base_len > 4096u) {
        return false;
    }

    int written = (base_len == 1 && base[0] == '/')
                      ? snprintf(out_dir, out_cap, "/%s", suffix)
                      : snprintf(out_dir, out_cap, "%.*s/%s", (int)base_len, base, suffix);
    if (written < 0 || (size_t)written >= out_cap) {
        out_dir[0] = '\0';
        return false;
    }
    return true;
}

/* Computes the deterministic SHA-256 hex string for a given canonical file path */
static inline bool waywal_thumb_compute_path_hash(const char *media_path, char *out_hex, size_t out_cap)
{
    if (!media_path || media_path[0] == '\0' || !out_hex || out_cap < 65u) {
        if (out_hex && out_cap > 0) {
            out_hex[0] = '\0';
        }
        return false;
    }

    uint32_t state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };

    const size_t total_len = strlen(media_path);
    size_t offset = 0;
    uint8_t block[64];
    while (total_len - offset >= 64u) {
        memcpy(block, media_path + offset, 64u);
        waywal_thumb_sha256_block(state, block);
        offset += 64u;
    }

    const size_t tail = total_len - offset;
    memset(block, 0, sizeof(block));
    if (tail > 0u) {
        memcpy(block, media_path + offset, tail);
    }
    block[tail] = 0x80u;
    if (tail >= 56u) {
        waywal_thumb_sha256_block(state, block);
        memset(block, 0, sizeof(block));
    }

    const uint64_t bit_len = (uint64_t)total_len * 8u;
    for (size_t i = 0; i < 8u; ++i) {
        block[56u + i] = (uint8_t)((bit_len >> (56u - (i * 8u))) & 0xffu);
    }
    waywal_thumb_sha256_block(state, block);

    static const char hex_digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 8u; ++i) {
        for (size_t nib = 0; nib < 8u; ++nib) {
            const uint32_t shift = (uint32_t)((7u - nib) * 4u);
            out_hex[(i * 8u) + nib] = hex_digits[(state[i] >> shift) & 0xfu];
        }
    }
    out_hex[64] = '\0';
    return true;
}

/* Resolves destination cache file path: ~/.cache/wwal/thumbnails/<sha256>.webp */
static inline bool waywal_thumb_resolve_cache_path(const char *media_path, char *out_path, size_t out_cap)
{
    if (!media_path || media_path[0] == '\0' || !out_path || out_cap == 0) {
        if (out_path && out_cap > 0) {
            out_path[0] = '\0';
        }
        return false;
    }
    out_path[0] = '\0';

    char cache_dir[3584];
    char hash_hex[65];
    if (!waywal_thumb_get_cache_dir(cache_dir, sizeof(cache_dir)) ||
        !waywal_thumb_compute_path_hash(media_path, hash_hex, sizeof(hash_hex))) {
        return false;
    }

    int written = snprintf(out_path, out_cap, "%s/%s.webp", cache_dir, hash_hex);
    if (written < 0 || (size_t)written >= out_cap) {
        out_path[0] = '\0';
        return false;
    }
    return true;
}

/* Checks if a valid cached thumbnail file exists on disk */
static inline bool waywal_thumb_is_cached(const char *media_path)
{
    char cache_path[4096];
    if (!waywal_thumb_resolve_cache_path(media_path, cache_path, sizeof(cache_path))) {
        return false;
    }
#if defined(_POSIX_C_SOURCE) || defined(_GNU_SOURCE) || defined(_DEFAULT_SOURCE) || defined(_XOPEN_SOURCE)
    struct stat st;
    return stat(cache_path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
#else
    FILE *fp = fopen(cache_path, "rb");
    if (!fp) {
        return false;
    }
    bool non_empty = fseek(fp, 0, SEEK_END) == 0 && ftell(fp) > 0;
    fclose(fp);
    return non_empty;
#endif
}

/* Initializes metadata descriptor */
static inline void waywal_thumb_meta_init(waywal_thumb_meta_t *meta)
{
    if (!meta) {
        return;
    }
    memset(meta, 0, sizeof(*meta));
    meta->aspect_ratio = (float)WAYWAL_THUMB_WIDTH / (float)WAYWAL_THUMB_HEIGHT;
}

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_THUMB_CACHE_H */
