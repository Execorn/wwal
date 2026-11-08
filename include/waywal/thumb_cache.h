// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_THUMB_CACHE_H
#define WAYWAL_THUMB_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/* Resolves the standard cache root directory ~/.cache/wwal/thumbnails */
bool waywal_thumb_get_cache_dir(char *out_dir, size_t out_cap);

/* Computes the deterministic SHA-256 hex string for a given canonical file path */
bool waywal_thumb_compute_path_hash(const char *media_path, char *out_hex, size_t out_cap);

/* Resolves destination cache file path: ~/.cache/wwal/thumbnails/<sha256>.webp */
bool waywal_thumb_resolve_cache_path(const char *media_path, char *out_path, size_t out_cap);

/* Checks if a valid cached thumbnail file exists on disk */
bool waywal_thumb_is_cached(const char *media_path);

/* Initializes metadata descriptor */
void waywal_thumb_meta_init(waywal_thumb_meta_t *meta);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_THUMB_CACHE_H */
