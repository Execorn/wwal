// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/state_store.h"

#include "waywal/log.h"

#include <errno.h>
#include <fcntl.h>
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

void waywal_state_store_init(waywal_state_store_t *store)
{
    if (!store) {
        return;
    }
    memset(store, 0, sizeof(*store));
    store->magic = WAYWAL_STATE_MAGIC;
    store->version = WAYWAL_STATE_VERSION;
}

bool waywal_state_store_resolve_path(path_buf_t *out_path, const char *override_dir)
{
    if (!out_path) {
        return false;
    }
    path_buf_init(out_path);

    const char *env_override = override_dir;
    if (!env_override || env_override[0] == '\0') {
        env_override = getenv("WWAL_STATE_DIR");
    }

    if (env_override && env_override[0] != '\0') {
        path_buf_from_str(out_path, env_override);
        return path_buf_push(out_path, "state.bin");
    }

    const char *xdg_state = getenv("XDG_STATE_HOME");
    if (xdg_state && xdg_state[0] != '\0') {
        path_buf_from_str(out_path, xdg_state);
        if (!path_buf_push(out_path, "wwal")) {
            return false;
        }
        return path_buf_push(out_path, "state.bin");
    }

    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') {
        return false;
    }

    path_buf_from_str(out_path, home);
    if (!path_buf_push(out_path, ".local/state/wwal")) {
        return false;
    }
    return path_buf_push(out_path, "state.bin");
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
    }

    *store = loaded;
    return true;
}

bool(waywal_state_store_save)(const waywal_state_store_t *store)
{
    path_buf_t path;
    if (!waywal_state_store_resolve_path(&path, NULL)) {
        return false;
    }
    return waywal_state_store_save_at(store, path_buf_cstr(&path));
}

bool(waywal_state_store_load)(waywal_state_store_t *store)
{
    path_buf_t path;
    if (!waywal_state_store_resolve_path(&path, NULL)) {
        return false;
    }
    return waywal_state_store_load_at(store, path_buf_cstr(&path));
}

bool waywal_state_store_save_dir(const waywal_state_store_t *store, const char *override_dir)
{
    if (override_dir && strlen(override_dir) >= 4 &&
        strcmp(override_dir + strlen(override_dir) - 4, ".bin") == 0) {
        return waywal_state_store_save_at(store, override_dir);
    }
    path_buf_t path;
    if (!waywal_state_store_resolve_path(&path, override_dir)) {
        return false;
    }
    return waywal_state_store_save_at(store, path_buf_cstr(&path));
}

bool waywal_state_store_load_dir(waywal_state_store_t *store, const char *override_dir)
{
    if (override_dir && strlen(override_dir) >= 4 &&
        strcmp(override_dir + strlen(override_dir) - 4, ".bin") == 0) {
        return waywal_state_store_load_at(store, override_dir);
    }
    path_buf_t path;
    if (!waywal_state_store_resolve_path(&path, override_dir)) {
        return false;
    }
    return waywal_state_store_load_at(store, path_buf_cstr(&path));
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
            return true;
        }
    }

    if (store->count >= WAYWAL_STATE_MAX_OUTPUTS) {
        return false;
    }

    store->outputs[store->count] = *state;
    store->outputs[store->count].output_name[sizeof(store->outputs[0].output_name) - 1] = '\0';
    store->outputs[store->count].media_path[sizeof(store->outputs[0].media_path) - 1] = '\0';
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
