// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WE_JSON_H
#define WAYWAL_WE_JSON_H

#include "waywal/we_scene.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

we_jval_t *we_json_parse_root(const char *src, size_t len);
we_jval_t *we_json_parse(const char *src, size_t len);
void we_jval_free(we_jval_t *v);
const we_jval_t *we_jobj_get(const we_jval_t *obj, const char *key);
const char *we_jval_str(const we_jval_t *v, const char *def);
const we_user_prop_t *we_project_find_prop(const we_project_t *proj, const char *key);
uint32_t we_parse_float_str(const char *s, float *out, uint32_t max_n);
uint32_t we_jval_floats(const we_jval_t *v, const we_project_t *proj, float *out, uint32_t max_n,
                        char *out_user_prop, size_t prop_cap);
float we_jval_float(const we_jval_t *v, const we_project_t *proj, float def, char *out_user_prop,
                    size_t prop_cap);
bool we_jval_bool_resolved(const we_jval_t *v, const we_project_t *proj, bool def,
                           char *out_user_prop, size_t prop_cap);
bool we_project_eval_prop_bool(const we_project_t *proj, const char *prop_expr, bool *out_val);

/*
 * Format finite IEEE-754 double via Ryu shortest round-trip ECMAScript/RFC 8785 rules.
 * Independent of process LC_NUMERIC; rejects NaN/Inf and undersized buffers.
 */
bool we_json_format_double(double val, char *out, size_t out_cap);

/*
 * Serialize DOM value as RFC 8785 JCS canonical JSON (caller must free()).
 * Rejects non-finite numbers, malformed UTF-8 / lone surrogates, and duplicate keys.
 */
char *we_jval_to_canonical_json(const we_jval_t *v, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WE_JSON_H */
