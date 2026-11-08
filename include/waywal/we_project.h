// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WE_PROJECT_H
#define WAYWAL_WE_PROJECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WE_MAX_USER_PROPS 64u

typedef enum {
    WE_PROP_FLOAT = 0,
    WE_PROP_BOOL = 1,
    WE_PROP_INT = 2,
    WE_PROP_COLOR = 3,
    WE_PROP_TEXT = 4,
} we_prop_type_t;

typedef struct {
    char key[64];
    char text[128];
    we_prop_type_t type;
    float val[4];
    uint32_t num_components;
    char str_val[256];
} we_user_prop_t;

typedef struct {
    char workshop_id[64];
    char workshop_dir[4096];
    char title[256];
    char type[64];  /* "scene", "video", "web", "application" */
    char file[256]; /* Default "scene.json" or video filename */
    char preview[256];
    we_user_prop_t props[WE_MAX_USER_PROPS];
    uint32_t prop_count;
} we_project_t;

/* Parses project.json from memory or directory */
bool we_project_parse(const char *json_str, size_t len, we_project_t *out_proj);
bool we_project_load_dir(const char *workshop_dir, we_project_t *out_proj);

/* Applies a single --prop key=val override or a semicolon/newline-delimited override list */
bool we_project_apply_override(we_project_t *proj, const char *key, const char *val_str);
void we_project_apply_overrides_str(we_project_t *proj, const char *overrides_str);

/* Finds user property by key in parsed project */
const we_user_prop_t *we_project_find_prop(const we_project_t *proj, const char *key);

/* Resolves a Workshop ID (e.g. "123456789") or filesystem path to a directory containing
 * project.json */
bool we_workshop_resolve_dir(const char *id_or_path, char *out_dir, size_t out_cap);

/* Scans Steam Workshop 431960 directories and returns an array of parsed we_project_t items */
bool we_workshop_scan_all(we_project_t **out_items, size_t *out_count);

/* Locates the preview image file (e.g. preview.jpg/png/gif/webp) for a Workshop directory */
bool we_workshop_find_preview_file(const char *workshop_dir, char *out_path, size_t out_cap);

/* Sanitizes a Workshop item title for safe UTF-8 filenames without splitting multi-byte codepoints
 */
void we_workshop_sanitize_title(const char *title, char *out_buf, size_t out_cap);

/* Populates a directory with human-readable preview symlinks for external pickers and prunes stale
 * symlinks */
bool we_workshop_sync_previews(const char *target_dir, size_t *out_synced, size_t *out_pruned);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WE_PROJECT_H */
