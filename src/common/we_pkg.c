// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/we_pkg.h"

#include "waywal/log.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) |
           ((uint32_t)p[3] << 24u);
}

uint64_t we_pkg_fnv1a64(const char *str, size_t len)
{
    uint64_t hash = 14695981039346656037ULL;
    const uint64_t prime = 1099511628211ULL;
    for (size_t i = 0; i < len; ++i) {
        hash ^= (uint64_t)(unsigned char)str[i];
        hash *= prime;
    }
    return hash;
}

static bool normalize_path_internal(const char *raw_path, size_t raw_len, char *out_norm,
                                    size_t out_cap, bool lowercase)
{
    if (!raw_path || !out_norm || out_cap < 2u) {
        return false;
    }

    while (raw_len > 0 && raw_path[raw_len - 1u] == '\0') {
        raw_len--;
    }
    if (raw_len == 0 || raw_len > WE_PKG_MAX_PATH_LEN) {
        return false;
    }

    char temp[WE_PKG_MAX_PATH_LEN + 2u];
    for (size_t i = 0; i < raw_len; ++i) {
        unsigned char c = (unsigned char)raw_path[i];
        if (c < 0x20u || c == 0x7Fu) {
            return false;
        }
        if (c == '\\') {
            temp[i] = '/';
        } else if (lowercase) {
            temp[i] = (char)tolower(c);
        } else {
            temp[i] = (char)c;
        }
    }
    temp[raw_len] = '\0';

    size_t out_len = 0;
    size_t pos = 0;
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
            continue;
        }
        if (seg_len == 2u && temp[seg_start] == '.' && temp[seg_start + 1u] == '.') {
            return false;
        }
        size_t need = seg_len + (out_len > 0 ? 1u : 0u);
        if (out_len + need >= out_cap) {
            return false;
        }
        if (out_len > 0) {
            out_norm[out_len++] = '/';
        }
        memcpy(out_norm + out_len, temp + seg_start, seg_len);
        out_len += seg_len;
    }

    if (out_len == 0) {
        return false;
    }
    out_norm[out_len] = '\0';
    return true;
}

bool we_pkg_normalize_path(const char *raw_path, size_t raw_len, char *out_norm, size_t out_cap)
{
    return normalize_path_internal(raw_path, raw_len, out_norm, out_cap, true);
}

static void populate_steam_assets_dir(we_pkg_t *pkg, const char *workshop_dir)
{
    pkg->assets_dir[0] = '\0';
    if (workshop_dir && workshop_dir[0] != '\0') {
        snprintf(pkg->workshop_dir, sizeof(pkg->workshop_dir), "%s", workshop_dir);
    }

    const char *env_assets = getenv("WWAL_WE_ASSETS_DIR");
    if (env_assets && env_assets[0] != '\0' && access(env_assets, R_OK) == 0) {
        snprintf(pkg->assets_dir, sizeof(pkg->assets_dir), "%s", env_assets);
        return;
    }

    if (workshop_dir && workshop_dir[0] != '\0') {
        const char *marker = strstr(workshop_dir, "/steamapps/");
        if (marker) {
            size_t root_len = (size_t)(marker - workshop_dir);
            if (root_len + 64u < sizeof(pkg->assets_dir)) {
                snprintf(pkg->assets_dir, sizeof(pkg->assets_dir),
                         "%.*s/steamapps/common/wallpaper_engine/assets", (int)root_len,
                         workshop_dir);
                if (access(pkg->assets_dir, R_OK) == 0) {
                    return;
                }
                pkg->assets_dir[0] = '\0';
            }
        }
    }

    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') {
        return;
    }

    static const char *const candidates[] = {
        ".local/share/Steam/steamapps/common/wallpaper_engine/assets",
        ".steam/steam/steamapps/common/wallpaper_engine/assets",
        ".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/wallpaper_engine/"
        "assets",
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        char buf[4096];
        int n = snprintf(buf, sizeof(buf), "%s/%s", home, candidates[i]);
        if (n > 0 && (size_t)n < sizeof(buf) && access(buf, R_OK) == 0) {
            snprintf(pkg->assets_dir, sizeof(pkg->assets_dir), "%s", buf);
            return;
        }
    }
}

static bool parse_pkg_table_at(we_pkg_t *pkg, const uint8_t *data, size_t size, size_t start_cursor)
{
    if (start_cursor + 4u > size) {
        return false;
    }

    uint32_t entry_count = read_u32_le(data + start_cursor);
    if (entry_count > WE_PKG_MAX_ENTRIES) {
        return false;
    }

    size_t cursor = start_cursor + 4u;
    if (entry_count == 0) {
        pkg->entry_count = 0;
        pkg->data_base_offset = cursor;
        pkg->entries = NULL;
        pkg->ht_slots = NULL;
        pkg->ht_cap = 0;
        return true;
    }

    /* Minimum 13 bytes per entry: u32 path_len (>=1) + 1 char + u32 offset + u32 length */
    if ((uint64_t)entry_count * 13ULL > (uint64_t)(size - cursor)) {
        return false;
    }

    we_pkg_entry_t *entries = (we_pkg_entry_t *)calloc(entry_count, sizeof(we_pkg_entry_t));
    if (!entries) {
        return false;
    }

    for (uint32_t i = 0; i < entry_count; ++i) {
        if (cursor + 4u > size) {
            free(entries);
            return false;
        }
        uint32_t path_len = read_u32_le(data + cursor);
        cursor += 4u;
        if (path_len == 0 || path_len > WE_PKG_MAX_PATH_LEN || cursor + path_len + 8u > size) {
            free(entries);
            return false;
        }

        const char *raw_path = (const char *)(data + cursor);
        cursor += path_len;

        if (!we_pkg_normalize_path(raw_path, path_len, entries[i].path, sizeof(entries[i].path))) {
            free(entries);
            return false;
        }

        entries[i].offset = read_u32_le(data + cursor);
        entries[i].length = read_u32_le(data + cursor + 4u);
        cursor += 8u;
    }

    size_t data_base_offset = cursor;
    for (uint32_t i = 0; i < entry_count; ++i) {
        uint64_t start = (uint64_t)data_base_offset + (uint64_t)entries[i].offset;
        uint64_t end = start + (uint64_t)entries[i].length;
        if (start < (uint64_t)data_base_offset || end < start || end > (uint64_t)size) {
            free(entries);
            return false;
        }
        entries[i].data = data + (size_t)start;
    }

    uint32_t ht_cap = 16u;
    while (ht_cap < entry_count * 2u) {
        ht_cap <<= 1u;
    }

    we_pkg_ht_slot_t *ht_slots = (we_pkg_ht_slot_t *)calloc(ht_cap, sizeof(we_pkg_ht_slot_t));
    if (!ht_slots) {
        free(entries);
        return false;
    }

    uint32_t mask = ht_cap - 1u;
    for (uint32_t i = 0; i < entry_count; ++i) {
        size_t plen = strlen(entries[i].path);
        uint64_t h = we_pkg_fnv1a64(entries[i].path, plen);
        uint32_t slot = (uint32_t)h & mask;
        while (ht_slots[slot].entry_idx != 0) {
            uint32_t existing_idx = ht_slots[slot].entry_idx - 1u;
            if (ht_slots[slot].hash == h &&
                strcmp(entries[existing_idx].path, entries[i].path) == 0) {
                /* Overwrite duplicate entry with latest occurrence */
                ht_slots[slot].entry_idx = i + 1u;
                goto next_entry;
            }
            slot = (slot + 1u) & mask;
        }
        ht_slots[slot].hash = h;
        ht_slots[slot].entry_idx = i + 1u;
    next_entry:;
    }

    pkg->entry_count = entry_count;
    pkg->data_base_offset = data_base_offset;
    pkg->entries = entries;
    pkg->ht_slots = ht_slots;
    pkg->ht_cap = ht_cap;
    return true;
}

we_pkg_t *we_pkg_open_mem(const uint8_t *data, size_t size, const char *workshop_dir)
{
    if (!data || size < 15u) {
        return NULL;
    }

    uint32_t magic_len = read_u32_le(data);
    if (magic_len < 7u || magic_len > 16u || 4u + (size_t)magic_len + 4u > size) {
        return NULL;
    }

    const char *magic_ptr = (const char *)(data + 4u);
    if (memcmp(magic_ptr, "PKGV", 4u) != 0) {
        return NULL;
    }

    char ver_str[16];
    size_t copy_len = magic_len < 15u ? magic_len : 15u;
    memcpy(ver_str, magic_ptr, copy_len);
    ver_str[copy_len] = '\0';
    while (copy_len > 0 && ver_str[copy_len - 1u] == '\0') {
        ver_str[--copy_len] = '\0';
    }

    if (copy_len < 5u) {
        return NULL;
    }
    for (size_t i = 4u; i < copy_len; ++i) {
        if (!isdigit((unsigned char)ver_str[i])) {
            return NULL;
        }
    }
    uint32_t ver_num = (uint32_t)strtoul(ver_str + 4u, NULL, 10);
    if (ver_num == 0 || ver_num > 99u) {
        return NULL;
    }

    we_pkg_t *pkg = (we_pkg_t *)calloc(1, sizeof(we_pkg_t));
    if (!pkg) {
        return NULL;
    }
    pkg->base_ptr = data;
    pkg->mmap_size = size;
    pkg->is_mmapped = false;
    pkg->fd = -1;
    snprintf(pkg->version_magic, sizeof(pkg->version_magic), "%s", ver_str);
    pkg->version_num = ver_num;
    populate_steam_assets_dir(pkg, workshop_dir);

    size_t table_cursor = 4u + (size_t)magic_len;
    if (!parse_pkg_table_at(pkg, data, size, table_cursor)) {
        /* Handle optional trailing '\0' byte immediately after magic[magic_len] */
        if (table_cursor < size && data[table_cursor] == '\0' &&
            parse_pkg_table_at(pkg, data, size, table_cursor + 1u)) {
            return pkg;
        }
        free(pkg);
        return NULL;
    }

    return pkg;
}

we_pkg_t *we_pkg_open_file(const char *pkg_path, const char *workshop_dir)
{
    if (!pkg_path || pkg_path[0] == '\0') {
        return NULL;
    }

    int fd = open(pkg_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 15 ||
        st.st_size > (2LL * 1024LL * 1024LL * 1024LL)) {
        close(fd);
        return NULL;
    }

    size_t mmap_sz = (size_t)st.st_size;
    void *mapped = mmap(NULL, mmap_sz, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    (void)madvise(mapped, mmap_sz, MADV_SEQUENTIAL);

    char derived_dir[4096] = {0};
    if (!workshop_dir || workshop_dir[0] == '\0') {
        const char *slash = strrchr(pkg_path, '/');
        if (slash) {
            size_t dlen = (size_t)(slash - pkg_path);
            if (dlen == 0) {
                dlen = 1u; /* Root "/" */
            }
            if (dlen < sizeof(derived_dir)) {
                memcpy(derived_dir, pkg_path, dlen);
                derived_dir[dlen] = '\0';
                workshop_dir = derived_dir;
            }
        } else {
            memcpy(derived_dir, ".", 2u);
            workshop_dir = derived_dir;
        }
    }

    we_pkg_t *pkg = we_pkg_open_mem((const uint8_t *)mapped, mmap_sz, workshop_dir);
    if (!pkg) {
        munmap(mapped, mmap_sz);
        close(fd);
        return NULL;
    }

    /* Switch back from MADV_SEQUENTIAL after header table scan to avoid random-access readahead
     * thrashing across large packed .tex/.mp4 payloads. */
    (void)madvise(mapped, mmap_sz, MADV_NORMAL);

    pkg->is_mmapped = true;
    pkg->fd = fd;
    return pkg;
}

we_pkg_t *we_pkg_open_dir(const char *workshop_dir)
{
    if (!workshop_dir || workshop_dir[0] == '\0') {
        return NULL;
    }

    struct stat st;
    if (stat(workshop_dir, &st) < 0) {
        return NULL;
    }

    if (S_ISREG(st.st_mode)) {
        size_t plen = strlen(workshop_dir);
        if (plen > 4u && strcasecmp(workshop_dir + plen - 4u, ".pkg") == 0) {
            return we_pkg_open_file(workshop_dir, NULL);
        }
        /* Possibly a path to project.json or scene.json; strip filename */
        char parent_dir[4096];
        const char *slash = strrchr(workshop_dir, '/');
        if (!slash) {
            return we_pkg_open_dir(".");
        }
        size_t dlen = (size_t)(slash - workshop_dir);
        if (dlen == 0) {
            dlen = 1u;
        }
        if (dlen >= sizeof(parent_dir)) {
            return NULL;
        }
        memcpy(parent_dir, workshop_dir, dlen);
        parent_dir[dlen] = '\0';
        return we_pkg_open_dir(parent_dir);
    }

    if (!S_ISDIR(st.st_mode)) {
        return NULL;
    }

    static const char *const pkg_names[] = {"scene.pkg", "gifscene.pkg"};
    for (size_t i = 0; i < sizeof(pkg_names) / sizeof(pkg_names[0]); ++i) {
        char candidate[4096];
        int n = snprintf(candidate, sizeof(candidate), "%s/%s", workshop_dir, pkg_names[i]);
        if (n > 0 && (size_t)n < sizeof(candidate) && access(candidate, R_OK) == 0) {
            we_pkg_t *pkg = we_pkg_open_file(candidate, workshop_dir);
            if (pkg) {
                return pkg;
            }
        }
    }

    /* Unpacked / loose-file Workshop directory */
    we_pkg_t *pkg = (we_pkg_t *)calloc(1, sizeof(we_pkg_t));
    if (!pkg) {
        return NULL;
    }
    pkg->fd = -1;
    snprintf(pkg->version_magic, sizeof(pkg->version_magic), "LOOSE");
    populate_steam_assets_dir(pkg, workshop_dir);
    return pkg;
}

void we_pkg_close(we_pkg_t *pkg)
{
    if (!pkg) {
        return;
    }
    free(pkg->entries);
    free(pkg->ht_slots);
    if (pkg->is_mmapped && pkg->base_ptr && pkg->mmap_size > 0) {
        munmap((void *)pkg->base_ptr, pkg->mmap_size);
    }
    if (pkg->fd >= 0) {
        close(pkg->fd);
    }
    free(pkg);
}

__attribute__((weak)) const char *we_shader_get_builtin(const char *path);

const uint8_t *we_pkg_find(const we_pkg_t *pkg, const char *rel_path, size_t *out_size)
{
    if (out_size) {
        *out_size = 0;
    }
    if (!pkg || !rel_path || !pkg->ht_slots || pkg->ht_cap == 0 || pkg->entry_count == 0) {
        return NULL;
    }

    char norm[WE_PKG_MAX_PATH_LEN + 1];
    if (!we_pkg_normalize_path(rel_path, strlen(rel_path), norm, sizeof(norm))) {
        return NULL;
    }

    size_t nlen = strlen(norm);
    uint64_t h = we_pkg_fnv1a64(norm, nlen);
    uint32_t mask = pkg->ht_cap - 1u;
    uint32_t slot = (uint32_t)h & mask;

    for (uint32_t probes = 0; probes < pkg->ht_cap; ++probes) {
        uint32_t idx1 = pkg->ht_slots[slot].entry_idx;
        if (idx1 == 0) {
            return NULL;
        }
        uint32_t idx = idx1 - 1u;
        if (pkg->ht_slots[slot].hash == h && strcmp(pkg->entries[idx].path, norm) == 0) {
            if (out_size) {
                *out_size = pkg->entries[idx].length;
            }
            return pkg->entries[idx].data;
        }
        slot = (slot + 1u) & mask;
    }

    return NULL;
}

static int open_case_insensitive_rel(const char *base_dir, const char *rel_path)
{
    char cur_path[4096];
    int n = snprintf(cur_path, sizeof(cur_path), "%s", base_dir);
    if (n <= 0 || (size_t)n >= sizeof(cur_path)) {
        return -1;
    }

    const char *p = rel_path;
    while (*p) {
        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *seg_end = strchr(p, '/');
        size_t seg_len = seg_end ? (size_t)(seg_end - p) : strlen(p);
        if (seg_len == 0 || seg_len >= 256u) {
            return -1;
        }
        char seg[256];
        memcpy(seg, p, seg_len);
        seg[seg_len] = '\0';

        /* Fast exact match check for this component */
        size_t cur_len = strlen(cur_path);
        if (cur_len + 1u + seg_len >= sizeof(cur_path)) {
            return -1;
        }
        cur_path[cur_len] = '/';
        memcpy(cur_path + cur_len + 1u, seg, seg_len + 1u);

        if (access(cur_path, F_OK) != 0) {
            /* Case-insensitive directory scan for component */
            cur_path[cur_len] = '\0';
            DIR *d = opendir(cur_path);
            if (!d) {
                return -1;
            }
            bool matched = false;
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (strcasecmp(de->d_name, seg) == 0) {
                    size_t dname_len = strlen(de->d_name);
                    if (cur_len + 1u + dname_len < sizeof(cur_path)) {
                        cur_path[cur_len] = '/';
                        memcpy(cur_path + cur_len + 1u, de->d_name, dname_len + 1u);
                        matched = true;
                    }
                    break;
                }
            }
            closedir(d);
            if (!matched) {
                return -1;
            }
        }
        p += seg_len;
    }

    return open(cur_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
}

static uint8_t *read_loose_file_with_case(const char *base_dir, const char *norm_rel,
                                          const char *case_rel, size_t *out_size)
{
    if (!base_dir || base_dir[0] == '\0' || !norm_rel || norm_rel[0] == '\0') {
        return NULL;
    }

    char full_path[4096];
    int n = snprintf(full_path, sizeof(full_path), "%s/%s", base_dir, norm_rel);
    if (n <= 0 || (size_t)n >= sizeof(full_path)) {
        return NULL;
    }

    int fd = open(full_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && case_rel && case_rel[0] != '\0' && strcmp(case_rel, norm_rel) != 0) {
        n = snprintf(full_path, sizeof(full_path), "%s/%s", base_dir, case_rel);
        if (n > 0 && (size_t)n < sizeof(full_path)) {
            fd = open(full_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        }
    }
    if (fd < 0) {
        fd = open_case_insensitive_rel(base_dir, case_rel && case_rel[0] ? case_rel : norm_rel);
    }
    if (fd < 0) {
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        st.st_size > (256LL * 1024LL * 1024LL)) {
        close(fd);
        return NULL;
    }

    size_t sz = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(sz + 1u);
    if (!buf) {
        close(fd);
        return NULL;
    }

    size_t total = 0;
    while (total < sz) {
        ssize_t r = read(fd, buf + total, sz - total);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buf);
            close(fd);
            return NULL;
        }
        if (r == 0) {
            break;
        }
        total += (size_t)r;
    }
    close(fd);

    if (total != sz) {
        free(buf);
        return NULL;
    }

    buf[sz] = '\0';
    if (out_size) {
        *out_size = sz;
    }
    return buf;
}

const uint8_t *we_pkg_resolve(const we_pkg_t *pkg, const char *rel_path, size_t *out_size,
                              bool *out_owned)
{
    if (out_size) {
        *out_size = 0;
    }
    if (out_owned) {
        *out_owned = false;
    }
    if (!rel_path) {
        return NULL;
    }

    if (pkg) {
        /* Layer 1: Zero-copy lookup in mmap'd scene.pkg / gifscene.pkg */
        const uint8_t *pkg_slice = we_pkg_find(pkg, rel_path, out_size);
        if (pkg_slice) {
            if (out_owned) {
                *out_owned = false;
            }
            return pkg_slice;
        }
    }

    size_t rlen = strlen(rel_path);
    char norm[WE_PKG_MAX_PATH_LEN + 1];
    if (!we_pkg_normalize_path(rel_path, rlen, norm, sizeof(norm))) {
        return NULL;
    }

    char case_rel[WE_PKG_MAX_PATH_LEN + 1];
    if (!normalize_path_internal(rel_path, rlen, case_rel, sizeof(case_rel), false)) {
        memcpy(case_rel, norm, strlen(norm) + 1u);
    }

    if (pkg) {
        /* Layer 2: Loose file in <workshop_dir>/<norm> (with case-preserving / case-insensitive
         * fallback) */
        uint8_t *loose = read_loose_file_with_case(pkg->workshop_dir, norm, case_rel, out_size);
        if (loose) {
            if (out_owned) {
                *out_owned = true;
            }
            return loose;
        }

        /* Layer 3: Steam Wallpaper Engine standard assets directory <assets_dir>/<norm> */
        loose = read_loose_file_with_case(pkg->assets_dir, norm, case_rel, out_size);
        if (loose) {
            if (out_owned) {
                *out_owned = true;
            }
            return loose;
        }

        /* If path does not start with "shaders/", retry Layers 1-3 with "shaders/<path>"
         * before falling through to Layer 4 built-in shaders. */
        if (strncmp(norm, "shaders/", 8) != 0) {
            char sh_norm[WE_PKG_MAX_PATH_LEN + 16];
            char sh_case[WE_PKG_MAX_PATH_LEN + 16];
            int n1 = snprintf(sh_norm, sizeof(sh_norm), "shaders/%s", norm);
            int n2 = snprintf(sh_case, sizeof(sh_case), "shaders/%s", case_rel);
            if (n1 > 0 && (size_t)n1 <= WE_PKG_MAX_PATH_LEN && n2 > 0 &&
                (size_t)n2 <= WE_PKG_MAX_PATH_LEN) {
                const uint8_t *pkg_slice = we_pkg_find(pkg, sh_norm, out_size);
                if (pkg_slice) {
                    if (out_owned) {
                        *out_owned = false;
                    }
                    return pkg_slice;
                }
                loose = read_loose_file_with_case(pkg->workshop_dir, sh_norm, sh_case, out_size);
                if (loose) {
                    if (out_owned) {
                        *out_owned = true;
                    }
                    return loose;
                }
                loose = read_loose_file_with_case(pkg->assets_dir, sh_norm, sh_case, out_size);
                if (loose) {
                    if (out_owned) {
                        *out_owned = true;
                    }
                    return loose;
                }
            }
        }
    }

    /* Layer 4: Built-in standard shaders/assets fallback */
    if (we_shader_get_builtin) {
        const char *builtin = we_shader_get_builtin(norm);
        if (builtin) {
            if (out_size) {
                *out_size = strlen(builtin);
            }
            if (out_owned) {
                *out_owned = false;
            }
            return (const uint8_t *)builtin;
        }
    }

    return NULL;
}
