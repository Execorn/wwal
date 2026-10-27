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

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WE_JSON_H */
