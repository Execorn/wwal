// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_PATH_H
#define WAYWAL_PATH_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAYWAL_PATH_MAX 4096

typedef struct {
    char data[WAYWAL_PATH_MAX];
    size_t len; /* Length excluding final null terminator */
} path_buf_t;

void path_buf_init(path_buf_t *pb);
void path_buf_from_str(path_buf_t *pb, const char *str);

/* Appends component with '/' separator */
bool path_buf_push(path_buf_t *pb, const char *component);

/* Appends characters to the current component without adding '/' */
bool path_buf_append(path_buf_t *pb, const char *suffix);

/* Extracts directory parent */
bool path_buf_parent(path_buf_t *pb);

const char *path_buf_cstr(const path_buf_t *pb);

/* Normalizes an absolute path in-place (collapses '//', strips '/./', resolves '/../') */
void waywal_normalize_path(char *path);

/* Expands leading '~/' and environment variables ($HOME, $XDG_CONFIG_HOME, ${VAR}, etc.) */
bool waywal_expand_path(const char *raw_path, char *out_buf, size_t out_cap);

typedef struct {
    char name[64];
    char path[WAYWAL_PATH_MAX];
    char filename[72];
    char title[64];
    char description[160];
    char author[64];
    bool audio_reactive;
    bool is_builtin;
} waywal_shader_preset_info_t;

bool waywal_shader_resolve_path(const char *name_or_path, char *out_buf, size_t out_cap);
bool waywal_shader_is_audio_reactive(const char *name_or_path);
size_t waywal_shader_preset_list(waywal_shader_preset_info_t *out_list, size_t max_entries);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_PATH_H */
