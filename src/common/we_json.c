// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/we_json.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
} json_lexer_t;

void we_jval_free(we_jval_t *v)
{
    if (!v) {
        return;
    }
    if (v->type == JVAL_STR) {
        free(v->str);
    } else if (v->type == JVAL_ARR) {
        for (size_t i = 0; i < v->arr.count; i++) {
            we_jval_free(v->arr.items[i]);
        }
        free(v->arr.items);
    } else if (v->type == JVAL_OBJ) {
        for (size_t i = 0; i < v->obj.count; i++) {
            free(v->obj.entries[i].key);
            we_jval_free(v->obj.entries[i].val);
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
    lx->pos++;

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
                if (lx->pos + 4 <= lx->len && isxdigit((unsigned char)lx->src[lx->pos]) &&
                    isxdigit((unsigned char)lx->src[lx->pos + 1]) &&
                    isxdigit((unsigned char)lx->src[lx->pos + 2]) &&
                    isxdigit((unsigned char)lx->src[lx->pos + 3])) {
                    char hex[5];
                    memcpy(hex, lx->src + lx->pos, 4);
                    hex[4] = '\0';
                    uint32_t cp = (uint32_t)strtoul(hex, NULL, 16);
                    lx->pos += 4;
                    if (cp >= 0xD800u && cp <= 0xDBFFu && lx->pos + 6 <= lx->len &&
                        lx->src[lx->pos] == '\\' && lx->src[lx->pos + 1] == 'u' &&
                        isxdigit((unsigned char)lx->src[lx->pos + 2]) &&
                        isxdigit((unsigned char)lx->src[lx->pos + 3]) &&
                        isxdigit((unsigned char)lx->src[lx->pos + 4]) &&
                        isxdigit((unsigned char)lx->src[lx->pos + 5])) {
                        char hex2[5];
                        memcpy(hex2, lx->src + lx->pos + 2, 4);
                        hex2[4] = '\0';
                        uint32_t low = (uint32_t)strtoul(hex2, NULL, 16);
                        if (low >= 0xDC00u && low <= 0xDFFFu) {
                            cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                            lx->pos += 6;
                        }
                    }
                    if (cp >= 0xD800u && cp <= 0xDFFFu) {
                        cp = 0xFFFDu;
                    }
                    if (len + 6 > cap) {
                        size_t ncap = cap * 2;
                        char *nb = realloc(out, ncap);
                        if (!nb) {
                            free(out);
                            return NULL;
                        }
                        out = nb;
                        cap = ncap;
                    }
                    if (cp == 0) {
                        out[len++] = ' ';
                    } else if (cp < 0x80u) {
                        out[len++] = (char)cp;
                    } else if (cp < 0x800u) {
                        out[len++] = (char)(0xC0u | (cp >> 6));
                        out[len++] = (char)(0x80u | (cp & 0x3Fu));
                    } else if (cp < 0x10000u) {
                        out[len++] = (char)(0xE0u | (cp >> 12));
                        out[len++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                        out[len++] = (char)(0x80u | (cp & 0x3Fu));
                    } else if (cp <= 0x10FFFFu) {
                        out[len++] = (char)(0xF0u | (cp >> 18));
                        out[len++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
                        out[len++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                        out[len++] = (char)(0x80u | (cp & 0x3Fu));
                    } else {
                        out[len++] = '?';
                    }
                    continue;
                } else {
                    c = '?';
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
    lx->pos++;
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
            we_jval_free(arr);
            return NULL;
        }
        if (arr->arr.count + 1 > cap) {
            size_t ncap = cap == 0 ? 8 : cap * 2;
            we_jval_t **ni = realloc(arr->arr.items, ncap * sizeof(we_jval_t *));
            if (!ni) {
                we_jval_free(elem);
                we_jval_free(arr);
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

    we_jval_free(arr);
    return NULL;
}

static we_jval_t *json_parse_object(json_lexer_t *lx, int depth)
{
    lx->pos++;
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
            we_jval_free(obj);
            return NULL;
        }
        json_skip_ws(lx);
        if (lx->pos >= lx->len || lx->src[lx->pos] != ':') {
            free(key);
            we_jval_free(obj);
            return NULL;
        }
        lx->pos++;

        we_jval_t *val = json_parse_value(lx, depth + 1);
        if (!val) {
            free(key);
            we_jval_free(obj);
            return NULL;
        }

        if (obj->obj.count + 1 > cap) {
            size_t ncap = cap == 0 ? 8 : cap * 2;
            we_jkv_t *ne = realloc(obj->obj.entries, ncap * sizeof(we_jkv_t));
            if (!ne) {
                free(key);
                we_jval_free(val);
                we_jval_free(obj);
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

    we_jval_free(obj);
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
        if (endptr == num_buf || !isfinite(num)) {
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

we_jval_t *we_json_parse_root(const char *src, size_t len)
{
    if (!src || len == 0) {
        return NULL;
    }
    size_t offset = 0;
    if (len >= 3 && (uint8_t)src[0] == 0xEF && (uint8_t)src[1] == 0xBB && (uint8_t)src[2] == 0xBF) {
        offset = 3;
    }
    json_lexer_t lx = {.src = src + offset, .len = len - offset, .pos = 0};
    return json_parse_value(&lx, 0);
}

const we_jval_t *we_jobj_get(const we_jval_t *obj, const char *key)
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

const char *we_jval_str(const we_jval_t *v, const char *def)
{
    if (!v) {
        return def;
    }
    if (v->type == JVAL_STR && v->str) {
        return v->str;
    }
    if (v->type == JVAL_OBJ) {
        const we_jval_t *inner = we_jobj_get(v, "value");
        if (inner && inner->type == JVAL_STR && inner->str) {
            return inner->str;
        }
    }
    return def;
}

const we_user_prop_t *we_project_find_prop(const we_project_t *proj, const char *key)
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

static bool eval_up_matches_cond_str(const we_user_prop_t *up, const char *cond_str)
{
    if (!up || !cond_str) {
        return false;
    }
    bool invert = false;
    while (*cond_str == '!') {
        invert = !invert;
        cond_str++;
    }
    while (*cond_str && isspace((unsigned char)*cond_str)) {
        cond_str++;
    }

    bool matched = false;
    if (up->str_val[0] != '\0' && strcasecmp(up->str_val, cond_str) == 0) {
        matched = true;
    } else if (up->num_components > 0) {
        if (strcasecmp(cond_str, "true") == 0) {
            matched = (up->val[0] != 0.0f);
        } else if (strcasecmp(cond_str, "false") == 0) {
            matched = (up->val[0] == 0.0f);
        } else {
            char *endp = NULL;
            float cv = strtof(cond_str, &endp);
            if (endp && endp > cond_str && isfinite(cv)) {
                while (*endp && isspace((unsigned char)*endp)) {
                    endp++;
                }
                if (*endp == '\0') {
                    matched = (fabsf(up->val[0] - cv) < 1e-4f);
                }
            }
        }
    }
    return invert ? !matched : matched;
}

bool we_project_eval_prop_bool(const we_project_t *proj, const char *prop_expr, bool *out_val)
{
    if (!proj || !prop_expr || prop_expr[0] == '\0' || !out_val) {
        return false;
    }
    const char *p = prop_expr;
    bool prefix_neg = false;
    while (*p == '!' || isspace((unsigned char)*p)) {
        if (*p == '!') {
            prefix_neg = !prefix_neg;
        }
        p++;
    }
    if (*p == '\0') {
        return false;
    }
    const char *eq = strchr(p, '=');
    if (!eq) {
        const we_user_prop_t *up = we_project_find_prop(proj, p);
        if (!up || up->num_components == 0) {
            return false;
        }
        bool v = (up->val[0] != 0.0f);
        *out_val = prefix_neg ? !v : v;
        return true;
    }

    size_t klen = (size_t)(eq - p);
    bool neq = prefix_neg;
    if (klen > 0 && p[klen - 1] == '!') {
        neq = !neq;
        klen--;
    }
    while (klen > 0 && isspace((unsigned char)p[klen - 1])) {
        klen--;
    }
    if (klen == 0 || klen >= 64) {
        return false;
    }
    char kbuf[64];
    memcpy(kbuf, p, klen);
    kbuf[klen] = '\0';

    const we_user_prop_t *up = we_project_find_prop(proj, kbuf);
    if (!up) {
        return false;
    }
    bool matched = eval_up_matches_cond_str(up, eq + 1);
    *out_val = neq ? !matched : matched;
    return true;
}

uint32_t we_parse_float_str(const char *s, float *out, uint32_t max_n)
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
        if (endptr == p || !isfinite(v)) {
            break;
        }
        out[count++] = v;
        p = endptr;
    }
    return count;
}

uint32_t we_jval_floats(const we_jval_t *v, const we_project_t *proj, float *out, uint32_t max_n,
                        char *out_user_prop, size_t prop_cap)
{
    if (out_user_prop && prop_cap > 0) {
        out_user_prop[0] = '\0';
    }
    if (!v || !out || max_n == 0) {
        return 0;
    }

    if (v->type == JVAL_OBJ) {
        const we_jval_t *u = we_jobj_get(v, "user");
        const char *ukey = NULL;
        const we_jval_t *ucond = NULL;
        if (u && u->type == JVAL_STR) {
            ukey = u->str;
        } else if (u && u->type == JVAL_OBJ) {
            ukey = we_jval_str(we_jobj_get(u, "name"), NULL);
            ucond = we_jobj_get(u, "condition");
        }
        if (ukey && ukey[0] != '\0') {
            bool key_neg = false;
            const char *clean_key = ukey;
            while (*clean_key == '!' || isspace((unsigned char)*clean_key)) {
                if (*clean_key == '!') {
                    key_neg = !key_neg;
                }
                clean_key++;
            }
            char cond_str[64] = {0};
            if (ucond) {
                if (ucond->type == JVAL_STR && ucond->str) {
                    snprintf(cond_str, sizeof(cond_str), "%s", ucond->str);
                } else if (ucond->type == JVAL_NUM) {
                    snprintf(cond_str, sizeof(cond_str), "%.6g", ucond->num);
                } else if (ucond->type == JVAL_BOOL) {
                    snprintf(cond_str, sizeof(cond_str), "%s", ucond->bval ? "true" : "false");
                }
            }

            if (out_user_prop && prop_cap > 0) {
                if (cond_str[0] != '\0') {
                    bool cond_neg = key_neg;
                    const char *cp = cond_str;
                    while (*cp == '!' || isspace((unsigned char)*cp)) {
                        if (*cp == '!') {
                            cond_neg = !cond_neg;
                        }
                        cp++;
                    }
                    snprintf(out_user_prop, prop_cap, "%s%s%s", clean_key, cond_neg ? "!=" : "=",
                             cp);
                } else {
                    snprintf(out_user_prop, prop_cap, "%s%s", key_neg ? "!" : "", clean_key);
                }
            }

            const we_user_prop_t *up = we_project_find_prop(proj, clean_key);
            if (up) {
                if (cond_str[0] != '\0') {
                    bool matched = eval_up_matches_cond_str(up, cond_str);
                    if (key_neg) {
                        matched = !matched;
                    }
                    out[0] = matched ? 1.0f : 0.0f;
                    return 1;
                }
                if (up->num_components > 0) {
                    uint32_t n = up->num_components < max_n ? up->num_components : max_n;
                    for (uint32_t i = 0; i < n; i++) {
                        out[i] = key_neg ? (up->val[i] == 0.0f ? 1.0f : 0.0f) : up->val[i];
                    }
                    return n;
                }
            }
        }
        const we_jval_t *inner = we_jobj_get(v, "value");
        if (inner) {
            return we_jval_floats(inner, proj, out, max_n, NULL, 0);
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
        return we_parse_float_str(v->str, out, max_n);
    }
    if (v->type == JVAL_ARR) {
        uint32_t n = (uint32_t)v->arr.count < max_n ? (uint32_t)v->arr.count : max_n;
        for (uint32_t i = 0; i < n; i++) {
            float tmp = 0.0f;
            we_jval_floats(v->arr.items[i], proj, &tmp, 1, NULL, 0);
            out[i] = tmp;
        }
        return n;
    }
    return 0;
}

float we_jval_float(const we_jval_t *v, const we_project_t *proj, float def, char *out_user_prop,
                    size_t prop_cap)
{
    float tmp = def;
    if (we_jval_floats(v, proj, &tmp, 1, out_user_prop, prop_cap) >= 1) {
        return tmp;
    }
    return def;
}

bool we_jval_bool_resolved(const we_jval_t *v, const we_project_t *proj, bool def,
                           char *out_user_prop, size_t prop_cap)
{
    float tmp = def ? 1.0f : 0.0f;
    if (we_jval_floats(v, proj, &tmp, 1, out_user_prop, prop_cap) >= 1) {
        return tmp != 0.0f;
    }
    return def;
}
