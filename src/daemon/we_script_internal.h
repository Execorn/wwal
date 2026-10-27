// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WE_SCRIPT_INTERNAL_H
#define WAYWAL_WE_SCRIPT_INTERNAL_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "waywal/we_script.h"
#include "waywal/log.h"

#include <ctype.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.71828182845904523536
#endif

typedef enum {
    VAL_UNDEF = 0,
    VAL_NULL,
    VAL_BOOL,
    VAL_NUM,
    VAL_STR,
    VAL_VEC,
    VAL_ARR,
    VAL_OBJ,
    VAL_FUNC,
    VAL_BUILTIN_OBJ,
    VAL_SCENE_LAYER,
    VAL_AUDIO_BUFFERS,
    VAL_AUDIO_CHANNEL,
} js_val_type_t;

typedef enum {
    BUILTIN_MATH = 1,
    BUILTIN_WEMATH,
    BUILTIN_WECOLOR,
    BUILTIN_ENGINE,
    BUILTIN_INPUT,
    BUILTIN_THIS_LAYER,
    BUILTIN_SCENE,
    BUILTIN_SHARED,
    BUILTIN_CONSOLE,
    BUILTIN_SCRIPT_PROPS_BUILDER,
} js_builtin_id_t;

typedef struct js_vec_obj {
    uint8_t dim;         /* 2, 3, or 4 */
    uint8_t bind_tag;    /* 0=none, 1=layer.origin, 2=layer.scale, 3=layer.angles, 4=layer.color,
                            5=layer.size */
    uint16_t layer_slot; /* 0=thisLayer, 1..WE_SCRIPT_MAX_SCENE_LAYERS=scene_layers[slot-1] */
    float v[4];
} js_vec_obj_t;

typedef struct js_val js_val_t;
typedef struct js_obj js_obj_t;
typedef struct js_arr js_arr_t;
typedef struct ast_node ast_node_t;

struct js_val {
    js_val_type_t type;
    union {
        bool bval;
        double num;
        const char *str;
        js_vec_obj_t *vec;
        js_arr_t *arr;
        js_obj_t *obj;
        const ast_node_t *func;
        js_builtin_id_t builtin_id;
        uint32_t layer_slot;
        struct {
            uint32_t res;
            uint8_t channel; /* 0=average, 1=left, 2=right */
        } audio;
    } u;
};

typedef struct {
    const char *key;
    js_val_t val;
} js_prop_t;

struct js_obj {
    js_prop_t *props;
    uint32_t count;
    uint32_t cap;
};

struct js_arr {
    js_val_t *items;
    uint32_t count;
    uint32_t cap;
};

/* =========================================================================
 * Lexer & Tokens
 * ========================================================================= */

typedef enum {
    TOK_EOF = 0,
    TOK_IDENT,
    TOK_NUM,
    TOK_STR,
    TOK_TRUE,
    TOK_FALSE,
    TOK_NULL,
    TOK_UNDEF,
    TOK_VAR,
    TOK_LET,
    TOK_CONST,
    TOK_FUNCTION,
    TOK_EXPORT,
    TOK_RETURN,
    TOK_IF,
    TOK_ELSE,
    TOK_FOR,
    TOK_WHILE,
    TOK_DO,
    TOK_SWITCH,
    TOK_CASE,
    TOK_DEFAULT,
    TOK_TRY,
    TOK_CATCH,
    TOK_FINALLY,
    TOK_THROW,
    TOK_BREAK,
    TOK_CONTINUE,
    TOK_NEW,
    TOK_THIS,
    TOK_TYPEOF,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACK,
    TOK_RBRACK,
    TOK_SEMI,
    TOK_COMMA,
    TOK_DOT,
    TOK_COLON,
    TOK_QUESTION,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_INC,
    TOK_DEC,
    TOK_ASSIGN,
    TOK_PLUS_ASSIGN,
    TOK_MINUS_ASSIGN,
    TOK_STAR_ASSIGN,
    TOK_SLASH_ASSIGN,
    TOK_PERCENT_ASSIGN,
    TOK_SHL_ASSIGN,
    TOK_SHR_ASSIGN,
    TOK_USHR_ASSIGN,
    TOK_AND_ASSIGN,
    TOK_OR_ASSIGN,
    TOK_XOR_ASSIGN,
    TOK_EQ,
    TOK_STRICT_EQ,
    TOK_NEQ,
    TOK_STRICT_NEQ,
    TOK_LT,
    TOK_LE,
    TOK_GT,
    TOK_GE,
    TOK_AND,
    TOK_OR,
    TOK_NULLISH,
    TOK_NOT,
    TOK_BIT_AND,
    TOK_BIT_OR,
    TOK_BIT_XOR,
    TOK_BIT_NOT,
    TOK_SHL,
    TOK_SHR,
    TOK_USHR,
} js_tok_type_t;

typedef struct {
    js_tok_type_t type;
    const char *text;
    double num;
} js_token_t;

typedef enum {
    AST_PROGRAM = 0,
    AST_BLOCK,
    AST_VAR_DECL,
    AST_FUNC_DECL,
    AST_IF,
    AST_WHILE,
    AST_DO_WHILE,
    AST_FOR,
    AST_SWITCH,
    AST_TRY,
    AST_THROW,
    AST_RETURN,
    AST_BREAK,
    AST_CONTINUE,
    AST_EXPR_STMT,
    AST_LITERAL,
    AST_IDENT,
    AST_THIS,
    AST_ARRAY_LIT,
    AST_OBJ_LIT,
    AST_UNARY,
    AST_UPDATE, /* ++ / -- */
    AST_BINARY,
    AST_ASSIGN,
    AST_TERNARY,
    AST_MEMBER,
    AST_INDEX,
    AST_CALL,
    AST_NEW,
} ast_type_t;

typedef struct {
    const char *name;
    ast_node_t *init;
} ast_var_item_t;

struct ast_node {
    ast_type_t type;
    js_tok_type_t op;
    bool prefix;
    bool is_exported;
    const char *name;
    js_val_t lit;
    ast_node_t *left;
    ast_node_t *right;
    ast_node_t *cond;
    ast_node_t *then_branch;
    ast_node_t *else_branch;
    ast_node_t *init;
    ast_node_t *step;
    ast_node_t *body;
    const char **params;
    uint32_t param_count;
    ast_node_t **children;
    ast_node_t **case_exprs;
    const char **keys;
    uint32_t child_count;
    ast_var_item_t *decls;
    uint32_t decl_count;
};

#define WE_SCRIPT_MAX_GLOBALS  96u
#define WE_SCRIPT_MAX_LOCALS   64u
#define WE_SCRIPT_MAX_PROPS    64u
#define WE_SCRIPT_SCRATCH_SIZE (256u * 1024u)

typedef struct {
    const char *name;
    js_val_t val;
} js_var_slot_t;

typedef struct {
    char name[64];
    float val[4];
    uint32_t num_components;
    char str_val[128];
} we_script_prop_item_t;

typedef struct {
    bool valid;
    bool accessed;
    we_script_layer_state_t state;
    js_vec_obj_t origin_vec;
    js_vec_obj_t scale_vec;
    js_vec_obj_t angles_vec;
    js_vec_obj_t color_vec;
    js_vec_obj_t size_vec;
} we_script_scene_layer_slot_t;

struct we_script {
    uint8_t *arena_base;
    size_t arena_cap;
    size_t arena_used;
    size_t ast_watermark;

    void *quickjs_dl;
    we_script_shared_t *shared;
    uint64_t rng_state;

    ast_node_t *root_ast;
    const ast_node_t *fn_init;
    const ast_node_t *fn_update;
    const ast_node_t *fn_cursor_move;
    const ast_node_t *fn_apply_user_props;

    js_var_slot_t globals[WE_SCRIPT_MAX_GLOBALS];
    uint32_t global_count;

    js_var_slot_t this_props[WE_SCRIPT_MAX_PROPS];
    uint32_t this_prop_count;

    we_script_prop_item_t user_props[WE_SCRIPT_MAX_PROPS];
    uint32_t user_prop_count;

    we_script_scene_layer_slot_t scene_layers[WE_SCRIPT_MAX_SCENE_LAYERS];

    we_script_env_t env;
    we_script_layer_state_t layer;
    js_vec_obj_t layer_origin_vec;
    js_vec_obj_t layer_scale_vec;
    js_vec_obj_t layer_angles_vec;
    js_vec_obj_t layer_color_vec;
    js_vec_obj_t layer_size_vec;

    /* Watchdog telemetry */
    struct timespec start_ts;
    uint32_t step_count;
    uint32_t last_steps;
    uint32_t eval_depth;
    bool aborted;
    bool timed_out;
    bool oom;
    bool uses_audio;
    bool parse_error;
    bool depth_exceeded;
};

static void *script_arena_alloc(we_script_t *vm, size_t size)
{
    if (!vm || !vm->arena_base) {
        return NULL;
    }
    size_t aligned = (size + 7u) & ~(size_t)7u;
    if (aligned == 0) {
        aligned = 8u;
    }
    if (vm->arena_used + aligned > vm->arena_cap) {
        vm->oom = true;
        vm->aborted = true;
        return NULL;
    }
    void *ptr = vm->arena_base + vm->arena_used;
    vm->arena_used += aligned;
    memset(ptr, 0, aligned);
    return ptr;
}

static char *script_arena_strdup(we_script_t *vm, const char *s, size_t len)
{
    char *dst = (char *)script_arena_alloc(vm, len + 1u);
    if (!dst) {
        return NULL;
    }
    if (len > 0 && s) {
        memcpy(dst, s, len);
    }
    dst[len] = '\0';
    return dst;
}

ast_node_t *we_script_parse_program(we_script_t *vm, const char *src, size_t len);

#endif /* WAYWAL_WE_SCRIPT_INTERNAL_H */
