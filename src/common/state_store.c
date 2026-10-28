// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/state_store.h"

#include "waywal/image_loader.h"
#include "waywal/log.h"
#include "waywal/we_scene.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool ensure_parent_dirs(const char *file_path)
{
    if (!file_path || file_path[0] == '\0') {
        return false;
    }

    path_buf_t dir;
    path_buf_from_str(&dir, file_path);
    if (!path_buf_parent(&dir)) {
        return true;
    }

    char tmp[WAYWAL_PATH_MAX];
    size_t len = strlen(dir.data);
    if (len == 0 || len >= sizeof(tmp)) {
        return false;
    }
    memcpy(tmp, dir.data, len + 1);

    struct stat st;
    for (size_t i = 1; i < len; ++i) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (!(stat(tmp, &st) == 0 && S_ISDIR(st.st_mode))) {
                if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                    return false;
                }
            }
            tmp[i] = '/';
        }
    }
    if (!(stat(tmp, &st) == 0 && S_ISDIR(st.st_mode))) {
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            return false;
        }
    }
    return true;
}

static bool write_all_bytes(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t written = write(fd, p, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        p += (size_t)written;
        remaining -= (size_t)written;
    }
    return true;
}

static bool read_all_bytes(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t nread = read(fd, p, remaining);
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (nread == 0) {
            return false;
        }
        p += (size_t)nread;
        remaining -= (size_t)nread;
    }
    return true;
}

static char g_state_namespace[64] = "";

void waywal_state_store_set_namespace(const char *namespace_str)
{
    if (!namespace_str || namespace_str[0] == '\0') {
        g_state_namespace[0] = '\0';
        return;
    }
    size_t j = 0;
    for (size_t i = 0; namespace_str[i] != '\0' && j + 1 < sizeof(g_state_namespace); ++i) {
        char c = namespace_str[i];
        g_state_namespace[j++] = (c == '/' || c == '\\') ? '_' : c;
    }
    g_state_namespace[j] = '\0';
}

void waywal_state_store_init(waywal_state_store_t *store)
{
    if (!store) {
        return;
    }
    memset(store, 0, sizeof(*store));
    store->magic = WAYWAL_STATE_MAGIC;
    store->version = WAYWAL_STATE_VERSION;
}

static bool resolve_state_file_with_name(path_buf_t *out_path, const char *override_dir,
                                         const char *filename)
{
    if (!out_path || !filename || filename[0] == '\0') {
        return false;
    }
    path_buf_init(out_path);

    const char *env_override = override_dir;
    if (!env_override || env_override[0] == '\0') {
        env_override = getenv("WWAL_STATE_DIR");
    }

    if (env_override && env_override[0] != '\0') {
        path_buf_from_str(out_path, env_override);
        return path_buf_push(out_path, filename);
    }

    const char *xdg_state = getenv("XDG_STATE_HOME");
    if (xdg_state && xdg_state[0] != '\0') {
        path_buf_from_str(out_path, xdg_state);
        if (!path_buf_push(out_path, "wwal")) {
            return false;
        }
        return path_buf_push(out_path, filename);
    }

    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') {
        return false;
    }

    path_buf_from_str(out_path, home);
    if (!path_buf_push(out_path, ".local/state/wwal")) {
        return false;
    }
    return path_buf_push(out_path, filename);
}

bool waywal_state_store_resolve_path(path_buf_t *out_path, const char *override_dir)
{
    char filename[128] = "state.bin";
    if (g_state_namespace[0] != '\0' && strcmp(g_state_namespace, "default") != 0) {
        snprintf(filename, sizeof(filename), "state-%s.bin", g_state_namespace);
    }
    return resolve_state_file_with_name(out_path, override_dir, filename);
}

bool waywal_state_store_save_at(const waywal_state_store_t *store, const char *file_path)
{
    if (!store || !file_path || file_path[0] == '\0') {
        return false;
    }
    if (store->count > WAYWAL_STATE_MAX_OUTPUTS) {
        return false;
    }
    if (!ensure_parent_dirs(file_path)) {
        return false;
    }

    char tmp_path[WAYWAL_PATH_MAX];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", file_path, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        return false;
    }

    waywal_state_store_t snapshot = *store;
    snapshot.magic = WAYWAL_STATE_MAGIC;
    snapshot.version = WAYWAL_STATE_VERSION;
    snapshot.reserved = 0;

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }

    bool ok = write_all_bytes(fd, &snapshot, sizeof(snapshot));
    if (ok && fdatasync(fd) != 0) {
        ok = false;
    }
    close(fd);

    if (!ok) {
        unlink(tmp_path);
        return false;
    }

    if (rename(tmp_path, file_path) != 0) {
        unlink(tmp_path);
        return false;
    }
    return true;
}

bool waywal_state_store_load_at(waywal_state_store_t *store, const char *file_path)
{
    if (!store || !file_path || file_path[0] == '\0') {
        return false;
    }

    int fd = open(file_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    waywal_state_store_t loaded;
    bool ok = read_all_bytes(fd, &loaded, sizeof(loaded));
    close(fd);

    if (!ok) {
        return false;
    }
    if (loaded.magic != WAYWAL_STATE_MAGIC || loaded.version != WAYWAL_STATE_VERSION ||
        loaded.count > WAYWAL_STATE_MAX_OUTPUTS) {
        return false;
    }

    for (uint32_t i = 0; i < loaded.count; ++i) {
        loaded.outputs[i].output_name[sizeof(loaded.outputs[i].output_name) - 1] = '\0';
        loaded.outputs[i].media_path[sizeof(loaded.outputs[i].media_path) - 1] = '\0';
        loaded.outputs[i].prop_overrides[sizeof(loaded.outputs[i].prop_overrides) - 1] = '\0';
    }

    *store = loaded;
    return true;
}

bool(waywal_state_store_save)(const waywal_state_store_t *store)
{
    return waywal_state_store_save_dir(store, NULL);
}

bool(waywal_state_store_load)(waywal_state_store_t *store)
{
    return waywal_state_store_load_dir(store, NULL);
}

bool waywal_state_store_save_dir(const waywal_state_store_t *store, const char *override_dir)
{
    if (override_dir && strlen(override_dir) >= 4 &&
        strcmp(override_dir + strlen(override_dir) - 4, ".bin") == 0) {
        return waywal_state_store_save_at(store, override_dir);
    }
    path_buf_t resolved_path;
    if (!waywal_state_store_resolve_path(&resolved_path, override_dir)) {
        return false;
    }
    return waywal_state_store_save_at(store, path_buf_cstr(&resolved_path));
}

bool waywal_state_store_load_dir(waywal_state_store_t *store, const char *override_dir)
{
    if (override_dir && strlen(override_dir) >= 4 &&
        strcmp(override_dir + strlen(override_dir) - 4, ".bin") == 0) {
        return waywal_state_store_load_at(store, override_dir);
    }
    path_buf_t resolved_path;
    if (!waywal_state_store_resolve_path(&resolved_path, override_dir)) {
        return false;
    }
    return waywal_state_store_load_at(store, path_buf_cstr(&resolved_path));
}

bool waywal_state_store_update_output(waywal_state_store_t *store,
                                      const waywal_output_saved_state_t *state)
{
    if (!store || !state || state->output_name[0] == '\0') {
        return false;
    }

    if (store->magic != WAYWAL_STATE_MAGIC || store->version != WAYWAL_STATE_VERSION) {
        waywal_state_store_init(store);
    }

    for (uint32_t i = 0; i < store->count; ++i) {
        if (strncmp(store->outputs[i].output_name, state->output_name,
                    sizeof(store->outputs[i].output_name)) == 0) {
            store->outputs[i] = *state;
            store->outputs[i].output_name[sizeof(store->outputs[i].output_name) - 1] = '\0';
            store->outputs[i].media_path[sizeof(store->outputs[i].media_path) - 1] = '\0';
            store->outputs[i].prop_overrides[sizeof(store->outputs[i].prop_overrides) - 1] = '\0';
            return true;
        }
    }

    if (store->count >= WAYWAL_STATE_MAX_OUTPUTS) {
        return false;
    }

    store->outputs[store->count] = *state;
    store->outputs[store->count].output_name[sizeof(store->outputs[0].output_name) - 1] = '\0';
    store->outputs[store->count].media_path[sizeof(store->outputs[0].media_path) - 1] = '\0';
    store->outputs[store->count].prop_overrides[sizeof(store->outputs[0].prop_overrides) - 1] =
        '\0';
    store->count++;
    return true;
}

const waywal_output_saved_state_t *waywal_state_store_find_output(const waywal_state_store_t *store,
                                                                  const char *output_name)
{
    if (!store || store->magic != WAYWAL_STATE_MAGIC || store->count > WAYWAL_STATE_MAX_OUTPUTS) {
        return NULL;
    }

    if (output_name && output_name[0] != '\0') {
        for (uint32_t i = 0; i < store->count; ++i) {
            if (strcmp(store->outputs[i].output_name, output_name) == 0) {
                return &store->outputs[i];
            }
        }
    }

    for (uint32_t i = 0; i < store->count; ++i) {
        if (strcmp(store->outputs[i].output_name, "*") == 0) {
            return &store->outputs[i];
        }
    }

    return NULL;
}

const char *waywal_filter_mode_to_swww_str(waywal_filter_mode_t filter)
{
    switch (filter) {
    case WAYWAL_FILTER_MITCHELL:
        return "CatmullRom";
    case WAYWAL_FILTER_BILINEAR:
        return "Bilinear";
    case WAYWAL_FILTER_NEAREST:
        return "Nearest";
    case WAYWAL_FILTER_LANCZOS3:
    default:
        return "Lanczos3";
    }
}

static bool write_text_file_atomic(const char *file_path, const char *content, size_t len)
{
    if (!file_path || file_path[0] == '\0' || !content) {
        return false;
    }
    if (!ensure_parent_dirs(file_path)) {
        return false;
    }

    char tmp_path[WAYWAL_PATH_MAX];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", file_path, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        return false;
    }

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    bool ok = write_all_bytes(fd, content, len);
    close(fd);
    if (!ok) {
        unlink(tmp_path);
        return false;
    }
    if (rename(tmp_path, file_path) != 0) {
        unlink(tmp_path);
        return false;
    }
    return true;
}

static bool file_has_png_magic(const char *file_path)
{
    if (!file_path || file_path[0] == '\0') {
        return false;
    }
    int fd = open(file_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 24 ||
        st.st_size > 32L * 1024L * 1024L) {
        close(fd);
        return false;
    }
    uint8_t sig[8] = {0};
    bool ok = read_all_bytes(fd, sig, sizeof(sig));
    close(fd);
    static const uint8_t png_sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    return ok && memcmp(sig, png_sig, sizeof(png_sig)) == 0;
}

static bool png_file_is_within_bounds(const char *file_path, uint32_t max_w, uint32_t max_h)
{
    if (!file_path || file_path[0] == '\0') {
        return false;
    }
    int fd = open(file_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 24 ||
        st.st_size > 32L * 1024L * 1024L) {
        close(fd);
        return false;
    }
    uint8_t hdr[24] = {0};
    bool ok = read_all_bytes(fd, hdr, sizeof(hdr));
    close(fd);
    static const uint8_t png_sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (!ok || memcmp(hdr, png_sig, sizeof(png_sig)) != 0 || memcmp(hdr + 12, "IHDR", 4) != 0) {
        return false;
    }
    uint32_t pw = ((uint32_t)hdr[16] << 24) | ((uint32_t)hdr[17] << 16) | ((uint32_t)hdr[18] << 8) |
                  (uint32_t)hdr[19];
    uint32_t ph = ((uint32_t)hdr[20] << 24) | ((uint32_t)hdr[21] << 16) | ((uint32_t)hdr[22] << 8) |
                  (uint32_t)hdr[23];
    return pw > 0 && ph > 0 && pw <= max_w && ph <= max_h;
}

static bool copy_file_atomic(const char *src_path, const char *dst_path)
{
    if (!src_path || !dst_path) {
        return false;
    }
    if (strcmp(src_path, dst_path) == 0) {
        return access(dst_path, R_OK) == 0;
    }
    if (!ensure_parent_dirs(dst_path)) {
        return false;
    }

    int sfd = open(src_path, O_RDONLY | O_CLOEXEC);
    if (sfd < 0) {
        return false;
    }

    char tmp_path[WAYWAL_PATH_MAX];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", dst_path, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        close(sfd);
        return false;
    }

    int dfd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (dfd < 0) {
        close(sfd);
        return false;
    }

    uint8_t buf[65536];
    bool ok = true;
    for (;;) {
        ssize_t r = read(sfd, buf, sizeof(buf));
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
            break;
        }
        if (r == 0) {
            break;
        }
        if (!write_all_bytes(dfd, buf, (size_t)r)) {
            ok = false;
            break;
        }
    }

    close(sfd);
    close(dfd);

    if (!ok || rename(tmp_path, dst_path) != 0) {
        unlink(tmp_path);
        return false;
    }
    return true;
}

static uint64_t hash_file_identity_u64(const char *path)
{
    uint64_t h = 14695981039346656037ULL;
    for (const unsigned char *p = (const unsigned char *)path; p && *p; ++p) {
        h ^= (uint64_t)(*p);
        h *= 1099511628211ULL;
    }
    struct stat st;
    if (path && stat(path, &st) == 0) {
        uint64_t sz = (uint64_t)st.st_size;
        uint64_t mt = (uint64_t)st.st_mtim.tv_sec ^ ((uint64_t)st.st_mtim.tv_nsec << 32);
        for (int i = 0; i < 8; ++i) {
            h ^= (sz >> (i * 8)) & 0xFFULL;
            h *= 1099511628211ULL;
            h ^= (mt >> (i * 8)) & 0xFFULL;
            h *= 1099511628211ULL;
        }
    }
    return h;
}

bool waywal_render_shader_preview_bgra(const char *shader_path, uint8_t *out_bgra, uint32_t width,
                                       uint32_t height)
{
    if (!out_bgra || width == 0 || height == 0 || width > 4096 || height > 4096) {
        return false;
    }

    uint64_t hash = 14695981039346656037ULL;
    float stops[3][3] = {
        {0.12f, 0.14f, 0.28f},
        {0.25f, 0.55f, 0.88f},
        {0.88f, 0.35f, 0.62f},
    };
    size_t found_stops = 0;

    if (shader_path && shader_path[0] != '\0') {
        char resolved_sh[WAYWAL_PATH_MAX];
        const char *effective_sh =
            waywal_shader_resolve_path(shader_path, resolved_sh, sizeof(resolved_sh))
                ? resolved_sh
                : shader_path;
        for (const unsigned char *p = (const unsigned char *)effective_sh; *p; ++p) {
            hash ^= (uint64_t)(*p);
            hash *= 1099511628211ULL;
        }
        int fd = open(effective_sh, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            char src[16384];
            ssize_t n = read(fd, src, sizeof(src) - 1);
            close(fd);
            if (n > 0) {
                src[n] = '\0';
                for (ssize_t i = 0; i < n; ++i) {
                    hash ^= (uint8_t)src[i];
                    hash *= 1099511628211ULL;
                }
                const char *cur = src;
                while (found_stops < 3 && (cur = strstr(cur, "vec3(")) != NULL) {
                    cur += 5;
                    float r = 0.0f, g = 0.0f, b = 0.0f;
                    if (sscanf(cur, " %f , %f , %f", &r, &g, &b) == 3 && isfinite(r) &&
                        isfinite(g) && isfinite(b)) {
                        if (r >= 0.0f && r <= 1.5f && g >= 0.0f && g <= 1.5f && b >= 0.0f &&
                            b <= 1.5f) {
                            stops[found_stops][0] = r > 1.0f ? 1.0f : r;
                            stops[found_stops][1] = g > 1.0f ? 1.0f : g;
                            stops[found_stops][2] = b > 1.0f ? 1.0f : b;
                            found_stops++;
                        }
                    }
                }
            }
        }
    }

    for (size_t k = found_stops; k < 3; ++k) {
        uint64_t s = hash ^ (0x9E3779B97F4A7C15ULL * (k + 1));
        stops[k][0] = 0.12f + 0.78f * (float)((s >> 0) & 0xFFU) / 255.0f;
        stops[k][1] = 0.12f + 0.78f * (float)((s >> 8) & 0xFFU) / 255.0f;
        stops[k][2] = 0.12f + 0.78f * (float)((s >> 16) & 0xFFU) / 255.0f;
    }

    float phase_x = (float)((hash >> 24) & 0xFFU) * 0.024f;
    float phase_y = (float)((hash >> 32) & 0xFFU) * 0.024f;

    for (uint32_t y = 0; y < height; ++y) {
        float v = (float)y / (float)(height > 1 ? height - 1 : 1);
        for (uint32_t x = 0; x < width; ++x) {
            float u = (float)x / (float)(width > 1 ? width - 1 : 1);
            float w1 = 0.5f + 0.5f * sinf(u * 4.5f + phase_x + cosf(v * 3.5f + phase_y));
            float w2 = 0.5f + 0.5f * cosf((u - v) * 5.0f - phase_x);
            float r = stops[0][0] * (1.0f - w1) + stops[1][0] * w1 * (1.0f - w2) +
                      stops[2][0] * w1 * w2;
            float g = stops[0][1] * (1.0f - w1) + stops[1][1] * w1 * (1.0f - w2) +
                      stops[2][1] * w1 * w2;
            float b = stops[0][2] * (1.0f - w1) + stops[1][2] * w1 * (1.0f - w2) +
                      stops[2][2] * w1 * w2;

            float dx = u - 0.5f;
            float dy = v - 0.5f;
            float vig = 1.0f - 0.35f * (dx * dx + dy * dy);
            r = fminf(1.0f, fmaxf(0.0f, r * vig));
            g = fminf(1.0f, fmaxf(0.0f, g * vig));
            b = fminf(1.0f, fmaxf(0.0f, b * vig));

            size_t idx = ((size_t)y * (size_t)width + (size_t)x) * 4U;
            out_bgra[idx + 0] = (uint8_t)(b * 255.0f + 0.5f);
            out_bgra[idx + 1] = (uint8_t)(g * 255.0f + 0.5f);
            out_bgra[idx + 2] = (uint8_t)(r * 255.0f + 0.5f);
            out_bgra[idx + 3] = 255U;
        }
    }
    return true;
}

static bool is_valid_swww_output_name(const char *name)
{
    if (!name || name[0] == '\0' || strcmp(name, "*") == 0 || strcmp(name, "unknown") == 0 ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return false;
    }
    if (strncasecmp(name, "desc:", 5) == 0 || strchr(name, ':') != NULL ||
        strchr(name, '/') != NULL || strchr(name, '\\') != NULL || strchr(name, ',') != NULL ||
        strstr(name, "..") != NULL) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        if (isspace(*p) || *p < 0x20 || *p == 0x7F) {
            return false;
        }
    }
    return true;
}

bool waywal_ecosystem_cache_sync(const char *output_name, const char *media_path,
                                 const char *filter_str, const uint8_t *bgra_pixels, uint32_t width,
                                 uint32_t height, bool caelestia_sync, bool no_cache)
{
    if (no_cache) {
        return true;
    }

    const char *home = getenv("HOME");
    const char *xdg_cache = getenv("XDG_CACHE_HOME");
    const char *wwal_cache_env = getenv("WWAL_CACHE_DIR");

    char wwal_cache_dir[WAYWAL_PATH_MAX] = {0};
    if (wwal_cache_env && wwal_cache_env[0] != '\0') {
        snprintf(wwal_cache_dir, sizeof(wwal_cache_dir), "%s", wwal_cache_env);
    } else if (xdg_cache && xdg_cache[0] != '\0') {
        snprintf(wwal_cache_dir, sizeof(wwal_cache_dir), "%s/wwal", xdg_cache);
    } else if (home && home[0] != '\0') {
        snprintf(wwal_cache_dir, sizeof(wwal_cache_dir), "%s/.cache/wwal", home);
    } else {
        return false;
    }

    char link_path[WAYWAL_PATH_MAX];
    snprintf(link_path, sizeof(link_path), "%s/current_wallpaper", wwal_cache_dir);

    char abs_media[WAYWAL_PATH_MAX] = {0};
    if (media_path && media_path[0] != '\0' && strcmp(media_path, "-") != 0) {
        if (realpath(media_path, abs_media) == NULL) {
            if (media_path[0] == '/') {
                snprintf(abs_media, sizeof(abs_media), "%s", media_path);
            } else {
                char cwd[WAYWAL_PATH_MAX];
                if (getcwd(cwd, sizeof(cwd)) != NULL) {
                    snprintf(abs_media, sizeof(abs_media), "%s/%s", cwd, media_path);
                } else {
                    snprintf(abs_media, sizeof(abs_media), "%s", media_path);
                }
            }
        }
        if (strcmp(abs_media, link_path) == 0) {
            abs_media[0] = '\0';
        }
    }

    char png_path[WAYWAL_PATH_MAX];
    snprintf(png_path, sizeof(png_path), "%s/current_wallpaper.png", wwal_cache_dir);
    (void)ensure_parent_dirs(png_path);

    bool png_written = false;
    if (abs_media[0] != '\0' && png_file_is_within_bounds(abs_media, 1280, 720) && width > 0 &&
        height > 0 && width <= 1280 && height <= 720) {
        png_written = copy_file_atomic(abs_media, png_path);
    }

    if (!png_written && bgra_pixels && width > 0 && height > 0) {
        png_written = image_save_png_bgra(png_path, bgra_pixels, width, height);
    }

    if (!png_written && abs_media[0] != '\0') {
        char ws_preview[WAYWAL_PATH_MAX];
        if (we_workshop_find_preview_file(abs_media, ws_preview, sizeof(ws_preview))) {
            uint8_t *px = NULL;
            uint32_t pw = 0, ph = 0;
            if (image_load(ws_preview, &px, &pw, &ph) && px && pw > 0 && ph > 0) {
                if (file_has_png_magic(ws_preview) && pw <= 1280 && ph <= 720) {
                    png_written = copy_file_atomic(ws_preview, png_path);
                }
                if (!png_written) {
                    png_written = image_save_png_bgra(png_path, px, pw, ph);
                }
            }
            if (px) {
                image_free(px);
            }
        }
    }

    if (!png_written && abs_media[0] != '\0') {
        uint64_t h1 = hash_file_identity_u64(abs_media);
        uint64_t h2 = hash_file_identity_u64(media_path);
        uint64_t hashes[2] = {h1, h2};
        for (size_t i = 0; i < 2 && !png_written; ++i) {
            char thumb_png[WAYWAL_PATH_MAX];
            snprintf(thumb_png, sizeof(thumb_png), "%s/thumbs/%016llx.png", wwal_cache_dir,
                     (unsigned long long)hashes[i]);
            if (file_has_png_magic(thumb_png)) {
                png_written = copy_file_atomic(thumb_png, png_path);
                if (png_written) {
                    break;
                }
            }

            char thumb_bgra[WAYWAL_PATH_MAX];
            snprintf(thumb_bgra, sizeof(thumb_bgra), "%s/thumbs/%016llx.bgra", wwal_cache_dir,
                     (unsigned long long)hashes[i]);
            int tfd = open(thumb_bgra, O_RDONLY | O_CLOEXEC);
            if (tfd >= 0) {
                struct stat tst;
                if (fstat(tfd, &tst) == 0 && (size_t)tst.st_size == 128U * 128U * 4U) {
                    uint8_t *tbuf = (uint8_t *)malloc(128U * 128U * 4U);
                    if (tbuf) {
                        if (read_all_bytes(tfd, tbuf, 128U * 128U * 4U)) {
                            png_written = image_save_png_bgra(png_path, tbuf, 128U, 128U);
                        }
                        free(tbuf);
                    }
                }
                close(tfd);
            }
        }
    }

    if (!png_written && abs_media[0] != '\0') {
        struct stat st;
        const char *ext = strrchr(abs_media, '.');
        bool is_non_image_ext =
            ext && (strcasecmp(ext, ".mp4") == 0 || strcasecmp(ext, ".webm") == 0 ||
                    strcasecmp(ext, ".mkv") == 0 || strcasecmp(ext, ".avi") == 0 ||
                    strcasecmp(ext, ".mov") == 0 || strcasecmp(ext, ".m4v") == 0 ||
                    strcasecmp(ext, ".frag") == 0 || strcasecmp(ext, ".glsl") == 0 ||
                    strcasecmp(ext, ".vert") == 0 || strcasecmp(ext, ".comp") == 0);
        if (!is_non_image_ext && stat(abs_media, &st) == 0 && S_ISREG(st.st_mode)) {
            uint8_t *px = NULL;
            uint32_t pw = 0, ph = 0;
            if (image_load(abs_media, &px, &pw, &ph) && px && pw > 0 && ph > 0) {
                if (file_has_png_magic(abs_media) && pw <= 1280 && ph <= 720) {
                    png_written = copy_file_atomic(abs_media, png_path);
                }
                if (!png_written) {
                    png_written = image_save_png_bgra(png_path, px, pw, ph);
                }
            }
            if (px) {
                image_free(px);
            }
        }
    }

    if (!png_written) {
        uint8_t *fallback_bgra = (uint8_t *)malloc(320U * 180U * 4U);
        if (fallback_bgra) {
            if (waywal_render_shader_preview_bgra(abs_media[0] ? abs_media : media_path,
                                                  fallback_bgra, 320U, 180U)) {
                png_written = image_save_png_bgra(png_path, fallback_bgra, 320U, 180U);
            }
            free(fallback_bgra);
        }
    }

    char resolved_ws_preview[WAYWAL_PATH_MAX] = {0};
    const char *effective_file_target = png_path;
    if (abs_media[0] != '\0' && strcmp(abs_media, link_path) != 0) {
        struct stat mst;
        if (stat(abs_media, &mst) == 0 && S_ISDIR(mst.st_mode)) {
            char ws_prev[WAYWAL_PATH_MAX];
            if (we_workshop_find_preview_file(abs_media, ws_prev, sizeof(ws_prev))) {
                if (realpath(ws_prev, resolved_ws_preview) == NULL) {
                    snprintf(resolved_ws_preview, sizeof(resolved_ws_preview), "%s", ws_prev);
                }
                effective_file_target = resolved_ws_preview;
            } else {
                effective_file_target = png_path;
            }
        } else {
            effective_file_target = abs_media;
        }
    }

    const char *symlink_target = effective_file_target;
    if (symlink_target[0] != '\0') {
        char tmp_link[WAYWAL_PATH_MAX];
        snprintf(tmp_link, sizeof(tmp_link), "%s/current_wallpaper.tmp.%d", wwal_cache_dir,
                 (int)getpid());
        unlink(tmp_link);
        if (symlink(symlink_target, tmp_link) == 0) {
            if (rename(tmp_link, link_path) != 0) {
                unlink(tmp_link);
            }
        }
    }

    if (output_name && output_name[0] != '\0') {
        const char *filt = (filter_str && filter_str[0] != '\0') ? filter_str : "Lanczos3";
        const char *target = effective_file_target;
        char payload[WAYWAL_PATH_MAX + 64];
        int plen = snprintf(payload, sizeof(payload), "%s\n%s\n", filt, target);
        if (plen > 0 && (size_t)plen < sizeof(payload)) {
            char cache_base[WAYWAL_PATH_MAX] = {0};
            if (xdg_cache && xdg_cache[0] != '\0') {
                snprintf(cache_base, sizeof(cache_base), "%s", xdg_cache);
            } else if (home && home[0] != '\0') {
                snprintf(cache_base, sizeof(cache_base), "%s/.cache", home);
            }
            if (cache_base[0] != '\0') {
                char out_list[256];
                snprintf(out_list, sizeof(out_list), "%s", output_name);
                char *saveptr = NULL;
                for (char *tok = strtok_r(out_list, ",", &saveptr); tok != NULL;
                     tok = strtok_r(NULL, ",", &saveptr)) {
                    while (*tok == ' ') {
                        tok++;
                    }
                    size_t tlen = strlen(tok);
                    while (tlen > 0 && tok[tlen - 1] == ' ') {
                        tok[--tlen] = '\0';
                    }
                    if (!is_valid_swww_output_name(tok)) {
                        continue;
                    }
                    char swww_file[WAYWAL_PATH_MAX];
                    char awww_file[WAYWAL_PATH_MAX];
                    if (snprintf(swww_file, sizeof(swww_file), "%s/swww/%s", cache_base, tok) <
                        (int)sizeof(swww_file)) {
                        (void)write_text_file_atomic(swww_file, payload, (size_t)plen);
                    }
                    if (snprintf(awww_file, sizeof(awww_file), "%s/awww/%s", cache_base, tok) <
                        (int)sizeof(awww_file)) {
                        (void)write_text_file_atomic(awww_file, payload, (size_t)plen);
                    }
                }
            }
        }
    }

    if (caelestia_sync && symlink_target[0] != '\0') {
        const char *xdg_state = getenv("XDG_STATE_HOME");
        char cael_file[WAYWAL_PATH_MAX] = {0};
        if (xdg_state && xdg_state[0] != '\0') {
            snprintf(cael_file, sizeof(cael_file), "%s/caelestia/wallpaper/path.txt", xdg_state);
        } else if (home && home[0] != '\0') {
            snprintf(cael_file, sizeof(cael_file), "%s/.local/state/caelestia/wallpaper/path.txt",
                     home);
        }
        if (cael_file[0] != '\0') {
            (void)write_text_file_atomic(cael_file, symlink_target, strlen(symlink_target));
        }
    }

    return png_written;
}
