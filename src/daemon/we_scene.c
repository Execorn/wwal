// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/we_scene.h"

#include "waywal/log.h"
#include "waywal/toml_parser.h"

#ifndef WWAL_WE_PROJECT_ONLY
#if __has_include("linux-dmabuf-v1-client-protocol.h")
#include "waywal/dmabuf.h"
#else
#include <drm_fourcc.h>
#include <gbm.h>
#include <sys/types.h>
#define WAYWAL_MAX_BUFFER_PLANES 4
struct dmabuf_bo {
    int fd[WAYWAL_MAX_BUFFER_PLANES];
    uint32_t stride[WAYWAL_MAX_BUFFER_PLANES];
    uint32_t offset[WAYWAL_MAX_BUFFER_PLANES];
    uint64_t modifier;
    uint32_t num_planes;
    uint32_t width;
    uint32_t height;
    uint32_t drm_format;
    struct gbm_bo *gbm_bo;
    struct wl_buffer *wl_buffer;
    void *egl_image;
    void *map_data;
    uint32_t gl_tex;
    bool is_10bit;
    bool in_use;
};
struct dmabuf_context {
    int drm_fd;
    struct gbm_device *gbm;
    void *dmabuf_proto;
    void *default_feedback;
    uint64_t preferred_modifier;
    uint32_t preferred_format;
    bool has_10bit;
    uint32_t format_10bit;
    uint64_t modifier_10bit;
    bool has_nv12;
    bool has_p010;
    dev_t compositor_dev;
    bool has_compositor_dev;
    bool available;
    bool map_supported;
};
#endif
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <drm_fourcc.h>
// clang-format off
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>
// clang-format on
#endif
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* =========================================================================
 * Lightweight Recursive-Descent JSON DOM Parser
 * ========================================================================= */

typedef enum {
    JVAL_NULL = 0,
    JVAL_BOOL,
    JVAL_NUM,
    JVAL_STR,
    JVAL_ARR,
    JVAL_OBJ,
} we_jtype_t;

typedef struct we_jval we_jval_t;

typedef struct {
    char *key;
    we_jval_t *val;
} we_jkv_t;

struct we_jval {
    we_jtype_t type;
    bool bval;
    double num;
    char *str;
    struct {
        we_jval_t **items;
        size_t count;
    } arr;
    struct {
        we_jkv_t *entries;
        size_t count;
    } obj;
};

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
} json_lexer_t;

static void jval_free(we_jval_t *v)
{
    if (!v) {
        return;
    }
    if (v->type == JVAL_STR) {
        free(v->str);
    } else if (v->type == JVAL_ARR) {
        for (size_t i = 0; i < v->arr.count; i++) {
            jval_free(v->arr.items[i]);
        }
        free(v->arr.items);
    } else if (v->type == JVAL_OBJ) {
        for (size_t i = 0; i < v->obj.count; i++) {
            free(v->obj.entries[i].key);
            jval_free(v->obj.entries[i].val);
        }
        free(v->obj.entries);
    }
    free(v);
}

static void json_skip_ws(json_lexer_t *lx)
{
    while (lx->pos < lx->len) {
        char c = lx->src[lx->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            lx->pos++;
        } else if (c == '/' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '/') {
            lx->pos += 2;
            while (lx->pos < lx->len && lx->src[lx->pos] != '\n') {
                lx->pos++;
            }
        } else if (c == '/' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '*') {
            lx->pos += 2;
            while (lx->pos + 1 < lx->len &&
                   !(lx->src[lx->pos] == '*' && lx->src[lx->pos + 1] == '/')) {
                lx->pos++;
            }
            if (lx->pos + 1 < lx->len) {
                lx->pos += 2;
            } else {
                lx->pos = lx->len;
            }
        } else {
            break;
        }
    }
}

static char *json_parse_raw_string(json_lexer_t *lx)
{
    json_skip_ws(lx);
    if (lx->pos >= lx->len || lx->src[lx->pos] != '"') {
        return NULL;
    }
    lx->pos++; /* skip '"' */

    size_t cap = 64;
    size_t len = 0;
    char *out = malloc(cap);
    if (!out) {
        return NULL;
    }

    while (lx->pos < lx->len) {
        char c = lx->src[lx->pos++];
        if (c == '"') {
            out[len] = '\0';
            return out;
        }
        if (c == '\\' && lx->pos < lx->len) {
            char esc = lx->src[lx->pos++];
            switch (esc) {
            case '"':
                c = '"';
                break;
            case '\\':
                c = '\\';
                break;
            case '/':
                c = '/';
                break;
            case 'b':
                c = '\b';
                break;
            case 'f':
                c = '\f';
                break;
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            case 'u':
                if (lx->pos + 4 <= lx->len) {
                    char hex[5];
                    memcpy(hex, lx->src + lx->pos, 4);
                    hex[4] = '\0';
                    unsigned int cp = (unsigned int)strtoul(hex, NULL, 16);
                    lx->pos += 4;
                    c = (cp < 0x80 && cp > 0) ? (char)cp : '?';
                }
                break;
            default:
                c = esc;
                break;
            }
        }
        if (len + 2 > cap) {
            size_t ncap = cap * 2;
            char *nb = realloc(out, ncap);
            if (!nb) {
                free(out);
                return NULL;
            }
            out = nb;
            cap = ncap;
        }
        out[len++] = c;
    }

    free(out);
    return NULL;
}

static we_jval_t *json_parse_value(json_lexer_t *lx, int depth);

static we_jval_t *json_parse_array(json_lexer_t *lx, int depth)
{
    lx->pos++; /* skip '[' */
    we_jval_t *arr = calloc(1, sizeof(we_jval_t));
    if (!arr) {
        return NULL;
    }
    arr->type = JVAL_ARR;

    size_t cap = 0;
    while (lx->pos < lx->len) {
        json_skip_ws(lx);
        if (lx->pos < lx->len && lx->src[lx->pos] == ']') {
            lx->pos++;
            return arr;
        }
        we_jval_t *elem = json_parse_value(lx, depth + 1);
        if (!elem) {
            jval_free(arr);
            return NULL;
        }
        if (arr->arr.count + 1 > cap) {
            size_t ncap = cap == 0 ? 8 : cap * 2;
            we_jval_t **ni = realloc(arr->arr.items, ncap * sizeof(we_jval_t *));
            if (!ni) {
                jval_free(elem);
                jval_free(arr);
                return NULL;
            }
            arr->arr.items = ni;
            cap = ncap;
        }
        arr->arr.items[arr->arr.count++] = elem;

        json_skip_ws(lx);
        if (lx->pos < lx->len && lx->src[lx->pos] == ',') {
            lx->pos++;
        }
    }

    jval_free(arr);
    return NULL;
}

static we_jval_t *json_parse_object(json_lexer_t *lx, int depth)
{
    lx->pos++; /* skip '{' */
    we_jval_t *obj = calloc(1, sizeof(we_jval_t));
    if (!obj) {
        return NULL;
    }
    obj->type = JVAL_OBJ;

    size_t cap = 0;
    while (lx->pos < lx->len) {
        json_skip_ws(lx);
        if (lx->pos < lx->len && lx->src[lx->pos] == '}') {
            lx->pos++;
            return obj;
        }
        char *key = json_parse_raw_string(lx);
        if (!key) {
            jval_free(obj);
            return NULL;
        }
        json_skip_ws(lx);
        if (lx->pos >= lx->len || lx->src[lx->pos] != ':') {
            free(key);
            jval_free(obj);
            return NULL;
        }
        lx->pos++; /* skip ':' */

        we_jval_t *val = json_parse_value(lx, depth + 1);
        if (!val) {
            free(key);
            jval_free(obj);
            return NULL;
        }

        if (obj->obj.count + 1 > cap) {
            size_t ncap = cap == 0 ? 8 : cap * 2;
            we_jkv_t *ne = realloc(obj->obj.entries, ncap * sizeof(we_jkv_t));
            if (!ne) {
                free(key);
                jval_free(val);
                jval_free(obj);
                return NULL;
            }
            obj->obj.entries = ne;
            cap = ncap;
        }
        obj->obj.entries[obj->obj.count].key = key;
        obj->obj.entries[obj->obj.count].val = val;
        obj->obj.count++;

        json_skip_ws(lx);
        if (lx->pos < lx->len && lx->src[lx->pos] == ',') {
            lx->pos++;
        }
    }

    jval_free(obj);
    return NULL;
}

static we_jval_t *json_parse_value(json_lexer_t *lx, int depth)
{
    if (depth > 64) {
        return NULL;
    }
    json_skip_ws(lx);
    if (lx->pos >= lx->len) {
        return NULL;
    }

    char c = lx->src[lx->pos];
    if (c == '{') {
        return json_parse_object(lx, depth);
    }
    if (c == '[') {
        return json_parse_array(lx, depth);
    }
    if (c == '"') {
        char *s = json_parse_raw_string(lx);
        if (!s) {
            return NULL;
        }
        we_jval_t *v = calloc(1, sizeof(we_jval_t));
        if (!v) {
            free(s);
            return NULL;
        }
        v->type = JVAL_STR;
        v->str = s;
        return v;
    }
    if (lx->len - lx->pos >= 4 && strncmp(lx->src + lx->pos, "true", 4) == 0) {
        lx->pos += 4;
        we_jval_t *v = calloc(1, sizeof(we_jval_t));
        if (v) {
            v->type = JVAL_BOOL;
            v->bval = true;
        }
        return v;
    }
    if (lx->len - lx->pos >= 5 && strncmp(lx->src + lx->pos, "false", 5) == 0) {
        lx->pos += 5;
        we_jval_t *v = calloc(1, sizeof(we_jval_t));
        if (v) {
            v->type = JVAL_BOOL;
            v->bval = false;
        }
        return v;
    }
    if (lx->len - lx->pos >= 4 && strncmp(lx->src + lx->pos, "null", 4) == 0) {
        lx->pos += 4;
        we_jval_t *v = calloc(1, sizeof(we_jval_t));
        if (v) {
            v->type = JVAL_NULL;
        }
        return v;
    }
    if (c == '-' || c == '+' || (c >= '0' && c <= '9') || c == '.') {
        size_t rem = lx->len - lx->pos;
        size_t copy_len = rem < 63 ? rem : 63;
        char num_buf[64];
        memcpy(num_buf, lx->src + lx->pos, copy_len);
        num_buf[copy_len] = '\0';
        char *endptr = NULL;
        double num = strtod(num_buf, &endptr);
        if (endptr == num_buf) {
            return NULL;
        }
        lx->pos += (size_t)(endptr - num_buf);
        we_jval_t *v = calloc(1, sizeof(we_jval_t));
        if (v) {
            v->type = JVAL_NUM;
            v->num = num;
        }
        return v;
    }
    return NULL;
}

static we_jval_t *json_parse_root(const char *src, size_t len)
{
    if (!src || len == 0) {
        return NULL;
    }
    /* Skip optional UTF-8 BOM */
    size_t offset = 0;
    if (len >= 3 && (uint8_t)src[0] == 0xEF && (uint8_t)src[1] == 0xBB && (uint8_t)src[2] == 0xBF) {
        offset = 3;
    }
    json_lexer_t lx = {.src = src + offset, .len = len - offset, .pos = 0};
    return json_parse_value(&lx, 0);
}

static const we_jval_t *jobj_get(const we_jval_t *obj, const char *key)
{
    if (!obj || obj->type != JVAL_OBJ || !key) {
        return NULL;
    }
    for (size_t i = 0; i < obj->obj.count; i++) {
        if (strcmp(obj->obj.entries[i].key, key) == 0) {
            return obj->obj.entries[i].val;
        }
    }
    for (size_t i = 0; i < obj->obj.count; i++) {
        if (strcasecmp(obj->obj.entries[i].key, key) == 0) {
            return obj->obj.entries[i].val;
        }
    }
    return NULL;
}

static const char *jval_str(const we_jval_t *v, const char *def)
{
    if (!v) {
        return def;
    }
    if (v->type == JVAL_STR && v->str) {
        return v->str;
    }
    if (v->type == JVAL_OBJ) {
        const we_jval_t *inner = jobj_get(v, "value");
        if (inner && inner->type == JVAL_STR && inner->str) {
            return inner->str;
        }
    }
    return def;
}

static const we_user_prop_t *project_find_prop(const we_project_t *proj, const char *key)
{
    if (!proj || !key || key[0] == '\0') {
        return NULL;
    }
    for (uint32_t i = 0; i < proj->prop_count; i++) {
        if (strcasecmp(proj->props[i].key, key) == 0) {
            return &proj->props[i];
        }
    }
    return NULL;
}

static uint32_t parse_float_str(const char *s, float *out, uint32_t max_n)
{
    if (!s || !out || max_n == 0) {
        return 0;
    }
    const char *p = s;
    uint32_t count = 0;
    while (*p && count < max_n) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) {
            p++;
        }
        if (!*p) {
            break;
        }
        char *endptr = NULL;
        float v = strtof(p, &endptr);
        if (endptr == p) {
            break;
        }
        out[count++] = v;
        p = endptr;
    }
    return count;
}

static uint32_t jval_floats(const we_jval_t *v, const we_project_t *proj, float *out,
                            uint32_t max_n, char *out_user_prop, size_t prop_cap)
{
    if (out_user_prop && prop_cap > 0) {
        out_user_prop[0] = '\0';
    }
    if (!v || !out || max_n == 0) {
        return 0;
    }

    if (v->type == JVAL_OBJ) {
        const we_jval_t *u = jobj_get(v, "user");
        const char *ukey = NULL;
        if (u && u->type == JVAL_STR) {
            ukey = u->str;
        } else if (u && u->type == JVAL_OBJ) {
            ukey = jval_str(jobj_get(u, "name"), NULL);
        }
        if (ukey && ukey[0] != '\0') {
            if (out_user_prop && prop_cap > 0) {
                snprintf(out_user_prop, prop_cap, "%s", ukey);
            }
            const we_user_prop_t *up = project_find_prop(proj, ukey);
            if (up && up->num_components > 0) {
                uint32_t n = up->num_components < max_n ? up->num_components : max_n;
                for (uint32_t i = 0; i < n; i++) {
                    out[i] = up->val[i];
                }
                return n;
            }
        }
        const we_jval_t *inner = jobj_get(v, "value");
        if (inner) {
            return jval_floats(inner, proj, out, max_n, NULL, 0);
        }
        return 0;
    }

    if (v->type == JVAL_NUM) {
        out[0] = (float)v->num;
        return 1;
    }
    if (v->type == JVAL_BOOL) {
        out[0] = v->bval ? 1.0f : 0.0f;
        return 1;
    }
    if (v->type == JVAL_STR && v->str) {
        if (strcasecmp(v->str, "true") == 0) {
            out[0] = 1.0f;
            return 1;
        }
        if (strcasecmp(v->str, "false") == 0) {
            out[0] = 0.0f;
            return 1;
        }
        return parse_float_str(v->str, out, max_n);
    }
    if (v->type == JVAL_ARR) {
        uint32_t n = (uint32_t)v->arr.count < max_n ? (uint32_t)v->arr.count : max_n;
        for (uint32_t i = 0; i < n; i++) {
            float tmp = 0.0f;
            jval_floats(v->arr.items[i], proj, &tmp, 1, NULL, 0);
            out[i] = tmp;
        }
        return n;
    }
    return 0;
}

#ifndef WWAL_WE_PROJECT_ONLY
static float jval_float(const we_jval_t *v, const we_project_t *proj, float def,
                        char *out_user_prop, size_t prop_cap)
{
    float tmp = def;
    if (jval_floats(v, proj, &tmp, 1, out_user_prop, prop_cap) >= 1) {
        return tmp;
    }
    return def;
}

static bool jval_bool_resolved(const we_jval_t *v, const we_project_t *proj, bool def,
                               char *out_user_prop, size_t prop_cap)
{
    float tmp = def ? 1.0f : 0.0f;
    if (jval_floats(v, proj, &tmp, 1, out_user_prop, prop_cap) >= 1) {
        return tmp != 0.0f;
    }
    return def;
}
#endif

/* =========================================================================
 * project.json Parsing & User Property Overrides
 * ========================================================================= */

bool we_project_parse(const char *json_str, size_t len, we_project_t *out_proj)
{
    if (!json_str || len == 0 || !out_proj) {
        return false;
    }
    we_jval_t *root = json_parse_root(json_str, len);
    if (!root || root->type != JVAL_OBJ) {
        jval_free(root);
        return false;
    }

    snprintf(out_proj->title, sizeof(out_proj->title), "%s",
             jval_str(jobj_get(root, "title"), "Untitled Wallpaper"));
    snprintf(out_proj->type, sizeof(out_proj->type), "%s",
             jval_str(jobj_get(root, "type"), "scene"));
    snprintf(out_proj->file, sizeof(out_proj->file), "%s",
             jval_str(jobj_get(root, "file"), "scene.json"));
    snprintf(out_proj->preview, sizeof(out_proj->preview), "%s",
             jval_str(jobj_get(root, "preview"), "preview.jpg"));

    const we_jval_t *gen = jobj_get(root, "general");
    const we_jval_t *props = gen ? jobj_get(gen, "properties") : jobj_get(root, "properties");
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
            snprintf(up->text, sizeof(up->text), "%s", jval_str(jobj_get(pval, "text"), pkey));

            const char *ptype = jval_str(jobj_get(pval, "type"), "slider");
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

            const we_jval_t *vnode = jobj_get(pval, "value");
            if (vnode) {
                if (vnode->type == JVAL_STR && vnode->str) {
                    snprintf(up->str_val, sizeof(up->str_val), "%s", vnode->str);
                }
                up->num_components = jval_floats(vnode, NULL, up->val, 4, NULL, 0);
                if (up->num_components == 0 && up->type == WE_PROP_BOOL) {
                    up->num_components = 1;
                }
            }
        }
    }

    jval_free(root);
    return true;
}

bool we_project_load_dir(const char *workshop_dir, we_project_t *out_proj)
{
    if (!workshop_dir || !out_proj) {
        return false;
    }
    memset(out_proj, 0, sizeof(*out_proj));
    snprintf(out_proj->workshop_dir, sizeof(out_proj->workshop_dir), "%s", workshop_dir);

    /* Extract trailing directory basename as default workshop_id */
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
        /* Fallback if directory only has scene.pkg without project.json */
        snprintf(json_path, sizeof(json_path), "%s/scene.pkg", workshop_dir);
        struct stat st;
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

    uint32_t nf = parse_float_str(val_str, target->val, 4);
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

/* =========================================================================
 * Steam Workshop 431960 Directory Discovery
 * ========================================================================= */

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
    return false;
}

bool we_workshop_resolve_dir(const char *id_or_path, char *out_dir, size_t out_cap)
{
    if (!id_or_path || id_or_path[0] == '\0' || !out_dir || out_cap == 0) {
        return false;
    }

    struct stat st;
    if (stat(id_or_path, &st) == 0) {
        if (S_ISDIR(st.st_mode) && dir_has_workshop_files(id_or_path)) {
            snprintf(out_dir, out_cap, "%s", id_or_path);
            return true;
        }
        if (S_ISREG(st.st_mode)) {
            const char *slash = strrchr(id_or_path, '/');
            if (slash && slash > id_or_path) {
                size_t dlen = (size_t)(slash - id_or_path);
                if (dlen < out_cap) {
                    memcpy(out_dir, id_or_path, dlen);
                    out_dir[dlen] = '\0';
                    if (dir_has_workshop_files(out_dir)) {
                        return true;
                    }
                }
            }
        }
    }

    /* Search across candidate Steam Workshop 431960 roots */
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
        /* If root itself is a single workshop item dir, include it directly */
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

#ifndef WWAL_WE_PROJECT_ONLY

/* =========================================================================
 * Material, Effect, and Scene Graph Parsing
 * ========================================================================= */

static we_blend_mode_t parse_blend_mode(const char *str)
{
    if (!str) {
        return WE_BLEND_NORMAL;
    }
    if (strcasecmp(str, "additive") == 0 || strcasecmp(str, "add") == 0) {
        return WE_BLEND_ADDITIVE;
    }
    if (strcasecmp(str, "multiply") == 0 || strcasecmp(str, "mult") == 0) {
        return WE_BLEND_MULTIPLY;
    }
    if (strcasecmp(str, "screen") == 0) {
        return WE_BLEND_SCREEN;
    }
    if (strcasecmp(str, "disabled") == 0 || strcasecmp(str, "opaque") == 0 ||
        strcasecmp(str, "none") == 0) {
        return WE_BLEND_DISABLED;
    }
    return WE_BLEND_NORMAL;
}

static void pass_set_combo(we_material_pass_t *pass, const char *name, int32_t val)
{
    if (!pass || !name || name[0] == '\0') {
        return;
    }
    for (uint32_t i = 0; i < pass->combo_count; i++) {
        if (strcmp(pass->combos[i].name, name) == 0) {
            pass->combos[i].value = val;
            return;
        }
    }
    if (pass->combo_count < WE_SHADER_MAX_COMBOS) {
        snprintf(pass->combos[pass->combo_count].name, sizeof(pass->combos[0].name), "%s", name);
        pass->combos[pass->combo_count].value = val;
        pass->combo_count++;
    }
}

static void pass_set_const_val(we_material_pass_t *pass, const char *name, const we_jval_t *v,
                               const we_project_t *proj)
{
    if (!pass || !name || name[0] == '\0' || !v) {
        return;
    }
    we_const_val_t *cv = NULL;
    for (uint32_t i = 0; i < pass->const_val_count; i++) {
        if (strcmp(pass->const_vals[i].name, name) == 0) {
            cv = &pass->const_vals[i];
            break;
        }
    }
    if (!cv && pass->const_val_count < WE_MAX_CONST_VALS) {
        cv = &pass->const_vals[pass->const_val_count++];
        memset(cv, 0, sizeof(*cv));
        snprintf(cv->name, sizeof(cv->name), "%s", name);
    }
    if (cv) {
        cv->num_components = jval_floats(v, proj, cv->val, 4, cv->user_prop, sizeof(cv->user_prop));
        if (v->type == JVAL_OBJ) {
            const char *sc = jval_str(jobj_get(v, "script"), NULL);
            if (sc && sc[0] != '\0') {
                free(cv->script_src);
                cv->script_src = strdup(sc);
            }
        }
    }
}

static void parse_pass_json_overrides(we_material_pass_t *pass, const we_jval_t *pnode,
                                      const we_project_t *proj)
{
    if (!pass || !pnode || pnode->type != JVAL_OBJ) {
        return;
    }

    const char *sh = jval_str(jobj_get(pnode, "shader"), NULL);
    if (sh && sh[0] != '\0') {
        snprintf(pass->shader_name, sizeof(pass->shader_name), "%s", sh);
    }
    const char *bl = jval_str(jobj_get(pnode, "blending"), NULL);
    if (bl) {
        pass->blending = parse_blend_mode(bl);
    }
    const char *dt = jval_str(jobj_get(pnode, "depthtest"), NULL);
    if (dt) {
        pass->depthtest_enabled = (strcasecmp(dt, "enabled") == 0);
    }
    const char *cm = jval_str(jobj_get(pnode, "cullmode"), NULL);
    if (cm) {
        pass->cull_enabled = (strcasecmp(cm, "nocull") != 0 && strcasecmp(cm, "disabled") != 0);
    }

    const we_jval_t *texs = jobj_get(pnode, "textures");
    if (texs && texs->type == JVAL_ARR) {
        for (size_t i = 0; i < texs->arr.count && i < WE_MAX_PASS_TEXTURES; i++) {
            const char *tpath = jval_str(texs->arr.items[i], NULL);
            if (tpath && tpath[0] != '\0') {
                snprintf(pass->tex_paths[i], sizeof(pass->tex_paths[i]), "%s", tpath);
            }
        }
    }

    const we_jval_t *combos = jobj_get(pnode, "combos");
    if (combos && combos->type == JVAL_OBJ) {
        for (size_t i = 0; i < combos->obj.count; i++) {
            const char *ck = combos->obj.entries[i].key;
            float cv = jval_float(combos->obj.entries[i].val, proj, 0.0f, NULL, 0);
            pass_set_combo(pass, ck, (int32_t)cv);
        }
    }

    const we_jval_t *cvals = jobj_get(pnode, "constantshadervalues");
    if (cvals && cvals->type == JVAL_OBJ) {
        for (size_t i = 0; i < cvals->obj.count; i++) {
            pass_set_const_val(pass, cvals->obj.entries[i].key, cvals->obj.entries[i].val, proj);
        }
    }
}

static we_jval_t *pkg_load_json(const we_pkg_t *pkg, const char *rel_path)
{
    if (!pkg || !rel_path || rel_path[0] == '\0') {
        return NULL;
    }
    size_t sz = 0;
    bool owned = false;
    const uint8_t *data = we_pkg_resolve(pkg, rel_path, &sz, &owned);
    if (!data || sz == 0) {
        if (owned && data) {
            free((void *)data);
        }
        return NULL;
    }
    we_jval_t *root = json_parse_root((const char *)data, sz);
    if (owned) {
        free((void *)data);
    }
    return root;
}

static we_tex_t *pkg_load_tex_cpu(const we_pkg_t *pkg, const char *tex_name)
{
    if (!pkg || !tex_name || tex_name[0] == '\0') {
        return NULL;
    }
    if (strncmp(tex_name, "_rt_", 4) == 0 || strncasecmp(tex_name, "util/", 5) == 0) {
        return NULL;
    }

    char candidates[4][320];
    size_t len = strlen(tex_name);
    bool has_ext = (len > 4 && strcasecmp(tex_name + len - 4, ".tex") == 0);
    if (has_ext) {
        snprintf(candidates[0], sizeof(candidates[0]), "%s", tex_name);
        snprintf(candidates[1], sizeof(candidates[1]), "materials/%s", tex_name);
        candidates[2][0] = '\0';
        candidates[3][0] = '\0';
    } else {
        snprintf(candidates[0], sizeof(candidates[0]), "%s.tex", tex_name);
        snprintf(candidates[1], sizeof(candidates[1]), "materials/%s.tex", tex_name);
        snprintf(candidates[2], sizeof(candidates[2]), "%s", tex_name);
        snprintf(candidates[3], sizeof(candidates[3]), "materials/%s", tex_name);
    }

    for (int i = 0; i < 4; i++) {
        if (candidates[i][0] == '\0') {
            continue;
        }
        size_t sz = 0;
        bool owned = false;
        const uint8_t *data = we_pkg_resolve(pkg, candidates[i], &sz, &owned);
        if (data && sz > 0) {
            we_tex_t *tex = we_tex_parse_cpu(data, sz);
            if (owned) {
                free((void *)data);
            }
            if (tex) {
                return tex;
            }
        } else if (owned && data) {
            free((void *)data);
        }
    }
    return NULL;
}

static char *pkg_load_shader_stage_raw(const we_pkg_t *pkg, const char *shader_name, bool is_frag)
{
    if (!shader_name || shader_name[0] == '\0') {
        shader_name = "genericimage2";
    }
    const char *ext = is_frag ? ".frag" : ".vert";
    char candidates[3][320];
    snprintf(candidates[0], sizeof(candidates[0]), "shaders/%s%s", shader_name, ext);
    snprintf(candidates[1], sizeof(candidates[1]), "shaders/effects/%s%s", shader_name, ext);
    snprintf(candidates[2], sizeof(candidates[2]), "%s%s", shader_name, ext);

    if (pkg) {
        for (int i = 0; i < 3; i++) {
            size_t sz = 0;
            bool owned = false;
            const uint8_t *data = we_pkg_resolve(pkg, candidates[i], &sz, &owned);
            if (data && sz > 0) {
                char *str = malloc(sz + 1);
                if (str) {
                    memcpy(str, data, sz);
                    str[sz] = '\0';
                }
                if (owned) {
                    free((void *)data);
                }
                if (str) {
                    return str;
                }
            } else if (owned && data) {
                free((void *)data);
            }
        }
    }

    const char *builtin = we_shader_get_builtin(candidates[2]);
    if (!builtin) {
        builtin = we_shader_get_builtin(is_frag ? "genericimage2.frag" : "genericimage2.vert");
    }
    return builtin ? strdup(builtin) : NULL;
}

static void finalize_material_pass_cpu(we_material_pass_t *pass, const we_pkg_t *pkg)
{
    if (!pass) {
        return;
    }
    if (pass->shader_name[0] == '\0') {
        snprintf(pass->shader_name, sizeof(pass->shader_name), "genericimage2");
    }

    pass->bound_tex_mask = 0;
    for (uint32_t s = 0; s < WE_MAX_PASS_TEXTURES; s++) {
        if (pass->tex_paths[s][0] != '\0') {
            pass->bound_tex_mask |= (1u << s);
            if (!pass->textures[s]) {
                pass->textures[s] = pkg_load_tex_cpu(pkg, pass->tex_paths[s]);
            }
        }
    }
    for (uint32_t b = 0; b < pass->bind_count; b++) {
        if (pass->binds[b].slot >= 0 && pass->binds[b].slot < (int32_t)WE_MAX_PASS_TEXTURES) {
            pass->bound_tex_mask |= (1u << pass->binds[b].slot);
        }
    }

    bool tex0_is_gif = false;
    if (pass->textures[0]) {
        if ((pass->textures[0]->flags & WE_TEX_FLAG_IS_GIF) != 0) {
            tex0_is_gif = true;
        }
        if (pass->textures[0]->format == WE_TEX_FMT_RG88) {
            pass_set_combo(pass, "TEX0FORMAT", 8);
        } else if (pass->textures[0]->format == WE_TEX_FMT_R8) {
            pass_set_combo(pass, "TEX0FORMAT", 9);
        }
    }

    char *v_raw = pkg_load_shader_stage_raw(pkg, pass->shader_name, false);
    char *f_raw = pkg_load_shader_stage_raw(pkg, pass->shader_name, true);
    we_shader_preprocess_pair(v_raw, f_raw, pkg, pass->combos, pass->combo_count,
                              pass->bound_tex_mask, tex0_is_gif, &pass->prep);

    /* Load any default textures declared in shader inline uniform JSON */
    bool loaded_default_tex = false;
    for (uint32_t u = 0; u < pass->prep.meta.uniform_count; u++) {
        const we_shader_uniform_meta_t *um = &pass->prep.meta.uniforms[u];
        if (um->sampler_slot >= 0 && um->sampler_slot < (int32_t)WE_MAX_PASS_TEXTURES &&
            um->default_tex[0] != '\0' && pass->tex_paths[um->sampler_slot][0] == '\0') {
            snprintf(pass->tex_paths[um->sampler_slot], sizeof(pass->tex_paths[um->sampler_slot]),
                     "%s", um->default_tex);
            pass->textures[um->sampler_slot] = pkg_load_tex_cpu(pkg, um->default_tex);
            pass->bound_tex_mask |= (1u << (uint32_t)um->sampler_slot);
            loaded_default_tex = true;
        }
    }

    if (loaded_default_tex) {
        if (pass->textures[0]) {
            if ((pass->textures[0]->flags & WE_TEX_FLAG_IS_GIF) != 0) {
                tex0_is_gif = true;
            }
            if (pass->textures[0]->format == WE_TEX_FMT_RG88) {
                pass_set_combo(pass, "TEX0FORMAT", 8);
            } else if (pass->textures[0]->format == WE_TEX_FMT_R8) {
                pass_set_combo(pass, "TEX0FORMAT", 9);
            }
        }
        we_shader_preprocessed_free(&pass->prep);
        we_shader_preprocess_pair(v_raw, f_raw, pkg, pass->combos, pass->combo_count,
                                  pass->bound_tex_mask, tex0_is_gif, &pass->prep);
    }

    free(v_raw);
    free(f_raw);
}

static bool pass_uses_audio_spectrum(const we_material_pass_t *pass)
{
    if (!pass) {
        return false;
    }
    for (uint32_t u = 0; u < pass->prep.meta.uniform_count; u++) {
        if (strncmp(pass->prep.meta.uniforms[u].name, "g_AudioSpectrum", 15) == 0) {
            return true;
        }
    }
    if (pass->prep.vert_glsl && strstr(pass->prep.vert_glsl, "g_AudioSpectrum") != NULL) {
        return true;
    }
    if (pass->prep.frag_glsl && strstr(pass->prep.frag_glsl, "g_AudioSpectrum") != NULL) {
        return true;
    }
    return false;
}

static bool load_material_pass_from_file(we_material_pass_t *pass, const we_pkg_t *pkg,
                                         const char *mat_path, const we_project_t *proj)
{
    if (!pass || !pkg || !mat_path || mat_path[0] == '\0') {
        return false;
    }
    we_jval_t *mroot = pkg_load_json(pkg, mat_path);
    if (!mroot) {
        return false;
    }
    const we_jval_t *passes = jobj_get(mroot, "passes");
    if (passes && passes->type == JVAL_ARR && passes->arr.count > 0) {
        parse_pass_json_overrides(pass, passes->arr.items[0], proj);
    } else {
        parse_pass_json_overrides(pass, mroot, proj);
    }
    jval_free(mroot);
    return true;
}

static we_puppet_t *pkg_load_puppet_cpu(const we_pkg_t *pkg, const char *mdl_path)
{
    if (!pkg || !mdl_path || mdl_path[0] == '\0') {
        return NULL;
    }
    char candidates[2][320];
    snprintf(candidates[0], sizeof(candidates[0]), "%s", mdl_path);
    snprintf(candidates[1], sizeof(candidates[1]), "models/%s", mdl_path);
    for (int i = 0; i < 2; i++) {
        size_t sz = 0;
        bool owned = false;
        const uint8_t *data = we_pkg_resolve(pkg, candidates[i], &sz, &owned);
        if (data && sz > 0) {
            we_puppet_t *p = we_puppet_parse_cpu(data, sz);
            if (owned) {
                free((void *)data);
            }
            if (p) {
                return p;
            }
        } else if (owned && data) {
            free((void *)data);
        }
    }
    return NULL;
}

static void apply_puppet_layers_json(we_puppet_t *puppet, const we_jval_t *lnode,
                                     const we_project_t *proj)
{
    if (!puppet || !lnode || lnode->type != JVAL_ARR || lnode->arr.count == 0) {
        return;
    }
    uint32_t count = 0;
    for (size_t i = 0; i < lnode->arr.count && count < WE_MDL_MAX_LAYERS; i++) {
        const we_jval_t *it = lnode->arr.items[i];
        if (!it || it->type != JVAL_OBJ) {
            continue;
        }
        we_puppet_layer_t *ly = &puppet->layers[count++];
        memset(ly, 0, sizeof(*ly));
        const we_jval_t *anim_node = jobj_get(it, "animation");
        const char *aname = jval_str(anim_node, NULL);
        if (aname && aname[0] != '\0' && !isdigit((unsigned char)aname[0])) {
            snprintf(ly->anim_name, sizeof(ly->anim_name), "%s", aname);
            ly->anim_id = -1;
        } else {
            ly->anim_id = (int32_t)jval_float(anim_node, proj, 0.0f, NULL, 0);
        }
        const char *name_field = jval_str(jobj_get(it, "name"), NULL);
        if (name_field && name_field[0] != '\0' && ly->anim_name[0] == '\0') {
            snprintf(ly->anim_name, sizeof(ly->anim_name), "%s", name_field);
        }
        ly->rate = jval_float(jobj_get(it, "rate"), proj, 1.0f, NULL, 0);
        ly->blend = jval_float(jobj_get(it, "blend"), proj, 1.0f, NULL, 0);
        ly->visible = jval_bool_resolved(jobj_get(it, "visible"), proj, true, NULL, 0);
    }
    if (count > 0) {
        puppet->layer_count = count;
    }
}

static we_particle_sys_t *pkg_load_particle_cpu(const we_pkg_t *pkg, const char *part_path)
{
    if (!pkg || !part_path || part_path[0] == '\0') {
        return NULL;
    }
    char candidates[2][320];
    snprintf(candidates[0], sizeof(candidates[0]), "%s", part_path);
    snprintf(candidates[1], sizeof(candidates[1]), "particles/%s", part_path);
    for (int i = 0; i < 2; i++) {
        size_t sz = 0;
        bool owned = false;
        const uint8_t *data = we_pkg_resolve(pkg, candidates[i], &sz, &owned);
        if (data && sz > 0) {
            we_particle_sys_t *ps = we_particle_create_from_json((const char *)data, sz);
            if (owned) {
                free((void *)data);
            }
            if (ps) {
                return ps;
            }
        } else if (owned && data) {
            free((void *)data);
        }
    }
    return NULL;
}

static void apply_particle_instance_override(we_particle_sys_t *psys, const we_jval_t *iovr,
                                             const we_project_t *proj)
{
    if (!psys || !iovr || iovr->type != JVAL_OBJ) {
        return;
    }
    float rate_mul = jval_float(jobj_get(iovr, "rate"), proj, 1.0f, NULL, 0);
    float count_mul = jval_float(jobj_get(iovr, "count"), proj, 1.0f, NULL, 0);
    float size_mul = jval_float(jobj_get(iovr, "size"), proj, 1.0f, NULL, 0);
    float life_mul = jval_float(jobj_get(iovr, "lifetime"), proj, 1.0f, NULL, 0);
    float speed_mul = jval_float(jobj_get(iovr, "speed"), proj, 1.0f, NULL, 0);
    float alpha_mul = jval_float(jobj_get(iovr, "alpha"), proj, 1.0f, NULL, 0);

    bool has_color = false;
    float col_ovr[3] = {1.0f, 1.0f, 1.0f};
    if (jval_floats(jobj_get(iovr, "colorn"), proj, col_ovr, 3, NULL, 0) >= 3 ||
        jval_floats(jobj_get(iovr, "color"), proj, col_ovr, 3, NULL, 0) >= 3) {
        if (col_ovr[0] > 1.0f || col_ovr[1] > 1.0f || col_ovr[2] > 1.0f) {
            col_ovr[0] /= 255.0f;
            col_ovr[1] /= 255.0f;
            col_ovr[2] /= 255.0f;
        }
        has_color = true;
    }

    if (count_mul > 0.0f && fabsf(count_mul - 1.0f) > 1e-4f) {
        uint32_t nc = (uint32_t)((float)psys->max_count * count_mul + 0.5f);
        if (nc < 1u) {
            nc = 1u;
        }
        if (nc > WE_PARTICLE_MAX_CAP) {
            nc = WE_PARTICLE_MAX_CAP;
        }
        if (nc > psys->padded_cap) {
            (void)we_particle_realloc_capacity(psys, nc);
        } else {
            psys->max_count = nc;
        }
    }
    for (uint32_t e = 0; e < psys->emitter_count; e++) {
        psys->emitters[e].rate *= rate_mul;
        if (psys->emitters[e].instantaneous > 0 && count_mul > 0.0f) {
            uint32_t inst = (uint32_t)((float)psys->emitters[e].instantaneous * count_mul + 0.5f);
            psys->emitters[e].instantaneous = inst > 0 ? inst : 1u;
        }
    }

    bool found_size = false;
    bool found_life = false;
    bool found_alpha = false;
    bool found_vel = false;
    bool found_color = false;

    for (uint32_t i = 0; i < psys->initializer_count; i++) {
        we_particle_initializer_t *in = &psys->initializers[i];
        switch (in->type) {
        case WE_INIT_SIZE_RANDOM:
            in->min_val[0] *= size_mul;
            in->max_val[0] *= size_mul;
            found_size = true;
            break;
        case WE_INIT_LIFETIME_RANDOM:
            in->min_val[0] *= life_mul;
            in->max_val[0] *= life_mul;
            found_life = true;
            break;
        case WE_INIT_ALPHA_RANDOM:
            in->min_val[0] *= alpha_mul;
            in->max_val[0] *= alpha_mul;
            found_alpha = true;
            break;
        case WE_INIT_VELOCITY_RANDOM:
            for (int k = 0; k < 3; k++) {
                in->min_val[k] *= speed_mul;
                in->max_val[k] *= speed_mul;
            }
            found_vel = true;
            break;
        case WE_INIT_TURBULENT_VELOCITY_RANDOM:
            in->min_val[0] *= speed_mul;
            in->max_val[0] *= speed_mul;
            in->scale *= speed_mul;
            found_vel = true;
            break;
        case WE_INIT_COLOR_RANDOM:
            if (has_color) {
                for (int k = 0; k < 3; k++) {
                    in->min_val[k] = col_ovr[k];
                    in->max_val[k] = col_ovr[k];
                }
            }
            found_color = true;
            break;
        default:
            break;
        }
    }

    if (!found_size && fabsf(size_mul - 1.0f) > 1e-4f &&
        psys->initializer_count < WE_PARTICLE_MAX_INITIALIZERS) {
        we_particle_initializer_t *in = &psys->initializers[psys->initializer_count++];
        memset(in, 0, sizeof(*in));
        in->type = WE_INIT_SIZE_RANDOM;
        in->min_val[0] = 20.0f * size_mul;
        in->max_val[0] = 20.0f * size_mul;
        in->exponent = 1.0f;
    }
    if (!found_life && fabsf(life_mul - 1.0f) > 1e-4f &&
        psys->initializer_count < WE_PARTICLE_MAX_INITIALIZERS) {
        we_particle_initializer_t *in = &psys->initializers[psys->initializer_count++];
        memset(in, 0, sizeof(*in));
        in->type = WE_INIT_LIFETIME_RANDOM;
        in->min_val[0] = 1.0f * life_mul;
        in->max_val[0] = 1.0f * life_mul;
        in->exponent = 1.0f;
    }
    if (!found_alpha && fabsf(alpha_mul - 1.0f) > 1e-4f &&
        psys->initializer_count < WE_PARTICLE_MAX_INITIALIZERS) {
        we_particle_initializer_t *in = &psys->initializers[psys->initializer_count++];
        memset(in, 0, sizeof(*in));
        in->type = WE_INIT_ALPHA_RANDOM;
        in->min_val[0] = alpha_mul;
        in->max_val[0] = alpha_mul;
        in->exponent = 1.0f;
    }
    if (!found_vel && fabsf(speed_mul - 1.0f) > 1e-4f &&
        psys->initializer_count < WE_PARTICLE_MAX_INITIALIZERS) {
        we_particle_initializer_t *in = &psys->initializers[psys->initializer_count++];
        memset(in, 0, sizeof(*in));
        in->type = WE_INIT_VELOCITY_RANDOM;
        in->exponent = 1.0f;
    }
    if (!found_color && has_color && psys->initializer_count < WE_PARTICLE_MAX_INITIALIZERS) {
        we_particle_initializer_t *in = &psys->initializers[psys->initializer_count++];
        memset(in, 0, sizeof(*in));
        in->type = WE_INIT_COLOR_RANDOM;
        for (int k = 0; k < 3; k++) {
            in->min_val[k] = col_ovr[k];
            in->max_val[k] = col_ovr[k];
        }
        in->exponent = 1.0f;
    }

    we_particle_reset_and_prewarm(psys);
}

static void sync_layer_from_obj(const we_scene_object_t *obj, we_script_layer_state_t *ls)
{
    memset(ls, 0, sizeof(*ls));
    memcpy(ls->origin, obj->origin, sizeof(ls->origin));
    memcpy(ls->scale, obj->scale, sizeof(ls->scale));
    memcpy(ls->angles, obj->angles, sizeof(ls->angles));
    memcpy(ls->size, obj->size, sizeof(ls->size));
    ls->alpha = obj->alpha;
    memcpy(ls->color, obj->color, sizeof(ls->color));
    ls->visible = obj->visible;
    snprintf(ls->name, sizeof(ls->name), "%s", obj->name);
    ls->id = obj->id;
    ls->dirty_mask = 0;
}

static void sync_obj_from_layer(we_scene_object_t *obj, const we_script_layer_state_t *ls)
{
    if (!obj || !ls || ls->dirty_mask == 0) {
        return;
    }
    if (ls->dirty_mask & WE_LAYER_DIRTY_ORIGIN) {
        memcpy(obj->origin, ls->origin, sizeof(obj->origin));
    }
    if (ls->dirty_mask & WE_LAYER_DIRTY_SCALE) {
        memcpy(obj->scale, ls->scale, sizeof(obj->scale));
    }
    if (ls->dirty_mask & WE_LAYER_DIRTY_ANGLES) {
        memcpy(obj->angles, ls->angles, sizeof(obj->angles));
    }
    if (ls->dirty_mask & WE_LAYER_DIRTY_SIZE) {
        memcpy(obj->size, ls->size, sizeof(obj->size));
    }
    if (ls->dirty_mask & WE_LAYER_DIRTY_ALPHA) {
        obj->alpha = ls->alpha;
        obj->alpha_user_prop[0] = '\0';
    }
    if (ls->dirty_mask & WE_LAYER_DIRTY_COLOR) {
        memcpy(obj->color, ls->color, sizeof(obj->color));
        obj->color_user_prop[0] = '\0';
    }
    if (ls->dirty_mask & WE_LAYER_DIRTY_VISIBLE) {
        obj->visible = ls->visible;
        obj->visible_user_prop[0] = '\0';
    }
}

static void sync_scene_layers_to_vm(const we_scene_t *scene, we_script_t *vm)
{
    if (!scene || !vm) {
        return;
    }
    uint32_t count = scene->object_count < WE_SCRIPT_MAX_SCENE_LAYERS ? scene->object_count
                                                                      : WE_SCRIPT_MAX_SCENE_LAYERS;
    for (uint32_t i = 0; i < count; i++) {
        we_script_layer_state_t ls;
        sync_layer_from_obj(&scene->objects[i], &ls);
        we_script_register_scene_layer(vm, i, &ls);
    }
}

static void sync_scene_layers_from_vm(we_scene_t *scene, const we_script_t *vm)
{
    if (!scene || !vm) {
        return;
    }
    uint32_t count = scene->object_count < WE_SCRIPT_MAX_SCENE_LAYERS ? scene->object_count
                                                                      : WE_SCRIPT_MAX_SCENE_LAYERS;
    for (uint32_t i = 0; i < count; i++) {
        we_script_layer_state_t ls;
        if (we_script_read_scene_layer(vm, i, &ls)) {
            sync_obj_from_layer(&scene->objects[i], &ls);
        }
    }
}

static void build_script_env(const we_scene_t *scene, uint32_t canvas_w, uint32_t canvas_h,
                             float dt_sec, float time_sec,
                             const waywal_shader_frame_params_t *params, we_script_env_t *env)
{
    memset(env, 0, sizeof(*env));
    env->frametime = dt_sec;
    env->runtime = time_sec;
    env->time_of_day = params ? (params->date[3] / 86400.0f) : 0.5f;
    float cw = canvas_w > 0 ? (float)canvas_w : scene->ortho_width;
    float ch = canvas_h > 0 ? (float)canvas_h : scene->ortho_height;
    env->screen_resolution[0] = cw;
    env->screen_resolution[1] = ch;
    env->canvas_size[0] = scene->ortho_width;
    env->canvas_size[1] = scene->ortho_height;

    float ptr_x = 0.5f, ptr_y = 0.5f;
    if (params && cw > 0.0f && ch > 0.0f && (params->mouse[0] > 0.0f || params->mouse[1] > 0.0f)) {
        ptr_x = params->mouse[0] / cw;
        ptr_y = params->mouse[1] / ch;
    }
    env->cursor_screen[0] = ptr_x * cw;
    env->cursor_screen[1] = ptr_y * ch;
    env->cursor_world[0] = ptr_x * scene->ortho_width;
    env->cursor_world[1] = (1.0f - ptr_y) * scene->ortho_height;
    env->cursor_world[2] = 0.0f;
    env->cursor_left_down = (params && (params->mouse[2] > 0.0f || params->mouse[3] > 0.0f));

    if (params && params->audio) {
        for (int i = 0; i < 64; i++) {
            int band = (i * (int)WAYWAL_AUDIO_BARK_BANDS) / 64;
            float v = params->audio->bands[band];
            env->audio_left[i] = v;
            env->audio_right[i] = v;
            env->audio_average[i] = v;
        }
    }
}

static void bind_project_props_to_script(we_script_t *vm, const we_project_t *proj)
{
    if (!vm || !proj) {
        return;
    }
    for (uint32_t i = 0; i < proj->prop_count; i++) {
        const we_user_prop_t *up = &proj->props[i];
        const char *sv =
            (up->type == WE_PROP_TEXT || (up->num_components == 0 && up->str_val[0] != '\0'))
                ? up->str_val
                : NULL;
        we_script_set_prop(vm, up->key, up->val, up->num_components, sv);
    }
}

static void attach_object_script(we_scene_t *scene, we_scene_object_t *obj,
                                 we_script_prop_target_t target, const char *uniform_name,
                                 const we_jval_t *pnode, const char *explicit_src)
{
    if (!scene || !obj || obj->script_count >= WE_MAX_OBJECT_SCRIPTS) {
        return;
    }
    const char *src = explicit_src;
    if (!src && pnode && pnode->type == JVAL_OBJ) {
        src = jval_str(jobj_get(pnode, "script"), NULL);
    }
    if (!src || src[0] == '\0') {
        return;
    }

    we_script_t *vm = we_script_create(src, strlen(src), &scene->script_shared);
    if (!vm) {
        return;
    }

    bind_project_props_to_script(vm, &scene->project);

    if (pnode && pnode->type == JVAL_OBJ) {
        const we_jval_t *sp = jobj_get(pnode, "scriptproperties");
        if (sp && sp->type == JVAL_OBJ) {
            for (size_t i = 0; i < sp->obj.count; i++) {
                const char *k = sp->obj.entries[i].key;
                const we_jval_t *v = sp->obj.entries[i].val;
                if (!k || !v) {
                    continue;
                }
                float fvals[4] = {0};
                uint32_t n = jval_floats(v, &scene->project, fvals, 4, NULL, 0);
                const char *sv = (n == 0) ? jval_str(v, NULL) : NULL;
                we_script_set_prop(vm, k, fvals, n, sv);
            }
        }
    }

    if (we_script_uses_audio(vm)) {
        scene->uses_audio = true;
    }

    if (target == WE_SCRIPT_PROP_ALPHA) {
        obj->alpha_user_prop[0] = '\0';
    } else if (target == WE_SCRIPT_PROP_COLOR) {
        obj->color_user_prop[0] = '\0';
    } else if (target == WE_SCRIPT_PROP_VISIBLE) {
        obj->visible_user_prop[0] = '\0';
    }

    we_obj_script_t *slot = &obj->scripts[obj->script_count++];
    slot->target = target;
    if (uniform_name && uniform_name[0] != '\0') {
        snprintf(slot->uniform_name, sizeof(slot->uniform_name), "%s", uniform_name);
    } else {
        slot->uniform_name[0] = '\0';
    }
    slot->vm = vm;
}

static void apply_scene_script_user_properties(we_scene_t *scene)
{
    if (!scene) {
        return;
    }
    we_script_env_t env;
    build_script_env(scene, (uint32_t)scene->ortho_width, (uint32_t)scene->ortho_height,
                     1.0f / 60.0f, 0.0f, NULL, &env);

    for (uint32_t i = 0; i < scene->object_count; i++) {
        we_scene_object_t *obj = &scene->objects[i];
        for (uint32_t s = 0; s < obj->script_count; s++) {
            we_obj_script_t *sc = &obj->scripts[s];
            if (!sc->vm) {
                continue;
            }
            we_script_layer_state_t ls;
            sync_scene_layers_to_vm(scene, sc->vm);
            sync_layer_from_obj(obj, &ls);
            if (we_script_eval_apply_user_properties(sc->vm, &env, &ls)) {
                sync_obj_from_layer(obj, &ls);
                sync_scene_layers_from_vm(scene, sc->vm);
            }
        }
    }
}

static void init_scene_scripts(we_scene_t *scene)
{
    if (!scene) {
        return;
    }
    we_script_env_t env;
    build_script_env(scene, (uint32_t)scene->ortho_width, (uint32_t)scene->ortho_height,
                     1.0f / 60.0f, 0.0f, NULL, &env);

    for (uint32_t i = 0; i < scene->object_count; i++) {
        we_scene_object_t *obj = &scene->objects[i];
        for (uint32_t s = 0; s < obj->script_count; s++) {
            we_obj_script_t *sc = &obj->scripts[s];
            if (!sc->vm) {
                continue;
            }
            we_script_layer_state_t ls;
            sync_scene_layers_to_vm(scene, sc->vm);
            sync_layer_from_obj(obj, &ls);
            if (we_script_eval_apply_user_properties(sc->vm, &env, &ls)) {
                sync_obj_from_layer(obj, &ls);
                sync_scene_layers_from_vm(scene, sc->vm);
            }

            sync_scene_layers_to_vm(scene, sc->vm);
            sync_layer_from_obj(obj, &ls);

            float in_val[4] = {0};
            float out_val[4] = {0};
            uint32_t ncomp = 1;
            we_const_val_t *target_cv = NULL;

            switch (sc->target) {
            case WE_SCRIPT_PROP_ORIGIN:
                memcpy(in_val, obj->origin, 3 * sizeof(float));
                ncomp = 3;
                break;
            case WE_SCRIPT_PROP_SCALE:
                memcpy(in_val, obj->scale, 3 * sizeof(float));
                ncomp = 3;
                break;
            case WE_SCRIPT_PROP_ANGLES:
                memcpy(in_val, obj->angles, 3 * sizeof(float));
                ncomp = 3;
                break;
            case WE_SCRIPT_PROP_ALPHA:
                in_val[0] = obj->alpha;
                ncomp = 1;
                break;
            case WE_SCRIPT_PROP_COLOR:
                memcpy(in_val, obj->color, 3 * sizeof(float));
                ncomp = 3;
                break;
            case WE_SCRIPT_PROP_VISIBLE:
                in_val[0] = obj->visible ? 1.0f : 0.0f;
                ncomp = 1;
                break;
            case WE_SCRIPT_PROP_UNIFORM:
                for (uint32_t c = 0; c < obj->base_pass.const_val_count; c++) {
                    if (strcasecmp(obj->base_pass.const_vals[c].name, sc->uniform_name) == 0) {
                        target_cv = &obj->base_pass.const_vals[c];
                        break;
                    }
                }
                if (target_cv) {
                    ncomp = target_cv->num_components > 0 ? target_cv->num_components : 1u;
                    memcpy(in_val, target_cv->val, ncomp * sizeof(float));
                }
                break;
            }

            memcpy(out_val, in_val, ncomp * sizeof(float));
            if (we_script_eval_init(sc->vm, &env, &ls, in_val, ncomp, out_val)) {
                if (sc->target == WE_SCRIPT_PROP_UNIFORM ||
                    memcmp(out_val, in_val, ncomp * sizeof(float)) != 0) {
                    switch (sc->target) {
                    case WE_SCRIPT_PROP_ORIGIN:
                        memcpy(obj->origin, out_val, 3 * sizeof(float));
                        break;
                    case WE_SCRIPT_PROP_SCALE:
                        memcpy(obj->scale, out_val, 3 * sizeof(float));
                        break;
                    case WE_SCRIPT_PROP_ANGLES:
                        memcpy(obj->angles, out_val, 3 * sizeof(float));
                        break;
                    case WE_SCRIPT_PROP_ALPHA:
                        obj->alpha = out_val[0];
                        break;
                    case WE_SCRIPT_PROP_COLOR:
                        memcpy(obj->color, out_val, 3 * sizeof(float));
                        break;
                    case WE_SCRIPT_PROP_VISIBLE:
                        obj->visible = (out_val[0] != 0.0f);
                        break;
                    case WE_SCRIPT_PROP_UNIFORM:
                        if (target_cv) {
                            memcpy(target_cv->val, out_val, ncomp * sizeof(float));
                        }
                        break;
                    }
                }
                sync_obj_from_layer(obj, &ls);
                sync_scene_layers_from_vm(scene, sc->vm);
            }
        }
    }
}

static void parse_scene_object(we_scene_t *scene, const we_jval_t *onode, we_scene_object_t *obj)
{
    memset(obj, 0, sizeof(*obj));
    obj->id = (int32_t)jval_float(jobj_get(onode, "id"), &scene->project, -1.0f, NULL, 0);
    obj->parent_id =
        (int32_t)jval_float(jobj_get(onode, "parent"), &scene->project, -1.0f, NULL, 0);
    snprintf(obj->name, sizeof(obj->name), "%s", jval_str(jobj_get(onode, "name"), "layer"));

    if (obj->parent_id >= 0) {
        obj->origin[0] = 0.0f;
        obj->origin[1] = 0.0f;
        obj->origin[2] = 0.0f;
    } else {
        obj->origin[0] = scene->ortho_width * 0.5f;
        obj->origin[1] = scene->ortho_height * 0.5f;
        obj->origin[2] = 0.0f;
    }
    jval_floats(jobj_get(onode, "origin"), &scene->project, obj->origin, 3, NULL, 0);

    obj->scale[0] = 1.0f;
    obj->scale[1] = 1.0f;
    obj->scale[2] = 1.0f;
    jval_floats(jobj_get(onode, "scale"), &scene->project, obj->scale, 3, NULL, 0);

    jval_floats(jobj_get(onode, "angles"), &scene->project, obj->angles, 3, NULL, 0);

    obj->size[0] = scene->ortho_width;
    obj->size[1] = scene->ortho_height;
    bool has_explicit_size =
        (jval_floats(jobj_get(onode, "size"), &scene->project, obj->size, 2, NULL, 0) >= 2);

    jval_floats(jobj_get(onode, "parallaxDepth"), &scene->project, obj->parallax_depth, 2, NULL, 0);

    obj->alpha = jval_float(jobj_get(onode, "alpha"), &scene->project, 1.0f, obj->alpha_user_prop,
                            sizeof(obj->alpha_user_prop));
    obj->brightness = jval_float(jobj_get(onode, "brightness"), &scene->project, 1.0f, NULL, 0);

    obj->color[0] = 1.0f;
    obj->color[1] = 1.0f;
    obj->color[2] = 1.0f;
    jval_floats(jobj_get(onode, "color"), &scene->project, obj->color, 3, obj->color_user_prop,
                sizeof(obj->color_user_prop));

    obj->color_blend_mode =
        (int32_t)jval_float(jobj_get(onode, "colorBlendMode"), &scene->project, 0.0f, NULL, 0);
    obj->visible = jval_bool_resolved(jobj_get(onode, "visible"), &scene->project, true,
                                      obj->visible_user_prop, sizeof(obj->visible_user_prop));
    obj->copybackground =
        jval_bool_resolved(jobj_get(onode, "copybackground"), &scene->project, false, NULL, 0);

    char mat_path[256] = {0};
    char mdl_path[256] = {0};
    char part_path[256] = {0};

    const char *direct_puppet = jval_str(jobj_get(onode, "puppet"), NULL);
    if (!direct_puppet) {
        direct_puppet = jval_str(jobj_get(onode, "model"), NULL);
    }
    if (direct_puppet && direct_puppet[0] != '\0') {
        snprintf(mdl_path, sizeof(mdl_path), "%s", direct_puppet);
    }

    const char *direct_part = jval_str(jobj_get(onode, "particle"), NULL);
    if (direct_part && direct_part[0] != '\0') {
        snprintf(part_path, sizeof(part_path), "%s", direct_part);
    }

    const char *img_path = jval_str(jobj_get(onode, "image"), NULL);
    if (img_path && img_path[0] != '\0') {
        snprintf(obj->image_path, sizeof(obj->image_path), "%s", img_path);
        size_t ilen = strlen(img_path);
        if (ilen > 4 && strcasecmp(img_path + ilen - 4, ".mdl") == 0) {
            snprintf(mdl_path, sizeof(mdl_path), "%s", img_path);
        } else {
            we_jval_t *mdl = pkg_load_json(scene->pkg, img_path);
            if (mdl) {
                const char *m = jval_str(jobj_get(mdl, "material"), NULL);
                if (m) {
                    snprintf(mat_path, sizeof(mat_path), "%s", m);
                }
                const char *p = jval_str(jobj_get(mdl, "puppet"), NULL);
                if (p && p[0] != '\0') {
                    snprintf(mdl_path, sizeof(mdl_path), "%s", p);
                }
                if (jobj_get(mdl, "emitter") || jobj_get(mdl, "initializer") ||
                    jobj_get(mdl, "operator")) {
                    snprintf(part_path, sizeof(part_path), "%s", img_path);
                }
                obj->fullscreen = jval_bool_resolved(jobj_get(mdl, "fullscreen"), &scene->project,
                                                     false, NULL, 0);
                obj->passthrough = jval_bool_resolved(jobj_get(mdl, "passthrough"), &scene->project,
                                                      false, NULL, 0);
                float mw = jval_float(jobj_get(mdl, "width"), &scene->project, 0.0f, NULL, 0);
                float mh = jval_float(jobj_get(mdl, "height"), &scene->project, 0.0f, NULL, 0);
                if (mw > 0.0f && mh > 0.0f && !has_explicit_size) {
                    obj->size[0] = mw;
                    obj->size[1] = mh;
                    has_explicit_size = true;
                }
                jval_free(mdl);
            } else {
                snprintf(mat_path, sizeof(mat_path), "%s", img_path);
            }
        }
    }

    /* Load 2D Puppet Warp .mdl skeletal mesh if referenced */
    if (mdl_path[0] != '\0') {
        obj->puppet = pkg_load_puppet_cpu(scene->pkg, mdl_path);
        if (obj->puppet) {
            const we_jval_t *pl = jobj_get(onode, "puppet_layers");
            if (!pl) {
                pl = jobj_get(onode, "puppetlayers");
            }
            if (!pl) {
                pl = jobj_get(onode, "animationlayers");
            }
            if (pl) {
                apply_puppet_layers_json(obj->puppet, pl, &scene->project);
            }
            if (mat_path[0] == '\0' && obj->puppet->mat_json_file[0] != '\0') {
                snprintf(mat_path, sizeof(mat_path), "%s", obj->puppet->mat_json_file);
            }
            if (!has_explicit_size) {
                float bw = obj->puppet->max_pos[0] - obj->puppet->min_pos[0];
                float bh = obj->puppet->max_pos[1] - obj->puppet->min_pos[1];
                if (bw > 1.0f && bh > 1.0f) {
                    obj->size[0] = bw;
                    obj->size[1] = bh;
                    has_explicit_size = true;
                }
            }
        }
    }

    /* Load SIMD SoA Particle System if referenced */
    if (part_path[0] != '\0') {
        obj->particle_sys = pkg_load_particle_cpu(scene->pkg, part_path);
        if (obj->particle_sys) {
            apply_particle_instance_override(obj->particle_sys, jobj_get(onode, "instanceoverride"),
                                             &scene->project);
            if (mat_path[0] == '\0' && obj->particle_sys->material_path[0] != '\0') {
                snprintf(mat_path, sizeof(mat_path), "%s", obj->particle_sys->material_path);
            }
        }
    }

    if (obj->puppet && obj->puppet->bone_count > 0u) {
        pass_set_combo(&obj->base_pass, "SKINNING", 1);
    }

    if (mat_path[0] != '\0' && !obj->passthrough) {
        if (load_material_pass_from_file(&obj->base_pass, scene->pkg, mat_path, &scene->project)) {
            obj->has_base_pass = true;
            parse_pass_json_overrides(&obj->base_pass, onode, &scene->project);
            if (obj->color_blend_mode > 0) {
                pass_set_combo(&obj->base_pass, "BLENDMODE", obj->color_blend_mode);
            }
            finalize_material_pass_cpu(&obj->base_pass, scene->pkg);
            if (pass_uses_audio_spectrum(&obj->base_pass)) {
                scene->uses_audio = true;
            }
            if (!has_explicit_size && !obj->fullscreen && obj->base_pass.textures[0]) {
                if (obj->base_pass.textures[0]->image_width > 0 &&
                    obj->base_pass.textures[0]->image_height > 0) {
                    obj->size[0] = (float)obj->base_pass.textures[0]->image_width;
                    obj->size[1] = (float)obj->base_pass.textures[0]->image_height;
                }
            }
        }
    }

    if (obj->particle_sys && !obj->has_base_pass) {
        obj->has_base_pass = true;
        obj->base_pass.blending = WE_BLEND_ADDITIVE;
        finalize_material_pass_cpu(&obj->base_pass, scene->pkg);
    }

    if (obj->puppet && !obj->has_base_pass && !obj->passthrough) {
        obj->has_base_pass = true;
        obj->base_pass.blending = WE_BLEND_NORMAL;
        finalize_material_pass_cpu(&obj->base_pass, scene->pkg);
    }

    /* Attach SceneScript VMs for object properties and material uniforms */
    attach_object_script(scene, obj, WE_SCRIPT_PROP_ORIGIN, NULL, jobj_get(onode, "origin"), NULL);
    attach_object_script(scene, obj, WE_SCRIPT_PROP_SCALE, NULL, jobj_get(onode, "scale"), NULL);
    attach_object_script(scene, obj, WE_SCRIPT_PROP_ANGLES, NULL, jobj_get(onode, "angles"), NULL);
    attach_object_script(scene, obj, WE_SCRIPT_PROP_ALPHA, NULL, jobj_get(onode, "alpha"), NULL);
    attach_object_script(scene, obj, WE_SCRIPT_PROP_COLOR, NULL, jobj_get(onode, "color"), NULL);
    attach_object_script(scene, obj, WE_SCRIPT_PROP_VISIBLE, NULL, jobj_get(onode, "visible"),
                         NULL);
    const we_jval_t *top_script = jobj_get(onode, "script");
    if (top_script) {
        attach_object_script(scene, obj, WE_SCRIPT_PROP_ORIGIN, NULL, top_script,
                             jval_str(top_script, NULL));
    }
    for (uint32_t c = 0; c < obj->base_pass.const_val_count; c++) {
        we_const_val_t *cv = &obj->base_pass.const_vals[c];
        if (cv->num_components == 0) {
            for (uint32_t u = 0; u < obj->base_pass.prep.meta.uniform_count; u++) {
                const we_shader_uniform_meta_t *um = &obj->base_pass.prep.meta.uniforms[u];
                if ((um->material_key[0] != '\0' && strcasecmp(cv->name, um->material_key) == 0) ||
                    strcasecmp(cv->name, um->name) == 0) {
                    if (um->num_components > 0) {
                        cv->num_components = um->num_components;
                        if (um->has_default_val) {
                            memcpy(cv->val, um->default_val, um->num_components * sizeof(float));
                        }
                    }
                    break;
                }
            }
        }
        if (cv->script_src && cv->script_src[0] != '\0') {
            if (cv->num_components == 0) {
                cv->num_components = 1u;
            }
            attach_object_script(scene, obj, WE_SCRIPT_PROP_UNIFORM, cv->name, NULL,
                                 cv->script_src);
        }
    }

    /* Parse object effects[] */
    const we_jval_t *effs = jobj_get(onode, "effects");
    if (effs && effs->type == JVAL_ARR && effs->arr.count > 0) {
        obj->effects = calloc(effs->arr.count, sizeof(we_effect_t));
        if (obj->effects) {
            for (size_t e = 0; e < effs->arr.count; e++) {
                const we_jval_t *enode = effs->arr.items[e];
                if (!enode || enode->type != JVAL_OBJ) {
                    continue;
                }
                const char *efile = jval_str(jobj_get(enode, "file"), NULL);
                if (!efile || efile[0] == '\0') {
                    continue;
                }

                we_effect_t *eff = &obj->effects[obj->effect_count];
                snprintf(eff->file_path, sizeof(eff->file_path), "%s", efile);
                snprintf(eff->name, sizeof(eff->name), "%s",
                         jval_str(jobj_get(enode, "name"), efile));
                eff->visible =
                    jval_bool_resolved(jobj_get(enode, "visible"), &scene->project, true,
                                       eff->visible_user_prop, sizeof(eff->visible_user_prop));

                we_jval_t *eroot = pkg_load_json(scene->pkg, efile);
                if (eroot) {
                    snprintf(eff->replacement_key, sizeof(eff->replacement_key), "%s",
                             jval_str(jobj_get(eroot, "replacementkey"), ""));

                    const we_jval_t *fbos = jobj_get(eroot, "fbos");
                    if (fbos && fbos->type == JVAL_ARR) {
                        for (size_t f = 0;
                             f < fbos->arr.count && eff->fbo_count < WE_MAX_EFFECT_FBOS; f++) {
                            const we_jval_t *fn = fbos->arr.items[f];
                            const char *fname = jval_str(jobj_get(fn, "name"), NULL);
                            if (fname) {
                                we_rt_fbo_t *rt = &eff->fbos[eff->fbo_count++];
                                snprintf(rt->name, sizeof(rt->name), "%s", fname);
                                rt->scale = (uint32_t)jval_float(jobj_get(fn, "scale"),
                                                                 &scene->project, 1.0f, NULL, 0);
                                if (rt->scale == 0) {
                                    rt->scale = 1;
                                }
                            }
                        }
                    }

                    const we_jval_t *epasses = jobj_get(eroot, "passes");
                    const we_jval_t *opasses = jobj_get(enode, "passes");
                    if (epasses && epasses->type == JVAL_ARR) {
                        for (size_t p = 0;
                             p < epasses->arr.count && eff->pass_count < WE_MAX_EFFECT_PASSES;
                             p++) {
                            const we_jval_t *epn = epasses->arr.items[p];
                            we_material_pass_t *mp = &eff->passes[eff->pass_count++];
                            memset(mp, 0, sizeof(*mp));
                            mp->blending = WE_BLEND_DISABLED;

                            snprintf(mp->command, sizeof(mp->command), "%s",
                                     jval_str(jobj_get(epn, "command"), ""));
                            snprintf(mp->target_fbo, sizeof(mp->target_fbo), "%s",
                                     jval_str(jobj_get(epn, "target"), ""));

                            const we_jval_t *binds = jobj_get(epn, "bind");
                            if (binds && binds->type == JVAL_ARR) {
                                for (size_t b = 0;
                                     b < binds->arr.count && mp->bind_count < WE_MAX_PASS_TEXTURES;
                                     b++) {
                                    const we_jval_t *bn = binds->arr.items[b];
                                    const char *bname = jval_str(jobj_get(bn, "name"), NULL);
                                    int32_t bidx = (int32_t)jval_float(
                                        jobj_get(bn, "index"), &scene->project, 0.0f, NULL, 0);
                                    if (bname && bidx >= 0 &&
                                        bidx < (int32_t)WE_MAX_PASS_TEXTURES) {
                                        snprintf(mp->binds[mp->bind_count].target_name,
                                                 sizeof(mp->binds[0].target_name), "%s", bname);
                                        mp->binds[mp->bind_count].slot = bidx;
                                        mp->bind_count++;
                                    }
                                }
                            }

                            const char *emat = jval_str(jobj_get(epn, "material"), NULL);
                            if (emat) {
                                load_material_pass_from_file(mp, scene->pkg, emat, &scene->project);
                            }
                            parse_pass_json_overrides(mp, epn, &scene->project);
                            if (opasses && opasses->type == JVAL_ARR && p < opasses->arr.count) {
                                parse_pass_json_overrides(mp, opasses->arr.items[p],
                                                          &scene->project);
                            }
                            if (strcasecmp(mp->command, "copy") != 0 &&
                                strcasecmp(mp->command, "swap") != 0) {
                                finalize_material_pass_cpu(mp, scene->pkg);
                                if (pass_uses_audio_spectrum(mp)) {
                                    scene->uses_audio = true;
                                }
                            }
                        }
                    }
                    jval_free(eroot);
                }
                obj->effect_count++;
            }
        }
    }
}

we_scene_t *we_scene_load_cpu(const char *workshop_dir, const char *prop_overrides)
{
    if (!workshop_dir || workshop_dir[0] == '\0') {
        return NULL;
    }

    char resolved_dir[4096];
    if (!we_workshop_resolve_dir(workshop_dir, resolved_dir, sizeof(resolved_dir))) {
        snprintf(resolved_dir, sizeof(resolved_dir), "%s", workshop_dir);
    }

    we_scene_t *scene = calloc(1, sizeof(we_scene_t));
    if (!scene) {
        return NULL;
    }

    if (!we_project_load_dir(resolved_dir, &scene->project)) {
        snprintf(scene->project.workshop_dir, sizeof(scene->project.workshop_dir), "%s",
                 resolved_dir);
        snprintf(scene->project.type, sizeof(scene->project.type), "scene");
        snprintf(scene->project.file, sizeof(scene->project.file), "scene.json");
    }
    if (prop_overrides && prop_overrides[0] != '\0') {
        we_project_apply_overrides_str(&scene->project, prop_overrides);
    }

    scene->pkg = we_pkg_open_dir(resolved_dir);
    if (!scene->pkg) {
        free(scene);
        return NULL;
    }

    scene->ortho_width = 1920.0f;
    scene->ortho_height = 1080.0f;
    scene->clear_enabled = true;
    scene->camera_up[1] = 1.0f;

    we_jval_t *sroot = pkg_load_json(scene->pkg, scene->project.file);
    if (!sroot && strcasecmp(scene->project.file, "scene.json") != 0) {
        sroot = pkg_load_json(scene->pkg, "scene.json");
    }
    if (!sroot) {
        sroot = pkg_load_json(scene->pkg, "gifscene.json");
    }

    if (sroot && sroot->type == JVAL_OBJ) {
        const we_jval_t *gen = jobj_get(sroot, "general");
        if (gen && gen->type == JVAL_OBJ) {
            const we_jval_t *ortho = jobj_get(gen, "orthogonalprojection");
            if (ortho && ortho->type == JVAL_OBJ) {
                scene->ortho_width =
                    jval_float(jobj_get(ortho, "width"), &scene->project, 1920.0f, NULL, 0);
                scene->ortho_height =
                    jval_float(jobj_get(ortho, "height"), &scene->project, 1080.0f, NULL, 0);
                if (scene->ortho_width <= 0.0f) {
                    scene->ortho_width = 1920.0f;
                }
                if (scene->ortho_height <= 0.0f) {
                    scene->ortho_height = 1080.0f;
                }
            }
            jval_floats(jobj_get(gen, "clearcolor"), &scene->project, scene->clear_color, 3, NULL,
                        0);
            scene->clear_enabled =
                jval_bool_resolved(jobj_get(gen, "clearenabled"), &scene->project, true, NULL, 0);
            scene->camera_parallax = jval_bool_resolved(jobj_get(gen, "cameraparallax"),
                                                        &scene->project, false, NULL, 0);
            scene->camera_parallax_amount =
                jval_float(jobj_get(gen, "cameraparallaxamount"), &scene->project, 0.5f, NULL, 0);
            scene->camera_parallax_delay =
                jval_float(jobj_get(gen, "cameraparallaxdelay"), &scene->project, 0.1f, NULL, 0);
            scene->camera_parallax_mouse_influence = jval_float(
                jobj_get(gen, "cameraparallaxmouseinfluence"), &scene->project, 1.0f, NULL, 0);
            scene->camera_shake =
                jval_bool_resolved(jobj_get(gen, "camerashake"), &scene->project, false, NULL, 0);
            scene->camera_shake_amp =
                jval_float(jobj_get(gen, "camerashakeamplitude"), &scene->project, 0.5f, NULL, 0);
            scene->camera_shake_roughness =
                jval_float(jobj_get(gen, "camerashakeroughness"), &scene->project, 1.0f, NULL, 0);
            scene->camera_shake_speed =
                jval_float(jobj_get(gen, "camerashakespeed"), &scene->project, 1.0f, NULL, 0);
        }

        scene->camera_eye[0] = scene->ortho_width * 0.5f;
        scene->camera_eye[1] = scene->ortho_height * 0.5f;
        scene->camera_eye[2] = -1000.0f;
        scene->camera_center[0] = scene->ortho_width * 0.5f;
        scene->camera_center[1] = scene->ortho_height * 0.5f;
        scene->camera_center[2] = 0.0f;

        const we_jval_t *cam = jobj_get(sroot, "camera");
        if (cam && cam->type == JVAL_OBJ) {
            jval_floats(jobj_get(cam, "eye"), &scene->project, scene->camera_eye, 3, NULL, 0);
            jval_floats(jobj_get(cam, "center"), &scene->project, scene->camera_center, 3, NULL, 0);
            jval_floats(jobj_get(cam, "up"), &scene->project, scene->camera_up, 3, NULL, 0);
        }

        const we_jval_t *objs = jobj_get(sroot, "objects");
        if (objs && objs->type == JVAL_ARR && objs->arr.count > 0) {
            scene->objects = calloc(objs->arr.count, sizeof(we_scene_object_t));
            if (scene->objects) {
                for (size_t i = 0; i < objs->arr.count; i++) {
                    if (objs->arr.items[i] && objs->arr.items[i]->type == JVAL_OBJ) {
                        parse_scene_object(scene, objs->arr.items[i],
                                           &scene->objects[scene->object_count++]);
                    }
                }
                init_scene_scripts(scene);
            }
        }
        jval_free(sroot);
    }

    return scene;
}

/* =========================================================================
 * OpenGL ES 3.1 Initialization, Matrix Math & Scene Graph Rendering
 * ========================================================================= */

static void mat4_identity(float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
}

static void mat4_mul(const float a[16], const float b[16], float out[16])
{
    float r[16];
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            r[col * 4 + row] = a[0 * 4 + row] * b[col * 4 + 0] + a[1 * 4 + row] * b[col * 4 + 1] +
                               a[2 * 4 + row] * b[col * 4 + 2] + a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
    memcpy(out, r, sizeof(r));
}

/* Top-down orthographic projection:
 * World (0, ortho_h) -> Clip (-1, -1) (FBO row 0, top of screen)
 * World (ortho_w, 0) -> Clip (+1, +1) (FBO row H-1, bottom of screen) */
static void mat4_ortho_topdown(float ortho_w, float ortho_h, float offset_x, float offset_y,
                               float out[16])
{
    mat4_identity(out);
    float w = ortho_w > 1.0f ? ortho_w : 1920.0f;
    float h = ortho_h > 1.0f ? ortho_h : 1080.0f;
    out[0] = 2.0f / w;
    out[5] = -2.0f / h;
    out[10] = -1.0f / 2000.0f;
    out[12] = -1.0f - (2.0f * offset_x / w);
    out[13] = 1.0f + (2.0f * offset_y / h);
    out[14] = 0.0f;
    out[15] = 1.0f;
}

static void compute_node_local_matrix(const we_scene_object_t *node, float parallax_dx,
                                      float parallax_dy, float out[16])
{
    float ox = node->origin[0] + parallax_dx * node->parallax_depth[0];
    float oy = node->origin[1] + parallax_dy * node->parallax_depth[1];
    float oz = node->origin[2];

    float sx = node->scale[0];
    float sy = node->scale[1];
    float sz = node->scale[2];

    float rz = node->angles[2];
    float cz = cosf(rz);
    float snz = sinf(rz);

    mat4_identity(out);
    out[0] = cz * sx;
    out[1] = snz * sx;
    out[4] = -snz * sy;
    out[5] = cz * sy;
    out[10] = sz;
    out[12] = ox;
    out[13] = oy;
    out[14] = oz;
}

static void compute_object_node_world_matrix(const we_scene_t *scene, const we_scene_object_t *obj,
                                             float parallax_dx, float parallax_dy, float out[16])
{
    const we_scene_object_t *chain[16];
    int depth = 0;
    const we_scene_object_t *cur = obj;
    while (cur && depth < 16) {
        chain[depth++] = cur;
        if (cur->parent_id < 0) {
            break;
        }
        const we_scene_object_t *parent = NULL;
        for (uint32_t i = 0; i < scene->object_count; i++) {
            if (scene->objects[i].id == cur->parent_id && &scene->objects[i] != cur) {
                parent = &scene->objects[i];
                break;
            }
        }
        bool cycle = false;
        for (int k = 0; k < depth && parent; k++) {
            if (chain[k] == parent) {
                cycle = true;
                break;
            }
        }
        if (!parent || cycle) {
            break;
        }
        cur = parent;
    }

    mat4_identity(out);
    for (int d = depth - 1; d >= 0; d--) {
        float local[16];
        compute_node_local_matrix(chain[d], parallax_dx, parallax_dy, local);
        mat4_mul(out, local, out);
    }
}

/* Computes object model matrix with Top-Down Orientation Invariant and parent hierarchy:
 * M_model = M_parent_chain * M_local_node * S(size.x * 0.5f, -size.y * 0.5f, 1.0f) */
static void compute_object_model_matrix(const we_scene_t *scene, const we_scene_object_t *obj,
                                        float parallax_dx, float parallax_dy, float out[16])
{
    if (obj->fullscreen) {
        mat4_identity(out);
        out[0] = scene->ortho_width * 0.5f;
        out[5] = -scene->ortho_height * 0.5f;
        out[10] = obj->scale[2];
        out[12] = scene->ortho_width * 0.5f;
        out[13] = scene->ortho_height * 0.5f;
        out[14] = obj->origin[2];
        return;
    }

    float world_node[16];
    compute_object_node_world_matrix(scene, obj, parallax_dx, parallax_dy, world_node);

    float quad_scale[16];
    mat4_identity(quad_scale);
    quad_scale[0] = obj->size[0] * 0.5f;
    quad_scale[5] = -obj->size[1] * 0.5f;
    mat4_mul(world_node, quad_scale, out);
}

static uint32_t create_solid_tex(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint8_t px[4] = {r, g, b, a};
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return (uint32_t)tex;
}

static uint32_t create_noise_tex_64(void)
{
    uint8_t px[64 * 64 * 4];
    uint32_t st = 0x9E3779B9u;
    for (size_t i = 0; i < sizeof(px); i++) {
        st ^= st << 13;
        st ^= st >> 17;
        st ^= st << 5;
        px[i] = (uint8_t)(st & 0xFFu);
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return (uint32_t)tex;
}

static void upload_pass_gl(we_material_pass_t *pass)
{
    if (!pass) {
        return;
    }
    for (uint32_t s = 0; s < WE_MAX_PASS_TEXTURES; s++) {
        if (pass->textures[s] && pass->textures[s]->gl_tex == 0) {
            we_tex_upload_gl(pass->textures[s]);
        }
    }
    if (pass->prep.vert_glsl && pass->prep.frag_glsl && pass->gl_prog == 0) {
        pass->gl_prog = we_shader_compile_program(pass->prep.vert_glsl, pass->prep.frag_glsl,
                                                  pass->shader_name);
        if (pass->gl_prog == 0 && strcasecmp(pass->shader_name, "genericimage2") != 0) {
            we_shader_preprocessed_t fb;
            if (we_shader_preprocess_pair(NULL, NULL, NULL, pass->combos, pass->combo_count,
                                          pass->bound_tex_mask, false, &fb)) {
                pass->gl_prog =
                    we_shader_compile_program(fb.vert_glsl, fb.frag_glsl, "genericimage2_fallback");
                we_shader_preprocessed_free(&fb);
            }
        }
    }
}

static EGLDisplay s_we_shared_dpy = EGL_NO_DISPLAY;
static EGLContext s_we_shared_ctx = EGL_NO_CONTEXT;
static uint32_t s_we_shared_refcount = 0;

bool we_scene_init_gl(we_scene_t *scene, dmabuf_context_t *dmabuf_ctx)
{
    if (!scene) {
        return false;
    }
    if (scene->gl_initialized) {
        return true;
    }

    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    bool owns_egl = false;

    if (s_we_shared_refcount > 0 && s_we_shared_dpy != EGL_NO_DISPLAY &&
        s_we_shared_ctx != EGL_NO_CONTEXT) {
        if (eglMakeCurrent(s_we_shared_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s_we_shared_ctx)) {
            dpy = s_we_shared_dpy;
            ctx = s_we_shared_ctx;
            owns_egl = true;
            s_we_shared_refcount++;
        }
    } else if (!dmabuf_ctx) {
        dpy = eglGetCurrentDisplay();
        ctx = eglGetCurrentContext();
    }

    if (dpy == EGL_NO_DISPLAY || ctx == EGL_NO_CONTEXT) {
        if (!dmabuf_ctx || !dmabuf_ctx->gbm || !dmabuf_ctx->available) {
            return false;
        }
        PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (get_platform_display) {
            dpy = get_platform_display(EGL_PLATFORM_GBM_KHR, dmabuf_ctx->gbm, NULL);
        }
        if (dpy == EGL_NO_DISPLAY) {
            dpy = eglGetDisplay((EGLNativeDisplayType)dmabuf_ctx->gbm);
        }
        if (dpy == EGL_NO_DISPLAY) {
            return false;
        }
        EGLint major = 0, minor = 0;
        if (!eglInitialize(dpy, &major, &minor) || !eglBindAPI(EGL_OPENGL_ES_API)) {
            eglTerminate(dpy);
            return false;
        }
        static const EGLint ctx_attribs[] = {
            EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE,
        };
        ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attribs);
        if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
            if (ctx != EGL_NO_CONTEXT) {
                eglDestroyContext(dpy, ctx);
            }
            eglTerminate(dpy);
            return false;
        }
        owns_egl = true;
        s_we_shared_dpy = dpy;
        s_we_shared_ctx = ctx;
        s_we_shared_refcount = 1;
    }

    scene->egl_display = dpy;
    scene->egl_context = ctx;
    scene->owns_egl = owns_egl;

    /* Unit quad VAO/VBO (Top-Down Orientation Invariant:
     * v0: (-1,-1,0) uv(0,0), v1: (+1,-1,0) uv(1,0), v2: (+1,+1,0) uv(1,1), v3: (-1,+1,0) uv(0,1))
     */
    static const float quad_verts[] = {
        -1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f,  -1.0f, 0.0f, 1.0f, 0.0f,
        1.0f,  1.0f,  0.0f, 1.0f, 1.0f, -1.0f, 1.0f,  0.0f, 0.0f, 1.0f,
    };

    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_verts), quad_verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (const void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (const void *)(3 * sizeof(float)));
    glBindVertexArray(0);

    scene->quad_vao = vao;
    scene->quad_vbo = vbo;

    /* Presentation shader: blits scene_fbo_tex into target_bo with forced opaque alpha 1.0 */
    static const char present_vs[] = "#version 310 es\n"
                                     "precision highp float;\n"
                                     "layout(location = 0) in vec3 a_Position;\n"
                                     "layout(location = 1) in vec2 a_TexCoord;\n"
                                     "out vec2 v_uv;\n"
                                     "void main() {\n"
                                     "    gl_Position = vec4(a_Position, 1.0);\n"
                                     "    v_uv = a_TexCoord;\n"
                                     "}\n";
    static const char present_fs[] = "#version 310 es\n"
                                     "precision highp float;\n"
                                     "in vec2 v_uv;\n"
                                     "uniform sampler2D u_tex;\n"
                                     "out vec4 fragColor;\n"
                                     "void main() {\n"
                                     "    vec4 c = texture(u_tex, v_uv);\n"
                                     "    fragColor = vec4(c.rgb, 1.0);\n"
                                     "}\n";
    scene->prog_present = we_shader_compile_program(present_vs, present_fs, "we_present");

    /* Default composite shader: draws an effect-processed RGBA texture into scene_fbo */
    static const char default_vs[] =
        "#version 310 es\n"
        "precision highp float;\n"
        "layout(location = 0) in vec3 a_Position;\n"
        "layout(location = 1) in vec2 a_TexCoord;\n"
        "uniform mat4 g_ModelViewProjectionMatrix;\n"
        "out vec2 v_uv;\n"
        "void main() {\n"
        "    gl_Position = g_ModelViewProjectionMatrix * vec4(a_Position, 1.0);\n"
        "    v_uv = a_TexCoord;\n"
        "}\n";
    static const char default_fs[] = "#version 310 es\n"
                                     "precision highp float;\n"
                                     "in vec2 v_uv;\n"
                                     "uniform sampler2D g_Texture0;\n"
                                     "out vec4 fragColor;\n"
                                     "void main() {\n"
                                     "    fragColor = texture(g_Texture0, v_uv);\n"
                                     "}\n";
    scene->default_program = we_shader_compile_program(default_vs, default_fs, "we_default");

    /* Batched SIMD Particle shader: supports per-vertex color/alpha at location = 2 */
    static const char particle_vs[] =
        "#version 310 es\n"
        "precision highp float;\n"
        "layout(location = 0) in vec3 a_Position;\n"
        "layout(location = 1) in vec2 a_TexCoord;\n"
        "layout(location = 2) in vec4 a_Color;\n"
        "uniform mat4 g_ModelViewProjectionMatrix;\n"
        "uniform vec2 g_Texture0Translation;\n"
        "uniform vec4 g_Texture0Rotation;\n"
        "out vec2 v_TexCoord;\n"
        "out vec4 v_Color;\n"
        "void main() {\n"
        "    gl_Position = g_ModelViewProjectionMatrix * vec4(a_Position, 1.0);\n"
        "    v_TexCoord = g_Texture0Translation + a_TexCoord.x * g_Texture0Rotation.xy + "
        "a_TexCoord.y * g_Texture0Rotation.zw;\n"
        "    v_Color = a_Color;\n"
        "}\n";
    static const char particle_fs[] = "#version 310 es\n"
                                      "precision highp float;\n"
                                      "in vec2 v_TexCoord;\n"
                                      "in vec4 v_Color;\n"
                                      "uniform sampler2D g_Texture0;\n"
                                      "uniform vec4 g_Color4;\n"
                                      "out vec4 fragColor;\n"
                                      "void main() {\n"
                                      "    vec4 tex = texture(g_Texture0, v_TexCoord);\n"
                                      "    fragColor = tex * v_Color * g_Color4;\n"
                                      "}\n";
    scene->particle_program = we_shader_compile_program(particle_vs, particle_fs, "we_particle");

    GLuint tfbo = 0;
    glGenFramebuffers(1, &tfbo);
    scene->target_fbo = tfbo;

    /* Built-in utility textures */
    scene->tex_util_white = create_solid_tex(255, 255, 255, 255);
    scene->tex_util_black = create_solid_tex(0, 0, 0, 255);
    scene->tex_util_flatnormal = create_solid_tex(128, 128, 255, 128);
    scene->tex_util_noise = create_noise_tex_64();
    if (scene->album_art_rgba && scene->album_art_w > 0 && scene->album_art_h > 0) {
        GLuint atex = 0;
        glGenTextures(1, &atex);
        glBindTexture(GL_TEXTURE_2D, atex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)scene->album_art_w,
                     (GLsizei)scene->album_art_h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     scene->album_art_rgba);
        scene->tex_album_art = (uint32_t)atex;
    }

    /* Upload all scene object textures, puppet meshes, particle buffers, and compile shaders */
    for (uint32_t i = 0; i < scene->object_count; i++) {
        we_scene_object_t *obj = &scene->objects[i];
        if (obj->has_base_pass) {
            upload_pass_gl(&obj->base_pass);
        }
        if (obj->puppet) {
            we_puppet_upload_gl(obj->puppet);
        }
        if (obj->particle_sys) {
            we_particle_upload_gl(obj->particle_sys);
        }
        for (uint32_t e = 0; e < obj->effect_count; e++) {
            for (uint32_t p = 0; p < obj->effects[e].pass_count; p++) {
                upload_pass_gl(&obj->effects[e].passes[p]);
            }
        }
    }

    scene->gl_initialized = true;
    return true;
}

we_scene_t *we_scene_create(dmabuf_context_t *dmabuf_ctx, const char *workshop_dir,
                            const char *prop_overrides)
{
    we_scene_t *scene = we_scene_load_cpu(workshop_dir, prop_overrides);
    if (!scene) {
        return NULL;
    }
    we_scene_init_gl(scene, dmabuf_ctx);
    return scene;
}

static void ensure_fbo_pair(uint32_t *inout_fbo, uint32_t *inout_tex, uint32_t *inout_w,
                            uint32_t *inout_h, uint32_t w, uint32_t h)
{
    if (*inout_fbo && *inout_tex && *inout_w == w && *inout_h == h) {
        return;
    }
    if (*inout_fbo) {
        GLuint f = *inout_fbo;
        glDeleteFramebuffers(1, &f);
    }
    if (*inout_tex) {
        GLuint t = *inout_tex;
        glDeleteTextures(1, &t);
    }
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 NULL);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    *inout_fbo = fbo;
    *inout_tex = tex;
    *inout_w = w;
    *inout_h = h;
}

static we_rt_fbo_t *get_or_create_scene_rt_fbo(we_scene_t *scene, const char *name, uint32_t w,
                                               uint32_t h)
{
    if (!scene || !name || name[0] == '\0' || w == 0 || h == 0) {
        return NULL;
    }
    for (uint32_t i = 0; i < scene->rt_fbo_count; i++) {
        if (strcasecmp(scene->rt_fbos[i].name, name) == 0) {
            ensure_fbo_pair(&scene->rt_fbos[i].gl_fbo, &scene->rt_fbos[i].gl_tex,
                            &scene->rt_fbos[i].width, &scene->rt_fbos[i].height, w, h);
            return &scene->rt_fbos[i];
        }
    }
    if (scene->rt_fbo_count >= WE_MAX_SCENE_RT_FBOS) {
        return NULL;
    }
    we_rt_fbo_t *rt = &scene->rt_fbos[scene->rt_fbo_count++];
    memset(rt, 0, sizeof(*rt));
    snprintf(rt->name, sizeof(rt->name), "%s", name);
    rt->scale = 1;
    ensure_fbo_pair(&rt->gl_fbo, &rt->gl_tex, &rt->width, &rt->height, w, h);
    return rt;
}

static uint32_t resolve_named_rt_tex(we_scene_t *scene, we_effect_t *eff, const char *name,
                                     uint32_t prev_tex, float out_res[4])
{
    if (!name || name[0] == '\0' || strcasecmp(name, "previous") == 0) {
        return prev_tex;
    }
    if (strcasecmp(name, "_rt_FullFrameBuffer") == 0 ||
        strcasecmp(name, "_rt_fullframebuffer") == 0) {
        out_res[0] = (float)scene->scene_fbo_w;
        out_res[1] = (float)scene->scene_fbo_h;
        out_res[2] = out_res[0];
        out_res[3] = out_res[1];
        return scene->full_fb_tex ? scene->full_fb_tex : scene->scene_fbo_tex;
    }
    if (strncasecmp(name, "util/white", 10) == 0 || strcasecmp(name, "_rt_default_white") == 0) {
        out_res[0] = out_res[1] = out_res[2] = out_res[3] = 1.0f;
        return scene->tex_util_white;
    }
    if (strncasecmp(name, "util/black", 10) == 0) {
        out_res[0] = out_res[1] = out_res[2] = out_res[3] = 1.0f;
        return scene->tex_util_black;
    }
    if (strncasecmp(name, "util/flatnormal", 15) == 0) {
        out_res[0] = out_res[1] = out_res[2] = out_res[3] = 1.0f;
        return scene->tex_util_flatnormal;
    }
    if (strncasecmp(name, "util/noise", 10) == 0 || strncasecmp(name, "util/clouds", 11) == 0) {
        out_res[0] = out_res[1] = out_res[2] = out_res[3] = 64.0f;
        return scene->tex_util_noise;
    }
    if (strcasecmp(name, "_rt_AlbumCover") == 0 || strcasecmp(name, "u_album_art") == 0) {
        float aw = scene->album_art_w > 0 ? (float)scene->album_art_w : 1.0f;
        float ah = scene->album_art_h > 0 ? (float)scene->album_art_h : 1.0f;
        out_res[0] = aw;
        out_res[1] = ah;
        out_res[2] = aw;
        out_res[3] = ah;
        return scene->tex_album_art ? scene->tex_album_art : scene->tex_util_black;
    }
    if (eff) {
        for (uint32_t i = 0; i < eff->fbo_count; i++) {
            if (strcasecmp(eff->fbos[i].name, name) == 0 && eff->fbos[i].gl_tex) {
                out_res[0] = (float)eff->fbos[i].width;
                out_res[1] = (float)eff->fbos[i].height;
                out_res[2] = out_res[0];
                out_res[3] = out_res[1];
                return eff->fbos[i].gl_tex;
            }
        }
    }
    for (uint32_t i = 0; i < scene->rt_fbo_count; i++) {
        if (strcasecmp(scene->rt_fbos[i].name, name) == 0 && scene->rt_fbos[i].gl_tex) {
            out_res[0] = (float)scene->rt_fbos[i].width;
            out_res[1] = (float)scene->rt_fbos[i].height;
            out_res[2] = out_res[0];
            out_res[3] = out_res[1];
            return scene->rt_fbos[i].gl_tex;
        }
    }
    return prev_tex;
}

static void bind_pass_uniforms(we_scene_t *scene, we_scene_object_t *obj, we_effect_t *eff,
                               we_material_pass_t *pass, const float model[16], const float vp[16],
                               const float mvp[16], uint32_t prev_tex, uint32_t rt_w, uint32_t rt_h,
                               uint32_t target_w, uint32_t target_h,
                               const waywal_shader_frame_params_t *params)
{
    GLuint prog = pass->gl_prog;
    glUseProgram(prog);

    float time_sec = params ? params->time_sec : 0.0f;
    float dt_sec =
        (params && params->time_delta_sec > 0.0f) ? params->time_delta_sec : (1.0f / 60.0f);

    GLint loc;
    if ((loc = glGetUniformLocation(prog, "g_Time")) >= 0) {
        glUniform1f(loc, time_sec);
    }
    if ((loc = glGetUniformLocation(prog, "g_Daytime")) >= 0) {
        float daytime = params ? (params->date[3] / 86400.0f) : 0.5f;
        glUniform1f(loc, daytime);
    }
    if ((loc = glGetUniformLocation(prog, "g_Frametime")) >= 0) {
        glUniform1f(loc, dt_sec);
    }
    if ((loc = glGetUniformLocation(prog, "u_media_playing")) >= 0) {
        float mplay =
            (params && params->media_playing > 0.0f) ? params->media_playing : scene->media_playing;
        glUniform1f(loc, mplay);
    }

    uint32_t norm_w = target_w > 0 ? target_w : rt_w;
    uint32_t norm_h = target_h > 0 ? target_h : rt_h;
    float ptr[2] = {0.5f, 0.5f};
    if (params && norm_w > 0 && norm_h > 0 &&
        (params->mouse[0] > 0.0f || params->mouse[1] > 0.0f)) {
        ptr[0] = params->mouse[0] / (float)norm_w;
        ptr[1] = params->mouse[1] / (float)norm_h;
    }
    if ((loc = glGetUniformLocation(prog, "g_PointerPosition")) >= 0) {
        glUniform2f(loc, ptr[0], ptr[1]);
    }
    if ((loc = glGetUniformLocation(prog, "g_PointerPositionLast")) >= 0) {
        glUniform2f(loc, scene->last_pointer[0], scene->last_pointer[1]);
    }

    float fw = rt_w > 0 ? (float)rt_w : 1920.0f;
    float fh = rt_h > 0 ? (float)rt_h : 1080.0f;
    if ((loc = glGetUniformLocation(prog, "g_TexelSize")) >= 0) {
        glUniform2f(loc, 1.0f / fw, 1.0f / fh);
    }
    if ((loc = glGetUniformLocation(prog, "g_TexelSizeHalf")) >= 0) {
        glUniform2f(loc, 0.5f / fw, 0.5f / fh);
    }
    if ((loc = glGetUniformLocation(prog, "g_Screen")) >= 0) {
        glUniform3f(loc, fw, fh, fw / fh);
    }

    if ((loc = glGetUniformLocation(prog, "g_ViewUp")) >= 0) {
        glUniform3f(loc, 0.0f, 1.0f, 0.0f);
    }
    if ((loc = glGetUniformLocation(prog, "g_ViewRight")) >= 0) {
        glUniform3f(loc, 1.0f, 0.0f, 0.0f);
    }
    if ((loc = glGetUniformLocation(prog, "g_OrientationUp")) >= 0) {
        glUniform3f(loc, 0.0f, 1.0f, 0.0f);
    }
    if ((loc = glGetUniformLocation(prog, "g_OrientationRight")) >= 0) {
        glUniform3f(loc, 1.0f, 0.0f, 0.0f);
    }
    if ((loc = glGetUniformLocation(prog, "g_OrientationForward")) >= 0) {
        glUniform3f(loc, 0.0f, 0.0f, 1.0f);
    }
    if ((loc = glGetUniformLocation(prog, "g_EyePosition")) >= 0) {
        glUniform3fv(loc, 1, scene->camera_eye);
    }

    float ident[16];
    mat4_identity(ident);
    if ((loc = glGetUniformLocation(prog, "g_ModelMatrix")) >= 0) {
        glUniformMatrix4fv(loc, 1, GL_FALSE, model ? model : ident);
    }
    if ((loc = glGetUniformLocation(prog, "g_ViewProjectionMatrix")) >= 0) {
        glUniformMatrix4fv(loc, 1, GL_FALSE, vp ? vp : ident);
    }
    if ((loc = glGetUniformLocation(prog, "g_ModelViewProjectionMatrix")) >= 0) {
        glUniformMatrix4fv(loc, 1, GL_FALSE, mvp ? mvp : ident);
    }
    if ((loc = glGetUniformLocation(prog, "g_ModelViewProjectionMatrixInverse")) >= 0) {
        glUniformMatrix4fv(loc, 1, GL_FALSE, ident);
    }
    if ((loc = glGetUniformLocation(prog, "g_EffectTextureProjectionMatrix")) >= 0) {
        glUniformMatrix4fv(loc, 1, GL_FALSE, ident);
    }
    if ((loc = glGetUniformLocation(prog, "g_EffectTextureProjectionMatrixInverse")) >= 0) {
        glUniformMatrix4fv(loc, 1, GL_FALSE, ident);
    }

    /* Object color & alpha uniforms */
    float alpha = obj ? obj->alpha : 1.0f;
    float brightness = obj ? obj->brightness : 1.0f;
    const float *col = obj ? obj->color : (const float[]){1.0f, 1.0f, 1.0f};
    if ((loc = glGetUniformLocation(prog, "g_Alpha")) >= 0) {
        glUniform1f(loc, alpha);
    }
    if ((loc = glGetUniformLocation(prog, "g_UserAlpha")) >= 0) {
        glUniform1f(loc, 1.0f);
    }
    if ((loc = glGetUniformLocation(prog, "g_Brightness")) >= 0) {
        glUniform1f(loc, brightness);
    }
    if ((loc = glGetUniformLocation(prog, "g_Contrast")) >= 0) {
        glUniform1f(loc, 1.0f);
    }
    if ((loc = glGetUniformLocation(prog, "g_Saturation")) >= 0) {
        glUniform1f(loc, 1.0f);
    }
    if ((loc = glGetUniformLocation(prog, "g_Color")) >= 0) {
        glUniform3fv(loc, 1, col);
    }
    if ((loc = glGetUniformLocation(prog, "g_Color4")) >= 0) {
        glUniform4f(loc, col[0], col[1], col[2], alpha);
    }

    /* Audio spectrum uniforms */
    float spec64[64] = {0};
    if (params && params->audio) {
        for (int i = 0; i < 64; i++) {
            int band = (i * (int)WAYWAL_AUDIO_BARK_BANDS) / 64;
            spec64[i] = params->audio->bands[band];
        }
    }
    static const char *const audio_unames[] = {
        "g_AudioSpectrum16Left",  "g_AudioSpectrum16Right", "g_AudioSpectrum32Left",
        "g_AudioSpectrum32Right", "g_AudioSpectrum64Left",  "g_AudioSpectrum64Right",
    };
    static const int audio_counts[] = {16, 16, 32, 32, 64, 64};
    for (int a = 0; a < 6; a++) {
        loc = glGetUniformLocation(prog, audio_unames[a]);
        if (loc < 0) {
            char arr_name[64];
            snprintf(arr_name, sizeof(arr_name), "%s[0]", audio_unames[a]);
            loc = glGetUniformLocation(prog, arr_name);
        }
        if (loc >= 0) {
            glUniform1fv(loc, audio_counts[a], spec64);
        }
    }

    /* Texture slots 0..7 */
    for (uint32_t s = 0; s < WE_MAX_PASS_TEXTURES; s++) {
        GLuint tex_id = scene->tex_util_white;
        float res[4] = {fw, fh, fw, fh};

        if (s == 0 && prev_tex != 0) {
            tex_id = prev_tex;
            if (obj && obj->ping_w > 0 && obj->ping_h > 0) {
                res[0] = (float)obj->ping_w;
                res[1] = (float)obj->ping_h;
                res[2] = res[0];
                res[3] = res[1];
            }
            const float trans[2] = {0.0f, 0.0f};
            const float rot[4] = {1.0f, 0.0f, 0.0f, 1.0f};
            if ((loc = glGetUniformLocation(prog, "g_Texture0Translation")) >= 0) {
                glUniform2fv(loc, 1, trans);
            }
            if ((loc = glGetUniformLocation(prog, "g_Texture0Rotation")) >= 0) {
                glUniform4fv(loc, 1, rot);
            }
        } else if (pass->textures[s] && pass->textures[s]->gl_tex != 0) {
            we_tex_t *wt = pass->textures[s];
            if (wt->is_video_mp4) {
                (void)we_tex_advance_video(wt, dt_sec);
            }
            float trans[2] = {0.0f, 0.0f};
            float rot[4] = {1.0f, 0.0f, 0.0f, 1.0f};
            if (s == 0 && (wt->flags & WE_TEX_FLAG_IS_GIF) != 0 && wt->frame_count > 0) {
                we_tex_advance_spritesheet(wt, dt_sec, trans, rot);
            }
            tex_id = wt->gl_tex;
            res[0] = (float)wt->texture_width;
            res[1] = (float)wt->texture_height;
            res[2] = (float)wt->image_width;
            res[3] = (float)wt->image_height;

            if (s == 0) {
                if ((loc = glGetUniformLocation(prog, "g_Texture0Translation")) >= 0) {
                    glUniform2fv(loc, 1, trans);
                }
                if ((loc = glGetUniformLocation(prog, "g_Texture0Rotation")) >= 0) {
                    glUniform4fv(loc, 1, rot);
                }
            }
        } else if (pass->tex_paths[s][0] != '\0') {
            tex_id = resolve_named_rt_tex(scene, eff, pass->tex_paths[s], prev_tex, res);
        }

        for (uint32_t b = 0; b < pass->bind_count; b++) {
            if (pass->binds[b].slot == (int32_t)s) {
                tex_id =
                    resolve_named_rt_tex(scene, eff, pass->binds[b].target_name, prev_tex, res);
            }
        }

        glActiveTexture(GL_TEXTURE0 + s);
        glBindTexture(GL_TEXTURE_2D, tex_id);

        char uname[64];
        snprintf(uname, sizeof(uname), "g_Texture%u", s);
        if ((loc = glGetUniformLocation(prog, uname)) >= 0) {
            glUniform1i(loc, (GLint)s);
        }
        snprintf(uname, sizeof(uname), "g_Texture%uResolution", s);
        if ((loc = glGetUniformLocation(prog, uname)) >= 0) {
            glUniform4fv(loc, 1, res);
        }
    }
    if ((loc = glGetUniformLocation(prog, "u_album_art")) >= 0) {
        GLuint art_tex = scene->tex_album_art ? scene->tex_album_art : scene->tex_util_black;
        glActiveTexture(GL_TEXTURE0 + WE_MAX_PASS_TEXTURES);
        glBindTexture(GL_TEXTURE_2D, art_tex);
        glUniform1i(loc, (GLint)WE_MAX_PASS_TEXTURES);
    }
    glActiveTexture(GL_TEXTURE0);

    /* Apply inline uniform defaults and material constantshadervalues */
    for (uint32_t u = 0; u < pass->prep.meta.uniform_count; u++) {
        const we_shader_uniform_meta_t *um = &pass->prep.meta.uniforms[u];
        if (um->sampler_slot >= 0) {
            continue;
        }
        loc = glGetUniformLocation(prog, um->name);
        if (loc < 0) {
            continue;
        }

        const float *val = um->has_default_val ? um->default_val : NULL;
        uint32_t ncomp = um->num_components;
        bool has_script_cv = false;
        bool has_explicit_cv = false;

        /* Check if overridden in pass->const_vals by material_key or uniform name */
        for (uint32_t c = 0; c < pass->const_val_count; c++) {
            const we_const_val_t *cv = &pass->const_vals[c];
            if ((um->material_key[0] != '\0' && strcasecmp(cv->name, um->material_key) == 0) ||
                strcasecmp(cv->name, um->name) == 0) {
                if (cv->script_src && cv->script_src[0] != '\0' && cv->num_components > 0) {
                    val = cv->val;
                    ncomp = cv->num_components;
                    has_script_cv = true;
                    has_explicit_cv = true;
                } else {
                    const we_user_prop_t *up =
                        cv->user_prop[0] ? project_find_prop(&scene->project, cv->user_prop) : NULL;
                    if (up && up->num_components > 0) {
                        val = up->val;
                        ncomp = up->num_components;
                        has_explicit_cv = true;
                    } else if (cv->user_prop[0] == '\0' && cv->num_components > 0) {
                        val = cv->val;
                        ncomp = cv->num_components;
                        has_explicit_cv = true;
                    }
                }
                break;
            }
        }

        if (!has_script_cv && !has_explicit_cv) {
            const we_user_prop_t *up = NULL;
            if (um->material_key[0] != '\0') {
                up = project_find_prop(&scene->project, um->material_key);
            }
            if (!up && um->name[0] != '\0') {
                up = project_find_prop(&scene->project, um->name);
            }
            if (up && up->num_components > 0) {
                val = up->val;
                ncomp = up->num_components;
            }
        }

        if (val && ncomp > 0) {
            if (strcmp(um->type, "float") == 0 || strcmp(um->type, "half") == 0) {
                glUniform1f(loc, val[0]);
            } else if (strcmp(um->type, "int") == 0 || strcmp(um->type, "bool") == 0) {
                glUniform1i(loc, (GLint)lroundf(val[0]));
            } else if (strcmp(um->type, "int2") == 0 || strcmp(um->type, "ivec2") == 0 ||
                       strcmp(um->type, "bvec2") == 0) {
                glUniform2i(loc, (GLint)lroundf(val[0]),
                            (GLint)lroundf(ncomp >= 2 ? val[1] : val[0]));
            } else if (strcmp(um->type, "int3") == 0 || strcmp(um->type, "ivec3") == 0 ||
                       strcmp(um->type, "bvec3") == 0) {
                glUniform3i(loc, (GLint)lroundf(val[0]),
                            (GLint)lroundf(ncomp >= 2 ? val[1] : val[0]),
                            (GLint)lroundf(ncomp >= 3 ? val[2] : val[0]));
            } else if (strcmp(um->type, "int4") == 0 || strcmp(um->type, "ivec4") == 0 ||
                       strcmp(um->type, "bvec4") == 0) {
                glUniform4i(loc, (GLint)lroundf(val[0]),
                            (GLint)lroundf(ncomp >= 2 ? val[1] : val[0]),
                            (GLint)lroundf(ncomp >= 3 ? val[2] : val[0]),
                            (GLint)lroundf(ncomp >= 4 ? val[3] : 1.0f));
            } else if (strcmp(um->type, "vec2") == 0 || strcmp(um->type, "float2") == 0 ||
                       strcmp(um->type, "half2") == 0) {
                glUniform2f(loc, val[0], ncomp >= 2 ? val[1] : val[0]);
            } else if (strcmp(um->type, "vec3") == 0 || strcmp(um->type, "float3") == 0 ||
                       strcmp(um->type, "half3") == 0) {
                glUniform3f(loc, val[0], ncomp >= 2 ? val[1] : val[0],
                            ncomp >= 3 ? val[2] : val[0]);
            } else if (strcmp(um->type, "vec4") == 0 || strcmp(um->type, "float4") == 0 ||
                       strcmp(um->type, "half4") == 0) {
                glUniform4f(loc, val[0], ncomp >= 2 ? val[1] : val[0], ncomp >= 3 ? val[2] : val[0],
                            ncomp >= 4 ? val[3] : 1.0f);
            }
        }
    }
}

static void apply_gl_blend_mode(we_blend_mode_t mode)
{
    switch (mode) {
    case WE_BLEND_DISABLED:
        glDisable(GL_BLEND);
        break;
    case WE_BLEND_ADDITIVE:
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case WE_BLEND_MULTIPLY:
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case WE_BLEND_SCREEN:
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_COLOR, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case WE_BLEND_NORMAL:
    default:
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        break;
    }
}

static void step_object_scripts(we_scene_t *scene, we_scene_object_t *obj, uint32_t norm_w,
                                uint32_t norm_h, float dt_sec, float time_sec, bool cursor_moved,
                                const waywal_shader_frame_params_t *params)
{
    if (!scene || !obj || obj->script_count == 0) {
        return;
    }
    we_script_env_t env;
    build_script_env(scene, norm_w, norm_h, dt_sec, time_sec, params, &env);

    for (uint32_t s = 0; s < obj->script_count; s++) {
        we_obj_script_t *sc = &obj->scripts[s];
        if (!sc->vm) {
            continue;
        }
        bind_project_props_to_script(sc->vm, &scene->project);

        we_script_layer_state_t ls;
        if (cursor_moved) {
            sync_scene_layers_to_vm(scene, sc->vm);
            sync_layer_from_obj(obj, &ls);
            if (we_script_eval_cursor_move(sc->vm, &env, &ls)) {
                sync_obj_from_layer(obj, &ls);
                sync_scene_layers_from_vm(scene, sc->vm);
            }
        }

        sync_scene_layers_to_vm(scene, sc->vm);
        sync_layer_from_obj(obj, &ls);

        float in_val[4] = {0};
        float out_val[4] = {0};
        uint32_t ncomp = 1;
        we_const_val_t *target_cv = NULL;

        switch (sc->target) {
        case WE_SCRIPT_PROP_ORIGIN:
            memcpy(in_val, obj->origin, 3 * sizeof(float));
            ncomp = 3;
            break;
        case WE_SCRIPT_PROP_SCALE:
            memcpy(in_val, obj->scale, 3 * sizeof(float));
            ncomp = 3;
            break;
        case WE_SCRIPT_PROP_ANGLES:
            memcpy(in_val, obj->angles, 3 * sizeof(float));
            ncomp = 3;
            break;
        case WE_SCRIPT_PROP_ALPHA:
            in_val[0] = obj->alpha;
            ncomp = 1;
            break;
        case WE_SCRIPT_PROP_COLOR:
            memcpy(in_val, obj->color, 3 * sizeof(float));
            ncomp = 3;
            break;
        case WE_SCRIPT_PROP_VISIBLE:
            in_val[0] = obj->visible ? 1.0f : 0.0f;
            ncomp = 1;
            break;
        case WE_SCRIPT_PROP_UNIFORM:
            for (uint32_t c = 0; c < obj->base_pass.const_val_count; c++) {
                if (strcasecmp(obj->base_pass.const_vals[c].name, sc->uniform_name) == 0) {
                    target_cv = &obj->base_pass.const_vals[c];
                    break;
                }
            }
            if (target_cv) {
                ncomp = target_cv->num_components > 0 ? target_cv->num_components : 1u;
                memcpy(in_val, target_cv->val, ncomp * sizeof(float));
            }
            break;
        }

        memcpy(out_val, in_val, ncomp * sizeof(float));
        if (we_script_eval_update(sc->vm, &env, &ls, in_val, ncomp, out_val)) {
            sync_obj_from_layer(obj, &ls);
            sync_scene_layers_from_vm(scene, sc->vm);
            if (sc->target == WE_SCRIPT_PROP_UNIFORM ||
                memcmp(out_val, in_val, ncomp * sizeof(float)) != 0) {
                switch (sc->target) {
                case WE_SCRIPT_PROP_ORIGIN:
                    memcpy(obj->origin, out_val, 3 * sizeof(float));
                    break;
                case WE_SCRIPT_PROP_SCALE:
                    memcpy(obj->scale, out_val, 3 * sizeof(float));
                    break;
                case WE_SCRIPT_PROP_ANGLES:
                    memcpy(obj->angles, out_val, 3 * sizeof(float));
                    break;
                case WE_SCRIPT_PROP_ALPHA:
                    obj->alpha = out_val[0];
                    break;
                case WE_SCRIPT_PROP_COLOR:
                    memcpy(obj->color, out_val, 3 * sizeof(float));
                    break;
                case WE_SCRIPT_PROP_VISIBLE:
                    obj->visible = (out_val[0] != 0.0f);
                    break;
                case WE_SCRIPT_PROP_UNIFORM:
                    if (target_cv) {
                        memcpy(target_cv->val, out_val, ncomp * sizeof(float));
                    }
                    break;
                }
            }
        }
    }
}

static void render_scene_to_fbo(we_scene_t *scene, uint32_t target_w, uint32_t target_h,
                                const waywal_shader_frame_params_t *params)
{
    uint32_t sw = (uint32_t)(scene->ortho_width > 1.0f ? scene->ortho_width : (float)target_w);
    uint32_t sh = (uint32_t)(scene->ortho_height > 1.0f ? scene->ortho_height : (float)target_h);
    if (sw == 0) {
        sw = target_w > 0 ? target_w : 1920;
    }
    if (sh == 0) {
        sh = target_h > 0 ? target_h : 1080;
    }
    uint32_t norm_w = target_w > 0 ? target_w : sw;
    uint32_t norm_h = target_h > 0 ? target_h : sh;

    float dt_sec =
        (params && params->time_delta_sec > 0.0f) ? params->time_delta_sec : (1.0f / 60.0f);
    if (scene->first_frame_done) {
        scene->sim_time += dt_sec;
    }
    float time_sec = (params && params->time_sec > 0.0f) ? params->time_sec : scene->sim_time;

    float cur_ptr[2] = {0.5f, 0.5f};
    if (params && norm_w > 0 && norm_h > 0 &&
        (params->mouse[0] > 0.0f || params->mouse[1] > 0.0f)) {
        cur_ptr[0] = params->mouse[0] / (float)norm_w;
        cur_ptr[1] = params->mouse[1] / (float)norm_h;
    }
    bool cursor_moved =
        scene->first_frame_done && (fabsf(cur_ptr[0] - scene->last_pointer[0]) > 1e-5f ||
                                    fabsf(cur_ptr[1] - scene->last_pointer[1]) > 1e-5f);
    scene->first_frame_done = true;

    ensure_fbo_pair(&scene->scene_fbo, &scene->scene_fbo_tex, &scene->scene_fbo_w,
                    &scene->scene_fbo_h, sw, sh);

    glBindFramebuffer(GL_FRAMEBUFFER, scene->scene_fbo);
    glViewport(0, 0, (GLsizei)sw, (GLsizei)sh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    if (scene->clear_enabled) {
        glClearColor(scene->clear_color[0], scene->clear_color[1], scene->clear_color[2], 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    /* Compute camera parallax & shake offsets normalized by target_w/target_h */
    float pdx = 0.0f, pdy = 0.0f;
    if (scene->camera_parallax && params && norm_w > 0 && norm_h > 0) {
        float mx = (params->mouse[0] / (float)norm_w) * 2.0f - 1.0f;
        float my = (params->mouse[1] / (float)norm_h) * 2.0f - 1.0f;
        float target_px =
            mx * scene->camera_parallax_amount * scene->camera_parallax_mouse_influence * 25.0f;
        float target_py =
            my * scene->camera_parallax_amount * scene->camera_parallax_mouse_influence * 25.0f;
        float alpha = scene->camera_parallax_delay > 0.001f ? 0.15f : 1.0f;
        scene->smooth_parallax[0] += (target_px - scene->smooth_parallax[0]) * alpha;
        scene->smooth_parallax[1] += (target_py - scene->smooth_parallax[1]) * alpha;
        pdx = scene->smooth_parallax[0];
        pdy = scene->smooth_parallax[1];
    }

    float shake_x = 0.0f, shake_y = 0.0f;
    if (scene->camera_shake && params) {
        float t = time_sec * scene->camera_shake_speed;
        shake_x = sinf(t * 1.7f) * scene->camera_shake_amp * 4.0f;
        shake_y = cosf(t * 2.3f) * scene->camera_shake_amp * 4.0f;
    }

    float vp[16];
    mat4_ortho_topdown((float)sw, (float)sh, shake_x, shake_y, vp);

    glBindVertexArray(scene->quad_vao);

    for (uint32_t i = 0; i < scene->object_count; i++) {
        we_scene_object_t *obj = &scene->objects[i];

        /* Refresh dynamic user-bound properties */
        if (obj->visible_user_prop[0] != '\0') {
            const we_user_prop_t *up = project_find_prop(&scene->project, obj->visible_user_prop);
            if (up && up->num_components > 0) {
                obj->visible = (up->val[0] != 0.0f);
            }
        }
        if (obj->alpha_user_prop[0] != '\0') {
            const we_user_prop_t *up = project_find_prop(&scene->project, obj->alpha_user_prop);
            if (up && up->num_components > 0) {
                obj->alpha = up->val[0];
            }
        }
        if (obj->color_user_prop[0] != '\0') {
            const we_user_prop_t *up = project_find_prop(&scene->project, obj->color_user_prop);
            if (up && up->num_components >= 3) {
                obj->color[0] = up->val[0];
                obj->color[1] = up->val[1];
                obj->color[2] = up->val[2];
            }
        }
    }

    /* Evaluate SceneScript update / cursorMove callbacks across all objects before rendering */
    for (uint32_t i = 0; i < scene->object_count; i++) {
        step_object_scripts(scene, &scene->objects[i], norm_w, norm_h, dt_sec, time_sec,
                            cursor_moved, params);
    }

    for (uint32_t i = 0; i < scene->object_count; i++) {
        we_scene_object_t *obj = &scene->objects[i];

        if (!obj->visible || obj->alpha <= 0.0001f) {
            continue;
        }

        /* Particle system simulation & batched rendering */
        if (obj->particle_sys) {
            float world_node[16], mvp[16];
            compute_object_node_world_matrix(scene, obj, pdx, pdy, world_node);
            mat4_mul(vp, world_node, mvp);

            float dx = cur_ptr[0] * (float)sw - world_node[12];
            float dy = (1.0f - cur_ptr[1]) * (float)sh - world_node[13];
            float m00 = world_node[0], m01 = world_node[4];
            float m10 = world_node[1], m11 = world_node[5];
            float det = m00 * m11 - m01 * m10;
            float lx = dx, ly = dy;
            if (fabsf(det) > 1e-8f) {
                float inv_det = 1.0f / det;
                lx = (m11 * dx - m01 * dy) * inv_det;
                ly = (-m10 * dx + m00 * dy) * inv_det;
            }
            we_particle_set_pointer(obj->particle_sys, lx, ly, 0.0f);
            we_particle_update(obj->particle_sys, dt_sec);

            if (scene->particle_program != 0) {
                glBindFramebuffer(GL_FRAMEBUFFER, scene->scene_fbo);
                glViewport(0, 0, (GLsizei)sw, (GLsizei)sh);
                apply_gl_blend_mode(obj->base_pass.blending);
                glUseProgram(scene->particle_program);

                GLint loc;
                if ((loc = glGetUniformLocation(scene->particle_program,
                                                "g_ModelViewProjectionMatrix")) >= 0) {
                    glUniformMatrix4fv(loc, 1, GL_FALSE, mvp);
                }
                if ((loc = glGetUniformLocation(scene->particle_program, "g_Color4")) >= 0) {
                    glUniform4f(loc, obj->color[0] * obj->brightness,
                                obj->color[1] * obj->brightness, obj->color[2] * obj->brightness,
                                obj->alpha);
                }
                GLuint ptex = scene->tex_util_white;
                float trans[2] = {0.0f, 0.0f};
                float rot[4] = {1.0f, 0.0f, 0.0f, 1.0f};
                if (obj->base_pass.textures[0] && obj->base_pass.textures[0]->gl_tex != 0) {
                    we_tex_t *wt = obj->base_pass.textures[0];
                    if (wt->is_video_mp4) {
                        (void)we_tex_advance_video(wt, dt_sec);
                    }
                    if ((wt->flags & WE_TEX_FLAG_IS_GIF) != 0 && wt->frame_count > 0) {
                        we_tex_advance_spritesheet(wt, dt_sec, trans, rot);
                    }
                    ptex = wt->gl_tex;
                }
                if ((loc = glGetUniformLocation(scene->particle_program,
                                                "g_Texture0Translation")) >= 0) {
                    glUniform2fv(loc, 1, trans);
                }
                if ((loc = glGetUniformLocation(scene->particle_program, "g_Texture0Rotation")) >=
                    0) {
                    glUniform4fv(loc, 1, rot);
                }
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, ptex);
                if ((loc = glGetUniformLocation(scene->particle_program, "g_Texture0")) >= 0) {
                    glUniform1i(loc, 0);
                }
                we_particle_draw_gl(obj->particle_sys, scene->particle_program);
                glBindVertexArray(scene->quad_vao);
            }
            continue;
        }

        if (!obj->has_base_pass || obj->base_pass.gl_prog == 0) {
            continue;
        }

        /* Count active effects */
        uint32_t active_effs = 0;
        for (uint32_t e = 0; e < obj->effect_count; e++) {
            if (obj->effects[e].visible_user_prop[0] != '\0') {
                const we_user_prop_t *up =
                    project_find_prop(&scene->project, obj->effects[e].visible_user_prop);
                if (up && up->num_components > 0) {
                    obj->effects[e].visible = (up->val[0] != 0.0f);
                }
            }
            if (obj->effects[e].visible && obj->effects[e].pass_count > 0) {
                active_effs++;
            }
        }

        if (obj->copybackground || active_effs > 0) {
            ensure_fbo_pair(&scene->full_fb_fbo, &scene->full_fb_tex, &scene->full_fb_w,
                            &scene->full_fb_h, sw, sh);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, scene->scene_fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scene->full_fb_fbo);
            glBlitFramebuffer(0, 0, (GLint)sw, (GLint)sh, 0, 0, (GLint)sw, (GLint)sh,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }

        if (active_effs == 0) {
            /* Fast path: render object directly into scene_fbo */
            float model[16], mvp[16];
            if (obj->puppet) {
                compute_object_node_world_matrix(scene, obj, pdx, pdy, model);
            } else {
                compute_object_model_matrix(scene, obj, pdx, pdy, model);
            }
            mat4_mul(vp, model, mvp);

            glBindFramebuffer(GL_FRAMEBUFFER, scene->scene_fbo);
            glViewport(0, 0, (GLsizei)sw, (GLsizei)sh);
            apply_gl_blend_mode(obj->base_pass.blending);
            bind_pass_uniforms(scene, obj, NULL, &obj->base_pass, model, vp, mvp, 0, sw, sh, norm_w,
                               norm_h, params);
            if (obj->puppet) {
                we_puppet_draw_gl(obj->puppet, obj->base_pass.gl_prog, (double)time_sec);
                glBindVertexArray(scene->quad_vao);
            } else {
                glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
            }
        } else {
            /* Ping-pong effect chain path */
            uint32_t pw = (uint32_t)(obj->size[0] > 1.0f ? obj->size[0] : (float)sw);
            uint32_t ph = (uint32_t)(obj->size[1] > 1.0f ? obj->size[1] : (float)sh);
            if (obj->fullscreen) {
                pw = sw;
                ph = sh;
            }
            ensure_fbo_pair(&obj->ping_fbo[0], &obj->ping_tex[0], &obj->ping_w, &obj->ping_h, pw,
                            ph);
            uint32_t dummy_w = obj->ping_w, dummy_h = obj->ping_h;
            if (!obj->ping_fbo[1]) {
                dummy_w = 0;
            }
            ensure_fbo_pair(&obj->ping_fbo[1], &obj->ping_tex[1], &dummy_w, &dummy_h, pw, ph);

            uint32_t cur_idx = 0;
            float ident[16];
            mat4_identity(ident);

            /* Seed ping_fbo[0] by drawing obj->base_pass so compressed S3TC/BC7 textures,
             * swizzles, and spritesheets render cleanly into ping_fbo[0] */
            glBindFramebuffer(GL_FRAMEBUFFER, obj->ping_fbo[0]);
            glViewport(0, 0, (GLsizei)pw, (GLsizei)ph);
            glDisable(GL_BLEND);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            if (obj->puppet) {
                float p_mvp[16];
                mat4_identity(p_mvp);
                p_mvp[0] = 2.0f / (float)pw;
                p_mvp[5] = -2.0f / (float)ph;
                p_mvp[10] = -1.0f / 2000.0f;
                bind_pass_uniforms(scene, obj, NULL, &obj->base_pass, ident, p_mvp, p_mvp, 0, pw,
                                   ph, norm_w, norm_h, params);
                we_puppet_draw_gl(obj->puppet, obj->base_pass.gl_prog, (double)time_sec);
                glBindVertexArray(scene->quad_vao);
            } else {
                bind_pass_uniforms(scene, obj, NULL, &obj->base_pass, ident, ident, ident, 0, pw,
                                   ph, norm_w, norm_h, params);
                glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
            }
            GLuint cur_tex = obj->ping_tex[0];

            for (uint32_t e = 0; e < obj->effect_count; e++) {
                we_effect_t *eff = &obj->effects[e];
                if (!eff->visible) {
                    continue;
                }
                for (uint32_t f = 0; f < eff->fbo_count; f++) {
                    uint32_t sc = eff->fbos[f].scale > 0 ? eff->fbos[f].scale : 1;
                    uint32_t fw = pw / sc > 0 ? pw / sc : 1;
                    uint32_t fh = ph / sc > 0 ? ph / sc : 1;
                    ensure_fbo_pair(&eff->fbos[f].gl_fbo, &eff->fbos[f].gl_tex, &eff->fbos[f].width,
                                    &eff->fbos[f].height, fw, fh);
                }
                for (uint32_t p = 0; p < eff->pass_count; p++) {
                    we_material_pass_t *ep = &eff->passes[p];
                    if (strcasecmp(ep->command, "swap") == 0) {
                        cur_idx ^= 1u;
                        cur_tex = obj->ping_tex[cur_idx];
                        continue;
                    }
                    if (strcasecmp(ep->command, "copy") == 0) {
                        if (ep->target_fbo[0] != '\0') {
                            we_rt_fbo_t *dst_rt = NULL;
                            for (uint32_t f = 0; f < eff->fbo_count; f++) {
                                if (strcasecmp(eff->fbos[f].name, ep->target_fbo) == 0 &&
                                    eff->fbos[f].gl_fbo) {
                                    dst_rt = &eff->fbos[f];
                                    break;
                                }
                            }
                            if (!dst_rt) {
                                dst_rt = get_or_create_scene_rt_fbo(scene, ep->target_fbo, pw, ph);
                            }
                            if (dst_rt && dst_rt->gl_fbo) {
                                glBindFramebuffer(GL_READ_FRAMEBUFFER, obj->ping_fbo[cur_idx]);
                                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_rt->gl_fbo);
                                glBlitFramebuffer(0, 0, (GLint)pw, (GLint)ph, 0, 0,
                                                  (GLint)dst_rt->width, (GLint)dst_rt->height,
                                                  GL_COLOR_BUFFER_BIT, GL_LINEAR);
                            }
                        }
                        continue;
                    }
                    if (!ep->gl_prog) {
                        continue;
                    }

                    uint32_t next_idx = cur_idx ^ 1u;
                    GLuint dst_fbo = obj->ping_fbo[next_idx];
                    GLuint dst_tex = obj->ping_tex[next_idx];
                    uint32_t dst_w = pw, dst_h = ph;
                    bool wrote_ping = true;

                    if (ep->target_fbo[0] != '\0') {
                        we_rt_fbo_t *dst_rt = NULL;
                        for (uint32_t f = 0; f < eff->fbo_count; f++) {
                            if (strcasecmp(eff->fbos[f].name, ep->target_fbo) == 0 &&
                                eff->fbos[f].gl_fbo) {
                                dst_rt = &eff->fbos[f];
                                break;
                            }
                        }
                        if (!dst_rt) {
                            dst_rt = get_or_create_scene_rt_fbo(scene, ep->target_fbo, pw, ph);
                        }
                        if (dst_rt && dst_rt->gl_fbo) {
                            dst_fbo = dst_rt->gl_fbo;
                            dst_tex = dst_rt->gl_tex;
                            dst_w = dst_rt->width;
                            dst_h = dst_rt->height;
                            wrote_ping = false;
                        }
                    }

                    glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
                    glViewport(0, 0, (GLsizei)dst_w, (GLsizei)dst_h);
                    apply_gl_blend_mode(ep->blending);
                    bind_pass_uniforms(scene, obj, eff, ep, ident, ident, ident, cur_tex, dst_w,
                                       dst_h, norm_w, norm_h, params);
                    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

                    if (wrote_ping) {
                        cur_idx = next_idx;
                        cur_tex = dst_tex;
                    }
                }
            }

            if (obj->id >= 0) {
                char comp_name[64];
                snprintf(comp_name, sizeof(comp_name), "_rt_imageLayerComposite_%d_a", obj->id);
                we_rt_fbo_t *comp_rt = get_or_create_scene_rt_fbo(scene, comp_name, pw, ph);
                if (comp_rt && comp_rt->gl_fbo) {
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, obj->ping_fbo[cur_idx]);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, comp_rt->gl_fbo);
                    glBlitFramebuffer(0, 0, (GLint)pw, (GLint)ph, 0, 0, (GLint)comp_rt->width,
                                      (GLint)comp_rt->height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
                }
            }

            /* Final object composite pass into scene_fbo using default_program */
            float model[16], mvp[16];
            compute_object_model_matrix(scene, obj, pdx, pdy, model);
            mat4_mul(vp, model, mvp);

            glBindFramebuffer(GL_FRAMEBUFFER, scene->scene_fbo);
            glViewport(0, 0, (GLsizei)sw, (GLsizei)sh);
            apply_gl_blend_mode(obj->base_pass.blending);
            if (scene->default_program != 0) {
                glUseProgram(scene->default_program);
                GLint loc_mvp =
                    glGetUniformLocation(scene->default_program, "g_ModelViewProjectionMatrix");
                if (loc_mvp >= 0) {
                    glUniformMatrix4fv(loc_mvp, 1, GL_FALSE, mvp);
                }
                GLint loc_t0 = glGetUniformLocation(scene->default_program, "g_Texture0");
                if (loc_t0 >= 0) {
                    glUniform1i(loc_t0, 0);
                }
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, cur_tex);
                glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
            } else {
                bind_pass_uniforms(scene, obj, NULL, &obj->base_pass, model, vp, mvp, cur_tex, sw,
                                   sh, norm_w, norm_h, params);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, cur_tex);
                glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
            }
        }
    }

    scene->last_pointer[0] = cur_ptr[0];
    scene->last_pointer[1] = cur_ptr[1];
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray(0);
}

static bool import_scene_bo_tex(we_scene_t *scene, dmabuf_bo_t *bo, GLuint *out_tex)
{
    if (!scene || !bo || !out_tex) {
        return false;
    }

    for (size_t i = 0; i < scene->num_imported_bos; i++) {
        if (scene->imported_bos[i].bo == bo && scene->imported_bos[i].gl_tex != 0 &&
            scene->imported_bos[i].egl_image != NULL) {
            *out_tex = scene->imported_bos[i].gl_tex;
            return true;
        }
    }

    PFNEGLCREATEIMAGEKHRPROC create_img =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC destroy_img =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target_tex2d =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC target_storage =
        (PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC)eglGetProcAddress("glEGLImageTargetTexStorageEXT");

    if (!create_img || !target_tex2d) {
        return false;
    }

    static const EGLint plane_fd_keys[4] = {
        EGL_DMA_BUF_PLANE0_FD_EXT,
        EGL_DMA_BUF_PLANE1_FD_EXT,
        EGL_DMA_BUF_PLANE2_FD_EXT,
        EGL_DMA_BUF_PLANE3_FD_EXT,
    };
    static const EGLint plane_offset_keys[4] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT,
        EGL_DMA_BUF_PLANE3_OFFSET_EXT,
    };
    static const EGLint plane_pitch_keys[4] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        EGL_DMA_BUF_PLANE1_PITCH_EXT,
        EGL_DMA_BUF_PLANE2_PITCH_EXT,
        EGL_DMA_BUF_PLANE3_PITCH_EXT,
    };
    static const EGLint plane_mod_lo_keys[4] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
    };
    static const EGLint plane_mod_hi_keys[4] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT,
    };

    uint32_t num_planes = bo->num_planes > 0 ? bo->num_planes : 1u;
    if (num_planes > 4u) {
        num_planes = 4u;
    }

    EGLint attribs[64];
    int n = 0;
    attribs[n++] = EGL_WIDTH;
    attribs[n++] = (EGLint)bo->width;
    attribs[n++] = EGL_HEIGHT;
    attribs[n++] = (EGLint)bo->height;
    attribs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
    attribs[n++] = (EGLint)bo->drm_format;

    for (uint32_t p = 0; p < num_planes; p++) {
        if (bo->fd[p] < 0) {
            continue;
        }
        attribs[n++] = plane_fd_keys[p];
        attribs[n++] = bo->fd[p];
        attribs[n++] = plane_offset_keys[p];
        attribs[n++] = (EGLint)bo->offset[p];
        attribs[n++] = plane_pitch_keys[p];
        attribs[n++] = (EGLint)bo->stride[p];
        if (bo->modifier != DRM_FORMAT_MOD_INVALID) {
            attribs[n++] = plane_mod_lo_keys[p];
            attribs[n++] = (EGLint)(bo->modifier & 0xFFFFFFFFu);
            attribs[n++] = plane_mod_hi_keys[p];
            attribs[n++] = (EGLint)((bo->modifier >> 32u) & 0xFFFFFFFFu);
        }
    }
    attribs[n] = EGL_NONE;

    EGLImageKHR img =
        create_img(scene->egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
    if (img == EGL_NO_IMAGE_KHR) {
        return false;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (target_storage) {
        target_storage(GL_TEXTURE_2D, (GLeglImageOES)img, NULL);
    } else {
        target_tex2d(GL_TEXTURE_2D, img);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    /* Track imported textures per scene rather than mutating bo->gl_tex across EGL contexts */
    size_t slot = scene->num_imported_bos;
    for (size_t i = 0; i < scene->num_imported_bos; i++) {
        if (scene->imported_bos[i].bo == bo) {
            if (scene->imported_bos[i].owns_egl) {
                if (scene->imported_bos[i].gl_tex && scene->imported_bos[i].gl_tex != tex) {
                    glDeleteTextures(1, &scene->imported_bos[i].gl_tex);
                }
                if (scene->imported_bos[i].egl_image && scene->imported_bos[i].egl_image != img &&
                    destroy_img) {
                    destroy_img(scene->egl_display, scene->imported_bos[i].egl_image);
                }
            }
            slot = i;
            break;
        }
    }
    if (slot >= 32u) {
        if (scene->imported_bos[0].owns_egl) {
            if (scene->imported_bos[0].gl_tex) {
                GLuint del_tex = scene->imported_bos[0].gl_tex;
                glDeleteTextures(1, &del_tex);
            }
            if (scene->imported_bos[0].egl_image && destroy_img) {
                void *del_img = scene->imported_bos[0].egl_image;
                destroy_img(scene->egl_display, del_img);
            }
        }
        for (size_t j = 1; j < 32u; j++) {
            scene->imported_bos[j - 1] = scene->imported_bos[j];
        }
        scene->num_imported_bos = 31u;
        slot = 31u;
    }

    scene->imported_bos[slot].bo = bo;
    scene->imported_bos[slot].gl_tex = tex;
    scene->imported_bos[slot].egl_image = img;
    scene->imported_bos[slot].owns_egl = true;
    if (slot == scene->num_imported_bos) {
        scene->num_imported_bos++;
    }
    *out_tex = tex;
    return true;
}

bool we_scene_render_frame(we_scene_t *scene, dmabuf_bo_t *target_bo,
                           const waywal_shader_frame_params_t *params)
{
    if (!scene || !target_bo || target_bo->width == 0 || target_bo->height == 0) {
        return false;
    }
    if (!scene->gl_initialized) {
        return false;
    }

    if (scene->egl_display && scene->egl_context && eglGetCurrentContext() != scene->egl_context) {
        eglMakeCurrent(scene->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, scene->egl_context);
    }

    GLuint bo_gl_tex = 0;
    if (!import_scene_bo_tex(scene, target_bo, &bo_gl_tex) || bo_gl_tex == 0) {
        return false;
    }

    render_scene_to_fbo(scene, target_bo->width, target_bo->height, params);

    /* Blit scene_fbo_tex into bo_gl_tex with opaque alpha 1.0 so transparent shaders do not render
     * black on Wayland */
    glBindFramebuffer(GL_FRAMEBUFFER, scene->target_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bo_gl_tex, 0);
    glViewport(0, 0, (GLsizei)target_bo->width, (GLsizei)target_bo->height);
    glDisable(GL_BLEND);

    glUseProgram(scene->prog_present);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene->scene_fbo_tex);
    GLint loc_tex = glGetUniformLocation(scene->prog_present, "u_tex");
    if (loc_tex >= 0) {
        glUniform1i(loc_tex, 0);
    }
    glBindVertexArray(scene->quad_vao);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glBindVertexArray(0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* Synchronize GPU completion before returning the DMA-BUF to Wayland */
    PFNEGLCREATESYNCKHRPROC create_sync =
        (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    PFNEGLCLIENTWAITSYNCKHRPROC wait_sync =
        (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");
    PFNEGLDESTROYSYNCKHRPROC destroy_sync =
        (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");

    if (create_sync && wait_sync && destroy_sync && scene->egl_display != EGL_NO_DISPLAY) {
        EGLSyncKHR sync = create_sync(scene->egl_display, EGL_SYNC_FENCE_KHR, NULL);
        if (sync != EGL_NO_SYNC_KHR) {
            wait_sync(scene->egl_display, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 100000000ULL);
            destroy_sync(scene->egl_display, sync);
        } else {
            glFinish();
        }
    } else {
        glFinish();
    }

    return true;
}

static void cpu_blend_pixel(uint8_t *dp, float sr, float sg, float sb, float sa,
                            we_blend_mode_t blend)
{
    if (sa <= 0.0001f) {
        return;
    }
    float dr = dp[0] / 255.0f;
    float dg = dp[1] / 255.0f;
    float db = dp[2] / 255.0f;

    float or_val = dr, og_val = dg, ob_val = db;
    if (blend == WE_BLEND_ADDITIVE) {
        or_val = dr + sr * sa;
        og_val = dg + sg * sa;
        ob_val = db + sb * sa;
    } else if (blend == WE_BLEND_MULTIPLY) {
        or_val = dr * (1.0f - sa) + (dr * sr) * sa;
        og_val = dg * (1.0f - sa) + (dg * sg) * sa;
        ob_val = db * (1.0f - sa) + (db * sb) * sa;
    } else if (blend == WE_BLEND_SCREEN) {
        or_val = 1.0f - (1.0f - dr) * (1.0f - sr * sa);
        og_val = 1.0f - (1.0f - dg) * (1.0f - sg * sa);
        ob_val = 1.0f - (1.0f - db) * (1.0f - sb * sa);
    } else if (blend == WE_BLEND_DISABLED) {
        or_val = sr;
        og_val = sg;
        ob_val = sb;
    } else {
        or_val = dr * (1.0f - sa) + sr * sa;
        og_val = dg * (1.0f - sa) + sg * sa;
        ob_val = db * (1.0f - sa) + sb * sa;
    }

    dp[0] = (uint8_t)(fminf(fmaxf(or_val, 0.0f), 1.0f) * 255.0f + 0.5f);
    dp[1] = (uint8_t)(fminf(fmaxf(og_val, 0.0f), 1.0f) * 255.0f + 0.5f);
    dp[2] = (uint8_t)(fminf(fmaxf(ob_val, 0.0f), 1.0f) * 255.0f + 0.5f);
    dp[3] = 255;
}

static void cpu_rasterize_triangle(uint8_t *out_rgba, uint32_t width, uint32_t height,
                                   float ortho_w, float ortho_h, const float model[16],
                                   const float p0[3], const float uv0[2], const float c0[4],
                                   const float p1[3], const float uv1[2], const float c1[4],
                                   const float p2[3], const float uv2[2], const float c2[4],
                                   const uint8_t *src_rgba, uint32_t tw, uint32_t th, float r_mul,
                                   float g_mul, float b_mul, float alpha, we_blend_mode_t blend)
{
    float wx0 = model[0] * p0[0] + model[4] * p0[1] + model[12];
    float wy0 = model[1] * p0[0] + model[5] * p0[1] + model[13];
    float wx1 = model[0] * p1[0] + model[4] * p1[1] + model[12];
    float wy1 = model[1] * p1[0] + model[5] * p1[1] + model[13];
    float wx2 = model[0] * p2[0] + model[4] * p2[1] + model[12];
    float wy2 = model[1] * p2[0] + model[5] * p2[1] + model[13];

    float sx0 = (wx0 / ortho_w) * (float)width;
    float sy0 = (1.0f - wy0 / ortho_h) * (float)height;
    float sx1 = (wx1 / ortho_w) * (float)width;
    float sy1 = (1.0f - wy1 / ortho_h) * (float)height;
    float sx2 = (wx2 / ortho_w) * (float)width;
    float sy2 = (1.0f - wy2 / ortho_h) * (float)height;

    float min_x = fminf(sx0, fminf(sx1, sx2));
    float max_x = fmaxf(sx0, fmaxf(sx1, sx2));
    float min_y = fminf(sy0, fminf(sy1, sy2));
    float max_y = fmaxf(sy0, fmaxf(sy1, sy2));

    int32_t x0 = (int32_t)floorf(min_x);
    int32_t x1 = (int32_t)ceilf(max_x);
    int32_t y0 = (int32_t)floorf(min_y);
    int32_t y1 = (int32_t)ceilf(max_y);
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > (int32_t)width) {
        x1 = (int32_t)width;
    }
    if (y1 > (int32_t)height) {
        y1 = (int32_t)height;
    }
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    float area = (sx1 - sx0) * (sy2 - sy0) - (sy1 - sy0) * (sx2 - sx0);
    if (fabsf(area) < 1e-6f) {
        return;
    }
    float inv_area = 1.0f / area;

    for (int32_t y = y0; y < y1; y++) {
        float py = (float)y + 0.5f;
        for (int32_t x = x0; x < x1; x++) {
            float px = (float)x + 0.5f;
            float w0 = ((sx1 - px) * (sy2 - py) - (sy1 - py) * (sx2 - px)) * inv_area;
            float w1 = ((sx2 - px) * (sy0 - py) - (sy2 - py) * (sx0 - px)) * inv_area;
            float w2 = 1.0f - w0 - w1;
            if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) {
                continue;
            }
            float u = fminf(fmaxf(w0 * uv0[0] + w1 * uv1[0] + w2 * uv2[0], 0.0f), 0.999999f);
            float v = fminf(fmaxf(w0 * uv0[1] + w1 * uv1[1] + w2 * uv2[1], 0.0f), 0.999999f);
            uint32_t tx_i = (uint32_t)(u * (float)tw);
            uint32_t ty_i = (uint32_t)(v * (float)th);
            const uint8_t *sp = &src_rgba[(ty_i * tw + tx_i) * 4];

            float vc_r = w0 * c0[0] + w1 * c1[0] + w2 * c2[0];
            float vc_g = w0 * c0[1] + w1 * c1[1] + w2 * c2[1];
            float vc_b = w0 * c0[2] + w1 * c1[2] + w2 * c2[2];
            float vc_a = w0 * c0[3] + w1 * c1[3] + w2 * c2[3];

            float sr = (sp[0] / 255.0f) * vc_r * r_mul;
            float sg = (sp[1] / 255.0f) * vc_g * g_mul;
            float sb = (sp[2] / 255.0f) * vc_b * b_mul;
            float sa = (sp[3] / 255.0f) * vc_a * alpha;

            cpu_blend_pixel(&out_rgba[(y * (int32_t)width + x) * 4], sr, sg, sb, sa, blend);
        }
    }
}

bool we_scene_render_rgba8(we_scene_t *scene, uint32_t width, uint32_t height,
                           const waywal_shader_frame_params_t *params, uint8_t *out_rgba)
{
    if (!scene || width == 0 || height == 0 || !out_rgba) {
        return false;
    }

    if (scene->gl_initialized) {
        if (scene->egl_display && scene->egl_context &&
            eglGetCurrentContext() != scene->egl_context) {
            eglMakeCurrent(scene->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, scene->egl_context);
        }
        render_scene_to_fbo(scene, width, height, params);
        if (scene->scene_fbo_w == width && scene->scene_fbo_h == height) {
            glBindFramebuffer(GL_FRAMEBUFFER, scene->scene_fbo);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE,
                         out_rgba);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        } else {
            /* Blit scene->scene_fbo into scene->target_fbo when requested dimensions differ */
            GLuint tmp_tex = 0;
            glGenTextures(1, &tmp_tex);
            glBindTexture(GL_TEXTURE_2D, tmp_tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width, (GLsizei)height, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, NULL);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scene->target_fbo);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   tmp_tex, 0);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, scene->scene_fbo);
            glBlitFramebuffer(0, 0, (GLint)scene->scene_fbo_w, (GLint)scene->scene_fbo_h, 0, 0,
                              (GLint)width, (GLint)height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
            glBindFramebuffer(GL_FRAMEBUFFER, scene->target_fbo);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE,
                         out_rgba);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteTextures(1, &tmp_tex);
        }
        return true;
    }

    /* Software CPU compositor fallback when running headless without an EGL/GLES context */
    uint8_t cr = (uint8_t)(fminf(fmaxf(scene->clear_color[0], 0.0f), 1.0f) * 255.0f + 0.5f);
    uint8_t cg = (uint8_t)(fminf(fmaxf(scene->clear_color[1], 0.0f), 1.0f) * 255.0f + 0.5f);
    uint8_t cb = (uint8_t)(fminf(fmaxf(scene->clear_color[2], 0.0f), 1.0f) * 255.0f + 0.5f);

    size_t npx = (size_t)width * (size_t)height;
    for (size_t i = 0; i < npx; i++) {
        out_rgba[i * 4 + 0] = cr;
        out_rgba[i * 4 + 1] = cg;
        out_rgba[i * 4 + 2] = cb;
        out_rgba[i * 4 + 3] = 255;
    }

    float ortho_w = scene->ortho_width > 1.0f ? scene->ortho_width : 1920.0f;
    float ortho_h = scene->ortho_height > 1.0f ? scene->ortho_height : 1080.0f;

    float dt_sec =
        (params && params->time_delta_sec > 0.0f) ? params->time_delta_sec : (1.0f / 60.0f);
    if (scene->first_frame_done) {
        scene->sim_time += dt_sec;
    }
    float time_sec = (params && params->time_sec > 0.0f) ? params->time_sec : scene->sim_time;

    float cur_ptr[2] = {0.5f, 0.5f};
    if (params && width > 0 && height > 0 && (params->mouse[0] > 0.0f || params->mouse[1] > 0.0f)) {
        cur_ptr[0] = params->mouse[0] / (float)width;
        cur_ptr[1] = params->mouse[1] / (float)height;
    }
    bool cursor_moved =
        scene->first_frame_done && (fabsf(cur_ptr[0] - scene->last_pointer[0]) > 1e-5f ||
                                    fabsf(cur_ptr[1] - scene->last_pointer[1]) > 1e-5f);
    scene->first_frame_done = true;

    static const uint8_t white_px[4] = {255, 255, 255, 255};
    static const float white_c4[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    for (uint32_t i = 0; i < scene->object_count; i++) {
        we_scene_object_t *obj = &scene->objects[i];
        if (obj->visible_user_prop[0] != '\0') {
            const we_user_prop_t *up = project_find_prop(&scene->project, obj->visible_user_prop);
            if (up && up->num_components > 0) {
                obj->visible = (up->val[0] != 0.0f);
            }
        }
        if (obj->alpha_user_prop[0] != '\0') {
            const we_user_prop_t *up = project_find_prop(&scene->project, obj->alpha_user_prop);
            if (up && up->num_components > 0) {
                obj->alpha = up->val[0];
            }
        }
        if (obj->color_user_prop[0] != '\0') {
            const we_user_prop_t *up = project_find_prop(&scene->project, obj->color_user_prop);
            if (up && up->num_components >= 3) {
                obj->color[0] = up->val[0];
                obj->color[1] = up->val[1];
                obj->color[2] = up->val[2];
            }
        }
    }

    for (uint32_t i = 0; i < scene->object_count; i++) {
        step_object_scripts(scene, &scene->objects[i], width, height, dt_sec, time_sec,
                            cursor_moved, params);
    }

    for (uint32_t i = 0; i < scene->object_count; i++) {
        we_scene_object_t *obj = &scene->objects[i];

        if (!obj->visible || obj->alpha <= 0.0001f) {
            continue;
        }

        float alpha = fminf(fmaxf(obj->alpha, 0.0f), 1.0f);
        float r_mul = fminf(fmaxf(obj->color[0] * obj->brightness, 0.0f), 1.0f);
        float g_mul = fminf(fmaxf(obj->color[1] * obj->brightness, 0.0f), 1.0f);
        float b_mul = fminf(fmaxf(obj->color[2] * obj->brightness, 0.0f), 1.0f);

        uint8_t *decoded_rgba = NULL;
        const uint8_t *src_rgba = white_px;
        uint32_t tw = 1, th = 1;
        we_tex_t *tex0 = obj->base_pass.textures[0];
        if (tex0 && tex0->is_video_mp4) {
            (void)we_tex_advance_video(tex0, dt_sec);
        }
        if (tex0 && we_tex_decode_rgba8(tex0, &decoded_rgba, &tw, &th) && decoded_rgba && tw > 0 &&
            th > 0) {
            src_rgba = decoded_rgba;
        }

        if (obj->particle_sys) {
            float world_node[16];
            compute_object_node_world_matrix(scene, obj, 0.0f, 0.0f, world_node);
            float dx = cur_ptr[0] * ortho_w - world_node[12];
            float dy = (1.0f - cur_ptr[1]) * ortho_h - world_node[13];
            float m00 = world_node[0], m01 = world_node[4];
            float m10 = world_node[1], m11 = world_node[5];
            float det = m00 * m11 - m01 * m10;
            float lx = dx, ly = dy;
            if (fabsf(det) > 1e-8f) {
                float inv_det = 1.0f / det;
                lx = (m11 * dx - m01 * dy) * inv_det;
                ly = (-m10 * dx + m00 * dy) * inv_det;
            }
            we_particle_set_pointer(obj->particle_sys, lx, ly, 0.0f);
            we_particle_update(obj->particle_sys, dt_sec);

            float trans[2] = {0.0f, 0.0f};
            float rot[4] = {1.0f, 0.0f, 0.0f, 1.0f};
            if (tex0 && (tex0->flags & WE_TEX_FLAG_IS_GIF) != 0 && tex0->frame_count > 0) {
                we_tex_advance_spritesheet(tex0, dt_sec, trans, rot);
            }

            we_particle_vertex_t *pverts = NULL;
            uint32_t vcnt = we_particle_build_vertices(obj->particle_sys, &pverts);
            uint32_t qcnt = vcnt / 4u;
            for (uint32_t q = 0; q < qcnt; q++) {
                const we_particle_vertex_t *v = &pverts[q * 4u];
                float uv0[2] = {trans[0] + v[0].uv[0] * rot[0] + v[0].uv[1] * rot[2],
                                trans[1] + v[0].uv[0] * rot[1] + v[0].uv[1] * rot[3]};
                float uv1[2] = {trans[0] + v[1].uv[0] * rot[0] + v[1].uv[1] * rot[2],
                                trans[1] + v[1].uv[0] * rot[1] + v[1].uv[1] * rot[3]};
                float uv2[2] = {trans[0] + v[2].uv[0] * rot[0] + v[2].uv[1] * rot[2],
                                trans[1] + v[2].uv[0] * rot[1] + v[2].uv[1] * rot[3]};
                float uv3[2] = {trans[0] + v[3].uv[0] * rot[0] + v[3].uv[1] * rot[2],
                                trans[1] + v[3].uv[0] * rot[1] + v[3].uv[1] * rot[3]};
                cpu_rasterize_triangle(out_rgba, width, height, ortho_w, ortho_h, world_node,
                                       v[0].pos, uv0, v[0].color, v[1].pos, uv1, v[1].color,
                                       v[2].pos, uv2, v[2].color, src_rgba, tw, th, r_mul, g_mul,
                                       b_mul, alpha, obj->base_pass.blending);
                cpu_rasterize_triangle(out_rgba, width, height, ortho_w, ortho_h, world_node,
                                       v[2].pos, uv2, v[2].color, v[3].pos, uv3, v[3].color,
                                       v[0].pos, uv0, v[0].color, src_rgba, tw, th, r_mul, g_mul,
                                       b_mul, alpha, obj->base_pass.blending);
            }
            free(decoded_rgba);
            continue;
        }

        if (obj->puppet) {
            we_puppet_t *p = obj->puppet;
            we_puppet_eval_skin_matrices(p, p->layers, p->layer_count, (double)time_sec,
                                         p->skin_mats);
            we_puppet_skin_vertices_cpu(p, p->skin_mats, p->skinned_verts);

            float world_node[16];
            compute_object_node_world_matrix(scene, obj, 0.0f, 0.0f, world_node);
            uint32_t tri_cnt = p->index_count / 3u;
            for (uint32_t t = 0; t < tri_cnt; t++) {
                uint16_t i0 = p->indices[t * 3u + 0u];
                uint16_t i1 = p->indices[t * 3u + 1u];
                uint16_t i2 = p->indices[t * 3u + 2u];
                if (i0 >= p->vertex_count || i1 >= p->vertex_count || i2 >= p->vertex_count) {
                    continue;
                }
                const we_skinned_vertex_t *v0 = &p->skinned_verts[i0];
                const we_skinned_vertex_t *v1 = &p->skinned_verts[i1];
                const we_skinned_vertex_t *v2 = &p->skinned_verts[i2];
                cpu_rasterize_triangle(out_rgba, width, height, ortho_w, ortho_h, world_node,
                                       v0->pos, v0->uv, white_c4, v1->pos, v1->uv, white_c4,
                                       v2->pos, v2->uv, white_c4, src_rgba, tw, th, r_mul, g_mul,
                                       b_mul, alpha, obj->base_pass.blending);
            }
            free(decoded_rgba);
            continue;
        }

        if (!obj->has_base_pass || !decoded_rgba) {
            free(decoded_rgba);
            continue;
        }

        float model[16];
        compute_object_model_matrix(scene, obj, 0.0f, 0.0f, model);
        float m00 = model[0], m01 = model[4], tx = model[12];
        float m10 = model[1], m11 = model[5], ty = model[13];
        float det = m00 * m11 - m01 * m10;
        if (fabsf(det) < 1e-8f) {
            free(decoded_rgba);
            continue;
        }
        float inv_det = 1.0f / det;

        for (uint32_t y = 0; y < height; y++) {
            float wy = (1.0f - ((float)y + 0.5f) / (float)height) * ortho_h;
            for (uint32_t x = 0; x < width; x++) {
                float wx = (((float)x + 0.5f) / (float)width) * ortho_w;
                float dx = wx - tx;
                float dy = wy - ty;
                float qx = (m11 * dx - m01 * dy) * inv_det;
                float qy = (-m10 * dx + m00 * dy) * inv_det;
                if (qx < -1.0f || qx > 1.0f || qy < -1.0f || qy > 1.0f) {
                    continue;
                }
                float u = fminf(fmaxf((qx + 1.0f) * 0.5f, 0.0f), 0.999999f);
                float v = fminf(fmaxf((qy + 1.0f) * 0.5f, 0.0f), 0.999999f);
                uint32_t sx = (uint32_t)(u * (float)tw);
                uint32_t sy = (uint32_t)(v * (float)th);
                const uint8_t *sp = &src_rgba[(sy * tw + sx) * 4];

                float sa = (sp[3] / 255.0f) * alpha;
                float sr = (sp[0] / 255.0f) * r_mul;
                float sg = (sp[1] / 255.0f) * g_mul;
                float sb = (sp[2] / 255.0f) * b_mul;

                cpu_blend_pixel(&out_rgba[(y * width + x) * 4], sr, sg, sb, sa,
                                obj->base_pass.blending);
            }
        }
        free(decoded_rgba);
    }

    scene->last_pointer[0] = cur_ptr[0];
    scene->last_pointer[1] = cur_ptr[1];
    return true;
}

static void sync_project_prop_vec(we_project_t *proj, const char *key, const char *val_str,
                                  const float *values, uint32_t num_components)
{
    if (!proj || !key || key[0] == '\0') {
        return;
    }
    (void)we_project_apply_override(proj, key, val_str);
    for (uint32_t i = 0; i < proj->prop_count; ++i) {
        if (strcasecmp(proj->props[i].key, key) == 0) {
            memset(proj->props[i].val, 0, sizeof(proj->props[i].val));
            memcpy(proj->props[i].val, values, num_components * sizeof(float));
            proj->props[i].num_components = num_components;
            break;
        }
    }
}

static void update_pass_uniform_override(we_scene_t *scene, we_material_pass_t *mp,
                                         const char *name, const char *val_str, const float *values,
                                         uint32_t num_components)
{
    if (!scene || !mp || !name) {
        return;
    }
    bool matched_cv = false;
    for (uint32_t c = 0; c < mp->const_val_count; ++c) {
        we_const_val_t *cv = &mp->const_vals[c];
        if (strcasecmp(cv->name, name) == 0 ||
            (cv->user_prop[0] != '\0' && strcasecmp(cv->user_prop, name) == 0)) {
            memset(cv->val, 0, sizeof(cv->val));
            memcpy(cv->val, values, num_components * sizeof(float));
            cv->num_components = num_components;
            if (cv->user_prop[0] != '\0') {
                sync_project_prop_vec(&scene->project, cv->user_prop, val_str, values,
                                      num_components);
            }
            matched_cv = true;
        }
    }
    for (uint32_t u = 0; u < mp->prep.meta.uniform_count; ++u) {
        const we_shader_uniform_meta_t *um = &mp->prep.meta.uniforms[u];
        if (strcasecmp(um->name, name) == 0 ||
            (um->material_key[0] != '\0' && strcasecmp(um->material_key, name) == 0)) {
            if (um->material_key[0] != '\0') {
                sync_project_prop_vec(&scene->project, um->material_key, val_str, values,
                                      num_components);
            }
            bool found_for_um = false;
            for (uint32_t c = 0; c < mp->const_val_count; ++c) {
                we_const_val_t *cv = &mp->const_vals[c];
                if (strcasecmp(cv->name, um->name) == 0 ||
                    (um->material_key[0] != '\0' && strcasecmp(cv->name, um->material_key) == 0)) {
                    memset(cv->val, 0, sizeof(cv->val));
                    memcpy(cv->val, values, num_components * sizeof(float));
                    cv->num_components = num_components;
                    if (cv->user_prop[0] != '\0') {
                        sync_project_prop_vec(&scene->project, cv->user_prop, val_str, values,
                                              num_components);
                    }
                    found_for_um = true;
                }
            }
            if (!found_for_um && !matched_cv && mp->const_val_count < WE_MAX_CONST_VALS) {
                we_const_val_t *ncv = &mp->const_vals[mp->const_val_count++];
                memset(ncv, 0, sizeof(*ncv));
                snprintf(ncv->name, sizeof(ncv->name), "%s", um->name);
                memcpy(ncv->val, values, num_components * sizeof(float));
                ncv->num_components = num_components;
            }
        }
    }
}

bool we_scene_set_uniform_override(we_scene_t *scene, const char *name, const float *values,
                                   uint32_t num_components)
{
    if (!scene || !name || name[0] == '\0' || !values || num_components < 1 || num_components > 4) {
        return false;
    }
    for (uint32_t i = 0; i < num_components; ++i) {
        if (!isfinite(values[i])) {
            return false;
        }
    }

    char val_str[128];
    if (num_components == 1) {
        snprintf(val_str, sizeof(val_str), "%.6g", values[0]);
    } else if (num_components == 2) {
        snprintf(val_str, sizeof(val_str), "%.6g %.6g", values[0], values[1]);
    } else if (num_components == 3) {
        snprintf(val_str, sizeof(val_str), "%.6g %.6g %.6g", values[0], values[1], values[2]);
    } else {
        snprintf(val_str, sizeof(val_str), "%.6g %.6g %.6g %.6g", values[0], values[1], values[2],
                 values[3]);
    }

    if (!we_project_apply_override(&scene->project, name, val_str)) {
        return false;
    }
    sync_project_prop_vec(&scene->project, name, val_str, values, num_components);

    for (uint32_t i = 0; i < scene->object_count; ++i) {
        we_scene_object_t *obj = &scene->objects[i];
        if (obj->color_user_prop[0] != '\0' && strcasecmp(obj->color_user_prop, name) == 0 &&
            num_components >= 3) {
            obj->color[0] = values[0];
            obj->color[1] = values[1];
            obj->color[2] = values[2];
        }
        if (obj->alpha_user_prop[0] != '\0' && strcasecmp(obj->alpha_user_prop, name) == 0) {
            obj->alpha = values[0];
        }
        if (obj->visible_user_prop[0] != '\0' && strcasecmp(obj->visible_user_prop, name) == 0) {
            obj->visible = (values[0] != 0.0f);
        }
        if (obj->has_base_pass) {
            update_pass_uniform_override(scene, &obj->base_pass, name, val_str, values,
                                         num_components);
        }
        for (uint32_t e = 0; e < obj->effect_count; ++e) {
            we_effect_t *eff = &obj->effects[e];
            if (eff->visible_user_prop[0] != '\0' &&
                strcasecmp(eff->visible_user_prop, name) == 0) {
                eff->visible = (values[0] != 0.0f);
            }
            for (uint32_t p = 0; p < eff->pass_count; ++p) {
                update_pass_uniform_override(scene, &eff->passes[p], name, val_str, values,
                                             num_components);
            }
        }
        for (uint32_t s = 0; s < obj->script_count; ++s) {
            if (obj->scripts[s].vm) {
                bind_project_props_to_script(obj->scripts[s].vm, &scene->project);
            }
        }
    }

    apply_scene_script_user_properties(scene);
    return true;
}

void waywal_we_scene_forget_bo(we_scene_t *scene, const dmabuf_bo_t *bo)
{
    if (!scene || !bo) {
        return;
    }
    PFNEGLDESTROYIMAGEKHRPROC destroy_img =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");

    for (size_t i = 0; i < scene->num_imported_bos; i++) {
        if (scene->imported_bos[i].bo == bo) {
            if (scene->gl_initialized && scene->egl_display && scene->egl_context &&
                scene->imported_bos[i].owns_egl) {
                if (eglGetCurrentContext() != scene->egl_context) {
                    eglMakeCurrent(scene->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                                   scene->egl_context);
                }
                if (scene->imported_bos[i].gl_tex) {
                    GLuint del_tex = scene->imported_bos[i].gl_tex;
                    glDeleteTextures(1, &del_tex);
                }
                if (scene->imported_bos[i].egl_image && destroy_img) {
                    void *del_img = scene->imported_bos[i].egl_image;
                    destroy_img(scene->egl_display, del_img);
                }
            }
            for (size_t j = i + 1; j < scene->num_imported_bos; j++) {
                scene->imported_bos[j - 1] = scene->imported_bos[j];
            }
            scene->num_imported_bos--;
            break;
        }
    }
}

static void free_material_pass(we_material_pass_t *pass, bool gl_init)
{
    if (!pass) {
        return;
    }
    for (uint32_t s = 0; s < WE_MAX_PASS_TEXTURES; s++) {
        if (pass->textures[s]) {
            we_tex_destroy(pass->textures[s]);
            pass->textures[s] = NULL;
        }
    }
    for (uint32_t c = 0; c < pass->const_val_count; c++) {
        free(pass->const_vals[c].script_src);
        pass->const_vals[c].script_src = NULL;
    }
    we_shader_preprocessed_free(&pass->prep);
    if (gl_init && pass->gl_prog != 0) {
        glDeleteProgram(pass->gl_prog);
        pass->gl_prog = 0;
    }
}

void we_scene_destroy(we_scene_t *scene)
{
    if (!scene) {
        return;
    }

    if (scene->gl_initialized && scene->egl_display && scene->egl_context) {
        if (eglGetCurrentContext() != scene->egl_context) {
            eglMakeCurrent(scene->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, scene->egl_context);
        }
    }

    if (scene->objects) {
        for (uint32_t i = 0; i < scene->object_count; i++) {
            we_scene_object_t *obj = &scene->objects[i];
            if (obj->puppet) {
                we_puppet_destroy(obj->puppet);
                obj->puppet = NULL;
            }
            if (obj->particle_sys) {
                we_particle_destroy(obj->particle_sys);
                obj->particle_sys = NULL;
            }
            for (uint32_t s = 0; s < obj->script_count; s++) {
                if (obj->scripts[s].vm) {
                    we_script_destroy(obj->scripts[s].vm);
                    obj->scripts[s].vm = NULL;
                }
            }
            free_material_pass(&obj->base_pass, scene->gl_initialized);
            if (obj->effects) {
                for (uint32_t e = 0; e < obj->effect_count; e++) {
                    we_effect_t *eff = &obj->effects[e];
                    if (scene->gl_initialized) {
                        for (uint32_t f = 0; f < eff->fbo_count; f++) {
                            if (eff->fbos[f].gl_fbo) {
                                glDeleteFramebuffers(1, &eff->fbos[f].gl_fbo);
                            }
                            if (eff->fbos[f].gl_tex) {
                                glDeleteTextures(1, &eff->fbos[f].gl_tex);
                            }
                        }
                    }
                    for (uint32_t p = 0; p < eff->pass_count; p++) {
                        free_material_pass(&eff->passes[p], scene->gl_initialized);
                    }
                }
                free(obj->effects);
            }
            if (scene->gl_initialized) {
                for (int k = 0; k < 2; k++) {
                    if (obj->ping_fbo[k]) {
                        glDeleteFramebuffers(1, &obj->ping_fbo[k]);
                    }
                    if (obj->ping_tex[k]) {
                        glDeleteTextures(1, &obj->ping_tex[k]);
                    }
                }
            }
        }
        free(scene->objects);
    }

    if (scene->gl_initialized) {
        PFNEGLDESTROYIMAGEKHRPROC destroy_img =
            (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
        for (size_t i = 0; i < scene->num_imported_bos; i++) {
            if (!scene->imported_bos[i].owns_egl) {
                continue;
            }
            if (scene->imported_bos[i].gl_tex) {
                GLuint del_tex = scene->imported_bos[i].gl_tex;
                glDeleteTextures(1, &del_tex);
            }
            if (scene->imported_bos[i].egl_image && destroy_img) {
                void *del_img = scene->imported_bos[i].egl_image;
                destroy_img(scene->egl_display, del_img);
            }
        }
        scene->num_imported_bos = 0;

        for (uint32_t r = 0; r < scene->rt_fbo_count; r++) {
            if (scene->rt_fbos[r].gl_fbo) {
                glDeleteFramebuffers(1, &scene->rt_fbos[r].gl_fbo);
            }
            if (scene->rt_fbos[r].gl_tex) {
                glDeleteTextures(1, &scene->rt_fbos[r].gl_tex);
            }
        }
        scene->rt_fbo_count = 0;

        if (scene->scene_fbo) {
            glDeleteFramebuffers(1, &scene->scene_fbo);
        }
        if (scene->scene_fbo_tex) {
            glDeleteTextures(1, &scene->scene_fbo_tex);
        }
        if (scene->full_fb_fbo) {
            glDeleteFramebuffers(1, &scene->full_fb_fbo);
        }
        if (scene->full_fb_tex) {
            glDeleteTextures(1, &scene->full_fb_tex);
        }
        if (scene->target_fbo) {
            glDeleteFramebuffers(1, &scene->target_fbo);
        }
        if (scene->prog_present) {
            glDeleteProgram(scene->prog_present);
        }
        if (scene->default_program) {
            glDeleteProgram(scene->default_program);
        }
        if (scene->particle_program) {
            glDeleteProgram(scene->particle_program);
        }
        if (scene->quad_vao) {
            glDeleteVertexArrays(1, &scene->quad_vao);
        }
        if (scene->quad_vbo) {
            glDeleteBuffers(1, &scene->quad_vbo);
        }
        if (scene->tex_util_white) {
            glDeleteTextures(1, &scene->tex_util_white);
        }
        if (scene->tex_util_black) {
            glDeleteTextures(1, &scene->tex_util_black);
        }
        if (scene->tex_util_flatnormal) {
            glDeleteTextures(1, &scene->tex_util_flatnormal);
        }
        if (scene->tex_util_noise) {
            glDeleteTextures(1, &scene->tex_util_noise);
        }
        if (scene->tex_album_art) {
            glDeleteTextures(1, &scene->tex_album_art);
        }

        if (scene->owns_egl && scene->egl_display != EGL_NO_DISPLAY &&
            scene->egl_context != EGL_NO_CONTEXT) {
            if (s_we_shared_refcount > 0) {
                s_we_shared_refcount--;
            }
            if (s_we_shared_refcount == 0) {
                eglMakeCurrent(scene->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                eglDestroyContext(scene->egl_display, scene->egl_context);
                s_we_shared_dpy = EGL_NO_DISPLAY;
                s_we_shared_ctx = EGL_NO_CONTEXT;
            }
        }
    }

    free(scene->album_art_rgba);
    scene->album_art_rgba = NULL;
    if (scene->pkg) {
        we_pkg_close(scene->pkg);
    }
    free(scene);
}

void we_scene_set_album_art(we_scene_t *scene, const uint8_t *rgba, uint32_t w, uint32_t h)
{
    if (!scene) {
        return;
    }
    free(scene->album_art_rgba);
    scene->album_art_rgba = NULL;
    scene->album_art_w = 0;
    scene->album_art_h = 0;

    if (rgba && w > 0 && h > 0) {
        size_t sz = (size_t)w * (size_t)h * 4u;
        scene->album_art_rgba = (uint8_t *)malloc(sz);
        if (scene->album_art_rgba) {
            memcpy(scene->album_art_rgba, rgba, sz);
            scene->album_art_w = w;
            scene->album_art_h = h;
        }
    }

    if (scene->gl_initialized && scene->egl_display && scene->egl_context) {
        if (eglGetCurrentContext() != scene->egl_context) {
            eglMakeCurrent(scene->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, scene->egl_context);
        }
        if (scene->tex_album_art) {
            glDeleteTextures(1, &scene->tex_album_art);
            scene->tex_album_art = 0;
        }
        if (scene->album_art_rgba && scene->album_art_w > 0 && scene->album_art_h > 0) {
            GLuint atex = 0;
            glGenTextures(1, &atex);
            glBindTexture(GL_TEXTURE_2D, atex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)scene->album_art_w,
                         (GLsizei)scene->album_art_h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         scene->album_art_rgba);
            scene->tex_album_art = (uint32_t)atex;
        }
    }
}

void we_scene_set_album_art_bgra(we_scene_t *scene, const uint8_t *bgra, uint32_t w, uint32_t h)
{
    if (!scene) {
        return;
    }
    if (!bgra || w == 0 || h == 0) {
        we_scene_set_album_art(scene, NULL, 0, 0);
        return;
    }
    size_t npx = (size_t)w * (size_t)h;
    uint8_t *rgba = (uint8_t *)malloc(npx * 4u);
    if (!rgba) {
        return;
    }
    for (size_t i = 0; i < npx; ++i) {
        rgba[i * 4u + 0u] = bgra[i * 4u + 2u];
        rgba[i * 4u + 1u] = bgra[i * 4u + 1u];
        rgba[i * 4u + 2u] = bgra[i * 4u + 0u];
        rgba[i * 4u + 3u] = bgra[i * 4u + 3u];
    }
    we_scene_set_album_art(scene, rgba, w, h);
    free(rgba);
}

void we_scene_set_media_playing(we_scene_t *scene, float playing)
{
    if (!scene) {
        return;
    }
    scene->media_playing = playing;
}

#endif /* !WWAL_WE_PROJECT_ONLY */
