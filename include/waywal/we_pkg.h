// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WE_PKG_H
#define WAYWAL_WE_PKG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WE_PKG_MAX_ENTRIES  65536u
#define WE_PKG_MAX_PATH_LEN 1024u

typedef struct {
    char path[WE_PKG_MAX_PATH_LEN + 1];
    uint32_t offset;
    uint32_t length;
    const uint8_t *data;
} we_pkg_entry_t;

typedef struct {
    uint64_t hash;
    uint32_t entry_idx; /* 1-based index into entries[], 0 = empty slot */
} we_pkg_ht_slot_t;

typedef struct we_pkg {
    const uint8_t *base_ptr;
    size_t mmap_size;
    bool is_mmapped;
    int fd;
    char version_magic[16];
    uint32_t version_num;
    uint32_t entry_count;
    size_t data_base_offset;
    we_pkg_entry_t *entries;
    we_pkg_ht_slot_t *ht_slots;
    uint32_t ht_cap;
    char workshop_dir[4096];
    char assets_dir[4096];
} we_pkg_t;

/* Normalizes a VFS path:
 * - Converts '\\' -> '/'
 * - Strips leading '/' and './'
 * - Collapses duplicate '/'
 * - Rejects any path containing '..' or control characters
 * - Lowercases ASCII characters */
bool we_pkg_normalize_path(const char *raw_path, size_t raw_len, char *out_norm, size_t out_cap);

/* 64-bit FNV-1a hash of a normalized path string */
uint64_t we_pkg_fnv1a64(const char *str, size_t len);

/* Opens a PKGV0001..PKGV0022 archive from an in-memory buffer (zero-copy) */
we_pkg_t *we_pkg_open_mem(const uint8_t *data, size_t size, const char *workshop_dir);

/* Opens and mmaps a scene.pkg / gifscene.pkg file on disk */
we_pkg_t *we_pkg_open_file(const char *pkg_path, const char *workshop_dir);

/* Opens a Workshop item directory (loads scene.pkg or gifscene.pkg if present, or loose-only VFS)
 */
we_pkg_t *we_pkg_open_dir(const char *workshop_dir);

/* Closes a we_pkg_t and unmaps/frees associated resources */
void we_pkg_close(we_pkg_t *pkg);

/* Looks up a path strictly inside the loaded .pkg archive (zero-copy, returns NULL if not found) */
const uint8_t *we_pkg_find(const we_pkg_t *pkg, const char *rel_path, size_t *out_size);

/* Resolves a path across the 4-layer VFS overlay:
 * 1. mmap'd scene.pkg / gifscene.pkg hash table (*out_owned = false)
 * 2. Loose file <workshop_dir>/<rel_path> on disk (*out_owned = true, caller frees with free())
 * 3. <steam_root>/steamapps/common/wallpaper_engine/assets/<rel_path> (*out_owned = true)
 * 4. Built-in fallback assets (*out_owned = false) */
const uint8_t *we_pkg_resolve(const we_pkg_t *pkg, const char *rel_path, size_t *out_size,
                              bool *out_owned);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WE_PKG_H */
