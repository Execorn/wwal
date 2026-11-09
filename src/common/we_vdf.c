// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/we_vdf.h"
#include "waywal/arena.h"
#include "waywal/toml_parser.h"

#include <ctype.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    arena_t arena;
} vdf_arena_ctx_t;

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    arena_t *arena;
    int depth;
} vdf_parser_t;

static void skip_whitespace_and_comments(vdf_parser_t *p)
{
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') {
            p->pos++;
            continue;
        }
        if (c == '/' && p->pos + 1 < p->len && p->src[p->pos + 1] == '/') {
            p->pos += 2;
            while (p->pos < p->len && p->src[p->pos] != '\n' && p->src[p->pos] != '\r') {
                p->pos++;
            }
            continue;
        }
        break;
    }
}

static void skip_quoted_string(vdf_parser_t *p)
{
    if (p->pos >= p->len || p->src[p->pos] != '"') {
        return;
    }
    p->pos++; /* skip initial quote */
    while (p->pos < p->len) {
        char c = p->src[p->pos++];
        if (c == '"') {
            break;
        }
        if (c == '\\' && p->pos < p->len) {
            p->pos++; /* skip escaped character */
        }
    }
}

static char *parse_quoted_string(vdf_parser_t *p)
{
    if (p->pos >= p->len || p->src[p->pos] != '"') {
        return NULL;
    }
    p->pos++; /* skip initial quote */

    /* Pre-scan to find matching unescaped closing quote */
    size_t scan_pos = p->pos;
    while (scan_pos < p->len) {
        char c = p->src[scan_pos++];
        if (c == '"') {
            break;
        }
        if (c == '\\' && scan_pos < p->len) {
            scan_pos++; /* skip escaped character */
        }
    }

    size_t token_len = scan_pos - p->pos;
    char *dest = arena_alloc(p->arena, token_len + 4, 1);
    if (!dest) {
        return NULL;
    }

    size_t out_len = 0;
    while (p->pos < p->len) {
        char c = p->src[p->pos++];
        if (c == '"') {
            break;
        }
        if (c == '\\') {
            if (p->pos >= p->len) {
                dest[out_len++] = '\\';
                break;
            }
            char next = p->src[p->pos++];
            switch (next) {
            case '"':
                dest[out_len++] = '"';
                break;
            case '\\':
                dest[out_len++] = '\\';
                break;
            case '/':
                dest[out_len++] = '/';
                break;
            case 'n':
                dest[out_len++] = '\n';
                break;
            case 'r':
                dest[out_len++] = '\r';
                break;
            case 't':
                dest[out_len++] = '\t';
                break;
            case 'b':
                dest[out_len++] = '\b';
                break;
            case 'f':
                dest[out_len++] = '\f';
                break;
            case 'u': {
                if (p->pos + 4 <= p->len) {
                    char hex_buf[5] = {p->src[p->pos], p->src[p->pos + 1], p->src[p->pos + 2],
                                       p->src[p->pos + 3], '\0'};
                    char *endptr = NULL;
                    long val = strtol(hex_buf, &endptr, 16);
                    if (endptr == hex_buf + 4 && val >= 0) {
                        p->pos += 4;
                        if (val <= 0x7F) {
                            dest[out_len++] = (char)val;
                        } else if (val <= 0x7FF) {
                            dest[out_len++] = (char)(0xC0 | ((val >> 6) & 0x1F));
                            dest[out_len++] = (char)(0x80 | (val & 0x3F));
                        } else {
                            dest[out_len++] = (char)(0xE0 | ((val >> 12) & 0x0F));
                            dest[out_len++] = (char)(0x80 | ((val >> 6) & 0x3F));
                            dest[out_len++] = (char)(0x80 | (val & 0x3F));
                        }
                        break;
                    }
                }
                dest[out_len++] = 'u';
                break;
            }
            default:
                dest[out_len++] = next;
                break;
            }
        } else {
            dest[out_len++] = c;
        }
    }
    dest[out_len] = '\0';
    return dest;
}

static char *parse_bare_string(vdf_parser_t *p)
{
    size_t start = p->pos;
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f' || c == '{' ||
            c == '}' || c == '"') {
            break;
        }
        if (c == '/' && p->pos + 1 < p->len && p->src[p->pos + 1] == '/') {
            break;
        }
        p->pos++;
    }

    size_t count = p->pos - start;
    if (count == 0) {
        return NULL;
    }

    char *dest = arena_alloc(p->arena, count + 1, 1);
    if (!dest) {
        return NULL;
    }
    memcpy(dest, &p->src[start], count);
    dest[count] = '\0';
    return dest;
}

static char *parse_token_string(vdf_parser_t *p)
{
    skip_whitespace_and_comments(p);
    if (p->pos >= p->len) {
        return NULL;
    }
    if (p->src[p->pos] == '"') {
        return parse_quoted_string(p);
    }
    if (p->src[p->pos] == '{' || p->src[p->pos] == '}') {
        return NULL;
    }
    return parse_bare_string(p);
}

static bool vdf_object_add_child(arena_t *arena, waywal_vdf_object_t *obj, waywal_vdf_node_t *child)
{
    if (obj->child_count >= obj->child_capacity) {
        size_t new_cap = obj->child_capacity == 0 ? 8 : obj->child_capacity * 2;
        waywal_vdf_node_t **new_children =
            arena_alloc(arena, new_cap * sizeof(waywal_vdf_node_t *), _Alignof(void *));
        if (!new_children) {
            return false;
        }
        if (obj->child_count > 0 && obj->children) {
            memcpy(new_children, obj->children, obj->child_count * sizeof(waywal_vdf_node_t *));
        }
        obj->children = new_children;
        obj->child_capacity = new_cap;
    }
    obj->children[obj->child_count++] = child;
    return true;
}

static bool parse_object_children(vdf_parser_t *p, waywal_vdf_node_t *parent_node)
{
    while (p->pos < p->len) {
        skip_whitespace_and_comments(p);
        if (p->pos >= p->len) {
            break;
        }

        if (p->src[p->pos] == '}') {
            break;
        }

        char *key = parse_token_string(p);
        if (!key) {
            if (p->pos < p->len && p->src[p->pos] == '}') {
                break;
            }
            p->pos++;
            continue;
        }

        skip_whitespace_and_comments(p);
        if (p->pos >= p->len) {
            waywal_vdf_node_t *node =
                arena_alloc_zero(p->arena, sizeof(waywal_vdf_node_t), _Alignof(waywal_vdf_node_t));
            if (node) {
                node->key = key;
                node->type = VDF_NODE_STRING;
                node->string_value = "";
                vdf_object_add_child(p->arena, &parent_node->object, node);
            }
            break;
        }

        if (p->src[p->pos] == '{') {
            p->pos++; /* consume '{' */
            waywal_vdf_node_t *obj_node =
                arena_alloc_zero(p->arena, sizeof(waywal_vdf_node_t), _Alignof(waywal_vdf_node_t));
            if (!obj_node) {
                return false;
            }
            obj_node->key = key;
            obj_node->type = VDF_NODE_OBJECT;

            if (p->depth < 64) {
                p->depth++;
                parse_object_children(p, obj_node);
                p->depth--;
                skip_whitespace_and_comments(p);
                if (p->pos < p->len && p->src[p->pos] == '}') {
                    p->pos++; /* consume '}' */
                }
            } else {
                int brace_count = 1;
                while (p->pos < p->len && brace_count > 0) {
                    skip_whitespace_and_comments(p);
                    if (p->pos >= p->len) {
                        break;
                    }
                    if (p->src[p->pos] == '{') {
                        brace_count++;
                        p->pos++;
                    } else if (p->src[p->pos] == '}') {
                        brace_count--;
                        p->pos++;
                    } else if (p->src[p->pos] == '"') {
                        skip_quoted_string(p);
                    } else {
                        p->pos++;
                    }
                }
            }

            vdf_object_add_child(p->arena, &parent_node->object, obj_node);
        } else if (p->src[p->pos] == '}') {
            waywal_vdf_node_t *node =
                arena_alloc_zero(p->arena, sizeof(waywal_vdf_node_t), _Alignof(waywal_vdf_node_t));
            if (node) {
                node->key = key;
                node->type = VDF_NODE_STRING;
                node->string_value = "";
                vdf_object_add_child(p->arena, &parent_node->object, node);
            }
            break;
        } else {
            char *value = parse_token_string(p);
            if (!value) {
                value = "";
            }
            waywal_vdf_node_t *str_node =
                arena_alloc_zero(p->arena, sizeof(waywal_vdf_node_t), _Alignof(waywal_vdf_node_t));
            if (!str_node) {
                return false;
            }
            str_node->key = key;
            str_node->type = VDF_NODE_STRING;
            str_node->string_value = value;
            vdf_object_add_child(p->arena, &parent_node->object, str_node);
        }
    }
    return true;
}

waywal_vdf_doc_t *waywal_vdf_parse(const char *text, size_t length)
{
    if (!text) {
        return NULL;
    }

    waywal_vdf_doc_t *doc = calloc(1, sizeof(waywal_vdf_doc_t));
    if (!doc) {
        return NULL;
    }

    vdf_arena_ctx_t *ctx = calloc(1, sizeof(vdf_arena_ctx_t));
    if (!ctx) {
        free(doc);
        return NULL;
    }

    size_t reserve = 2 * 1024 * 1024;
    if (length * 6 + 512 * 1024 > reserve) {
        reserve = length * 6 + 512 * 1024;
    }

    if (!arena_init(&ctx->arena, reserve)) {
        free(ctx);
        free(doc);
        return NULL;
    }

    doc->arena_ctx = ctx;

    waywal_vdf_node_t *container =
        arena_alloc_zero(&ctx->arena, sizeof(waywal_vdf_node_t), _Alignof(waywal_vdf_node_t));
    if (!container) {
        waywal_vdf_free(doc);
        return NULL;
    }
    container->key = "";
    container->type = VDF_NODE_OBJECT;

    vdf_parser_t parser = {
        .src = text,
        .len = length,
        .pos = 0,
        .arena = &ctx->arena,
        .depth = 0,
    };

    parse_object_children(&parser, container);

    if (container->object.child_count == 0) {
        doc->root = NULL;
    } else if (container->object.child_count == 1) {
        doc->root = container->object.children[0];
    } else {
        doc->root = container;
    }

    return doc;
}

void waywal_vdf_free(waywal_vdf_doc_t *doc)
{
    if (!doc) {
        return;
    }
    if (doc->arena_ctx) {
        vdf_arena_ctx_t *ctx = (vdf_arena_ctx_t *)doc->arena_ctx;
        arena_destroy(&ctx->arena);
        free(ctx);
    }
    free(doc);
}

const waywal_vdf_node_t *waywal_vdf_find_child(const waywal_vdf_node_t *parent, const char *key)
{
    if (!parent || !key) {
        return NULL;
    }

    if (parent->type == VDF_NODE_OBJECT && parent->object.children) {
        for (size_t i = 0; i < parent->object.child_count; i++) {
            const waywal_vdf_node_t *child = parent->object.children[i];
            if (child && child->key && strcasecmp(child->key, key) == 0) {
                return child;
            }
        }
    }

    if (parent->key && strcasecmp(parent->key, key) == 0) {
        return parent;
    }

    return NULL;
}

const char *waywal_vdf_get_string(const waywal_vdf_node_t *parent, const char *key,
                                  const char *fallback)
{
    if (!parent || !key) {
        return fallback;
    }

    const waywal_vdf_node_t *child = waywal_vdf_find_child(parent, key);
    if (child) {
        if (child->type == VDF_NODE_STRING && child->string_value) {
            return child->string_value;
        }
    }
    return fallback;
}

static char *read_entire_file_local(const char *path, size_t *out_len)
{
    if (!path || !out_len) {
        return NULL;
    }
    *out_len = 0;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }

    size_t read_bytes = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[read_bytes] = '\0';
    *out_len = read_bytes;
    return buf;
}

static bool check_workshop_content_on_disk(const char *base_path)
{
    char probe[4096];
    struct stat st;

    if (snprintf(probe, sizeof(probe), "%s/steamapps/workshop/content/431960", base_path) <
            (int)sizeof(probe) &&
        stat(probe, &st) == 0 && S_ISDIR(st.st_mode)) {
        return true;
    }
    if (snprintf(probe, sizeof(probe), "%s/workshop/content/431960", base_path) <
            (int)sizeof(probe) &&
        stat(probe, &st) == 0 && S_ISDIR(st.st_mode)) {
        return true;
    }
    if (snprintf(probe, sizeof(probe), "%s/431960", base_path) < (int)sizeof(probe) &&
        stat(probe, &st) == 0 && S_ISDIR(st.st_mode)) {
        return true;
    }
    if (snprintf(probe, sizeof(probe), "%s/steamapps/common/wallpaper_engine", base_path) <
            (int)sizeof(probe) &&
        stat(probe, &st) == 0 && S_ISDIR(st.st_mode)) {
        return true;
    }
    return false;
}

static void add_or_merge_library(waywal_steam_library_t *out_libs, size_t *inout_count,
                                 size_t max_libs, const char *raw_path, const char *label,
                                 uint64_t total_size, uint64_t app_size, bool has_we)
{
    if (!out_libs || !inout_count || *inout_count >= max_libs || !raw_path || raw_path[0] == '\0') {
        return;
    }

    char canon[4096];
    if (realpath(raw_path, canon) == NULL) {
        strncpy(canon, raw_path, sizeof(canon) - 1);
        canon[sizeof(canon) - 1] = '\0';
    }

    size_t clen = strlen(canon);
    while (clen > 1 && canon[clen - 1] == '/') {
        canon[--clen] = '\0';
    }

    if (!has_we) {
        has_we = check_workshop_content_on_disk(canon);
    }

    for (size_t i = 0; i < *inout_count; i++) {
        if (strcmp(out_libs[i].path, canon) == 0) {
            if (has_we) {
                out_libs[i].has_we_app = true;
            }
            if (app_size > out_libs[i].app_size_bytes) {
                out_libs[i].app_size_bytes = app_size;
            }
            if (total_size > out_libs[i].total_size_bytes) {
                out_libs[i].total_size_bytes = total_size;
            }
            if (label && label[0] != '\0' && out_libs[i].label[0] == '\0') {
                strncpy(out_libs[i].label, label, sizeof(out_libs[i].label) - 1);
            }
            return;
        }
    }

    waywal_steam_library_t *lib = &out_libs[*inout_count];
    memset(lib, 0, sizeof(*lib));
    strncpy(lib->path, canon, sizeof(lib->path) - 1);
    if (label && label[0] != '\0') {
        strncpy(lib->label, label, sizeof(lib->label) - 1);
    }
    lib->total_size_bytes = total_size;
    lib->app_size_bytes = app_size;
    lib->has_we_app = has_we;
    (*inout_count)++;
}

static void parse_libraryfolders_vdf(const char *vdf_path, waywal_steam_library_t *out_libs,
                                     size_t *inout_count, size_t max_libs)
{
    size_t file_len = 0;
    char *buf = read_entire_file_local(vdf_path, &file_len);
    if (!buf) {
        return;
    }

    waywal_vdf_doc_t *doc = waywal_vdf_parse(buf, file_len);
    free(buf);
    if (!doc || !doc->root) {
        if (doc) {
            waywal_vdf_free(doc);
        }
        return;
    }

    const waywal_vdf_node_t *lf = waywal_vdf_find_child(doc->root, "libraryfolders");
    if (!lf) {
        lf = doc->root;
    }

    if (lf && lf->type == VDF_NODE_OBJECT && lf->object.children) {
        for (size_t i = 0; i < lf->object.child_count && *inout_count < max_libs; i++) {
            const waywal_vdf_node_t *item = lf->object.children[i];
            if (!item) {
                continue;
            }

            if (item->type == VDF_NODE_OBJECT) {
                const char *path = waywal_vdf_get_string(item, "path", NULL);
                if (!path || path[0] == '\0') {
                    path = item->key;
                }
                const char *label = waywal_vdf_get_string(item, "label", "");
                const char *totalsize_str = waywal_vdf_get_string(item, "totalsize", "0");
                uint64_t total_size = strtoull(totalsize_str, NULL, 10);

                bool has_we = false;
                uint64_t app_size = 0;

                const waywal_vdf_node_t *apps = waywal_vdf_find_child(item, "apps");
                if (apps && apps->type == VDF_NODE_OBJECT) {
                    const waywal_vdf_node_t *app_entry = waywal_vdf_find_child(apps, "431960");
                    if (app_entry) {
                        has_we = true;
                        if (app_entry->type == VDF_NODE_STRING && app_entry->string_value) {
                            app_size = strtoull(app_entry->string_value, NULL, 10);
                        }
                    }
                }

                add_or_merge_library(out_libs, inout_count, max_libs, path, label, total_size,
                                     app_size, has_we);
            } else if (item->type == VDF_NODE_STRING && item->string_value) {
                /* Format v1: "1" "/mnt/storage/SteamLibrary" */
                const char *path = item->string_value;
                const char *label = item->key;
                add_or_merge_library(out_libs, inout_count, max_libs, path, label, 0, 0, false);
            }
        }
    }

    waywal_vdf_free(doc);
}

static void probe_candidate_steam_root(const char *steam_root, waywal_steam_library_t *out_libs,
                                       size_t *inout_count, size_t max_libs)
{
    if (!steam_root || steam_root[0] == '\0' || *inout_count >= max_libs) {
        return;
    }

    char vdf_path[4096];
    if (snprintf(vdf_path, sizeof(vdf_path), "%s/steamapps/libraryfolders.vdf", steam_root) <
        (int)sizeof(vdf_path)) {
        struct stat st;
        if (stat(vdf_path, &st) == 0 && S_ISREG(st.st_mode)) {
            parse_libraryfolders_vdf(vdf_path, out_libs, inout_count, max_libs);
        }
    }

    /* Also check <steam_root>/libraryfolders.vdf */
    if (snprintf(vdf_path, sizeof(vdf_path), "%s/libraryfolders.vdf", steam_root) <
        (int)sizeof(vdf_path)) {
        struct stat st;
        if (stat(vdf_path, &st) == 0 && S_ISREG(st.st_mode)) {
            parse_libraryfolders_vdf(vdf_path, out_libs, inout_count, max_libs);
        }
    }

    /* Also record steam_root itself if it exists */
    struct stat root_st;
    if (stat(steam_root, &root_st) == 0 && S_ISDIR(root_st.st_mode)) {
        add_or_merge_library(out_libs, inout_count, max_libs, steam_root, "", 0, 0, false);
    }
}

static void probe_colon_separated_roots(const char *path_list, waywal_steam_library_t *out_libs,
                                        size_t *inout_count, size_t max_libs)
{
    if (!path_list || path_list[0] == '\0' || !out_libs || !inout_count) {
        return;
    }

    const char *p = path_list;
    while (*p && *inout_count < max_libs) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        if (len > 0 && len < 4096) {
            char candidate[4096];
            memcpy(candidate, p, len);
            candidate[len] = '\0';

            /* Trim spaces */
            char *start = candidate;
            while (*start && isspace((unsigned char)*start)) {
                start++;
            }
            char *end = start + strlen(start);
            while (end > start && isspace((unsigned char)*(end - 1))) {
                end--;
                *end = '\0';
            }

            if (start[0] != '\0') {
                probe_candidate_steam_root(start, out_libs, inout_count, max_libs);
                /* If it's a direct workshop path without libraryfolders.vdf */
                struct stat st;
                if (stat(start, &st) == 0 && S_ISDIR(st.st_mode)) {
                    add_or_merge_library(out_libs, inout_count, max_libs, start, "custom", 0, 0,
                                         true);
                }
            }
        }
        if (!colon) {
            break;
        }
        p = colon + 1;
    }
}

static void parse_workshop_dir_from_toml(const char *toml_path, char *out_dir, size_t max_len)
{
    if (!toml_path || !out_dir || max_len == 0) {
        return;
    }
    out_dir[0] = '\0';

    FILE *fp = fopen(toml_path, "r");
    if (!fp) {
        return;
    }

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '#' || *p == ';') {
            continue;
        }
        if (strncmp(p, "workshop_dir", 12) == 0) {
            p += 12;
            while (*p && isspace((unsigned char)*p)) {
                p++;
            }
            if (*p == '=') {
                p++;
                while (*p && isspace((unsigned char)*p)) {
                    p++;
                }
                char quote = *p;
                if (quote == '"' || quote == '\'') {
                    p++;
                    char *end_q = strchr(p, quote);
                    if (end_q) {
                        size_t val_len = (size_t)(end_q - p);
                        if (val_len < max_len) {
                            memcpy(out_dir, p, val_len);
                            out_dir[val_len] = '\0';
                        }
                    }
                }
                break;
            }
        }
    }
    fclose(fp);
}

size_t waywal_steam_discover_libraries(waywal_steam_library_t *out_libs, size_t max_libs)
{
    if (!out_libs || max_libs == 0) {
        return 0;
    }

    size_t count = 0;

    /* 1. Explicit Environment Variable: $WWAL_WORKSHOP_DIR */
    const char *env_ws = getenv("WWAL_WORKSHOP_DIR");
    if (env_ws && env_ws[0] != '\0') {
        probe_colon_separated_roots(env_ws, out_libs, &count, max_libs);
    }

    /* 2. Application Configuration: ~/.config/waywal/config.toml, ~/.config/wwal/config.toml */
    waywal_config_t cfg;
    if (waywal_config_load_default(&cfg) && cfg.daemon.workshop_dir[0] != '\0') {
        probe_colon_separated_roots(cfg.daemon.workshop_dir, out_libs, &count, max_libs);
    }

    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') {
        struct passwd *pw = getpwuid(getuid());
        if (pw) {
            home = pw->pw_dir;
        }
    }

    if (home && home[0] != '\0') {
        char custom_cfg[4096];
        char parsed_dir[4096];

        /* Probe ~/.config/waywal/config.toml */
        if (snprintf(custom_cfg, sizeof(custom_cfg), "%s/.config/waywal/config.toml", home) <
            (int)sizeof(custom_cfg)) {
            parse_workshop_dir_from_toml(custom_cfg, parsed_dir, sizeof(parsed_dir));
            if (parsed_dir[0] != '\0') {
                probe_colon_separated_roots(parsed_dir, out_libs, &count, max_libs);
            }
        }

        /* Probe ~/.config/wwal/config.toml */
        if (snprintf(custom_cfg, sizeof(custom_cfg), "%s/.config/wwal/config.toml", home) <
            (int)sizeof(custom_cfg)) {
            parse_workshop_dir_from_toml(custom_cfg, parsed_dir, sizeof(parsed_dir));
            if (parsed_dir[0] != '\0') {
                probe_colon_separated_roots(parsed_dir, out_libs, &count, max_libs);
            }
        }

        /* 3. Native Linux Steam: ~/.local/share/Steam and ~/.steam/steam, ~/.steam/root */
        static const char *const native_steam_rel[] = {
            ".local/share/Steam",
            ".steam/steam",
            ".steam/root",
        };
        for (size_t i = 0; i < sizeof(native_steam_rel) / sizeof(native_steam_rel[0]); i++) {
            char root_buf[4096];
            if (snprintf(root_buf, sizeof(root_buf), "%s/%s", home, native_steam_rel[i]) <
                (int)sizeof(root_buf)) {
                probe_candidate_steam_root(root_buf, out_libs, &count, max_libs);
            }
        }

        /* 4. Sandboxed Flatpak Steam */
        static const char *const flatpak_steam_rel[] = {
            ".var/app/com.valvesoftware.Steam/.local/share/Steam",
            ".var/app/com.valvesoftware.Steam/.steam/steam",
        };
        for (size_t i = 0; i < sizeof(flatpak_steam_rel) / sizeof(flatpak_steam_rel[0]); i++) {
            char root_buf[4096];
            if (snprintf(root_buf, sizeof(root_buf), "%s/%s", home, flatpak_steam_rel[i]) <
                (int)sizeof(root_buf)) {
                probe_candidate_steam_root(root_buf, out_libs, &count, max_libs);
            }
        }

        /* 5. Sandboxed Snap Steam */
        static const char *const snap_steam_rel[] = {
            "snap/steam/common/.local/share/Steam",
        };
        for (size_t i = 0; i < sizeof(snap_steam_rel) / sizeof(snap_steam_rel[0]); i++) {
            char root_buf[4096];
            if (snprintf(root_buf, sizeof(root_buf), "%s/%s", home, snap_steam_rel[i]) <
                (int)sizeof(root_buf)) {
                probe_candidate_steam_root(root_buf, out_libs, &count, max_libs);
            }
        }
    }

    return count;
}
