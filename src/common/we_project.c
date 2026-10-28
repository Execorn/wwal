// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "waywal/toml_parser.h"
#include "waywal/we_json.h"
#include "waywal/we_scene.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

bool we_project_parse(const char *json_str, size_t len, we_project_t *out_proj)
{
    if (!json_str || len == 0 || !out_proj) {
        return false;
    }
    we_jval_t *root = we_json_parse_root(json_str, len);
    if (!root || root->type != JVAL_OBJ) {
        we_jval_free(root);
        return false;
    }

    snprintf(out_proj->title, sizeof(out_proj->title), "%s",
             we_jval_str(we_jobj_get(root, "title"), "Untitled Wallpaper"));
    snprintf(out_proj->type, sizeof(out_proj->type), "%s",
             we_jval_str(we_jobj_get(root, "type"), "scene"));
    snprintf(out_proj->file, sizeof(out_proj->file), "%s",
             we_jval_str(we_jobj_get(root, "file"), "scene.json"));
    snprintf(out_proj->preview, sizeof(out_proj->preview), "%s",
             we_jval_str(we_jobj_get(root, "preview"), "preview.jpg"));

    const we_jval_t *gen = we_jobj_get(root, "general");
    const we_jval_t *props = gen ? we_jobj_get(gen, "properties") : we_jobj_get(root, "properties");
    if (props && props->type == JVAL_OBJ) {
        for (size_t i = 0; i < props->obj.count && out_proj->prop_count < WE_MAX_USER_PROPS; i++) {
            const char *pkey = props->obj.entries[i].key;
            const we_jval_t *pval = props->obj.entries[i].val;
            if (!pkey || !pval || pval->type != JVAL_OBJ) {
                continue;
            }

            we_user_prop_t *up = &out_proj->props[out_proj->prop_count++];
            memset(up, 0, sizeof(*up));
            snprintf(up->key, sizeof(up->key), "%s", pkey);
            snprintf(up->text, sizeof(up->text), "%s",
                     we_jval_str(we_jobj_get(pval, "text"), pkey));

            const char *ptype = we_jval_str(we_jobj_get(pval, "type"), "slider");
            if (strcasecmp(ptype, "bool") == 0) {
                up->type = WE_PROP_BOOL;
            } else if (strcasecmp(ptype, "color") == 0) {
                up->type = WE_PROP_COLOR;
            } else if (strcasecmp(ptype, "combo") == 0 || strcasecmp(ptype, "int") == 0) {
                up->type = WE_PROP_INT;
            } else if (strcasecmp(ptype, "text") == 0 || strcasecmp(ptype, "file") == 0) {
                up->type = WE_PROP_TEXT;
            } else {
                up->type = WE_PROP_FLOAT;
            }

            const we_jval_t *vnode = we_jobj_get(pval, "value");
            if (vnode) {
                if (vnode->type == JVAL_STR && vnode->str) {
                    snprintf(up->str_val, sizeof(up->str_val), "%s", vnode->str);
                } else if (vnode->type == JVAL_NUM) {
                    snprintf(up->str_val, sizeof(up->str_val), "%.6g", vnode->num);
                } else if (vnode->type == JVAL_BOOL) {
                    snprintf(up->str_val, sizeof(up->str_val), "%s",
                             vnode->bval ? "true" : "false");
                }
                up->num_components = we_jval_floats(vnode, NULL, up->val, 4, NULL, 0);
                if (up->num_components == 0 && up->type == WE_PROP_BOOL) {
                    up->num_components = 1;
                }
            }
        }
    }

    we_jval_free(root);
    return true;
}

bool we_project_load_dir(const char *workshop_dir, we_project_t *out_proj)
{
    if (!workshop_dir || !out_proj) {
        return false;
    }
    memset(out_proj, 0, sizeof(*out_proj));
    snprintf(out_proj->workshop_dir, sizeof(out_proj->workshop_dir), "%s", workshop_dir);

    size_t dlen = strlen(workshop_dir);
    while (dlen > 1 && workshop_dir[dlen - 1] == '/') {
        dlen--;
    }
    const char *slash = memrchr(workshop_dir, '/', dlen);
    const char *id_start = slash ? (slash + 1) : workshop_dir;
    size_t id_len = dlen - (size_t)(id_start - workshop_dir);
    if (id_len >= sizeof(out_proj->workshop_id)) {
        id_len = sizeof(out_proj->workshop_id) - 1;
    }
    memcpy(out_proj->workshop_id, id_start, id_len);
    out_proj->workshop_id[id_len] = '\0';

    char json_path[4096];
    if (snprintf(json_path, sizeof(json_path), "%s/project.json", workshop_dir) >=
        (int)sizeof(json_path)) {
        return false;
    }

    int fd = open(json_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(json_path, sizeof(json_path), "%s/scene.pkg", workshop_dir);
        struct stat st;
        if (stat(json_path, &st) != 0 || !S_ISREG(st.st_mode)) {
            snprintf(json_path, sizeof(json_path), "%s/gifscene.pkg", workshop_dir);
        }
        if (stat(json_path, &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(out_proj->title, sizeof(out_proj->title), "%s", out_proj->workshop_id);
            snprintf(out_proj->type, sizeof(out_proj->type), "scene");
            snprintf(out_proj->file, sizeof(out_proj->file), "scene.json");
            snprintf(out_proj->preview, sizeof(out_proj->preview), "preview.jpg");
            return true;
        }
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (16 * 1024 * 1024)) {
        close(fd);
        return false;
    }

    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) {
        close(fd);
        return false;
    }

    ssize_t rd = read(fd, buf, (size_t)st.st_size);
    close(fd);
    if (rd != st.st_size) {
        free(buf);
        return false;
    }
    buf[st.st_size] = '\0';

    bool ok = we_project_parse(buf, (size_t)st.st_size, out_proj);
    free(buf);
    return ok;
}

bool we_project_apply_override(we_project_t *proj, const char *key, const char *val_str)
{
    if (!proj || !key || key[0] == '\0' || !val_str) {
        return false;
    }

    we_user_prop_t *target = NULL;
    for (uint32_t i = 0; i < proj->prop_count; i++) {
        if (strcasecmp(proj->props[i].key, key) == 0) {
            target = &proj->props[i];
            break;
        }
    }
    if (!target) {
        if (proj->prop_count >= WE_MAX_USER_PROPS) {
            return false;
        }
        target = &proj->props[proj->prop_count++];
        memset(target, 0, sizeof(*target));
        snprintf(target->key, sizeof(target->key), "%s", key);
        snprintf(target->text, sizeof(target->text), "%s", key);
        target->type = WE_PROP_FLOAT;
    }

    snprintf(target->str_val, sizeof(target->str_val), "%s", val_str);
    if (strcasecmp(val_str, "true") == 0 || strcmp(val_str, "1") == 0) {
        target->val[0] = 1.0f;
        target->num_components = 1;
        return true;
    }
    if (strcasecmp(val_str, "false") == 0 || strcmp(val_str, "0") == 0) {
        target->val[0] = 0.0f;
        target->num_components = 1;
        return true;
    }

    uint32_t nf = we_parse_float_str(val_str, target->val, 4);
    if (nf > 0) {
        target->num_components = nf;
        if (nf == 3) {
            target->type = WE_PROP_COLOR;
        }
    }
    return true;
}

void we_project_apply_overrides_str(we_project_t *proj, const char *overrides_str)
{
    if (!proj || !overrides_str || overrides_str[0] == '\0') {
        return;
    }
    const char *p = overrides_str;
    while (*p) {
        while (*p && (*p == ';' || *p == '\n' || *p == '\r' || isspace((unsigned char)*p))) {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *end = p;
        while (*end && *end != ';' && *end != '\n' && *end != '\r') {
            end++;
        }
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (eq && eq > p) {
            char kbuf[64] = {0};
            char vbuf[256] = {0};
            size_t klen = (size_t)(eq - p);
            while (klen > 0 && isspace((unsigned char)p[klen - 1])) {
                klen--;
            }
            if (klen >= sizeof(kbuf)) {
                klen = sizeof(kbuf) - 1;
            }
            memcpy(kbuf, p, klen);
            kbuf[klen] = '\0';

            const char *vstart = eq + 1;
            while (vstart < end && isspace((unsigned char)*vstart)) {
                vstart++;
            }
            size_t vlen = (size_t)(end - vstart);
            if (vlen >= sizeof(vbuf)) {
                vlen = sizeof(vbuf) - 1;
            }
            memcpy(vbuf, vstart, vlen);
            vbuf[vlen] = '\0';

            we_project_apply_override(proj, kbuf, vbuf);
        }
        p = end;
    }
}

static void append_colon_paths(const char *path_list, char roots[12][4096], size_t *inout_count,
                               size_t max_roots)
{
    if (!path_list || path_list[0] == '\0' || !inout_count) {
        return;
    }
    const char *p = path_list;
    while (*p && *inout_count < max_roots) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        if (len > 0 && len < 4096) {
            memcpy(roots[*inout_count], p, len);
            roots[*inout_count][len] = '\0';
            (*inout_count)++;
        }
        if (!colon) {
            break;
        }
        p = colon + 1;
    }
}

static size_t collect_workshop_roots(char roots[12][4096])
{
    size_t count = 0;
    const char *env_dir = getenv("WWAL_WORKSHOP_DIR");
    append_colon_paths(env_dir, roots, &count, 6);

    waywal_config_t cfg;
    if (waywal_config_load_default(&cfg) && cfg.daemon.workshop_dir[0] != '\0') {
        append_colon_paths(cfg.daemon.workshop_dir, roots, &count, 8);
    }

    const char *home = getenv("HOME");
    if (home && home[0] != '\0') {
        static const char *const rel_roots[] = {
            ".local/share/Steam/steamapps/workshop/content/431960",
            ".steam/steam/steamapps/workshop/content/431960",
            ".steam/root/steamapps/workshop/content/431960",
            ".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/workshop/content/431960",
            "snap/steam/common/.local/share/Steam/steamapps/workshop/content/431960",
        };
        for (size_t i = 0; i < sizeof(rel_roots) / sizeof(rel_roots[0]) && count < 12; i++) {
            snprintf(roots[count++], 4096, "%s/%s", home, rel_roots[i]);
        }
    }
    return count;
}

static bool dir_has_workshop_files(const char *dir)
{
    char probe[4096];
    struct stat st;
    if (snprintf(probe, sizeof(probe), "%s/project.json", dir) < (int)sizeof(probe) &&
        stat(probe, &st) == 0 && S_ISREG(st.st_mode)) {
        return true;
    }
    if (snprintf(probe, sizeof(probe), "%s/scene.pkg", dir) < (int)sizeof(probe) &&
        stat(probe, &st) == 0 && S_ISREG(st.st_mode)) {
        return true;
    }
    if (snprintf(probe, sizeof(probe), "%s/gifscene.pkg", dir) < (int)sizeof(probe) &&
        stat(probe, &st) == 0 && S_ISREG(st.st_mode)) {
        return true;
    }
    return false;
}

static bool check_path_or_parent_workshop(const char *path, char *out_dir, size_t out_cap)
{
    struct stat st;
    if (!path || stat(path, &st) != 0) {
        return false;
    }

    char cur[4096];
    if (S_ISDIR(st.st_mode)) {
        if (snprintf(cur, sizeof(cur), "%s", path) >= (int)sizeof(cur)) {
            return false;
        }
    } else if (S_ISREG(st.st_mode)) {
        const char *slash = strrchr(path, '/');
        if (!slash || slash <= path) {
            return false;
        }
        size_t dlen = (size_t)(slash - path);
        if (dlen >= sizeof(cur)) {
            return false;
        }
        memcpy(cur, path, dlen);
        cur[dlen] = '\0';
    } else {
        return false;
    }

    size_t clen = strlen(cur);
    while (clen > 1 && cur[clen - 1] == '/') {
        cur[--clen] = '\0';
    }

    if (dir_has_workshop_files(cur)) {
        snprintf(out_dir, out_cap, "%s", cur);
        return true;
    }

    for (int depth = 0; depth < 4; ++depth) {
        char *slash = strrchr(cur, '/');
        if (!slash || slash <= cur) {
            break;
        }
        *slash = '\0';
        if (dir_has_workshop_files(cur)) {
            snprintf(out_dir, out_cap, "%s", cur);
            return true;
        }
    }
    return false;
}

bool we_workshop_resolve_dir(const char *id_or_path, char *out_dir, size_t out_cap)
{
    if (!id_or_path || id_or_path[0] == '\0' || !out_dir || out_cap == 0) {
        return false;
    }

    if (check_path_or_parent_workshop(id_or_path, out_dir, out_cap)) {
        return true;
    }

    char real_buf[4096];
    if (realpath(id_or_path, real_buf) != NULL &&
        check_path_or_parent_workshop(real_buf, out_dir, out_cap)) {
        return true;
    }

    struct stat existing_st;
    if (stat(id_or_path, &existing_st) == 0 || strchr(id_or_path, '/') != NULL) {
        return false;
    }

    char roots[12][4096];
    size_t root_count = collect_workshop_roots(roots);
    for (size_t i = 0; i < root_count; i++) {
        char candidate[4096];
        if (snprintf(candidate, sizeof(candidate), "%s/%s", roots[i], id_or_path) <
                (int)sizeof(candidate) &&
            dir_has_workshop_files(candidate)) {
            snprintf(out_dir, out_cap, "%s", candidate);
            return true;
        }
    }

    return false;
}

static int cmp_projects_by_id(const void *a, const void *b)
{
    const we_project_t *pa = (const we_project_t *)a;
    const we_project_t *pb = (const we_project_t *)b;
    return strcmp(pa->workshop_id, pb->workshop_id);
}

bool we_workshop_scan_all(we_project_t **out_items, size_t *out_count)
{
    if (!out_items || !out_count) {
        return false;
    }
    *out_items = NULL;
    *out_count = 0;

    char roots[12][4096];
    size_t root_count = collect_workshop_roots(roots);

    size_t cap = 0;
    size_t count = 0;
    we_project_t *items = NULL;

    for (size_t r = 0; r < root_count; r++) {
        if (dir_has_workshop_files(roots[r])) {
            we_project_t proj;
            if (we_project_load_dir(roots[r], &proj)) {
                bool dup = false;
                for (size_t k = 0; k < count; k++) {
                    if (strcmp(items[k].workshop_id, proj.workshop_id) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) {
                    if (count + 1 > cap) {
                        size_t ncap = cap == 0 ? 16 : cap * 2;
                        we_project_t *ni = realloc(items, ncap * sizeof(we_project_t));
                        if (!ni) {
                            free(items);
                            return false;
                        }
                        items = ni;
                        cap = ncap;
                    }
                    items[count++] = proj;
                }
            }
        }

        DIR *d = opendir(roots[r]);
        if (!d) {
            continue;
        }
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') {
                continue;
            }
            char subdir[4096];
            if (snprintf(subdir, sizeof(subdir), "%s/%s", roots[r], de->d_name) >=
                (int)sizeof(subdir)) {
                continue;
            }
            if (!dir_has_workshop_files(subdir)) {
                continue;
            }
            we_project_t proj;
            if (!we_project_load_dir(subdir, &proj)) {
                continue;
            }
            bool dup = false;
            for (size_t k = 0; k < count; k++) {
                if (strcmp(items[k].workshop_id, proj.workshop_id) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            if (count + 1 > cap) {
                size_t ncap = cap == 0 ? 16 : cap * 2;
                we_project_t *ni = realloc(items, ncap * sizeof(we_project_t));
                if (!ni) {
                    closedir(d);
                    free(items);
                    return false;
                }
                items = ni;
                cap = ncap;
            }
            items[count++] = proj;
        }
        closedir(d);
    }

    if (count > 1) {
        qsort(items, count, sizeof(we_project_t), cmp_projects_by_id);
    }
    *out_items = items;
    *out_count = count;
    return true;
}

static bool has_supported_preview_image_ext(const char *filename)
{
    if (!filename) {
        return false;
    }
    const char *dot = strrchr(filename, '.');
    if (!dot || dot[1] == '\0') {
        return false;
    }
    static const char *const exts[] = {
        ".jpg", ".jpeg", ".png", ".gif",  ".webp", ".bmp", ".avif", ".jxl",
        ".svg", ".qoi",  ".heic", ".heif", ".ppm",  ".pgm", ".pnm",
    };
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); ++i) {
        if (strcasecmp(dot, exts[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool candidate_stays_inside_ws_dir(const char *ws_dir, const char *candidate)
{
    char real_ws[4096];
    char real_cand[4096];
    if (!realpath(ws_dir, real_ws) || !realpath(candidate, real_cand)) {
        return false;
    }
    size_t wlen = strlen(real_ws);
    if (strncmp(real_cand, real_ws, wlen) != 0) {
        return false;
    }
    return real_cand[wlen] == '/' || real_cand[wlen] == '\0';
}

bool we_workshop_find_preview_file(const char *workshop_dir, char *out_path, size_t out_cap)
{
    if (!workshop_dir || workshop_dir[0] == '\0' || !out_path || out_cap == 0) {
        return false;
    }
    out_path[0] = '\0';

    char ws_dir[4096];
    if (!we_workshop_resolve_dir(workshop_dir, ws_dir, sizeof(ws_dir))) {
        return false;
    }

    struct stat st;
    we_project_t proj;
    if (we_project_load_dir(ws_dir, &proj) && proj.preview[0] != '\0' &&
        strchr(proj.preview, '/') == NULL && strchr(proj.preview, '\\') == NULL &&
        strcmp(proj.preview, ".") != 0 && strcmp(proj.preview, "..") != 0 &&
        strcasecmp(proj.preview, "project.json") != 0 &&
        strcasecmp(proj.preview, "scene.json") != 0 &&
        strcasecmp(proj.preview, "scene.pkg") != 0 &&
        strcasecmp(proj.preview, "gifscene.pkg") != 0 &&
        has_supported_preview_image_ext(proj.preview)) {
        char candidate[4096];
        if (snprintf(candidate, sizeof(candidate), "%s/%s", ws_dir, proj.preview) <
                (int)sizeof(candidate) &&
            stat(candidate, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
            candidate_stays_inside_ws_dir(ws_dir, candidate)) {
            snprintf(out_path, out_cap, "%s", candidate);
            return true;
        }
    }

    static const char *const fallbacks[] = {
        "preview.jpg", "preview.png", "preview.gif", "preview.webp", "preview.jpeg",
    };
    for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); ++i) {
        char candidate[4096];
        if (snprintf(candidate, sizeof(candidate), "%s/%s", ws_dir, fallbacks[i]) <
                (int)sizeof(candidate) &&
            stat(candidate, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
            candidate_stays_inside_ws_dir(ws_dir, candidate)) {
            snprintf(out_path, out_cap, "%s", candidate);
            return true;
        }
    }

    return false;
}

void we_workshop_sanitize_title(const char *title, char *out_buf, size_t out_cap)
{
    if (!out_buf || out_cap == 0) {
        return;
    }
    out_buf[0] = '\0';
    if (out_cap == 1) {
        return;
    }

    size_t max_bytes = out_cap - 1;
    if (max_bytes > 96) {
        max_bytes = 96;
    }

    size_t pos = 0;
    bool prev_space = true;
    const unsigned char *p = (const unsigned char *)(title ? title : "");

    while (*p != '\0' && pos < max_bytes) {
        unsigned char c = *p;
        if (c < 0x80) {
            bool unsafe = (c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':' || c == '*' ||
                           c == '?' || c == '"' || c == '<' || c == '>' || c == '|');
            if (unsafe || isspace(c)) {
                if (!prev_space && pos + 1 <= max_bytes) {
                    out_buf[pos++] = ' ';
                    prev_space = true;
                }
            } else if (pos == 0 && c == '.') {
                /* Skip leading dot so synced preview symlinks are not hidden dotfiles */
            } else {
                out_buf[pos++] = (char)c;
                prev_space = false;
            }
            p++;
        } else {
            size_t seq_len = 0;
            bool valid = false;
            if (c >= 0xC2 && c <= 0xDF) {
                seq_len = 2;
                valid = ((p[1] & 0xC0) == 0x80);
            } else if (c == 0xE0) {
                seq_len = 3;
                valid = (p[1] >= 0xA0 && p[1] <= 0xBF && (p[2] & 0xC0) == 0x80);
            } else if ((c >= 0xE1 && c <= 0xEC) || (c >= 0xEE && c <= 0xEF)) {
                seq_len = 3;
                valid = ((p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80);
            } else if (c == 0xED) {
                seq_len = 3;
                valid = (p[1] >= 0x80 && p[1] <= 0x9F && (p[2] & 0xC0) == 0x80);
            } else if (c == 0xF0) {
                seq_len = 4;
                valid = (p[1] >= 0x90 && p[1] <= 0xBF && (p[2] & 0xC0) == 0x80 &&
                         (p[3] & 0xC0) == 0x80);
            } else if (c >= 0xF1 && c <= 0xF3) {
                seq_len = 4;
                valid = ((p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80);
            } else if (c == 0xF4) {
                seq_len = 4;
                valid = (p[1] >= 0x80 && p[1] <= 0x8F && (p[2] & 0xC0) == 0x80 &&
                         (p[3] & 0xC0) == 0x80);
            }

            if (!valid) {
                p++;
                continue;
            }
            /* Filter Unicode C1 control characters (U+0080..U+009F -> 0xC2 0x80..0x9F) */
            if (seq_len == 2 && c == 0xC2 && p[1] >= 0x80 && p[1] <= 0x9F) {
                if (!prev_space && pos + 1 <= max_bytes) {
                    out_buf[pos++] = ' ';
                    prev_space = true;
                }
                p += 2;
                continue;
            }
            if (pos + seq_len > max_bytes) {
                break;
            }
            for (size_t k = 0; k < seq_len; ++k) {
                out_buf[pos++] = (char)p[k];
            }
            prev_space = false;
            p += seq_len;
        }
    }

    while (pos > 0 && (out_buf[pos - 1] == ' ' || out_buf[pos - 1] == '.')) {
        pos--;
    }
    out_buf[pos] = '\0';

    if (pos == 0) {
        snprintf(out_buf, out_cap, "Untitled");
    }
}

static bool mkdir_p_preview_dir(const char *dir_path)
{
    if (!dir_path || dir_path[0] == '\0') {
        return false;
    }
    char tmp[4096];
    size_t len = strlen(dir_path);
    if (len == 0 || len >= sizeof(tmp)) {
        return false;
    }
    memcpy(tmp, dir_path, len + 1);

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

bool we_workshop_sync_previews(const char *target_dir, size_t *out_synced, size_t *out_pruned)
{
    if (out_synced) {
        *out_synced = 0;
    }
    if (out_pruned) {
        *out_pruned = 0;
    }

    char resolved_dir[4096] = {0};
    char cache_root_dir[4096] = {0};
    if (target_dir && target_dir[0] != '\0') {
        snprintf(resolved_dir, sizeof(resolved_dir), "%s", target_dir);
    } else {
        const char *wwal_cache = getenv("WWAL_CACHE_DIR");
        const char *xdg_cache = getenv("XDG_CACHE_HOME");
        const char *home = getenv("HOME");
        if (wwal_cache && wwal_cache[0] != '\0') {
            snprintf(cache_root_dir, sizeof(cache_root_dir), "%s", wwal_cache);
        } else if (xdg_cache && xdg_cache[0] != '\0') {
            snprintf(cache_root_dir, sizeof(cache_root_dir), "%s/wwal", xdg_cache);
        } else if (home && home[0] != '\0') {
            snprintf(cache_root_dir, sizeof(cache_root_dir), "%s/.cache/wwal", home);
        } else {
            return false;
        }
        snprintf(resolved_dir, sizeof(resolved_dir), "%s/workshop-previews", cache_root_dir);
    }

    if (!mkdir_p_preview_dir(resolved_dir)) {
        return false;
    }

    if (cache_root_dir[0] != '\0') {
        char alias_link[4096];
        char alias_tmp[4096];
        if (snprintf(alias_link, sizeof(alias_link), "%s/workshop", cache_root_dir) <
                (int)sizeof(alias_link) &&
            snprintf(alias_tmp, sizeof(alias_tmp), "%s/.workshop.tmp.%d", cache_root_dir,
                     (int)getpid()) < (int)sizeof(alias_tmp)) {
            struct stat ast;
            if (lstat(alias_link, &ast) != 0 || S_ISLNK(ast.st_mode)) {
                unlink(alias_tmp);
                if (symlink("workshop-previews", alias_tmp) == 0) {
                    if (rename(alias_tmp, alias_link) != 0) {
                        unlink(alias_tmp);
                    }
                }
            }
        }
    }

    we_project_t *items = NULL;
    size_t count = 0;
    if (!we_workshop_scan_all(&items, &count)) {
        return false;
    }

    typedef struct {
        char link_name[256];
        char target_path[4096];
        bool valid;
    } preview_entry_t;

    preview_entry_t *entries =
        count > 0 ? (preview_entry_t *)calloc(count, sizeof(preview_entry_t)) : NULL;
    if (count > 0 && !entries) {
        free(items);
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        char preview_file[4096];
        if (!we_workshop_find_preview_file(items[i].workshop_dir, preview_file,
                                           sizeof(preview_file))) {
            continue;
        }
        char abs_preview[4096];
        if (realpath(preview_file, abs_preview) == NULL) {
            snprintf(abs_preview, sizeof(abs_preview), "%s", preview_file);
        }

        const char *slash = strrchr(abs_preview, '/');
        const char *base = slash ? slash + 1 : abs_preview;
        const char *dot = strrchr(base, '.');
        const char *ext = (dot && dot[1] != '\0') ? dot : ".jpg";

        char clean_title[128];
        we_workshop_sanitize_title(items[i].title, clean_title, sizeof(clean_title));
        snprintf(entries[i].link_name, sizeof(entries[i].link_name), "%s [%s]%s", clean_title,
                 items[i].workshop_id[0] ? items[i].workshop_id : "0", ext);
        snprintf(entries[i].target_path, sizeof(entries[i].target_path), "%s", abs_preview);
        entries[i].valid = true;
    }

    size_t pruned = 0;
    DIR *d = opendir(resolved_dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') {
                continue;
            }
            char full_path[4096];
            if (snprintf(full_path, sizeof(full_path), "%s/%s", resolved_dir, de->d_name) >=
                (int)sizeof(full_path)) {
                continue;
            }
            struct stat lst;
            if (lstat(full_path, &lst) != 0 || !S_ISLNK(lst.st_mode)) {
                continue;
            }
            bool keep = false;
            struct stat tst;
            if (stat(full_path, &tst) == 0) {
                for (size_t i = 0; i < count && entries; ++i) {
                    if (entries[i].valid && strcmp(entries[i].link_name, de->d_name) == 0) {
                        keep = true;
                        break;
                    }
                }
            }
            if (!keep) {
                if (unlink(full_path) == 0) {
                    pruned++;
                }
            }
        }
        closedir(d);
    }

    size_t synced = 0;
    for (size_t i = 0; i < count && entries; ++i) {
        if (!entries[i].valid) {
            continue;
        }
        char link_path[4096];
        char tmp_link[4096];
        if (snprintf(link_path, sizeof(link_path), "%s/%s", resolved_dir, entries[i].link_name) >=
                (int)sizeof(link_path) ||
            snprintf(tmp_link, sizeof(tmp_link), "%s/.tmp.%d.%zu", resolved_dir, (int)getpid(),
                     i) >= (int)sizeof(tmp_link)) {
            continue;
        }
        unlink(tmp_link);
        if (symlink(entries[i].target_path, tmp_link) == 0) {
            if (rename(tmp_link, link_path) == 0) {
                synced++;
            } else {
                unlink(tmp_link);
            }
        }
    }

    free(entries);
    free(items);

    if (out_synced) {
        *out_synced = synced;
    }
    if (out_pruned) {
        *out_pruned = pruned;
    }
    return true;
}
