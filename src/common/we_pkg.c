// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/we_pkg.h"

#include "waywal/log.h"
#include "waywal/os_compat.h"

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
        (".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/wallpaper_engine/"
         "assets"),
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
                /* Reject duplicate normalized paths so iteration matches hash lookups. */
                free(ht_slots);
                free(entries);
                return false;
            }
            slot = (slot + 1u) & mask;
        }
        ht_slots[slot].hash = h;
        ht_slots[slot].entry_idx = i + 1u;
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
    if (!data || size < 16u) {
        return NULL;
    }

    uint32_t magic_len = read_u32_le(data);
    if ((magic_len != 8u && magic_len != 9u) || 4u + (size_t)magic_len + 4u > size) {
        return NULL;
    }

    const char *magic_ptr = (const char *)(data + 4u);
    if (memcmp(magic_ptr, "PKGV", 4u) != 0) {
        return NULL;
    }
    if (magic_len == 9u && magic_ptr[8] != '\0') {
        return NULL;
    }

    char ver_str[16];
    memcpy(ver_str, magic_ptr, 8u);
    ver_str[8] = '\0';

    for (size_t i = 4u; i < 8u; ++i) {
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
    pkg->owns_base_ptr = false;
    pkg->fd = -1;
    snprintf(pkg->version_magic, sizeof(pkg->version_magic), "%s", ver_str);
    pkg->version_num = ver_num;
    populate_steam_assets_dir(pkg, workshop_dir);

    size_t table_cursor = 4u + (size_t)magic_len;
    if (!parse_pkg_table_at(pkg, data, size, table_cursor)) {
        free(pkg);
        return NULL;
    }

    return pkg;
}

static we_pkg_t *pkg_open_from_fd(int fd, const char *pkg_path, const char *workshop_dir)
{
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 16 ||
        st.st_size > (1024LL * 1024LL * 1024LL)) {
        close(fd);
        return NULL;
    }

    size_t buf_sz = (size_t)st.st_size;
    /* Copy into heap buffer so concurrent external file truncation cannot trigger SIGBUS. */
    uint8_t *buf = (uint8_t *)malloc(buf_sz);
    if (!buf) {
        close(fd);
        return NULL;
    }

    size_t total = 0u;
    while (total < buf_sz) {
        ssize_t r = read(fd, buf + total, buf_sz - total);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buf);
            close(fd);
            return NULL;
        }
        if (r == 0) {
            free(buf);
            close(fd);
            return NULL;
        }
        total += (size_t)r;
    }
    close(fd);

    char derived_dir[4096] = {0};
    if (!workshop_dir || workshop_dir[0] == '\0') {
        if (pkg_path && pkg_path[0] != '\0') {
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
    }

    we_pkg_t *pkg = we_pkg_open_mem(buf, buf_sz, workshop_dir);
    if (!pkg) {
        free(buf);
        return NULL;
    }
    pkg->owns_base_ptr = true;
    return pkg;
}

we_pkg_t *we_pkg_open_file(const char *pkg_path, const char *workshop_dir)
{
    if (!pkg_path || pkg_path[0] == '\0') {
        return NULL;
    }

    int fd = open(pkg_path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }
    return pkg_open_from_fd(fd, pkg_path, workshop_dir);
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

    int ws_dfd = open(workshop_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (ws_dfd >= 0) {
        static const char *const pkg_names[] = {"scene.pkg", "gifscene.pkg"};
        for (size_t i = 0; i < sizeof(pkg_names) / sizeof(pkg_names[0]); ++i) {
            int pfd = wwal_openat2_beneath(ws_dfd, pkg_names[i], O_RDONLY | O_NONBLOCK, 0);
            if (pfd >= 0) {
                we_pkg_t *pkg = pkg_open_from_fd(pfd, NULL, workshop_dir);
                if (pkg) {
                    close(ws_dfd);
                    return pkg;
                }
            }
        }
        close(ws_dfd);
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
    if (pkg->owns_base_ptr && pkg->base_ptr) {
        free((void *)pkg->base_ptr);
    } else if (pkg->is_mmapped && pkg->base_ptr && pkg->mmap_size > 0) {
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

static int open_case_insensitive_rel(int base_dfd, const char *rel_path)
{
    if (base_dfd < 0 || !rel_path || rel_path[0] == '\0' || rel_path[0] == '/') {
        return -1;
    }

    int cur_dfd = fcntl(base_dfd, F_DUPFD_CLOEXEC, 0);
    if (cur_dfd < 0) {
        return -1;
    }

    const char *p = rel_path;
    while (*p != '\0') {
        const char *seg_end = strchr(p, '/');
        bool is_last = (seg_end == NULL);
        size_t seg_len = seg_end ? (size_t)(seg_end - p) : strlen(p);
        if (seg_len == 0u || seg_len >= 256u) {
            close(cur_dfd);
            return -1;
        }
        char seg[256];
        memcpy(seg, p, seg_len);
        seg[seg_len] = '\0';

        if (strcmp(seg, ".") == 0 || strcmp(seg, "..") == 0) {
            close(cur_dfd);
            return -1;
        }

        if (is_last) {
            int fd = openat(cur_dfd, seg, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0 || errno != ENOENT) {
                close(cur_dfd);
                return fd;
            }

            /* Scan single directory level when exact-case filename is missing (never follows
             * symlinks). */
            int scan_dfd = fcntl(cur_dfd, F_DUPFD_CLOEXEC, 0);
            if (scan_dfd < 0) {
                close(cur_dfd);
                return -1;
            }
            DIR *d = fdopendir(scan_dfd);
            if (!d) {
                close(scan_dfd);
                close(cur_dfd);
                return -1;
            }
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
                    continue;
                }
                if (strcasecmp(de->d_name, seg) == 0) {
                    struct stat st;
                    if (fstatat(cur_dfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
                        S_ISREG(st.st_mode)) {
                        fd = openat(cur_dfd, de->d_name,
                                    O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
                    }
                    break;
                }
            }
            closedir(d);
            close(cur_dfd);
            return fd;
        }

        int next_dfd = openat(cur_dfd, seg, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next_dfd < 0 && errno == ENOENT) {
            int scan_dfd = fcntl(cur_dfd, F_DUPFD_CLOEXEC, 0);
            if (scan_dfd >= 0) {
                DIR *d = fdopendir(scan_dfd);
                if (d) {
                    struct dirent *de;
                    while ((de = readdir(d)) != NULL) {
                        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
                            continue;
                        }
                        if (strcasecmp(de->d_name, seg) == 0) {
                            struct stat st;
                            if (fstatat(cur_dfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
                                S_ISDIR(st.st_mode)) {
                                next_dfd = openat(cur_dfd, de->d_name,
                                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                            }
                            break;
                        }
                    }
                    closedir(d);
                } else {
                    close(scan_dfd);
                }
            }
        }
        close(cur_dfd);
        if (next_dfd < 0) {
            return -1;
        }
        cur_dfd = next_dfd;
        p = seg_end + 1;
    }

    close(cur_dfd);
    return -1;
}

static uint8_t *read_loose_file_with_case(const char *base_dir, const char *norm_rel,
                                          const char *case_rel, size_t *out_size)
{
    if (!base_dir || base_dir[0] == '\0' || !norm_rel || norm_rel[0] == '\0') {
        return NULL;
    }

    int base_dfd = open(base_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (base_dfd < 0) {
        return NULL;
    }

    int fd = wwal_openat2_beneath(base_dfd, norm_rel, O_RDONLY | O_NONBLOCK, 0);
    if (fd < 0 && (errno == ENOENT || errno == ENOTDIR) && case_rel && case_rel[0] != '\0' &&
        strcmp(case_rel, norm_rel) != 0) {
        fd = wwal_openat2_beneath(base_dfd, case_rel, O_RDONLY | O_NONBLOCK, 0);
    }
    if (fd < 0 && errno == ENOENT) {
        const char *fallback_rel = (case_rel && case_rel[0] != '\0') ? case_rel : norm_rel;
        fd = open_case_insensitive_rel(base_dfd, fallback_rel);
    }
    close(base_dfd);
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
